#pragma once
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>

namespace scriptTyping {
struct Edit { std::size_t start, end; std::wstring text; std::size_t caret; };
// Keep indices identical to RichEdit's UTF-16 text, masking comments and strings.
inline std::wstring Code(std::wstring_view source) {
    std::wstring code(source);
    enum { Normal, String, Line, Block } state = Normal;
    wchar_t quote=L'"';
    for (std::size_t i=0;i<source.size();++i) {
        const wchar_t c=source[i], next=i+1<source.size()?source[i+1]:L'\0';
        if (state==Normal) {
            if(c==L'"' || c==L'\'') {state=String;quote=c;}
            else if(c==L'/' && (next==L'/' || next==L'*')) { state=next==L'/'?Line:Block; code[i++]=L' '; }
            else continue;
        } else if(state==String && c==L'\\' && next) { code[i++]=L' '; code[i]=L' '; continue; }
        else if(state==String && c==quote) state=Normal;
        else if(state==Line && (c==L'\r'||c==L'\n')) state=Normal;
        else if(state==Block && c==L'*' && next==L'/') {code[i++]=L' ';state=Normal;}
        if(code[i]!=L'\r' && code[i]!=L'\n')code[i]=L' ';
    }
    return code;
}
inline std::optional<Edit> OnCharacter(std::wstring_view source, std::size_t start, std::size_t end, wchar_t character) {
    if(start>source.size() || end>source.size() || start>end)return {};
    const auto code=Code(source);
    int depth=0;
    for(std::size_t i=0;i<start;++i) {if(code[i]==L'{')++depth;else if(code[i]==L'}')depth=std::max(0,depth-1);}
    const auto indent=[&](int level){return std::wstring(static_cast<std::size_t>(std::clamp(level,0,128))*4,L' ');};
    if(character==L'\r') {
        std::size_t next=end;while(next<source.size() && (source[next]==L' ' || source[next]==L'\t'))++next;
        auto text=L"\r"+indent(depth);const auto caret=text.size();
        // Splitting an empty brace pair keeps its closing brace aligned.
        std::size_t previous=start;while(previous && std::iswspace(code[previous-1]))--previous;
        if(previous && code[previous-1]==L'{' && next<code.size() && code[next]==L'}') {
            text+=L"\r"+indent(depth-1);end=next;
        } else if(next<code.size() && code[next]==L'}') {text=L"\r"+indent(depth-1);end=next;return Edit{start,end,text,text.size()};}
        return Edit{start,end,text,caret};
    }
    if(character==L'}') {
        auto line=source.find_last_of(L"\r\n",start?start-1:0);line=line==source.npos?0:line+1;
        if(line<=start && source.substr(line,start-line).find_first_not_of(L" \t")==source.npos) {
            auto text=indent(depth-1)+L"}";return Edit{line,end,text,text.size()};
        }
    }
    if(character!=L')')return {};
    // Find the unmatched opening parenthesis, then require `func identifier`.
    std::size_t open=start;int parens=1;
    while(open) {--open;if(code[open]==L')')++parens;else if(code[open]==L'(' && --parens==0)break;}
    if(parens)return {};
    auto p=open;while(p && std::iswspace(code[p-1]))--p;
    const auto nameEnd=p;while(p && (std::iswalnum(code[p-1])||code[p-1]==L'_'))--p;
    if(p==nameEnd)return {};
    while(p && std::iswspace(code[p-1]))--p;
    if(p<4 || code.substr(p-4,4)!=L"func" || (p>4 && (std::iswalnum(code[p-5])||code[p-5]==L'_')))return {};
    // A caret inside a comment or string must not trigger completion.
    auto probe=std::wstring(source);probe.insert(start,1,L')');if(Code(probe)[start]!=L')')return {};
    auto next=end;while(next<code.size() && std::iswspace(code[next]))++next;
    if(next<code.size() && (code[next]==L'{' || code[next]==L':' || code[next]==L')'))return {};
    auto text=L")\r"+indent(depth)+L"{\r"+indent(depth+1);const auto caret=text.size();
    text+=L"\r"+indent(depth)+L"}";return Edit{start,end,text,caret};
}
}
