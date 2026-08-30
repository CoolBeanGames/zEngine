# zEngine

zEngine currently contains a small Direct3D 11 renderer hosted inside a lightweight native Windows editor shell. The scene viewport initially renders an indexed, depth-tested cube. Each cube vertex receives a random color when the program starts, and a directional light is calculated in the vertex shader. The cube/model is now controlled by its GameObject transform instead of automatically rotating.

The editor provides an options bar, resizable scene browser, resizable inspector, resizable media library, central scene viewport, and status/progress bar. Drag the narrow gaps between panels to resize them. FBX import, preview, empty GameObject creation, selection, and inspector editing are functional; menus, Add Folder, and play controls remain placeholders.

## GameObjects and transforms

- Click **+ Create Empty** in the Scene Browser to create and select a GameObject. Click its scene-tree row to select it again; scroll over the tree to browse longer lists.
- Edit its **Name** and comma-separated **Tags** in the Inspector. Tags are trimmed, case-sensitive, and deduplicated. Names must not be blank.
- Click a transform number to select its text and type a value. Valid values apply immediately. **Enter** commits; **Escape** restores the value from before the edit; **Tab/Shift-Tab** advances between fields. Incomplete/invalid numbers are shown with a red background and do not overwrite the last valid transform; leaving the field restores that valid number.
- Press and drag horizontally on a numeric field: right increases, left decreases. Position/scale change by 0.01 per pixel and rotation by 0.5 degrees per pixel. Hold **Shift before dragging** for ten-times finer changes. Escape or losing mouse capture cancels an unfinished drag.
- Position is world-space; rotation uses X, then Y, then Z Euler angles in degrees; scale is per-axis. Zero and negative scales are supported. Inspector values are bounded to +/-1,000,000 and must be finite. Small inspector panels scroll to keep fields accessible.

An empty object contains only its transform and metadata: it has no mesh and no behaviors by default. The selected object's **editor-only colored axes** show its position, orientation, and scale (X red, Y green, Z blue), against a world grid. These guides are not runtime GameObject components. The existing cube/FBX preview is a separate GameObject and can also be renamed and transformed. Moving an empty never moves that preview mesh. Moving objects far from the fixed camera can move them out of view; camera navigation is not implemented yet.

This is an **in-memory foundation only**. Scene/project management, parenting, serialization, undo/redo, viewport picking/manipulator dragging, script execution, and behavior lifecycle callbacks are intentionally deferred. GameObject edits are not saved on exit.

### Module boundaries

`zEngineCore` owns platform-independent `GameObject`, `Transform`, `ObjectStore`, and `Behavior` types. Every object has a stable ID, UTF-8 name, tag list, and non-removable transform. `ObjectStore` keeps object addresses stable, and objects own attached behavior lifetimes. `Behavior::Owner()`, `Enabled()`, `AddBehavior<T>()`, and `GetBehavior<T>()` provide the C++ extension/ownership foundation without introducing a scripting runtime.

`InspectorPanel` is a reusable native editor widget bound to a GameObject; it does not depend on the FBX importer or renderer. The editor submits value-only `ViewportFrame` transform data to the renderer, and `RenderTransform.h` adapts that data to DirectX matrices. The renderer does not own or reference GameObjects. Mesh normalization stays separate from the object's editable transform, and nonuniform scaling uses inverse-transpose normals.

This follows the component ownership pattern described in Unity's [GameObject reference](https://docs.unity3d.com/6000.0/Documentation/Manual/class-GameObject.html) and [MonoBehaviour reference](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/MonoBehaviour.html), with a tag **list** as required by zEngine rather than Unity's single-tag model.

## Script assets and editor

