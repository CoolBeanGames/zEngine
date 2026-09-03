#include "FbxImporter.h"
#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr std::size_t MaxFileBytes = 128 * 1024 * 1024;
    constexpr std::size_t MaxImageBytes = 32 * 1024 * 1024;
    constexpr std::size_t MaxCorners = 3'000'000;

    std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path, std::size_t limit)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open asset file.");
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > limit)
            throw std::runtime_error("Asset file is empty or exceeds the import size limit.");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
            throw std::runtime_error("Cannot read asset file.");
        return bytes;
    }

    std::filesystem::path TexturePath(ufbx_string name)
    {
        return std::filesystem::u8path(name.data, name.data + name.length);
    }

    bool IsLocalImage(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        // FBX paths are untrusted: do not follow UNC paths or references outside the source folder.
        if (!path.root_name().empty() && path.root_name().wstring().starts_with(L"\\\\")) return false;
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        if (error) return false;
        const auto relative = canonical.lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return false;
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        const bool image = extension == L".png" || extension == L".jpg" || extension == L".jpeg" ||
            extension == L".bmp" || extension == L".tif" || extension == L".tiff" || extension == L".gif";
        return image && std::filesystem::is_regular_file(canonical, error) && !error;
    }

    const ufbx_material_map& ColorMap(const ufbx_material& material)
    {
        const auto& pbr = material.pbr.base_color;
        return (pbr.has_value || pbr.texture) ? pbr : material.fbx.diffuse_color;
    }

    const ufbx_texture* AlbedoTexture(const ufbx_material& material)
    {
        const auto& map = ColorMap(material);
        if (!map.texture_enabled || !map.texture) return nullptr;
        return map.texture->file_textures.count ? map.texture->file_textures.data[0] : map.texture;
    }

    std::vector<std::uint8_t> ReadAlbedo(const ufbx_texture& texture,
                                        const std::filesystem::path& source)
    {
        if (texture.content.size)
        {
            if (texture.content.size > MaxImageBytes) throw std::runtime_error("Embedded albedo is too large.");
            const auto* bytes = static_cast<const std::uint8_t*>(texture.content.data);
            return {bytes, bytes + texture.content.size};
        }
        const auto root = std::filesystem::canonical(source.parent_path());
        const auto relative = TexturePath(texture.relative_filename);
        const auto absolute = TexturePath(texture.filename);
        const auto basename = !relative.empty() ? relative.filename() : absolute.filename();
        const std::array candidates{root / relative, absolute, root / basename,
            root / "textures" / basename, root / (source.stem().wstring() + L".fbm") / basename};
        for (const auto& candidate : candidates)
        {
            if (IsLocalImage(candidate, root)) return ReadBytes(candidate, MaxImageBytes);
        }
        throw std::runtime_error("Missing albedo image (place it beside the FBX or in its textures folder).");
    }

    Float3 ToFloat3(ufbx_vec3 value)
    {
        Float3 result{static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
        if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
            throw std::runtime_error("FBX contains invalid numeric data.");
        return result;
    }

    // ZE-126: sniff an image container from its leading bytes so extracted FBX
    // textures land as real, viewable files instead of opaque blobs.
    const wchar_t* ImageExtension(const std::vector<std::uint8_t>& b)
    {
        const auto has = [&](std::initializer_list<std::uint8_t> sig, std::size_t at = 0) {
            if (b.size() < at + sig.size()) return false;
            std::size_t i = at; for (std::uint8_t s : sig) if (b[i++] != s) return false; return true;
        };
        if (has({0x89, 0x50, 0x4E, 0x47})) return L".png";
        if (has({0xFF, 0xD8, 0xFF})) return L".jpg";
        if (has({0x42, 0x4D})) return L".bmp";
        if (has({0x47, 0x49, 0x46, 0x38})) return L".gif";
        if (has({0x44, 0x44, 0x53, 0x20})) return L".dds";
        if (has({0x49, 0x49, 0x2A, 0x00}) || has({0x4D, 0x4D, 0x00, 0x2A})) return L".tif";
        return L"";
    }

    void WriteBytes(const std::filesystem::path& path, const void* data, std::size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file || !file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot write imported asset. Check project folder permissions and disk space.");
        file.close();
        if (!file) throw std::runtime_error("Cannot finish writing imported asset.");
    }
}

