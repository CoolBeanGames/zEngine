#include "zscript/Script.h"
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
    Error([&]{Compile("class A : gameObject {func f(){transform.global_position.x=1;}}");},"read-only");
    const auto at=TransformOf(r,a),pt=TransformOf(r,parent);r.Set(a,"parent",parent);
    r.Set(pt,"position",Vector3{10,0,0});r.Set(pt,"rotation",Vector3{0,0,90});r.Set(pt,"scale",Vector3{2,3,4});r.Set(at,"position",Vector3{1,0,0});
    auto position=std::get<Vector3>(r.Get(at,"global_position")),rotation=std::get<Vector3>(r.Get(at,"global_rotation")),scale=std::get<Vector3>(r.Get(at,"global_scale"));
    Check(std::abs(position.x-10)<1e-8 && std::abs(position.y-2)<1e-8 && std::abs(rotation.z-90)<1e-8 && scale==Vector3{2,3,4},"Global TRS does not compose local-to-parent transforms");
    r.Set(a,"parent",ObjectRef{});Check(std::get<Vector3>(r.Get(at,"global_position"))==Vector3{1,0,0},"Global transform cached stale parent");
    Error([&]{r.Set(at,"global_scale",Vector3{});},"read-only");
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
    })");
    Runtime runtime(program);const auto object=runtime.Create("MathTest");
    Check(std::get<bool>(runtime.Call(object,"scalar")),"Mathf scalar methods returned incorrect values");
    Check(std::get<bool>(runtime.Call(object,"vector")),"Mathf vector methods returned incorrect values");
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
void NativeTypeAliases() {
    auto program=Compile(R"(class Native : rigidbody {
        rigidbody saved; staticbody static_saved; kinematicbody kinematic_saved; physicsbody physics_saved; collider collider_saved; area area_saved; behavior behavior_saved; transform transform_saved;
        func take(rigidbody body){saved=body;physics_saved=body;behavior_saved=body;transform_saved=body.transform;}
        func valid():bool{return saved is rigidbody && saved is physicsbody && saved is behavior && saved is gameobject && physics_saved is RigidBody && transform_saved is Transform;}
    })");
    Runtime runtime(program);const auto object=runtime.Create("Native");runtime.Call(object,"take",{object});
    Check(std::get<bool>(runtime.Call(object,"valid")),"Lowercase native behavior aliases were not canonicalized consistently");
    Check(program->HasClass("rigidbody") && program->IsGameObject("staticbody"),"Native aliases were not recognized by Program type queries");
}
int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"native type aliases",NativeTypeAliases},{"timers",Timers},{"Mathf functions",MathfFunctions},{"prefab references",PrefabReferences},{"text and global transforms", TextAndGlobalTransforms},
        {"parenting and native object lookup", Parenting},
        {"arrays, type tests, local variables", ArraysTypesAndLocals},
        {"signals", Signals},
        {"empty code and comments", EmptyAndComments}, {"movement and Vector3", Movement}, {"lifecycle and inheritance", LifecycleAndInheritance},
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
