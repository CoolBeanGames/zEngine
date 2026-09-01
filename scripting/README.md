# zEngine scripting runtime (first milestone)

Standalone C++20 compiler and stack bytecode VM. No engine/editor dependencies or downloads. The parent build now links this library through the separate `zEngineScriptHost` adapter; this directory can still be built/tested independently.

## Signals

Declare an instance signal with `signal custom_signal;`. Connect a void function reference (without calling it), then emit arguments: `custom_signal.connect(on_custom); custom_signal.emit(7);`. The recipient could be `func on_custom(int amount) { ... }` or a method on another script-created object, such as `receiver.on_custom`. Signals are inherited and isolated per instance; they cannot be assigned over or exported as scene data.

`connect`, `disconnect`, and `is_connected` take one function reference. Duplicate connections are ignored. Emission is synchronous in connection order and forwards all arguments. All callback signatures are checked before dispatch. Listeners added during emission wait until the next emission; disconnected listeners are skipped. A callback error aborts emission and faults the owning engine behavior normally. Recursion shares the VM instruction/depth limits; each VM permits at most 4,096 connections by default.

`transform.was_moved`, `transform.was_rotated`, and `transform.was_scaled` pass the new **local Vector3**. Script assignments emit immediately, including individual vector components; unchanged assignments do not emit. Native/other-behavior changes are observed before the listener's next scheduled callback (at least its next Update tick, even with an empty update body). Authoring and initial transform synchronization do not emit. Play/Stop rebuilds connections. `parent` and `find(name)` expose other scene objects' native transforms; lookup of attached behavior methods and a connection UI remain deferred. References to script-created objects within one VM also work.

## Scene parenting

### Global transform reads

`transform.global_position`, `transform.global_rotation`, and `transform.global_scale` are read-only `Vector3` properties available only in scripts, not built-in Inspector controls. Reads compose the current parent chain, including changes made earlier in the same function. Position follows the exact local-to-world matrix; rotation composes parent/local rotations and returns equivalent XYZ Euler angles in degrees. Scale contains the world matrix's axis magnitudes, with an odd reflection assigned to X. Rotated non-uniform scales can introduce shear, which cannot be represented by three scale values. Zero scale is supported. These are value snapshots: changing a local variable holding a result does not change the transform.

```cpp
Vector3 position = transform.global_position;
Vector3 rotation = parent.transform.global_rotation;
Vector3 scale = transform.global_scale;
```

### Strings, characters, and multiline fields

Strings are UTF-8 values; `char` is one Unicode scalar value, written with single quotes. Indexing is zero-based by Unicode scalar, not UTF-8 byte or UTF-16 unit (combining marks count separately). Indexed characters are read-only. Negative/out-of-range indices are errors. Single/double quotes and backslashes can be escaped; `\n`, `\r`, and `\t` work in both literal types.

```cpp
multiline export string description = "First line\nSecond line";
export char initial = 'A';

func edit_text() {
    string title = "Hello" + ' ' + "world";
    title &= '!';                       // Single & also concatenates text.
    char first = title[0];
    bool matches = first == 'H';
    bool ordered = "apple" < "banana";
    title = title.truncate(5);           // Returns "Hello"; assign to keep it.
    string middle = title.substr(1, 3);  // "ell": start, character count.
    int length = title.size();
}
```

`+`, `&`, `+=`, and `&=` combine strings/characters, always producing a string. There is no implicit numeric-to-text conversion or bitwise meaning for `&`; `&&` remains short-circuit boolean AND. `char` can initialize/assign a string, but a string must be indexed to produce a character. Text comparisons (`== != < <= > >=`) are case-sensitive ordinal comparisons, without locale sorting or Unicode normalization. Strings are values: `truncate(maxLength)` and `substr(start,count)` return new strings, never mutate the receiver. Lengths must be nonnegative; excessive lengths clamp to the available text, but a substring start past the end is an error.

