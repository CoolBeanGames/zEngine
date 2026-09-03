#include "LightmapAssets.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace zengine::lightmap
{
    namespace
    {
        bool HasExtension(const std::filesystem::path& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
            return ext == L".lightmap";
        }
        void Write(HANDLE file, std::string_view source)
        {
            DWORD written = 0;
            if (!WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) ||
                written != source.size() || !FlushFileBuffers(file))
                throw std::runtime_error("Cannot write lightmap to disk.");
        }

        struct V3 { float x, y, z; };
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Length(V3 a) { return std::sqrt(Dot(a, a)); }
        V3 Normalize(V3 a) { const float l = Length(a); return l > 1e-8f ? a * (1.0f / l) : V3{0, 1, 0}; }
        V3 From(Float3 f) { return {f.x, f.y, f.z}; }

        // Mirrors ColorCube.hlsl LightAtten. `toLight` is filled with the unit
        // direction from the surface point toward the light.
        float LightAtten(const BakeLight& light, V3 worldPos, V3& toLight, float& distToLight)
        {
            if (light.type == 0)
            {
                toLight = Normalize(From(light.direction) * -1.0f);
                distToLight = 1e30f;
                return 1.0f;
            }
            const V3 delta = From(light.position) - worldPos;
            const float dist = Length(delta);
            distToLight = dist;
            toLight = dist > 1e-4f ? delta * (1.0f / dist) : V3{0, 1, 0};
            const float t = std::clamp(dist / std::max(light.range, 1e-3f), 0.0f, 1.0f);
            float a = std::pow(std::clamp(1.0f - t, 0.0f, 1.0f), std::max(light.falloff, 0.1f));
            if (light.type == 2)
            {
                const float aligned = Dot(Normalize(From(light.direction)), toLight * -1.0f);
                a *= std::clamp((aligned - light.spotCosOuter) /
                                std::max(light.spotCosInner - light.spotCosOuter, 1e-3f), 0.0f, 1.0f);
            }
            return a;
        }

        // Moller-Trumbore. Returns true if the segment [origin, origin + dir*maxT]
        // crosses the triangle (a, b, c).
        bool RayHitsTriangle(V3 origin, V3 dir, float maxT, V3 a, V3 b, V3 c)
        {
            const V3 e1 = b - a, e2 = c - a;
            const V3 p = Cross(dir, e2);
            const float det = Dot(e1, p);
            if (std::fabs(det) < 1e-9f) return false;
            const float invDet = 1.0f / det;
            const V3 tv = origin - a;
            const float u = Dot(tv, p) * invDet;
            if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
            const V3 q = Cross(tv, e1);
            const float v = Dot(dir, q) * invDet;
            if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
            const float t = Dot(e2, q) * invDet;
            return t > 1e-3f && t < maxT - 1e-3f;
        }
    }

    const LightmapDoc::Entry* LightmapDoc::Find(GameObjectId object) const
    {
        for (const auto& e : entries) if (e.object == object) return &e;
        return nullptr;
    }

    bool IsLightmap(const std::filesystem::path& path) { return HasExtension(path); }

    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Lightmaps must be inside the current project's Assets directory.");
        if (f == file.end() || !HasExtension(file)) throw std::runtime_error("Select a .lightmap asset.");
        return file;
    }

    std::string Encode(const LightmapDoc& doc)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(9);
        out << "ZLIGHTMAP 1\n";
        out << "entries " << doc.entries.size() << '\n';
        for (const auto& entry : doc.entries)
        {
            out << "object " << entry.object << ' ' << entry.colors.size() << '\n';
            for (const auto& c : entry.colors) out << c.x << ' ' << c.y << ' ' << c.z << '\n';
        }
        return out.str();
    }

    LightmapDoc Decode(std::string_view text)
    {
        if (text.size() > MaxSourceBytes) throw std::runtime_error("Lightmap exceeds the size limit.");
        std::istringstream in{std::string(text)};
        in.imbue(std::locale::classic());
        std::string magic; int version = 0;
        if (!(in >> magic >> version) || magic != "ZLIGHTMAP" || version != 1)
            throw std::runtime_error("Not a ZLIGHTMAP 1 file.");
        LightmapDoc doc;
        std::string token;
        if (!(in >> token) || token != "entries") throw std::runtime_error("Lightmap is missing its entry count.");
        std::size_t count = 0;
        if (!(in >> count) || count > 200000) throw std::runtime_error("Lightmap entry count is out of range.");
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!(in >> token) || token != "object") throw std::runtime_error("Malformed lightmap entry.");
            LightmapDoc::Entry entry;
            std::size_t vertices = 0;
            if (!(in >> entry.object >> vertices) || vertices > 20000000)
                throw std::runtime_error("Malformed lightmap entry header.");
            entry.colors.reserve(vertices);
            for (std::size_t v = 0; v < vertices; ++v)
            {
                Float3 c{};
                if (!(in >> c.x >> c.y >> c.z) || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                    throw std::runtime_error("Malformed lightmap colour.");
                entry.colors.push_back(c);
            }
            doc.entries.push_back(std::move(entry));
        }
        return doc;
    }

    LightmapDoc Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Lightmap exceeds the size limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read lightmap.");
        return Decode(text);
    }

    void Save(const std::filesystem::path& root, const std::filesystem::path& path, const LightmapDoc& doc)
    {
        const auto file = Resolve(root, path);
        const auto encoded = Encode(doc);
        if (encoded.size() > MaxSourceBytes) throw std::runtime_error("Lightmap exceeds the size limit.");
        std::filesystem::path temp;
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (unsigned i = 0; i < 1000 && handle == INVALID_HANDLE_VALUE; ++i)
        {
            temp = file; temp += L".save-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
            handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
                throw std::runtime_error("Cannot create a temporary save file.");
        }
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a temporary save file.");
        try
        {
            Write(handle, encoded); CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace lightmap. Original file was preserved.");
        }
        catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); DeleteFileW(temp.c_str()); throw; }
    }

    LightmapDoc Bake(const std::vector<BakeMesh>& meshes, const std::vector<BakeLight>& lights, Float3 ambient)
    {
        // One flat world-space triangle list for the shadow-ray test.
        std::vector<std::array<V3, 3>> triangles;
        for (const auto& mesh : meshes)
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            {
                const auto a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
                if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size()) continue;
                triangles.push_back({From(mesh.positions[a]), From(mesh.positions[b]), From(mesh.positions[c])});
            }

        LightmapDoc doc;
        for (const auto& mesh : meshes)
        {
            LightmapDoc::Entry entry;
            entry.object = mesh.object;
            entry.colors.resize(mesh.positions.size(), ambient);
            for (std::size_t v = 0; v < mesh.positions.size(); ++v)
            {
                const V3 p = From(mesh.positions[v]);
                const V3 n = Normalize(v < mesh.normals.size() ? From(mesh.normals[v]) : V3{0, 1, 0});
                V3 sum{ambient.x, ambient.y, ambient.z};
                for (const auto& light : lights)
                {
                    V3 toLight; float distToLight = 0;
                    const float atten = LightAtten(light, p, toLight, distToLight);
                    const float ndotl = std::max(Dot(n, toLight), 0.0f);
                    if (atten <= 0.0f || ndotl <= 0.0f) continue;

                    const V3 origin = p + n * 0.02f;
                    bool occluded = false;
                    for (const auto& tri : triangles)
                        if (RayHitsTriangle(origin, toLight, distToLight, tri[0], tri[1], tri[2])) { occluded = true; break; }
                    if (occluded) continue;

                    const float k = ndotl * atten * light.intensity;
                    sum = sum + V3{light.color.x * k, light.color.y * k, light.color.z * k};
                }
                entry.colors[v] = {sum.x, sum.y, sum.z};
            }
            doc.entries.push_back(std::move(entry));
        }
        return doc;
    }

    ModelData Apply(ModelData base, const LightmapDoc::Entry& entry)
    {
        if (entry.colors.size() != base.vertices.size()) return base;
        for (std::size_t i = 0; i < base.vertices.size(); ++i)
        {
            base.vertices[i].color.x *= entry.colors[i].x;
            base.vertices[i].color.y *= entry.colors[i].y;
            base.vertices[i].color.z *= entry.colors[i].z;
        }
        return base;
    }
}
