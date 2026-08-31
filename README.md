# zEngine

zEngine currently contains a small Direct3D 11 renderer hosted inside a lightweight native Windows editor shell. Startup restores the last project and scene, or prompts to create/open one. New scenes start empty. Add a Mesh Renderer and choose **Use Cube** to render the built-in indexed, depth-tested cube. Each cube vertex receives a random color when the program starts, and a directional light is calculated in the vertex shader. The cube/model is controlled by its GameObject transform instead of automatically rotating.

The editor provides an options bar, resizable scene browser, resizable inspector, resizable media library, central scene viewport, and status/progress bar. Drag the narrow gaps between panels to resize them. Projects, scene assets, the File menu, FBX import, multi-object mesh rendering, GameObject creation, script editing/execution, Inspector variables, and Play/Pause/Step are functional. The other top options menus and Add Folder remain placeholders.

## Projects

- **File > New Project...** asks for a project name and parent location (type a path or use Browse). It creates `<location>/<name>/<name>.zproject` and `<location>/<name>/Assets`. Existing folders are never overwritten. A follow-up prompt lets you create a scene or open one inside Assets.
- **File > Open Project...** opens a `.zproject` config and restores its last-opened scene. The project name appears in the options bar and window title. Scenes, scripts, FBX packages, and their albedo textures all belong to that project's Assets folder; switching projects replaces the library, scene tree, Inspector, script windows, and cached render bindings.
- The config stores a version, name, owned Assets folder, scene list, and last-opened scene. Creating, opening, or saving a scene records it automatically. Scene references are project-relative: move/copy the whole project folder together, then open its config at the new location. Externally copied scene files become visible after Refresh Assets and enter the config's list when opened.
- Startup remembers the most recently opened project in `%LOCALAPPDATA%/zEngine/editor.state`. It restores the last scene worked on, not an arbitrary scene from the list. Missing/corrupt settings or project files prompt to create/open a project; an unavailable last scene prompts to create/open a scene. Cancel leaves the editor/project open without creating files. Assets and objects cannot be created without an active project/scene, respectively.
- Switching projects protects unsaved scripts and scenes with Save/Discard/Cancel prompts. Stop Play and wait for imports/model loads before switching. Scene saves remain explicit (**Ctrl+S**); remembering the current scene does not autosave its edits. Config saves detect external changes and use atomic replacement. A config/settings write failure is reported in the status bar without discarding a successfully saved scene.

`zEngineProjects` is a lightweight, independent config/path/persistence module with no renderer, scene-runtime, or scripting dependency. `EditorProjects.cpp` connects it to the editor, and `ProjectDialog` supplies the native creation dialog. The config is bounded versioned UTF-8 text (`ZENGINE_PROJECT 1`), with up to 10,000 scene references and a 1 MiB file limit. Assets and scene references cannot escape the project through parent traversal or filesystem links.

## GameObjects and transforms

- Click **+ Create Empty** in the Scene Browser to create and select a GameObject. Click its scene-tree row to select it again; scroll over the tree to browse longer lists.
- Edit its **Name** and comma-separated **Tags** in the Inspector. Tags are trimmed, case-sensitive, and deduplicated. Names must not be blank.
- Click a transform number to select its text and type a value. Valid values apply immediately. **Enter** commits; **Escape** restores the value from before the edit; **Tab/Shift-Tab** advances between fields. Incomplete/invalid numbers are shown with a red background and do not overwrite the last valid transform; leaving the field restores that valid number.
- Press and drag horizontally on a numeric field: right increases, left decreases. Position/scale change by 0.01 per pixel and rotation by 0.5 degrees per pixel. Hold **Shift before dragging** for ten-times finer changes. Escape or losing mouse capture cancels an unfinished drag.
- Position is world-space; rotation uses X, then Y, then Z Euler angles in degrees; scale is per-axis. Zero and negative scales are supported. Inspector values are bounded to +/-1,000,000 and must be finite. Small inspector panels scroll to keep fields accessible.

An empty object contains only its transform and metadata: it has no mesh and no behaviors by default. The selected object's **editor-only transform handles** use X red, Y green, and Z blue against a world grid. These guides are not runtime GameObject components. A cube is a regular GameObject with a Mesh Renderer behavior. Moving one object never changes another object's transform. Moving objects far from the fixed camera can move them out of view; camera navigation is not implemented yet.