Use `multiline export string` or `export multiline string` for a wrapping multiline Inspector field with Enter/newlines and a vertical scrollbar. The tag is class-only, must accompany `export`, and only applies to strings. Untagged strings stay single-line. Multiline text and exported characters persist in scenes/prefabs and standalone builds. The editor normalizes CRLF to LF when committing multiline text. Runtime strings retain the existing 1 MiB default limit; multiline Inspector entry is limited to 65,536 UTF-16 code units. An uninitialized `char` is the null character, shown as `\0` in the Inspector.

### Parent references

The built-in `gameObject` has a writable `parent : gameObject`, null by default. In an engine behavior, use `parent = find("Platform");`, `this.parent = null;`, or `someObject.parent = otherObject;`. `find` resolves an exact, unique scene object name; no match returns null and duplicate names are an error. Local transforms remain unchanged by reparenting. Do not construct a new `gameObject()` as a native parent: script-created objects are not scene objects. Cycles and hierarchies deeper than 64 parent links are rejected before native changes are applied. Play/Stop restores authored parenting and transforms.

### Prefab references and spawning

`prefab` is an engine-provided asset-reference type. Export it to get a prefab picker in the Inspector; the editor also accepts dropping a `.zprefab` directly onto that field. It cannot be constructed in script.

```cpp
class CrateSpawner : gameObject {
    export prefab crate;
    func start() {
        gameObject made = crate.spawn();
        made.transform.position = transform.position;
        made.parent = this;
    }
}
```

`spawn()` takes no arguments, instantiates the complete resolved prefab hierarchy in the active scene, and returns its root `gameObject`. Calling it with an unassigned reference or in a VM host without a prefab-spawn callback is a runtime error. The editor removes runtime-spawned objects on Stop; standalone games use the same packaged prefab loader.

The standalone VM has no scene dependency: embedders supply `Runtime::SetObjectLookup` returning same-runtime object references. Without a callback `find` returns null. The engine host creates scene proxies lazily, synchronizes them at behavior callbacks, and validates native parent and transform changes together. Reentrant proxy construction shares the active script's instruction budget. Scene objects' attached scripts are independent VMs; scene proxies expose native `transform`, `parent`, and `find`, not arbitrary attached script members.

See `examples/SignalBehavior.zsh` for a custom signal and transform listener you can attach and test in Play.

### Native behavior references

Native physics components are script types: `Behavior`, `PhysicsBody`, `RigidBody`, `KinematicBody`, `StaticBody`, `Area`, and `Collider`. A GameObject exposes read-only `physics` plus the typed references `rigidbody`, `kinematic_body`, `static_body`, `area`, and `collider`; a reference is `null` when that component is not attached. These types can be used for fields, locals, parameters, return values, inheritance, and `is` checks. Native instances are supplied by the engine and cannot be constructed directly.

```cpp
class BallController : RigidBody {
    func start() {
        launch(rigidbody);
    }
    func launch(RigidBody body) {
        body.velocity = transform.forward * 5;
        body.add_impulse(transform.up * 2);
    }
}
```

Deriving from a native behavior still produces a normal GameObject lifecycle script. Attach the matching native component to the same GameObject; otherwise using its native methods reports that no active physics body exists. `PhysicsBody` remains useful when code accepts any body type.

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

