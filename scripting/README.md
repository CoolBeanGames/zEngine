# zEngine scripting runtime (first milestone)

Standalone C++20 compiler and stack bytecode VM. No engine/editor dependencies or downloads. The parent build now links this library through the separate `zEngineScriptHost` adapter; this directory can still be built/tested independently.

## Build and test

From this directory in a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Language

`.zsh` is the editor asset extension; the compiler accepts source text independently of file extension. One compilation unit can contain multiple classes, with forward references. Keywords and identifiers are case-sensitive except that `GameObject` and `gameObject` deliberately name the same built-in base type.

```cpp
class Mover : gameObject
{
    Vector3 velocity = Vector3(1, 0, 0);

    // Start runs once when the host starts this instance.
    func start() {}

    func update(float delta)
    {
        move(this, velocity * delta);
    }

    /* Empty draw hooks generate no executable code. */
    func draw() {}

    func move(GameObject obj, Vector3 amount)
    {
        obj.transform.position += amount;
    }

    func height() : float
    {
        return transform.position.y;
    }
}
```

- Types: signed 64-bit `int`, 64-bit `float`, `bool`, UTF-8 byte `string`, value-type `Vector3`, and class reference types. `void` is only a return type. Integer to float widening is implicit; narrowing is rejected.
- Fields, locals, and parameters use `type name`; initializers use `= value`. Statements end with `;`; bodies use braces. Uninitialized scalar/vector fields and locals are zero/false/empty, and class references are `null`.
- Classes introduce type names and support single inheritance. All members are public. Overrides must preserve the full signature and dispatch through the actual object type, including through base references. No overloads, constructors with parameters, access modifiers, or `super` yet.
- Construct objects with `ClassName()`. Construct vectors with `Vector3()` or `Vector3(x, y, z)`. Vectors copy by value; class variables share object references. `this` names the current instance.
- Member access and assignment use `.`. Compound `+=`, `-=`, `*=`, `/=` evaluate the destination receiver once. Vectors support addition/subtraction, negation, scalar multiply/divide, equality, and readable/writable `x`, `y`, `z` components.
- Arithmetic, comparisons, short-circuit `&&`/`||`, unary `!`, `if`/`else`, `while`, lexical local scopes, and `return` are supported. Conditions require `bool`. Integer division truncates toward zero.
- `func name(params) : return_type` declares a result; omitting the suffix means `void`. Non-void functions must return on every path; an empty non-void function is an error.
- `//` line comments and `/* ... */` non-nested block comments are ignored outside string literals. Unterminated block comments are diagnosed. Strings support escaped quotes, backslashes, newline, carriage-return, and tab escapes.
- Top-level free functions, imports, arrays, exceptions, hot reload, filesystem/network access, and native function registration are not implemented. Put behavior functions inside a class.

## Inspector declarations

Class fields are hidden from the script Inspector by default. Prefix a field with `export` to expose it. Add `label("text");` between declarations to insert organizational text. These are class-level declarations, not executable statements; they cannot appear in functions or modify parameters. `export` applies only to the immediately following typed field. Both keywords are reserved.

```cpp
class Movement : gameObject
{
    label("Movement");
    export float speed = 5;
    export Vector3 direction = Vector3(1, 0, 0);
    float elapsed = 0; // Hidden, but still accessible to scripts.

    label("Debug");
    export bool showDebug = false;
}
```

Labels require a string literal and a trailing semicolon. Normal string escapes are supported. Labels do not implicitly export fields, create variables, or generate bytecode. Repeated/empty labels are retained. Exporting changes only Inspector visibility, not script access, field type, initialization, storage, or execution. All current field types can be exported, including object references; rendering suitable controls is the host editor's responsibility.

`Program::InspectorLayout(className)` returns an immutable ordered list of `InspectorEntry` records: `Kind::Label` carries `text`; `Kind::Field` carries the exported field `name` and canonical `type`. Each record includes `declaringClass`, `source`, and one-based `line`/`column` (field-name location for fields, keyword location for labels). Labels have empty name/type, and fields have empty text. Hidden fields and built-in transform fields are omitted. Inherited entries appear first, followed by the derived class's entries in source order. The reference remains valid while its Program lives. Unknown class names throw ScriptError; valid classes without annotations return an empty list.