GameObjects, attachments, priorities, mesh assignments, and authored Inspector variables persist when you save the current **scene asset**. Parenting, scene undo/redo, and clicking meshes to select objects are still deferred.

### Viewport transform tools

- Select an object in the Scene Browser, then choose **Move W**, **Rotate E**, or **Scale R** in the top toolbar. W/E/R also switch tools when the viewport or main editor has keyboard focus; typing in Inspector/script fields is unaffected.
- **Move:** drag a colored arrow to move along world X, Y, or Z. **Rotate:** drag a colored ring to change that Inspector Euler angle, following the engine's X-then-Y-then-Z rotation order. Near edge-on rings use a tangential drag fallback. **Scale:** drag a box-ended local axis outward/inward to increase/decrease that scale component. Zero and negative scale values remain supported and recoverable.
- The hovered/active axis turns yellow. Handles have an approximately constant screen size independent of mesh size and object scale. They render over geometry so even an empty or zero-scale object can be edited. The default camera uses a slight three-quarter angle so the world axes do not overlap head-on.
- Changes update the model and Inspector live. Release the mouse to finish; save the scene to persist the transform. **Escape**, capture/focus loss, resizing the editor, or changing tools cancels an unfinished drag and restores its original transform without clearing earlier unsaved edits. Gizmo dragging is disabled during Play.

`zEngineGizmos` contains shared handle geometry, projection, hit testing, and drag math without a GPU/window dependency. `ViewportCamera` keeps renderer and picking projection consistent. `EditorGizmos.cpp` owns selection, native input, and drag transactions. The renderer draws the same bounded geometry used for picking through a small reusable vertex buffer; no external gizmo/UI framework is required. Plane handles, uniform-scale handles, snapping, and scene undo/redo are not implemented yet.

## Scene assets

- Click **+ New Scene** in the Media Library, use **File > New Scene**, or right-click the library and choose **Create Scene (.zscene)**. This creates a uniquely named `NewScene.zscene` (numbered as needed) inside the current project's `Assets` directory, registers it in the project config, and opens it with an empty Scene Browser.
- **Double-click a scene asset** to open it, or use **File > Open Scene...**. Opening replaces the Scene Browser contents, Inspector selection/data, scripts, and rendered meshes with that scene's objects. The scene name appears in the window title and tree root; `*` indicates unsaved edits. New scenes start empty, with no implicit cube.
- Use **File > Save Scene / Ctrl+S** to save. The first save lets you choose a `.zscene` filename inside Assets. **Save Scene As... / Ctrl+Shift+S** saves a separate scene asset. Ctrl+S in a script editor continues to save that script, not the scene. Scene saves are explicit, not autosaves.
- Switching scenes or closing the editor prompts to **Save, Discard, or Cancel** when authored scene data changed. Failed/malformed loads leave the current scene intact. Saves use a temporary file and atomic replacement; external changes to the loaded scene are detected instead of silently overwritten. Use Save As to preserve your local version when the original changed externally.
- A scene stores object order and stable IDs, names, tags, all nine transform values, behavior order/enabled flags/priorities, Mesh Renderer asset references, script attachments, and authored exported `int`, `float`, `bool`, `string`, and `Vector3` values (including current defaults). Each object's script values are independent. Script source, labels, and hidden-field initialization stay in the referenced `.zsh` asset. GPU resources, editor guide axes, runtime counters/VM state, and runtime object-reference handles are not scene data.
- **Play runs the currently open scene**, including unsaved authoring edits. Stop restores its authored transforms, names/tags, priorities/enabled flags, and script variables. Scene saves/switching and structural object/model edits are blocked during Play so temporary runtime edits cannot contaminate the saved scene. Stop before switching or saving. Model loading must finish before Play; pending authored model assignments must finish before saving/switching.
- Imported meshes are reloaded from project-relative references. An old background load cannot attach to a different scene, even if object IDs match. Missing mesh/script files leave their references and saved script data intact so you can repair the asset; they do not prevent opening the scene. Missing/broken attached scripts prevent Play and report an error.

Scene files are bounded, versioned UTF-8 text (`ZENGINE_SCENE 1`), currently limited to 8 MiB, 10,000 objects, 256 behaviors/tags per object, and 1,024 saved variables per behavior. Invalid IDs, duplicate mesh components, unknown behavior/value types, nonfinite numbers, path traversal, truncation, and unsupported versions are rejected. `zEngineScenes` contains the scene document, codec, and object/behavior reconstruction with no Win32 or rendering dependencies. `zEngineSceneAssets` provides project-contained file I/O and atomic saves. The editor supplies scene selection and render bindings. No third-party serialization dependency is required.

