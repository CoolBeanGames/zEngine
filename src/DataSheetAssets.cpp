#include "DataSheetAssets.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace zengine::datasheet
{
    namespace
    {
        std::string Trim(std::string s)
        {
            const auto ns = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
            s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
            return s;
        }
        bool Identifier(const std::string& s)
        {
            const auto a = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            return !s.empty() && s.size() <= 96 && a(static_cast<unsigned char>(s.front())) &&
                std::all_of(s.begin(), s.end(), [&](unsigned char c) { return a(c) || (c >= '0' && c <= '9'); });
        }
        std::string Quote(std::string_view s)
        {
            std::string o = "\"";
            for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c != '\r' && c != '\n') o += c; }
            return o + '"';
        }
        std::string Unquote(std::string_view s)
        {
            std::string t = Trim(std::string(s));
            if (t.size() < 2 || t.front() != '"' || t.back() != '"') throw std::runtime_error("Expected a quoted value.");
            std::string o;
            for (std::size_t i = 1; i + 1 < t.size(); ++i) { if (t[i] == '\\' && i + 2 < t.size()) o += t[++i]; else o += t[i]; }
            return o;
        }
        std::optional<std::size_t> AsIndex(std::string_view s)
        {
            if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
            std::size_t v = 0; std::from_chars(s.data(), s.data() + s.size(), v); return v;
        }
    }

    bool IsSheet(const std::filesystem::path& path)
    {
        auto ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        return ext == L".zsheet";
    }
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Data sheets must be inside the current project's Assets directory.");
        if (f == file.end() || !IsSheet(file)) throw std::runtime_error("Select a .zsheet data sheet asset.");
        return file;
    }

    std::string Encode(const SheetDoc& doc)
    {
        if (!Identifier(doc.type)) throw std::runtime_error("A data sheet needs a struct type name.");
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "ZSHEET 1\r\ntype " << doc.type << "\r\n";
        std::vector<std::string> seen;
        for (const auto& row : doc.rows)
        {
            if (row.key.empty() || row.key.find_first_of("\r\n\"") != std::string::npos)
                throw std::runtime_error("Invalid data sheet row key.");
            if (std::find(seen.begin(), seen.end(), row.key) != seen.end())
                throw std::runtime_error("Duplicate data sheet row key '" + row.key + "'.");
            seen.push_back(row.key);
            out << "row " << Quote(row.key) << "\r\n";
            for (const auto& c : row.cells)
            {
                if (!Identifier(c.name) || !dataobj::IsStorableType(c.type)) continue;
                out << "field " << c.name << ' ' << c.type << ' ';
                if (c.type == "string") out << Quote(c.value);
                else out << (c.value.empty() ? (c.type == "bool" ? "false" : c.type == "Vector3" ? "0, 0, 0" : c.type == "Vector2" ? "0, 0" : "0") : c.value);
                out << "\r\n";
            }
        }
        return out.str();
    }

    SheetDoc Decode(std::string_view text)
    {
        std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
        std::string line;
        if (!std::getline(in, line) || Trim(line).substr(0, 8) != "ZSHEET 1") throw std::runtime_error("Not a ZSHEET 1 file.");
        SheetDoc doc;
        Row* current = nullptr;
        while (std::getline(in, line))
        {
            const auto t = Trim(line);
            if (t.empty()) continue;
            std::istringstream row{t};
            std::string kind; row >> kind;
            if (kind == "type") { row >> doc.type; if (!Identifier(doc.type)) throw std::runtime_error("Malformed data sheet type."); }
            else if (kind == "row")
            {
                std::string rest; std::getline(row, rest);
                doc.rows.push_back({Unquote(Trim(rest)), {}});
                current = &doc.rows.back();
            }
            else if (kind == "field")
            {
                if (!current) throw std::runtime_error("A data sheet field must follow a row.");
                dataobj::FieldValue fv; row >> fv.name >> fv.type;
                if (!Identifier(fv.name) || !dataobj::IsStorableType(fv.type)) throw std::runtime_error("Malformed data sheet field.");
                std::string rest; std::getline(row, rest); rest = Trim(rest);
                fv.value = fv.type == "string" ? Unquote(rest) : rest;
                current->cells.push_back(std::move(fv));
            }
            else throw std::runtime_error("Unknown data sheet directive '" + kind + "'.");
        }
        if (doc.type.empty()) throw std::runtime_error("Data sheet is missing its type.");
        return doc;
    }

    SheetDoc Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Data sheet exceeds the 1 MiB limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read data sheet.");
        return Decode(text);
    }

    namespace
    {
        void WriteAtomic(const std::filesystem::path& file, std::string_view bytes)
        {
            std::filesystem::path temp; HANDLE h = INVALID_HANDLE_VALUE;
            for (unsigned i = 0; i < 1000 && h == INVALID_HANDLE_VALUE; ++i)
            {
                temp = file; temp += L".save-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
                h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
                    throw std::runtime_error("Cannot create a temporary save file.");
            }
            if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a temporary save file.");
            DWORD written = 0;
            const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size() && FlushFileBuffers(h);
            CloseHandle(h);
            if (!ok) { DeleteFileW(temp.c_str()); throw std::runtime_error("Cannot write data sheet to disk."); }
            if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { DeleteFileW(temp.c_str()); throw std::runtime_error("Cannot replace the data sheet file."); }
        }
    }

    void Save(const std::filesystem::path& root, const std::filesystem::path& path, const SheetDoc& doc, const SheetDoc* expected)
    {
        const auto file = Resolve(root, path);
        if (expected && std::filesystem::exists(file) && Load(file) != *expected)
            throw std::runtime_error("The data sheet changed on disk. Reload it before saving.");
        WriteAtomic(file, Encode(doc));
    }

    std::filesystem::path Create(const std::filesystem::path& root, const std::string& type, std::string name)
    {
        if (!Identifier(type)) throw std::runtime_error("Pick a data object type.");
        if (name.empty()) name = type + "Sheet";
        if (!Identifier(name)) throw std::runtime_error("Data sheet names must be identifiers.");
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i)
        {
            const auto stem = name + (i ? std::to_string(i) : "");
            const auto path = Resolve(root, std::filesystem::path(stem + ".zsheet"));
            if (std::filesystem::exists(path)) continue;
            SheetDoc doc; doc.type = type;
            WriteAtomic(path, Encode(doc));
            return path;
        }
        throw std::runtime_error("Too many data sheets with this name.");
    }

    std::string Cell(const SheetDoc& doc, const std::vector<std::string>& fieldNames,
                     std::string_view row, std::string_view col, std::string* outType)
    {
        // Resolve the row.
        const Row* r = nullptr;
        if (const auto idx = AsIndex(row)) { if (*idx < doc.rows.size()) r = &doc.rows[*idx]; }
        else for (const auto& candidate : doc.rows) if (candidate.key == row) { r = &candidate; break; }
        if (!r) throw std::runtime_error("data sheet has no row '" + std::string(row) + "'.");
        // Resolve the column name.
        std::string field;
        if (const auto idx = AsIndex(col)) { if (*idx < fieldNames.size()) field = fieldNames[*idx]; }
        else if (std::find(fieldNames.begin(), fieldNames.end(), std::string(col)) != fieldNames.end()) field = col;
        if (field.empty()) throw std::runtime_error("data sheet has no column '" + std::string(col) + "'.");
        // Find the cell (or fall back to the struct's default for that field).
        for (const auto& c : r->cells) if (c.name == field) { if (outType) *outType = c.type; return c.value; }
        if (outType) *outType = "";
        return "";
    }
}
