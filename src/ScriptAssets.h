#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zengine::scripts
{
    constexpr std::size_t MaxSourceBytes = 256 * 1024;
    bool IsScript(const std::filesystem::path& path);
    // All editor writes must resolve inside this project's Assets directory.
    std::filesystem::path Resolve(const std::filesystem::path& root, const std::filesystem::path& path);
    std::string Load(const std::filesystem::path& path);
    void Save(const std::filesystem::path& root, const std::filesystem::path& path,
              std::string_view source, const std::string* expected = nullptr);
    std::filesystem::path Create(const std::filesystem::path& root, std::string name = "NewBehavior");
    enum class TokenKind { Keyword, Type, Number, String, Comment };
    struct Span { std::size_t start, length; TokenKind kind; };
    struct Diagnostic { std::size_t start, length, line, column; std::string message; };
    struct Analysis { std::vector<Span> spans; std::vector<Diagnostic> errors; };
    Analysis Analyze(std::wstring_view source);
}
