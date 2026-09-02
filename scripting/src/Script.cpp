#include "zscript/Script.h"
#include "zscript/Text.h"
#include "zscript/NativeTypes.h"
#include "WorldTransform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <type_traits>
#include <set>
#include <utility>

namespace zengine::script {
namespace {
struct Token {
    enum Kind { Identifier, Number, String, Character, Symbol, End } kind = End;
    std::string text;
    std::size_t line = 1, column = 1;
};
[[noreturn]] void Fail(const std::string& source, const Token& token, const std::string& message) {
    throw ScriptError({source, token.line, token.column, message});
}
bool Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
std::string Canonical(std::string name) {
    // Lower-case spelling -> canonical native type name, derived once from the phone book.
    static const std::map<std::string,std::string> aliases = [] {
        std::map<std::string,std::string> map;
        for (const auto& type : NativeTypes()) {
            std::string lower(type.name);
            std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            map.emplace(std::move(lower),std::string(type.name));
        }
        return map;
    }();
    if (name == "GameObject") return "gameObject";
    auto lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(const auto it=aliases.find(lower);it!=aliases.end())return it->second;
    return name;
}
bool Reserved(std::string_view s) {
    static const std::set<std::string_view> words = {"class", "func", "return", "if", "else", "while", "for", "true", "false", "null", "this", "int", "float", "bool", "string", "void", "gameObject", "GameObject", "Vector3", "Vector2", "Transform", "export", "label"};
    return s == "char" || s == "multiline" || s == "signal" || s == "Input" || s == "Physics" || s == "array" || s == "prefab" || s == "is" || s == "not" || s == "and" || s == "or" || s == "nor" || words.contains(s);
}
std::vector<Token> Lex(std::string_view s, const std::string& source) {
    if (s.size() > 1024 * 1024) Fail(source, {}, "Source exceeds 1 MiB limit");
    std::vector<Token> tokens;
    std::size_t p = 0, line = 1, column = 1;
    auto take = [&]() { char c = s[p++]; if (c == '\n') { ++line; column = 1; } else ++column; return c; };
    auto peek = [&](std::size_t offset = 0) { return p + offset < s.size() ? s[p + offset] : '\0'; };
    // Accept a UTF-8 BOM, as emitted by some Windows editors.
    if (s.starts_with("\xEF\xBB\xBF")) p = 3;
    while (p < s.size()) {
        if (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') { take(); continue; }
        Token t{Token::Symbol, {}, line, column};
        if (peek() == '/' && peek(1) == '/') { while (p < s.size() && peek() != '\n') take(); continue; }
        if (peek() == '/' && peek(1) == '*') {
            take(); take();
            while (p < s.size() && !(peek() == '*' && peek(1) == '/')) take();
            if (p == s.size()) Fail(source, t, "Unterminated comment");
            take(); take(); continue;
        }
        if (Alpha(peek())) {
            t.kind = Token::Identifier;
            while (Alpha(peek()) || Digit(peek())) t.text += take();
        } else if (Digit(peek())) {
            t.kind = Token::Number;
            while (Digit(peek())) t.text += take();
            if (peek() == '.' && Digit(peek(1))) { t.text += take(); while (Digit(peek())) t.text += take(); }
            if (peek() == 'e' || peek() == 'E') {
                t.text += take();
                if (peek() == '+' || peek() == '-') t.text += take();
                if (!Digit(peek())) Fail(source, t, "Expected exponent digits");
                while (Digit(peek())) t.text += take();
            }
        } else if (peek() == '"' || peek() == '\'') {
            const char quote=take();t.kind=quote=='"'?Token::String:Token::Character;
            while (p < s.size() && peek() != quote) {
                char c = take();
                if (c == '\n' || c == '\r') Fail(source, t, "Newline in string literal");
                if (c == '\\') {
                    if (p == s.size()) Fail(source, t, "Unterminated string");
                    c = take();
                    switch (c) {
                    case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                    case '\\': case '"': case '\'': break;
                    default: Fail(source, t, "Unknown string escape");
                    }
                }
                t.text += c;
            }
            if (p == s.size()) Fail(source, t, "Unterminated string");
            take();
        } else {
            t.text += take();
            const std::string pair = t.text + peek();
            if (pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" || pair == "&&" || pair == "||" || pair == "+=" || pair == "-=" || pair == "*=" || pair == "/=" || pair == "&=") t.text += take();
            else if (std::string_view("[]{}();:,.=+-*/!<>&").find(t.text[0]) == std::string_view::npos) Fail(source, t, "Unexpected character");
        }
        tokens.push_back(std::move(t));
        if (tokens.size() > 100000) Fail(source, tokens.back(), "Token limit exceeded");
    }
    tokens.push_back({Token::End, {}, line, column});
    return tokens;
}
struct Expr {
    enum Kind { Literal, Name, Member, Call, Unary, Binary, Array, Index, IsType, Interpolate } kind = Literal;
    Token token;
    Value value;
    std::size_t height = 1;
    std::vector<std::unique_ptr<Expr>> children;
};
struct Stmt {
    enum Kind { Block, Variable, Assignment, Expression, Return, If, While } kind = Block;
    Token token;
    std::string type, operation;
    std::vector<std::unique_ptr<Expr>> expressions;
    std::vector<Stmt> children;
};
struct FieldAst { Token name; std::string type; std::unique_ptr<Expr> initializer; };
struct FunctionAst { Token name; std::string result = "void"; std::vector<FieldAst> params; Stmt body; };
struct ClassAst { Token name; std::string base; std::vector<FieldAst> fields; std::vector<FunctionAst> methods; std::vector<InspectorEntry> inspector; std::vector<Token> signals; };
class Parser {
    std::vector<Token> tokens;
    const std::string& source;
    std::size_t pos = 0, depth = 0;
    struct Guard {
        Parser& parser;
        explicit Guard(Parser& p) : parser(p) { if (++parser.depth > 128) Fail(parser.source, parser.Current(), "Syntax nesting limit exceeded"); }
        ~Guard() { --parser.depth; }
    };
    const Token& Current() const { return tokens[pos]; }
    bool Is(std::string_view text) const { return Current().text == text && Current().kind != Token::String && Current().kind != Token::Character; }
    bool Match(std::string_view text) { if (!Is(text)) return false; ++pos; return true; }
    Token Expect(std::string_view text) { Token t = Current(); if (!Match(text)) Fail(source, t, "Expected '" + std::string(text) + "'"); return t; }
    Token Identifier(bool type = false) {
        Token t = Current();
        if (t.kind != Token::Identifier || (!type && Reserved(t.text))) Fail(source, t, "Expected " + std::string(type ? "type" : "identifier"));
        ++pos; if (type) t.text = Canonical(t.text); return t;
    }
    static int Precedence(std::string_view op) {
        if (op == "||" || op == "or" || op == "nor") return 1;
        if (op == "&&" || op == "and") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "is") return 4;
        if (op == "+" || op == "-" || op == "&") return 5;
        if (op == "*" || op == "/") return 6;
        return 0;
    }
    void CheckHeight(Expr& e) const {
        e.height = 1;
        for (const auto& child : e.children) e.height = std::max(e.height, child->height + 1);
        if (e.height > 128) Fail(source, e.token, "Expression nesting limit exceeded");
    }
    std::unique_ptr<Expr> Expression(int min = 1) {
        Guard guard(*this);
        auto e = std::make_unique<Expr>(); e->token = Current();
        if (Match("-") || Match("!") || Match("not")) { e->kind = Expr::Unary; e->children.push_back(Expression(7)); }
        else if (Match("(")) { e = Expression(); Expect(")"); }
        else if (Match("[")) {
            e->kind = Expr::Array;
            if (!Is("]")) do { e->children.push_back(Expression()); } while (Match(","));
            Expect("]");
        }
        else if (Match("{")) {
            e->kind = Expr::Interpolate;
            e->children.push_back(Expression());
            Expect("}");
        }
        else if (Current().kind == Token::Number) {
            ++pos;
            try {
                if (e->token.text.find_first_of(".eE") != std::string::npos) {
                    double v = std::stod(e->token.text);
                    if (!std::isfinite(v)) Fail(source, e->token, "Non-finite literal");
                    e->value = v;
                } else e->value = static_cast<std::int64_t>(std::stoll(e->token.text));
            } catch (const std::out_of_range&) { Fail(source, e->token, "Numeric literal out of range"); }
        } else if (Current().kind == Token::String || Current().kind == Token::Character) {
            try {if(Current().kind==Token::Character)e->value=text::Character(Current().text);else {text::Offsets(Current().text);e->value=Current().text;}}
            catch(const std::exception& error){Fail(source,Current(),error.what());}++pos;
        }
        else if (Match("true")) e->value = true;
        else if (Match("false")) e->value = false;
        else if (Match("null")) e->value = ObjectRef{};
        else if (Current().kind == Token::Identifier) { e->kind = Expr::Name; ++pos; }
        else Fail(source, Current(), "Expected expression");
        CheckHeight(*e);
        std::size_t chain = 0;
        while (Is(".") || Is("(") || Is("[")) {
            if (++chain > 128) Fail(source, Current(), "Member/call chain limit exceeded");
            auto next = std::make_unique<Expr>();
            if (Match(".")) { next->kind = Expr::Member; next->token = Identifier(); next->children.push_back(std::move(e)); }
            else if (Is("[")) {
                next->kind = Expr::Index; next->token = Expect("[");
                next->children.push_back(std::move(e)); next->children.push_back(Expression()); Expect("]");
            }
            else {
                next->kind = Expr::Call; next->token = Expect("("); next->children.push_back(std::move(e));
                if (!Is(")")) do { next->children.push_back(Expression()); } while (Match(","));
                Expect(")");
            }
            CheckHeight(*next); e = std::move(next);
        }
        std::size_t operators = 0;
        while ((Current().kind == Token::Symbol || Is("is") || Is("and") || Is("or") || Is("nor")) && Precedence(Current().text) >= min) {
            if (++operators > 128) Fail(source, Current(), "Expression length limit exceeded");
            auto next = std::make_unique<Expr>(); next->kind = Expr::Binary; next->token = Current(); ++pos;
            next->children.push_back(std::move(e));
            if (next->token.text == "is") { next->kind = Expr::IsType; next->value = Identifier(true).text; }
            else next->children.push_back(Expression(Precedence(next->token.text) + 1));
            CheckHeight(*next); e = std::move(next);
        }
        return e;
    }
    Stmt Statement() {
        Guard guard(*this);
        Stmt s; s.token = Current();
        if (Is("export") || Is("label") || Is("multiline")) Fail(source, Current(), "Inspector declarations are only allowed at class scope");
        if (Match("{")) {
            while (!Is("}")) { if (Current().kind == Token::End) Fail(source, Current(), "Unterminated block"); s.children.push_back(Statement()); }
            Expect("}"); return s;
        }
        if (Match("if") || Match("while") || Match("for")) {
            s.kind = s.token.text == "if" ? Stmt::If : Stmt::While;
            Expect("("); s.expressions.push_back(Expression()); Expect(")");
            if (!Is("{")) Fail(source, Current(), "Control flow requires a braced block");
            s.children.push_back(Statement());
            if (s.kind == Stmt::If && Match("else")) {
                if (!Is("{")) Fail(source, Current(), "Else requires a braced block");
                s.children.push_back(Statement());
            }
            return s;
        }
        if (Match("return")) {
            s.kind = Stmt::Return;
            if (!Is(";")) s.expressions.push_back(Expression());
        } else if (Current().kind == Token::Identifier && tokens[pos + 1].kind == Token::Identifier && tokens[pos + 1].text != "is") {
            s.kind = Stmt::Variable; s.type = Identifier(true).text; s.token = Identifier();
            if (Match("=")) s.expressions.push_back(Expression());
        } else {
            s.kind = Stmt::Expression; s.expressions.push_back(Expression());
            if (Is("=") || Is("+=") || Is("-=") || Is("*=") || Is("/=") || Is("&=")) {
                s.operation = Current().text; ++pos; s.kind = Stmt::Assignment; s.expressions.push_back(Expression());
            }
        }
        Expect(";"); return s;
    }
public:
    Parser(std::vector<Token> input, const std::string& name) : tokens(std::move(input)), source(name) {}
    std::vector<ClassAst> Parse() {
        std::vector<ClassAst> classes;
        while (Current().kind != Token::End) {
            Expect("class"); ClassAst c; c.name = Identifier();
            if (Match(":")) c.base = Identifier(true).text;
            Expect("{");
            while (!Is("}")) {
                const Token declaration = Current();
                if (Match("label")) {
                    Expect("(");
                    if (Current().kind != Token::String) Fail(source, Current(), "Label requires a string literal");
                    InspectorEntry entry; entry.kind = InspectorEntry::Kind::Label; entry.text = Current().text; ++pos;
                    entry.declaringClass = c.name.text; entry.source = source; entry.line = declaration.line; entry.column = declaration.column;
                    Expect(")"); Match(";"); c.inspector.push_back(std::move(entry));
                } else if (Match("signal")) {
                    c.signals.push_back(Identifier()); Expect(";");
                } else if (Match("func")) {
                    FunctionAst f; f.name = Identifier(); Expect("(");
                    if (!Is(")")) do {
                        FieldAst param; param.type = Identifier(true).text; param.name = Identifier(); f.params.push_back(std::move(param));
                    } while (Match(","));
                    Expect(")"); if (Match(":")) f.result = Identifier(true).text;
                    if (!Is("{")) Fail(source, Current(), "Expected function body");
                    f.body = Statement(); c.methods.push_back(std::move(f));
                } else {
                    bool exported=false,multiline=false;
                    while(Is("export") || Is("multiline")) {const bool multi=Match("multiline");if(!multi)Expect("export");auto& flag=multi?multiline:exported;if(flag)Fail(source,Current(),"Duplicate field tag");flag=true;}
                    if (exported && (Current().kind != Token::Identifier || Is("func") || Is("label") || Is("export")))
                        Fail(source, Current(), "Export must precede a typed class field");
                    FieldAst field; field.type = Identifier(true).text; field.name = Identifier();
                    if(multiline && (!exported || field.type!="string"))Fail(source,field.name,"multiline requires an exported string field");
                    if (Match("=")) field.initializer = Expression();
                    Expect(";");
                    if (exported) {
                        InspectorEntry entry; entry.kind = InspectorEntry::Kind::Field; entry.name = field.name.text; entry.type = field.type;
                        entry.multiline=multiline;
                        entry.declaringClass = c.name.text; entry.source = source; entry.line = field.name.line; entry.column = field.name.column;
                        c.inspector.push_back(std::move(entry));
                    }
                    c.fields.push_back(std::move(field));
                }
            }
            Expect("}"); classes.push_back(std::move(c));
            if (classes.size() > 1024) Fail(source, Current(), "Class limit exceeded");
        }
        return classes;
    }
};
enum class Op { Constant, Duplicate, DuplicatePair, MakeArray, ArrayGet, ArraySet, ArrayCall, TextCall, Concat, Stringify, IsType, FindType, MakeVector, SetComponent, Self, Input, Physics, Math, LoadLocal, StoreLocal, LoadField, StoreField, Call, SignalCall, New, Pop, Return, Negate, Not, Add, Subtract, Multiply, Divide, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Jump, JumpFalse };
struct Instruction { Op op; Token token; std::size_t a = 0; std::string name; Value value; };
struct Function {
    Token token;
    std::string result = "void";
    std::vector<std::string> params, locals;
    std::vector<Instruction> code;
};
struct Field { Token token; std::string type; };
struct Class {
    std::string name, base;
    std::vector<Field> fields;
    std::vector<InspectorEntry> inspector;
    std::map<std::string, Function> methods;
    std::set<std::string> signals;
    Function initializer;
};
bool Numeric(const std::string& t) { return t == "int" || t == "float"; }
bool TextType(const std::string& t) { return t=="string" || t=="char"; }
bool GlobalField(const std::string& name) {return name=="global_position" || name=="global_rotation" || name=="global_scale";}
bool DirectionField(const std::string& name) {return name=="forward" || name=="up" || name=="right";}
bool NativeBehavior(const std::string& name){const auto* t=FindNativeType(name);return t && (t->component || t->name=="Behavior" || t->name=="PhysicsBody");}
bool NativeClass(const std::string& name){const auto* t=FindNativeType(name);return t && t->scriptClass;}
Value DefaultValue(const std::string& t) {
    if (t == "int") return std::int64_t{0};
    if (t == "float") return 0.0;
    if (t == "bool") return false;
    if (t == "string") return std::string{};
    if (t == "char") return char32_t{};
    if (t == "void") return {};
    if (t == "Vector3") return Vector3{};
    if (t == "Vector2") return Vector2{};
    if (t == "prefab") return ObjectRef{};
    return ObjectRef{};
}
}
struct Program::Impl {
    std::string source;
    std::map<std::string, Class> classes;
    ProgramStats stats;
    bool IsType(const std::string& t) const { return t == "char" || t == "array" || t == "int" || t == "float" || t == "bool" || t == "string" || t == "Vector3" || t == "Vector2" || classes.contains(t); }
    // True for anything derived from gameObject OR gameObject2D (both are scene objects
    // with a transform, parent and the built-in lifecycle hooks / helper methods).
    bool IsGameObjectLike(const std::string& name) const {
        return Assignable("gameObject", name) || Assignable("gameObject2D", name);
    }
    bool Assignable(const std::string& target, const std::string& from) const {
        if (target == from || (target == "float" && from == "int")) return true;
        if(target=="string" && from=="char")return true;
        if (!classes.contains(target)) return false;
        if (from == "null") return true;
        auto it = classes.find(from);
        while (it != classes.end()) { if (it->first == target) return true; it = classes.find(it->second.base); }
        return false;
    }
    const Function* Method(const std::string& type, const std::string& name) const {
        auto c = classes.find(type);
        while (c != classes.end()) {
            auto f = c->second.methods.find(name); if (f != c->second.methods.end()) return &f->second;
            c = classes.find(c->second.base);
        }
        return nullptr;
    }
    const Field* FindField(const std::string& type, const std::string& name, std::size_t* index = nullptr) const {
        auto c = classes.find(type); if (c == classes.end()) return nullptr;
        for (std::size_t i = 0; i < c->second.fields.size(); ++i) if (c->second.fields[i].token.text == name) {
            if (index) *index = i; return &c->second.fields[i];
        }
        return nullptr;
    }
};
namespace {
class BytecodeCompiler {
    Program::Impl& program;
    Class& owner;
    Function& function;
    std::vector<std::map<std::string, std::size_t>> scopes{1};
    std::size_t depth = 0;
    struct Guard {
        BytecodeCompiler& c;
        Guard(BytecodeCompiler& compiler, const Token& token) : c(compiler) { if (++c.depth > 256) Fail(c.program.source, token, "Expression nesting limit exceeded"); }
        ~Guard() { --c.depth; }
    };
    void Require(bool condition, const Token& t, const std::string& message) const { if (!condition) Fail(program.source, t, message); }
    std::size_t Emit(Op op, const Token& t, std::size_t a = 0, std::string name = {}, Value value = {}) {
        function.code.push_back({op, t, a, std::move(name), std::move(value)}); return function.code.size() - 1;
    }
    void Patch(std::size_t at) { function.code[at].a = function.code.size(); }
    std::size_t Local(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) { auto local = it->find(name); if (local != it->end()) return local->second; }
        return std::numeric_limits<std::size_t>::max();
    }
    void Compatible(const std::string& target, const std::string& from, const Token& t) const {
        Require(from != "void" && (target == "any" || from == "any" || program.Assignable(target, from)), t, "Cannot assign '" + from + "' to '" + target + "'");
    }
    std::size_t Declare(const Token& t, const std::string& type) {
        Require(program.IsType(type), t, "Unknown or invalid variable type '" + type + "'");
        Require(!program.classes.contains(t.text), t, "Class names are type keywords");
        Require(!scopes.back().contains(t.text), t, "Duplicate local '" + t.text + "'");
        auto index = function.locals.size(); function.locals.push_back(type); scopes.back()[t.text] = index; return index;
    }
    std::string FieldType(const std::string& type, const Token& field) const {
        if (type == "Vector3" && (field.text == "x" || field.text == "y" || field.text == "z")) return "float";
        if (type == "Vector2" && (field.text == "x" || field.text == "y")) return "float";
        const auto* f = program.FindField(type, field.text);
        Require(f != nullptr, field, "Unknown field '" + field.text + "' on '" + type + "'");
        return f->type;
    }
    std::string MemberType(const std::string& type, const Token& token) const {
        auto c = program.classes.find(type);
        if (c != program.classes.end() && c->second.signals.contains(token.text)) return "signal";
        if (program.Method(type, token.text)) return "callable";
        return FieldType(type, token);
    }
    std::string Expression(const Expr& e) {
        Guard guard(*this, e.token);
        const auto& t = e.token;
        if (e.kind == Expr::Array) {
            for (const auto& item : e.children) Require(Expression(*item) != "void", t, "Array elements cannot be void");
            Emit(Op::MakeArray, t, e.children.size()); return "array";
        }
        if (e.kind == Expr::Interpolate) {
            const auto type = Expression(*e.children[0]);
            Require(type != "void" && type != "array", t, "Cannot interpolate this value");
            Emit(Op::Stringify, t); return "string";
        }
        if (e.kind == Expr::Index) {
            const auto receiver=Expression(*e.children[0]);Require(receiver=="array" || receiver=="string" || receiver=="any",t,"Indexing requires an array or string");
            Compatible("int", Expression(*e.children[1]), t);
            Emit(Op::ArrayGet, t); return receiver=="string"?"char":"any";
        }
        if (e.kind == Expr::IsType) {
            Require(Expression(*e.children[0]) != "void", t, "Cannot test a void value");
            const auto type = std::get<std::string>(e.value);
            Require(program.IsType(type) || type == "null", t, "Unknown type '" + type + "'");
            Emit(Op::IsType, t, 0, type); return "bool";
        }
        if (e.kind == Expr::Literal) {
            Emit(Op::Constant, t, 0, {}, e.value);
            if (std::holds_alternative<std::int64_t>(e.value)) return "int";
            if (std::holds_alternative<double>(e.value)) return "float";
            if (std::holds_alternative<bool>(e.value)) return "bool";
            if (std::holds_alternative<std::string>(e.value)) return "string";
            if (std::holds_alternative<char32_t>(e.value)) return "char";
            return "null";
        }
        if (e.kind == Expr::Name) {
            if (t.text == "Input") { Emit(Op::Input,t); return "InputService"; }
            if (t.text == "Physics") { Emit(Op::Physics,t); return "PhysicsService"; }
            if (t.text == "Mathf") { Emit(Op::Math,t); return "Mathf"; }
            if (t.text == "this") { Emit(Op::Self, t); return owner.name; }
            auto local = Local(t.text);
            if (local != std::numeric_limits<std::size_t>::max()) { Emit(Op::LoadLocal, t, local); return function.locals[local]; }
            auto type = MemberType(owner.name, t); Emit(Op::Self, t); Emit(Op::LoadField, t, 0, t.text); return type;
        }
        if (e.kind == Expr::Member) {
            auto receiver = Expression(*e.children[0]); auto type = MemberType(receiver, t);
            Emit(Op::LoadField, t, 0, t.text); return type;
        }
        if (e.kind == Expr::Call) {
            const Expr& callee = *e.children[0];
            if (callee.kind == Expr::Name && callee.token.text == "Vector3") {
                Require(e.children.size() == 1 || e.children.size() == 4, t, "Vector3 takes zero or three numeric arguments");
                for (std::size_t i = 1; i < e.children.size(); ++i) Require(Numeric(Expression(*e.children[i])), t, "Vector3 components must be numeric");
                Emit(Op::MakeVector, t, e.children.size() - 1, "Vector3"); return "Vector3";
            }
            if (callee.kind == Expr::Name && callee.token.text == "Vector2") {
                Require(e.children.size() == 1 || e.children.size() == 3, t, "Vector2 takes zero or two numeric arguments");
                for (std::size_t i = 1; i < e.children.size(); ++i) Require(Numeric(Expression(*e.children[i])), t, "Vector2 components must be numeric");
                Emit(Op::MakeVector, t, e.children.size() - 1, "Vector2"); return "Vector2";
            }
            if (callee.kind == Expr::Name && program.classes.contains(Canonical(callee.token.text))) {
                Require(callee.token.text != "InputService" && callee.token.text != "InputAction" && callee.token.text != "Mouse" && callee.token.text != "PhysicsService" && callee.token.text != "Mathf" && callee.token.text != "Timer" && callee.token.text != "prefab" && !NativeBehavior(callee.token.text),t,"Native service and behavior objects are supplied by the host");
                Require(e.children.size() == 1, t, "Class construction takes no arguments");
                Emit(Op::New, t, 0, Canonical(callee.token.text)); return Canonical(callee.token.text);
            }
            std::string receiver;
            // GameObject.find(...) / gameObject.find_by_type(...) - the base type name reads
            // as "the current gameObject" (a static-style call), like a bare call would.
            const bool typePrefixed = callee.kind == Expr::Member && callee.children[0]->kind == Expr::Name
                && Canonical(callee.children[0]->token.text) == "gameObject" && Local(callee.children[0]->token.text) == std::numeric_limits<std::size_t>::max();
            if (callee.kind == Expr::Name || typePrefixed) { Emit(Op::Self, t); receiver = owner.name; }
            else if (callee.kind == Expr::Member) receiver = Expression(*callee.children[0]);
            else Fail(program.source, t, "Expected a method or class name");
            // getBehavior(RigidBody) / this.getBehavior(...) / obj.getBehavior(...) -
            // compile-time sugar for the native accessor field on a gameObject.
            if (callee.token.text == "getBehavior" && (callee.kind == Expr::Name || callee.kind == Expr::Member)
                && program.Assignable("gameObject", receiver) && !program.Method(receiver, "getBehavior")) {
                Require(e.children.size() == 2 && e.children[1]->kind == Expr::Name, t, "getBehavior takes one component type, e.g. getBehavior(RigidBody)");
                const auto typeName = Canonical(e.children[1]->token.text);
                const auto* nt = FindNativeType(typeName);
                Require(nt && !nt->accessor.empty(), e.children[1]->token, "getBehavior needs a native component type (RigidBody, KinematicBody, StaticBody, Area, Collider, Camera, PhysicsBody, Transform)");
                Emit(Op::LoadField, t, 0, std::string(nt->accessor));
                return typeName;
            }
            // find_by_type(RigidBody) - first scene object carrying that native component.
            if (callee.token.text == "find_by_type" && program.Assignable("gameObject", receiver) && !program.Method(receiver, "find_by_type")) {
                Require(e.children.size() == 2 && e.children[1]->kind == Expr::Name, t, "find_by_type takes one component type, e.g. find_by_type(RigidBody)");
                const auto typeName = Canonical(e.children[1]->token.text);
                const auto* nt = FindNativeType(typeName);
                Require(nt && (nt->component || nt->name == "PhysicsBody" || nt->name == "Behavior"), e.children[1]->token,
                        "find_by_type needs a native component type (RigidBody, KinematicBody, StaticBody, Area, Collider, Camera, PhysicsBody, Behavior)");
                Emit(Op::FindType, t, 0, std::string(nt->name));
                return "gameObject";
            }
            if(receiver=="string" || (receiver=="any" && (callee.token.text=="truncate" || callee.token.text=="substr"))) {
                const auto& method=callee.token.text;
                Require(method=="size" || method=="truncate" || method=="substr",t,"Unknown string method");
                Require(e.children.size()==(method=="size"?1:method=="truncate"?2:3),t,"Wrong string argument count");
                for(std::size_t i=1;i<e.children.size();++i)Compatible("int",Expression(*e.children[i]),t);
                Emit(Op::TextCall,t,e.children.size()-1,method);return method=="size"?"int":"string";
            }
            if (receiver == "array" || receiver == "any") {
                const auto& method = callee.token.text;
                Require(method == "append" || method == "erase" || method == "size", t, "Unknown array method");
                Require(e.children.size() == (method == "size" ? 1 : 2), t, "Wrong array argument count");
                if (method != "size") {
                    const auto type = Expression(*e.children[1]);
                    Require(type != "void", t, "Array value cannot be void");
                    if (method == "erase") Compatible("int", type, t);
                }
                Emit(Op::ArrayCall, t, e.children.size() - 1, method);
                return method == "size" ? "int" : "void";
            }
            if (receiver == "signal") {
                const auto& method = callee.token.text;
                Require(method == "connect" || method == "disconnect" || method == "is_connected" || method == "emit", t, "Unknown signal method");
                if (method != "emit") {
                    Require(e.children.size() == 2, t, "Signal connection method takes one function reference");
                    Require(Expression(*e.children[1]) == "callable", t, "Expected a function reference (without parentheses)");
                } else {
                    Require(e.children.size() <= 65, t, "Signal argument limit exceeded");
                    for (std::size_t i = 1; i < e.children.size(); ++i)
                        Require(Expression(*e.children[i]) != "void", t, "Signal arguments cannot be void");
                }
                Emit(Op::SignalCall, t, e.children.size() - 1, method);
                return method == "is_connected" ? "bool" : "void";
            }
            const auto* f = program.Method(receiver, callee.token.text);
            Require(f != nullptr, callee.token, "Unknown method '" + callee.token.text + "' on '" + receiver + "'");
            Require(f->params.size() == e.children.size() - 1, t, "Wrong argument count for '" + callee.token.text + "'");
            for (std::size_t i = 1; i < e.children.size(); ++i) Compatible(f->params[i - 1], Expression(*e.children[i]), e.children[i]->token);
            Emit(Op::Call, t, f->params.size(), callee.token.text); return f->result;
        }
        if (e.kind == Expr::Unary) {
            auto type = Expression(*e.children[0]);
            Require(type == "any" || ((t.text == "!" || t.text == "not") ? type == "bool" : (Numeric(type) || type == "Vector3" || type == "Vector2")), t, "Invalid unary operand");
            Emit((t.text == "!" || t.text == "not") ? Op::Not : Op::Negate, t); return type;
        }
        auto left = Expression(*e.children[0]);
        if (t.text == "&&" || t.text == "||" || t.text == "and" || t.text == "or" || t.text == "nor") {
            Require(left == "bool", t, "Logical operands must be bool");
            auto branch = Emit(Op::JumpFalse, t);
            if (t.text == "||" || t.text == "or") {
                Emit(Op::Constant, t, 0, {}, true); auto finish = Emit(Op::Jump, t); Patch(branch);
                Require(Expression(*e.children[1]) == "bool", t, "Logical operands must be bool"); Patch(finish);
            } else if (t.text == "nor") {
                Emit(Op::Constant, t, 0, {}, false); auto finish = Emit(Op::Jump, t); Patch(branch);
                Require(Expression(*e.children[1]) == "bool", t, "Logical operands must be bool");Emit(Op::Not,t);Patch(finish);
            } else {
                Require(Expression(*e.children[1]) == "bool", t, "Logical operands must be bool");
                auto finish = Emit(Op::Jump, t); Patch(branch); Emit(Op::Constant, t, 0, {}, false); Patch(finish);
            }
            return "bool";
        }
        auto right = Expression(*e.children[1]);
        static const std::map<std::string, Op> operators = {{"&", Op::Concat},{"+", Op::Add}, {"-", Op::Subtract}, {"*", Op::Multiply}, {"/", Op::Divide}, {"==", Op::Equal}, {"!=", Op::NotEqual}, {"<", Op::Less}, {"<=", Op::LessEqual}, {">", Op::Greater}, {">=", Op::GreaterEqual}};
        if (t.text == "==" || t.text == "!=") {
            Require(left != "void" && right != "void" && (left == "any" || right == "any" || program.Assignable(left, right) || program.Assignable(right, left)), t, "Incompatible equality operands");
            Emit(operators.at(t.text), t); return "bool";
        }
            if (t.text == "<" || t.text == "<=" || t.text == ">" || t.text == ">=") {
            Require((TextType(left) && TextType(right)) || ((Numeric(left) || left == "any") && (Numeric(right) || right == "any")) || (left=="any" && TextType(right)) || (right=="any" && TextType(left)), t, "Comparison operands must both be numeric or text"); Emit(operators.at(t.text), t); return "bool";
        }
        auto result = ArithmeticType(t.text, left, right, t); Emit(operators.at(t.text), t); return result;
    }
    std::string ArithmeticType(const std::string& op, const std::string& left, const std::string& right, const Token& t) const {
        if (left != "void" && right != "void" && (left == "any" || right == "any")) return "any";
        if ((op == "+" || op=="&") && TextType(left) && TextType(right)) return "string";
        Require(op!="&",t,"Concatenation operands must be strings or characters");
        for (const char* v : {"Vector3", "Vector2"}) {
            if ((op == "+" || op == "-" || op == "*") && left == v && right == v) return v;
            if ((op == "*" || op == "/") && left == v && Numeric(right)) return v;
            if (op == "*" && Numeric(left) && right == v) return v;
        }
        Require(Numeric(left) && Numeric(right), t, "Arithmetic operands must be numeric or compatible Vector3/Vector2 values");
        return left == "float" || right == "float" ? "float" : "int";
    }
    struct Slot { std::string type; bool field; std::size_t index; std::string name; };
    Slot Destination(const Expr& e) {
        if (e.kind == Expr::Name) {
            Require(e.token.text != "this", e.token, "Cannot assign to this");
            auto local = Local(e.token.text);
            if (local != std::numeric_limits<std::size_t>::max()) return {function.locals[local], false, local, {}};
            auto type = FieldType(owner.name, e.token); Emit(Op::Self, e.token); return {type, true, 0, e.token.text};
        }
        Require(e.kind == Expr::Member, e.token, "Assignment target must be a variable or field");
        auto receiver = Expression(*e.children[0]);
        Require(receiver != "Vector3" && receiver != "Vector2", e.token, "Cannot assign to a temporary vector component");
        Require(receiver != "InputAction" && receiver != "Mouse",e.token,"Input state is read-only");
        const auto nativeAccessor=[&](const std::string& field){for(const auto& n:NativeTypes())if(!n.accessor.empty()&&n.accessor!="transform"&&n.accessor==field)return true;return false;};
        Require(!(program.Assignable("gameObject",receiver) && nativeAccessor(e.token.text)),e.token,"Native behavior references are read-only");
            Require(!(program.Assignable("Transform",receiver) && GlobalField(e.token.text)),e.token,"Global transform fields are read-only");
        return {FieldType(receiver, e.token), true, 0, e.token.text};
    }
    void Load(const Slot& slot, const Token& t) {
        if (slot.field) { Emit(Op::Duplicate, t); Emit(Op::LoadField, t, 0, slot.name); }
        else Emit(Op::LoadLocal, t, slot.index);
    }
    bool Statement(const Stmt& s) {
        Guard guard(*this, s.token);
        const auto& t = s.token;
        if (s.kind == Stmt::Block) {
            scopes.emplace_back(); bool returns = false;
            for (const auto& child : s.children) { bool r = Statement(child); returns = returns || r; }
            scopes.pop_back(); return returns;
        }
        if (s.kind == Stmt::Variable) {
            Require(program.IsType(s.type), t, "Unknown or invalid variable type '" + s.type + "'");
            if (s.expressions.empty()) {
                if (s.type == "array") Emit(Op::MakeArray, t);
                else Emit(Op::Constant, t, 0, {}, DefaultValue(s.type));
            }
            else Compatible(s.type, Expression(*s.expressions[0]), t);
            Emit(Op::StoreLocal, t, Declare(t, s.type)); return false;
        }
        if (s.kind == Stmt::Assignment) {
            const auto& target = *s.expressions[0];
            if (target.kind == Expr::Index) {
                Compatible("array", Expression(*target.children[0]), t);
                Compatible("int", Expression(*target.children[1]), t);
                if (s.operation != "=") { Emit(Op::DuplicatePair, t); Emit(Op::ArrayGet, t); }
                Require(Expression(*s.expressions[1]) != "void", t, "Array value cannot be void");
                if (s.operation != "=") Emit(s.operation == "&=" ? Op::Concat : s.operation == "+=" ? Op::Add : s.operation == "-=" ? Op::Subtract : s.operation == "*=" ? Op::Multiply : Op::Divide, t);
                Emit(Op::ArraySet, t); return false;
            }
            const Expr* destination = &target;
            bool component = false;
            if (target.kind == Expr::Member) {
                auto mark = function.code.size(); auto receiverType = Expression(*target.children[0]); function.code.resize(mark);
                component = receiverType == "Vector3" || receiverType == "Vector2";
                if (component) { FieldType(receiverType, target.token); destination = target.children[0].get(); }
            }
            auto slot = Destination(*destination);
            if (component || s.operation != "=") Load(slot, t);
            if (component && s.operation != "=") { Emit(Op::Duplicate, t); Emit(Op::LoadField, target.token, 0, target.token.text); }
            auto expected = component ? std::string("float") : slot.type;
            auto actual = Expression(*s.expressions[1]);
            if (s.operation != "=") {
                const auto op = s.operation.substr(0, 1);
                actual = ArithmeticType(op, expected, actual, t);
                Emit(op == "&" ? Op::Concat : op == "+" ? Op::Add : op == "-" ? Op::Subtract : op == "*" ? Op::Multiply : Op::Divide, t);
            }
            Compatible(expected, actual, t);
            if (component) Emit(Op::SetComponent, target.token, 0, target.token.text);
            Emit(slot.field ? Op::StoreField : Op::StoreLocal, t, slot.index, slot.name);
            return false;
        }
        if (s.kind == Stmt::Expression) { Expression(*s.expressions[0]); Emit(Op::Pop, t); return false; }
        if (s.kind == Stmt::Return) {
            if (s.expressions.empty()) { Require(function.result == "void", t, "Return value required"); Emit(Op::Constant, t); }
            else { Require(function.result != "void", t, "Void function cannot return a value"); Compatible(function.result, Expression(*s.expressions[0]), t); }
            Emit(Op::Return, t); return true;
        }
        auto loop = function.code.size();
        Require(Expression(*s.expressions[0]) == "bool", t, "Condition must be bool");
        auto branch = Emit(Op::JumpFalse, t); bool first = Statement(s.children[0]);
        if (s.kind == Stmt::While) { Emit(Op::Jump, t, loop); Patch(branch); return false; }
        auto finish = Emit(Op::Jump, t); Patch(branch);
        bool second = s.children.size() == 2 && Statement(s.children[1]); Patch(finish);
        return first && second;
    }
public:
    BytecodeCompiler(Program::Impl& p, Class& c, Function& f) : program(p), owner(c), function(f) {}
    void Compile(const FunctionAst& ast) {
        for (const auto& param : ast.params) Declare(param.name, param.type);
        // The top-level body shares parameter scope, so redeclaring a parameter is an error.
        bool returns = false;
        for (const auto& s : ast.body.children) { bool r = Statement(s); returns = returns || r; }
        Require(function.result == "void" || returns, ast.name, "Non-void function must return on every path");
        if (!function.code.empty() && function.result == "void") { Emit(Op::Constant, ast.name); Emit(Op::Return, ast.name); }
    }
    void Initialize(const ClassAst& ast) {
        for (const auto& field : ast.fields) if (field.initializer) {
            Emit(Op::Self, field.name); Compatible(field.type, Expression(*field.initializer), field.name); Emit(Op::StoreField, field.name, 0, field.name.text);
        }
        if (!function.code.empty()) { Emit(Op::Constant, ast.name); Emit(Op::Return, ast.name); }
    }
};
void BuildDeclarations(Program::Impl& program, const std::vector<ClassAst>& asts) {
    Class input; input.name="InputService";
    for(const auto& name:{"is_action_pressed","is_action_just_pressed","is_action_just_released","get_axis","get_vector","action"}) {
        Function f;f.params={"string"};f.result=std::string(name)=="action"?"InputAction":std::string(name)=="get_axis"?"float":std::string(name)=="get_vector"?"Vector2":"bool";
        input.methods.emplace(name,std::move(f));
    }
    input.fields={{{Token::Identifier,"mouse"},"Mouse"}}; // Input.mouse
    program.classes.emplace(input.name,std::move(input));
    Class action;action.name="InputAction";action.signals={"just_pressed","just_released","is_pressed","was_just_pressed","was_just_released"};
    action.fields={{{Token::Identifier,"pressed"},"bool"},{{Token::Identifier,"axis"},"Vector2"},{{Token::Identifier,"value"},"Vector2"}};
    program.classes.emplace(action.name,std::move(action));
    Class mouse;mouse.name="Mouse";
    mouse.signals={"clicked","click_ended","held","was_just_moved"};
    mouse.fields={{{Token::Identifier,"delta"},"Vector2"},{{Token::Identifier,"position"},"Vector2"}};
    program.classes.emplace(mouse.name,std::move(mouse));
    Class physicsService;physicsService.name="PhysicsService";
    for(const auto& name:{"cast","cast_all"}){Function f;f.params={"Vector3","Vector3","int"};f.result=std::string(name)=="cast"?"gameObject":"array";physicsService.methods.emplace(name,std::move(f));}
    program.classes.emplace(physicsService.name,std::move(physicsService));
    Class math;math.name="Mathf";
    for(const auto& name:{"sin","cos","tan","sqrt","exp","round"}){Function f;f.params={"float"};f.result="float";math.methods.emplace(name,std::move(f));}
    {Function f;f.params={"float","float","float"};f.result="float";math.methods.emplace("lerp",std::move(f));}
    {Function f;f.params={"Vector3","Vector3"};f.result="float";math.methods.emplace("dot",std::move(f));}
    {Function f;f.params={"Vector3","Vector3"};f.result="Vector3";math.methods.emplace("cross",std::move(f));}
    program.classes.emplace(math.name,std::move(math));
    Class physicsBody;physicsBody.name="PhysicsBody";
    physicsBody.fields={{{Token::Identifier,"velocity"},"Vector3"},{{Token::Identifier,"angular_velocity"},"Vector3"}};
    physicsBody.signals={"collision_entered","collision_stayed","collision_exited","area_entered","area_stayed","area_exited"};
    for(const auto& name:{"add_force","add_impulse","add_torque","add_angular_impulse"}){Function f;f.params={"Vector3"};physicsBody.methods.emplace(name,std::move(f));}
    program.classes.emplace(physicsBody.name,std::move(physicsBody));
    Class prefab;prefab.name="prefab";Function spawn;spawn.result="gameObject";prefab.methods.emplace("spawn",std::move(spawn));program.classes.emplace(prefab.name,std::move(prefab));
    Class transform; transform.name = "Transform";
    transform.signals = {"was_moved", "was_rotated", "was_scaled"};
    for (const auto& name : {"position", "rotation", "scale"}) transform.fields.push_back({Token{Token::Identifier, name}, "Vector3"});
    for (const auto& name : {"global_position", "global_rotation", "global_scale"}) transform.fields.push_back({Token{Token::Identifier, name}, "Vector3"});
    for (const auto& name : {"forward", "up", "right"}) transform.fields.push_back({Token{Token::Identifier, name}, "Vector3"});
    program.classes.emplace("Transform", std::move(transform));
    Class gameObject; gameObject.name = "gameObject";
    gameObject.fields.push_back({Token{Token::Identifier, "transform"}, "Transform"});
    gameObject.fields.push_back({Token{Token::Identifier, "parent"}, "gameObject"});
    // Read-only accessor field per native component type (physics, rigidbody, collider, ...).
    for(const auto& native:NativeTypes())
        if(!native.accessor.empty() && native.accessor!="transform")
            gameObject.fields.push_back({Token{Token::Identifier,std::string(native.accessor)},std::string(native.name)});
    Function find;find.params={"string"};find.result="gameObject";gameObject.methods.emplace("find",std::move(find));
    Function makeTimer;makeTimer.params={"float"};makeTimer.result="Timer";gameObject.methods.emplace("make_timer",std::move(makeTimer));
    Function print;print.params={"string"};print.result="void";gameObject.methods.emplace("print",std::move(print));
    Function getTags;getTags.result="array";gameObject.methods.emplace("get_tags",std::move(getTags));
    Function hasTag;hasTag.params={"string"};hasTag.result="bool";gameObject.methods.emplace("has_tag",std::move(hasTag));
    program.classes.emplace("gameObject", std::move(gameObject));
    // gameObject2D: identical to gameObject but with a 2D (screen-space) transform.
    Class transform2d; transform2d.name = "Transform2D";
    transform2d.signals = {"was_moved", "was_rotated", "was_scaled"};
    transform2d.fields.push_back({Token{Token::Identifier, "position"}, "Vector2"});
    transform2d.fields.push_back({Token{Token::Identifier, "rotation"}, "float"});
    transform2d.fields.push_back({Token{Token::Identifier, "scale"}, "Vector2"});
    program.classes.emplace("Transform2D", std::move(transform2d));
    Class gameObject2D; gameObject2D.name = "gameObject2D";
    gameObject2D.fields.push_back({Token{Token::Identifier, "transform"}, "Transform2D"});
    gameObject2D.fields.push_back({Token{Token::Identifier, "parent"}, "gameObject2D"});
    gameObject2D.methods.emplace("find", [] { Function f; f.params={"string"}; f.result="gameObject2D"; return f; }());
    gameObject2D.methods.emplace("make_timer", [] { Function f; f.params={"float"}; f.result="Timer"; return f; }());
    gameObject2D.methods.emplace("print", [] { Function f; f.params={"string"}; f.result="void"; return f; }());
    gameObject2D.methods.emplace("get_tags", [] { Function f; f.result="array"; return f; }());
    gameObject2D.methods.emplace("has_tag", [] { Function f; f.params={"string"}; f.result="bool"; return f; }());
    program.classes.emplace("gameObject2D", std::move(gameObject2D));
    // UI controls (ZE-61): a native class chain rooted at gameObject2D. A user's
    // "class Menu : PanelContainer" inherits the transform, the layout fields and
    // the "clicked" signal. The engine ui:: behaviors are the source of truth;
    // these classes let scripts drive and listen to them.
    {
        auto uiField = [](const char* name, const char* type) { return Field{Token{Token::Identifier, name}, type}; };
        auto uiClass = [&](const char* name, const char* base, std::vector<Field> extra) {
            Class c; c.name = name; c.base = base;
            c.fields = program.classes.at(base).fields;
            c.signals = program.classes.at(base).signals;
            c.signals.insert("clicked");
            for (auto& f : extra) c.fields.push_back(std::move(f));
            program.classes.emplace(name, std::move(c));
        };
        uiClass("uiControl", "gameObject2D", {uiField("anchor", "string"), uiField("size", "Vector2"), uiField("min_size", "Vector2"),
                                             uiField("order", "int"), uiField("visible", "bool"), uiField("clickable", "bool")});
        uiClass("uiContainer", "uiControl", {uiField("padding", "float"), uiField("spacing", "float")});
        uiClass("uiHTileBox", "uiContainer", {uiField("fill_cross", "bool")});
        uiClass("uiVTileBox", "uiContainer", {uiField("fill_cross", "bool")});
        uiClass("uiCenterBox", "uiContainer", {});
        uiClass("uiMarginBox", "uiContainer", {uiField("margin_left", "float"), uiField("margin_top", "float"),
                                               uiField("margin_right", "float"), uiField("margin_bottom", "float")});
        uiClass("uiPanel", "uiContainer", {uiField("texture", "string"), uiField("tint", "Vector3")});
        uiClass("uiText", "uiControl", {uiField("text", "string"), uiField("pixel_height", "float"), uiField("color", "Vector3")});
        uiClass("uiLongText", "uiText", {});
        uiClass("uiTextEntry", "uiControl", {uiField("text", "string"), uiField("placeholder", "string"), uiField("pixel_height", "float")});
        uiClass("uiTextureRect", "uiControl", {uiField("texture", "string"), uiField("tint", "Vector3")});
        uiClass("uiColorRect", "uiControl", {uiField("color", "Vector3")});
        uiClass("uiProgressBar", "uiControl", {uiField("value", "float"), uiField("vertical", "bool"),
                                              uiField("fill_color", "Vector3"), uiField("background_color", "Vector3")});
    }
    Class timer;timer.name="Timer";timer.base="gameObject";timer.fields=program.classes.at("gameObject").fields;timer.signals={"finished"};program.classes.emplace(timer.name,std::move(timer));
    Class behavior;behavior.name="Behavior";behavior.base="gameObject";behavior.fields=program.classes.at("gameObject").fields;program.classes.emplace(behavior.name,std::move(behavior));
    auto& nativePhysics=program.classes.at("PhysicsBody");nativePhysics.base="Behavior";nativePhysics.fields.insert(nativePhysics.fields.begin(),program.classes.at("gameObject").fields.begin(),program.classes.at("gameObject").fields.end());
    for(const auto& native:NativeTypes())if(native.physicsBody){Class body;body.name=std::string(native.name);body.base=std::string(native.base);body.fields=nativePhysics.fields;body.signals=nativePhysics.signals;program.classes.emplace(body.name,std::move(body));}
    // Non-physics native components (Collider, Camera, ...) are plain Behavior-derived classes.
    for(const auto& native:NativeTypes())
        if(native.component && !native.physicsBody && !program.classes.contains(std::string(native.name))) {
            Class c;c.name=std::string(native.name);c.base=std::string(native.base);c.fields=program.classes.at("gameObject").fields;program.classes.emplace(c.name,std::move(c));
        }
    std::map<std::string, const ClassAst*> byName;
    for (const auto& ast : asts) {
        if (program.classes.contains(ast.name.text)) Fail(program.source, ast.name, "Duplicate class '" + ast.name.text + "'");
        Class c; c.name = ast.name.text; c.base = ast.base; c.initializer.token = ast.name;
        program.classes.emplace(c.name, std::move(c)); byName[ast.name.text] = &ast;
    }
    std::map<std::string, int> state;
    auto build = [&](auto&& self, const std::string& name, std::size_t depth) -> void {
        if (NativeClass(name) || state[name] == 2) return;
        const auto& ast = *byName.at(name); auto& c = program.classes.at(name);
        if (state[name] == 1) Fail(program.source, ast.name, "Inheritance cycle");
        if (depth > 128) Fail(program.source, ast.name, "Inheritance depth limit exceeded");
        state[name] = 1;
        if (!c.base.empty()) {
            if(c.base=="InputService" || c.base=="InputAction" || c.base=="Mouse" || c.base=="PhysicsService" || c.base=="Mathf" || c.base=="Timer" || c.base=="prefab")Fail(program.source,ast.name,"Cannot inherit native service types");
            if (!program.classes.contains(c.base)) Fail(program.source, ast.name, "Unknown base class '" + c.base + "'");
            self(self, c.base, depth + 1); c.fields = program.classes.at(c.base).fields;
            c.inspector = program.classes.at(c.base).inspector;
            c.signals = program.classes.at(c.base).signals;
        }
        for (const auto& signal : ast.signals) {
            if (program.classes.contains(signal.text) || program.FindField(name, signal.text) || program.Method(c.base, signal.text) || !c.signals.insert(signal.text).second)
                Fail(program.source, signal, "Duplicate/inherited signal member '" + signal.text + "'");
        }
        c.inspector.insert(c.inspector.end(), ast.inspector.begin(), ast.inspector.end());
        for (const auto& f : ast.fields) {
            if (!program.IsType(f.type)) Fail(program.source, f.name, "Unknown or invalid field type '" + f.type + "'");
            if (program.classes.contains(f.name.text)) Fail(program.source, f.name, "Class names are type keywords");
            if (c.signals.contains(f.name.text) || program.FindField(name, f.name.text) || program.Method(c.base, f.name.text)) Fail(program.source, f.name, "Duplicate/inherited member '" + f.name.text + "'");
            c.fields.push_back({f.name, f.type});
        }
        for (const auto& method : ast.methods) {
            if (program.classes.contains(method.name.text)) Fail(program.source, method.name, "Class names are type keywords");
            if (c.signals.contains(method.name.text) || c.methods.contains(method.name.text) || program.FindField(name, method.name.text)) Fail(program.source, method.name, "Duplicate member '" + method.name.text + "'");
            Function f; f.token = method.name; f.result = method.result;
            if (f.result != "void" && !program.IsType(f.result)) Fail(program.source, method.name, "Unknown return type '" + f.result + "'");
            for (const auto& param : method.params) {
                if (!program.IsType(param.type)) Fail(program.source, param.name, "Unknown or invalid parameter type");
                f.params.push_back(param.type);
            }
            if (const auto* base = program.Method(c.base, method.name.text)) {
                if (base->result != f.result || base->params != f.params) Fail(program.source, method.name, "Override must preserve the inherited signature");
            }
            if (program.IsGameObjectLike(name)) {
                if (method.name.text == "start" || method.name.text == "draw") {
                    if (f.result != "void" || !f.params.empty()) Fail(program.source, method.name, "Lifecycle hook must be void with no parameters");
                } else if (method.name.text == "update" || method.name.text == "physicsUpdate") {
                    if (f.result != "void" || f.params != std::vector<std::string>{"float"}) Fail(program.source, method.name, method.name.text=="update"?"Update signature must be func update(float delta)":"Physics update signature must be func physicsUpdate(float delta)");
                }
            }
            c.methods.emplace(method.name.text, std::move(f));
        }
        state[name] = 2;
    };
    for (const auto& ast : asts) build(build, ast.name.text, 0);
}
}
ScriptError::ScriptError(Diagnostic diagnostic)
    : std::runtime_error(diagnostic.source + ":" + std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.column) + ": " + diagnostic.message), diagnostic_(std::move(diagnostic)) {}
