#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
using Value = std::variant<std::monostate, std::int64_t, double, bool, std::string, ObjectRef, Vector3>;
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
};
class Runtime {
public:
    explicit Runtime(std::shared_ptr<const Program> program, RuntimeLimits limits = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ObjectRef Create(std::string_view className);
    Value Call(ObjectRef object, std::string_view method, const std::vector<Value>& arguments = {});
    Value Get(ObjectRef object, std::string_view field) const;
    void Set(ObjectRef object, std::string_view field, Value value);
    // Start is once per instance. Update/Draw ensure Start first. Empty hooks are skipped.
    void Start(ObjectRef object);
    void Update(ObjectRef object, double delta);
    void Draw(ObjectRef object);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zengine::script