The current tree remains flat; parent/child relationships and cross-object script reference serialization will come with their corresponding systems. The last scene is reopened automatically through its project config on editor startup.

### Module boundaries

`zEngineCore` owns platform-independent `GameObject`, `Transform`, `ObjectStore`, and `Behavior` types. Every object has a stable ID, UTF-8 name, tag list, and non-removable transform. `ObjectStore` keeps object addresses stable, and objects own attached behavior lifetimes. `Behavior::Owner()`, `Enabled()`, `AddBehavior<T>()`, and `GetBehavior<T>()` provide the C++ extension/ownership foundation without introducing a scripting runtime.

`InspectorPanel` is a reusable native editor widget bound to a GameObject; it does not depend on the FBX importer or renderer. `RenderScene.h` defines per-frame mesh handles and copied transforms, and `RenderTransform.h` adapts transforms to DirectX matrices. The renderer does not own or reference GameObjects. Nonuniform scaling uses inverse-transpose normals.

This follows the component ownership pattern described in Unity's [GameObject reference](https://docs.unity3d.com/6000.0/Documentation/Manual/class-GameObject.html) and [MonoBehaviour reference](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/MonoBehaviour.html), with a tag **list** as required by zEngine rather than Unity's single-tag model.

## Script assets and editor

- Click **+ New Script** in the Media Library, or right-click the library and choose **Create Behavior Script (.zsh)**. This creates `NewBehavior.zsh` (numbered automatically when that name exists) under the current project's `Assets` folder, with `start`, `update(float delta)`, and `draw` hooks, and opens it for editing.
- Double-click a `.zsh` asset to open its resizable script window. Each file has a single editor window. Keywords, built-in types, numbers, strings, and comments are colored; **Tab** inserts four spaces and **Ctrl+Z/Ctrl+Y** undo/redo text edits.
- Use **Save / Ctrl+S** to save UTF-8 text, and **Reload / Ctrl+R** to load disk changes. A `*` in the title marks unsaved edits. Closing a script or the main editor asks whether to save, discard, or cancel. Saves use a temporary file followed by replacement and reject overwriting a file changed externally since loading. Invalid source can still be saved while you work.
- Full compiler diagnostics (unknown names, type mismatches, missing semicolons, etc.) and lexical checks show an error background/underline with source line/column messages. **Go to first error** selects and scrolls to the first problem. `export`, `label`, `GameObject`, `Transform`, and `Vector3` are syntax-highlighted.
- Select a GameObject and click **+ Add Script** in the Inspector, then choose a `.zsh` file inside this project's Assets directory. Alternatively, drag a library script onto a specific **Scene Browser object row** or the selected object's **Inspector**. Multiple different scripts are supported; duplicate attachments are rejected. Behavior names appear as bold headers, three points larger than body text, without asset paths or file extensions. Script labels retain the body text size but are bold.
- Right-click **Refresh Assets** to discover `.zsh` files added externally (including subfolders); existing scripts are also discovered on startup. External script drops/import, script renaming, and behavior removal are not implemented yet.

Script files persist on disk; attachments and authored Inspector values persist in the saved scene. Attaching a script compiles its metadata and initializes an isolated preview for Inspector defaults, but does **not run lifecycle hooks**. Hooks run only in Play mode. `.zsh` here is zEngine source, not a shell script, and is never launched through the OS.

The lightweight `zEngineScriptTools` library owns script file operations, diagnostics, and the native Windows RichEdit editor. `ScriptBehavior` in `zEngineCore` stores a project-relative asset reference and an optional abstract script instance. Neither the renderer nor core depends on the editor or compiler. Source files are limited to 256 KiB, lexical diagnostics to 100 errors, and coloring to 8,000 tokens per refresh. Syntax coloring is debounced by 250 ms and excluded from text undo history.

### Run a movement script

Create `NewBehavior.zsh`, replace its contents with the following, save it, and attach it to the Color Cube. Keep the behavior class name identical to the filename without `.zsh` (numbered files need the matching numbered class).