ModelData FbxImporter::Load(const std::filesystem::path& input, bool projectPackage)
{
    const auto file = std::filesystem::absolute(input);
    const auto bytes = ReadBytes(file, MaxFileBytes);
    ufbx_load_opts options{};
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.generate_missing_normals = true;
    options.target_axes = ufbx_axes_left_handed_y_up;
    options.target_unit_meters = 1.0;
    options.temp_allocator.memory_limit = MaxFileBytes * 2;
    options.result_allocator.memory_limit = MaxFileBytes * 2;
    options.ignore_animation = true;
    options.load_external_files = false;
    ufbx_error error{};
    const std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &options, &error), ufbx_free_scene);
    if (!scene)
    {
        std::array<char, 1024> description{};
        ufbx_format_error(description.data(), description.size(), &error);
        throw std::runtime_error(description.data());
    }
    if (scene->materials.count > 256) throw std::runtime_error("FBX exceeds the 256-material preview limit.");

    ModelData result;
    result.materials.resize(scene->materials.count + 1); // 0 is the default white material.
    std::size_t imageBytes = 0;
    for (const ufbx_material* material : scene->materials)
    {
        const auto index = material->typed_id + 1;
        auto& output = result.materials.at(index);
        const auto& map = ColorMap(*material);
        if (map.has_value) output.color = ToFloat3(map.value_vec3);
        if (const auto* texture = AlbedoTexture(*material))
        {
            try
            {
                output.image = projectPackage
                    ? ReadBytes(file.parent_path() / "albedo" / (std::to_string(index) + ".image"), MaxImageBytes)
                    : ReadAlbedo(*texture, file);
                imageBytes += output.image.size();
                if (imageBytes > MaxFileBytes) throw std::runtime_error("Total albedo images exceed 128 MB.");
            }
            catch (const std::exception& issue)
            {
                output.image.clear();
                result.warnings.emplace_back(issue.what());
            }
        }
    }

    for (const ufbx_node* node : scene->nodes)
    {
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || !node->visible || mesh->num_triangles == 0) continue;
        if (mesh->num_indices > MaxCorners || mesh->num_triangles > MaxCorners / 3 ||
            result.indices.size() + mesh->num_triangles * 3 > MaxCorners)
            throw std::runtime_error("FBX exceeds the one-million-triangle preview limit.");
        std::vector<std::uint32_t> remap(mesh->num_indices, UFBX_NO_INDEX);
        std::vector<std::uint32_t> triangles(mesh->max_face_triangles * 3);
        const auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);

        for (std::size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
        {
            const auto face = mesh->faces.data[faceIndex];
            if (face.num_indices < 3) continue;
            const auto count = ufbx_triangulate_face(triangles.data(), triangles.size(), mesh, face) * 3;
            std::uint32_t materialIndex = 0;
            if (mesh->face_material.count)
            {
                const auto slot = mesh->face_material.data[faceIndex];
                if (slot < node->materials.count) materialIndex = node->materials.data[slot]->typed_id + 1;
            }
            if (result.parts.empty() || result.parts.back().material != materialIndex)
                result.parts.push_back({static_cast<std::uint32_t>(result.indices.size()), 0, materialIndex});
            const auto& material = result.materials.at(materialIndex);
            for (std::size_t corner = 0; corner < count; ++corner)
            {
                const auto sourceIndex = triangles[corner];
                if (remap[sourceIndex] == UFBX_NO_INDEX)
                {
                    MeshVertex vertex{};
                    vertex.position = ToFloat3(ufbx_transform_position(&node->geometry_to_world,
                        ufbx_get_vertex_vec3(&mesh->vertex_position, sourceIndex)));
                    vertex.normal = ToFloat3(ufbx_transform_direction(&normalMatrix,
                        ufbx_get_vertex_vec3(&mesh->vertex_normal, sourceIndex)));
                    vertex.color = material.color;
                    if (mesh->vertex_color.exists)
                    {
                        const auto color = ufbx_get_vertex_vec4(&mesh->vertex_color, sourceIndex);
                        vertex.color.x *= static_cast<float>(color.x);
                        vertex.color.y *= static_cast<float>(color.y);
                        vertex.color.z *= static_cast<float>(color.z);
                    }
                    if (mesh->vertex_uv.exists)
                    {
                        const auto uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, sourceIndex);
                        vertex.uv = {static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y)};
                    }
                    remap[sourceIndex] = static_cast<std::uint32_t>(result.vertices.size());
                    result.vertices.push_back(vertex);
                }
                result.indices.push_back(remap[sourceIndex]);
                ++result.parts.back().indexCount;
            }
        }
    }
    if (result.indices.empty()) throw std::runtime_error("FBX contains no visible triangle meshes.");
    // Group by material rather than issuing a draw call for every alternating FBX face.
    std::vector<std::vector<std::uint32_t>> batches(result.materials.size());
    for (const auto& part : result.parts)
    {
        auto& batch = batches[part.material];
        batch.insert(batch.end(), result.indices.begin() + part.firstIndex,
                     result.indices.begin() + part.firstIndex + part.indexCount);
    }
    result.indices.clear();
    result.parts.clear();
    for (std::size_t material = 0; material < batches.size(); ++material)
    {
        const auto& batch = batches[material];
        if (batch.empty()) continue;
        result.parts.push_back({static_cast<std::uint32_t>(result.indices.size()),
                                static_cast<std::uint32_t>(batch.size()), static_cast<std::uint32_t>(material)});
        result.indices.insert(result.indices.end(), batch.begin(), batch.end());
    }
    return result;
}