Program::Program(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
ProgramStats Program::Stats() const { return impl_->stats; }
bool Program::HasClass(std::string_view name) const { return impl_->classes.contains(Canonical(std::string(name))); }
bool Program::IsGameObject(std::string_view name) const { return impl_->IsGameObjectLike(Canonical(std::string(name))); }
bool Program::HasCode(std::string_view className, std::string_view method) const {
    const auto* f = impl_->Method(Canonical(std::string(className)), std::string(method));
    return f && !f->code.empty();
}
const std::vector<InspectorEntry>& Program::InspectorLayout(std::string_view className) const {
    auto found = impl_->classes.find(Canonical(std::string(className)));
    if (found == impl_->classes.end()) Fail(impl_->source, {}, "Unknown class '" + std::string(className) + "'");
    return found->second.inspector;
}
CompileResult Compiler::Compile(std::string_view source, std::string sourceName) {
    try {
        auto p = std::make_shared<Program::Impl>(); p->source = std::move(sourceName);
        auto asts = Parser(Lex(source, p->source), p->source).Parse(); BuildDeclarations(*p, asts);
        for (const auto& ast : asts) {
            auto& c = p->classes.at(ast.name.text);
            BytecodeCompiler(*p, c, c.initializer).Initialize(ast);
            for (const auto& method : ast.methods) BytecodeCompiler(*p, c, c.methods.at(method.name.text)).Compile(method);
        }
        p->stats.declaredClasses = asts.size();
        for (const auto& [name, c] : p->classes) {
            bool active = c.fields.size() > (p->Assignable("gameObject", name) ? p->classes.at("gameObject").fields.size() : 0u);
            auto count = [&](const Function& f) { if (!f.code.empty()) { ++p->stats.emittedFunctions; p->stats.instructions += f.code.size(); } };
            count(c.initializer);
            for (const auto& [methodName, f] : c.methods) { (void)methodName; count(f); }
            std::set<std::string> seen;
            const Class* current = &c;
            while (current) {
                for (const auto& [methodName, f] : current->methods) if (seen.insert(methodName).second && !f.code.empty()) active = true;
                current = current->base.empty() ? nullptr : &p->classes.at(current->base);
            }
            if (!NativeClass(name) && active) ++p->stats.executableClasses;
        }
        return {std::shared_ptr<const Program>(new Program(std::move(p))), {}};
    } catch (const ScriptError& error) { return {nullptr, {error.Detail()}}; }
}
struct Runtime::Impl {
    struct Object {
        ObjectRef transformOwner;
        ObjectRef physicsOwner;
        std::string prefabAsset;
        const Class* type = nullptr;
        std::vector<Value> fields;
        std::map<std::string, std::vector<CallableRef>> connections;
        enum StartState { Pending, Starting, Started, StartFailed } start = Pending;
        bool failed = false;
        bool alive = true;
    };
    std::shared_ptr<const Program::Impl> program;
    RuntimeLimits limits;
    std::uint64_t identity;
    std::vector<std::unique_ptr<Object>> objects;
    std::vector<std::vector<Value>> arrays;
    std::size_t arrayElements = 0;
    std::size_t remaining = 0, depth = 0;
    std::size_t connectionCount = 0;
    ObjectRef inputService;
    ObjectRef physicsService;
    ObjectRef mathService;
    ObjectRef mouseService;
    InputFrame inputFrame;
    MouseFrame mouseFrame;
    bool mouseSeen = false;
    Vector2 mousePrevious;
    std::map<std::string,ObjectRef> inputActions;
    struct TimerRecord { ObjectRef object; double remaining = 0; };
    std::vector<TimerRecord> timers;
    std::function<ObjectRef(std::string_view)> objectLookup;
    std::function<ObjectRef(std::string_view)> typeLookup;              // find_by_type
    std::function<std::vector<std::string>(ObjectRef)> tagLookup;       // get_tags / has_tag
    Runtime::PhysicsBodyCall physicsBodyCall;
    Runtime::PhysicsCastCall physicsCastCall;
    Runtime::PrefabSpawnCall prefabSpawnCall;
    Runtime::PrintCallback printCallback;
    static std::uint64_t NextIdentity() { static std::atomic<std::uint64_t> next{1}; return next.fetch_add(1); }
    Impl(std::shared_ptr<const Program::Impl> p, RuntimeLimits l) : program(std::move(p)), limits(l), identity(NextIdentity()) {}
    [[noreturn]] void Error(const Token& t, const std::string& message) const { Fail(program->source, t, message); }
    void Reset() { remaining = limits.instructionsPerCall; }
    void Tick(const Token& t) { if (remaining == 0) Error(t, "Instruction budget exceeded"); --remaining; }
    struct Frame {
        Impl& vm;
        Frame(Impl& v, const Token& t) : vm(v) { if (vm.depth >= vm.limits.callDepth) vm.Error(t, "Call depth limit exceeded"); ++vm.depth; }
        ~Frame() { --vm.depth; }
    };
    Object& Resolve(ObjectRef ref, const Token& t = {}) const {
        if (ref.id == 0) Error(t, "Null object reference");
        if (ref.runtime != identity || ref.id > objects.size()) Error(t, "Object belongs to another runtime or is invalid");
        auto& object = *objects[ref.id - 1];
        if (!object.alive) Error(t, "Object has been deleted");
        if (object.failed) Error(t, "Object initialization failed");
        return object;
    }
    ObjectRef Reference(const Value& value, const Token& t) const {
        const auto* ref = std::get_if<ObjectRef>(&value);
        if (!ref) Error(t, "Expected object reference");
        Resolve(*ref, t); return *ref;
    }
    std::size_t ArrayId(const Value& value, const Token& t) const {
        auto ref = std::get_if<ArrayRef>(&value);
        if (!ref || ref->runtime != identity || !ref->id || ref->id > arrays.size()) Error(t, "Invalid array or array belongs to another runtime");
        return ref->id - 1;
    }
    std::size_t Index(const Value& value, std::size_t size, const Token& t) const {
        auto index = std::get_if<std::int64_t>(&value);
        if (!index || *index < 0 || static_cast<std::uint64_t>(*index) >= size) Error(t, "Index must be an in-range integer");
        return static_cast<std::size_t>(*index);
    }
    ArrayRef MakeArray(std::vector<Value> values, const Token& t) {
        if (arrays.size() >= limits.arrays || values.size() > limits.arrayElements - arrayElements) Error(t, "Array allocation limit exceeded");
        for (const auto& value : values) Coerce(value, Type(value, t), t);
        arrayElements += values.size(); arrays.push_back(std::move(values));
        return {identity, arrays.size()};
    }
    std::string Type(const Value& value, const Token& t) const {
        switch (value.index()) {
        case 0: return "void"; case 1: return "int"; case 2: return "float";
        case 3: return "bool"; case 4: return "string"; case 6: return "Vector3";
        case 7: return "signal"; case 8: return "callable";
        case 9: ArrayId(value, t); return "array";
        case 10: return "char"; case 11:return "prefab"; case 12: return "Vector2";
        default: { auto ref = std::get<ObjectRef>(value); if (ref.id == 0 && ref.runtime == 0) return "null"; return Resolve(ref, t).type->name; }
        }
    }
    Value Coerce(Value value, const std::string& type, const Token& t) const {
        const auto from = Type(value, t);
        if (!program->Assignable(type, from)) Error(t, "Cannot assign '" + from + "' to '" + type + "'");
        if (type == "float" && from == "int") value = static_cast<double>(std::get<std::int64_t>(value));
        if(type=="string" && from=="char")value=text::Encode(std::get<char32_t>(value));
        if(auto c=std::get_if<char32_t>(&value);c && !text::Scalar(*c))Error(t,"Invalid Unicode character");
        if (auto v = std::get_if<double>(&value); v && !std::isfinite(*v)) Error(t, "Non-finite float");
        if (auto v = std::get_if<Vector3>(&value); v && (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z))) Error(t, "Non-finite Vector3");
        if (auto v = std::get_if<Vector2>(&value); v && (!std::isfinite(v->x) || !std::isfinite(v->y))) Error(t, "Non-finite Vector2");
        if(auto s=std::get_if<std::string>(&value)){
            if(s->size()>limits.stringBytes)Error(t,"String size limit exceeded");
            try{std::size_t at=0;while(at<s->size())text::Next(*s,at);}catch(const std::exception& error){Error(t,error.what());}
        }
        return value;
    }
    Value Global(ObjectRef transform,const std::string& name,const Token& t) const {
        auto matrix=world::Identity(),rotation=world::Identity();unsigned ancestors=0;
        while(transform.id){
            const auto p=std::get<Vector3>(Get(transform,"position",t)),r=std::get<Vector3>(Get(transform,"rotation",t)),s=std::get<Vector3>(Get(transform,"scale",t));
            matrix=world::Multiply(matrix,world::Local(p,r,s));rotation=world::Multiply(rotation,world::Rotation(r));
            const auto owner=Resolve(transform,t).transformOwner;
            const auto parent=owner.id?std::get<ObjectRef>(Get(owner,"parent",t)):ObjectRef{};
            if(parent.id && ++ancestors>64)Error(t,"Global transform hierarchy exceeds 64 levels");
            transform=parent.id?std::get<ObjectRef>(Get(parent,"transform",t)):ObjectRef{};
        }
        Vector3 result;
        if(name=="global_position")result={matrix[3][0],matrix[3][1],matrix[3][2]};else if(name=="global_rotation")result=world::Euler(rotation);else if(name=="global_scale")result=world::Scale(matrix);
        else {const int row=name=="right"?0:name=="up"?1:2;result={rotation[row][0],rotation[row][1],rotation[row][2]};}
        return Coerce(result,"Vector3",t);
    }
    Vector3 DirectionRotation(ObjectRef transform, const std::string& name, Vector3 direction, const Token& t) const {
        const auto length=std::hypot(direction.x,std::hypot(direction.y,direction.z));
        if(length<=1e-12 || !std::isfinite(length)) Error(t,"Transform direction must be non-zero");
        direction={direction.x/length,direction.y/length,direction.z/length};
        Vector3 up={0,1,0};
        if(std::abs(direction.y)>0.999) up={1,0,0};
        auto cross=[](Vector3 a,Vector3 b){return Vector3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
        auto normalize=[&](Vector3 v){const auto n=std::hypot(v.x,std::hypot(v.y,v.z));return Vector3{v.x/n,v.y/n,v.z/n};};
        Vector3 right,forward;
        if(name=="forward") { forward=direction; right=normalize(cross(up,forward)); up=normalize(cross(forward,right)); }
        else { right=direction; forward=normalize(cross(right,up)); up=normalize(cross(forward,right)); }
        auto matrix=world::Identity();
        matrix[0][0]=right.x;matrix[0][1]=right.y;matrix[0][2]=right.z;
        matrix[1][0]=up.x;matrix[1][1]=up.y;matrix[1][2]=up.z;
        matrix[2][0]=forward.x;matrix[2][1]=forward.y;matrix[2][2]=forward.z;
        return world::Euler(matrix);
    }
    Value Get(ObjectRef ref, const std::string& name, const Token& t = {}) const {
        auto& o = Resolve(ref, t); std::size_t index = 0;
        if(program->Assignable("Transform",o.type->name) && (GlobalField(name)||DirectionField(name)))return Global(ref,name,t);
        if(program->Assignable("PhysicsBody",o.type->name) && (name=="velocity"||name=="angular_velocity") && physicsBodyCall)return Coerce(physicsBodyCall(o.physicsOwner,name=="velocity"?"get_velocity":"get_angular_velocity",{}),"Vector3",t);
        if (o.type->signals.contains(name)) return SignalRef{ref, name};
        if (program->Method(o.type->name, name)) return CallableRef{ref, name};
        if (!program->FindField(o.type->name, name, &index)) Error(t, "Unknown field '" + name + "'");
        return o.fields[index];
    }
    void Set(ObjectRef ref, const std::string& name, Value value, const Token& t = {}, bool notify = true) {
        auto& o = Resolve(ref, t); std::size_t index = 0; std::string fieldName=name;
        auto field = program->FindField(o.type->name, fieldName, &index);
        if (!field) Error(t, "Unknown field '" + name + "'");
        if(o.type->name=="InputAction" || o.type->name=="Mouse")Error(t,"Input state is read-only");
        if(program->Assignable("Transform",o.type->name) && GlobalField(fieldName))Error(t,"Global transform fields are read-only");
        if(program->Assignable("Transform",o.type->name) && DirectionField(fieldName)) {
            value=Coerce(std::move(value),"Vector3",t);
            const auto rotation=DirectionRotation(ref,fieldName,std::get<Vector3>(value),t);
            auto rotationIndex=std::size_t{}; program->FindField(o.type->name,"rotation",&rotationIndex); value=rotation; fieldName="rotation"; index=rotationIndex;
        }
        value = Coerce(std::move(value), field->type, t);
        if(program->Assignable("PhysicsBody",o.type->name) && (name=="velocity"||name=="angular_velocity") && physicsBodyCall){physicsBodyCall(o.physicsOwner,name=="velocity"?"set_velocity":"set_angular_velocity",{value});}
        if(fieldName=="transform" && program->IsGameObjectLike(o.type->name)){
            const auto next=std::get<ObjectRef>(value),previous=std::get<ObjectRef>(o.fields[index]);
            if(next.id){auto& target=Resolve(next,t);if(target.transformOwner.id && target.transformOwner!=ref)Error(t,"A Transform cannot belong to two GameObjects");target.transformOwner=ref;}
            if(previous.id && previous!=next)Resolve(previous,t).transformOwner={};
        }
        if(fieldName=="parent" && program->IsGameObjectLike(o.type->name)) {
            auto parent=std::get<ObjectRef>(value);unsigned parentDepth=0;
            while(parent.id){if(parent==ref || ++parentDepth>64)Error(t,"Parenting would create a cycle or exceed 64 levels");parent=std::get<ObjectRef>(Get(parent,"parent",t));}
        }
        const bool changed = o.fields[index] != value;
        o.fields[index] = value;
        if (notify && changed && program->Assignable("Transform", o.type->name)) {
            const std::string signal = name == "position" ? "was_moved" : name == "rotation" ? "was_rotated" : name == "scale" ? "was_scaled" : "";
            if (!signal.empty()) Signal({ref, signal}, "emit", {value}, t);
        }
    }
    Value Signal(const SignalRef& signal, const std::string& method, const std::vector<Value>& args, const Token& t) {
        auto& owner = Resolve(signal.owner, t);
        if (!owner.type->signals.contains(signal.name)) Error(t, "Unknown signal '" + signal.name + "'");
        auto& connections = owner.connections[signal.name];
        if (method == "emit") {
            Frame frame(*this, t); Tick(t);
            if (args.size() > 64) Error(t, "Signal argument limit exceeded");
            if (program->Assignable("Transform", owner.type->name) &&
                (signal.name == "was_moved" || signal.name == "was_rotated" || signal.name == "was_scaled")) {
                if (args.size() != 1) Error(t, "Transform signals require one Vector3 argument");
                Coerce(args[0], "Vector3", t);
            }
            if(program->Assignable("PhysicsBody",owner.type->name)){if(args.size()!=1)Error(t,"Physics signals require one GameObject argument");Coerce(args[0],"gameObject",t);}
            // Snapshot permits safe connect/disconnect during a callback. New listeners wait until next emission.
            const auto snapshot = connections;
            // Validate every recipient before invoking any, avoiding partial dispatch on signature mistakes.
            for (const auto& callback : snapshot) {
                const auto* f = program->Method(Resolve(callback.owner, t).type->name, callback.name);
                if (!f || f->params.size() != args.size()) Error(t, "Signal callback argument count mismatch");
                for (std::size_t i = 0; i < args.size(); ++i) Coerce(args[i], f->params[i], t);
            }
            for (const auto& callback : snapshot) {
                Tick(t);
                if (std::find(connections.begin(), connections.end(), callback) != connections.end()) Invoke(callback.owner, callback.name, args, t);
            }
            return {};
        }
        if (args.size() != 1 || !std::holds_alternative<CallableRef>(args[0])) Error(t, "Expected one function reference");
        const auto callback = std::get<CallableRef>(args[0]);
        const auto* f = program->Method(Resolve(callback.owner, t).type->name, callback.name);
        if (!f || f->result != "void") Error(t, "Signal callback must be a void function");
        if(owner.type->name=="InputAction" && !f->params.empty())Error(t,"Input signal callbacks take no arguments");
        if(owner.type->name=="Mouse") {
            if((signal.name=="clicked"||signal.name=="click_ended"||signal.name=="held") && f->params!=std::vector<std::string>{"int"})Error(t,"Mouse click signal callback must take one int (button index)");
            if(signal.name=="was_just_moved" && f->params!=std::vector<std::string>{"Vector2","Vector2"})Error(t,"Mouse was_just_moved callback must take (Vector2 old, Vector2 new)");
        }
        if(program->Assignable("PhysicsBody",owner.type->name) && f->params!=std::vector<std::string>{"gameObject"})Error(t,"Physics signal callback must take one gameObject");
        if (program->Assignable("Transform", owner.type->name) &&
            (signal.name == "was_moved" || signal.name == "was_rotated" || signal.name == "was_scaled") && f->params != std::vector<std::string>{"Vector3"})
            Error(t, "Transform signal callback must take one Vector3");
        const auto found = std::find(connections.begin(), connections.end(), callback);
        if (method == "is_connected") return found != connections.end();
        if (method == "disconnect") {
            if (found != connections.end()) { connections.erase(found); --connectionCount; }
        } else if (method == "connect") {
            if (found == connections.end()) {
                if (connectionCount >= limits.signalConnections) Error(t, "Signal connection limit exceeded");
                connections.push_back(callback); ++connectionCount;
            }
        } else Error(t, "Unknown signal method");
        return {};
    }
    ObjectRef Create(const std::string& name, const Token& t) {
        auto found = program->classes.find(name);
        if (found == program->classes.end()) Error(t, "Unknown class '" + name + "'");
        Frame frame(*this, t); Tick(t);
        if (objects.size() >= limits.objects) Error(t, "Object limit exceeded");
        auto object = std::make_unique<Object>(); object->type = &found->second;
        for (const auto& field : object->type->fields) object->fields.push_back(field.type == "array" ? Value{MakeArray({},t)} : DefaultValue(field.type));
        ObjectRef ref{identity, objects.size() + 1}; objects.push_back(std::move(object));
        std::vector<const Class*> bases;
        const Class* c = &found->second;
        while (c) { bases.push_back(c); c = c->base.empty() ? nullptr : &program->classes.at(c->base); }
        try {
                        if (program->Assignable("Transform", name)) Set(ref, "scale", Vector3{1, 1, 1}, t);
            if (program->Assignable("gameObject", name)) {
                const auto transform=Create("Transform",t);Resolve(transform,t).transformOwner=ref;Set(ref,"transform",transform,t);
                if(program->Assignable("PhysicsBody",name)){
                    Resolve(ref,t).physicsOwner=ref;Set(ref,"physics",ref,t);
                    for(const auto& native:NativeTypes())
                        if(native.physicsBody && program->Assignable(std::string(native.name),name)){Set(ref,std::string(native.accessor),ref,t);break;}
                } else {
                    const auto physics=Create("PhysicsBody",t);Resolve(physics,t).physicsOwner=ref;Set(ref,"physics",physics,t);
                    for(const auto& native:NativeTypes())
                        if(native.component && !native.physicsBody && program->Assignable(std::string(native.name),name)){Set(ref,std::string(native.accessor),ref,t);break;}
                }
            }
            else if (program->Assignable("gameObject2D", name)) {
                const auto transform=Create("Transform2D",t);Resolve(transform,t).transformOwner=ref;Set(ref,"transform",transform,t);
            }
            for (auto it = bases.rbegin(); it != bases.rend(); ++it) Execute(ref, (*it)->initializer, {}, t);
        } catch (...) { objects[ref.id - 1]->failed = true; throw; }
        return ref;
    }
    void BindNativeBehavior(ObjectRef ownerRef,const std::string& type,const Token& t={}) {
        auto& owner=Resolve(ownerRef,t);if(!program->Assignable("gameObject",owner.type->name))Error(t,"Native behaviors require a gameObject owner");
        const auto* native=FindNativeType(type);
        if(!native||!native->component)Error(t,"Unknown native behavior type '"+type+"'");
        ObjectRef behavior=program->Assignable(type,owner.type->name)?ownerRef:Create(type,t);
        if(native->physicsBody){Resolve(behavior,t).physicsOwner=ownerRef;Set(ownerRef,"physics",behavior,t,false);}
        Set(ownerRef,std::string(native->accessor),behavior,t,false);
    }
    static double Number(const Value& v) { if (auto i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i); return std::get<double>(v); }
    static bool IsText(const Value& value){return std::holds_alternative<std::string>(value) || std::holds_alternative<char32_t>(value);}
    static std::string Text(const Value& value){return std::holds_alternative<char32_t>(value)?text::Encode(std::get<char32_t>(value)):std::get<std::string>(value);}
    std::vector<std::size_t> TextOffsets(const std::string& value,const Token& t) const {
        if(value.size()>limits.stringBytes)Error(t,"String size limit exceeded");
        try{return text::Offsets(value);}catch(const std::exception& error){Error(t,error.what());}
    }
    Value TextOperation(const Value& receiver,const std::string& method,const std::vector<Value>& args,const Token& t) const {
        const auto* value=std::get_if<std::string>(&receiver);if(!value)Error(t,"String method requires a string");
        const auto offsets=TextOffsets(*value,t);const auto size=offsets.size()-1;
        if(method=="size" && args.empty())return static_cast<std::int64_t>(size);
        if((method!="truncate" && method!="substr") || args.size()!=(method=="truncate"?1:2))Error(t,"Invalid string method or arguments");
        const auto count=[&](const Value& argument){const auto* n=std::get_if<std::int64_t>(&argument);if(!n || *n<0)Error(t,"String positions and lengths must be nonnegative integers");return static_cast<std::uint64_t>(*n);};
        if(method=="truncate")return value->substr(0,offsets[std::min<std::uint64_t>(count(args[0]),size)]);
        const auto start=count(args[0]);if(start>size)Error(t,"String start is out of range");const auto length=std::min<std::uint64_t>(count(args[1]),size-start);
        return value->substr(offsets[start],offsets[start+length]-offsets[start]);
    }
    Value Arithmetic(Op op, Value left, Value right, const Token& t) const {
        if((op==Op::Add || op==Op::Concat) && IsText(left) && IsText(right)){
            auto a=Text(left);const auto b=Text(right);
            if(a.size()>limits.stringBytes || b.size()>limits.stringBytes-a.size())Error(t,"String size limit exceeded");a+=b;return a;
        }
        if(op==Op::Concat)Error(t,"Concatenation operands must be strings or characters");
        const auto aType = Type(left,t), bType = Type(right,t);
        const auto vectorOk=[&](const char* v){
            return ((op==Op::Add || op==Op::Subtract || op==Op::Multiply) && aType==v && bType==v)
                || ((op==Op::Multiply || op==Op::Divide) && aType==v && Numeric(bType))
                || (op==Op::Multiply && Numeric(aType) && bType==v);
        };
        const bool valid = (Numeric(aType) && Numeric(bType)) ||
            (op == Op::Add && aType == "string" && bType == "string") || vectorOk("Vector3") || vectorOk("Vector2");
        if (!valid) Error(t, "Incompatible arithmetic operands");
        // Component-wise op for Vector3 or Vector2 (validity already checked above).
        auto vectorOp=[&](auto a,auto b,bool aVec,bool bVec)->Value{
            using V=decltype(a);
            auto comp=[&](double l,double r)->double{ switch(op){case Op::Add:return l+r;case Op::Subtract:return l-r;default:return l*r;} };
            V out{};
            if(aVec && bVec){out.x=comp(a.x,b.x);out.y=comp(a.y,b.y);if constexpr(std::is_same_v<V,Vector3>)out.z=comp(a.z,b.z);}
            else if(aVec){ double s=Number(right); if(op==Op::Divide&&s==0)Error(t,"Division by zero");
                out.x=op==Op::Divide?a.x/s:a.x*s;out.y=op==Op::Divide?a.y/s:a.y*s;if constexpr(std::is_same_v<V,Vector3>)out.z=op==Op::Divide?a.z/s:a.z*s;}
            else { double s=Number(left); out.x=s*b.x;out.y=s*b.y;if constexpr(std::is_same_v<V,Vector3>)out.z=s*b.z;}
            return Coerce(out, std::is_same_v<V,Vector3>?"Vector3":"Vector2", t);
        };
        if (std::holds_alternative<Vector3>(left) || std::holds_alternative<Vector3>(right)) {
            const auto* a=std::get_if<Vector3>(&left); const auto* b=std::get_if<Vector3>(&right);
            return vectorOp(a?*a:Vector3{}, b?*b:Vector3{}, a!=nullptr, b!=nullptr);
        }
        if (std::holds_alternative<Vector2>(left) || std::holds_alternative<Vector2>(right)) {
            const auto* a=std::get_if<Vector2>(&left); const auto* b=std::get_if<Vector2>(&right);
            return vectorOp(a?*a:Vector2{}, b?*b:Vector2{}, a!=nullptr, b!=nullptr);
        }
        if (op == Op::Add && std::holds_alternative<std::string>(left)) {
            auto a = std::get<std::string>(std::move(left)); const auto& b = std::get<std::string>(right);
            if (a.size() > limits.stringBytes || b.size() > limits.stringBytes - a.size()) Error(t, "String size limit exceeded");
            a += b; return a;
        }
        if (std::holds_alternative<std::int64_t>(left) && std::holds_alternative<std::int64_t>(right)) {
            const auto a = std::get<std::int64_t>(left), b = std::get<std::int64_t>(right);
            constexpr auto lo = std::numeric_limits<std::int64_t>::min(), hi = std::numeric_limits<std::int64_t>::max();
            if (op == Op::Add) {
                if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) Error(t, "Integer overflow");
                return a + b;
            }
            if (op == Op::Subtract) {
                if ((b < 0 && a > hi + b) || (b > 0 && a < lo + b)) Error(t, "Integer overflow");
                return a - b;
            }
            if (op == Op::Multiply) {
                if ((a > 0 && ((b > 0 && a > hi / b) || (b < 0 && b < lo / a))) ||
                    (a < 0 && ((b > 0 && a < lo / b) || (b < 0 && a < hi / b)))) Error(t, "Integer overflow");
                return a * b;
            }
            if (b == 0) Error(t, "Division by zero");
            if (a == lo && b == -1) Error(t, "Integer overflow");
            return a / b;
        }
        double a = Number(left), b = Number(right), result = 0;
        switch (op) {
        case Op::Add: result = a + b; break;
        case Op::Subtract: result = a - b; break;
        case Op::Multiply: result = a * b; break;
        default: if (b == 0) Error(t, "Division by zero"); result = a / b; break;
        }
        if (!std::isfinite(result)) Error(t, "Non-finite arithmetic result");
        return result;
    }
    Value Compare(Op op, const Value& a, const Value& b, const Token& t) const {
        if(IsText(a) && IsText(b)){
            const auto left=Text(a),right=Text(b);
            switch(op){case Op::Equal:return left==right;case Op::NotEqual:return left!=right;case Op::Less:return left<right;case Op::LessEqual:return left<=right;case Op::Greater:return left>right;default:return left>=right;}
        }
        auto numeric = [](const Value& v) { return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v); };
        if (op == Op::Equal || op == Op::NotEqual) {
            bool equal = a.index() == b.index() ? a == b : (numeric(a) && numeric(b) && Number(a) == Number(b));
            return op == Op::Equal ? equal : !equal;
        }
        auto compare = [op](auto left, auto right) {
            switch (op) { case Op::Less: return left < right; case Op::LessEqual: return left <= right; case Op::Greater: return left > right; default: return left >= right; }
        };
        if (!numeric(a) || !numeric(b)) Error(t, "Comparison operands must be numeric");
        if (std::holds_alternative<std::int64_t>(a) && std::holds_alternative<std::int64_t>(b)) return compare(std::get<std::int64_t>(a), std::get<std::int64_t>(b));
        return compare(Number(a), Number(b));
    }
    // True when `type` resolves `name` to the built-in gameObject/gameObject2D method
    // rather than a user override (so the host special-case should run).
    bool BuiltinObjectMethod(const std::string& type, const std::string& name) const {
        const auto* m = program->Method(type, name);
        if (!m) return false;
        for (const char* base : {"gameObject", "gameObject2D"}) {
            const auto& methods = program->classes.at(base).methods;
            if (const auto it = methods.find(name); it != methods.end() && m == &it->second) return true;
        }
        return false;
    }
    Value Invoke(ObjectRef ref, const std::string& name, const std::vector<Value>& args, const Token& t) {
        const auto& object = Resolve(ref, t);
        if(name=="print" && BuiltinObjectMethod(object.type->name,"print")) {
            if(args.size()!=1)Error(t,"print takes one string");
            const auto text=std::get<std::string>(Coerce(args[0],"string",t));if(printCallback)printCallback(text);return {};
        }
        if(name=="make_timer" && BuiltinObjectMethod(object.type->name,"make_timer")) {
            if(args.size()!=1)Error(t,"make_timer takes a duration in seconds");
            const auto duration=Number(Coerce(args[0],"float",t));if(duration<0)Error(t,"Timer duration must be nonnegative");
            const auto timer=Create("Timer",t);timers.push_back({timer,duration});return timer;
        }
        if(object.type->name=="prefab") {if(name!="spawn"||!args.empty())Error(t,"prefab.spawn takes no arguments");if(object.prefabAsset.empty())Error(t,"Assign a prefab asset before calling spawn");if(!prefabSpawnCall)Error(t,"Prefab spawning is not available in this runtime");return prefabSpawnCall(object.prefabAsset);}
        if(program->Assignable("PhysicsBody",object.type->name) && (name=="add_force"||name=="add_impulse"||name=="add_torque"||name=="add_angular_impulse")) {
            if(!physicsBodyCall)Error(t,"Physics is not available in this runtime");
            if(args.size()!=1)Error(t,"Physics body force methods take one Vector3");
            return physicsBodyCall(object.physicsOwner,name,{Coerce(args[0],"Vector3",t)});
        }
        if(object.type->name=="PhysicsService") {
            if(!physicsCastCall)Error(t,"Physics is not available in this runtime");
            if(args.size()!=3)Error(t,"Physics casts take from, to, and a layer mask");
            const auto from=std::get<Vector3>(Coerce(args[0],"Vector3",t)),to=std::get<Vector3>(Coerce(args[1],"Vector3",t));const auto mask=static_cast<std::uint32_t>(std::get<std::int64_t>(Coerce(args[2],"int",t)));
            const auto hits=physicsCastCall(from,to,mask);if(name=="cast")return hits.empty()?Value{ObjectRef{}}:Value{hits.front()};
            std::vector<Value> values;values.reserve(hits.size());for(auto hit:hits)values.push_back(hit);return MakeArray(std::move(values),t);
        }
        if(object.type->name=="Mathf") {
            auto number=[&](std::size_t index){return Number(Coerce(args[index],"float",t));};
            if(name=="lerp") {
                if(args.size()!=3)Error(t,"Mathf.lerp takes from, to, and weight");
                const auto from=number(0),to=number(1),weight=number(2),result=from+(to-from)*weight;
                if(!std::isfinite(result))Error(t,"Mathf produced a non-finite result");return result;
            }
            if(name=="dot" || name=="cross") {
                if(args.size()!=2)Error(t,"Mathf vector methods take two Vector3 values");
                const auto a=std::get<Vector3>(Coerce(args[0],"Vector3",t)),b=std::get<Vector3>(Coerce(args[1],"Vector3",t));
                if(name=="dot")return a.x*b.x+a.y*b.y+a.z*b.z;
                return Coerce(Vector3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x},"Vector3",t);
            }
            if(args.size()!=1)Error(t,"Mathf unary methods take one number");
            const auto value=number(0);double result=0;
            if(name=="sin")result=std::sin(value);else if(name=="cos")result=std::cos(value);else if(name=="tan")result=std::tan(value);
            else if(name=="sqrt"){if(value<0)Error(t,"Mathf.sqrt requires a nonnegative value");result=std::sqrt(value);}
            else if(name=="exp")result=std::exp(value);else if(name=="round")result=std::round(value);else Error(t,"Unknown Mathf method");
            if(!std::isfinite(result))Error(t,"Mathf produced a non-finite result");return result;
        }
        if(name=="find" && BuiltinObjectMethod(object.type->name,"find")) {
            Tick(t);if(args.size()!=1)Error(t,"find takes one scene object name");
            const auto requested=std::get<std::string>(Coerce(args[0],"string",t));
            return Coerce(objectLookup?Value{objectLookup(requested)}:Value{ObjectRef{}},"gameObject",t);
        }
        if((name=="get_tags"||name=="has_tag") && BuiltinObjectMethod(object.type->name,name)) {
            Tick(t);
            const std::vector<std::string> tags = tagLookup ? tagLookup(ref) : std::vector<std::string>{};
            if(name=="has_tag") {
                if(args.size()!=1)Error(t,"has_tag takes one tag string");
                const auto want=std::get<std::string>(Coerce(args[0],"string",t));
                return std::any_of(tags.begin(),tags.end(),[&](const auto& s){return s==want;});
            }
            if(!args.empty())Error(t,"get_tags takes no arguments");
            std::vector<Value> values;values.reserve(tags.size());
            for(const auto& s:tags)values.push_back(s);
            return MakeArray(std::move(values),t);
        }
        if(object.type->name=="InputService") {
            if(args.size()!=1)Error(t,"Input calls take one action name");
            const auto action=std::get<std::string>(Coerce(args[0],"string",t));const auto found=inputFrame.find(action);
            const InputState empty;const auto& state=found==inputFrame.end()?empty:found->second;
            if(name=="action") {
                if(found==inputFrame.end())Error(t,"Unknown input action '"+action+"'");
                return inputActions.at(action);
            }
            if(name=="is_action_pressed")return state.pressed;
            if(name=="is_action_just_pressed")return state.justPressed;
            if(name=="is_action_just_released")return state.justReleased;
            if(name=="get_axis")return state.x;
            if(name=="get_vector")return Vector2{state.x,state.y};
            Error(t,"Unknown Input method");
        }
        const auto* function = program->Method(object.type->name, name);
        if (!function) Error(t, "Unknown method '" + name + "'");
        return Execute(ref, *function, args, t);
    }
    Value Execute(ObjectRef ref, const Function& f, const std::vector<Value>& args, const Token& caller) {
        if (args.size() != f.params.size()) Error(caller, "Wrong argument count");
        std::vector<Value> locals;
        locals.reserve(f.locals.size());
        for (std::size_t i = 0; i < args.size(); ++i) locals.push_back(Coerce(args[i], f.params[i], caller));
        // Empty functions retain only a signature, with no frame or instruction dispatch.
        if (f.code.empty()) return {};
        Frame frame(*this, caller);
        for (std::size_t i = args.size(); i < f.locals.size(); ++i) locals.push_back(DefaultValue(f.locals[i]));
        std::vector<Value> stack;
        auto pop = [&]() { Value value = std::move(stack.back()); stack.pop_back(); return value; };
        for (std::size_t pc = 0; pc < f.code.size();) {
            const auto& ins = f.code[pc++]; const auto& t = ins.token; Tick(t);
            switch (ins.op) {
            case Op::Constant:
                if (auto s = std::get_if<std::string>(&ins.value); s && s->size() > limits.stringBytes) Error(t, "String size limit exceeded");
                stack.push_back(ins.value); break;
                        case Op::Duplicate: stack.push_back(stack.back()); break;
            case Op::DuplicatePair: { auto a = stack[stack.size()-2], b = stack.back(); stack.push_back(a); stack.push_back(b); break; }
            case Op::MakeArray: {
                std::vector<Value> values(ins.a);
                for (std::size_t i = ins.a; i > 0; --i) values[i-1] = pop();
                stack.push_back(MakeArray(std::move(values),t)); break;
            }
            case Op::ArrayGet: {
                auto index = pop(),receiver=pop();
                if(const auto* string=std::get_if<std::string>(&receiver)){const auto offsets=TextOffsets(*string,t);auto at=offsets[Index(index,offsets.size()-1,t)];stack.push_back(text::Next(*string,at));break;}
                const auto& items = arrays[ArrayId(receiver,t)];
                stack.push_back(items[Index(index,items.size(),t)]); break;
            }
            case Op::ArraySet: {
                auto value = pop(), index = pop(); auto& items = arrays[ArrayId(pop(),t)];
                items[Index(index,items.size(),t)] = Coerce(value,Type(value,t),t); break;
            }
            case Op::ArrayCall: {
                Value argument; if (ins.a) argument = pop();auto receiver=pop();
                if(std::holds_alternative<std::string>(receiver)){stack.push_back(TextOperation(receiver,ins.name,ins.a?std::vector<Value>{argument}:std::vector<Value>{},t));break;}
                auto& items = arrays[ArrayId(receiver,t)];
                if (ins.name == "size") stack.push_back(static_cast<std::int64_t>(items.size()));
                else {
                    if (ins.name == "append") {
                        if (arrayElements >= limits.arrayElements) Error(t,"Array element limit exceeded");
                        items.push_back(Coerce(argument,Type(argument,t),t)); ++arrayElements;
                    } else { items.erase(items.begin()+Index(argument,items.size(),t)); --arrayElements; }
                    stack.push_back({});
                }
                break;
            }
            case Op::TextCall: {
                std::vector<Value> arguments(ins.a);for(std::size_t i=ins.a;i>0;--i)arguments[i-1]=pop();
                stack.push_back(TextOperation(pop(),ins.name,arguments,t));break;
            }
            case Op::IsType: {
                const auto actual = Type(pop(),t);
                stack.push_back(actual == ins.name || (actual != "null" && program->classes.contains(ins.name) && program->Assignable(ins.name,actual))); break;
            }
            case Op::FindType: {
                Tick(t); pop(); // discard the implicit receiver
                stack.push_back(Coerce(typeLookup ? Value{typeLookup(ins.name)} : Value{ObjectRef{}}, "gameObject", t)); break;
            }
            case Op::MakeVector: {
                if (ins.name == "Vector2") {
                    Vector2 v; if (ins.a == 2) { v.y = Number(pop()); v.x = Number(pop()); }
                    stack.push_back(Coerce(v, "Vector2", t)); break;
                }
                Vector3 v; if (ins.a == 3) { v.z = Number(pop()); v.y = Number(pop()); v.x = Number(pop()); }
                stack.push_back(Coerce(v, "Vector3", t)); break;
            }
            case Op::SetComponent: {
                double component = Number(pop()); auto value = pop();
                if (auto* v = std::get_if<Vector2>(&value)) {
                    if (ins.name == "x") v->x = component; else v->y = component;
                    stack.push_back(Coerce(*v, "Vector2", t)); break;
                }
                auto v = std::get<Vector3>(value);
                if (ins.name == "x") v.x = component; else if (ins.name == "y") v.y = component; else v.z = component;
                stack.push_back(Coerce(v, "Vector3", t)); break;
            }
            case Op::Self: stack.push_back(ref); break;
            case Op::Input:
                if(!inputService.id) {
                    inputService=Create("InputService",t);
                    mouseService=Create("Mouse",t);
                    std::size_t mi=0;program->FindField("InputService","mouse",&mi);Resolve(inputService).fields[mi]=mouseService;
                    ApplyMouseFields();
                }
                stack.push_back(inputService);break;
            case Op::Physics: if(!physicsService.id)physicsService=Create("PhysicsService",t);stack.push_back(physicsService);break;
            case Op::Math: if(!mathService.id)mathService=Create("Mathf",t);stack.push_back(mathService);break;
            case Op::LoadLocal: stack.push_back(locals[ins.a]); break;
            case Op::StoreLocal: locals[ins.a] = Coerce(pop(), f.locals[ins.a], t); break;
                        case Op::LoadField: {
                auto value = pop();
                if (auto v = std::get_if<Vector3>(&value)) stack.push_back(ins.name == "x" ? v->x : ins.name == "y" ? v->y : v->z);
                else if (auto v = std::get_if<Vector2>(&value)) stack.push_back(ins.name == "x" ? v->x : v->y);
                else stack.push_back(Get(Reference(value, t), ins.name, t));
                break;
            }
            case Op::StoreField: { auto value = pop(); auto target = Reference(pop(), t); Set(target, ins.name, std::move(value), t); break; }
            case Op::Call: {
                std::vector<Value> arguments(ins.a);
                for (std::size_t i = ins.a; i > 0; --i) arguments[i - 1] = pop();
                auto target = Reference(pop(), t); stack.push_back(Invoke(target, ins.name, arguments, t)); break;
            }
            case Op::SignalCall: {
                std::vector<Value> arguments(ins.a);
                for (std::size_t i = ins.a; i > 0; --i) arguments[i - 1] = pop();
                const auto signal = std::get<SignalRef>(pop());
                stack.push_back(Signal(signal, ins.name, arguments, t)); break;
            }
            case Op::New: stack.push_back(Create(ins.name, t)); break;
            case Op::Pop: pop(); break;
            case Op::Return: return Coerce(pop(), f.result, t);
            case Op::Negate: {
                auto value = pop();
                if (auto i = std::get_if<std::int64_t>(&value)) {
                    if (*i == std::numeric_limits<std::int64_t>::min()) Error(t, "Integer overflow");
                    stack.push_back(-*i);
                } else if (auto v3 = std::get_if<Vector3>(&value)) stack.push_back(Vector3{-v3->x, -v3->y, -v3->z});
                else if (auto v2 = std::get_if<Vector2>(&value)) stack.push_back(Vector2{-v2->x, -v2->y});
                else if (auto number = std::get_if<double>(&value)) stack.push_back(-*number);
                else Error(t,"Invalid unary operand");
                break;
            }
            case Op::Not: stack.push_back(!std::get<bool>(Coerce(pop(),"bool",t))); break;
            case Op::Jump: pc = ins.a; break;
            case Op::JumpFalse: if (!std::get<bool>(Coerce(pop(),"bool",t))) pc = ins.a; break;
            case Op::Concat: case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide: {
                auto right = pop(), left = pop(); stack.push_back(Arithmetic(ins.op, std::move(left), std::move(right), t)); break;
            }
            case Op::Stringify: {
                const auto value = pop(); std::ostringstream text; text.imbue(std::locale::classic()); text<<std::setprecision(17);
                if (const auto* v=std::get_if<std::int64_t>(&value)) text<<*v;
                else if (const auto* number=std::get_if<double>(&value)) text<<*number;
                else if (const auto* boolean=std::get_if<bool>(&value)) text<<(*boolean?"true":"false");
                else if (const auto* string=std::get_if<std::string>(&value)) text<<*string;
                else if (const auto* character=std::get_if<char32_t>(&value)) text<<script::text::Encode(*character);
                else if (const auto* vector3=std::get_if<Vector3>(&value)) text<<vector3->x<<", "<<vector3->y<<", "<<vector3->z;
                else if (const auto* vector2=std::get_if<Vector2>(&value)) text<<vector2->x<<", "<<vector2->y;
                else Error(t,"Only scalar values can be interpolated");
                stack.push_back(text.str()); break;
            }
            default: { auto right = pop(), left = pop(); stack.push_back(Compare(ins.op, left, right,t)); break; }
            }
        }
        Error(f.token, "Function ended without returning");
    }
    void SetInput(const InputFrame& frame,bool emitEvents) {
        if(frame.size()>256)Error({},"Input action limit exceeded");
        for(const auto& [name,s]:frame)if(name.empty() || name.size()>80 || !std::isfinite(s.x) || !std::isfinite(s.y) || std::abs(s.x)>1 || std::abs(s.y)>1)Error({},"Invalid input state");
        inputFrame=frame;
        for(const auto& [name,s]:frame) {
            if(!inputActions.contains(name))inputActions.emplace(name,Create("InputAction",{}));
            auto& fields=Resolve(inputActions.at(name)).fields;fields[0]=s.pressed;fields[1]=Vector2{s.x,s.y};fields[2]=Vector2{s.x,s.y};
        }
        for(const auto& [name,ref]:inputActions)if(!frame.contains(name)){auto& f=Resolve(ref).fields;f[0]=false;f[1]=Vector2{};f[2]=Vector2{};}
        if(emitEvents)for(const auto& [name,s]:frame) {
            const auto ref=inputActions.at(name);
            if(s.justPressed){Signal({ref,"just_pressed"},"emit",{},{});Signal({ref,"was_just_pressed"},"emit",{},{});}
            if(s.justReleased){Signal({ref,"just_released"},"emit",{},{});Signal({ref,"was_just_released"},"emit",{},{});}
            if(s.pressed)Signal({ref,"is_pressed"},"emit",{},{});
        }
    }
    void ApplyMouseFields() {
        if(!mouseService.id)return;
        auto& fields=Resolve(mouseService).fields;
        const Vector2 position{mouseFrame.x,mouseFrame.y};
        const Vector2 delta=mouseSeen?Vector2{position.x-mousePrevious.x,position.y-mousePrevious.y}:Vector2{};
        fields[0]=delta; fields[1]=position; // Mouse.delta, Mouse.position (declaration order)
    }
    void SetMouse(const MouseFrame& frame,bool emitEvents) {
        if(!std::isfinite(frame.x)||!std::isfinite(frame.y)||std::abs(frame.x)>16||std::abs(frame.y)>16)Error({},"Invalid mouse position");
        const Vector2 position{frame.x,frame.y};
        const bool moved=mouseSeen && (position.x!=mousePrevious.x || position.y!=mousePrevious.y);
        const Vector2 previous=mousePrevious;
        mouseFrame=frame;
        ApplyMouseFields();
        if(emitEvents && mouseService.id) {
            if(moved)Signal({mouseService,"was_just_moved"},"emit",{previous,position},{});
            for(std::size_t i=0;i<frame.buttons.size();++i) {
                const auto& b=frame.buttons[i];const auto index=static_cast<std::int64_t>(i);
                if(b.justPressed)Signal({mouseService,"clicked"},"emit",{index},{});
                if(b.justReleased)Signal({mouseService,"click_ended"},"emit",{index},{});
                if(b.pressed && !b.justPressed)Signal({mouseService,"held"},"emit",{index},{});
            }
        }
        mousePrevious=position; mouseSeen=true;
    }
    void AdvanceTimers(double delta) {
        const auto initial=timers.size();std::vector<ObjectRef> expired;
        for(std::size_t i=0;i<initial;++i)if((timers[i].remaining-=delta)<=0)expired.push_back(timers[i].object);
        for(const auto timer:expired) {
            try { Signal({timer,"finished"},"emit",{},{}); }
            catch (...) { Resolve(timer).alive=false;timers.erase(std::remove_if(timers.begin(),timers.end(),[&](const auto& item){return item.object==timer;}),timers.end());throw; }
            auto& object=Resolve(timer);for(const auto& [_,connections]:object.connections)connectionCount-=connections.size();object.connections.clear();object.alive=false;
            timers.erase(std::remove_if(timers.begin(),timers.end(),[&](const auto& item){return item.object==timer;}),timers.end());
        }
    }
    void Behavior(ObjectRef ref) const {
        const auto& object = Resolve(ref);
        if (!program->IsGameObjectLike(object.type->name)) Error({}, "Lifecycle hooks require a gameObject-derived behavior");
    }
    void Start(ObjectRef ref) {
        Behavior(ref); auto& object = Resolve(ref);
        if (object.start == Object::Started) return;
        if (object.start == Object::StartFailed) Error({}, "Start previously failed");
        if (object.start == Object::Starting) Error({}, "Reentrant start");
        object.start = Object::Starting;
        try {
            if (auto f = program->Method(object.type->name, "start"); f && !f->code.empty()) Execute(ref, *f, {}, f->token);
            object.start = Object::Started;
        } catch (...) { object.start = Object::StartFailed; throw; }
    }
    void Hook(ObjectRef ref, const std::string& name, const std::vector<Value>& args) {
        Start(ref);
        if (auto f = program->Method(Resolve(ref).type->name, name); f && !f->code.empty()) Execute(ref, *f, args, f->token);
    }
};
Runtime::Runtime(std::shared_ptr<const Program> program, RuntimeLimits limits) {
    if (!program) throw std::invalid_argument("Runtime requires a successfully compiled program");
    impl_ = std::make_unique<Impl>(program->impl_, limits);
}
Runtime::~Runtime() = default;
ObjectRef Runtime::Create(std::string_view className) { if(!impl_->depth)impl_->Reset(); return impl_->Create(Canonical(std::string(className)), {}); }
ObjectRef Runtime::CreatePrefab(std::string asset) {if(asset.empty())return {};auto ref=Create("prefab");impl_->Resolve(ref).prefabAsset=std::move(asset);return ref;}
std::string Runtime::PrefabAsset(ObjectRef ref) const {if(!ref.id)return {};const auto& object=impl_->Resolve(ref);if(object.type->name!="prefab")throw std::invalid_argument("Object is not a prefab reference");return object.prefabAsset;}
Value Runtime::Call(ObjectRef object, std::string_view method, const std::vector<Value>& arguments) {
    impl_->Reset(); return impl_->Invoke(object, std::string(method), arguments, {});
}
Value Runtime::Get(ObjectRef object, std::string_view field) const { return impl_->Get(object, std::string(field)); }
void Runtime::Set(ObjectRef object, std::string_view field, Value value, bool notify) { if(!impl_->depth)impl_->Reset(); impl_->Set(object, std::string(field), std::move(value), {}, notify); }
namespace {
[[noreturn]] void ArrayFault(const std::string& message) { throw ScriptError({"<array>", 1, 1, message}); }
}
std::size_t Runtime::ArrayLength(ArrayRef array) const {
    return impl_->arrays[impl_->ArrayId(Value{array}, {})].size();
}
Value Runtime::ArrayElement(ArrayRef array, std::size_t index) const {
    const auto& items = impl_->arrays[impl_->ArrayId(Value{array}, {})];
    if (index >= items.size()) ArrayFault("Array index out of range");
    return items[index];
}
void Runtime::SetArrayElement(ArrayRef array, std::size_t index, Value value) {
    impl_->Reset();
    auto& items = impl_->arrays[impl_->ArrayId(Value{array}, {})];
    if (index >= items.size()) ArrayFault("Array index out of range");
    items[index] = std::holds_alternative<ObjectRef>(value) ? value : impl_->Coerce(value, impl_->Type(value, {}), {});
}
void Runtime::AppendArrayElement(ArrayRef array, Value value) {
    impl_->Reset();
    auto& items = impl_->arrays[impl_->ArrayId(Value{array}, {})];
    if (impl_->arrayElements >= impl_->limits.arrayElements) ArrayFault("Array element limit exceeded");
    items.push_back(std::holds_alternative<ObjectRef>(value) ? value : impl_->Coerce(value, impl_->Type(value, {}), {}));
    ++impl_->arrayElements;
}
void Runtime::RemoveArrayElement(ArrayRef array, std::size_t index) {
    auto& items = impl_->arrays[impl_->ArrayId(Value{array}, {})];
    if (index >= items.size()) ArrayFault("Array index out of range");
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    --impl_->arrayElements;
}
void Runtime::SetObjectLookup(std::function<ObjectRef(std::string_view)> lookup){impl_->objectLookup=std::move(lookup);}
void Runtime::SetTypeLookup(std::function<ObjectRef(std::string_view)> lookup){impl_->typeLookup=std::move(lookup);}
void Runtime::SetTagLookup(std::function<std::vector<std::string>(ObjectRef)> lookup){impl_->tagLookup=std::move(lookup);}
void Runtime::SetPhysicsCallbacks(PhysicsBodyCall bodyCall,PhysicsCastCall castCall){impl_->physicsBodyCall=std::move(bodyCall);impl_->physicsCastCall=std::move(castCall);}
void Runtime::BindNativeBehavior(ObjectRef owner,std::string_view behaviorType){impl_->Reset();impl_->BindNativeBehavior(owner,std::string(behaviorType));}
void Runtime::SetPrefabSpawnCallback(PrefabSpawnCall callback){impl_->prefabSpawnCall=std::move(callback);}
void Runtime::SetPrintCallback(PrintCallback callback){impl_->printCallback=std::move(callback);}
void Runtime::Connect(SignalRef signal, CallableRef callback) { impl_->Reset(); impl_->Signal(signal, "connect", {callback}, {}); }
void Runtime::Emit(SignalRef signal, const std::vector<Value>& arguments) { impl_->Reset(); impl_->Signal(signal, "emit", arguments, {}); }
void Runtime::SetInput(const InputFrame& frame,bool emitEvents) {impl_->Reset();impl_->SetInput(frame,emitEvents);}
void Runtime::SetMouse(const MouseFrame& frame,bool emitEvents) {impl_->Reset();impl_->SetMouse(frame,emitEvents);}
void Runtime::Start(ObjectRef object) { impl_->Reset(); impl_->Start(object); }
void Runtime::Update(ObjectRef object, double delta) {
    if (!std::isfinite(delta) || delta < 0) impl_->Error({}, "Delta must be finite and nonnegative");
    impl_->Reset(); impl_->Hook(object, "update", {delta}); impl_->AdvanceTimers(delta);
}
void Runtime::PhysicsUpdate(ObjectRef object, double delta) {
    if (!std::isfinite(delta) || delta < 0) impl_->Error({}, "Delta must be finite and nonnegative");
    impl_->Reset(); impl_->Hook(object, "physicsUpdate", {delta});
}
void Runtime::Draw(ObjectRef object) { impl_->Reset(); impl_->Hook(object, "draw", {}); }

} // namespace zengine::script