```cpp
class NewBehavior : gameObject
{
    label("Movement");
    export float speed = 1;
    export Vector3 direction = Vector3(1, 0, 0);
    export bool moving = true;

    float elapsed = 0; // Normal field: usable in code, hidden in the Inspector.

    func start()
    {
        elapsed = 0;
    }

    func update(float delta)
    {
        elapsed += delta;
        if (moving)
        {
            transform.position += direction * speed * delta;
            transform.rotation.y += 30 * delta;
        }
    }
}
```

- **Play** (triangle in the Scene header) creates independent runtime instances, applies authored variable overrides, and runs nonempty `start` hooks once. Compile/initialization failures prevent the scene from starting. Save dirty script documents before Play.
- The triangle becomes **Stop** (square). Stop destroys runtime instances and restores the transforms captured at Play, discarding runtime variable changes. **Pause** freezes Update and Draw; **Step** advances one 1/60-second Update and one Draw on the next render.
- Nonempty `update(float delta)` runs at a fixed 60 Hz; `delta` is seconds. Catch-up is capped to 0.1 seconds per frame to avoid a backlog after stalls. Nonempty `draw()` runs once per rendered frame only if the owner has an enabled Mesh Renderer with a loaded mesh submitted to that frame. Editor axes/grid do not qualify. This is submission gating, not pixel-visibility/occlusion testing.
- Each behavior has a finite floating-point **Priority**, default 0. Higher runs first, including fractional/negative values. Equal-priority behaviors are shuffled independently for Start batches, Update ticks, and Draw phases. Mid-phase priority changes apply next phase. Native behaviors start immediately after full construction/ownership; script instances start when bound for Play.
- `export` exposes `int`, `float`, `bool`, `string`, and `Vector3` fields. Enter booleans as `true`/`false`, vectors as `x, y, z`, and strings without surrounding quotes. `label("text");` inserts text in declaration order, with inherited entries first. Other fields remain hidden. Exported class references are shown read-only; native cross-object reference assignment is not available yet.
- Valid edits apply immediately. Invalid/incomplete entries turn red without changing the last valid value. Enter or leaving the field commits/reverts invalid text; Escape cancels, Tab/Shift-Tab moves between fields. Authored values survive Stop and compatible source saves; save the scene to persist them across editor restarts. Changes made during Play are temporary. Defaults for newly attached instances still come from the `.zsh` source.
- Saved code changes apply on the next Play, not midway through an active instance. Runtime errors identify the asset and source location in the status bar and stop only the failing behavior until the next Play. A failed callback does not partially copy its transform back to the native object. Moving beyond the fixed camera will take an object out of view.

`zEngineScripting` is the standalone compiler/VM. `zEngineScriptHost` is a separate platform-independent adapter linking that VM to the core, with no filesystem, Win32, or renderer dependencies. `BehaviorLifecycle` in the core handles scheduling. The editor supplies saved source, session controls, and render-submission eligibility. Each attachment owns a bounded VM context; compiled definitions can be shared, but mutable fields are independent. All three transform vectors synchronize before/after each callback, so scripts on the same GameObject see earlier scripts' changes in priority order. Cross-object scene lookup/binding, hot reload, and behavior removal remain future work. See `scripting/README.md` for language syntax and runtime limits.

## Mesh Renderer behavior

1. Select a GameObject and click **+ Add Behavior → Mesh Renderer**. The component initially has no model, so an empty object stays invisible until assigned one. Only one Mesh Renderer can be added through the Inspector. **Add Behavior → Script...** opens the existing script attachment picker; the **+ Add Script** shortcut is also retained.
2. In its Mesh Renderer controls, use **Choose Model...** to select an imported `Assets/<package>/model.fbx`, or **Use Cube** for the built-in vertex-colored cube. You can also drag a library FBX onto a GameObject's scene-tree row or its selected Inspector; this adds the component if needed and assigns the model.
3. Edit the object's position, rotation, and scale normally: the attached model follows its owner's transform. Multiple objects can render the same or different models simultaneously. Uncheck **Mesh Renderer enabled** to hide only that mesh; **Clear** removes the model assignment but retains the component and transform.

Imported geometry now retains its authored coordinates, size, and pivot after the importer's node-transform conversion. The old preview-only centering/size normalization has been removed. Large models may need a smaller GameObject scale to fit the fixed camera; distant objects may be outside its view. Assignment never changes the target object's name or transform. All models currently use the existing vertex-lit, albedo-only shader and two-sided rendering.

