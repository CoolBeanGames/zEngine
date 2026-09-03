#pragma once
#include "ShaderAssets.h" // shaders::Parameter / ParamType / Parse

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// ZE-65: a Material Instance (".material") takes a shader (a ".shader" from ZE-64,
// or the engine's built-in Standard shader) and pins concrete values to its
// parameters. This is what a Model - and later a UI element - actually references
// for rendering.
namespace zengine::materials
{
    constexpr std::size_t MaxSourceBytes = 64 * 1024;

    // One pinned parameter value on a material.
    struct Value
    {
        std::string name;
        shaders::ParamType type = shaders::ParamType::Float4;
        std::array<float, 4> numbers{{0, 0, 0, 0}};
        std::string texture; // project-relative image path (type == Texture2D)
        bool operator==(const Value&) const = default;
    };

    struct MaterialDoc
    {
        std::string shader;        // project-relative ".shader" path; empty = built-in Standard (albedo x tint)
        std::vector<Value> values; // author-pinned parameter values
        bool operator==(const MaterialDoc&) const = default;
    };

    bool IsMaterial(const std::filesystem::path& path); // extension ".material"
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    MaterialDoc Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              const MaterialDoc& doc, const MaterialDoc* expected = nullptr);
    std::filesystem::path Create(const std::filesystem::path& root, std::string name = "NewMaterial");
    std::string Encode(const MaterialDoc& doc);
    MaterialDoc Decode(std::string_view text); // throws std::runtime_error on malformed input

    // Built-in "Standard" shader parameters: an albedo texture multiplied by a tint,
    // sampled through the model's UVs. Used when MaterialDoc::shader is empty.
    const std::vector<Value>& StandardParameters();

    // The material's effective parameter list: the shader's declared parameters
    // (built-in Standard, or shaders::Parse of the referenced ".shader") with the
    // material's pinned values applied over the shader defaults, in declaration
    // order. `loadShaderSource` returns a ".shader" file's text by project-relative
    // path (only called for a non-empty MaterialDoc::shader).
    struct Effective
    {
        bool builtin = true;
        std::string pixelShaderHlsl;   // "" when builtin
        std::vector<Value> parameters; // merged, shader declaration order
        bool ok = true;
        std::string error;

        const Value* Find(std::string_view name) const;
        std::array<float, 4> Numbers(std::string_view name, std::array<float, 4> fallback = {{0, 0, 0, 0}}) const;
        std::string Texture(std::string_view name) const;
    };
    Effective Resolve(const MaterialDoc& doc,
                      const std::function<std::string(const std::string&)>& loadShaderSource);
}