The main Inspector should iterate this list, draw labels as text, and show controls only for field entries. Field names work with Runtime::Get/Set for initialized instances; the metadata does not execute initializers or capture instance values. Persisting authoring overrides, applying them before lifecycle start, and drawing controls belong to engine/editor integration. Native GameObject/Transform controls can remain in their existing separate Inspector section. This scripting change supplies metadata only and does not change any Inspector UI files.

## Empty-code handling

A class containing only empty void functions, including the original start/update/draw template, is valid and produces zero executable instructions and zero executable script classes. Declarations/signatures remain as metadata so the editor and type checker can use the type. An empty override suppresses inherited behavior; it does not fall back to the base implementation. Inherited script fields or executable methods keep a class active. Calls to explicitly requested empty methods are legal no-ops. The host can still explicitly instantiate an empty type; native transform storage is not executable script code.

## Host API and engine integration

Include `zscript/Script.h` and link `zEngineScripting`. `Compiler::Compile(text, filename)` returns either an immutable program or a diagnostic with source name and one-based line/column. This first version reports the first compile error. `Program::Stats()` exposes declaration and emitted-code counts.

```cpp
using namespace zengine::script;
auto result = Compiler::Compile(sourceText, "Mover.zsh");
if (!result) { /* display result.diagnostics */ }
else {
    Runtime runtime(result.program);
    auto behavior = runtime.Create("Mover");
    auto target = runtime.Create("GameObject");
    runtime.Call(behavior, "move", {target, Vector3{1, 0, 0}});
    auto transform = std::get<ObjectRef>(runtime.Get(target, "transform"));
    auto position = std::get<Vector3>(runtime.Get(transform, "position"));
    // Forward position to the real scene through a future engine adapter.
}
```

Every runtime GameObject has a Transform with `position`, `rotation`, and `scale` Vector3 fields. Position/rotation default to zero, scale to one. These are standalone data fields, NOT bindings to the engine's real Transform. Rotation interpretation and coordinate conversion belong to the future adapter. Native transform objects count toward the runtime object limit.

`Start(object)` runs a behavior's start hook at most once. `Update(object, delta)` and `Draw(object)` ensure Start first, then dispatch the relevant hook if it has code. Update accepts finite, nonnegative delta. These methods do not run automatically: the host owns scheduling. Creating an object initializes fields, not lifecycle hooks. Explicit `Call` is ordinary method invocation and does not enforce lifecycle-once semantics. A failed Start is not retried; later lifecycle calls report the fault. Other failed calls may be retried and keep any mutations performed before the error.

`Get`/`Set` provide typed field access for the host. Handles belong to one Runtime; cross-runtime handles, null dereferences, incorrect argument types, and invalid members are errors. The runtime owns all instances until destruction, including failed-construction objects, with no per-object deletion or garbage collection yet. This supports cycles without shared-pointer ownership leaks but is intended for a bounded scene/session lifetime. Use one Runtime per execution context; it is not thread-safe. Programs can be shared across independent runtimes.

Execution has configurable instruction, call-depth, object-count, and per-string limits. Source/token/syntax/inheritance limits bound compilation. Integer overflow, division by zero, and non-finite numeric results produce ScriptError with source coordinates. Limits prevent common runaway scripts; this is not a hardened security sandbox or a global memory quota.

## Prompt for the main chat (future integration)

After merging the scripting branch, add `add_subdirectory(scripting)` to the root build and link `zEngineScripting` where needed. Feed .zsh text to Compiler::Compile and map Diagnostic source/line/column/message to editor error highlighting. Use Program::InspectorLayout to display label entries and only exported field entries, with typed controls and persisted per-instance overrides; add export/label to syntax highlighting. Build a scene adapter that maps real GameObject identity to script ObjectRef, associates an attached behavior with its owner, and synchronizes transform values. Drive Start/Update/Draw from the engine play lifecycle, report ScriptError, and define object destruction/reload behavior. Do not treat the current runtime-owned transforms as already bound to scene objects.

The engine now implements this integration in `src/ScriptHost.h/.cpp`, `src/core/BehaviorLifecycle.h/.cpp`, and the native editor. See the root README for Play/Pause/Step and exported field editing. `Program::IsGameObject()` validates behavior classes and `Program::HasCode()` reports effective nonempty method bodies (including inheritance/empty overrides). Native cross-object references and scene persistence are not implemented yet; runtime-owned transforms remain standalone when using this library without the host adapter.
