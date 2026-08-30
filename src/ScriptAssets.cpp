#include "ScriptAssets.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <stdexcept>

namespace zengine::scripts
{
    bool IsScript(const std::filesystem::path& path)
    {
        auto ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        return ext == L".zsh";
    }
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path)
    {
        const auto base = std::filesystem::weakly_canonical(root);
        const auto file = std::filesystem::weakly_canonical(path.is_absolute() ? path : base / path);
        auto b = base.begin(), f = file.begin();
        for (; b != base.end(); ++b, ++f)
            if (f == file.end() || _wcsicmp(b->c_str(), f->c_str()) != 0)
                throw std::runtime_error("Scripts must be inside the current project's Assets directory.");
        if (f == file.end() || !IsScript(file)) throw std::runtime_error("Select a .zsh script asset.");
        return file;
    }
    std::string Load(const std::filesystem::path& path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size > MaxSourceBytes) throw std::runtime_error("Script exceeds the 256 KiB editor limit.");
        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read script.");
        if (text.find('\0') != std::string::npos || (!text.empty() &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)))
            throw std::runtime_error("Scripts must be UTF-8 text without NUL characters.");
        return text;
    }
    namespace
    {
        void Write(HANDLE file, std::string_view source)
        {
            DWORD written = 0;
            if (!WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) ||
                written != source.size() || !FlushFileBuffers(file)) throw std::runtime_error("Cannot write script to disk.");
        }
    }
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              std::string_view source, const std::string* expected)
    {
        const auto file = Resolve(root, path);
        if (source.size() > MaxSourceBytes) throw std::runtime_error("Script exceeds the 256 KiB editor limit.");
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
                throw std::runtime_error("Cannot replace script. Original file was preserved.");
        }
        catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); DeleteFileW(temp.c_str()); throw; }
    }
    std::filesystem::path Create(const std::filesystem::path& root, std::string name)
    {
        const auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (name.empty() || name.size() > 80 || !alpha(name.front()) ||
            !std::all_of(name.begin(), name.end(), [&](unsigned char c) { return alpha(c) || (c >= '0' && c <= '9'); }))
            throw std::runtime_error("Script names must be identifiers: letters, digits, underscores; not starting with a digit.");
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i)
        {
            const auto type = name + (i ? std::to_string(i) : "");
            const auto path = Resolve(root, std::filesystem::path(type + ".zsh"));
            const auto source = "class " + type + " : gameObject\r\n{\r\n    func start()\r\n    {\r\n    }\r\n\r\n"
                "    func update(float delta)\r\n    {\r\n    }\r\n\r\n    func draw()\r\n    {\r\n    }\r\n}\r\n";
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                throw std::runtime_error("Cannot create script asset.");
            }
            try { Write(file, source); CloseHandle(file); }
            catch (...) { CloseHandle(file); DeleteFileW(path.c_str()); throw; }
            return path;
        }
        throw std::runtime_error("Too many scripts with this name.");
    }
    Analysis Analyze(std::wstring_view source)
    {
        Analysis out;
        struct Bracket { wchar_t c; std::size_t pos; };
        std::vector<Bracket> brackets;
        auto error = [&](std::size_t pos, std::size_t length, const char* message)
        {
            if (out.errors.size() >= 100) return;
            std::size_t line = 1, column = 1;
            for (std::size_t j = 0; j < pos; ++j) { if (source[j] == L'\n' || (source[j] == L'\r' && (j + 1 == source.size() || source[j+1] != L'\n'))) { ++line; column = 1; } else ++column; }
            out.errors.push_back({pos, length, line, column, message});
        };
        for (std::size_t p = 0; p < source.size();)
        {
            const auto start = p;
            const wchar_t c = source[p++];
            if (std::iswspace(c) || (start == 0 && c == 0xfeff)) continue;
            if (c == L'/' && p < source.size() && (source[p] == L'/' || source[p] == L'*'))
            {
                const bool block = source[p++] == L'*';
                if (block)
                {
                    while (p + 1 < source.size() && !(source[p] == L'*' && source[p+1] == L'/')) ++p;
                    if (p + 1 >= source.size()) { p = source.size(); error(start, p-start, "Unterminated block comment."); }
                    else p += 2;
                }
                else while (p < source.size() && source[p] != L'\r' && source[p] != L'\n') ++p;
                out.spans.push_back({start, p-start, TokenKind::Comment}); continue;
            }
            if (c == L'"')
            {
                bool closed = false;
                while (p < source.size() && source[p] != L'\r' && source[p] != L'\n')
                {
                    if (source[p] == L'"') { ++p; closed = true; break; }
                    if (source[p] == L'\\' && p+1 < source.size() && source[p+1] != L'\r' && source[p+1] != L'\n') ++p;
                    ++p;
                }
                out.spans.push_back({start, p-start, TokenKind::String});
                if (!closed) error(start, p-start, "Unterminated string.");
                continue;
            }
            if (std::iswalpha(c) || c == L'_')
            {
                while (p < source.size() && (std::iswalnum(source[p]) || source[p] == L'_')) ++p;
                const auto word = source.substr(start, p-start);
                if (word == L"class" || word == L"func" || word == L"return" || word == L"if" || word == L"else" ||
                    word == L"while" || word == L"true" || word == L"false" || word == L"null" || word == L"this")
                    out.spans.push_back({start,p-start,TokenKind::Keyword});
                else if (word == L"int" || word == L"float" || word == L"bool" || word == L"string" || word == L"void" || word == L"gameObject")
                    out.spans.push_back({start,p-start,TokenKind::Type});
                continue;
            }
            if (c >= L'0' && c <= L'9')
            {
                while (p < source.size() && (std::iswdigit(source[p]) || source[p] == L'.')) ++p;
                out.spans.push_back({start,p-start,TokenKind::Number}); continue;
            }
            if (c == L'{' || c == L'(' || c == L'[') brackets.push_back({c,start});
            else if (c == L'}' || c == L')' || c == L']')
            {
                const wchar_t open = c == L'}' ? L'{' : c == L')' ? L'(' : L'[';
                if (brackets.empty() || brackets.back().c != open) error(start, 1, "Unexpected closing bracket.");
                else brackets.pop_back();
            }
            else if (std::wstring_view(L";:.,+-*/%=!<>&|").find(c) == std::wstring_view::npos)
                error(start, 1, "Unexpected character.");
        }
        for (const auto& bracket : brackets) error(bracket.pos, 1, "Unclosed bracket.");
        return out;
    }
}
