#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ZE-128: a Data Object asset (".zdata"). It stores one instance of a zScript
// `struct` (a "data object", ZE-91) - the struct's type name plus a value for
// each of its instance fields. A behaviour references a ".zdata" through an
// exported field of the matching data-object type; at Play the host instantiates
// the struct and fills it from the file. `data_object.save()` writes the live
// instance back here, so the change outlives the Play session.
namespace zengine::dataobj
{
    constexpr std::size_t MaxSourceBytes = 256 * 1024;

    struct FieldValue
    {
        std::string name;
        std::string type;  // canonical zScript type: int / float / bool / string / Vector3 / Vector2
        std::string value; // textual form (see Encode); empty => the type's default
        bool operator==(const FieldValue&) const = default;
    };
    struct DataDoc
    {
        std::string type;               // the data-object struct type name
        std::vector<FieldValue> fields; // in the struct's declared (base-to-derived) order
        bool operator==(const DataDoc&) const = default;
    };

    bool IsData(const std::filesystem::path& path); // extension ".zdata"
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    DataDoc Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              const DataDoc& doc, const DataDoc* expected = nullptr);
    // `type` is the struct name; `name` seeds the file name (defaults to the type).
    std::filesystem::path Create(const std::filesystem::path& root, const std::string& type, std::string name = {});

    std::string Encode(const DataDoc& doc);
    DataDoc Decode(std::string_view text); // throws std::runtime_error on malformed input

    // Value text helpers (shared with the inspector / host):
    //   int    -> "42"          bool -> "true" / "false"
    //   float  -> "3.5"         string -> the raw text, quoted+escaped on disk
    //   Vector3-> "1, 2, 3"     Vector2-> "1, 2"
    bool IsStorableType(std::string_view canonicalType);
}
