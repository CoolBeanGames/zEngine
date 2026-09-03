#include "ShaderAssets.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace zengine::shaders
{
    namespace
    {
        bool HasExtension(const std::filesystem::path& path)
        {
            auto ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
            return ext == L".shader";
        }
        void Write(HANDLE file, std::string_view source)
        {
            DWORD written = 0;
            if (!WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) ||
                written != source.size() || !FlushFileBuffers(file))
                throw std::runtime_error("Cannot write shader to disk.");
        }

        constexpr const char* kTemplate =
            "// Material shader (HLSL). Scalars in the cbuffer below and Texture2D\r\n"
            "// declarations become editable parameters on every Material Instance\r\n"
            "// (ZE-65). This file only defines code + parameters; it is not applied\r\n"
            "// to any object yet.\r\n"
            "\r\n"
            "cbuffer Parameters : register(b0)\r\n"
            "{\r\n"
            "    float4 albedo;     // default: 1 1 1 1\r\n"
            "    float  smoothness; // default: 0.5\r\n"
            "};\r\n"
            "\r\n"
            "Texture2D albedoMap : register(t0);\r\n"
            "SamplerState linearSampler : register(s0);\r\n"
            "\r\n"
            "struct PixelInput\r\n"
            "{\r\n"
            "    float4 position : SV_POSITION;\r\n"
            "    float2 uv       : TEXCOORD0;\r\n"
            "};\r\n"
            "\r\n"
            "float4 PSMain(PixelInput input) : SV_TARGET\r\n"
            "{\r\n"
            "    return albedoMap.Sample(linearSampler, input.uv) * albedo;\r\n"
            "}\r\n";
    }

    const char* ParamTypeName(ParamType type)
    {
        switch (type)
        {
        case ParamType::Float:     return "float";
        case ParamType::Float2:    return "float2";
        case ParamType::Float3:    return "float3";
        case ParamType::Float4:    return "float4";
        case ParamType::Texture2D: return "Texture2D";
        }
        return "float";
    }

    bool IsShader(const std::filesystem::path& path) { return HasExtension(path); }

    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Shaders must be inside the current project's Assets directory.");
        if (f == file.end() || !HasExtension(file)) throw std::runtime_error("Select a .shader asset.");
        return file;
    }

    std::string Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Shader exceeds the 256 KiB editor limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read shader.");
        if (text.find('\0') != std::string::npos || (!text.empty() &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)))
            throw std::runtime_error("Shaders must be UTF-8 text without NUL characters.");
        return text;
    }

    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              std::string_view source, const std::string* expected)
    {
        const auto file = Resolve(root, path);
        if (source.size() > MaxSourceBytes) throw std::runtime_error("Shader exceeds the 256 KiB editor limit.");
        if (expected && Load(file) != *expected)
            throw std::runtime_error("The file changed on disk. Reload it before saving to avoid overwriting external edits.");
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
            Write(handle, source); CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            if (expected && Load(file) != *expected) throw std::runtime_error("File changed during save. Reload before saving.");
            if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace shader. Original file was preserved.");
        }
        catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); DeleteFileW(temp.c_str()); throw; }
    }

    std::filesystem::path Create(const std::filesystem::path& root, std::string name)
    {
        const auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (name.empty() || name.size() > 80 || !alpha(name.front()) ||
            !std::all_of(name.begin(), name.end(), [&](unsigned char c) { return alpha(c) || (c >= '0' && c <= '9'); }))
            throw std::runtime_error("Shader names must be identifiers: letters, digits, underscores; not starting with a digit.");
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i)
        {
            const auto stem = name + (i ? std::to_string(i) : "");
            const auto path = Resolve(root, std::filesystem::path(stem + ".shader"));
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                throw std::runtime_error("Cannot create shader asset.");
            }
            try { Write(file, kTemplate); CloseHandle(file); }
            catch (...) { CloseHandle(file); DeleteFileW(path.c_str()); throw; }
            return path;
        }
        throw std::runtime_error("Too many shaders with this name.");
    }

    // ---- parsing ---------------------------------------------------------

    namespace
    {
        std::string StripComments(std::string_view src)
        {
            std::string out;
            for (std::size_t i = 0; i < src.size();)
            {
                if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
                { while (i < src.size() && src[i] != '\n') ++i; }
                else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
                { i += 2; while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i; i = std::min(i + 2, src.size()); }
                else out.push_back(src[i++]);
            }
            return out;
        }
        bool ParamTypeFor(const std::string& token, ParamType& out)
        {
            if (token == "float")  { out = ParamType::Float;  return true; }
            if (token == "float2") { out = ParamType::Float2; return true; }
            if (token == "float3") { out = ParamType::Float3; return true; }
            if (token == "float4") { out = ParamType::Float4; return true; }
            return false;
        }
        int Components(ParamType type)
        {
            switch (type)
            {
            case ParamType::Float:  return 1;
            case ParamType::Float2: return 2;
            case ParamType::Float3: return 3;
            case ParamType::Float4: return 4;
            default: return 0;
            }
        }
    }

    ShaderProgram Parse(std::string_view source)
    {
        ShaderProgram program;
        program.source.assign(source);

        // Parameters: the scalars declared inside `cbuffer Parameters { ... }`, and
        // every top-level Texture2D. Defaults come from a trailing `// default: ...`.
        const std::string clean = StripComments(source);
        const auto cbuffer = clean.find("cbuffer");
        if (cbuffer != std::string::npos)
        {
            const auto open = clean.find('{', cbuffer);
            const auto close = open == std::string::npos ? std::string::npos : clean.find('}', open);
            if (open != std::string::npos && close != std::string::npos)
            {
                std::istringstream body(clean.substr(open + 1, close - open - 1));
                std::string statement;
                while (std::getline(body, statement, ';'))
                {
                    std::istringstream decl(statement);
                    std::string type, ident;
                    if (!(decl >> type >> ident)) continue;
                    while (!ident.empty() && !(std::isalnum(static_cast<unsigned char>(ident.back())) || ident.back() == '_')) ident.pop_back();
                    ParamType kind;
                    if (ident.empty() || !ParamTypeFor(type, kind)) continue;
                    Parameter parameter{ident, kind, {{0, 0, 0, 0}}};

                    // Pull the default from the original source line (comments intact).
                    const auto at = source.find(ident);
                    if (at != std::string_view::npos)
                    {
                        const auto lineEnd = source.find('\n', at);
                        const auto line = std::string(source.substr(at, lineEnd == std::string_view::npos ? lineEnd : lineEnd - at));
                        const auto marker = line.find("default");
                        if (marker != std::string::npos)
                        {
                            std::istringstream defaults(line.substr(marker + 7));
                            char skip;
                            if (defaults.peek() == ':') defaults >> skip;
                            for (int c = 0; c < Components(kind); ++c) defaults >> parameter.value[static_cast<std::size_t>(c)];
                        }
                    }
                    program.parameters.push_back(std::move(parameter));
                }
            }
        }
        for (std::size_t at = clean.find("Texture2D"); at != std::string::npos; at = clean.find("Texture2D", at + 1))
        {
            std::istringstream rest(clean.substr(at + 9));
            std::string ident;
            if (!(rest >> ident)) continue;
            while (!ident.empty() && !(std::isalnum(static_cast<unsigned char>(ident.back())) || ident.back() == '_')) ident.pop_back();
            if (!ident.empty())
                program.parameters.push_back(Parameter{ident, ParamType::Texture2D, {{0, 0, 0, 0}}});
        }

        // Compile the pixel entry point to prove the HLSL is valid.
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(source.data(), source.size(), "material.shader", nullptr, nullptr,
                                      "PSMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (errors)
        {
            program.errors.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            while (!program.errors.empty() && (program.errors.back() == '\0' || program.errors.back() == '\n' || program.errors.back() == '\r'))
                program.errors.pop_back();
            errors->Release();
        }
        program.compiled = SUCCEEDED(hr);
        if (code) code->Release();
        if (!program.compiled && program.errors.empty()) program.errors = "HLSL compilation failed (no diagnostics).";
        return program;
    }

    // ---- editor analysis ------------------------------------------------

    scripts::Analysis Analyze(std::wstring_view source)
    {
        using scripts::TokenKind;
        scripts::Analysis out;
        std::vector<std::pair<wchar_t, std::size_t>> brackets;
        std::wstring line;

        for (std::size_t p = 0; p < source.size();)
        {
            const auto start = p;
            const wchar_t c = source[p++];
            if (std::iswspace(c) || (start == 0 && c == 0xfeff)) continue;

            if (c == L'#') // preprocessor directive: highlight the whole line as a keyword run
            {
                while (p < source.size() && source[p] != L'\n' && source[p] != L'\r') ++p;
                out.spans.push_back({start, p - start, TokenKind::Keyword});
                continue;
            }
            if (c == L'/' && p < source.size() && (source[p] == L'/' || source[p] == L'*'))
            {
                const bool block = source[p++] == L'*';
                if (block)
                {
                    while (p + 1 < source.size() && !(source[p] == L'*' && source[p + 1] == L'/')) ++p;
                    p = p + 1 >= source.size() ? source.size() : p + 2;
                }
                else while (p < source.size() && source[p] != L'\r' && source[p] != L'\n') ++p;
                out.spans.push_back({start, p - start, TokenKind::Comment});
                continue;
            }
            if (c == L'"')
            {
                while (p < source.size() && source[p] != L'"' && source[p] != L'\r' && source[p] != L'\n') ++p;
                if (p < source.size() && source[p] == L'"') ++p;
                out.spans.push_back({start, p - start, TokenKind::String});
                continue;
            }
            if (std::iswalpha(c) || c == L'_')
            {
                while (p < source.size() && (std::iswalnum(source[p]) || source[p] == L'_')) ++p;
                const auto word = source.substr(start, p - start);
                static const std::vector<std::wstring_view> keywords = {
                    L"cbuffer", L"struct", L"return", L"if", L"else", L"for", L"while", L"do", L"switch", L"case",
                    L"true", L"false", L"register", L"static", L"const", L"inline", L"in", L"out", L"inout",
                    L"SamplerState", L"Texture1D", L"Texture2D", L"Texture3D", L"TextureCube", L"RWTexture2D",
                    L"StructuredBuffer", L"RWStructuredBuffer", L"groupshared", L"numthreads"};
                static const std::vector<std::wstring_view> types = {
                    L"void", L"bool", L"int", L"int2", L"int3", L"int4", L"uint", L"uint2", L"uint3", L"uint4",
                    L"half", L"float", L"float2", L"float3", L"float4", L"double",
                    L"float2x2", L"float3x3", L"float4x4", L"matrix", L"min16float"};
                if (std::find(keywords.begin(), keywords.end(), word) != keywords.end())
                    out.spans.push_back({start, p - start, TokenKind::Keyword});
                else if (std::find(types.begin(), types.end(), word) != types.end())
                    out.spans.push_back({start, p - start, TokenKind::Type});
                continue;
            }
            if (c >= L'0' && c <= L'9')
            {
                while (p < source.size() && (std::iswalnum(source[p]) || source[p] == L'.')) ++p;
                out.spans.push_back({start, p - start, TokenKind::Number});
                continue;
            }
            if (c == L'{' || c == L'(' || c == L'[') brackets.push_back({c, start});
            else if (c == L'}' || c == L')' || c == L']')
            {
                const wchar_t open = c == L'}' ? L'{' : c == L')' ? L'(' : L'[';
                if (brackets.empty() || brackets.back().first != open)
                {
                    if (out.errors.size() < 100) out.errors.push_back({start, std::size_t{1}, std::size_t{0}, std::size_t{0}, "Unexpected closing bracket."});
                }
                else brackets.pop_back();
            }
        }
        for (const auto& [bracket, pos] : brackets)
        {
            (void)bracket;
            if (out.errors.size() < 100) out.errors.push_back({pos, std::size_t{1}, std::size_t{0}, std::size_t{0}, "Unclosed bracket."});
        }

        // Fold in the HLSL compiler diagnostics.
        const std::string utf8 = [&] {
            const int n = WideCharToMultiByte(CP_UTF8, 0, source.data(), static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, source.data(), static_cast<int>(source.size()), s.data(), n, nullptr, nullptr);
            return s;
        }();
        const auto program = Parse(utf8);
        if (!program.compiled && !program.errors.empty() && out.errors.size() < 100)
            out.errors.push_back({std::size_t{0}, source.empty() ? std::size_t{0} : std::size_t{1},
                                  std::size_t{1}, std::size_t{1}, program.errors});
        return out;
    }
}
