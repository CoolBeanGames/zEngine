#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <functional>

namespace zengine::script {

// Handles are valid only in the Runtime that created them. Zero is null.
struct ObjectRef {
    std::uint64_t runtime = 0;
    std::size_t id = 0;
    bool operator==(const ObjectRef&) const = default;
};
struct Vector3 {
    double x = 0, y = 0, z = 0;
    bool operator==(const Vector3&) const = default;
};
struct Vector2 {
    double x = 0, y = 0;
    bool operator==(const Vector2&) const = default;
};
struct SignalRef {
    ObjectRef owner;
    std::string name;
    bool operator==(const SignalRef&) const = default;
};
struct CallableRef {
    ObjectRef owner;
    std::string name;
    bool operator==(const CallableRef&) const = default;
};
struct ArrayRef {
    std::uint64_t runtime = 0;
    std::size_t id = 0;
    bool operator==(const ArrayRef&) const = default;
};
struct PrefabRef {
    std::string asset;
    bool operator==(const PrefabRef&) const = default;
};
using Value = std::variant<std::monostate, std::int64_t, double, bool, std::string, ObjectRef, Vector3, SignalRef, CallableRef, ArrayRef, char32_t, PrefabRef, Vector2>;
struct Diagnostic {
    std::string source;
    std::size_t line = 1;
    std::size_t column = 1;
    std::string message;
};
class ScriptError : public std::runtime_error {
public:
    explicit ScriptError(Diagnostic diagnostic);
    const Diagnostic& Detail() const noexcept { return diagnostic_; }
private:
    Diagnostic diagnostic_;
};
struct ProgramStats {
    std::size_t declaredClasses = 0;
    std::size_t executableClasses = 0;
    std::size_t emittedFunctions = 0;
    std::size_t instructions = 0;
};
// Inspector-only declarations, in base-to-derived and source order.
// Hidden fields are omitted; their script accessibility is unchanged.
struct InspectorEntry {
    enum class Kind { Field, Label };
    Kind kind = Kind::Field;
    std::string name; // Field name; empty for labels.
    std::string type; // Canonical field type; empty for labels.
    std::string text; // Label text; empty for fields.
    std::string declaringClass;
    bool multiline = false;
    std::string source;
    std::size_t line = 1;
    std::size_t column = 1;
};
class Program {
public:
    struct Impl;
    ProgramStats Stats() const;
    bool HasClass(std::string_view name) const;
    bool IsGameObject(std::string_view name) const;
    bool HasCode(std::string_view className, std::string_view method) const;
    // Reference remains valid for this Program lifetime. Unknown classes throw ScriptError.
    const std::vector<InspectorEntry>& InspectorLayout(std::string_view className) const;
private:
    explicit Program(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
    friend class Compiler;
    friend class Runtime;
};
struct CompileResult {
    std::shared_ptr<const Program> program;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return program != nullptr; }
};
class Compiler {
public:
    // One compilation unit may contain multiple classes, including forward references.
    static CompileResult Compile(std::string_view source, std::string sourceName = "<script>");
};
struct RuntimeLimits {
    std::size_t instructionsPerCall = 100000;
    std::size_t callDepth = 128;
    std::size_t objects = 10000;
    std::size_t stringBytes = 1024 * 1024;
    std::size_t signalConnections = 4096;
    std::size_t arrays = 10000;
    std::size_t arrayElements = 100000;
};
struct InputState { double x=0,y=0; bool pressed=false,justPressed=false,justReleased=false; };
using InputFrame = std::map<std::string,InputState>;
struct MouseButtonState { bool pressed=false,justPressed=false,justReleased=false; };
// Host snapshot of the pointer. x/y are the viewport position normalized to -1..1
// (0,0 = centre, +y up). Button 0 = left, 1 = right, 2 = middle. The Runtime derives
// the movement delta and the was_just_moved signal from successive frames.
struct MouseFrame { double x=0,y=0; bool inside=false; std::array<MouseButtonState,8> buttons{}; };
class Runtime {
public:
    explicit Runtime(std::shared_ptr<const Program> program, RuntimeLimits limits = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ObjectRef Create(std::string_view className);
    ObjectRef CreatePrefab(std::string asset);
    std::string PrefabAsset(ObjectRef) const;
    Value Call(ObjectRef object, std::string_view method, const std::vector<Value>& arguments = {});
    Value Get(ObjectRef object, std::string_view field) const;
    void Set(ObjectRef object, std::string_view field, Value value, bool notify = true);
    // Element access for a script `array` value obtained from Get(). Editor-facing:
    // arrays are untyped, so any coercible Value may be stored. Out-of-range indices throw.
    std::size_t ArrayLength(ArrayRef array) const;
    Value ArrayElement(ArrayRef array, std::size_t index) const;
    void SetArrayElement(ArrayRef array, std::size_t index, Value value);
    void AppendArrayElement(ArrayRef array, Value value);
    void RemoveArrayElement(ArrayRef array, std::size_t index);
    void Connect(SignalRef signal, CallableRef callback);
    void Emit(SignalRef signal, const std::vector<Value>& arguments = {});
    // Host snapshot for one fixed tick, before Update. Events share one VM budget.
    void SetInput(const InputFrame& frame, bool emitEvents = true);
    // Pointer snapshot for Input.mouse. Emits clicked/click_ended/held/was_just_moved.
    void SetMouse(const MouseFrame& frame, bool emitEvents = true);
    // Host lookup returns same-VM scene proxies. Missing names return a null reference.
    void SetObjectLookup(std::function<ObjectRef(std::string_view)> lookup);
    // find_by_type(Type): host returns the first scene proxy carrying that native component (null if none).
    void SetTypeLookup(std::function<ObjectRef(std::string_view)> lookup);
    // get_tags()/has_tag(): host returns the tags of the given scene proxy.
    void SetTagLookup(std::function<std::vector<std::string>(ObjectRef)> lookup);
    // get_children(): host returns the direct child scene proxies of the given proxy.
    void SetChildrenLookup(std::function<std::vector<ObjectRef>(ObjectRef)> lookup);
    using PhysicsBodyCall = std::function<Value(ObjectRef, std::string_view, const std::vector<Value>&)>;
    using PhysicsCastCall = std::function<std::vector<ObjectRef>(Vector3, Vector3, std::uint32_t)>;
    using PrefabSpawnCall = std::function<ObjectRef(std::string_view)>;
    // Host-owned physics bridge. Scripts never see the backend physics API.
    void SetPhysicsCallbacks(PhysicsBodyCall bodyCall, PhysicsCastCall castCall);
    // Binds a host-owned native component to a typed, read-only script reference.
    void BindNativeBehavior(ObjectRef owner,std::string_view behaviorType);
    void SetPrefabSpawnCallback(PrefabSpawnCall);
    using PrintCallback=std::function<void(std::string_view)>;
    void SetPrintCallback(PrintCallback);
    // Host-owned scene management for the `Scene` service: `load` switches the
    // running scene by name; `current` returns the active scene's name.
    void SetSceneCallbacks(std::function<void(std::string_view)> load, std::function<std::string()> current);
    // Start is once per instance. Update/Draw ensure Start first. Empty hooks are skipped.
    void Start(ObjectRef object);
    void Update(ObjectRef object, double delta);
    void PhysicsUpdate(ObjectRef object, double delta);
    void Draw(ObjectRef object);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zengine::script