- Types: signed 64-bit `int`, 64-bit `float`, `bool`, UTF-8 `string`, Unicode `char`, value-type `Vector3`, heterogeneous `array`, engine `prefab` references, and class reference types. `void` is only a return type. Integer to float widening is implicit; narrowing is rejected.
- Fields, locals, and parameters use `type name`; initializers use `= value`. Statements end with `;`; bodies use braces. Uninitialized scalar/vector fields and locals are zero/false/empty, and class references are `null`.
- Classes introduce type names and support single inheritance. All members are public. Overrides must preserve the full signature and dispatch through the actual object type, including through base references. No overloads, constructors with parameters, access modifiers, or `super` yet.
- Construct objects with `ClassName()`. Construct vectors with `Vector3()` or `Vector3(x, y, z)`. Vectors copy by value; class variables share object references. `this` names the current instance.
- Member access and assignment use `.`. Compound `+=`, `-=`, `*=`, `/=` evaluate the destination receiver once. Vectors support addition/subtraction, negation, scalar multiply/divide, component-wise Vector3 multiplication, equality, and readable/writable `x`, `y`, `z` components.
- Arithmetic, comparisons, short-circuit `&&`/`and`, `||`/`or`, logical `nor`, unary `!`/`not`, `!=`, `if`/`else`, conditional `while` and `for` loops, lexical local scopes, and `return` are supported. Conditions require `bool`. `for (condition)` is a condition-controlled loop equivalent to `while (condition)`; it intentionally does not use the C-style initializer/step header. `a nor b` is equivalent to `not (a or b)` and short-circuits when `a` is true. Integer division truncates toward zero.
- `func name(params) : return_type` declares a result; omitting the suffix means `void`. Non-void functions must return on every path; an empty non-void function is an error.
- `//` line comments and `/* ... */` non-nested block comments are ignored outside string literals. Unterminated block comments are diagnosed. Strings support escaped quotes, backslashes, newline, carriage-return, and tab escapes.
- Top-level free functions, imports, exceptions, hot reload, filesystem/network access, and native function registration are not implemented. Put behavior functions inside a class.

### Arrays, type tests, and locals

```cpp
array values = [1, "hello", Vector3(1, 2, 3)];
func inspect() {
    int index = 0; // Local to this call; not an Inspector field.
    if (values[index] is int) {
        int number = values[index];
        values[index] = number + 1;
    }
    values.append(true);
    values.erase(1); // Remove by zero-based index.
    int count = values.size();
}
```

`value is Type` returns bool and checks the actual stored type: `1 is int` is true and `1.0 is int` is false. Class tests also accept inherited base types. `null is null` is true; null is not an instance of a class. Arrays can nest, mix types, and share references when assigned or passed to functions. Bounds, integer indices, and assignments into typed variables are checked at runtime. Array identity, not recursive contents, defines equality. An uninitialized array starts empty. Array storage is owned by the VM and bounded by `RuntimeLimits` (10,000 arrays / 100,000 total elements by default); Play/Stop releases it. Exported arrays are currently read-only in the Inspector and are initialized from source, not serialized as authored overrides.

All variable types support `type name = value` both at class scope and inside functions. Function locals are recreated per call, scoped to their brace block, may shadow class fields (`this.name` still accesses the field), and cannot be accessed from another function. `export` and `label` inside a function are errors.

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

Labels require a string literal; their trailing semicolon is optional (`label("Movement")`). Normal string escapes are supported. Labels do not implicitly export fields, create variables, or generate bytecode. Repeated/empty labels are retained. Exporting changes only Inspector visibility, not script access, field type, initialization, storage, or execution. All current field types can be exported, including object references; rendering suitable controls is the host editor's responsibility.

`Program::InspectorLayout(className)` returns an immutable ordered list of `InspectorEntry` records: `Kind::Label` carries `text`; `Kind::Field` carries the exported field `name` and canonical `type`. Each record includes `declaringClass`, `source`, and one-based `line`/`column` (field-name location for fields, keyword location for labels). Labels have empty name/type, and fields have empty text. Hidden fields and built-in transform fields are omitted. Inherited entries appear first, followed by the derived class's entries in source order. The reference remains valid while its Program lives. Unknown class names throw ScriptError; valid classes without annotations return an empty list.

The main Inspector should iterate this list, draw labels as text, and show controls only for field entries. Field names work with Runtime::Get/Set for initialized instances; the metadata does not execute initializers or capture instance values. Persisting authoring overrides, applying them before lifecycle start, and drawing controls belong to engine/editor integration. Native GameObject/Transform controls can remain in their existing separate Inspector section. This scripting change supplies metadata only and does not change any Inspector UI files.

## Empty-code handling

A class containing only empty void functions, including the original start/update/draw template, is valid and produces zero executable instructions and zero executable script classes. Declarations/signatures remain as metadata so the editor and type checker can use the type. An empty override suppresses inherited behavior; it does not fall back to the base implementation. Inherited script fields or executable methods keep a class active. Calls to explicitly requested empty methods are legal no-ops. The host can still explicitly instantiate an empty type; native transform storage is not executable script code.

