#include "FbxImporter.h"
#include "Renderer.h"
#include "EditorShell.h"
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <functional>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct TestDirectory
    {
        std::filesystem::path path;
        TestDirectory()
        {
            path = std::filesystem::temp_directory_path() /
                (L"zEngine-asset-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            Require(std::filesystem::create_directory(path), "Could not reserve test directory");
        }
        ~TestDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored); // Only this test's freshly reserved directory.
        }
    };

    std::filesystem::path CreateSource(const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        const auto source = directory / L"mod\u00e8le.fbx";
        std::filesystem::copy_file(std::filesystem::path(TEST_FIXTURE_DIR) / "materials.fbx", source);
        // 2x2, 24-bit BMP with red/green/blue/white corners and padded rows.
        const unsigned char bitmap[]{
            'B','M',70,0,0,0, 0,0,0,0, 54,0,0,0,
            40,0,0,0, 2,0,0,0, 2,0,0,0, 1,0,24,0,
            0,0,0,0, 16,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,255, 0,255,0, 0,0, 255,0,0, 255,255,255, 0,0
        };
        std::ofstream image(directory / "checker.bmp", std::ios::binary);
        image.write(reinterpret_cast<const char*>(bitmap), sizeof(bitmap));
        return source;
    }

    void ImportTests()
    {
        TestDirectory test;
        const auto source = CreateSource(test.path / "source");
        const auto model = FbxImporter::Load(source);
        Require(model.indices.size() == 12, "Both mesh instances must be imported");
        Require(model.vertices.size() == 12, "Polygon-corner UV seams must be preserved");
        Require(model.materials.size() == 3, "Expected default and two FBX materials");
        Require(model.parts.size() == 2, "Material ranges should be batched across instances");
        Require(model.warnings.empty(), "Valid albedo unexpectedly produced a warning");
        Require(model.materials[2].image.size() == 70, "External albedo image was not loaded");
        float minimum = model.vertices.front().position.x, maximum = minimum;
        bool flippedV = false;
        for (const auto& vertex : model.vertices)
        {
            minimum = std::min(minimum, vertex.position.x);
            maximum = std::max(maximum, vertex.position.x);
            Require(std::isfinite(vertex.normal.z) && std::abs(vertex.normal.z) > 0, "Missing normals must be generated");
            flippedV = flippedV || vertex.uv.y == 1.0f;
        }
        Require(maximum - minimum > 20, "Node transforms/instances were not baked");
        Require(flippedV, "FBX UVs must use the Direct3D texture origin");

        std::vector<std::string> warnings;
        const auto package = FbxImporter::Import(source, test.path / "Assets", warnings);
        const auto duplicate = FbxImporter::Import(source, test.path / "Assets", warnings);
        Require(package != duplicate, "Duplicate imports must not overwrite assets");
        Require(std::filesystem::exists(package.parent_path() / "asset.ready"), "Package was not marked complete");
        std::filesystem::remove(source.parent_path() / "checker.bmp");
        Require(!FbxImporter::Load(source).warnings.empty(), "Missing textures should warn and fall back");
        std::filesystem::remove(source);
        const auto restored = FbxImporter::Load(package, true);
        Require(restored.warnings.empty(), "Project package still depends on the original texture location");
        Require(restored.materials[2].image == model.materials[2].image, "Cached albedo changed");
        Require(restored.indices == model.indices, "Project reload changed geometry");

        const auto invalid = test.path / "invalid.fbx";
        { std::ofstream file(invalid); file << "This is not FBX"; }
        bool rejected = false;
        try { FbxImporter::Import(invalid, test.path / "Assets", warnings); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Malformed FBX must be rejected");
        std::cout << "PASS: instances, transforms, normals, UVs, material ranges, albedo, Unicode paths, persistence, duplicates, malformed input\n";
    }

    void GpuTests()
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.lpszClassName = L"zEngineGpuTest";
        Require(RegisterClassW(&windowClass) != 0, "Window registration failed");
        const HWND window = CreateWindowW(windowClass.lpszClassName, L"Hidden renderer test", WS_OVERLAPPEDWINDOW,
                                           0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
        Require(window != nullptr, "Window creation failed");
        {
            TestDirectory test;
            const auto source = CreateSource(test.path / "source");
            auto model = FbxImporter::Load(source);
            Renderer renderer;
            renderer.Initialize(window, 320, 240);
            renderer.Render();
            Require(renderer.SetModel(model).empty(), "WIC/GPU albedo upload failed");
            renderer.Render();
            renderer.Resize(180, 400);
            renderer.Render();
            model.materials[2].image = {1, 2, 3, 4};
            Require(!renderer.SetModel(model).empty(), "Broken images must use fallback with a warning");
            renderer.Render();
            model.indices[0] = UINT32_MAX;
            bool rejected = false;
            try { renderer.SetModel(model); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid GPU mesh must be rejected");
            renderer.Render(); // The last valid preview must still be usable.
        }
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        CoUninitialize();
        std::cout << "PASS: D3D11 initialization, HLSL compilation, cube/model draws, WIC albedo, resize, texture fallback, transactional mesh failure\n";
    }

    void EditorTests()
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        TestDirectory test;
        const auto source = CreateSource(test.path / "source");
        const auto project = test.path / "project";
        {
            EditorShell editor(GetModuleHandleW(nullptr));
            const HWND window = editor.Create(SW_HIDE, project);
            editor.InitializeRenderer();
            const HWND viewport = FindWindowExW(window, nullptr, L"zEngineViewportWindow", nullptr);
            Require(viewport != nullptr, "Editor viewport missing");
            RECT client{};
            GetClientRect(window, &client);
            const auto pumpUntil = [&](const std::function<bool()>& predicate) {
                const auto deadline = GetTickCount64() + 10000;
                while (!predicate() && GetTickCount64() < deadline)
                {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    editor.Render();
                    Sleep(5);
                }
                Require(predicate(), "Editor asset operation timed out");
            };
            const auto sendDrop = [&](POINT point) {
                const auto filename = source.wstring();
                const SIZE_T bytes = sizeof(DROPFILES) + (filename.size() + 2) * sizeof(wchar_t);
                const HGLOBAL memory = GlobalAlloc(GHND, bytes);
                Require(memory != nullptr, "Drop allocation failed");
                auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
                drop->pFiles = sizeof(DROPFILES);
                drop->pt = point;
                drop->fWide = TRUE;
                std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), filename.c_str(),
                            (filename.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(memory);
                SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
            };
            const auto package = project / "Assets" / source.stem() / "asset.ready";
            sendDrop({500, 100}); // Outside the library must not import.
            editor.Render();
            Require(!std::filesystem::exists(package), "Drops outside the media library must be ignored");
            sendDrop({400, client.bottom - 90});
            pumpUntil([&]() { return std::filesystem::exists(package); });
            // The marker is written on the worker; let the UI consume the completed result.
            for (int frame = 0; frame < 20; ++frame) { editor.Render(); Sleep(5); }
            const auto rowPoint = MAKELPARAM(350, client.bottom - 220);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, rowPoint);
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(600, 200));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(600, 200));
            pumpUntil([&]() {
                wchar_t title[256]{};
                GetWindowTextW(viewport, title, 256);
                return std::wstring(title).find(source.stem().wstring()) != std::wstring::npos;
            });
            RECT before{}, after{};
            GetWindowRect(viewport, &before);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(226, 200));
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(260, 200));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(260, 200));
            editor.Render();
            GetWindowRect(viewport, &after);
            Require(after.left > before.left, "Splitter no longer resizes the imported-model viewport");
        }
        CoUninitialize();
        std::cout << "PASS: library-only OS drop, background import, library-to-viewport drag, model replacement, splitter resize\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--gpu") GpuTests();
        else if (argc > 1 && std::string(argv[1]) == "--editor") EditorTests();
        else if (argc == 1) ImportTests();
        else throw std::runtime_error("Unknown test mode");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
