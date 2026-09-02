#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scriptCompletion {
// signature: the parameter list shown in hover tooltips, e.g. L"float delta" (empty = no params).
struct Item { std::wstring name, type; bool function=false; std::wstring signature; };
struct Type { std::wstring base; std::map<std::wstring,Item> members; };
struct Result { std::size_t start=0; std::wstring prefix; bool members=false; std::vector<Item> items; };
// Tolerant source index: completion works while a declaration is incomplete.
// It never executes source and is deliberately separate from the VM/compiler.
class Index {
public:
    Index();
    void AddSource(const std::wstring& source);
    void AddString(std::wstring value);
    Result Complete(const std::wstring& source,std::size_t caret) const;
    // Tooltip for the identifier under `caret`: "name(params) -> result" for a function
    // or "name(params)" for a signal; empty when there is nothing useful to show.
    std::wstring Hover(const std::wstring& source,std::size_t caret) const;
private:
    std::map<std::wstring,Type> types_;
    std::set<std::wstring> strings_;
};
}
