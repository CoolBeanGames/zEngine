#include "MaterialAssets.h"

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

namespace zengine::materials
{
    namespace
    {
        bool HasExtension(const std::filesystem::path& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
            return ext == L".material";
        }
        void Write(HANDLE file, std::string_view source)
        {
            DWORD written = 0;
            if (!WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) ||
                written != source.size() || !FlushFileBuffers(file))
                throw std::runtime_error("Cannot write material to disk.");
        }
        int Components(shaders::ParamType type)
        {
            switch (type)
            {
            case shaders::ParamType::Float:  return 1;
            case shaders::ParamType::Float2: return 2;
            case shaders::ParamType::Float3: return 3;
            case shaders::ParamType::Float4: return 4;
            default: return 0;
            }
        }
        const char* TypeToken(shaders::ParamType type)
        {
            switch (type)
            {
            case shaders::ParamType::Float:     return "float";
            case shaders::ParamType::Float2:    return "float2";
            case shaders::ParamType::Float3:    return "float3";
            case shaders::ParamType::Float4:    return "float4";
            case shaders::ParamType::Texture2D: return "texture";
            }
            return "float4";
        }
        bool TokenToType(const std::string& token, shaders::ParamType& out)
        {
            if (token == "float")   { out = shaders::ParamType::Float;     return true; }
            if (token == "float2")  { out = shaders::ParamType::Float2;    return true; }
            if (token == "float3")  { out = shaders::ParamType::Float3;    return true; }
            if (token == "float4")  { out = shaders::ParamType::Float4;    return true; }
            if (token == "texture") { out = shaders::ParamType::Texture2D; return true; }
            return false;
        }
        std::string ReadQuoted(std::istream& in)
        {
            in >> std::ws;
            if (in.peek() != '"') throw std::runtime_error("Expected a quoted material string.");
            std::string text;
            if (!(in >> std::quoted(text))) throw std::runtime_error("Malformed quoted material string.");
            return text;
        }
    }

    bool IsMaterial(const std::filesystem::path& path) { return HasExtension(path); }

    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Materials must be inside the current project's Assets directory.");
        if (f == file.end() || !HasExtension(file)) throw std::runtime_error("Select a .material asset.");
        return file;
    }

    std::string Encode(const MaterialDoc& doc)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "ZMATERIAL 1\n";
        out << "shader " << std::quoted(doc.shader) << '\n';
        if (!doc.lit) out << "unlit 1\n"; // ZE-74 (absent => lit, so old files load unchanged)
        for (const auto& value : doc.values)
        {
            out << "value " << std::quoted(value.name) << ' ' << TypeToken(value.type) << ' ';
            if (value.type == shaders::ParamType::Texture2D)
                out << std::quoted(value.texture);
            else
                for (int c = 0; c < Components(value.type); ++c) { if (c) out << ' '; out << value.numbers[static_cast<std::size_t>(c)]; }
            out << '\n';
        }
        return out.str();
    }

    MaterialDoc Decode(std::string_view text)
    {
        if (text.size() > MaxSourceBytes) throw std::runtime_error("Material exceeds the size limit.");
        std::istringstream in{std::string(text)};
        in.imbue(std::locale::classic());
        std::string magic; int version = 0;
        if (!(in >> magic >> version) || magic != "ZMATERIAL" || version != 1)
            throw std::runtime_error("Not a ZMATERIAL 1 file.");
        MaterialDoc doc;
        std::string token;
        while (in >> token)
        {
            if (token == "shader")
            {
                doc.shader = ReadQuoted(in);
                if (!doc.shader.empty() && (doc.shader.find("..") != std::string::npos || doc.shader.front() == '/'))
                    throw std::runtime_error("Material shader path must be project-relative.");
            }
            else if (token == "unlit")
            {
                int v = 0; if (!(in >> v)) throw std::runtime_error("Invalid material lighting flag.");
                doc.lit = v == 0;
            }
            else if (token == "value")
            {
                if (doc.values.size() >= 256) throw std::runtime_error("Too many material parameters.");
                Value value;
                value.name = ReadQuoted(in);
                std::string typeToken;
                if (!(in >> typeToken) || !TokenToType(typeToken, value.type) || value.name.empty())
                    throw std::runtime_error("Malformed material parameter.");
                if (value.type == shaders::ParamType::Texture2D)
                    value.texture = ReadQuoted(in);
                else
                    for (int c = 0; c < Components(value.type); ++c)
                        if (!(in >> value.numbers[static_cast<std::size_t>(c)]) || !std::isfinite(value.numbers[static_cast<std::size_t>(c)]))
                            throw std::runtime_error("Malformed material parameter value.");
                doc.values.push_back(std::move(value));
            }
            else throw std::runtime_error("Unknown material directive '" + token + "'.");
        }
        return doc;
    }

    MaterialDoc Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Material exceeds the size limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read material.");
        return Decode(text);
    }

    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              const MaterialDoc& doc, const MaterialDoc* expected)
    {
        const auto file = Resolve(root, path);
        const auto encoded = Encode(doc);
        if (encoded.size() > MaxSourceBytes) throw std::runtime_error("Material exceeds the size limit.");
        if (expected && Load(file) != *expected)
            throw std::runtime_error("The material changed on disk. Reload before saving.");
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
                throw std::runtime_error("Cannot replace material. Original file was preserved.");
        }
        catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); DeleteFileW(temp.c_str()); throw; }
    }

    std::filesystem::path Create(const std::filesystem::path& root, std::string name)
    {
        const auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (name.empty() || name.size() > 80 || !alpha(name.front()) ||
            !std::all_of(name.begin(), name.end(), [&](unsigned char c) { return alpha(c) || (c >= '0' && c <= '9'); }))
            throw std::runtime_error("Material names must be identifiers: letters, digits, underscores; not starting with a digit.");
        std::filesystem::create_directories(root);
        MaterialDoc doc; // built-in Standard, opaque white
        doc.values = StandardParameters();
        const auto encoded = Encode(doc);
        for (unsigned i = 0; i < 10000; ++i)
        {
            const auto stem = name + (i ? std::to_string(i) : "");
            const auto path = Resolve(root, std::filesystem::path(stem + ".material"));
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                throw std::runtime_error("Cannot create material asset.");
            }
            try { Write(file, encoded); CloseHandle(file); }
            catch (...) { CloseHandle(file); DeleteFileW(path.c_str()); throw; }
            return path;
        }
        throw std::runtime_error("Too many materials with this name.");
    }

    const std::vector<Value>& StandardParameters()
    {
        static const std::vector<Value> params = {
            Value{"tint",   shaders::ParamType::Float4, {{1, 1, 1, 1}}, ""},
            Value{"albedo", shaders::ParamType::Texture2D, {{0, 0, 0, 0}}, ""},
        };
        return params;
    }

    const Value* Effective::Find(std::string_view name) const
    {
        for (const auto& value : parameters) if (value.name == name) return &value;
        return nullptr;
    }
    std::array<float, 4> Effective::Numbers(std::string_view name, std::array<float, 4> fallback) const
    {
        const auto* value = Find(name);
        return value ? value->numbers : fallback;
    }
    std::string Effective::Texture(std::string_view name) const
    {
        const auto* value = Find(name);
        return value ? value->texture : std::string{};
    }

    Effective Resolve(const MaterialDoc& doc,
                      const std::function<std::string(const std::string&)>& loadShaderSource)
    {
        Effective effective;
        effective.lit = doc.lit;
        std::vector<Value> declared;

        if (doc.shader.empty())
        {
            effective.builtin = true;
            declared = StandardParameters();
        }
        else
        {
            effective.builtin = false;
            std::string source;
            try { source = loadShaderSource(doc.shader); }
            catch (const std::exception& error) { effective.ok = false; effective.error = error.what(); return effective; }
            const auto parsed = shaders::Parse(source);
            effective.pixelShaderHlsl = source;
            if (!parsed.compiled) { effective.ok = false; effective.error = parsed.errors; }
            for (const auto& parameter : parsed.parameters)
                declared.push_back(Value{parameter.name, parameter.type, parameter.value, ""});
        }

        // Apply the material's pinned values over the shader-declared defaults.
        for (auto& value : declared)
            if (const auto it = std::find_if(doc.values.begin(), doc.values.end(),
                    [&](const Value& v) { return v.name == value.name && v.type == value.type; });
                it != doc.values.end())
            {
                value.numbers = it->numbers;
                value.texture = it->texture;
            }
        effective.parameters = std::move(declared);
        return effective;
    }
}
