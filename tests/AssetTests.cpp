#include "FbxImporter.h"
#include "Renderer.h"
#include "EditorShell.h"
#include "core/MeshRenderer.h"
#include "InspectorPanel.h"
#include "RenderTransform.h"
#include "WindowCapture.h"
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
            ViewportFrame frame;
            frame.meshes.push_back({renderer.Cube(),{}});
            renderer.Render(frame);
            Require(renderer.LastMeshCount() == 1, "Cube submission failed");
            std::vector<std::string> warnings;
            const auto importedMesh = renderer.UploadModel(model,warnings);
            Require(warnings.empty(), "WIC/GPU albedo upload failed");
            frame.meshes.push_back({importedMesh,{}});
            frame.meshes[0].transform.SetPosition({-1.5f,0,0});
            frame.meshes[1].transform.SetPosition({1.5f,0,0});
            renderer.Render(frame);
            Require(renderer.LastMeshCount() == 2, "Different meshes must render in the same frame");
            renderer.Resize(180, 400);
            renderer.Render(frame);
            model.materials[2].image = {1, 2, 3, 4};
            frame.meshes.push_back({renderer.UploadModel(model,warnings),{}});
            Require(!warnings.empty(), "Broken images must use fallback with a warning");
            renderer.Render(frame);
            model.indices[0] = UINT32_MAX;
            bool rejected = false;
            try { renderer.UploadModel(model,warnings); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid GPU mesh must be rejected");
            renderer.Render(frame);
            Require(renderer.LastMeshCount() == 3, "Failed upload must preserve all existing render resources");
            frame.showEditorGuides = true;
            frame.meshes[0].transform.SetPosition({0.5f, 0, 0});
            frame.meshes[0].transform.SetRotation({15, 30, 45});
            frame.meshes[0].transform.SetScale({2, -1, 0.5f});
            frame.selectionTransform = frame.meshes[0].transform;
            renderer.Render(frame);
            frame.meshes[0].transform.SetScale({0, 1, 1});
            renderer.Render(frame); // Singular scales must not cause inverse-matrix NaNs.
            renderer.Render({});
            Require(renderer.LastMeshCount() == 0, "An empty scene must not render an implicit preview mesh");
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

    void MeshBehaviorTests(bool capture)
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        TestDirectory test;
        const auto project = test.path / "Project";
        std::vector<std::string> warnings;
        const auto imported = FbxImporter::Import(CreateSource(test.path/"source"),project/"Assets",warnings);
        {
            EditorShell editor(GetModuleHandleW(nullptr));
            HWND window = editor.Create(SW_HIDE,project);
            editor.InitializeRenderer();
            const auto cubeId = editor.SelectedGameObject()->Id();
            Require(editor.BuildSceneFrame().meshes.size() == 1,"Default cube needs an actual Mesh Renderer");
            auto& first = editor.CreateEmptyGameObject();
            const auto inspector = FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
            Require(GetDlgItem(inspector,InspectorPanel::AddBehaviorButton)!=nullptr,"Add Behavior button missing");
            SendMessageW(inspector,WM_COMMAND,InspectorPanel::AddMeshCommand,0);
            Require(first.GetBehavior<zengine::MeshRenderer>() && first.GetBehavior<zengine::MeshRenderer>()->Asset().empty(),"Add Behavior -> Mesh Renderer failed");
            Require(!editor.AddMeshRenderer(first.Id()) && first.BehaviorCount()==1,"Duplicate mesh component allowed");
            SendMessageW(inspector,WM_COMMAND,InspectorPanel::CubeMeshButton,0);
            first.GetTransform().SetPosition({-2,0,0});
            first.GetTransform().SetRotation({0,35,0});
            first.GetTransform().SetScale({0.5f,1,0.5f});
            auto& second = editor.CreateEmptyGameObject();
            editor.AssignCube(second.Id());
            SetWindowTextW(GetDlgItem(inspector,InspectorPanel::FirstTransformField),L"2");
            auto frame = editor.BuildSceneFrame();
            Require(frame.meshes.size()==3 && frame.meshes[1].mesh==frame.meshes[2].mesh,"Cube resources must be shared across objects");
            Require(frame.meshes[1].transform.Position().x==-2 && frame.meshes[2].transform.Position().x==2 &&
                frame.meshes[1].transform.Rotation().y==35 && frame.meshes[1].transform.Scale().x==0.5f,"Object-local TRS submission failed");
            editor.Render();
            if (capture) CaptureWindow(window,"mesh-renderer-qa.bmp");
            SendMessageW(GetDlgItem(inspector,InspectorPanel::MeshEnabled),BM_CLICK,0,0);
            Require(!second.GetBehavior<zengine::MeshRenderer>()->Enabled() && editor.BuildSceneFrame().meshes.size()==2,"Disabling mesh must hide only its owner");
            SendMessageW(GetDlgItem(inspector,InspectorPanel::MeshEnabled),BM_CLICK,0,0);
            SendMessageW(inspector,WM_COMMAND,InspectorPanel::ClearMeshButton,0);
            Require(second.BehaviorCount()==1 && second.GetBehavior<zengine::MeshRenderer>()->Asset().empty() && editor.BuildSceneFrame().meshes.size()==2,"Clear must remove model without removing component");
            const auto pump = [&](const std::function<bool()>& predicate) {
                const auto deadline = GetTickCount64()+10000;
                while (!predicate() && GetTickCount64()<deadline) { editor.Render(); Sleep(5); }
                Require(predicate(),"Mesh assignment timed out");
            };
            // Capture target ID, then change selection before loading completes.
            editor.QueueModel(imported,first.Id());
            auto& empty = editor.CreateEmptyGameObject();
            pump([&]() { return first.GetBehavior<zengine::MeshRenderer>()->Asset()!=zengine::MeshRenderer::CubeAsset; });
            Require(editor.SelectedGameObject()->Id()==empty.Id() && empty.BehaviorCount()==0,"Async load followed selection instead of target");
            Require(first.Name()=="GameObject" && first.GetTransform().Position().x==-2,"Model assignment changed name/transform");
            editor.QueueModel(imported,second.Id());
            pump([&]() { return !second.GetBehavior<zengine::MeshRenderer>()->Asset().empty(); });
            frame=editor.BuildSceneFrame();
            Require(frame.meshes.size()==3 && frame.meshes[1].mesh==frame.meshes[2].mesh,"Imported model resources were not shared");
            // Assigning cube after a queued request invalidates that request.
            editor.QueueModel(imported,second.Id());
            editor.Render(); // Start cached asynchronous request.
            editor.AssignCube(second.Id());
            for (int i=0;i<30;++i) { editor.Render(); Sleep(2); }
            Require(second.GetBehavior<zengine::MeshRenderer>()->Asset()==zengine::MeshRenderer::CubeAsset,"Stale request overwrote newer model choice");
            bool rejected=false;
            try { editor.QueueModel(test.path/"source"/imported.filename(),first.Id()); } catch (...) { rejected=true; }
            Require(rejected,"Unimported external models must not be assigned");
            // Dropping on the viewport instantiates an additional object rather than replacing cube.
            RECT client{}; GetClientRect(window,&client);
            const auto row=MAKELPARAM(350,client.bottom-220);
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,row);
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(600,200));
            SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(600,200));
            pump([&]() { return editor.GameObjects().Size()==5; });
            Require(editor.GameObjects().Find(cubeId)->GetBehavior<zengine::MeshRenderer>()->Asset()==zengine::MeshRenderer::CubeAsset,"FBX scene placement replaced default cube");
            Require(editor.BuildSceneFrame().meshes.size()==4,"Multiple instantiated models missing from scene");
        }
        CoUninitialize();
        std::cout << "PASS: Mesh Renderer component, Inspector add/enable/clear, independent transforms, shared GPU meshes, captured async targets, scene instantiation\n";
    }

    void GameObjectEditorTests()
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
        TestDirectory test;
        {
            EditorShell editor(GetModuleHandleW(nullptr));
            const HWND window = editor.Create(SW_HIDE, test.path / "Project");
            editor.InitializeRenderer();
            const auto previewId = editor.SelectedGameObject()->Id();
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(80, 80));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(80, 80));
            Require(editor.GameObjects().Size() == 2, "Create Empty did not add a scene-tree object");
            const auto* object = editor.SelectedGameObject();
            Require(object->Id() != previewId && object->BehaviorCount() == 0 && object->Tags().empty(), "New empty object state is wrong");
            const HWND inspector = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
            Require(inspector != nullptr, "Inspector child missing");
            const auto field = [&](int id) { return GetDlgItem(inspector, id); };
            SetWindowTextW(field(InspectorPanel::NameField), L"Moving Empty");
            SendMessageW(field(InspectorPanel::NameField), WM_KEYDOWN, VK_RETURN, 0);
            Require(object->Name() == "Moving Empty", "Name field did not update GameObject");
            SetWindowTextW(field(InspectorPanel::TagsField), L"enemy, movable, enemy");
            Require(object->Tags().size() == 2 && object->HasTag("movable"), "Tag field did not update tag list");
            const wchar_t* numbers[]{L"1.25", L"-2.5", L"0.5", L"10", L"20", L"90", L"2", L"-1", L"0.5"};
            for (int i = 0; i < 9; ++i) SetWindowTextW(field(InspectorPanel::FirstTransformField + i), numbers[i]);
            const auto& transform = object->GetTransform();
            Require(transform.Position().x == 1.25f && transform.Position().y == -2.5f && transform.Position().z == 0.5f, "Position text is not live");
            Require(transform.Rotation().x == 10 && transform.Rotation().y == 20 && transform.Rotation().z == 90, "Rotation text is not live");
            Require(transform.Scale().x == 2 && transform.Scale().y == -1 && transform.Scale().z == 0.5f, "Scale text is not live");
            const HWND x = field(InspectorPanel::FirstTransformField);
            for (const auto* invalid : {L"nan", L"inf", L"-", L"", L"1x", L"1000001"})
            {
                SetWindowTextW(x, invalid);
                Require(transform.Position().x == 1.25f, "Invalid text modified the object");
                SendMessageW(x, WM_KEYDOWN, VK_RETURN, 0);
            }
            const auto drag = [&](HWND target, int delta, bool cancel) {
                SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
                SendMessageW(target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(8 + delta, 8));
                if (cancel) SendMessageW(target, WM_KEYDOWN, VK_ESCAPE, 0);
                SendMessageW(target, WM_LBUTTONUP, 0, MAKELPARAM(8 + delta, 8));
            };
            drag(x, 20, false);
            Require(std::abs(transform.Position().x - 1.45f) < 0.0001f, "Right drag must increase position live");
            drag(x, -20, false);
            Require(std::abs(transform.Position().x - 1.25f) < 0.0001f, "Left drag must decrease position live");
            drag(x, 40, true);
            Require(std::abs(transform.Position().x - 1.25f) < 0.0001f, "Escape must cancel scrubbing");
            drag(field(InspectorPanel::FirstTransformField + 3), 20, false);
            Require(transform.Rotation().x == 20, "Rotation scrub failed");
            drag(field(InspectorPanel::FirstTransformField + 6), -20, false);
            Require(std::abs(transform.Scale().x - 1.8f) < 0.0001f, "Scale scrub failed");
            // A click without dragging enters text mode; Escape restores the pre-edit value.
            SendMessageW(x, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
            SendMessageW(x, WM_LBUTTONUP, 0, MAKELPARAM(8, 8));
            SetWindowTextW(x, L"9");
            SendMessageW(x, WM_KEYDOWN, VK_ESCAPE, 0);
            Require(std::abs(transform.Position().x - 1.25f) < 0.0001f, "Escape must cancel a typed edit");
            editor.Render();
            // Select the original mesh object: empty transforms must not leak into it.
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
            Require(editor.SelectedGameObject()->Id() == previewId, "Scene-tree selection failed");
            Require(editor.SelectedGameObject()->GetTransform().Position().x == 0, "Object transforms are not independent");
            SetWindowTextW(x, L"2");
            editor.Render();
            Require(editor.SelectedGameObject()->GetTransform().Position().x == 2, "Preview transform editing failed");
            // Script creation, attachment and discovery coexist with GameObject editing.
            const auto script = editor.CreateScriptAsset();
            Require(std::filesystem::is_regular_file(script) && script.extension() == ".zsh", "Script asset was not saved");
            Require(GetDlgItem(inspector, InspectorPanel::AddScriptButton) != nullptr, "Add Script button missing");
            RECT client{}; GetClientRect(window, &client);
            const auto rowPoint = MAKELPARAM(350, client.bottom - 220);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, rowPoint);
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(50, 166));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 166));
            Require(object->BehaviorCount() == 1 && editor.SelectedGameObject()->Id() == object->Id(), "Script drag did not attach to targeted tree object");
            Require(!editor.AttachScript(object->Id(), script) && object->BehaviorCount() == 1, "Duplicate script attachment allowed");
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, rowPoint);
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(client.right-100, 300));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(client.right-100, 300));
            Require(editor.SelectedGameObject()->BehaviorCount() == 2, "Inspector script drop failed alongside Mesh Renderer");
            Require(object->GetTransform().Position().x == 1.25f, "Script attachment changed transform");
            // Renderer adapter applies scale, then X/Y/Z degree rotations, then translation.
            zengine::Transform example;
            example.SetScale({2, 3, 4}); example.SetRotation({0, 0, 90}); example.SetPosition({3, 4, 5});
            DirectX::XMFLOAT3 point;
            DirectX::XMStoreFloat3(&point, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(1, 0, 0, 1), TransformMatrix(example)));
            Require(std::abs(point.x - 3) < 0.0001f && std::abs(point.y - 6) < 0.0001f && std::abs(point.z - 5) < 0.0001f, "Renderer TRS adapter is incorrect");
        }
        CoUninitialize();
        std::cout << "PASS: Create Empty, scene selection, names/tags, all nine live transform fields, bidirectional scrubbing, cancellation, independent transforms, TRS adapter\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--gpu") GpuTests();
        else if (argc > 1 && std::string(argv[1]) == "--editor") EditorTests();
        else if (argc > 1 && std::string(argv[1]) == "--objects") GameObjectEditorTests();
        else if (argc > 1 && std::string(argv[1]) == "--meshes") MeshBehaviorTests(argc > 2 && std::string(argv[2]) == "--capture");
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
