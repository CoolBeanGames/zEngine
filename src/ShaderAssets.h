#pragma once
#include "ScriptAssets.h" // reuse scripts::Analysis / Span / Diagnostic / TokenKind

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ZE-64: the first half of the Material system. A ".shader" asset is plain HLSL
// text authored in the editor. It only *defines* code and parameters - it is not
// yet applied to any object (that is the Material Instance work, ZE-65).
namespace zengine::shaders
{
    constexpr std::size_t MaxSourceBytes = 256 * 1024;

    bool IsShader(const std::filesystem::path& path); // extension ".shader"
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    std::string Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              std::string_view source, const std::string* expected = nullptr);
    std::filesystem::path Create(const std::filesystem::path& root, std::string name = "NewMaterial");

    // A tweakable value the Material Instance layer (ZE-65) will expose per material.
    enum class ParamType { Float, Float2, Float3, Float4, Texture2D };
    const char* ParamTypeName(ParamType type);
    struct Parameter
    {
        std::string name;
        ParamType type = ParamType::Float;
        std::array<float, 4> value{{0, 0, 0, 0}}; // numeric default (unused for Texture2D)
    };

    struct ShaderProgram
    {
        std::string source;
        std::vector<Parameter> parameters; // cbuffer "Parameters" scalars + Texture2D slots
        bool compiled = false;
        std::string errors;                // HLSL compile diagnostics ("" when compiled)
    };

    // Parses the parameter declarations and compiles the pixel entry point
    // (PSMain / ps_5_0) with D3DCompile to prove the HLSL is valid.
    ShaderProgram Parse(std::string_view source);

    // Editor tokenising + error list (HLSL keywords/types; also surfaces the
    // D3DCompile diagnostics as inline errors).
    scripts::Analysis Analyze(std::wstring_view source);
}
