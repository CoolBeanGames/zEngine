#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Renderer-owned formats: FBX types never cross the importer boundary.
struct Float2 { float x, y; };
struct Float3 { float x, y, z; };

struct MeshVertex
{
    Float3 position;
    Float3 normal;
    Float3 color;
    Float2 uv{};
};

struct MeshPart
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t material = 0;
};

struct AlbedoMaterial
{
    Float3 color{1.0f, 1.0f, 1.0f};
    // Original encoded image; decoded by the renderer, not the FBX parser.
    std::vector<std::uint8_t> image;
};

struct ModelData
{
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshPart> parts;
    std::vector<AlbedoMaterial> materials;
    std::vector<std::string> warnings;
};
