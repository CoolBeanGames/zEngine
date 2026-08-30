# zEngine

zEngine currently contains a small Direct3D 11 renderer hosted inside a lightweight native Windows editor shell. The scene viewport renders an indexed, depth-tested cube that rotates continuously. Each cube vertex receives a random color when the program starts, and a directional light is calculated in the vertex shader.

The editor shell provides the initial non-functional layout for the future tools: an options bar, resizable scene browser, resizable inspector, resizable media library, central scene viewport, and status/progress bar. Drag the narrow gaps between panels to resize them. Asset importing, inspector editing, menus, and play controls are visual placeholders at this stage.

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