## Host API and engine integration

## Math helpers

`Mathf` is a built-in static-style service available to every script. It provides `lerp(from, to, weight)`, `sin`, `cos`, `tan`, `sqrt`, `exp`, and `round` for scalar values, plus `dot(a, b)` and `cross(a, b)` for `Vector3` values. Integer arguments are accepted where a float is expected; invalid square roots and non-finite results are reported as script errors.

```cpp
float halfway = Mathf.lerp(0, 10, 0.5);
float facing = Mathf.dot(transform.forward, Vector3(0, 0, 1));
Vector3 normal = Mathf.cross(Vector3(1, 0, 0), Vector3(0, 1, 0));
```

## Timers

GameObject behaviors can create one-shot, runtime-owned timers. A timer is an invisible GameObject that emits its ordinary `finished` signal after the requested number of seconds, then deletes itself. Timer time advances with the owning script runtime's Update calls, so pausing the game also pauses timers.

```cpp
func start() {
    make_timer(1.5).finished.connect(on_delay_finished);
}
func on_delay_finished() {
    // Runs once after 1.5 seconds of game time.
}
```

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

`Start(object)` runs a behavior's start hook at most once. `Update(object, delta)`, `PhysicsUpdate(object, delta)`, and `Draw(object)` ensure Start first, then dispatch the relevant hook if it has code. Update deltas must be finite and nonnegative. The engine schedules `func physicsUpdate(float delta)` after normal Update and immediately before each fixed physics step. These methods do not run automatically in the standalone language library: the host owns scheduling. Creating an object initializes fields, not lifecycle hooks. Explicit `Call` is ordinary method invocation and does not enforce lifecycle-once semantics. A failed Start is not retried; later lifecycle calls report the fault. Other failed calls may be retried and keep any mutations performed before the error.

`Get`/`Set` provide typed field access for the host. Handles belong to one Runtime; cross-runtime handles, null dereferences, incorrect argument types, and invalid members are errors. The runtime owns all instances until destruction, including failed-construction objects, with no per-object deletion or garbage collection yet. This supports cycles without shared-pointer ownership leaks but is intended for a bounded scene/session lifetime. Use one Runtime per execution context; it is not thread-safe. Programs can be shared across independent runtimes.

Execution has configurable instruction, call-depth, object-count, and per-string limits. Source/token/syntax/inheritance limits bound compilation. Integer overflow, division by zero, and non-finite numeric results produce ScriptError with source coordinates. Limits prevent common runaway scripts; this is not a hardened security sandbox or a global memory quota.

## Prompt for the main chat (future integration)

After merging the scripting branch, add `add_subdirectory(scripting)` to the root build and link `zEngineScripting` where needed. Feed .zsh text to Compiler::Compile and map Diagnostic source/line/column/message to editor error highlighting. Use Program::InspectorLayout to display label entries and only exported field entries, with typed controls and persisted per-instance overrides; add export/label to syntax highlighting. Build a scene adapter that maps real GameObject identity to script ObjectRef, associates an attached behavior with its owner, and synchronizes transform values. Drive Start/Update/physicsUpdate/Draw from the engine play lifecycle, report ScriptError, and define object destruction/reload behavior. Do not treat the current runtime-owned transforms as already bound to scene objects.

The engine now implements this integration in `src/ScriptHost.h/.cpp`, `src/core/BehaviorLifecycle.h/.cpp`, and the native editor. See the root README for Play/Pause/Step and exported field editing. `Program::IsGameObject()` validates behavior classes and `Program::HasCode()` reports effective nonempty method bodies (including inheritance/empty overrides). The separate `zEngineScenes` module persists attachments and authored scalar/vector Inspector values in `.zscene` assets; live VM state is never serialized. Native cross-object references are not implemented yet. Runtime-owned transforms remain standalone when using this library without the host adapter.
