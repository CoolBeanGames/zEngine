#pragma once
// ZE-113: a ".lightmap" is an offline bake of the direct lighting term for the
// static meshes in a scene. Each entry holds one object's per-vertex RGB
// multiplier (ambient + every static light's N.L * attenuation * colour, with a
// shadow-ray occlusion test). A static + lightmapped mesh renders unlit, straight
// from `originalVertexColour * bakedTerm`, with no per-frame light loop.

#include "ModelData.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zengine::lightmap
{
    using GameObjectId = std::uint64_t; // matches zengine::GameObjectId (core/GameObject.h)
    constexpr std::size_t MaxSourceBytes = 64 * 1024 * 1024;

    struct LightmapDoc
    {
        struct Entry
        {
            GameObjectId object = 0;
            std::vector<Float3> colors; // one per mesh vertex, in ModelData order
            bool operator==(const Entry& other) const
            {
                if (object != other.object || colors.size() != other.colors.size()) return false;
                for (std::size_t i = 0; i < colors.size(); ++i)
                    if (colors[i].x != other.colors[i].x || colors[i].y != other.colors[i].y || colors[i].z != other.colors[i].z)
                        return false;
                return true;
            }
        };
        std::vector<Entry> entries;
        bool operator==(const LightmapDoc& other) const { return entries == other.entries; }

        const Entry* Find(GameObjectId object) const;
    };

    bool IsLightmap(const std::filesystem::path& path); // extension ".lightmap"
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    LightmapDoc Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path, const LightmapDoc& doc);
    std::string Encode(const LightmapDoc& doc);
    LightmapDoc Decode(std::string_view text); // throws std::runtime_error on malformed input

    // --- The baker -------------------------------------------------------------
    // Geometry is passed already in world space; `indices` index into `positions`.
    struct BakeMesh
    {
        GameObjectId object = 0;
        std::vector<Float3> positions;
        std::vector<Float3> normals;
        std::vector<std::uint32_t> indices;
    };
    // Mirrors the renderer's light model (see ColorCube.hlsl LightAtten).
    struct BakeLight
    {
        int type = 0; // 0 directional, 1 point, 2 spot
        Float3 position{0, 0, 0};
        Float3 direction{0, -1, 0};
        Float3 color{1, 1, 1};
        float intensity = 1;
        float range = 10;
        float falloff = 2;
        float spotCosInner = 0.94f;
        float spotCosOuter = 0.82f;
    };

    LightmapDoc Bake(const std::vector<BakeMesh>& meshes,
                     const std::vector<BakeLight>& lights,
                     Float3 ambient = {0.10f, 0.10f, 0.12f});

    // Applies a baked entry to a copy of the base model: multiplies each vertex
    // colour by the baked term. A vertex-count mismatch returns the model
    // unchanged (the caller should then fall back to the lit path).
    ModelData Apply(ModelData base, const LightmapDoc::Entry& entry);
}
