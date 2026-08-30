#include "zscript/Script.h"
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
using namespace zengine::script;
namespace {
void Check(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
std::shared_ptr<const Program> Compile(const std::string& source) {
    auto r = Compiler::Compile(source, "test.zsh");
    if (!r) throw ScriptError(r.diagnostics.at(0));
    return r.program;
}
template<class F> void Error(F action, const std::string& text) {
    try { action(); } catch (const ScriptError& e) {
        Check(e.Detail().message.find(text) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("Expected error: " + text);
}
std::int64_t Int(const Value& v) { return std::get<std::int64_t>(v); }
double Float(const Value& v) { return std::get<double>(v); }
ObjectRef TransformOf(Runtime& r, ObjectRef o) { return std::get<ObjectRef>(r.Get(o, "transform")); }
void EmptyAndComments() {
    auto p = Compile(R"(// class Fake { broken syntax
        class Empty : gameObject { /* arbitrary } { ; */ func start() {} func update(float delta) {} func draw() {} }
        class Plain {} class Nested { func empty() { { /* skipped */ {} } } })");
    auto s = p->Stats();
    Check(s.declaredClasses == 3 && s.executableClasses == 0 && s.emittedFunctions == 0 && s.instructions == 0, "Empty code emitted");
    Runtime r(p); auto o = r.Create("Empty"); r.Start(o); r.Update(o, 0.1); r.Draw(o);
    Error([&] { r.Call(o, "update"); }, "argument count");
    Error([&] { r.Call(o, "update", {true}); }, "Cannot assign");
    auto comments = Compile("class Text { string a = \"// literal\"; string b = \"/* literal */\"; }");
    Runtime t(comments); auto text = t.Create("Text");
    Check(std::get<std::string>(t.Get(text, "a")) == "// literal" && std::get<std::string>(t.Get(text, "b")) == "/* literal */", "Comment markers in strings");
    Error([&] { Compile("class A { /* unfinished"); }, "Unterminated comment");
}
void Movement() {
    auto p = Compile(R"(class Mover : gameObject {
        int calls;
        func move(GameObject obj, Vector3 amount) { obj.transform.position += amount; }
        func target() : GameObject { calls += 1; return this; }
        func shift() { target().transform.position += Vector3(1, 2, 3); target().transform.position.x += 4; }
        func edit() { Vector3 v = Vector3(1, 2, 3); Vector3 copy = v; v.x = 7; v.y *= 2; v.z /= 3; transform.position = v + copy; }
        func arithmetic() : Vector3 { return -(Vector3(1, 2, 3) * 2 - Vector3(1, 1, 1)) / 2; }
        func scaleVector() : Vector3 { return 2 * Vector3(1, 2, 3); }
    })");
    Runtime r(p); auto mover = r.Create("Mover"), object = r.Create("GameObject"); auto transform = TransformOf(r, object);
    Check(std::get<Vector3>(r.Get(transform, "scale")) == Vector3{1, 1, 1}, "Default scale");
    r.Call(mover, "move", {object, Vector3{2, 3, 4}}); r.Call(mover, "move", {object, Vector3{-1, 0, 2}});
    Check(std::get<Vector3>(r.Get(transform, "position")) == Vector3{1, 3, 6}, "Requested move syntax");
    r.Call(mover, "shift"); Check(Int(r.Get(mover, "calls")) == 2, "Assignment receiver evaluated more than once");
    Check(std::get<Vector3>(r.Get(TransformOf(r, mover), "position")) == Vector3{5, 2, 3}, "Component compound assignment");
    r.Call(mover, "edit"); Check(std::get<Vector3>(r.Get(TransformOf(r, mover), "position")) == Vector3{8, 6, 4}, "Vector value semantics");
    Check(std::get<Vector3>(r.Call(mover, "arithmetic")) == Vector3{-0.5, -1.5, -2.5}, "Vector operators");
    Check(std::get<Vector3>(r.Call(mover, "scaleVector")) == Vector3{2, 4, 6}, "Scalar vector multiply");
    Check(p->HasClass("GameObject") && p->HasClass("gameObject"), "GameObject aliases");
    Error([&] { Compile("class A { func f(GameObject obj) { Obj.transform.position = Vector3(); } }"); }, "Unknown field");
}
void LifecycleAndInheritance() {
    auto p = Compile(R"(class Child : Base { func update(float dt) { elapsed += dt * 2; frames += 1; } }
        class Quiet : Base { func start() {} func update(float dt) {} }
        class Base : GameObject { int starts; int frames; int draws; float elapsed = 1;
            func start() { starts += 1; } func update(float dt) { elapsed += dt; frames += 1; }
            func draw() { draws += 1; } func seconds() : float { return elapsed; } }
        class Driver { Base target = Child(); func run(float dt) { target.update(dt); } }
    )");
    Runtime r(p); auto a = r.Create("Child"), b = r.Create("Quiet");
    r.Update(a, 0.25); r.Start(a); r.Update(a, 0.5); r.Draw(a); r.Update(b, 1);
    Check(Int(r.Get(a, "starts")) == 1 && Int(r.Get(a, "frames")) == 2 && Int(r.Get(a, "draws")) == 1, "Lifecycle counts");
    Check(Float(r.Call(a, "seconds")) == 2.5, "Inherited state/override");
    Check(Int(r.Get(b, "starts")) == 0 && Float(r.Get(b, "elapsed")) == 1, "Empty override");
    auto driver = r.Create("Driver"); r.Call(driver, "run", {0.5}); auto target = std::get<ObjectRef>(r.Get(driver, "target"));
    Check(Float(r.Get(target, "elapsed")) == 2, "Virtual dispatch through base reference");
    r.Set(a, "elapsed", std::int64_t{9}); Check(Float(r.Get(a, "elapsed")) == 9, "Host widening");
}
void ValuesAndControlFlow() {
    auto p = Compile(R"(class Math { int calls; string text = "hi";
        func sum(int n) : int { int total = 0; int i = 0; while (i < n) { total += i; i += 1; } { int total = 99; } return total; }
        func choose(bool yes) : string { if (yes) { return text + "!"; } else { return "no"; } }
        func calc() : float { return -(2 + 3) * 4 / 2.0 + 0.5; }
        func side() : bool { calls += 1; return true; }
        func logic() : bool { bool a = false && side(); bool b = true || side(); bool c = true && side(); bool d = false || side(); return 3 >= 3 && 2 <= 4 && 5 > 4 && 1 != 2 && !false; }
        func widen(int n) : float { return n; }
    })");
    Runtime r(p); auto o = r.Create("Math");
    Check(Int(r.Call(o, "sum", {std::int64_t{5}})) == 10, "Loop/local scopes");
    Check(std::get<std::string>(r.Call(o, "choose", {true})) == "hi!" && std::get<std::string>(r.Call(o, "choose", {false})) == "no", "Branch returns");
    Check(Float(r.Call(o, "calc")) == -9.5 && Float(r.Call(o, "widen", {std::int64_t{4}})) == 4, "Arithmetic/coercion");
    Check(std::get<bool>(r.Call(o, "logic")) && Int(r.Get(o, "calls")) == 2, "Short circuit");
    Error([&] { r.Start(o); }, "gameObject-derived");
}
void ReferencesAndInitializers() {
    auto p = Compile(R"(class Node { Node next; int value = 2; }
        class Base { int a = 3; } class Child : Base { int b = a + 4; }
        class Holder { Node node = Node(); func read() : int { return node.next.value; } func empty() : bool { return node.next == null; } })");
    Runtime r(p); auto child = r.Create("Child"), h = r.Create("Holder");
    Check(Int(r.Get(child, "b")) == 7 && std::get<bool>(r.Call(h, "empty")), "Initializer/default order");
    Error([&] { r.Call(h, "read"); }, "Null object"); auto node = std::get<ObjectRef>(r.Get(h, "node")); r.Set(node, "next", node);
    Check(Int(r.Call(h, "read")) == 2, "Cyclic reference");
}
void Diagnostics() {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"class A { int x = 1 }", "Expected ';'"}, {"class A { func f() {", "Unterminated block"},
        {"class A : Missing {}", "Unknown base"}, {"class A : B {} class B : A {}", "Inheritance cycle"},
        {"class A {} class A {}", "Duplicate class"}, {"class A { Missing x; }", "field type"}, {"class A { void x; }", "field type"},
        {"class A { int x; int x; }", "Duplicate/inherited"}, {"class A { int x; func x() {} }", "Duplicate member"},
        {"class A { func f(int x, int x) {} }", "Duplicate local"}, {"class A { func f(int x) { int x; } }", "Duplicate local"},
        {"class A { func f() { int x = true; } }", "Cannot assign"}, {"class A { func f() { missing(); } }", "Unknown method"},
        {"class A { func f() : int {} }", "must return"}, {"class A { func f(bool b) : int { if (b) { return 1; } } }", "must return"},
        {"class A { func f() { return 1; } }", "Void function"}, {"class A { func f() : float { return; } }", "Return value required"},
        {"class A { func f() { if (1) {} } }", "Condition must"}, {"class A { func f() { int n = true + 1; } }", "Arithmetic operands"},
        {"class A { func f() { this = A(); } }", "Cannot assign to this"}, {"class A { func f() { int A; } }", "type keywords"},
        {"class A : gameObject { func update() {} }", "Update signature"}, {"class A : gameObject { func start(int n) {} }", "Lifecycle hook"},
        {"class A { func f(int n) {} } class B : A { func f(float n) {} }", "Override"},
        {"class A { func f() {} } class B : A { int f; }", "Duplicate/inherited"},
        {"class A { string s = \"bad; }", "Unterminated string"}, {"class A { float f = 1e999; }", "out of range"},
        {"class A { Vector3 v = Vector3(1, 2); }", "zero or three"}, {"class A { Vector3 v = Vector3(true, 2, 3); }", "must be numeric"},
        {"class A { func f() { Vector3 v; v += 1; } }", "Arithmetic operands"},
        {"class A { func f() { Vector3().x = 1; } }", "Assignment target"}
    };
    for (const auto& [source, message] : cases) Error([&] { Compile(source); }, message);
    auto bad = Compiler::Compile("class A {\n func f() {\n  missing = 1;\n }\n}", "broken.zsh");
    Check(!bad && bad.diagnostics[0].line == 3 && bad.diagnostics[0].column == 3 && bad.diagnostics[0].source == "broken.zsh", "Diagnostic source location");
}
void TestRuntimeLimits() {
    auto p = Compile(R"(class Risk { int n; string text = "ok";
        func forever() { while (true) { n += 1; } } func recurse() { recurse(); }
        func zero() : int { return 1 / 0; } func overflow() : int { return 9223372036854775807 + 1; }
        func mul() : int { return 9223372036854775807 * 2; } func sub() : int { return (-9223372036854775807 - 1) - 1; }
        func neg() : int { return -(-9223372036854775807 - 1); } func div() : int { return (-9223372036854775807 - 1) / -1; }
        func infinity() : float { return 1e308 * 1e308; } func big() { text += text + text; }
        func vectorZero() : Vector3 { return Vector3(1, 2, 3) / 0; } func good() : int { return 42; }
    })");
    zengine::script::RuntimeLimits l; l.instructionsPerCall = 100; l.callDepth = 8; l.objects = 2; l.stringBytes = 4;
    Runtime r(p, l); auto o = r.Create("Risk");
    Error([&] { r.Call(o, "forever"); }, "Instruction budget"); Error([&] { r.Call(o, "recurse"); }, "Call depth");
    for (const auto& m : {"overflow", "mul", "sub", "neg", "div"}) Error([&] { r.Call(o, m); }, "Integer overflow");
    Error([&] { r.Call(o, "zero"); }, "Division by zero"); Error([&] { r.Call(o, "vectorZero"); }, "Division by zero");
    Error([&] { r.Call(o, "infinity"); }, "Non-finite"); Error([&] { r.Call(o, "big"); }, "String size");
    Check(Int(r.Call(o, "good")) == 42, "Fault recovery"); Error([&] { r.Set(o, "n", true); }, "Cannot assign");
    Error([&] { r.Get({}, "n"); }, "Null object"); r.Create("Risk"); Error([&] { r.Create("Risk"); }, "Object limit");
}
void HostBoundary() {
    auto p = Compile(R"(class Test : gameObject { int starts; Test other; float amount;
        func start() { starts += 1; int fail = 1 / 0; } })");
    Runtime a(p), b(p); auto x = a.Create("Test"), y = b.Create("Test");
    Error([&] { a.Get(y, "starts"); }, "another runtime"); Error([&] { a.Set(x, "other", y); }, "another runtime");
    Error([&] { a.Set(x, "amount", std::numeric_limits<double>::quiet_NaN()); }, "Non-finite");
    Error([&] { a.Set(TransformOf(a, x), "position", Vector3{std::numeric_limits<double>::infinity(), 0, 0}); }, "Non-finite");
    Error([&] { a.Update(x, -1); }, "Delta"); Error([&] { a.Start(x); }, "Division by zero");
    Error([&] { a.Update(x, 0.1); }, "Start previously failed"); Check(Int(a.Get(x, "starts")) == 1, "Start retried");
    zengine::script::RuntimeLimits l; l.callDepth = 8; Runtime r(Compile("class Node { Node child = Node(); }"), l);
    Error([&] { r.Create("Node"); }, "Call depth");
    Check(Compile("")->Stats().instructions == 0, "Empty source");
    Error([&] { Compile("class X { func f() { int n = " + std::string(150, '(') + "1" + std::string(150, ')') + "; } }"); }, "nesting limit");
    Error([&] { Compile(std::string(1024 * 1024 + 1, ' ')); }, "Source exceeds");
}
void InheritedNativeDefaultsAndExpressionDepth() {
    Runtime runtime(Compile("class DerivedTransform : Transform {}"));
    auto transform = runtime.Create("DerivedTransform");
    Check(std::get<Vector3>(runtime.Get(transform, "scale")) == Vector3{1, 1, 1}, "Inherited native defaults");
    std::string expression = "1";
    for (int i = 0; i < 40; ++i) expression = "(" + expression + "+1+1+1+1+1+1+1+1)";
    Error([&] { Compile("class Deep { func run() : int { return " + expression + "; } }"); }, "nesting limit");
}
void Examples() {
    for (const auto& name : {"Counter.zsh", "EmptyBehavior.zsh", "Mover.zsh"}) {
        std::ifstream in(std::string(SCRIPT_EXAMPLES) + "/" + name, std::ios::binary);
        Check(static_cast<bool>(in), std::string("Missing example ") + name);
        Compile(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }
}
}
int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"empty code and comments", EmptyAndComments}, {"movement and Vector3", Movement}, {"lifecycle and inheritance", LifecycleAndInheritance},
        {"values and control flow", ValuesAndControlFlow}, {"references and initializers", ReferencesAndInitializers},
        {"compile diagnostics", Diagnostics}, {"runtime limits", TestRuntimeLimits}, {"host boundary", HostBoundary}, {"native defaults and expression depth", InheritedNativeDefaultsAndExpressionDepth}, {"example files", Examples}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
