#include "ScriptCompletion.h"
#include "ScriptTyping.h"
#include "ScriptAssets.h"
#include "zscript/NativeTypes.h"
#include <algorithm>
#include <cwctype>
#include <functional>

namespace scriptCompletion {
namespace {
// Native type names are ASCII; widen them for the wide-string completion index.
std::wstring Wide(std::string_view text) { return std::wstring(text.begin(), text.end()); }
struct Token { std::wstring text; std::size_t pos; };
bool Word(wchar_t c){return std::iswalnum(c)||c==L'_';}
std::vector<Token> Tokens(const std::wstring& source) {
    const auto code=scriptTyping::Code(source);std::vector<Token> tokens;
    for(std::size_t i=0;i<code.size();) {
        if(std::iswspace(code[i])){++i;continue;}const auto start=i++;
        if(Word(code[start]))while(i<code.size()&&Word(code[i]))++i;
        tokens.push_back({code.substr(start,i-start),start});
    }
    return tokens;
}
std::size_t Closing(const std::vector<Token>& t,std::size_t begin,const wchar_t* open,const wchar_t* close) {
    int depth=0;for(auto i=begin;i<t.size();++i){if(t[i].text==open)++depth;else if(t[i].text==close&&!--depth)return i;}return t.size();
}
struct ClassRange { std::wstring name;std::size_t begin,end; };
std::vector<ClassRange> Classes(const std::vector<Token>& t) {
    std::vector<ClassRange> result;
    for(std::size_t i=0;i+2<t.size();++i)if(t[i].text==L"class") {
        auto b=i+2;while(b<t.size() && b<i+5 && t[b].text!=L"{")++b;
        if(b<t.size() && t[b].text==L"{")result.push_back({t[i+1].text,b,Closing(t,b,L"{",L"}")});
    }
    return result;
}
}
Index::Index() {
    auto add=[&](const wchar_t* type,const wchar_t* name,const wchar_t* result,bool function=false){types_[type].members[name]={name,result,function};};
    auto sig=[&](const wchar_t* type,const wchar_t* name,const wchar_t* signature){ if(auto t=types_.find(type);t!=types_.end()){ if(auto m=t->second.members.find(name);m!=t->second.members.end())m->second.signature=signature; } };
    for(const auto type:{L"char",L"int",L"float",L"bool",L"string",L"void",L"array"})types_[type];
    // Native types (Vector3, prefab, gameObject, Transform, physics bodies, ...) come from the shared registry.
    for(const auto& native:zengine::script::NativeTypes())types_[Wide(native.name)];
    types_[L"GameObject"].base=L"gameObject";
    for(const auto& native:zengine::script::NativeTypes())if(!native.base.empty())types_[Wide(native.name)].base=Wide(native.base);
    add(L"gameObject",L"transform",L"Transform");
    add(L"gameObject",L"parent",L"gameObject");add(L"gameObject",L"find",L"gameObject",true);add(L"gameObject",L"make_timer",L"Timer",true);add(L"gameObject",L"getBehavior",L"Behavior",true);
    add(L"gameObject",L"find_by_type",L"gameObject",true);add(L"gameObject",L"get_tags",L"array",true);add(L"gameObject",L"has_tag",L"bool",true);
    for(const auto& native:zengine::script::NativeTypes())
        if(!native.accessor.empty() && native.accessor!=std::string_view("transform"))
            types_[L"gameObject"].members[Wide(native.accessor)]={Wide(native.accessor),Wide(native.name),false};
    for(const auto name:{L"position",L"rotation",L"scale"})add(L"Transform",name,L"Vector3");
    for(const auto name:{L"global_position",L"global_rotation",L"global_scale"})add(L"Transform",name,L"Vector3");
    for(const auto name:{L"forward",L"up",L"right"})add(L"Transform",name,L"Vector3");
    add(L"string",L"size",L"int",true);add(L"string",L"truncate",L"string",true);add(L"string",L"substr",L"string",true);
    for(const auto name:{L"was_moved",L"was_rotated",L"was_scaled"})add(L"Transform",name,L"signal");
    for(const auto name:{L"x",L"y",L"z"})add(L"Vector3",name,L"float");
    for(const auto name:{L"x",L"y"})add(L"Vector2",name,L"float");
    add(L"array",L"append",L"void",true);add(L"array",L"erase",L"void",true);add(L"array",L"size",L"int",true);
    add(L"prefab",L"spawn",L"gameObject",true);
    for(const auto name:{L"connect",L"disconnect",L"emit"})add(L"signal",name,L"void",true);
    add(L"signal",L"is_connected",L"bool",true);
    types_[L"Timer"].base=L"gameObject";add(L"Timer",L"finished",L"signal");
    for(const auto name:{L"is_action_pressed",L"is_action_just_pressed",L"is_action_just_released"})add(L"Input",name,L"bool",true);
    add(L"Input",L"action",L"InputAction",true);add(L"Input",L"get_axis",L"float",true);add(L"Input",L"get_vector",L"Vector2",true);
    for(const auto name:{L"just_pressed",L"just_released",L"is_pressed",L"was_just_pressed",L"was_just_released"})add(L"InputAction",name,L"signal");
    add(L"InputAction",L"pressed",L"bool");add(L"InputAction",L"axis",L"Vector2");add(L"InputAction",L"value",L"Vector2");
    add(L"Input",L"mouse",L"Mouse");
    add(L"Mouse",L"delta",L"Vector2");add(L"Mouse",L"position",L"Vector2");
    for(const auto name:{L"clicked",L"click_ended",L"held",L"was_just_moved"})add(L"Mouse",name,L"signal");
    add(L"Physics",L"cast",L"gameObject",true);add(L"Physics",L"cast_all",L"array",true);
    for(const auto name:{L"lerp",L"sin",L"cos",L"tan",L"sqrt",L"exp",L"round"})add(L"Mathf",name,L"float",true);
    add(L"Mathf",L"dot",L"float",true);add(L"Mathf",L"cross",L"Vector3",true);
    add(L"Mathf",L"clamp",L"float",true);add(L"Mathf",L"random",L"float",true);add(L"Mathf",L"random_range",L"float",true);add(L"Mathf",L"random_int",L"int",true);
    add(L"PhysicsBody",L"velocity",L"Vector3");add(L"PhysicsBody",L"angular_velocity",L"Vector3");
    for(const auto name:{L"add_force",L"add_impulse",L"add_torque",L"add_angular_impulse"})add(L"PhysicsBody",name,L"void",true);
    for(const auto name:{L"collision_entered",L"collision_stayed",L"collision_exited",L"area_entered",L"area_stayed",L"area_exited"})add(L"PhysicsBody",name,L"signal");
    // Accept the lower-case spelling of every native type, matching the compiler's Canonical().
    // Hover signatures for the built-ins that take parameters.
    sig(L"gameObject",L"find",L"string name");sig(L"gameObject",L"make_timer",L"float seconds");sig(L"gameObject",L"print",L"string text");
    sig(L"gameObject",L"getBehavior",L"type component");sig(L"gameObject",L"find_by_type",L"type component");sig(L"gameObject",L"has_tag",L"string tag");
    sig(L"string",L"truncate",L"int length");sig(L"string",L"substr",L"int start, int count");
    sig(L"array",L"append",L"value item");sig(L"array",L"erase",L"int index");
    sig(L"signal",L"connect",L"function callback");sig(L"signal",L"disconnect",L"function callback");sig(L"signal",L"is_connected",L"function callback");sig(L"signal",L"emit",L"...args");
    for(const auto name:{L"is_action_pressed",L"is_action_just_pressed",L"is_action_just_released",L"action",L"get_axis",L"get_vector"})sig(L"Input",name,L"string action");
    for(const auto name:{L"sin",L"cos",L"tan",L"sqrt",L"exp",L"round"})sig(L"Mathf",name,L"float value");
    sig(L"Mathf",L"lerp",L"float from, float to, float weight");sig(L"Mathf",L"dot",L"Vector3 a, Vector3 b");sig(L"Mathf",L"cross",L"Vector3 a, Vector3 b");
    sig(L"Mathf",L"clamp",L"float value, float min, float max");sig(L"Mathf",L"random",L"");sig(L"Mathf",L"random_range",L"float min, float max");sig(L"Mathf",L"random_int",L"float min, float max");
    sig(L"Physics",L"cast",L"Vector3 from, Vector3 to, int layer_mask");sig(L"Physics",L"cast_all",L"Vector3 from, Vector3 to, int layer_mask");
    for(const auto name:{L"add_force",L"add_impulse",L"add_torque",L"add_angular_impulse"})sig(L"PhysicsBody",name,L"Vector3 amount");
    for(const auto name:{L"was_moved",L"was_rotated",L"was_scaled"})sig(L"Transform",name,L"Vector3 value");
    for(const auto name:{L"collision_entered",L"collision_stayed",L"collision_exited",L"area_entered",L"area_stayed",L"area_exited"})sig(L"PhysicsBody",name,L"gameObject other");
    for(const auto name:{L"clicked",L"click_ended",L"held"})sig(L"Mouse",name,L"int button");
    sig(L"Mouse",L"was_just_moved",L"Vector2 old_position, Vector2 new_position");
    sig(L"Timer",L"finished",L"");
    for(const auto& native:zengine::script::NativeTypes()) {
        std::wstring lower=Wide(native.name);
        std::transform(lower.begin(),lower.end(),lower.begin(),[](wchar_t c){return std::towlower(c);});
        if(auto it=types_.find(Wide(native.name)); it!=types_.end() && lower!=it->first) types_[lower]=it->second;
    }
}
void Index::AddString(std::wstring value) {
    if(value.empty() || value.size()>128 || strings_.size()>=4096)return;
    std::wstring escaped;for(auto c:value){if(c==L'"'||c==L'\\')escaped+=L'\\';if(c==L'\r'||c==L'\n')return;escaped+=c;}strings_.insert(std::move(escaped));
}
void Index::AddSource(const std::wstring& source) {
    const auto t=Tokens(source);
    for(const auto& c:Classes(t)) {
        auto& type=types_[c.name];
        if(c.begin>=2 && t[c.begin-2].text==L":")type.base=t[c.begin-1].text;
        for(auto i=c.begin+1;i<c.end && i<t.size();) {
            if(t[i].text==L"{"){i=Closing(t,i,L"{",L"}");if(i<t.size())++i;continue;}
            // ZE-79: consume any member modifiers so `func` / fields still parse, and remember them.
            bool modStatic=false,modPrivate=false;
            while(i<c.end && i<t.size() && (t[i].text==L"static"||t[i].text==L"private"||t[i].text==L"abstract"||t[i].text==L"override")) {
                if(t[i].text==L"static")modStatic=true; if(t[i].text==L"private")modPrivate=true; ++i;
            }
            if(i+2<t.size() && t[i].text==L"func") {
                const auto close=Closing(t,i+2,L"(",L")");std::wstring result=L"void";
                if(close+2<t.size() && t[close+1].text==L":")result=t[close+2].text;
                std::wstring signature; // "type name, type name" reconstructed from the token run
                for(auto j=i+3;j+1<close && j+1<t.size();j+=2) {
                    if(!signature.empty())signature+=L", ";
                    signature+=t[j].text; if(j+1<close)signature+=L' '+t[j+1].text;
                }
                const bool priv=modPrivate || (!t[i+1].text.empty() && t[i+1].text[0]==L'_');
                type.members[t[i+1].text]={t[i+1].text,result,true,signature,priv,modStatic};
                i=std::min(close+1,t.size());continue;
            }
            if(i+2<t.size() && t[i].text==L"signal")type.members[t[i+1].text]={t[i+1].text,L"signal"};
            else if(i+2<t.size() && Word(t[i].text[0]) && Word(t[i+1].text[0]) && (t[i+2].text==L"="||t[i+2].text==L";")) {
                const bool priv=modPrivate || (!t[i+1].text.empty() && t[i+1].text[0]==L'_');
                type.members[t[i+1].text]={t[i+1].text,t[i].text,false,{},priv,modStatic};
            }
            ++i;
        }
    }
    for(const auto& span:zengine::scripts::Analyze(source).spans)
        if(span.kind==zengine::scripts::TokenKind::String && span.length>=2 && source[span.start+span.length-1]==L'"' && span.length<=130 && strings_.size()<4096)
            strings_.insert(source.substr(span.start+1,span.length-2));
}
Result Index::Complete(const std::wstring& source,std::size_t caret) const {
    Result result;result.start=caret;if(caret>source.size())return result;
    Index index=*this;index.AddSource(source);
    std::map<std::wstring,Item> candidates;
    const auto analysis=zengine::scripts::Analyze(source.substr(0,caret));
    for(const auto& span:analysis.spans)if(span.start+span.length==caret && span.length &&
        (span.kind==zengine::scripts::TokenKind::Comment || span.kind==zengine::scripts::TokenKind::String)) {
        if(span.kind==zengine::scripts::TokenKind::Comment || source[span.start]==L'\'' || (span.length>1 && source[caret-1]==L'"'))return result;
        result.start=span.start+1;result.prefix=source.substr(result.start,caret-result.start);
        for(const auto& s:index.strings_)if(s.starts_with(result.prefix) && s!=result.prefix)result.items.push_back({s,L"project value"});
        return result;
    }
    while(result.start && Word(source[result.start-1]))--result.start;
    result.prefix=source.substr(result.start,caret-result.start);
    const auto t=Tokens(source);std::wstring currentClass;
    std::size_t classBegin=0,classEnd=t.size();
    for(const auto& c:Classes(t))if(t[c.begin].pos<caret && (c.end==t.size()||t[c.end].pos>=caret)){currentClass=c.name;classBegin=c.begin;classEnd=c.end;break;}
    auto members=[&](std::wstring type){std::map<std::wstring,Item> found;std::set<std::wstring> seen;
        while(index.types_.contains(type)&&seen.insert(type).second){const auto& c=index.types_.at(type);found.insert(c.members.begin(),c.members.end());type=c.base;}return found;};
    auto variables=members(currentClass);variables[L"this"]={L"this",currentClass};variables[L"Input"]={L"Input",L"Input"};variables[L"Physics"]={L"Physics",L"Physics"};variables[L"Mathf"]={L"Mathf",L"Mathf"};
    // Only parameters/locals in the current method's live lexical scopes are visible.
    for(auto i=classBegin;i<classEnd && i+2<t.size();++i)if(t[i].text==L"func") {
        const auto close=Closing(t,i+2,L"(",L")");auto body=close+1;
        while(body<t.size() && t[body].text!=L"{" && body<close+4)++body;
        if(body>=t.size() || t[body].text!=L"{")continue;
        const auto end=Closing(t,body,L"{",L"}");
        if(t[body].pos>=caret || (end<t.size() && t[end].pos<caret))continue;
        for(auto j=i+3;j+1<close;++j)if(index.types_.contains(t[j].text))variables[t[j+1].text]={t[j+1].text,t[j].text};
        std::vector<std::map<std::wstring,Item>> scopes(1);
        for(auto j=body+1;j<t.size() && t[j].pos<result.start;++j) {
            if(t[j].text==L"{")scopes.emplace_back();
            else if(t[j].text==L"}" && scopes.size()>1)scopes.pop_back();
            else if(j+2<t.size() && index.types_.contains(t[j].text) && Word(t[j+1].text[0]) && (t[j+2].text==L"="||t[j+2].text==L";"))scopes.back()[t[j+1].text]={t[j+1].text,t[j].text};
        }
        for(const auto& scope:scopes)for(const auto& [name,item]:scope)variables[name]=item;
        break;
    }
    auto before=result.start;while(before && std::iswspace(source[before-1]))--before;
    result.members=before && source[before-1]==L'.';
    if(result.members) {
        int end=-1;for(std::size_t i=0;i<t.size() && t[i].pos<before-1;++i)end=static_cast<int>(i);
        std::function<std::wstring(int,int)> resolve=[&](int last,int depth)->std::wstring {
            if(last<0 || depth>32)return {};
            if(t[last].text==L")") {int count=1;--last;while(last>=0){if(t[last].text==L")")++count;else if(t[last].text==L"("&&!--count)break;--last;}return resolve(last-1,depth+1);}
            if(last>=2 && t[last-1].text==L"."){auto m=members(resolve(last-2,depth+1));auto it=m.find(t[last].text);return it==m.end()?L"":it->second.type;}
            if(auto it=variables.find(t[last].text);it!=variables.end())return it->second.type;
            return index.types_.contains(t[last].text)?t[last].text:L"";
        };
        const auto receiverType=resolve(end,0);
        const bool selfReceiver=end>=0 && (t[end].text==L"this" || t[end].text==receiverType);
        candidates=members(receiverType);
        // ZE-79: a private member is only offered when completing inside its own class.
        if(receiverType!=currentClass && !selfReceiver)
            for(auto it=candidates.begin();it!=candidates.end();) it->second.isPrivate ? it=candidates.erase(it) : ++it;
    } else {
        if(result.prefix.empty())return result;
        candidates=variables;
        for(const auto& [name,type]:index.types_)if(name!=L"signal" && name!=L"InputAction" && name!=L"Mouse")candidates[name]={name,L"type"};
        for(const auto word:{L"class",L"func",L"return",L"if",L"else",L"while",L"for",L"true",L"false",L"null",L"export",L"multiline",L"label",L"signal",L"is",L"not",L"and",L"or",L"nor",L"abstract",L"override",L"private",L"static",L"super"})candidates[word]={word,L"keyword"};
        auto preceding=source.substr(0,result.start);const auto last=preceding.find_last_not_of(L" \t\r\n");
        if(last!=preceding.npos && last>=3 && preceding.substr(last-3,4)==L"func") {
            candidates.clear();for(const auto name:{L"start",L"update",L"physicsUpdate",L"draw"})candidates[name]={name,L"lifecycle",true};
            for(const auto& [name,item]:members(currentClass))if(item.function)candidates[name]=item;
        }
    }
    for(const auto& [name,item]:candidates)if(name.starts_with(result.prefix) && (result.members||name!=result.prefix)) {
        result.items.push_back(item);if(result.items.size()==128)break;
    }
    return result;
}
std::wstring Index::Hover(const std::wstring& source,std::size_t caret) const {
    if(caret>source.size())return {};
    Index index=*this;index.AddSource(source);
    const auto t=Tokens(source);
    int k=-1;
    for(std::size_t i=0;i<t.size();++i)if(Word(t[i].text[0]) && t[i].pos<=caret && caret<=t[i].pos+t[i].text.size()){k=static_cast<int>(i);break;}
    if(k<0)return {};
    const auto here=t[k].pos;
    std::wstring currentClass;
    for(const auto& c:Classes(t))if(t[c.begin].pos<here && (c.end==t.size()||t[c.end].pos>=here)){currentClass=c.name;break;}
    auto members=[&](std::wstring type){std::map<std::wstring,Item> found;std::set<std::wstring> seen;
        while(index.types_.contains(type)&&seen.insert(type).second){const auto& c=index.types_.at(type);found.insert(c.members.begin(),c.members.end());type=c.base;}return found;};
    std::map<std::wstring,Item> variables=members(currentClass);
    variables[L"this"]={L"this",currentClass};variables[L"Input"]={L"Input",L"Input"};variables[L"Physics"]={L"Physics",L"Physics"};variables[L"Mathf"]={L"Mathf",L"Mathf"};
    for(std::size_t i=0;i+2<t.size();++i)if(t[i].text==L"func") {
        const auto close=Closing(t,i+2,L"(",L")");
        auto body=close+1;while(body<t.size() && t[body].text!=L"{" && body<close+4)++body;
        if(body>=t.size()||t[body].text!=L"{")continue;
        const auto end=Closing(t,body,L"{",L"}");
        if(t[body].pos>=here || (end<t.size() && t[end].pos<here))continue;
        for(auto j=i+3;j+1<close && j<t.size();++j)if(index.types_.contains(t[j].text))variables[t[j+1].text]={t[j+1].text,t[j].text};
        for(auto j=body+1;j<t.size() && t[j].pos<here;++j)
            if(j+2<t.size() && index.types_.contains(t[j].text) && Word(t[j+1].text[0]) && (t[j+2].text==L"="||t[j+2].text==L";"))
                variables[t[j+1].text]={t[j+1].text,t[j].text};
        break;
    }
    std::function<std::wstring(int,int)> resolve=[&](int last,int depth)->std::wstring {
        if(last<0||depth>32)return {};
        if(t[last].text==L")"){int c=1;--last;while(last>=0){if(t[last].text==L")")++c;else if(t[last].text==L"("&&!--c)break;--last;}return resolve(last-1,depth+1);}
        if(last>=2 && t[last-1].text==L"."){auto m=members(resolve(last-2,depth+1));auto it=m.find(t[last].text);return it==m.end()?std::wstring{}:it->second.type;}
        if(auto it=variables.find(t[last].text);it!=variables.end())return it->second.type;
        return index.types_.contains(t[last].text)?t[last].text:std::wstring{};
    };
    Item found;
    if(k>=2 && t[k-1].text==L"."){auto m=members(resolve(k-2,0));auto it=m.find(t[k].text);if(it==m.end())return {};found=it->second;}
    else {auto v=variables.find(t[k].text);if(v==variables.end())return {};found=v->second;}
    if(!found.function && found.type!=L"signal")return {};
    std::wstring tip=found.name+L"("+found.signature+L")";
    if(found.function && !found.type.empty() && found.type!=L"void")tip+=L" \x2192 "+found.type; // arrow
    if(found.type==L"signal")tip+=L"  (signal)";
    return tip;
}
}