std::filesystem::path FbxImporter::Import(const std::filesystem::path& source,
                                         const std::filesystem::path& assetsDirectory,
                                         std::vector<std::string>& warnings)
{
    auto extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".fbx") throw std::runtime_error("Only FBX files can be imported at this stage.");
    auto data = Load(source);
    warnings = std::move(data.warnings);
    std::filesystem::create_directories(assetsDirectory);
    auto name = source.stem().wstring();
    if (name.empty()) name = L"Model";
    // A new directory is reserved for every import: never overwrite a previous asset.
    std::filesystem::path package;
    for (unsigned suffix = 0;; ++suffix)
    {
        package = assetsDirectory / (name + (suffix ? L" (" + std::to_wstring(suffix) + L")" : L""));
        if (std::filesystem::create_directory(package)) break;
    }
    // Incomplete packages are deliberately not listed (asset.ready is written last).
    std::filesystem::copy_file(source, package / "model.fbx");
    std::filesystem::create_directory(package / "albedo");
    // ZE-126: also drop each albedo as a real image file and author a default
    // ".material" (built-in Standard) pointing at the first one, so an imported
    // model shows its textures with an editable material and no manual setup.
    std::filesystem::create_directory(package / "textures");
    std::string materialAlbedo; // package-relative image path for the default material
    Float3 materialTint{1, 1, 1};
    for (std::size_t index = 0; index < data.materials.size(); ++index)
    {
        const auto& image = data.materials[index].image;
        if (image.empty()) continue;
        WriteBytes(package / "albedo" / (std::to_string(index) + ".image"), image.data(), image.size());
        if (const std::wstring ext = ImageExtension(image); !ext.empty())
        {
            const auto file = std::wstring(L"texture_") + std::to_wstring(index) + ext;
            WriteBytes(package / "textures" / file, image.data(), image.size());
            if (materialAlbedo.empty())
            {
                const auto rel = (std::filesystem::path("textures") / file).generic_u8string();
                materialAlbedo.assign(rel.begin(), rel.end());
                materialTint = data.materials[index].color;
            }
        }
    }
    const auto packageName = package.filename().u8string();
    const std::string materialPathRel(packageName.begin(), packageName.end());
    std::ostringstream material;
    material.imbue(std::locale::classic());
    material << "ZMATERIAL 1\nshader \"\"\n";
    material << "value \"tint\" float4 " << materialTint.x << ' ' << materialTint.y << ' ' << materialTint.z << " 1\n";
    material << "value \"albedo\" texture \"" << (materialAlbedo.empty() ? "" : materialPathRel + "/" + materialAlbedo) << "\"\n";
    material << "value \"roughness\" float 0.5\nvalue \"specular\" float 0\n";
    const auto materialText = material.str();
    WriteBytes(package / (package.filename().wstring() + L".material"), materialText.data(), materialText.size());
    constexpr char marker[] = "zEngine FBX package v1 / ufbx 0.21.3\n";
    WriteBytes(package / "asset.ready", marker, sizeof(marker) - 1);
    return package / "model.fbx";
}