- Click **+ New Script** in the Media Library, or right-click the library and choose **Create Behavior Script (.zsh)**. This creates `NewBehavior.zsh` (numbered automatically when that name exists) under the current project's `Assets` folder, with `start`, `update(float delta)`, and `draw` hooks, and opens it for editing.
- Double-click a `.zsh` asset to open its resizable script window. Each file has a single editor window. Keywords, built-in types, numbers, strings, and comments are colored; **Tab** inserts four spaces and **Ctrl+Z/Ctrl+Y** undo/redo text edits.
- Use **Save / Ctrl+S** to save UTF-8 text, and **Reload / Ctrl+R** to load disk changes. A `*` in the title marks unsaved edits. Closing a script or the main editor asks whether to save, discard, or cancel. Saves use a temporary file followed by replacement and reject overwriting a file changed externally since loading. Invalid source can still be saved while you work.
- Malformed strings, block comments, unmatched brackets, and unexpected characters get an error background/underline, with line/column messages below the source. **Go to first error** selects and scrolls to the first problem. This branch currently provides **basic lexical/structural checks, not full language validation**: unknown names, type mismatches, missing semicolons, and other grammar errors require the compiler. Once `scripting/src/Script.cpp` and its public header are merged into this checkout, reconfigure CMake to enable compiler diagnostics automatically. No dependency on a sibling worktree is used.
- Select a GameObject and click **+ Add Script** in the Inspector, then choose a `.zsh` file inside this project's Assets directory. Alternatively, drag a library script onto a specific **Scene Browser object row** or the selected object's **Inspector**. Multiple different scripts are supported; duplicate attachments are rejected. The Inspector lists attached project-relative asset paths.
- Right-click **Refresh Assets** to discover `.zsh` files added externally (including subfolders); existing scripts are also discovered on startup. External script drops/import, script renaming, and behavior removal are not implemented yet.

Script files persist on disk, but **attachments are currently in-memory GameObject data and do not survive editor exit** until scene serialization is added. Attaching a script does **not execute it**: the scripting runtime's lifecycle/scene bridge remains a separate milestone. `.zsh` here is zEngine source, not a shell script, and is never launched through the OS.

The lightweight `zEngineScriptTools` library owns script file operations, lexical diagnostics, and the native Windows RichEdit editor. `ScriptBehavior` in `zEngineCore` only stores a project-relative asset reference; neither the renderer nor core depends on the editor or compiler. Source files are limited to 256 KiB, diagnostics to 100 lexical errors, and coloring to 8,000 tokens per refresh to keep pathological inputs bounded. Syntax coloring is debounced by 250 ms and excluded from text undo history.

## FBX import and preview

1. Drag one or more `.fbx` files from Windows Explorer into the bottom **Media Library**. The status bar shows background import activity and any errors.
2. Drag an imported asset's row from the library into the central **Scene** viewport to replace the cube (or previous model). The source mesh is normalized to the preview size, then uses the preview GameObject's current transform. This selects/renames the preview object without overwriting its transform, and does not attach a mesh to any empty GameObject. This is a preview replacement, not general mesh instantiation or scene saving.
3. Scroll over the media library to browse longer asset lists. Escape cancels an asset drag.

Until project creation/selection is implemented, the active project is `Project` beside `zEngine.exe`. Each import creates a unique `Project/Assets/<model name>/` package containing `model.fbx`, copied/extracted albedo image bytes, and an `asset.ready` marker. The library discovers completed packages on restart. Repeat imports receive a numbered folder and never overwrite existing assets. Incomplete imports are not listed. Back up this Project folder before deleting your build output. The editor shell also accepts an explicit project directory in its C++ `Create()` API.

### Supported in this first pass

- Static binary/ASCII FBX polygon meshes; triangulation, node/geometry transforms, instances, generated normals, vertex colors, primary UVs, and per-face materials.
- Diffuse/base-color tint and one albedo image per material. Embedded images and external **PNG, JPEG, BMP, TIFF, and GIF** are decoded using Windows Imaging Component. Alpha is currently ignored (opaque rendering).
- Put external images alongside the FBX, in its referenced subdirectory, in `textures`, or in the matching `<filename>.fbm` folder. References outside the source directory and network paths are not followed. Copied project images are used thereafter; moving/deleting the original source files does not break the imported asset.
- Missing, corrupt, or unsupported images fall back to the material color with a status warning. Malformed FBX or failed mesh uploads leave the previous viewport model intact.
- Animation, skin deformation, blend shapes, normal/roughness maps, layered textures, custom UV sets/transforms, material editing, and scene serialization are not implemented. Static previews are two-sided.

For bounded resource usage, imports currently allow FBX files up to 128 MB, up to one million triangles and 256 materials, with individual encoded images up to 32 MB. Albedo images are downscaled to at most 2048 pixels per side for GPU upload, with a 128 MB preview texture budget. Source image bytes are preserved. Geometry is batched by material to limit draw calls.

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