`MeshRenderer` is a platform-independent native `Behavior` containing an asset reference and inherited enabled state. The editor/render-system adapter resolves references into immutable GPU mesh handles. Objects sharing an imported model share GPU geometry and textures; the cache holds weak references, so replacing/clearing the last assignment releases the old resource (after any in-flight load/frame reference). Uploads are transactional, and asynchronous requests capture the target's stable ID and assignment revision: changing selection or choosing another mesh during loading cannot redirect or overwrite that newer assignment. No per-frame file reads or model uploads are used.

Mesh components and their project-relative assignments persist in scene assets, alongside transforms and script attachments. Model geometry/textures stay in their imported asset packages rather than being duplicated into each scene.

## FBX import and scene placement

1. Drag one or more `.fbx` files from Windows Explorer into the bottom **Media Library**. The status bar shows background import activity and any errors.
2. Drag an imported asset's row from the library into the central **Scene** viewport to create a new GameObject at the origin with that model attached. Existing objects (including the cube) are preserved. Move the new object using its transform fields. To assign to an existing object instead, drop onto its tree row or Inspector.
3. Scroll over the media library to browse longer asset lists. Escape cancels an asset drag.

Each import creates a unique `<active project>/Assets/<model name>/` package containing `model.fbx`, copied/extracted albedo image bytes, and an `asset.ready` marker. The library discovers completed packages on restart. Repeat imports receive a numbered folder and never overwrite existing assets. Incomplete imports are not listed. Projects created outside the build directory survive deleting build output. The C++ embedding/test `Create(show, directory)` API can adopt an explicit legacy project folder, creating a config without overwriting its assets; this explicit mode retains the old untitled demo cube. The normal application uses project startup instead. To migrate old assets through the UI, create a project and copy the old Assets contents into its Assets folder, then open a scene.

### Supported in this first pass

- Static binary/ASCII FBX polygon meshes; triangulation, node/geometry transforms, instances, generated normals, vertex colors, primary UVs, and per-face materials.
- Diffuse/base-color tint and one albedo image per material. Embedded images and external **PNG, JPEG, BMP, TIFF, and GIF** are decoded using Windows Imaging Component. Alpha is currently ignored (opaque rendering).
- Put external images alongside the FBX, in its referenced subdirectory, in `textures`, or in the matching `<filename>.fbm` folder. References outside the source directory and network paths are not followed. Copied project images are used thereafter; moving/deleting the original source files does not break the imported asset.
- Missing, corrupt, or unsupported images fall back to the material color with a status warning. Malformed FBX or failed mesh uploads leave all existing scene models intact.
- Animation, skin deformation, blend shapes, normal/roughness maps, layered textures, custom UV sets/transforms, and material editing are not implemented. Static previews are two-sided.

For bounded per-asset resource usage, imports currently allow FBX files up to 128 MB, up to one million triangles and 256 materials, with individual encoded images up to 32 MB. Albedo images are downscaled to at most 2048 pixels per side for GPU upload, with a 128 MB texture budget per model. Source image bytes are preserved. Geometry is batched by material to limit draw calls. Multiple distinct models still consume additional memory; there is no scene-wide memory budget or streaming system yet.

The importer is a separate static library (`zEngineAssets`) using vendored [ufbx 0.21.3](https://github.com/ufbx/ufbx/tree/v0.21.3). The renderer receives engine-owned mesh/material structures, not FBX-library objects. No dependency downloads are needed to build.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 or newer with **Desktop development with C++**
- CMake 3.24 or newer (the Visual Studio CMake component is sufficient)

## Build and run

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\zEngine.exe
```

Press Escape or close the window to exit. Resize the window to update the render targets and perspective aspect ratio.

The renderer loads `shaders/ColorCube.hlsl` at startup. The build copies that source next to the executable, allowing the HLSL to be edited and recompiled without embedding shader bytecode in the C++ source. Rebuild after editing so the changed source is copied to the output directory.

## Regression tests

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Tests cover the platform-independent object/behavior ownership core, live inspector fields and scrub/cancel interactions, independent object transforms, the renderer's TRS adapter, FBX transforms/instances, normals, UV seams, materials, Unicode source paths, missing images, duplicate imports, project-local texture persistence, malformed files, D3D11/HLSL/WIC rendering (including editor guides and singular scales), and simulated editor file-drop/asset-drag messages. GPU/editor tests create hidden windows and need a D3D11-capable device. Test fixtures and temporary projects are isolated from your active project.
