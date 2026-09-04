#include "DataAssets.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace zengine::dataobj
{
    namespace
    {
        std::string Trim(std::string s)
        {
            const auto notspace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
            return s;
        }
        bool Identifier(const std::string& s)
        {
            const auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            return !s.empty() && s.size() <= 96 && alpha(static_cast<unsigned char>(s.front())) &&
                std::all_of(s.begin(), s.end(), [&](unsigned char c) { return alpha(c) || (c >= '0' && c <= '9'); });
        }
        std::string QuoteString(std::string_view s)
        {
            std::string out = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (c == '\n') out += "\\n";
                else if (c == '\r') {}
                else if (c == '\t') out += "\\t";
                else out += c;
            }
            return out + '"';
        }
        std::string UnquoteString(std::string_view s)
        {
            std::string t = Trim(std::string(s));
            if (t.size() < 2 || t.front() != '"' || t.back() != '"') throw std::runtime_error("Expected a quoted string value.");
            std::string out;
            for (std::size_t i = 1; i + 1 < t.size(); ++i)
            {
                if (t[i] == '\\' && i + 2 < t.size())
                {
                    const char n = t[++i];
                    out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                }
                else out += t[i];
            }
            return out;
        }
    }

    bool IsStorableType(std::string_view t)
    {
        return t == "int" || t == "float" || t == "bool" || t == "string" || t == "Vector3" || t == "Vector2";
    }

    bool IsData(const std::filesystem::path& path)
    {
        auto ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        return ext == L".zdata";
    }
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Data objects must be inside the current project's Assets directory.");
        if (f == file.end() || !IsData(file)) throw std::runtime_error("Select a .zdata data object asset.");
        return file;
    }

    std::string Encode(const DataDoc& doc)
    {
        if (!Identifier(doc.type)) throw std::runtime_error("A data object needs a struct type name.");
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "ZDATA 1\r\n";
        out << "type " << doc.type << "\r\n";
        for (const auto& f : doc.fields)
        {
            if (!Identifier(f.name) || !IsStorableType(f.type)) continue;
            out << "field " << f.name << ' ' << f.type << ' ';
            if (f.type == "string") out << QuoteString(f.value);
            else out << (f.value.empty() ? (f.type == "bool" ? "false" : f.type == "Vector3" ? "0, 0, 0" : f.type == "Vector2" ? "0, 0" : "0") : f.value);
            out << "\r\n";
        }
        return out.str();
    }

    DataDoc Decode(std::string_view text)
    {
        std::istringstream in{std::string(text)};
        in.imbue(std::locale::classic());
        std::string line;
        if (!std::getline(in, line) || Trim(line).substr(0, 8) != "ZDATA 1") throw std::runtime_error("Not a ZDATA 1 file.");
        DataDoc doc;
        while (std::getline(in, line))
        {
            const auto trimmed = Trim(line);
            if (trimmed.empty()) continue;
            std::istringstream row{trimmed};
            std::string kind; row >> kind;
            if (kind == "type") { row >> doc.type; if (!Identifier(doc.type)) throw std::runtime_error("Malformed data object type."); }
            else if (kind == "field")
            {
                FieldValue fv; row >> fv.name >> fv.type;
                if (!Identifier(fv.name) || !IsStorableType(fv.type)) throw std::runtime_error("Malformed data object field.");
                std::string rest; std::getline(row, rest); rest = Trim(rest);
                fv.value = fv.type == "string" ? UnquoteString(rest) : rest;
                doc.fields.push_back(std::move(fv));
            }
            else throw std::runtime_error("Unknown data object directive '" + kind + "'.");
        }
        if (doc.type.empty()) throw std::runtime_error("Data object is missing its type.");
        return doc;
    }

    DataDoc Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Data object exceeds the 256 KiB limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read data object.");
        return Decode(text);
    }

    namespace
    {
        void WriteAtomic(const std::filesystem::path& file, std::string_view bytes)
        {
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
            DWORD written = 0;
            const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size() && FlushFileBuffers(handle);
            CloseHandle(handle);
            if (!ok) { DeleteFileW(temp.c_str()); throw std::runtime_error("Cannot write data object to disk."); }
            if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { DeleteFileW(temp.c_str()); throw std::runtime_error("Cannot replace the data object file."); }
        }
    }

    void Save(const std::filesystem::path& root, const std::filesystem::path& path, const DataDoc& doc, const DataDoc* expected)
    {
        const auto file = Resolve(root, path);
        if (expected && std::filesystem::exists(file) && Load(file) != *expected)
            throw std::runtime_error("The data object changed on disk. Reload it before saving.");
        WriteAtomic(file, Encode(doc));
    }

    std::filesystem::path Create(const std::filesystem::path& root, const std::string& type, std::string name)
    {
        if (!Identifier(type)) throw std::runtime_error("Pick a data object type.");
        if (name.empty()) name = type;
        if (!Identifier(name)) throw std::runtime_error("Data object names must be identifiers.");
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i)
        {
            const auto stem = name + (i ? std::to_string(i) : "");
            const auto path = Resolve(root, std::filesystem::path(stem + ".zdata"));
            if (std::filesystem::exists(path)) continue;
            DataDoc doc; doc.type = type;
            WriteAtomic(path, Encode(doc));
            return path;
        }
        throw std::runtime_error("Too many data objects with this name.");
    }
}
