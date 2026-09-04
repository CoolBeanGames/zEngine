#include "zscript/Script.h"
#include "zscript/NativeTypes.h"
#include <fstream>
#include <functional>
#include <cmath>
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
        func forwardMask() : Vector3 { return Vector3(2, 3, 4) * transform.forward; }
        func statics() : Vector3 { return Vector3.up() + Vector3.right() * 2 + Vector3.zero(); }   // ZE-79
        func statics2() : Vector2 { return Vector2.one() - Vector2.left(); }
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
    Check(std::get<Vector3>(r.Call(mover,"forwardMask"))==Vector3{0,0,4},"Vector3 multiply by transform.forward");
    Check(p->HasClass("GameObject") && p->HasClass("gameObject"), "GameObject aliases");
    Check(std::get<Vector3>(r.Call(mover, "statics")) == Vector3{2, 1, 0}, "ZE-79: Vector3 static factories");
    Check(std::get<Vector2>(r.Call(mover, "statics2")) == Vector2{2, 1}, "ZE-79: Vector2 static factories");
    Error([&] { Compile("class A { func f() { Vector3 v; v.up(); } }"); }, "Unknown method 'up' on 'Vector3'");
    Error([&] { Compile("class A { func f() { Vector3.sideways(); } }"); }, "Unknown static member");
    Error([&] { Compile("class A { func f(GameObject obj) { Obj.transform.position = Vector3(); } }"); }, "Unknown field");
}
void LifecycleAndInheritance() {
    auto p = Compile(R"(class Child : Base { func update(float dt) { elapsed += dt * 2; frames += 1; } }
        class Quiet : Base { func start() {} func update(float dt) {} func physicsUpdate(float dt) {} }
        class Base : GameObject { int starts; int frames; int fixedTicks; int draws; float elapsed = 1;
            func start() { starts += 1; } func update(float dt) { elapsed += dt; frames += 1; }
            func physicsUpdate(float dt) { fixedTicks += 1; }
            func draw() { draws += 1; } func seconds() : float { return elapsed; } }
        class Driver { Base target = Child(); func run(float dt) { target.update(dt); } }
    )");
    Runtime r(p); auto a = r.Create("Child"), b = r.Create("Quiet");
    r.Update(a, 0.25); r.Start(a); r.Update(a, 0.5); r.PhysicsUpdate(a,1.0/60);r.PhysicsUpdate(a,1.0/60);r.Draw(a); r.Update(b, 1);r.PhysicsUpdate(b,1.0/60);
    Check(Int(r.Get(a, "starts")) == 1 && Int(r.Get(a, "frames")) == 2 && Int(r.Get(a, "fixedTicks"))==2 && Int(r.Get(a, "draws")) == 1, "Lifecycle counts");
    Check(Float(r.Call(a, "seconds")) == 2.5, "Inherited state/override");
    Check(Int(r.Get(b, "starts")) == 0 && Int(r.Get(b,"fixedTicks"))==0 && Float(r.Get(b, "elapsed")) == 1, "Empty override");
    auto driver = r.Create("Driver"); r.Call(driver, "run", {0.5}); auto target = std::get<ObjectRef>(r.Get(driver, "target"));
    Check(Float(r.Get(target, "elapsed")) == 2, "Virtual dispatch through base reference");
    r.Set(a, "elapsed", std::int64_t{9}); Check(Float(r.Get(a, "elapsed")) == 9, "Host widening");
    auto native=Compile("class NativeMover : RigidBody { RigidBody saved; func take(RigidBody body){saved=body;} }");Runtime nativeRuntime(native);auto nativeMover=nativeRuntime.Create("NativeMover");nativeRuntime.Call(nativeMover,"take",{nativeMover});
    Check(std::get<ObjectRef>(nativeRuntime.Get(nativeMover,"physics"))==nativeMover&&std::get<ObjectRef>(nativeRuntime.Get(nativeMover,"rigidbody"))==nativeMover&&std::get<ObjectRef>(nativeRuntime.Get(nativeMover,"saved"))==nativeMover,"Native behavior inheritance/reference typing");
}
void ScopeKeywords() {
    // ZE-79: super, override, abstract, private, _ shorthand.
    auto p = Compile(R"ZS(class Animal {
        abstract func speak() : string;
        private func _secret() : int { return 7; }
        func describe() : string { return "an " & speak() & " count " & {_secret()}; }
    }
    class Dog : Animal {
        override func speak() : string { return "dog"; }
    }
    class Puppy : Dog {
        override func speak() : string { return super.speak() & " small"; }
        func both() : string { return super.speak() & "/" & speak(); }
    })ZS");
    Runtime r(p);
    auto dog = r.Create("Dog"), puppy = r.Create("Puppy");
    Check(std::get<std::string>(r.Call(dog, "describe")) == "an dog count 7", "abstract impl + private helper");
    Check(std::get<std::string>(r.Call(puppy, "speak")) == "dog small", "super.speak() calls the base implementation");
    Check(std::get<std::string>(r.Call(puppy, "both")) == "dog/dog small", "super is static, speak() is virtual");
    Check(std::get<std::string>(r.Call(puppy, "describe")) == "an dog small count 7", "inherited describe() dispatches virtually");

    Error([&] { Compile("class A { abstract func f(); }  class B { func g() { A x = A(); } }"); }, "abstract class 'A'");
    Error([&] { Compile("class A { func f() {} }  class B : A { override func g() {} }"); }, "does not replace an inherited method");
    Error([&] { Compile("class A { private func p() {} }  class B { func g(A a) { a.p(); } }"); }, "private to A");
    Error([&] { Compile("class A { int _hidden; }  class B { func g(A a) : int { return a._hidden; } }"); }, "private to A");
    Error([&] { Compile("class P { func q() {} }  class A : P { func x() { super.nope(); } }"); }, "No inherited 'nope'");
    Error([&] { Compile("class A { func x() { super.y(); } }"); }, "'super' requires a base class");
    Error([&] { Compile("class A { abstract func f() { return 1; } }"); }, "Expected");
    // A concrete subclass of an abstract base is constructible; the abstract base is not.
    Check(Compile("class A { abstract func f() : int; }  class B : A { override func f() : int { return 1; } func make() { B b = B(); } }") != nullptr, "concrete subclass constructs");

    // ZE-79: static fields + methods - class-level state, no `this`.
    auto s = Compile(R"ZS(class Counter {
        static int total = 10;
        private static int _step = 1;
        static func bump() : int { total += _step; return total; }
        static func reset() { total = 0; }
        func mine() : int { return Counter.total; }
    })ZS");
    Runtime rs(s);
    auto c1 = rs.Create("Counter"), c2 = rs.Create("Counter");
    Check(Int(rs.Call(c1, "bump")) == 11 && Int(rs.Call(c2, "bump")) == 12, "static state is shared across instances");
    Check(Int(rs.Call(c1, "mine")) == 12, "instance reads a static via ClassName.field");
    rs.Call(c1, "reset");
    Check(Int(rs.Call(c2, "mine")) == 0, "static reset seen everywhere");

    Error([&] { Compile("class A { static func s() {} func f() { s(); this.s(); } }  class B { func g(A a) { a.s(); } }"); }, "static");
    Error([&] { Compile("class A { int n; static func s() : int { return n; } }"); }, "static method cannot use the instance member");
    Error([&] { Compile("class A { static int n; }  class B { func f() : int { A a = A(); return a.n; } }"); }, "static member of A");
    Error([&] { Compile("class A { int n; func f() : int { return A.n; } }"); }, "instance member of A");
}
void ValuesAndControlFlow() {
    auto p = Compile(R"(class Math { int calls; string text = "hi";
        func sum(int n) : int { int total = 0; int i = 0; while (i < n) { total += i; i += 1; } { int total = 99; } return total; }
        func conditional_for(int n) : int { int total=0; int i=0; for(i<n){total+=i;i+=1;} return total; }
        func choose(bool yes) : string { if (yes) { return text + "!"; } else { return "no"; } }
        func calc() : float { return -(2 + 3) * 4 / 2.0 + 0.5; }
        func side() : bool { calls += 1; return true; }
        func logic() : bool { bool a = false && side(); bool b = true || side(); bool c = true && side(); bool d = false || side(); return 3 >= 3 && 2 <= 4 && 5 > 4 && 1 != 2 && !false; }
        func word_logic() : bool { calls=0; bool a=false and side(); bool b=true or side(); bool c=false nor false; bool d=true nor side(); bool e=false nor side(); return not a and b and c and not d and not e and 1!=2; }
        func widen(int n) : float { return n; }
    })");
    Runtime r(p); auto o = r.Create("Math");
    Check(Int(r.Call(o, "sum", {std::int64_t{5}})) == 10, "Loop/local scopes");
    Check(Int(r.Call(o,"conditional_for",{std::int64_t{5}}))==10,"Conditional for loop failed");
    Check(std::get<std::string>(r.Call(o, "choose", {true})) == "hi!" && std::get<std::string>(r.Call(o, "choose", {false})) == "no", "Branch returns");
    Check(Float(r.Call(o, "calc")) == -9.5 && Float(r.Call(o, "widen", {std::int64_t{4}})) == 4, "Arithmetic/coercion");
    Check(std::get<bool>(r.Call(o, "logic")) && Int(r.Get(o, "calls")) == 2, "Short circuit");
    Check(std::get<bool>(r.Call(o,"word_logic")) && Int(r.Get(o,"calls"))==1,"Keyword logical operators or nor short circuit failed");
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
        {"class A : gameObject { func update() {} }", "Update signature"}, {"class A : gameObject { func physicsUpdate() {} }", "Physics update signature"}, {"class A : gameObject { func start(int n) {} }", "Lifecycle hook"},
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
void InspectorMetadata() {
    auto p = Compile(R"(class Child : Parent {
        label("Child settings"); export Vector3 direction = Vector3(1, 0, 0); int hiddenChild;
    }
    class Parent : gameObject {
        label("Movement"); export float speed = 2; int hidden = 7;
        func update(float dt) { hidden += 1; }
        export bool enabled = true;
    })");
    const auto& base = p->InspectorLayout("Parent");
    const auto& child = p->InspectorLayout("Child");
    Check(base.size() == 3 && child.size() == 5, "Hidden/native fields exposed or inherited entries lost");
    Check(child[0].kind == InspectorEntry::Kind::Label && child[0].text == "Movement", "Base label ordering");
    Check(child[1].kind == InspectorEntry::Kind::Field && child[1].name == "speed" && child[1].type == "float", "Exported field metadata");
    Check(child[2].name == "enabled" && child[3].text == "Child settings" && child[4].name == "direction", "Declaration order across methods/inheritance");
    Check(child[1].declaringClass == "Parent" && child[4].declaringClass == "Child" && child[4].source == "test.zsh", "Declaration origin");
    Check(child[0].name.empty() && child[0].type.empty() && child[1].text.empty(), "Entry payload kinds");
    Runtime runtime(p); auto object = runtime.Create("Child");
    runtime.Set(object, child[1].name, 4.0); runtime.Update(object, 0.1);
    Check(Float(runtime.Get(object, "speed")) == 4 && Int(runtime.Get(object, "hidden")) == 8, "Inspector editing or hidden-field execution");
    Check(p->InspectorLayout("gameObject").empty() && p->InspectorLayout("GameObject").empty() && p->InspectorLayout("Transform").empty(), "Native fields leaked into script Inspector");
    Error([&] { p->InspectorLayout("Missing"); }, "Unknown class");

    auto types = Compile(R"(class Custom {} class All {
        export int count; export float speed; export bool enabled; export string title;
        export Vector3 direction; export GameObject target; export Transform pose; export Custom custom;
    })");
    const auto& all = types->InspectorLayout("All");
    const std::vector<std::string> expected = {"int", "float", "bool", "string", "Vector3", "gameObject", "Transform", "Custom"};
    Check(all.size() == expected.size(), "Missing exported types");
    for (std::size_t i = 0; i < all.size(); ++i) Check(all[i].type == expected[i], "Wrong exported type");

    auto located = Compiler::Compile("class Located {\nlabel(\"Group\");\nexport float speed = 2;\n}", "located.zsh");
    Check(static_cast<bool>(located), "Source-coordinate fixture");
    const auto& entries = located.program->InspectorLayout("Located");
    Check(entries[0].line == 2 && entries[0].column == 1 && entries[1].line == 3 && entries[1].column == 14 && entries[1].source == "located.zsh", "Inspector source coordinates");
}
void InspectorMetadataHasNoExecutionCost() {
    auto labeled = Compile(R"(class OnlyLabels : gameObject { label("Title"); label("/* literal */ // literal"); func start() {} }
        class Hidden { int value; })");
    Check(labeled->InspectorLayout("OnlyLabels").size() == 2 && labeled->InspectorLayout("Hidden").empty(), "Label-only metadata/hidden default");
    Check(labeled->InspectorLayout("OnlyLabels")[1].text == "/* literal */ // literal", "Comments inside label text");
    Check(labeled->Stats().instructions == 0 && labeled->Stats().emittedFunctions == 0 && labeled->Stats().executableClasses == 1, "Labels emitted runtime work");
    auto empty = Compile("class Labels : gameObject { label(\"\"); label(\"Title\"); }");
    Check(empty->Stats().executableClasses == 0 && empty->Stats().instructions == 0 && empty->InspectorLayout("Labels")[0].text.empty(), "Labels defeated empty-code elimination");
    auto normal = Compile("class A : gameObject { float speed = 2; float elapsed; func update(float dt) { elapsed += speed * dt; } }");
    auto exported = Compile("class A : gameObject { label(\"Motion\"); export float speed = 2; float elapsed; func update(float dt) { elapsed += speed * dt; } label(\"End\"); }");
    Check(normal->Stats().instructions == exported->Stats().instructions && normal->Stats().emittedFunctions == exported->Stats().emittedFunctions, "Inspector annotations changed bytecode");
    Runtime runtime(exported); auto object = runtime.Create("A"); runtime.Set(object, "speed", 3.0); runtime.Update(object, 0.5);
    Check(Float(runtime.Get(object, "elapsed")) == 1.5, "Edited exported value not used by script");
    auto escapes = Compile(R"(class Text { label("a\"b\\c\nnext"); })");
    Check(escapes->InspectorLayout("Text")[0].text == "a\"b\\c\nnext", "Label string escapes");
}
void InspectorDiagnostics() {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"class A { export func f() {} }", "typed class field"},
        {"class A { export label(\"Wrong\"); }", "typed class field"},
        {"class A { export export int count; }", "Duplicate field tag"},
        {"class A { export }", "typed class field"},
        {"class A { export Missing value; }", "field type"},
        {"class A { export void value; }", "field type"},
        {"class A { export int value = true; }", "Cannot assign"},
        {"class A { label(123); }", "string literal"},
        {"class A { label(); }", "string literal"},
        {"class A { label(\"Title\" + \"Text\"); }", "Expected ')'"},
        {"class A { func f() { export int local; } }", "class scope"},
        {"class A { func f() { label(\"Local\"); } }", "class scope"},
        {"class A { int label; }", "Expected identifier"},
        {"class A { func export() {} }", "Expected identifier"},
        {"class A { export int value; } class B : A { int value; }", "Duplicate/inherited"}
    };
    for (const auto& [source, message] : cases) Error([&] { Compile(source); }, message);
}
void Signals() {
    auto p = Compile(R"(
        class Receiver { int sum; func receive(int n, string text) { sum += n; } }
        class Base : gameObject { signal ping; }
        class Sender : Base {
            Receiver other = Receiver(); int sum; Vector3 moved; int moves;
            func receive(int n, string text) { sum += n; }
            func on_moved(Vector3 value) { moved = value; moves += 1; }
            func start() {
                ping.connect(receive); ping.connect(receive); ping.connect(other.receive);
                transform.was_moved.connect(on_moved);
            }
            func send() { ping.emit(3, "test"); transform.position.x += 2; }
            func remove() { ping.disconnect(receive); }
            func connected() : bool { return ping.is_connected(receive); }
        })");
    Runtime vm(p); auto a=vm.Create("Sender"), b=vm.Create("Sender"); vm.Start(a); vm.Start(b);
    vm.Call(a,"send");
    Check(Int(vm.Get(a,"sum"))==3 && Int(vm.Get(b,"sum"))==0, "Signal isolation or duplicate connect");
    Check(Int(vm.Get(std::get<ObjectRef>(vm.Get(a,"other")),"sum"))==3, "Other receiver not called");
    Check(std::get<Vector3>(vm.Get(a,"moved")).x==2 && Int(vm.Get(a,"moves"))==1, "Transform signal not emitted");
    vm.Call(a,"remove"); vm.Call(a,"send");
    Check(!std::get<bool>(vm.Call(a,"connected")) && Int(vm.Get(a,"sum"))==3, "Disconnect failed");
    const auto transform=std::get<ObjectRef>(vm.Get(a,"transform"));
    vm.Set(transform,"position",Vector3{4,0,0});
    Check(Int(vm.Get(a,"moves"))==2, "Unchanged transform emitted");
    Error([&] { vm.Emit({a,"ping"},{true}); }, "argument count");
    Runtime foreign(p); auto foreignObject=foreign.Create("Sender");
    Error([&] { vm.Connect({a,"ping"},{foreignObject,"receive"}); }, "another runtime");
    Error([&] { Compile("class A { signal ping; int ping; }"); }, "Duplicate");
    Error([&] { Compile("class A { signal ping; func f() { ping = 1; } }"); }, "Unknown field");
    Error([&] { Compile("class A { signal ping; func f() { ping.connect(3); } }"); }, "function reference");
    auto recursion=Compile("class A { signal ping; func f() { ping.emit(); } func run() { ping.connect(f); ping.emit(); } }");
    Runtime recursive(recursion, {1000,16,100,1024,4}); auto r=recursive.Create("A");
    Error([&] { recursive.Call(r,"run"); }, "depth limit");
    auto mutation=Compile(R"(class A {
        signal ping; int count;
        func first() { ping.disconnect(second); ping.connect(third); count += 1; }
        func second() { count += 10; } func third() { count += 100; }
        func run() { ping.connect(first); ping.connect(second); ping.emit(); }
        func again() { ping.emit(); }
    })");
    Runtime m(mutation); auto o=m.Create("A"); m.Call(o,"run");
    Check(Int(m.Get(o,"count"))==1,"Listener mutation during emission");
    m.Call(o,"again"); Check(Int(m.Get(o,"count"))==102,"New listener not deferred to next emission");
}
void Examples() {
    for (const auto& name : {"Counter.zsh", "EmptyBehavior.zsh", "Mover.zsh", "InspectorBehavior.zsh", "SignalBehavior.zsh"}) {
        std::ifstream in(std::string(SCRIPT_EXAMPLES) + "/" + name, std::ios::binary);
        Check(static_cast<bool>(in), std::string("Missing example ") + name);
        Compile(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }
}
void TypeManifestIsCurrent() {
    // ZE-79: scripting/types.json is generated from TypeManifest(). Keep it in sync -
    // if this fails, run the test binary with `regen` to rewrite the file.
    const std::string fresh = zengine::script::TypeManifest();
    Check(fresh.find("\"zscript_type_manifest\": 1") != std::string::npos, "manifest header missing");
    Check(fresh.find("\"type_name\": \"Vector3\"") != std::string::npos
          && fresh.find("\"instance_name\": \"gameObject\"") != std::string::npos, "manifest is missing core types");
    std::ifstream in(ZSCRIPT_TYPES_JSON, std::ios::binary);
    std::string onDisk; { std::istreambuf_iterator<char> it(in), end; onDisk.assign(it, end); }
    if (onDisk != fresh) {
        std::ofstream actual(std::string(ZSCRIPT_TYPES_JSON) + ".actual", std::ios::binary);
        actual << fresh;
        Check(false, "scripting/types.json is stale - regenerate it (see types.json.actual)");
    }
}
void ArraysTypesAndLocals() {
    auto p=Compile(R"(class Base {} class Child : Base {}
        class A : gameObject {
            label("Values")
            export int count=7;
            array items=[1,2.0,true,"text",Vector3(1,2,3),Child(),[9]];
            func run():bool {
                int count=3; float speed=2; bool ok=true; string text="local";
                Vector3 v=Vector3(4,5,6); Child child=Child(); array local=[];
                local.append(v); local.append(child); local.append(text);
                array alias=local; alias[2]="changed";
                items[0]+=4; items.append(local); items.erase(1);
                return count==3 && this.count==7 && speed is float && ok is bool && text is string &&
                    items[0] is int && !(items[0] is float) && items[1] is bool && items[3] is Vector3 &&
                    items[4] is Child && items[4] is Base && items[5] is array && items[5][0]==9 &&
                    local[2]=="changed" && items.size()==7 && null is null;
            }
            func fresh():int { int count=0; count+=1; return count; }
            func bounds() { items[-1]=2; }
            func bad() { int n=items[1]; }
            func arithmetic() { items[1]+1; }
            func empty():int { array a; return a.size(); }
        })");
    Runtime r(p);auto a=r.Create("A"),b=r.Create("A");
    Check(std::get<bool>(r.Call(a,"run")),"Array operations, is checks, or local initializers failed");
    Check(Int(r.Call(a,"fresh"))==1 && Int(r.Call(a,"fresh"))==1 && Int(r.Get(a,"count"))==7,"Locals leaked between calls or shadowed class field incorrectly");
    Check(Int(r.Call(a,"empty"))==0,"Uninitialized array should be empty");
    Check(std::get<bool>(r.Call(b,"run")),"Array defaults shared across instances");
    Error([&]{r.Call(a,"bounds");},"in-range integer");
    Error([&]{r.Call(a,"bad");},"Cannot assign");
    Error([&]{r.Call(a,"arithmetic");},"arithmetic operands");
    Runtime foreign(p);auto c=foreign.Create("A");
    Error([&]{r.Set(a,"items",foreign.Get(c,"items"));},"another runtime");
    Error([&]{Compile("class A { func f() { export int a; } }");},"class scope");
    Error([&]{Compile("class A { func f() { label(\"Bad\") } }");},"class scope");
    Error([&]{Compile("class A { func f() { int local=1; } func g():int { return local; } }");},"Unknown field");
    Error([&]{Compile("class A { func f():bool { return 1 is Missing; } }");},"Unknown type");
    RuntimeLimits limits;limits.arrayElements=1;
    Runtime limited(Compile("class A { array a=[]; func f() { a.append(1); a.append(2); } }"),limits);
    auto tiny=limited.Create("A");Error([&]{limited.Call(tiny,"f");},"limit exceeded");
    // Host-facing element API used by the editor inspector (ZE-33).
    Runtime api(Compile("class A : gameObject { array a=[1,2,3]; }"));
    auto obj=api.Create("A"); auto arr=std::get<ArrayRef>(api.Get(obj,"a"));
    Check(api.ArrayLength(arr)==3 && Int(api.ArrayElement(arr,1))==2,"Array length/element read failed");
    api.SetArrayElement(arr,1,std::int64_t{42}); Check(Int(api.ArrayElement(arr,1))==42,"SetArrayElement failed");
    api.AppendArrayElement(arr,std::string("tail"));
    Check(api.ArrayLength(arr)==4 && std::get<std::string>(api.ArrayElement(arr,3))=="tail","AppendArrayElement failed");
    api.RemoveArrayElement(arr,0); // [1,42,3,"tail"] -> [42,3,"tail"]
    Check(api.ArrayLength(arr)==3 && Int(api.ArrayElement(arr,0))==42,"RemoveArrayElement failed");
    Error([&]{api.ArrayElement(arr,9);},"out of range");
    Error([&]{api.SetArrayElement(arr,9,std::int64_t{0});},"out of range");
}
}
void Parenting() {
    auto p=Compile("class A : gameObject {func lookup():gameObject{return find(\"Root\");} func loop(){while(true){find(\"Root\");}}}");
    Runtime r(p);auto a=r.Create("A"),b=r.Create("gameObject");
    Check(std::get<ObjectRef>(r.Get(a,"parent")).id==0,"Parent must default to null");
    Check(std::get<ObjectRef>(r.Call(a,"lookup")).id==0,"Unbound host lookup must return null");
    r.Set(a,"parent",b);Check(std::get<ObjectRef>(r.Get(a,"parent"))==b,"Parent write failed");
    Error([&]{r.Set(b,"parent",a);},"cycle");Error([&]{r.Set(a,"parent",a);},"cycle");
    r.Set(a,"parent",ObjectRef{});r.SetObjectLookup([&](std::string_view name){return name=="Root"?b:ObjectRef{};});
    Check(std::get<ObjectRef>(r.Call(a,"lookup"))==b,"Host lookup not bridged");
    RuntimeLimits limits;limits.instructionsPerCall=100;Runtime limited(p,limits);const auto ref=limited.Create("A");
    limited.SetObjectLookup([&](std::string_view){auto proxy=limited.Create("gameObject");limited.Set(proxy,"parent",ObjectRef{},false);return proxy;});
    Error([&]{limited.Call(ref,"loop");},"Instruction budget");
}
void TextAndGlobalTransforms() {
    auto p=Compile(R"(class A : gameObject {
        multiline export string description="first\nsecond";
        export char initial='A';
        string word="hé🙂";
        func run():bool {
            char c=word[1]; string combined='a'+'b'; combined &= 'c'; combined += "d";
            array mixed=[word,'x']; string shortened=word.truncate(2);
            return c is char && c=='é' && word[2]=='🙂' && word.size()==3 &&
                shortened=="hé" && word.size()==3 && word.substr(1,20)=="é🙂" &&
                word.substr(3,2)=="" && word.truncate(0)=="" && combined=="abcd" &&
                "hello" & '!' == "hello!" && 'a'<'b' && "abc"<"abd" && 'a'=="a" &&
                mixed[0].size()==3 && mixed[0].truncate(1)=="h" && mixed[1] is char;
        }
        func badIndex(){char c=word[3];}
        func negative(){string s=word.truncate(-1);}
        func badStart(){string s=word.substr(4,1);}
        func concatenate():string{return word & word;}
        func char_input():bool{return Input.is_action_pressed('x');}
        func char_lookup():gameObject{return find('P');}
    })");
    Runtime r(p);const auto a=r.Create("A"),parent=r.Create("gameObject");
    InputState pressed;pressed.pressed=true;r.SetInput({{"x",pressed}});Check(std::get<bool>(r.Call(a,"char_input")),"Character did not coerce to native input string");
    r.SetObjectLookup([&](std::string_view name){return name=="P"?parent:ObjectRef{};});Check(std::get<ObjectRef>(r.Call(a,"char_lookup"))==parent,"Character did not coerce to lookup name");
    Check(std::get<bool>(r.Call(a,"run")),"Unicode chars, concatenation, comparison, indexing or truncation failed");
    Check(p->InspectorLayout("A")[0].multiline && !p->InspectorLayout("A")[1].multiline,"Multiline metadata missing");
    Error([&]{r.Call(a,"badIndex");},"in-range");Error([&]{r.Call(a,"negative");},"nonnegative");Error([&]{r.Call(a,"badStart");},"out of range");
    Error([&]{Compile("class A {export multiline int count;}");},"exported string");
    Error([&]{Compile("class A {multiline string text;}");},"exported string");
    Error([&]{Compile("class A {func f(){multiline export string s;}}");},"class scope");
    Error([&]{Compile("class A {char bad='ab';}");},"one Unicode character");
    Error([&]{Compile("class A {string bad=1 & 2;}");},"Concatenation");
    Check(Compile("class A : gameObject {func f(){transform.global_position.x=1;}}") != nullptr, "ZE-83: global fields are writable");
    const auto at=TransformOf(r,a),pt=TransformOf(r,parent);r.Set(a,"parent",parent);
    r.Set(pt,"position",Vector3{10,0,0});r.Set(pt,"rotation",Vector3{0,0,90});r.Set(pt,"scale",Vector3{2,3,4});r.Set(at,"position",Vector3{1,0,0});
    auto position=std::get<Vector3>(r.Get(at,"global_position")),rotation=std::get<Vector3>(r.Get(at,"global_rotation")),scale=std::get<Vector3>(r.Get(at,"global_scale"));
    Check(std::abs(position.x-10)<1e-8 && std::abs(position.y-2)<1e-8 && std::abs(rotation.z-90)<1e-8 && scale==Vector3{2,3,4},"Global TRS does not compose local-to-parent transforms");
    // ZE-83: writing a global component sets the local value that produces it, whatever the parent does.
    r.Set(at,"global_position",Vector3{5,7,0});
    { const auto g=std::get<Vector3>(r.Get(at,"global_position")); Check(std::abs(g.x-5)<1e-6 && std::abs(g.y-7)<1e-6, "global_position write did not round-trip through the parent"); }
    r.Set(at,"global_rotation",Vector3{0,0,0});
    { const auto g=std::get<Vector3>(r.Get(at,"global_rotation")); Check(std::abs(g.z)<1e-6, "global_rotation write did not cancel the parent rotation"); }
    r.Set(a,"parent",ObjectRef{});Check(std::get<Vector3>(r.Get(at,"global_position"))==std::get<Vector3>(r.Get(at,"position")),"Detached: global == local");
    r.Set(at,"global_scale",Vector3{3,3,3});Check(std::get<Vector3>(r.Get(at,"scale"))==Vector3{3,3,3},"global_scale write with no parent == local scale");
    r.Set(at,"scale",Vector3{0,-2,3});Check(std::get<Vector3>(r.Get(at,"global_scale"))==Vector3{0,2,3},"Zero scale global read failed");
    Error([&]{r.Set(a,"initial",char32_t{0xd800});},"Unicode");
    Error([&]{r.Set(a,"description",std::string("\xc0\xaf"));},"UTF-8");
    RuntimeLimits limits;limits.stringBytes=12;Runtime limited(p,limits);auto small=limited.Create("A");Error([&]{limited.Call(small,"concatenate");},"String size limit");
}
void PrefabReferences() {
    auto program=Compile(R"(class Spawner : gameObject { export prefab template; gameObject made; func start(){made=template.spawn();} func result():gameObject{return made;} })");
    Runtime runtime(program);const auto spawner=runtime.Create("Spawner");
    Check(std::get<ObjectRef>(runtime.Get(spawner,"template"))==ObjectRef{},"Prefab field did not default to null");
    runtime.Set(spawner,"template",runtime.CreatePrefab("Props/Crate.zprefab"));
    std::string requested;const auto spawned=runtime.Create("gameObject");runtime.SetPrefabSpawnCallback([&](std::string_view asset){requested=asset;return spawned;});
    runtime.Start(spawner);Check(requested=="Props/Crate.zprefab" && std::get<ObjectRef>(runtime.Call(spawner,"result"))==spawned,"Prefab spawn did not return the created GameObject");
    Error([&]{Compile("class Invalid { prefab p=prefab(); }");},"supplied by the host");
}
void SceneService() {
    auto program=Compile(R"(class Switcher : gameObject {
        string went = "";
        func start(){ went = Scene.current(); Scene.load("Level2"); }
        func loaded():string { return went; }
    })");
    Runtime runtime(program);
    const auto s=runtime.Create("Switcher");
    std::string requested, current="Level1";
    runtime.SetSceneCallbacks([&](std::string_view name){requested=name;}, [&]{return current;});
    runtime.Start(s);
    Check(requested=="Level2","Scene.load did not reach the host callback");
    Check(std::get<std::string>(runtime.Call(s,"loaded"))=="Level1","Scene.current did not return the host name");
    // Missing callback -> a clear runtime fault, not a crash.
    Runtime bare(program);
    Error([&]{bare.Start(bare.Create("Switcher"));},"Scene loading is not available");
    // Scene is a host service, not constructible / inheritable.
    Error([&]{Compile("class Bad : SceneService { }");},"Cannot inherit native service types");
    Error([&]{Compile("class Bad { func f(){ SceneService x = SceneService(); } }");},"supplied by the host");
}
void MathfFunctions() {
    auto program=Compile(R"(class MathTest {
        func scalar():bool {
            return Mathf.lerp(2,6,0.25)==3 && Mathf.sin(0)==0 && Mathf.cos(0)==1 &&
                Mathf.tan(0)==0 && Mathf.sqrt(9)==3 && Mathf.exp(0)==1 && Mathf.round(2.6)==3;
        }
        func vector():bool {
            Vector3 a=Vector3(1,2,3); Vector3 b=Vector3(4,5,6);
            return Mathf.dot(a,b)==32 && Mathf.cross(Vector3(1,0,0),Vector3(0,1,0))==Vector3(0,0,1);
        }
        func invalid(){float value=Mathf.sqrt(-1);}
        func clamp_ok():bool {
            return Mathf.clamp(5,0,10)==5 && Mathf.clamp(-3,0,10)==0 && Mathf.clamp(99,0,10)==10;
        }
        func bad_clamp(){float v=Mathf.clamp(1,10,0);}
        func randoms():bool {
            int i=0; while (i<200) {
                float r=Mathf.random();
                float g=Mathf.random_range(-2,2);
                int n=Mathf.random_int(3,6);
                if (r<0 or r>=1) { return false; }
                if (g < -2 or g >= 2) { return false; }
                if (n<3 or n>6) { return false; }
                i = i + 1;
            }
            return true;
        }
    })");
    Runtime runtime(program);const auto object=runtime.Create("MathTest");
    Check(std::get<bool>(runtime.Call(object,"scalar")),"Mathf scalar methods returned incorrect values");
    Check(std::get<bool>(runtime.Call(object,"vector")),"Mathf vector methods returned incorrect values");
    Check(std::get<bool>(runtime.Call(object,"clamp_ok")),"Mathf.clamp returned incorrect values");
    Check(std::get<bool>(runtime.Call(object,"randoms")),"Mathf.random* left its documented range");
    Error([&]{runtime.Call(object,"bad_clamp");},"min <= max");
    Error([&]{runtime.Call(object,"invalid");},"nonnegative");
    Error([&]{Compile("class Bad : Mathf {}");},"Cannot inherit");
    Error([&]{Compile("class Bad { Mathf value=Mathf(); }");},"supplied by the host");
}
void Timers() {
    auto program=Compile(R"(class Clock : gameObject {
        int fired; Timer timer;
        func start(){timer=make_timer(1);timer.finished.connect(on_finished);}
        func on_finished(){fired+=1;}
        func count():int{return fired;}
        func touch(){timer.finished.emit();}
    })");
    Runtime runtime(program);const auto clock=runtime.Create("Clock");runtime.Update(clock,0.4);
    Check(Int(runtime.Call(clock,"count"))==0,"Timer fired too early");runtime.Update(clock,0.6);
    Check(Int(runtime.Call(clock,"count"))==1,"Timer did not emit finished at its duration");runtime.Update(clock,10);
    Check(Int(runtime.Call(clock,"count"))==1,"One-shot timer fired more than once");
    Error([&]{runtime.Call(clock,"touch");},"deleted");
    Error([&]{Compile("class Bad : Timer {}");},"Cannot inherit");
    Error([&]{Compile("class Bad : gameObject { Timer t=Timer(); }");},"supplied by the host");
    auto invalid=Compile("class Invalid : gameObject {func start(){make_timer(-1);}}");Runtime bad(invalid);auto object=bad.Create("Invalid");Error([&]{bad.Start(object);},"nonnegative");
}
void AudioPlayerClass() {
    // ZE-67: a script inherits audioPlayer -> play()/stop() route to the host and
    // the started / looped / finished signals connect like any other.
    int played = 0, stopped = 0;
    auto program = Compile(R"(class Music : audioPlayer {
        int starts; int loops; int ends;
        func start(){ started.connect(on_start); looped.connect(on_loop); finished.connect(on_end); play(); }
        func replay(){ play(); }
        func silence(){ stop(); }
        func on_start(){ starts += 1; }
        func on_loop(){ loops += 1; }
        func on_end(){ ends += 1; }
        func counts():int { return starts * 100 + loops * 10 + ends; }
    })");
    Runtime runtime(program);
    runtime.SetAudioCallback([&](ObjectRef, std::string_view method) {
        if (method == "play") played += 1; else if (method == "stop") stopped += 1;
    });
    const auto music = runtime.Create("Music");
    runtime.Start(music);
    Check(played == 1 && stopped == 0, "start() did not call play() through the host");
    runtime.Call(music, "silence");
    Check(stopped == 1, "stop() did not reach the host");
    runtime.Call(music, "replay");
    Check(played == 2, "a second play() did not reach the host");

    // The host raises started / looped / finished on the object's own proxy.
    runtime.Emit({music, "started"}, {});
    runtime.Emit({music, "looped"}, {});
    runtime.Emit({music, "looped"}, {});
    runtime.Emit({music, "finished"}, {});
    Check(Int(runtime.Call(music, "counts")) == 121, "audioPlayer signals did not reach their handlers");

    // ZE-109: a script on an Area with an AudioEffect can toggle it and retune the reverb.
    int enables = 0, disables = 0; float lastDecay = 0, lastWet = 0;
    auto areaProgram = Compile(R"(class Cave : audioArea {
        func start(){ set_reverb(4.0, 0.9); disable(); enable(); }
    })");
    Runtime areaRuntime(areaProgram);
    areaRuntime.SetAudioAreaCallback([&](ObjectRef, std::string_view m, float a, float b) {
        if (m == "enable") enables += 1;
        else if (m == "disable") disables += 1;
        else { lastDecay = a; lastWet = b; }
    });
    const auto cave = areaRuntime.Create("Cave");
    areaRuntime.Start(cave);
    Check(enables == 1 && disables == 1 && lastDecay == 4.0f && std::fabs(lastWet - 0.9f) < 1e-4f,
          "audioArea methods did not route to the host");

    // ZE-74: lightSource - toggle and retune a Light from script.
    int lon = 0, loff = 0; float lr = 0, lg = 0, lb = 0, li = 0, lscatter = 0;
    auto lightProgram = Compile(R"(class Lamp : lightSource {
        func start(){ disable(); enable(); set_color(1.0, 0.5, 0.25); set_intensity(3.0); set_fog_scatter(0.8); }
    })");
    Runtime lightRuntime(lightProgram);
    lightRuntime.SetLightCallback([&](ObjectRef, std::string_view m, float a, float b, float c) {
        if (m == "enable") lon += 1;
        else if (m == "disable") loff += 1;
        else if (m == "set_color") { lr = a; lg = b; lb = c; }
        else if (m == "set_intensity") li = a;
        else if (m == "set_fog_scatter") lscatter = a;
    });
    const auto lamp = lightRuntime.Create("Lamp");
    lightRuntime.Start(lamp);
    Check(lon == 1 && loff == 1 && lr == 1.0f && std::fabs(lg - 0.5f) < 1e-4f && li == 3.0f && std::fabs(lscatter - 0.8f) < 1e-4f,
          "lightSource methods did not route to the host");

    // ZE-76: decalProjector - toggle and retune a Decal from script.
    int don = 0, doff = 0; float dr = 0, dg = 0, db = 0, dop = 0;
    auto decalProgram = Compile(R"(class Splat : decalProjector {
        func start(){ disable(); enable(); set_tint(0.9, 0.2, 0.1); set_opacity(0.4); }
    })");
    Runtime decalRuntime(decalProgram);
    decalRuntime.SetDecalCallback([&](ObjectRef, std::string_view m, float a, float b, float c) {
        if (m == "enable") don += 1;
        else if (m == "disable") doff += 1;
        else if (m == "set_tint") { dr = a; dg = b; db = c; }
        else if (m == "set_opacity") dop = a;
    });
    const auto splat = decalRuntime.Create("Splat");
    decalRuntime.Start(splat);
    Check(don == 1 && doff == 1 && dr == 0.9f && std::fabs(dg - 0.2f) < 1e-4f && std::fabs(dop - 0.4f) < 1e-4f,
          "decalProjector methods did not route to the host");
}
void GetBehavior() {
    auto p=Compile(R"(class Player : rigidbody {
        func check():bool {
            rigidbody rb = getBehavior(rigidbody);
            transform tr = getBehavior(Transform);
            physicsbody pb = this.getBehavior(PhysicsBody);
            return rb is RigidBody && tr is Transform && pb is PhysicsBody && rb is behavior;
        }
    })");
    Runtime r(p); auto o=r.Create("Player");
    Check(std::get<bool>(r.Call(o,"check")),"getBehavior did not return the native components");
    Error([&]{Compile("class A : gameObject { func f(){ collider c = getBehavior(Vector3); } }");},"native component type");
    Error([&]{Compile("class A : gameObject { func f(){ int x=0; collider c = getBehavior(x); } }");},"native component type");
    Error([&]{Compile("class A : gameObject { func f(){ getBehavior(RigidBody, Collider); } }");},"one component type");
    Error([&]{Compile("class A : gameObject { func f(){ getBehavior(gameObject); } }");},"native component type");
}
void FindAndTags() {
    auto p=Compile(R"(class Target : rigidbody {}
    class Q : gameObject {
        func by_type():rigidbody { return find_by_type(RigidBody); }   // ZE-86: returns the behavior, not the gameObject
        func by_type_is_body():bool { return find_by_type(RigidBody) is PhysicsBody; }
        func by_type_static():collider { return GameObject.find_by_type(Collider); }
        func by_name():gameObject { return GameObject.find("Target"); }
        func first_tag():string { array t=get_tags(); return t[0]; }
        func tag_count():int { return get_tags().size(); }
        func is_boss():bool { return has_tag("boss"); }
        func is_minion():bool { return has_tag("minion"); }
    })");
    Runtime r(p); auto q=r.Create("Q"); auto target=r.Create("Target");   // rigidbody accessor self-binds on create
    r.SetObjectLookup([&](std::string_view n){ return n=="Target"?target:ObjectRef{}; });
    r.SetTypeLookup([&](std::string_view type){ return type=="RigidBody"?target:ObjectRef{}; });
    r.SetTagLookup([&](ObjectRef){ return std::vector<std::string>{"boss","elite"}; });
    Check(std::get<ObjectRef>(r.Call(q,"by_type"))==target,"find_by_type did not resolve to the searched behavior on the found object");
    Check(std::get<bool>(r.Call(q,"by_type_is_body")),"find_by_type(RigidBody) should be a PhysicsBody reference");
    Check(std::get<ObjectRef>(r.Call(q,"by_type_static")).id==0,"find_by_type(Collider) should be null here");
    Check(std::get<ObjectRef>(r.Call(q,"by_name"))==target,"GameObject.find did not resolve");
    Check(Int(r.Call(q,"tag_count"))==2 && std::get<std::string>(r.Call(q,"first_tag"))=="boss","get_tags did not return the host tags");
    Check(std::get<bool>(r.Call(q,"is_boss")) && !std::get<bool>(r.Call(q,"is_minion")),"has_tag membership check failed");
    Error([&]{Compile("class A : gameObject { func f(){ find_by_type(Vector3); } }");},"native component type");
    Error([&]{Compile("class A : gameObject { func f(){ find_by_type(\"x\"); } }");},"find_by_type takes one component type");
}
void NativeTypeAliases() {
    auto program=Compile(R"(class Native : rigidbody {
        rigidbody saved; staticbody static_saved; kinematicbody kinematic_saved; physicsbody physics_saved; collider collider_saved; area area_saved; behavior behavior_saved; transform transform_saved;
        func take(rigidbody body){saved=body;physics_saved=body;behavior_saved=body;transform_saved=body.transform;}
        func valid():bool{return saved is rigidbody && saved is physicsbody && saved is behavior && saved is gameobject && physics_saved is RigidBody && transform_saved is Transform;}
    })");
    Runtime runtime(program);const auto object=runtime.Create("Native");runtime.Call(object,"take",{object});
    Check(std::get<bool>(runtime.Call(object,"valid")),"Lowercase native behavior aliases were not canonicalized consistently");
    Check(program->HasClass("rigidbody") && program->IsGameObject("staticbody"),"Native aliases were not recognized by Program type queries");

    // The phone book (zscript/NativeTypes.h) is the single source: every registered
    // native type must be a known class, and every component's gameObject accessor
    // must resolve - so adding one row there wires the whole compiler automatically.
    for (const auto& native : zengine::script::NativeTypes()) {
        if (!native.scriptClass) continue;
        Check(program->HasClass(std::string(native.name)), "Registered native type not a known class: " + std::string(native.name));
        if (!native.accessor.empty()) {
            // ui_control lives on gameObject2D; every other accessor on gameObject.
            const std::string base = native.accessor == "ui_control" ? "gameObject2D" : "gameObject";
            auto check = Compile("class Probe : " + base + " { func start(){ " + std::string(native.accessor) + "; } }");
            Check(static_cast<bool>(check), base + "." + std::string(native.accessor) + " accessor for " + std::string(native.name) + " did not resolve");
        }
        if (native.component) {
            auto derived = Compile("class Derived : " + std::string(native.name) + " { func start(){} }");
            Check(static_cast<bool>(derived), "Cannot derive a script from registered component " + std::string(native.name));
        }
    }
}
void Vector2Type() {
    auto p=Compile(R"(class V : gameObject {
        export Vector2 pos = Vector2(1, 2);
        func make():Vector2 { return Vector2(3, 4); }
        func zero():Vector2 { return Vector2(); }
        func add():Vector2 { return Vector2(1,2) + Vector2(10,20); }
        func sub():Vector2 { return Vector2(10,20) - Vector2(1,2); }
        func scale():Vector2 { return Vector2(1,2) * 3; }
        func rscale():Vector2 { return 3 * Vector2(1,2); }
        func div():Vector2 { return Vector2(6,9) / 3; }
        func mul():Vector2 { return Vector2(2,3) * Vector2(4,5); }
        func neg():Vector2 { return -Vector2(1,-2); }
        func comp():float { Vector2 v = Vector2(5,6); v.x = 9; v.y += 1; return v.x + v.y; }
        func checks():bool { return Vector2(1,2) is Vector2 && !(Vector2(1,2) is Vector3) && !(5 is Vector2); }
        func text():string { return "v=" & {Vector2(1,2)}; }
    })");
    Runtime r(p); auto v=r.Create("V");
    Check(std::get<Vector2>(r.Get(v,"pos"))==Vector2{1,2},"Vector2 field initializer");
    Check(std::get<Vector2>(r.Call(v,"make"))==Vector2{3,4},"Vector2(x,y)");
    Check(std::get<Vector2>(r.Call(v,"zero"))==Vector2{0,0},"Vector2()");
    Check(std::get<Vector2>(r.Call(v,"add"))==Vector2{11,22} && std::get<Vector2>(r.Call(v,"sub"))==Vector2{9,18},"Vector2 add/sub");
    Check(std::get<Vector2>(r.Call(v,"scale"))==Vector2{3,6} && std::get<Vector2>(r.Call(v,"rscale"))==Vector2{3,6},"Vector2 scalar multiply");
    Check(std::get<Vector2>(r.Call(v,"div"))==Vector2{2,3} && std::get<Vector2>(r.Call(v,"mul"))==Vector2{8,15},"Vector2 divide / component multiply");
    Check(std::get<Vector2>(r.Call(v,"neg"))==Vector2{-1,2},"Vector2 negate");
    Check(std::get<double>(r.Call(v,"comp"))==16,"Vector2 component read/write");
    Check(std::get<bool>(r.Call(v,"checks")),"Vector2 is-type checks");
    Check(std::get<std::string>(r.Call(v,"text"))=="v=1, 2","Vector2 interpolation");
    Error([&]{Compile("class A { Vector2 v = Vector2(1); }");},"zero or two");
    Error([&]{Compile("class A { func f(){ Vector2 v; v.z = 1; } }");},"Unknown field 'z' on 'Vector2'");
    Error([&]{Compile("class A { func f(){ Vector2 a = Vector2(); Vector3 b = a; } }");},"Cannot assign 'Vector2' to 'Vector3'");
    Error([&]{Compile("class A { func f(){ Vector2 a = Vector2() + Vector3(); } }");},"Arithmetic operands");
}
void GameObject2DScript() {
    auto p = Compile(R"(class Sprite : gameObject2D {
        export Vector2 spawn = Vector2(10, 20);
        func start() { transform.position = spawn; transform.rotation = 45; }
        func move(float dx) { transform.position.x += dx; }
        func where():Vector2 { return transform.position; }
        func angle():float { return transform.rotation; }
        func mine():bool { return this is gameObject2D && !(this is gameObject); }
    })");
    Check(static_cast<bool>(p), "gameObject2D-derived class did not compile");
    Check(p->IsGameObject("Sprite"), "gameObject2D-derived class not recognised as gameObject-like");
    Runtime r(p); auto s = r.Create("Sprite"); r.Start(s);
    Check(std::get<Vector2>(r.Call(s, "where")) == Vector2{10, 20}, "Transform2D.position not initialised from start()");
    Check(std::get<double>(r.Call(s, "angle")) == 45, "Transform2D.rotation (float) not set");
    r.Call(s, "move", {2.0});
    Check(std::get<Vector2>(r.Call(s, "where")) == Vector2{12, 20}, "Transform2D.position.x component write failed");
    Check(std::get<bool>(r.Call(s, "mine")), "gameObject2D type identity wrong");
    Error([&] { Compile("class B : gameObject2D { func f(){ transform.position = Vector3(1,2,3); } }"); }, "Cannot assign 'Vector3'");
}
void UiControlClasses() {
    auto p = Compile(R"(class Menu : uiPanel {
        export string title = "Play";
        int clicks = 0;
        func start() {
            anchor = "center";
            size = Vector2(220, 140);
            tint = Vector3(0.1, 0.1, 0.12);
            clicked.connect(on_click);
        }
        func on_click() { clicks += 1; }
        func shown():string { return title; }
    }
    class Row : uiHTileBox { func start() { fill_cross = true; spacing = 4; } }
    class Caption : uiText { func start() { text = "hi"; pixel_height = 18; color = Vector3(1,1,1); } }
    class Bar : uiProgressBar { func start() { value = 0.5; vertical = false; } }
    class Field : uiTextEntry { func start() { placeholder = "name"; } }
    class List : uiScroll { func start() { scroll_y = 10; horizontal = false; } }
    class Player : uiVideo { func start() { video = "clip.zvid"; loop = true; speed = 1; } }
    class Doc : uiHtml { func start() { html = "<p>hi</p>"; } }
    class Ok : uiButton {
        int downs = 0; int ups = 0;
        func start() { text = "OK"; pressed.connect(on_down); released.connect(on_up); }
        func on_down() { downs += 1; }
        func on_up() { ups += 1; }
    }
    class Hoverable : uiColorRect {
        int enters = 0; bool focused_now = false;
        func start() {
            mouse_entered.connect(on_enter); mouse_exited.connect(on_exit);
            focus_entered.connect(on_focus); focus_exited.connect(on_blur);
        }
        func on_enter() { enters += 1; }
        func on_exit() { enters -= 1; }
        func on_focus() { focused_now = true; }
        func on_blur() { focused_now = false; }
    }
    class Entry2 : uiTextEntry {
        int submits = 0;
        func start() { submitted.connect(on_submit); }
        func on_submit() { submits += 1; }
    })");
    Check(static_cast<bool>(p), "UI control subclasses did not compile");
    Check(p->IsGameObject("Menu") && p->IsGameObject("Row") && p->IsGameObject("Bar"), "UI controls are gameObject2D-like");
    Check(p->IsGameObject("List") && p->IsGameObject("Ok") && p->IsGameObject("Doc"), "ZE-66 UI controls are gameObject2D-like");

    Runtime r(p); auto m = r.Create("Menu"); r.Start(m);
    Check(std::get<std::string>(r.Call(m, "shown")) == "Play", "inherited + own fields work on a UI subclass");
    {
        auto ok = r.Create("Ok"); r.Start(ok);
        r.Emit({ok, "pressed"}, {});
        r.Emit({ok, "released"}, {});
        Check(std::get<std::int64_t>(r.Get(ok, "downs")) == 1 && std::get<std::int64_t>(r.Get(ok, "ups")) == 1,
              "uiButton pressed / released signals reach handlers");
    }
    Check(std::get<Vector2>(r.Get(m, "size")) == Vector2{220, 140}, "layout field set from start()");
    Check(std::get<Vector2>(r.Get(std::get<ObjectRef>(r.Get(m, "transform")), "position")) == Vector2{0, 0}, "UI control carries a Transform2D");
    r.Emit({m, "clicked"}, {});
    Check(std::get<std::int64_t>(r.Get(m, "clicks")) == 1, "the clicked signal reaches a connected handler");
    {
        auto h = r.Create("Hoverable"); r.Start(h);
        r.Emit({h, "mouse_entered"}, {}); r.Emit({h, "focus_entered"}, {});
        Check(std::get<std::int64_t>(r.Get(h, "enters")) == 1 && std::get<bool>(r.Get(h, "focused_now")),
              "ZE-96 hover / focus signals reach handlers");
        r.Emit({h, "mouse_exited"}, {}); r.Emit({h, "focus_exited"}, {});
        Check(std::get<std::int64_t>(r.Get(h, "enters")) == 0 && !std::get<bool>(r.Get(h, "focused_now")),
              "ZE-96 hover / focus exit signals reach handlers");
        auto e = r.Create("Entry2"); r.Start(e);
        r.Emit({e, "submitted"}, {});
        Check(std::get<std::int64_t>(r.Get(e, "submits")) == 1, "ZE-96 TextEntry submitted signal reaches a handler");
    }

    Error([&] { Compile("class X { func f(){ int uiColorRect; } }"); }, "type keywords");
    Error([&] { Compile("class Bad : uiContainer { func f(){ transform.position = Vector3(1,2,3); } }"); }, "Cannot assign 'Vector3'");

    // ZE-87: a plain script references UI controls - typed export field, find_by_type, getBehavior.
    auto ref = Compile(R"(class Panel2 : uiPanel {}
    class Hud : gameObject {
        export uiButton start_button;
        uiPanel root;
        int taps = 0;
        func start() {
            root = find_by_type(uiPanel);
            start_button.clicked.connect(on_tap);
        }
        func on_tap() { taps += 1; }
        func is_panel():bool { return find_by_type(uiPanel) is uiControl; }
    })");
    Check(static_cast<bool>(ref), "a script could not reference UI controls (ZE-87)");
    {
        Runtime u(ref);
        auto hud = u.Create("Hud");
        auto panel = u.Create("Panel2");
        u.SetTypeLookup([&](std::string_view t){ return t=="uiPanel" ? panel : ObjectRef{}; });
        Check(std::get<bool>(u.Call(hud,"is_panel")), "find_by_type(uiPanel) did not resolve to the control");
    }
    Error([&]{ Compile("class A : gameObject { func f(){ getBehavior(uiButton); } }"); }, "gameObject2D");
}
void MouseInput() {
    auto program=Compile(R"(class Cursor : gameObject {
        int clicks; int releases; int held; int moves;
        Vector2 last_from; Vector2 last_to; int last_button = -1;
        func start(){
            Input.mouse.clicked.connect(on_click);
            Input.mouse.click_ended.connect(on_release);
            Input.mouse.held.connect(on_held);
            Input.mouse.was_just_moved.connect(on_move);
        }
        func on_click(int b){ clicks += 1; last_button = b; }
        func on_release(int b){ releases += 1; }
        func on_held(int b){ held += 1; }
        func on_move(Vector2 from, Vector2 to){ moves += 1; last_from = from; last_to = to; }
        func position():Vector2{ return Input.mouse.position; }
        func delta():Vector2{ return Input.mouse.delta; }
        func clicks_count():int{ return clicks; }
        func releases_count():int{ return releases; }
        func held_count():int{ return held; }
        func moves_count():int{ return moves; }
        func button():int{ return last_button; }
    })");
    Runtime r(program); const auto c=r.Create("Cursor"); r.Start(c);
    MouseFrame f; f.x=0.5; f.y=-0.25;
    r.SetMouse(f);                    // first frame: establishes position, no delta / move event
    Check(std::get<Vector2>(r.Call(c,"position"))==Vector2{0.5,-0.25},"Mouse.position not exposed");
    Check(std::get<Vector2>(r.Call(c,"delta"))==Vector2{},"First mouse frame should have zero delta");
    Check(Int(r.Call(c,"moves_count"))==0,"was_just_moved fired on the first frame");
    f.x=0.75; f.y=-0.25; f.buttons[0]={true,true,false};
    r.SetMouse(f);
    Check(std::get<Vector2>(r.Call(c,"delta"))==Vector2{0.25,0},"Mouse delta not derived from successive frames");
    Check(Int(r.Call(c,"moves_count"))==1,"was_just_moved did not fire on movement");
    Check(Int(r.Call(c,"clicks_count"))==1 && Int(r.Call(c,"button"))==0,"clicked(button) did not fire");
    f.buttons[0]={true,false,false};   // still held
    r.SetMouse(f);
    Check(Int(r.Call(c,"held_count"))==1 && Int(r.Call(c,"clicks_count"))==1,"held(button) did not fire / clicked repeated");
    f.buttons[0]={false,false,true};   // released
    r.SetMouse(f);
    Check(Int(r.Call(c,"releases_count"))==1,"click_ended(button) did not fire");
    Error([&]{Compile("class Bad : Mouse {}");},"Cannot inherit");
    Error([&]{Compile("class Bad : gameObject { Mouse m=Mouse(); }");},"supplied by the host");
    Error([&]{Compile("class Bad : gameObject { func f(){ Input.mouse.delta = Vector2(1,0); } }");},"read-only");
    // Callback signature is checked when connect() runs (like the Transform/PhysicsBody signals).
    { Runtime bad(Compile("class Bad : gameObject { func start(){ Input.mouse.clicked.connect(f); } func f(){} }"));
      auto o=bad.Create("Bad"); Error([&]{bad.Start(o);},"button index"); }
    { Runtime bad(Compile("class Bad : gameObject { func start(){ Input.mouse.was_just_moved.connect(f); } func f(Vector3 v){} }"));
      auto o=bad.Create("Bad"); Error([&]{bad.Start(o);},"Mouse was_just_moved"); }
    Error([&]{ Runtime bad(Compile("class X : gameObject {}")); bad.SetMouse([]{ MouseFrame m; m.x=99; return m; }()); },"Invalid mouse position");

    // ZE-84: Input.set_mouse_mode(Mouse.<mode>) routes an int 0-4 to the host.
    { Runtime m(Compile(R"(class M : gameObject {
        func lock(){ Input.set_mouse_mode(Mouse.captured); }
        func hide(){ Input.set_mouse_mode(Mouse.confined_hidden); }
        func free(){ Input.set_mouse_mode(Mouse.visible); }
      })"));
      int seen=-99; m.SetMouseModeCallback([&](int mode){ seen=mode; });
      auto o=m.Create("M");
      m.Call(o,"lock");   Check(seen==4,"Mouse.captured did not reach the host as 4");
      m.Call(o,"hide");   Check(seen==3,"Mouse.confined_hidden did not reach the host as 3");
      m.Call(o,"free");   Check(seen==0,"Mouse.visible did not reach the host as 0");
    }
    Error([&]{Compile("class A : gameObject { func f(){ Input.set_mouse_mode(Mouse.nope); } }");},"Unknown mouse mode");
    Error([&]{Compile("class A : gameObject { func f(){ Input.set_mouse_mode(\"captured\"); } }");},"Cannot assign 'string' to 'int'");
}
void DataObjects() {
    // ZE-91: `struct` declares a data object rooted at `data`; it can be created, can
    // inherit another data object, carry public functions, and be referenced by a behavior.
    auto p = Compile(R"(struct Item {
        int count = 1;
        string name = "thing";
        func doubled():int { return count * 2; }
    }
    struct Weapon : Item {
        float damage = 5;
        func dps(float rate):float { return damage * rate; }
    }
    class Hero : gameObject {
        Weapon w;
        int total = 0;
        func start() {
            w = Weapon();
            w.count = 3;
            w.damage = 10;
            total = w.doubled();
            w.save();
        }
        func report():float { return w.dps(2); }
    })");
    Check(static_cast<bool>(p), "data objects did not compile");
    Check(p->IsDataObject("Item") && p->IsDataObject("Weapon"), "struct types report as data objects");
    Check(!p->IsDataObject("Hero") && !p->IsGameObject("Item"), "data objects are not behaviors and vice versa");

    Runtime r(p);
    int saves = 0; ObjectRef savedRef{};
    r.SetDataSaveCallback([&](ObjectRef ref){ ++saves; savedRef = ref; });
    auto hero = r.Create("Hero");
    r.Start(hero);
    Check(Int(r.Get(hero, "total")) == 6, "data object method/field access through a behavior field");
    Check(std::get<double>(r.Call(hero, "report")) == 20.0, "inherited + own data-object members work");
    Check(saves == 1 && savedRef.id != 0, "data.save() reached the host callback");

    // A struct cannot be a gameObject, cannot be attached, cannot inherit a behavior.
    Error([&]{ Compile("struct Bad : gameObject { int x; }"); }, "data object can only inherit another data object");
    Error([&]{ Compile("struct Thing { int a; } class Bad : Thing { }"); }, "Only a `struct` can inherit a data object");
    Error([&]{ Compile("struct Bad { export int x; }"); }, "drop 'export'");
    Error([&]{ Compile("struct Bad { private int x; }"); }, "all public");
    Error([&]{ Compile("struct Bad { signal ping; }"); }, "cannot declare signals");
    Error([&]{ Compile("struct Thing { int a; } class Host : gameObject { func f(){ getBehavior(Thing); } }"); }, "native component type");
}
void DataSheet() {
    // ZE-92: data_sheet is a referenceable value type read as sheet[row, column].
    auto p = Compile(R"(class Loot : gameObject {
        export data_sheet weapons;
        int dmg = 0;
        string who = "";
        func start() {
            dmg = weapons["sword", "damage"];
            who = weapons[0, 1];
        }
    })");
    Check(static_cast<bool>(p), "data_sheet script did not compile");
    Runtime r(p);
    r.SetDataSheetCallback([](ObjectRef, const Value& row, const Value& col) -> Value {
        const auto rk = std::holds_alternative<std::string>(row) ? std::get<std::string>(row) : std::to_string(std::get<std::int64_t>(row));
        const auto ck = std::holds_alternative<std::string>(col) ? std::get<std::string>(col) : std::to_string(std::get<std::int64_t>(col));
        if (ck == "damage" || ck == "0") return static_cast<std::int64_t>(7);
        if (ck == "1" || ck == "name") return std::string("Excalibur");
        return {};
    });
    auto loot = r.Create("Loot");
    auto sheet = r.Create("data_sheet"); // a bare sheet ref stands in for a bound asset
    r.Set(loot, "weapons", sheet);
    r.Start(loot);
    Check(Int(r.Get(loot, "dmg")) == 7, "sheet[row, column] int cell did not reach the script");
    Check(std::get<std::string>(r.Get(loot, "who")) == "Excalibur", "sheet[int, int] string cell failed");

    Error([&]{ Compile("class A : gameObject { export data_sheet s; func f(){ int x = s[0]; } }"); }, "sheet[row, column]");
    Error([&]{ Compile("class A : gameObject { export data_sheet s; func f(){ s[0,1] = 3; } }"); }, "read-only");
    Error([&]{ Compile("class A : data_sheet { }"); }, "Cannot inherit native service types");
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "regen") {
        std::ofstream(ZSCRIPT_TYPES_JSON, std::ios::binary) << zengine::script::TypeManifest();
        std::cout << "wrote " << ZSCRIPT_TYPES_JSON << '\n';
        return 0;
    }
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"type manifest is current (ZE-79)", TypeManifestIsCurrent},
        {"Vector2 type", Vector2Type}, {"gameObject2D scripts", GameObject2DScript}, {"UI control classes", UiControlClasses}, {"mouse input", MouseInput}, {"getBehavior", GetBehavior}, {"find_by_type and tags", FindAndTags},
        {"data objects (ZE-91)",DataObjects},
        {"data sheet (ZE-92)",DataSheet},
        {"native type aliases",NativeTypeAliases},{"timers",Timers},{"audioPlayer class",AudioPlayerClass},{"Mathf functions",MathfFunctions},{"Scene service",SceneService},{"prefab references",PrefabReferences},{"text and global transforms", TextAndGlobalTransforms},
        {"parenting and native object lookup", Parenting},
        {"arrays, type tests, local variables", ArraysTypesAndLocals},
        {"signals", Signals},
        {"empty code and comments", EmptyAndComments}, {"movement and Vector3", Movement}, {"lifecycle and inheritance", LifecycleAndInheritance},
        {"scope keywords (ZE-79)", ScopeKeywords},
        {"values and control flow", ValuesAndControlFlow}, {"references and initializers", ReferencesAndInitializers},
        {"compile diagnostics", Diagnostics}, {"runtime limits", TestRuntimeLimits}, {"host boundary", HostBoundary}, {"native defaults and expression depth", InheritedNativeDefaultsAndExpressionDepth}, {"Inspector metadata", InspectorMetadata}, {"Inspector metadata has no execution cost", InspectorMetadataHasNoExecutionCost}, {"Inspector diagnostics", InspectorDiagnostics}, {"example files", Examples}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
