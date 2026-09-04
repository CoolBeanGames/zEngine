#pragma once
#include "DataAssets.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ZE-92: a Data Sheet asset (".zsheet"). It is a table of data-object instances
// of one zScript `struct` type: every row is an instance (keyed by a name or a
// numeric id), every column is one of the struct's fields. Scripts read a cell
// with `sheet[row, column]`, where row/column may each be an int index or a
// string name.
namespace zengine::datasheet
{
    constexpr std::size_t MaxSourceBytes = 1024 * 1024;

    struct Row
    {
        std::string key;                              // row name / id (unique within the sheet)
        std::vector<dataobj::FieldValue> cells;        // one per struct field, in declared order
        bool operator==(const Row&) const = default;
    };
    struct SheetDoc
    {
        std::string type;             // the data-object struct type name
        std::vector<Row> rows;
        bool operator==(const SheetDoc&) const = default;
    };

    bool IsSheet(const std::filesystem::path& path); // extension ".zsheet"
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    SheetDoc Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              const SheetDoc& doc, const SheetDoc* expected = nullptr);
    std::filesystem::path Create(const std::filesystem::path& root, const std::string& type, std::string name = {});

    std::string Encode(const SheetDoc& doc);
    SheetDoc Decode(std::string_view text); // throws std::runtime_error on malformed input

    // Cell lookup used by the script host. `row`/`col` are the raw text of an int
    // index ("0", "2") or a name. Returns the cell's value text, or throws.
    // `fieldNames` is the struct's field order (column names).
    std::string Cell(const SheetDoc& doc, const std::vector<std::string>& fieldNames,
                     std::string_view row, std::string_view col, std::string* outType = nullptr);
}
