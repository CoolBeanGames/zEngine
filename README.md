# zEngine

zEngine currently contains a small Direct3D 11 renderer hosted inside a lightweight native Windows editor shell. The scene viewport renders an indexed, depth-tested cube that rotates continuously. Each cube vertex receives a random color when the program starts, and a directional light is calculated in the vertex shader.

The editor provides an options bar, resizable scene browser, resizable inspector, resizable media library, central scene viewport, and status/progress bar. Drag the narrow gaps between panels to resize them. FBX import and preview are functional; inspector editing, menus, Add Folder, and play controls remain placeholders.

## FBX import and preview

1. Drag one or more `.fbx` files from Windows Explorer into the bottom **Media Library**. The status bar shows background import activity and any errors.
2. Drag an imported asset's row from the library into the central **Scene** viewport to replace the cube (or previous model). The model is automatically centered and fitted to the viewport and continues the rotating preview. This is a preview replacement, not scene instantiation or saving.
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

Tests cover FBX transforms/instances, normals, UV seams, materials, Unicode source paths, missing images, duplicate imports, project-local texture persistence, malformed files, D3D11/HLSL/WIC rendering, and simulated editor file-drop/asset-drag messages. GPU/editor tests create hidden windows and need a D3D11-capable device. Test fixtures and temporary projects are isolated from your active project.
