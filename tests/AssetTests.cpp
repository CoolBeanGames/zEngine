#include "FbxImporter.h"
#include "Renderer.h"
#include "EditorShell.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "physics/PhysicsBehavior.h"
#include "ui/UiControl.h"
#include "InspectorPanel.h"
#include "MaterialEditor.h"
#include "audio/AudioSource.h"
#include "audio/AudioEffect.h"
#include "core/Light.h"
#include "core/Environment.h"
#include "RenderTransform.h"
#include "WindowCapture.h"
#include "ScriptAssets.h"
#include "ScriptEditor.h"
#include "SceneAssets.h"
#include "PrefabAssets.h"
#include "AssetLibrary.h"
#include "runtime/GamePackage.h"
#include "runtime/GameSession.h"
#include "input/InputAssets.h"
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

            // ZE-60: 2D / UI overlay pass - screen-space sprites, nine-slice, rotation, and text.
            const std::array<std::uint8_t, 16> checker{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
            const auto panel = renderer.UploadTexture(2, 2, checker.data());
            Require(static_cast<bool>(panel) && static_cast<bool>(renderer.WhiteTexture()), "2D texture upload failed");
            bool textureRejected = false;
            try { renderer.UploadTexture(0, 4, checker.data()); } catch (const std::exception&) { textureRejected = true; }
            Require(textureRejected, "Invalid 2D texture dimensions must be rejected");

            ViewportFrame ui;
            ui.sprites.push_back({renderer.WhiteTexture(), {8, 8, 120, 40}, {}, {}, {0.1f, 0.1f, 0.12f, 0.7f}, 0, {0.5f, 0.5f}});
            ui.sprites.push_back({panel, {160, 40, 200, 120}, {}, {6, 6, 6, 6}, {1, 1, 1, 1}, 0, {0.5f, 0.5f}});
            ui.sprites.push_back({panel, {60, 140, 48, 48}, {}, {}, {1, 1, 1, 1}, 30, {0.5f, 0.5f}});
            ui.texts.push_back({"Hello 2D", 12, 60, 18, {1, 1, 1, 1}});
            ui.fps = 60;
            renderer.Render(ui);
            Require(renderer.LastSpriteCount() >= 3 + 1, "2D overlay did not submit sprites and text");
            renderer.Resize(300, 220);
            renderer.Render(ui); // screen-space overlay must survive a resize
            renderer.Render({});
            Require(renderer.LastSpriteCount() == 0, "An empty frame must not draw a stale overlay");
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
            const auto rowPoint = MAKELPARAM(50, client.bottom - 220);
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
            {
                // ZE-65: create a Material Instance, assign it to the cube via the Inspector,
                // edit a parameter, and confirm it persists + reaches the render submission.
                const auto materialPath = editor.CreateMaterialAsset(); // cube stays selected
                const auto rel = std::filesystem::relative(materialPath, editor.AssetsDirectory()).generic_u8string();
                const std::string relative(reinterpret_cast<const char*>(rel.data()), rel.size());
                const HWND inspector0 = FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
                const HWND materialField = GetDlgItem(inspector0, InspectorPanel::FirstBehaviorField + 2); // header, priority, material
                Require(materialField != nullptr, "Inspector has no Material row for a Mesh Renderer");
                SetWindowTextW(materialField, std::wstring(relative.begin(), relative.end()).c_str()); // relative path is ASCII
                SendMessageW(inspector0, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(materialField), EN_CHANGE), reinterpret_cast<LPARAM>(materialField));
                Require(editor.GameObjects().Find(cubeId)->GetBehavior<zengine::MeshRenderer>()->Material() == relative,
                        "Inspector did not assign the .material to the Mesh Renderer");
                // tint parameter rows now exist: material(2), tint x/y/z/w (3..6).
                const HWND tintBlue = GetDlgItem(inspector0, InspectorPanel::FirstBehaviorField + 5);
                Require(tintBlue != nullptr, "Inspector has no tint parameter row");
                SetWindowTextW(tintBlue, L"0.5");
                SendMessageW(inspector0, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(tintBlue), EN_CHANGE), reinterpret_cast<LPARAM>(tintBlue));
                const auto savedDoc = zengine::materials::Load(materialPath);
                const auto* tint = savedDoc.values.empty() ? nullptr : &savedDoc.values.front();
                Require(tint && tint->name == "tint" && std::abs(tint->numbers[2] - 0.5f) < 0.001f,
                        "Inspector edit did not persist to the .material file");
                const auto materialFrame = editor.BuildSceneFrame();
                Require(!materialFrame.meshes.empty() && materialFrame.meshes[0].material != nullptr,
                        "Assigned material did not reach the render submission");

                // ZE-102: a .material opens in a standalone editor whose fields save back to the file.
                const HWND matWin = editor.OpenMaterialEditor(materialPath);
                Require(matWin != nullptr, "Material editor did not open for a .material");
                const HWND tintR = GetDlgItem(matWin, MaterialEditor::Field0); // row 0 = tint, component 0
                Require(tintR != nullptr, "Material editor has no tint value field");
                SetWindowTextW(tintR, L"0.25");
                SendMessageW(matWin, WM_COMMAND, MAKEWPARAM(MaterialEditor::Save, BN_CLICKED), 0);
                const auto editedDoc = zengine::materials::Load(materialPath);
                const zengine::materials::Value* editedTint = nullptr;
                for (const auto& v : editedDoc.values) if (v.name == "tint") editedTint = &v;
                Require(editedTint && std::abs(editedTint->numbers[0] - 0.25f) < 0.001f,
                        "Material editor did not save the edited value to the .material file");

                // ZE-103: the .material drop path assigns to a model's Mesh Renderer.
                Require(editor.AssignMaterialToObject(cubeId, relative),
                        "Dropping a .material onto a model did not assign it");
                Require(editor.GameObjects().Find(cubeId)->GetBehavior<zengine::MeshRenderer>()->Material() == relative,
                        "Material drop set the wrong path on the Mesh Renderer");
                Require(!editor.AssignMaterialToObject(zengine::GameObjectId{999999}, relative),
                        "Material drop onto a non-existent object should be rejected");
            }
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
            Require(editor.SelectedGameObject()->Id()==second.Id(),"Creating a mesh object did not keep it selected");
            SendMessageW(GetDlgItem(inspector,InspectorPanel::MeshEnabled),BM_CLICK,0,0);
            Require(!second.GetBehavior<zengine::MeshRenderer>()->Enabled(),"Mesh enabled checkbox did not update its selected owner");
            Require(editor.BuildSceneFrame().meshes.size()==2,"Disabling mesh did not hide exactly one owner");
            SendMessageW(GetDlgItem(inspector,InspectorPanel::MeshEnabled),BM_CLICK,0,0);
            SendMessageW(inspector,WM_COMMAND,InspectorPanel::ClearMeshButton,0);
            Require(second.BehaviorCount()==1 && second.GetBehavior<zengine::MeshRenderer>()->Asset().empty() && editor.BuildSceneFrame().meshes.size()==2,"Clear must remove model without removing component");
            const auto pump = [&](const std::function<bool()>& predicate,const char* failure) {
                const auto deadline = GetTickCount64()+10000;
                while (!predicate() && GetTickCount64()<deadline) { editor.Render(); Sleep(5); }
                Require(predicate(),failure);
            };
            // Capture target ID, then change selection before loading completes.
            editor.QueueModel(imported,first.Id());
            auto& empty = editor.CreateEmptyGameObject();
            pump([&]() { return first.GetBehavior<zengine::MeshRenderer>()->Asset()!=zengine::MeshRenderer::CubeAsset; },"First asynchronous mesh assignment timed out");
            Require(editor.SelectedGameObject()->Id()==empty.Id() && empty.BehaviorCount()==0,"Async load followed selection instead of target");
            Require(first.Name()=="GameObject" && first.GetTransform().Position().x==-2,"Model assignment changed name/transform");
            editor.QueueModel(imported,second.Id());
            pump([&]() { return !second.GetBehavior<zengine::MeshRenderer>()->Asset().empty(); },"Second asynchronous mesh assignment timed out");
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
            const auto row=MAKELPARAM(50,client.bottom-220);
            SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,row);
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(600,200));
            SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(600,200));
            pump([&]() { return editor.GameObjects().Size()==5; },"Viewport model instantiation timed out");
            Require(editor.GameObjects().Find(cubeId)->GetBehavior<zengine::MeshRenderer>()->Asset()==zengine::MeshRenderer::CubeAsset,"FBX scene placement replaced default cube");
            Require(editor.BuildSceneFrame().meshes.size()==4,"Multiple instantiated models missing from scene");

            // ZE-67: Add > Audio Player creates an AudioSource; the editor serializes it into the scene.
            auto& ap = editor.CreateAudioPlayerObject();
            if (HWND rn = GetDlgItem(window, 3900)) SendMessageW(rn, WM_KEYDOWN, VK_RETURN, 0); // finish inline rename
            auto* src = ap.GetBehavior<zengine::audio::AudioSource>();
            Require(src != nullptr, "Audio Player object has no AudioSource behavior");
            src->SetClip("sfx/step.wav"); src->SetLoop(true); src->SetSpatial(true);
            src->SetVolume(0.4f); src->SetMaxDistance(30);
            const auto audioScenePath = editor.AssetsDirectory() / L"AudioScene.zscene";
            Require(editor.SaveScene(audioScenePath), "could not save the audio scene");
            const auto audioDoc = zengine::scenes::Decode(zengine::scenes::Load(audioScenePath));
            bool foundAudio = false;
            for (const auto& o : audioDoc.objects) for (const auto& b : o.behaviors)
                if (b.kind == zengine::scenes::BehaviorData::Kind::Audio && b.audioClip == "sfx/step.wav"
                    && b.audioLoop && b.audioSpatial && std::abs(b.audioVolume - 0.4f) < 0.001f && b.audioMaxDistance == 30.0f)
                    foundAudio = true;
            Require(foundAudio, "the editor did not serialize the AudioSource into the scene");

            // ZE-111: the selected 3D source contributes an audible-range gizmo; a global one does not.
            src->SetSpatial(true); src->SetMinDistance(2); src->SetMaxDistance(18);
            const auto rangeFrame = editor.BuildSceneFrame();
            Require(rangeFrame.audioRanges.size() == 1
                    && std::abs(rangeFrame.audioRanges[0].minDistance - 2.0f) < 0.001f
                    && std::abs(rangeFrame.audioRanges[0].maxDistance - 18.0f) < 0.001f
                    && rangeFrame.audioRanges[0].selected,
                    "selected 3D AudioSource did not produce an audible-range gizmo");
            src->SetSpatial(false);
            Require(editor.BuildSceneFrame().audioRanges.empty(), "a global (2D) AudioSource must show no range gizmo");

            // ZE-109: an Audio Effect can be added (via the Inspector) to an object that has an Area.
            ap.AddBehavior<zengine::physics::Collider>().SetSize({6, 6, 6});
            ap.AddBehavior<zengine::physics::Area>();
            const HWND ins = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
            SendMessageW(ins, WM_COMMAND, MAKEWPARAM(InspectorPanel::AddAudioEffectCommand, 0), 0);
            auto* fx = ap.GetBehavior<zengine::audio::AudioEffect>();
            Require(fx != nullptr, "Inspector Add > Audio Effect did not attach an AudioEffect");
            fx->SetDecay(2.5f); fx->SetWetMix(0.7f); fx->SetBlendDistance(1.25f);
            const auto fxScenePath = editor.AssetsDirectory() / L"FxScene.zscene";
            Require(editor.SaveScene(fxScenePath), "could not save the audio-effect scene");
            const auto fxDoc = zengine::scenes::Decode(zengine::scenes::Load(fxScenePath));
            bool foundFx = false;
            for (const auto& o : fxDoc.objects) for (const auto& b : o.behaviors)
                if (b.kind == zengine::scenes::BehaviorData::Kind::AudioEffect
                    && std::abs(b.audioEffectDecay - 2.5f) < 0.001f && std::abs(b.audioEffectWetMix - 0.7f) < 0.001f)
                    foundFx = true;
            Require(foundFx, "AudioEffect did not serialize into the scene (v12)");

            // ZE-74: a scene with no lights renders unlit; adding a light populates the frame.
            Require(editor.BuildSceneFrame().lights.empty(), "an empty scene should have no lights");
            auto& lamp = editor.CreateEmptyGameObject();
            if (HWND rn2 = GetDlgItem(window, 3900)) SendMessageW(rn2, WM_KEYDOWN, VK_RETURN, 0);
            auto& lightBehaviour = lamp.AddBehavior<zengine::Light>();
            lightBehaviour.SetLightType(zengine::Light::Type::Point);
            lightBehaviour.SetColor({0.2f, 0.9f, 0.4f});
            lightBehaviour.SetIntensity(2.0f);
            lightBehaviour.SetRange(12);
            lamp.GetTransform().SetPosition({0, 3, 0});
            const auto litFrame = editor.BuildSceneFrame();
            Require(litFrame.lights.size() == 1 && litFrame.lights[0].type == 1
                    && std::abs(litFrame.lights[0].color.y - 0.9f) < 0.001f
                    && std::abs(litFrame.lights[0].intensity - 2.0f) < 0.001f
                    && std::abs(litFrame.lights[0].position.y - 3.0f) < 0.001f,
                    "adding a Light did not reach the render frame");
            Require(!litFrame.lightGizmos.empty(), "a light produces an editor gizmo");
            const auto litScenePath = editor.AssetsDirectory() / L"LitScene.zscene";
            Require(editor.SaveScene(litScenePath), "could not save the lit scene");
            const auto litDoc = zengine::scenes::Decode(zengine::scenes::Load(litScenePath));
            bool foundLight = false;
            for (const auto& o : litDoc.objects) for (const auto& b : o.behaviors)
                if (b.kind == zengine::scenes::BehaviorData::Kind::Light && b.lightType == 1
                    && std::abs(b.lightIntensity - 2.0f) < 0.001f && std::abs(b.lightRange - 12.0f) < 0.001f)
                    foundLight = true;
            Require(foundLight, "the Light did not serialize into the scene (v13)");

            // ZE-75: an Environment behaviour adds fog to the frame + serializes (v14).
            Require(!editor.BuildSceneFrame().environment.has_value(), "no Environment => no fog");
            auto& env = lamp.AddBehavior<zengine::Environment>();
            env.SetFog(zengine::Environment::FogMode::Linear);
            env.SetFogColor({0.4f, 0.45f, 0.6f});
            env.SetFogNear(2); env.SetFogFar(30);
            env.SetVolumetric(true);
            lightBehaviour.SetFogScatter(0.6f);
            const auto fogFrame = editor.BuildSceneFrame();
            Require(fogFrame.environment.has_value() && fogFrame.environment->fogMode == 1
                    && std::abs(fogFrame.environment->fogFar - 30.0f) < 0.001f
                    && fogFrame.environment->volumetric == 1
                    && std::abs(fogFrame.lights[0].fogScatter - 0.6f) < 0.001f,
                    "Environment / fogScatter did not reach the render frame");
            const auto fogScenePath = editor.AssetsDirectory() / L"FogScene.zscene";
            Require(editor.SaveScene(fogScenePath), "could not save the foggy scene");
            const auto fogDoc = zengine::scenes::Decode(zengine::scenes::Load(fogScenePath));
            bool foundEnv = false;
            for (const auto& o : fogDoc.objects) for (const auto& b : o.behaviors)
                if (b.kind == zengine::scenes::BehaviorData::Kind::Environment && b.envFogMode == 1) foundEnv = true;
            Require(foundEnv, "the Environment did not serialize into the scene (v14)");

            if (capture) { for (int i = 0; i < 3; ++i) editor.Render(); CaptureWindow(window, L"lighting-qa.bmp"); }
        }
        CoUninitialize();
        std::cout << "PASS: Mesh Renderer component, Inspector add/enable/clear, independent transforms, shared GPU meshes, captured async targets, scene instantiation, material instances\n";
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
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 140));
            Require(editor.SelectedGameObject()->Id() == previewId, "Scene-tree selection failed");
            Require(editor.SelectedGameObject()->GetTransform().Position().x == 0, "Object transforms are not independent");
            SetWindowTextW(x, L"2");
            editor.Render();
            Require(editor.SelectedGameObject()->GetTransform().Position().x == 2, "Preview transform editing failed");
            // Script creation, attachment and discovery coexist with GameObject editing.
            const auto script = editor.CreateScriptAsset();
            Require(std::filesystem::is_regular_file(script) && script.extension() == ".zsh", "Script asset was not saved");
            // "Add Script" is a submenu of project scripts under Add Behavior now, not a standalone button.
            Require(GetDlgItem(inspector, InspectorPanel::AddScriptButton) == nullptr, "Standalone Add Script button should be gone");
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 167));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 167));
            Require(editor.SelectedGameObject()->Id() == object->Id(), "Could not select the empty object for the Add Script submenu test");
            SendMessageW(inspector, WM_COMMAND, MAKEWPARAM(InspectorPanel::AddScriptSubFirst, 0), 0);
            Require(object->BehaviorCount() == 1 && dynamic_cast<const zengine::ScriptBehavior*>(&object->BehaviorAt(0)) != nullptr, "Add Behavior > Add Script submenu did not attach the project script");
            RECT client{}; GetClientRect(window, &client);
            const auto rowPoint = MAKELPARAM(50, client.bottom - 220);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, rowPoint);
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(50, 166));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 166));
            Require(object->BehaviorCount() == 1 && editor.SelectedGameObject()->Id() == object->Id(), "Script drag did not attach to targeted tree object");
            Require(!editor.AttachScript(object->Id(), script) && object->BehaviorCount() == 1, "Duplicate script attachment allowed");
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 140));
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

void ScriptIntegrationEditorTests(bool capture)
{
    TestDirectory test;
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM init failed");
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const auto window=editor.Create(SW_HIDE,test.path); editor.InitializeRenderer();
        const auto cube=editor.SelectedGameObject()->Id();
        const auto path=editor.CreateScriptAsset();
        editor.OpenScript(path);
        // The script editor is now an embedded child of the editor's Script tab.
        Require(editor.CurrentViewTab()==EditorShell::ViewTab::Script,"OpenScript did not switch to the Script tab");
        const HWND scriptWindow=FindWindowExW(window,nullptr,L"zEngineScriptEditor",nullptr);
        Require(scriptWindow!=nullptr,"Inline script editor did not open");
        const auto source=GetDlgItem(scriptWindow,ScriptEditor::SourceControl);
        Require(source!=nullptr,"Script source editor missing");
        const wchar_t* code=LR"(class NewBehavior : gameObject {
            label("Movement"); export float speed=2; export Vector3 direction=Vector3(1,0,0);
            export int ticks=0; export int draws=0;
            func start() { transform.position.y+=speed; }
            func update(float dt) { ticks+=1; transform.position+=direction*speed*dt; }
            func draw() { draws+=1; transform.rotation.z+=1; }
        })";
        SetWindowTextW(source,code);
        Require(!editor.Play(),"Play must reject unsaved documents");
        SendMessageW(scriptWindow,WM_COMMAND,ScriptEditor::SaveCommand,0);
        Require(zengine::scripts::Load(path).find("export float speed=2")!=std::string::npos,"Script save not on disk");
        Require(editor.AttachScript(cube,path),"Attach script to cube failed");
        const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
        const auto field=[&](int row) { return GetDlgItem(inspector,InspectorPanel::FirstBehaviorField+row); };
        // Mesh title/priority/material (ZE-65), script title/priority, Movement label, then variables.
        Require(field(4) && field(6) && field(7) && field(8),"Dynamic priority/variable controls missing");
        SetWindowTextW(field(4),L"1.5"); SetWindowTextW(field(6),L"6");
        SetWindowTextW(field(7),L"2");SetWindowTextW(field(8),L"0");SetWindowTextW(field(9),L"0");
        RECT vx{},vy{},vz{};GetWindowRect(field(7),&vx);GetWindowRect(field(8),&vy);GetWindowRect(field(9),&vz);
        Require(vx.top==vy.top && vy.top==vz.top && vx.right<vy.left && vy.right<vz.left,"Vector3 fields are not three distinct boxes");
        SetWindowTextW(field(6),L"nan"); SendMessageW(field(6),WM_KEYDOWN,VK_RETURN,0);
        wchar_t text[64]{}; GetWindowTextW(field(6),text,64);
        Require(std::wstring(text)==L"6","Invalid exported input did not revert");
        Require(editor.SelectedGameObject()->BehaviorAt(1).Priority()==1.5f,"Priority control did not edit behavior");
        auto& empty=editor.CreateEmptyGameObject(); const auto emptyId=empty.Id();
        Require(editor.AttachScript(emptyId,path),"Attach to empty object failed");
        Require(editor.Play(),"Editor Play failed"); editor.SetPaused(true);
        Require(zengine::As3D(editor.GameObjects().Find(cube))->GetTransform().Position().y==6 && empty.GetTransform().Position().y==2,"Start/independent exported values failed");
        editor.Step(); editor.Render();
        const auto& cubeTransform=zengine::As3D(editor.GameObjects().Find(cube))->GetTransform();
        Require(std::abs(cubeTransform.Position().x-0.2f)<0.0001f,"Script did not move rendered cube");
        Require(cubeTransform.Rotation().z==1 && empty.GetTransform().Rotation().z==0,"Draw was not gated by Mesh Renderer submission");
        editor.Render(); Require(cubeTransform.Rotation().z==1,"Paused Draw continued running");
        editor.Stop(); Require(cubeTransform.Position().x==0 && cubeTransform.Position().y==0 && cubeTransform.Rotation().z==0,"Stop failed to restore scene");
        // Selection + saved source edits rebuild metadata while preserving author overrides.
        // Scene-tree selection commits on button release, not mouse-down.
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(50,140));
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(50,140));
        SetWindowTextW(source,(std::wstring(code)+L"\n// saved change").c_str());
        SendMessageW(scriptWindow,WM_COMMAND,ScriptEditor::SaveCommand,0);
        GetWindowTextW(field(6),text,64); Require(std::wstring(text)==L"6","Save/reload lost Inspector override");
        SetFocus(field(6)); SetWindowTextW(field(6),L"9"); SendMessageW(field(6),WM_KEYDOWN,VK_ESCAPE,0);
        GetWindowTextW(field(6),text,64); Require(std::wstring(text)==L"6","Escape failed to restore exported value");
        if (capture)
        {
            SendMessageW(inspector,WM_VSCROLL,SB_PAGEDOWN,0);
            SendMessageW(inspector,WM_VSCROLL,SB_PAGEDOWN,0);
            editor.Render(); CaptureWindow(window,L"script-integration-qa.bmp");
        }
        SetWindowTextW(source,L"class NewBehavior : gameObject { func update(float dt) { missing=1; } }");
        SendMessageW(scriptWindow,WM_COMMAND,ScriptEditor::SaveCommand,0);
        Require(!editor.Play() && !editor.Playing(),"Compile error allowed Play");
        Require(cubeTransform.Position().x==0,"Compile failure changed scene");
        SetWindowTextW(source,code); SendMessageW(scriptWindow,WM_COMMAND,ScriptEditor::SaveCommand,0);
        Require(editor.Play(),"Could not recover from compilation error"); editor.Stop();
    }
    CoUninitialize();
    std::cout<<"PASS: create/edit/save .zsh, attach, Inspector variables/priority, Play/Step/Stop, actual mesh movement, draw gate, compile recovery\n";
}

// Answers only modal prompts owned by this test editor, on this test's UI thread.
struct ScenePromptAnswer
{
    HWND owner; int answer; UINT_PTR timer=0; bool handled=false;
    static inline ScenePromptAnswer* active=nullptr;
    ScenePromptAnswer(HWND window,int response):owner(window),answer(response)
    {
        active=this;
        timer=SetTimer(nullptr,0,10,[](HWND,UINT,UINT_PTR,DWORD) {
            if (!active) return;
            EnumThreadWindows(GetCurrentThreadId(),[](HWND dialog,LPARAM data)->BOOL {
                auto& self=*reinterpret_cast<ScenePromptAnswer*>(data);
                wchar_t name[32]{}; GetClassNameW(dialog,name,32);
                if (GetWindow(dialog,GW_OWNER)==self.owner && std::wstring(name)==L"#32770" && GetDlgItem(dialog,self.answer))
                { self.handled=true; SendMessageW(dialog,WM_COMMAND,self.answer,0); return FALSE; }
                return TRUE;
            },reinterpret_cast<LPARAM>(active));
        });
        Require(timer!=0,"Cannot install scene prompt test responder");
    }
    ~ScenePromptAnswer() { KillTimer(nullptr,timer); active=nullptr; }
};
void SceneEditorTests(bool capture)
{
    TestDirectory test; const auto project=test.path/L"Project"; const auto assets=project/L"Assets";
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM init failed");
    std::filesystem::path sceneA,sceneB;
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE,project); editor.InitializeRenderer();
        const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
        const auto field=[&](int id) { return GetDlgItem(inspector,id); };
        SetWindowTextW(field(InspectorPanel::NameField),L"Scene A Cube");
        SetWindowTextW(field(InspectorPanel::TagsField),L"player, visible");
        SetWindowTextW(field(InspectorPanel::FirstTransformField),L"1.25");
        const auto script=editor.CreateScriptAsset();
        zengine::scripts::Save(assets,script,"class NewBehavior : gameObject { label(\"Movement\"); export float speed=2; func update(float dt) { transform.position.x+=speed*dt; } }");
        Require(editor.AttachScript(1,script),"Scene script attach failed");
        SetWindowTextW(field(InspectorPanel::FirstBehaviorField+4),L"1.5"); // +3 material row (ZE-65) -> script priority is +4
        SetWindowTextW(field(InspectorPanel::FirstBehaviorField+6),L"6");
        auto& empty=editor.CreateEmptyGameObject(); const auto emptyId=empty.Id();
        SetWindowTextW(field(InspectorPanel::NameField),L"Scene A Empty");
        sceneA=assets/L"A.zscene"; Require(editor.SaveScene(sceneA) && !editor.SceneDirty(),"Save Scene failed");
        const auto original=zengine::scenes::Load(sceneA);
        Require(editor.NewScene() && editor.GameObjects().Size()==0 && !editor.SelectedGameObject(),"New Scene did not clear tree/Inspector");
        sceneB=editor.ScenePath(); Require(zengine::scenes::IsScene(sceneB),"New Scene asset not created");
        editor.CreateEmptyGameObject(); SetWindowTextW(field(InspectorPanel::NameField),L"Scene B Only");
        Require(editor.SaveScene(),"Save active new scene failed");
        RECT client{}; GetClientRect(window,&client);
        // A.zscene sorts first. Opening it uses the same native double-click path as the library.
        SendMessageW(window,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(50,client.bottom-220));
        Require(editor.ScenePath()==sceneA && editor.GameObjects().Size()==2,"Library double-click did not open scene");
        auto* cube=editor.GameObjects().Find(1);
        Require(cube!=nullptr,"Scene cube identity lost");
        Require(cube->Name()=="Scene A Cube",("Scene cube name changed to: "+cube->Name()).c_str());
        Require(cube->HasTag("player"),"Scene cube tags were lost");
        Require(editor.GameObjects().Find(emptyId)!=nullptr,"Scene empty object identity lost");
        Require(editor.GameObjects().Find(emptyId)->Name()=="Scene A Empty",("Scene empty name changed to: "+editor.GameObjects().Find(emptyId)->Name()).c_str());
        Require(zengine::As3D(cube)->GetTransform().Position().x==1.25f && cube->BehaviorAt(1).Priority()==1.5f,"Transform or behavior priority lost");
        wchar_t value[64]{}; GetWindowTextW(field(InspectorPanel::FirstBehaviorField+6),value,64);
        Require(std::wstring(value)==L"6" && editor.BuildSceneFrame().meshes.size()==1,"Script variable or mesh binding lost");
        Require(editor.Play(),"Current scene did not play"); editor.SetPaused(true); editor.Step(); editor.Render();
        Require(std::abs(zengine::As3D(cube)->GetTransform().Position().x-1.35f)<0.0001f,"Wrong scene/variable was played");
        SetWindowTextW(field(InspectorPanel::NameField),L"Temporary play name");
        SetWindowTextW(field(InspectorPanel::FirstBehaviorField+4),L"99");
        SendMessageW(field(InspectorPanel::MeshEnabled),BM_SETCHECK,BST_UNCHECKED,0);
        SendMessageW(inspector,WM_COMMAND,MAKEWPARAM(InspectorPanel::MeshEnabled,BN_CLICKED),reinterpret_cast<LPARAM>(field(InspectorPanel::MeshEnabled)));
        bool rejected=false; try { editor.SaveScene(); } catch (...) { rejected=true; }
        Require(rejected && zengine::scenes::Load(sceneA)==original,"Play-mode save overwrote authored scene");
        rejected=false; try { editor.OpenScene(sceneB); } catch (...) { rejected=true; }
        Require(rejected && editor.ScenePath()==sceneA,"Scene switched during Play");
        editor.Stop();
        Require(cube->Name()=="Scene A Cube" && zengine::As3D(cube)->GetTransform().Position().x==1.25f && cube->BehaviorAt(1).Priority()==1.5f && cube->BehaviorAt(0).Enabled() && !editor.SceneDirty(),"Stop did not restore scene Inspector state");
        SetWindowTextW(field(InspectorPanel::FirstTransformField),L"77"); Require(editor.SceneDirty(),"Scene edit did not mark dirty");
        { ScenePromptAnswer answer(window,IDCANCEL); Require(!editor.OpenScene(sceneB) && answer.handled && editor.ScenePath()==sceneA,"Cancel lost current scene"); }
        { ScenePromptAnswer answer(window,IDYES); Require(editor.OpenScene(sceneB) && answer.handled,"Save-and-switch failed"); }
        Require(editor.GameObjects().Size()==1 && editor.SelectedGameObject()->Name()=="Scene B Only" && editor.BuildSceneFrame().meshes.empty(),"Old scene content leaked into new scene");
        Require(editor.OpenScene(sceneA) && editor.SelectedGameObject()->GetTransform().Position().x==77,"Saved Inspector edits did not persist");
        SetWindowTextW(field(InspectorPanel::FirstTransformField),L"88");
        { ScenePromptAnswer answer(window,IDNO); Require(editor.OpenScene(sceneB) && answer.handled,"Discard-and-switch failed"); }
        Require(zengine::scenes::Decode(zengine::scenes::Load(sceneA)).objects.front().transform.Position().x==77,"Discard wrote changes to disk");
        const auto invalid=assets/L"Broken.zscene"; { std::ofstream out(invalid); out<<"invalid scene"; }
        rejected=false; try { editor.OpenScene(invalid); } catch (...) { rejected=true; }
        Require(rejected && editor.SelectedGameObject()->Name()=="Scene B Only","Broken scene destroyed current scene");
        // Old asynchronous mesh results cannot touch a different scene with the same object ID.
        std::vector<std::string> warnings;
        const auto imported=FbxImporter::Import(CreateSource(test.path/L"source"),assets,warnings);
        auto meshScene=zengine::scenes::Decode(zengine::scenes::Load(sceneA));
        const auto relative=std::filesystem::relative(imported,assets).generic_u8string();
        meshScene.objects[0].behaviors[0].asset=std::string(reinterpret_cast<const char*>(relative.data()),relative.size());
        const auto loading=assets/L"Loading.zscene"; zengine::scenes::Save(assets,loading,zengine::scenes::Encode(meshScene));
        Require(editor.OpenScene(loading),"Loading scene failed");
        Require(!editor.Play(),"Play began before restored models loaded");
        editor.Render(); // Starts an async load for ID 1.
        Require(editor.OpenScene(sceneB),"Could not switch away from restored mesh load");
        for (int i=0;i<30;++i) { editor.Render(); Sleep(2); }
        Require(editor.SelectedGameObject()->Name()=="Scene B Only" && editor.SelectedGameObject()->BehaviorCount()==0 && editor.BuildSceneFrame().meshes.empty(),"Stale mesh load modified another scene");
        if (capture) { Require(editor.OpenScene(sceneA),"Capture scene open"); editor.Render(); CaptureWindow(window,L"scene-assets-qa.bmp"); }
    }
    {
        EditorShell editor(GetModuleHandleW(nullptr)); (void)editor.Create(SW_HIDE,project); editor.InitializeRenderer();
        Require(editor.OpenScene(sceneA) && editor.GameObjects().Size()==2 && editor.SelectedGameObject()->GetTransform().Position().x==77,"Scene failed across editor restart");
        Require(editor.Play(),"Restored scene cannot play after restart"); editor.SetPaused(true); editor.Step(); editor.Render();
        Require(std::abs(editor.SelectedGameObject()->GetTransform().Position().x-77.1f)<0.0001f,"Exported speed lost across restart"); editor.Stop();
    }
    CoUninitialize();
    std::cout<<"PASS: new/save/open scene assets, double-click, Inspector persistence, current-scene Play, save/discard/cancel, restart, stale async loads\n";
}

void ProjectTests(bool capture);
void EditorBuildTests() {
    TestDirectory test;Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    const auto output=test.path/L"Builds";std::filesystem::create_directory(output);
    {
        EditorShell editor(GetModuleHandleW(nullptr));(void)editor.Create(SW_HIDE,test.path/L"Project");editor.InitializeRenderer();
        Require(editor.SaveScene(editor.AssetsDirectory()/L"Launch.zscene"),"Could not save build fixture");
        Require(editor.BuildProject(output) && editor.Building(),"Editor did not start asynchronous game build");
        bool rejected=false;try{editor.BuildProject(output);}catch(...){rejected=true;}Require(rejected,"Editor started overlapping builds");
        const auto deadline=GetTickCount64()+30000;while(editor.Building() && GetTickCount64()<deadline){editor.Render();Sleep(2);}
        if(editor.Building() || !editor.BuildError().empty() || !std::filesystem::is_regular_file(editor.LastBuild()))throw std::runtime_error("Editor game build did not complete: "+editor.BuildError());
        const auto packaged=zengine::projects::Open(editor.LastBuild().parent_path()/L"Data"/L"Game.zproject");
        Require(packaged.config.lastScene=="Assets/Launch.zscene","Editor exported wrong startup scene");
        Require(editor.Play(),"Play after build failed");rejected=false;try{editor.BuildProject(output);}catch(...){rejected=true;}Require(rejected,"Building during Play was allowed");editor.Stop();
    }
    CoUninitialize();std::cout<<"PASS: asynchronous editor build, startup scene selection, overlap and Play guards\n";
}
void BuildTests() {
    TestDirectory test;Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    const auto output=test.path/L"Output with spaces";std::filesystem::create_directory(output);
    auto project=zengine::projects::Create(test.path,L"Game \u03a9");const auto assets=zengine::projects::Assets(project);
    zengine::input::Ensure(assets);zengine::input::Map map{{"move",zengine::input::Kind::Button,{"Space"}}};
    const auto inputSource=zengine::input::Load(assets);zengine::input::Save(assets,map,&inputSource);
    std::filesystem::create_directory(assets/L"Scripts");
    const auto script=zengine::scripts::Create(assets/L"Scripts","Spinner");const auto original=zengine::scripts::Load(script);
    zengine::scripts::Save(assets,script,"class Spinner : gameObject { export float speed=60; export char suffix='?'; multiline export string note=\"a\\nb\"; export prefab template; export gameObject anchor; func start(){parent=find(\"Root\");gameObject made=template.spawn();made.transform.position.x=9;string word=\"ab\" & suffix;if(word[2]=='!' && note.truncate(1)==\"a\"){transform.position.x+=2;}Vector3 global=transform.global_position;if(global.x!=3){transform.position.x=999;}if(anchor.transform.position.x!=1){transform.position.x=888;}} func update(float dt){transform.rotation.y+=speed*dt; if(Input.is_action_just_pressed(\"move\")){transform.position.y+=3;}} }",&original);
    zengine::scenes::Document prefabDocument;zengine::scenes::ObjectData prefabRoot;prefabRoot.id=1;prefabRoot.name="Spawned Cube";zengine::scenes::BehaviorData prefabMesh;prefabMesh.asset=zengine::MeshRenderer::CubeAsset;prefabRoot.behaviors.push_back(prefabMesh);prefabDocument.objects.push_back(prefabRoot);
    const auto prefab=zengine::prefabs::Create(assets,prefabDocument);const auto prefabPath=std::filesystem::relative(prefab,assets).generic_u8string();const std::string prefabRef(reinterpret_cast<const char*>(prefabPath.data()),prefabPath.size());
    std::vector<std::string> warnings;const auto model=FbxImporter::Import(CreateSource(test.path/L"source"),assets/L"Models",warnings);
    zengine::scenes::Document scene;zengine::scenes::ObjectData object;object.id=1;object.name="Exported Actor";
    zengine::scenes::BehaviorData mesh;const auto modelRef=std::filesystem::relative(model,assets).generic_u8string();mesh.asset.assign(modelRef.begin(),modelRef.end());object.behaviors.push_back(mesh);
    zengine::scenes::BehaviorData behavior;behavior.kind=zengine::scenes::BehaviorData::Kind::Script;behavior.asset="Scripts/Spinner.zsh";behavior.variables["speed"]=120.0;behavior.variables["suffix"]=U'!';behavior.variables["template"]=zengine::script::PrefabRef{prefabRef};behavior.objectReferences["anchor"]=2;object.behaviors.push_back(behavior);scene.objects.push_back(object);
    zengine::scenes::ObjectData parent;parent.id=2;parent.name="Root";parent.transform.SetPosition({1,0,0});scene.objects.push_back(parent);
    const auto first=assets/L"First.zscene";zengine::scenes::Save(assets,first,zengine::scenes::Encode(scene));zengine::projects::TrackScene(project,first);
    scene.objects[0].name="Second Actor";scene.objects[0].behaviors[1].variables["speed"]=30.0;
    const auto second=assets/L"Second.zscene";zengine::scenes::Save(assets,second,zengine::scenes::Encode(scene));zengine::projects::TrackScene(project,second);zengine::projects::Save(project);
    {zengine::game::Session session(project,"Assets/First.zscene");session.Start();zengine::input::Hardware hardware;hardware.keys[VK_SPACE]=true;session.Tick(1.0f/60,hardware);session.Tick(1.0f/60,hardware);
        Require(zengine::As3D(session.Objects().At(0)).GetTransform().Position().y==3 && session.Objects().At(0).Parent()==2 && session.Objects().Size()==3 && zengine::As3D(session.Objects().At(2)).GetTransform().Position().x==9,"Standalone runtime input, parenting, or prefab spawning failed");
        Require(zengine::As3D(session.Objects().At(0)).GetTransform().Position().x!=888,"Standalone build did not bind an exported scene-tree object reference");}
    {   // ZE-63: Scene.load("<name>") switches the running scene at the end of the tick.
        const auto switcherScript=zengine::scripts::Create(assets/L"Scripts","Switcher");const auto switcherOrig=zengine::scripts::Load(switcherScript);
        zengine::scripts::Save(assets,switcherScript,"class Switcher : gameObject { func start(){ Scene.load(\"Second\"); } func update(float dt){} }",&switcherOrig);
        zengine::scenes::Document sw;zengine::scenes::ObjectData so;so.id=1;so.name="Switcher";
        zengine::scenes::BehaviorData sb;sb.kind=zengine::scenes::BehaviorData::Kind::Script;sb.asset="Scripts/Switcher.zsh";so.behaviors.push_back(sb);sw.objects.push_back(so);
        const auto switcherScene=assets/L"Switcher.zscene";zengine::scenes::Save(assets,switcherScene,zengine::scenes::Encode(sw));
        zengine::projects::TrackScene(project,switcherScene);zengine::projects::Save(project);
        zengine::game::Session s(project,"Assets/Switcher.zscene");s.Start();
        const auto before=s.SceneGeneration();
        s.Tick(1.0f/60,{});
        Require(s.Scene()=="Assets/Second.zscene" && s.SceneGeneration()!=before,"Scene.load did not switch the running scene by name");
        Require(s.Objects().Size()>=2 && s.Objects().At(0).Name()=="Second Actor","Scene.load did not instantiate the target scene objects");
    }
    unsigned progress=0;const auto executable=zengine::game::Export(project,first,{},output,zengine::game::ExecutableDirectory(),[&](unsigned value,const std::string&){Require(value>=progress,"Build progress regressed");progress=value;});
    Require(progress==100 && std::filesystem::exists(executable) && !std::filesystem::exists(executable.parent_path()/L"zEngine.exe"),"Export did not produce an editor-independent executable");
    Require(std::filesystem::exists(executable.parent_path()/L"Data"/L"Assets"/std::filesystem::relative(model,assets)),"Packaged model missing");
    Require(std::filesystem::exists(executable.parent_path()/L"Data"/L"Assets"/std::filesystem::relative(model.parent_path()/L"albedo"/L"2.image",assets)),"Packaged albedo missing");
    const auto duplicate=zengine::game::Export(project,first,{},output,zengine::game::ExecutableDirectory());Require(duplicate!=executable && std::filesystem::exists(executable),"Build overwrote previous output");
    bool rejected=false;try{zengine::game::Export(project,first,{},assets,zengine::game::ExecutableDirectory());}catch(...){rejected=true;}Require(rejected,"Build was allowed inside source Assets");
    const auto goodCode=zengine::scripts::Load(script);zengine::scripts::Save(assets,script,"broken source",&goodCode);
    rejected=false;try{zengine::game::Export(project,first,{},output,zengine::game::ExecutableDirectory());}catch(...){rejected=true;}Require(rejected && std::distance(std::filesystem::directory_iterator(output),std::filesystem::directory_iterator{})==2,"Failed build published partial output or damaged existing builds");
    const auto brokenCode=zengine::scripts::Load(script);zengine::scripts::Save(assets,script,goodCode,&brokenCode);
    const auto run=[&](const std::wstring& args,DWORD expected){
        std::wstring command=L"\""+executable.wstring()+L"\" --frames 3 "+args;
        STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
        Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,test.path.c_str(),&start,&process)!=FALSE,"Could not launch packaged game");
        CloseHandle(process.hThread);const auto wait=WaitForSingleObject(process.hProcess,30000);DWORD code=999;GetExitCodeProcess(process.hProcess,&code);
        if(wait!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,99);CloseHandle(process.hProcess);
        Require(wait==WAIT_OBJECT_0 && code==expected,"Packaged game returned unexpected status");
    };
    // Move the source project away: the exported game must depend only on its own files.
    std::filesystem::rename(project.file.parent_path(),test.path/L"Original project moved");
    std::filesystem::rename(test.path/L"source",test.path/L"Original import moved");
    const auto report=test.path/L"player-report.txt";run(L"--report \""+report.wstring()+L"\"",0);
    std::ifstream in(report);std::string text{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    Require(text.find("meshes 2")!=text.npos && text.find("position 2 0 0 rotation 0 6 0")!=text.npos && text.find("Spawned Cube")!=text.npos && text.find("position 9 0 0")!=text.npos,"Packaged game did not render/spawn prefab and execute exported script values");
    Require(text.find("parent 2")!=text.npos,"Standalone executable did not apply scripted parenting");
    run(L"--scene Assets/Second.zscene --report \""+report.wstring()+L"\"",0);std::ifstream secondReport(report);text.assign(std::istreambuf_iterator<char>(secondReport),{});
    Require(text.find("Second Actor")!=text.npos && text.find("rotation 0 1.5 0")!=text.npos,"Packaged scenes lost independent variables");
    run(L"--scene Assets/Missing.zscene",1);
    std::filesystem::rename(executable.parent_path()/L"Data"/L"Assets"/L"Scripts",executable.parent_path()/L"Data"/L"Assets"/L"MissingScripts");run(L"",1);
    CoUninitialize();std::cout<<"PASS: standalone runtime input, all-scene packaging, albedo, exported variables, Unicode/spaces, relocated source, non-editor process, missing-data errors, non-overwriting builds\n";
}
void FolderTests(bool capture) {
    TestDirectory test;Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    {
        EditorShell editor(GetModuleHandleW(nullptr));const auto window=editor.Create(SW_HIDE,test.path/L"Project");editor.InitializeRenderer();
        const auto root=editor.AssetsDirectory(),actors=editor.CreateAssetFolder(L"Actors");
        editor.OpenAssetFolder(actors);const auto scripts=editor.CreateAssetFolder(L"Scripts");editor.OpenAssetFolder(scripts);
        const auto script=editor.CreateScriptAsset();Require(script.parent_path()==scripts,"Script ignored current folder");
        Require(GetDlgItem(window,3900)!=nullptr,"New script did not enter rename mode");SendMessageW(GetDlgItem(window,3900),WM_KEYDOWN,VK_RETURN,0);
        Require(editor.NewScene() && editor.ScenePath().parent_path()==scripts,"Scene ignored current folder");
        Require(GetDlgItem(window,3900)!=nullptr,"New scene did not enter rename mode");SendMessageW(GetDlgItem(window,3900),WM_KEYDOWN,VK_RETURN,0);
        auto& object=editor.CreateEmptyGameObject();Require(GetDlgItem(window,3900)!=nullptr,"New GameObject did not enter rename mode");SendMessageW(GetDlgItem(window,3900),WM_KEYDOWN,VK_RETURN,0);editor.AssignCube(object.Id());
        const auto prefab=editor.CreatePrefab(object.Id());Require(prefab.parent_path()==scripts,"Prefab ignored current folder");
        Require(GetDlgItem(window,3900)!=nullptr,"New prefab did not enter rename mode");SendMessageW(GetDlgItem(window,3900),WM_KEYDOWN,VK_RETURN,0);Require(editor.SaveScene(),"Save nested scene failed");
        std::vector<std::string> warnings;const auto model=FbxImporter::Import(CreateSource(test.path/L"source"),actors,warnings);
        const auto id=editor.CreateEmptyGameObject().Id();Require(GetDlgItem(window,3900)!=nullptr,"Created model GameObject did not enter rename mode");SetWindowTextW(GetDlgItem(window,3900),L"Grid Actor");SendMessageW(GetDlgItem(window,3900),WM_KEYDOWN,VK_RETURN,0);Require(editor.GameObjects().Find(id)->Name()=="Grid Actor","Inline GameObject rename failed");editor.QueueModel(model,id);
        const auto deadline=GetTickCount64()+10000;while(editor.BuildSceneFrame().meshes.size()<2 && GetTickCount64()<deadline){editor.Render();Sleep(2);}
        Require(editor.BuildSceneFrame().meshes.size()==2,"Model in nested asset folder could not render");Require(editor.SaveScene(),"Save folder model failed");
        editor.OpenAssetFolder(root);const auto entries=assetLibrary::List(root,root);
        Require(entries.size()==2 && assetLibrary::Type(entries[1])==assetLibrary::Kind::Input && entries[0]==actors,"Root listing flattened folders or lost protected Input Map");
        bool rejected=false;try{editor.OpenAssetFolder(test.path);}catch(...){rejected=true;}Require(rejected,"Folder navigation escaped project");
        rejected=false;try{editor.CreateAssetFolder(L"../bad");}catch(...){rejected=true;}Require(rejected,"Folder name allowed traversal");
        rejected=false;try{editor.CreateAssetFolder(L"Actors");}catch(...){rejected=true;}Require(rejected,"Existing folder overwritten");
        RECT area{};GetClientRect(window,&area);
        SendMessageW(window,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(50,area.bottom-220));
        Require(editor.AssetFolder()==actors,"Double-click did not open folder");
        SendMessageW(window,WM_COMMAND,EditorShell::UpFolderCommand,0);Require(editor.AssetFolder()==root,"Up navigation failed");
        editor.OpenAssetFolder(scripts);
        Require(assetLibrary::Type(script)==assetLibrary::Kind::Script && assetLibrary::Type(prefab)==assetLibrary::Kind::Prefab && assetLibrary::Type(editor.ScenePath())==assetLibrary::Kind::Scene && assetLibrary::Type(model)==assetLibrary::Kind::Model,"Asset icon classification failed");
        const auto archive=editor.CreateAssetFolder(L"Archive");editor.MoveAsset(script,archive);const auto moved=archive/script.filename();
        Require(!std::filesystem::exists(script)&&std::filesystem::exists(moved),"Moving an asset into a folder failed");
        editor.RenameAsset(moved,L"MovedScript");const auto renamed=archive/L"MovedScript.zsh";
        Require(zengine::scripts::Load(renamed).find("class MovedScript")!=std::string::npos,"Renaming a script did not update its class name");
        Require(!std::filesystem::exists(moved)&&std::filesystem::exists(renamed)&&assetLibrary::Type(renamed)==assetLibrary::Kind::Script,"Asset rename failed or lost its type");
        if(capture){editor.Render();CaptureWindow(window,L"asset-folders-qa.bmp");}
    }
    CoUninitialize();std::cout<<"PASS: asset grid navigation, automatic inline rename, moving and renaming assets, nested assets, protected Input Map, path containment\n";
}
void TextInspectorTests(bool capture) {
    TestDirectory test;Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    {
        EditorShell editor(GetModuleHandleW(nullptr));const auto window=editor.Create(SW_HIDE,test.path/L"Project");editor.InitializeRenderer();
        Require(editor.SaveScene(editor.AssetsDirectory()/L"Initial.zscene") && editor.NewScene(),"Text test scene setup");
        const auto id=editor.CreateEmptyGameObject().Id();const auto script=editor.CreateScriptAsset();
        const auto source=zengine::scripts::Load(script);
        zengine::scripts::Save(editor.AssetsDirectory(),script,"class NewBehavior : gameObject {multiline export string notes=\"first\\nsecond\";export char mark='A';export string single=\"one line\";}",&source);
        Require(editor.AttachScript(id,script),"Attach text script");
        const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr),notes=GetDlgItem(inspector,InspectorPanel::FirstBehaviorField+2),single=GetDlgItem(inspector,InspectorPanel::FirstBehaviorField+4);
        Require(notes && (GetWindowLongPtrW(notes,GWL_STYLE)&ES_MULTILINE) && !(GetWindowLongPtrW(single,GWL_STYLE)&ES_MULTILINE),"Multiline style leaked or was omitted");
        Require(SendMessageW(notes,EM_GETLINECOUNT,0,0)==2,"Initial LF text did not display on separate lines");
        SetFocus(notes);SetWindowTextW(notes,L"hello\r\nworld");const auto end=GetWindowTextLengthW(notes);SendMessageW(notes,EM_SETSEL,end,end);SendMessageW(notes,WM_CHAR,VK_RETURN,0);
        Require(SendMessageW(notes,EM_GETLINECOUNT,0,0)==3,"Enter did not insert a newline");
        SetDlgItemTextW(inspector,InspectorPanel::FirstBehaviorField+3,L"\u00e9");Require(editor.SaveScene(),"Save multiline/character fields");
        const auto scene=editor.ScenePath();const auto document=zengine::scenes::Decode(zengine::scenes::Load(scene));
        const auto& variables=document.objects[0].behaviors[0].variables;
        Require(std::get<std::string>(variables.at("notes"))=="hello\nworld\n",("Multiline value was not persisted canonically: "+std::get<std::string>(variables.at("notes"))).c_str());
        Require(std::get<char32_t>(variables.at("mark"))==U'\u00e9',"Character inspector value not persisted");
        Require(editor.OpenScene(scene),"Reload text scene");
        Require(SendMessageW(GetDlgItem(inspector,InspectorPanel::FirstBehaviorField+2),EM_GETLINECOUNT,0,0)==3,"Multiline text lost on reload");
        if(capture){editor.Render();CaptureWindow(window,L"text-inspector-qa.bmp");}
    }
    CoUninitialize();std::cout<<"PASS: multiline Inspector style, Enter/newlines, character values, saving and reloading\n";
}
void HierarchyTests(bool capture) {
    TestDirectory test;Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    {
        EditorShell editor(GetModuleHandleW(nullptr));const auto window=editor.Create(SW_HIDE,test.path/L"Project");editor.InitializeRenderer();
        const auto root=editor.SelectedGameObject()->Id();
        auto& child=editor.CreateEmptyGameObject();const auto childId=child.Id();child.SetName("Child");child.GetTransform().SetPosition({2,0,0});editor.AssignCube(childId);
        auto& parent=editor.CreateEmptyGameObject();const auto parentId=parent.Id();parent.SetName("Parent");parent.GetTransform().SetPosition({3,0,0});
        const auto drag=[&](int from,int to){SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(80,143+27*from));SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(80,143+27*to));SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(80,143+27*to));};
        drag(1,2);Require(editor.GameObjects().Find(childId)->Parent()==parentId,"Scene tree drag failed to parent");
        Require(editor.GameObjects().HierarchyOrder()==std::vector<zengine::GameObjectId>{root,parentId,childId},"Tree hierarchy ordering incorrect");
        auto frame=editor.BuildSceneFrame();Require(frame.meshes.size()==2 && frame.meshes[1].parentMatrix && frame.meshes[1].parentMatrix->_41==3 && child.GetTransform().Position().x==2,"Parent transform missing from rendered child or local transform changed");
        editor.SetObjectParent(parentId,root);
        bool rejected=false;try{editor.SetObjectParent(root,childId);}catch(...){rejected=true;}Require(rejected,"Editor accepted parenting cycle");
        // Collapsing the root hides descendants; clicking a now-empty row must not select one.
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,143));
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(80,170));SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(80,170));
        Require(editor.SelectedGameObject()->Id()==parentId,"Collapsed descendants still receive row clicks");
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,143));
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(80,197));SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(80,197));
        Require(editor.SelectedGameObject()->Id()==childId,"Expanded child row selected wrong object");
        SendMessageW(window,WM_COMMAND,EditorShell::UnparentCommand,0);Require(child.Parent()==0,"Unparent context command failed");
        editor.SetObjectParent(childId,parentId);Require(editor.SaveScene(editor.AssetsDirectory()/L"Hierarchy.zscene"),"Save hierarchy failed");const auto path=editor.ScenePath();
        Require(editor.OpenScene(path) && editor.GameObjects().Find(childId)->Parent()==parentId && editor.GameObjects().Find(parentId)->Parent()==root,"Forward parent references failed scene reload");
        const auto prefab=editor.CreatePrefab(parentId);const auto instance=editor.InstantiatePrefab(prefab);
        editor.SetObjectParent(instance,root);Require(editor.SaveScene(),"Save parented prefab instance failed");
        const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
        SetDlgItemTextW(inspector,InspectorPanel::FirstTransformField+6,L"4");
        Require(zengine::As3D(editor.GameObjects().Find(instance))->GetTransform().Scale().x==4,"Prefab override fixture failed");
        editor.RevertPrefabTransform(instance);Require(editor.GameObjects().Find(instance)->Parent()==root && zengine::As3D(editor.GameObjects().Find(instance))->GetTransform().Scale().x==1,"Reverting prefab transforms lost parent or retained override");
        Require(editor.SaveScene() && editor.OpenScene(path) && editor.GameObjects().Find(instance)->Parent()==root,"Prefab instance parent lost on reload");
        auto generated=zengine::GameObjectId{};for(std::size_t i=0;i<editor.GameObjects().Size();++i)if(editor.GameObjects().At(i).Parent()==instance)generated=editor.GameObjects().At(i).Id();
        Require(generated!=0,"Prefab children missing");rejected=false;try{editor.SetObjectParent(root,generated);}catch(...){rejected=true;}Require(rejected,"Generated prefab child accepted as authoring parent");
        const auto rigid=editor.CreateGameObject(EditorShell::ObjectPreset::RigidBody,root).Id();Require(editor.GameObjects().Find(rigid)->Parent()==root&&editor.GameObjects().Find(rigid)->GetBehavior<zengine::physics::Collider>()&&editor.GameObjects().Find(rigid)->GetBehavior<zengine::physics::RigidBody>(),"Rigid-body context preset is incomplete");
        editor.CopyGameObject(rigid);const auto pasted=editor.PasteGameObject(root);Require(editor.GameObjects().Find(pasted)&&editor.GameObjects().Find(pasted)->Parent()==root&&editor.GameObjects().Find(pasted)->GetBehavior<zengine::physics::RigidBody>(),"Context copy/paste lost hierarchy or behaviors");
        editor.DeleteGameObject(rigid);Require(!editor.GameObjects().Find(rigid)&&editor.GameObjects().Find(pasted),"Context delete removed the wrong hierarchy");
        const auto cube=editor.CreateGameObject(EditorShell::ObjectPreset::Cube,root).Id();Require(editor.GameObjects().Find(cube)->GetBehavior<zengine::MeshRenderer>()->Asset()==zengine::MeshRenderer::CubeAsset,"Cube context preset has no cube mesh");
        const auto camera=editor.CreateGameObject(EditorShell::ObjectPreset::Camera,root).Id();Require(editor.GameObjects().Find(camera)->Name()=="Camera","Camera context preset missing");editor.DeleteGameObject(cube);editor.DeleteGameObject(camera);editor.DeleteGameObject(pasted);
        Require(editor.Play(),"Hierarchy Play failed");rejected=false;try{editor.SetObjectParent(instance,0);}catch(...){rejected=true;}Require(rejected,"Authoring reparent allowed during Play");editor.Stop();
        if(capture){editor.Render();CaptureWindow(window,L"hierarchy-qa.bmp");}
    }
    CoUninitialize();std::cout<<"PASS: tree parenting, context presets/copy/paste/delete, unparenting, collapse/expand, local transforms, save/reload, nested prefabs and Play edit guards\n";
}
void GizmoTests(bool capture);
void PrefabTests();
void ProjectStartupTests(const std::string& mode,bool capture);

// Drives the ObjectPicker modal from a timer inside its own message loop. The handler
// must not throw: it runs from a Win32 timer callback nested in the modal message pump.
struct PickerDriver
{
    static inline PickerDriver* active=nullptr;
    std::function<void(HWND)> onDialog;
    int listCountSeen=-1; bool handled=false; UINT_PTR timer=0;
    explicit PickerDriver(std::function<void(HWND)> handler):onDialog(std::move(handler))
    {
        MSG quit{}; while (PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
        active=this;
        timer=SetTimer(nullptr,0,15,[](HWND,UINT,UINT_PTR,DWORD){
            if(!active || active->handled) return;
            EnumThreadWindows(GetCurrentThreadId(),[](HWND dialog,LPARAM)->BOOL{
                wchar_t type[32]{}; GetClassNameW(dialog,type,32);
                if(std::wstring(type)==L"#32770" && GetDlgItem(dialog,ObjectPicker::ResultList) && IsWindowVisible(dialog))
                {
                    active->handled=true;
                    active->onDialog(dialog);
                    active->listCountSeen=static_cast<int>(SendMessageW(GetDlgItem(dialog,ObjectPicker::ResultList),LB_GETCOUNT,0,0));
                    return FALSE;
                }
                return TRUE;
            },0);
        });
        Require(timer!=0,"Cannot automate the picker modal");
    }
    ~PickerDriver(){ KillTimer(nullptr,timer); active=nullptr; }
};
void ObjectPickerTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    const auto request=[]{
        ObjectPicker::Request r; r.title=L"Pick a thing"; r.allowClear=true;
        r.items={
            {L"Player",L"Scripts/Player.zsh",L"Script",L"Scripts/Player.zsh",assetLibrary::Kind::Script},
            {L"Enemy", L"Scripts/Enemy.zsh", L"Script",L"Scripts/Enemy.zsh", assetLibrary::Kind::Script},
            {L"Bullet",L"Bullet.zprefab",    L"Prefab",L"Bullet.zprefab",    assetLibrary::Kind::Prefab},
        };
        return r;
    };
    const auto filterSearch=[](HWND dlg,const wchar_t* text){
        SetDlgItemTextW(dlg,ObjectPicker::SearchField,text);
        SendMessageW(dlg,WM_COMMAND,MAKEWPARAM(ObjectPicker::SearchField,EN_CHANGE),reinterpret_cast<LPARAM>(GetDlgItem(dlg,ObjectPicker::SearchField)));
    };
    const auto pickRow=[](HWND dlg,int row){
        HWND list=GetDlgItem(dlg,ObjectPicker::ResultList);
        SendMessageW(list,LB_SETCURSEL,row,0);
        SendMessageW(dlg,WM_COMMAND,MAKEWPARAM(ObjectPicker::OkButton,BN_CLICKED),0);
    };

    // Window picker: live search narrows the list, then Select returns that item.
    {
        PickerDriver driver([&](HWND dlg){ filterSearch(dlg,L"enem"); pickRow(dlg,0); });
        const auto choice=ObjectPicker::Window(GetDesktopWindow(),request());
        Require(driver.listCountSeen==1,"Search did not narrow the picker to one row");
        Require(choice.picked && choice.value==L"Scripts/Enemy.zsh","Window picker search+select failed");
    }
    // Type-filter chip hides a whole group (turning off "Script" leaves only the Prefab).
    {
        PickerDriver driver([&](HWND dlg){
            HWND chip=GetDlgItem(dlg,ObjectPicker::FirstFilterChip); // groups in first-seen order: Script, then Prefab
            SendMessageW(chip,BM_SETCHECK,BST_UNCHECKED,0);
            SendMessageW(dlg,WM_COMMAND,MAKEWPARAM(ObjectPicker::FirstFilterChip,BN_CLICKED),reinterpret_cast<LPARAM>(chip));
            if(capture){ SetForegroundWindow(dlg); Sleep(60); CaptureScreenRect(dlg,L"object-picker-qa.bmp"); }
            pickRow(dlg,0);
        });
        const auto choice=ObjectPicker::Window(GetDesktopWindow(),request());
        Require(driver.listCountSeen==1,"Filter chip did not hide the Script group");
        Require(choice.picked && choice.value==L"Bullet.zprefab","Picker chip filter/select failed");
    }
    // Clear and Cancel results.
    {
        PickerDriver driver([&](HWND dlg){ SendMessageW(dlg,WM_COMMAND,MAKEWPARAM(ObjectPicker::ClearButton,BN_CLICKED),0); });
        const auto choice=ObjectPicker::Window(GetDesktopWindow(),request());
        Require(choice.cleared && !choice.picked,"Picker Clear did not report a cleared choice");
    }
    {
        PickerDriver driver([&](HWND dlg){ SendMessageW(dlg,WM_COMMAND,MAKEWPARAM(IDCANCEL,0),0); });
        const auto choice=ObjectPicker::Window(GetDesktopWindow(),request());
        Require(!choice.cleared && !choice.picked,"Picker Cancel returned a value");
    }
    // Dropdown shape: frameless, still a #32770 with the same controls.
    {
        PickerDriver driver([&](HWND dlg){ pickRow(dlg,2); });
        const auto choice=ObjectPicker::Dropdown(GetDesktopWindow(),RECT{100,100,320,124},request());
        Require(choice.picked && choice.value==L"Bullet.zprefab","Dropdown picker select failed");
    }
    // Test responder path: the editor's Add Script -> Browse opens the picker; a responder attaches.
    {
        TestDirectory test;
        ObjectPicker::SetTestResponder([](const ObjectPicker::Request& r)->ObjectPicker::Choice {
            for(const auto& item:r.items) if(item.group==L"Script") return ObjectPicker::Choice::Pick(item.value);
            return ObjectPicker::Choice::Cancel();
        });
        {
            EditorShell editor(GetModuleHandleW(nullptr));
            const HWND window=editor.Create(SW_HIDE,test.path/"Project");
            editor.InitializeRenderer();
            const auto script=editor.CreateScriptAsset(); (void)script;
            auto& object=editor.CreateEmptyGameObject();
            const HWND inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
            SendMessageW(inspector,WM_COMMAND,MAKEWPARAM(InspectorPanel::AddScriptCommand,0),0); // the "Browse..." item
            Require(object.BehaviorCount()==1 && dynamic_cast<const zengine::ScriptBehavior*>(&object.BehaviorAt(0))!=nullptr,
                    "Add Script > Browse did not route through the object picker responder");
        }
        ObjectPicker::SetTestResponder(nullptr);
    }
    CoUninitialize();
    std::cout<<"PASS: object picker window/dropdown, live search, type-filter chips, clear/cancel, editor integration\n";
}
void CameraTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    TestDirectory test;
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const HWND window=editor.Create(SW_HIDE,test.path/"Project");
        editor.InitializeRenderer();

        auto& first=editor.CreateGameObject(EditorShell::ObjectPreset::Camera);
        Require(first.GetBehavior<zengine::Camera>()!=nullptr,"Camera preset did not attach a Camera behavior");
        Require(first.HasTag(zengine::Camera::MainTag),"A new camera did not become the main camera");
        first.GetTransform().SetPosition({0,2,-8}); first.GetTransform().SetRotation({8,0,0});

        // A second camera takes over as main.
        auto& second=editor.CreateGameObject(EditorShell::ObjectPreset::Camera);
        Require(second.HasTag(zengine::Camera::MainTag) && !first.HasTag(zengine::Camera::MainTag),"Spawning a second camera did not transfer the main tag");

        // Re-tagging the first camera "main" via the object (as the Inspector would) flips it back exclusively.
        first.SetTags({std::string(zengine::Camera::MainTag)});
        second.SetTags({std::string(zengine::Camera::MainTag)}); // both tagged, as if edited directly
        editor.SyncMainCamera(first.Id());
        Require(first.HasTag(zengine::Camera::MainTag) && !second.HasTag(zengine::Camera::MainTag),"Enabling main on another camera did not remove it elsewhere");

        // The Game tab views the scene through the main camera; the Scene tab draws its frustum.
        editor.SetViewTab(EditorShell::ViewTab::Game);
        const auto gameFrame=editor.BuildSceneFrame();
        Require(gameFrame.gameView.has_value(),"Game tab did not view through the main camera");
        editor.SetViewTab(EditorShell::ViewTab::Scene);
        const auto sceneFrame=editor.BuildSceneFrame();
        Require(sceneFrame.cameraGizmos.size()==2 && !sceneFrame.gameView.has_value(),"Scene tab lost the camera frustum gizmos");

        // fov/near/far are editable and serialised.
        first.GetBehavior<zengine::Camera>()->SetFieldOfView(75);
        first.GetBehavior<zengine::Camera>()->SetNearPlane(0.25f);
        Require(editor.SaveScene(editor.AssetsDirectory()/L"Cam.zscene"),"Could not save the camera scene");
        Require(editor.OpenScene(editor.AssetsDirectory()/L"Cam.zscene"),"Could not reload the camera scene");
        const zengine::Camera* reloaded=nullptr;
        for(std::size_t i=0;i<editor.GameObjects().Size();++i)
            if(auto* c=const_cast<zengine::GameObject&>(zengine::As3D(editor.GameObjects().At(i))).GetBehavior<zengine::Camera>(); c && editor.GameObjects().At(i).HasTag(zengine::Camera::MainTag)) reloaded=c;
        Require(reloaded && std::abs(reloaded->FieldOfView()-75)<0.01f && std::abs(reloaded->NearPlane()-0.25f)<0.001f,"Camera settings were not serialised");

        // Camera is a referenceable script type.
        const auto probe=zengine::script::Compiler::Compile("class Rig : gameObject { export Camera view; func start(){ view; } }","Rig");
        Require(static_cast<bool>(probe),"Camera is not a usable script reference type");

        if(capture){ editor.SetViewTab(EditorShell::ViewTab::Scene); for(int i=0;i<6;++i) editor.Render(); CaptureScreen(window,L"camera-qa.bmp"); }
    }
    CoUninitialize();
    std::cout<<"PASS: camera GameObject, single main-camera tag, frustum gizmos, Game-tab camera view, serialisation, script reference type\n";
}
void UiEditorTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    TestDirectory test;
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const HWND window=editor.Create(SW_HIDE,test.path/"Project");
        editor.InitializeRenderer();

        auto& panel=editor.CreateUiControl("panel");
        Require(panel.Is2D(),"UI control is not a GameObject2D");
        auto* panelUi=panel.GetBehavior<zengine::ui::PanelContainer>();
        Require(panelUi!=nullptr,"panel did not get a PanelContainer behavior");
        panelUi->SetAnchor(zengine::ui::Anchor::Fill);
        panelUi->SetTint({0.1f,0.1f,0.14f,0.85f});

        auto& bar=editor.CreateUiControl("progressBar",panel.Id());
        Require(bar.Parent()==panel.Id(),"UI child was not parented to the selected control");
        auto* barUi=bar.GetBehavior<zengine::ui::ProgressBar>();
        barUi->SetValue(0.3f); barUi->SetSize({260,20}); barUi->SetOrder(1);

        auto& label=editor.CreateUiControl("text",panel.Id());
        label.GetBehavior<zengine::ui::Text>()->SetValue("Loading...");

        Require(editor.SaveScene(editor.AssetsDirectory()/L"Menu.zscene"),"Could not save the UI scene");
        Require(editor.OpenScene(editor.AssetsDirectory()/L"Menu.zscene"),"Could not reload the UI scene");

        const zengine::ui::ProgressBar* reloadedBar=nullptr; const zengine::ui::PanelContainer* reloadedPanel=nullptr;
        for(std::size_t i=0;i<editor.GameObjects().Size();++i)
        {
            auto& object=const_cast<zengine::ObjectStore&>(editor.GameObjects()).At(i);
            if(auto* b=object.GetBehavior<zengine::ui::ProgressBar>()) reloadedBar=b;
            if(auto* p=object.GetBehavior<zengine::ui::PanelContainer>()) reloadedPanel=p;
        }
        Require(reloadedPanel && reloadedPanel->GetAnchor()==zengine::ui::Anchor::Fill && reloadedPanel->Tint().w>0.8f,"Panel props lost across save/reload");
        Require(reloadedBar && std::abs(reloadedBar->Value()-0.3f)<0.001f && reloadedBar->Order()==1,"ProgressBar props lost across save/reload");

        // ZE-81: every UI control is editable in the Inspector (2D object binding).
        {
            auto& editable=editor.CreateUiControl("progressBar"); // CreateUiControl selects it
            auto* pb=editable.GetBehavior<zengine::ui::ProgressBar>();
            Require(pb!=nullptr,"progressBar control missing");
            const HWND inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
            Require(inspector!=nullptr,"Inspector window not found for a 2D selection");

            const auto commit=[&](HWND field,unsigned code){ SendMessageW(inspector,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(field),code),reinterpret_cast<LPARAM>(field)); };

            const HWND valueField=editor.InspectorUiField("value");
            Require(valueField!=nullptr,"Inspector has no ProgressBar 'value' row");
            SetWindowTextW(valueField,L"0.8"); commit(valueField,EN_CHANGE);
            Require(std::abs(pb->Value()-0.8f)<0.001f,"Inspector edit did not write ProgressBar.value");

            const HWND verticalField=editor.InspectorUiField("vertical"); // bool -> combo
            Require(verticalField!=nullptr,"Inspector has no 'vertical' row");
            SendMessageW(verticalField,CB_SETCURSEL,1,0); commit(verticalField,CBN_SELCHANGE);
            Require(pb->Vertical(),"Inspector combo did not set ProgressBar.vertical");

            const HWND fillBlue=editor.InspectorUiField("fill_color",2); // colour component
            Require(fillBlue!=nullptr,"Inspector has no 'fill_color' row");
            SetWindowTextW(fillBlue,L"0.5"); commit(fillBlue,EN_CHANGE);
            Require(std::abs(pb->Fill().z-0.5f)<0.001f,"Inspector did not write a colour component");

            const HWND anchorField=editor.InspectorUiField("anchor"); // enum -> combo
            Require(anchorField!=nullptr,"Inspector has no 'anchor' row");
            const auto centre=SendMessageW(anchorField,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(L"center"));
            Require(centre>=0,"anchor combo is missing 'center'");
            SendMessageW(anchorField,CB_SETCURSEL,centre,0); commit(anchorField,CBN_SELCHANGE);
            Require(pb->GetAnchor()==zengine::ui::Anchor::Center,"Inspector combo did not set the anchor");
            // ZE-101: the combo must have room for its drop-down list (the dropped
            // control rect is far taller than one closed row).
            { RECT cb{}; SendMessageW(anchorField,CB_GETDROPPEDCONTROLRECT,0,reinterpret_cast<LPARAM>(&cb));
              Require((cb.bottom-cb.top)>100,"anchor combo has no room for its drop-down list"); }

            // 2D transform: rotation occupies the Z slot; the unused axes are disabled.
            const HWND rotation=GetDlgItem(inspector,InspectorPanel::FirstTransformField+5); // component 1 (Rotation), Z slot
            const HWND positionZ=GetDlgItem(inspector,InspectorPanel::FirstTransformField+2); // component 0 (Position), Z slot
            Require(rotation && IsWindowEnabled(rotation),"2D rotation field should be editable");
            Require(positionZ && !IsWindowEnabled(positionZ),"2D position Z field should be disabled");
            SetWindowTextW(rotation,L"45"); SendMessageW(inspector,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(rotation),EN_CHANGE),reinterpret_cast<LPARAM>(rotation));
            Require(std::abs(editable.GetTransform().Rotation()-45.0f)<0.01f,"Inspector did not write the 2D rotation");
            editor.DeleteGameObject(editable.Id()); // drop the scratch control before the capture below
        }

        // ZE-97: a texture field (K::Texture) accepts a dropped library asset path.
        {
            auto& tex=editor.CreateUiControl("textureRect"); // selects it
            auto* tr=tex.GetBehavior<zengine::ui::TextureRect>();
            Require(tr!=nullptr,"textureRect control missing");
            const HWND inspector2=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
            for (int i=0;i<40;++i) SendMessageW(inspector2,WM_VSCROLL,MAKEWPARAM(SB_LINEDOWN,0),0);
            const HWND texField=editor.InspectorUiField("texture");
            Require(texField!=nullptr,"Inspector has no textureRect 'texture' row");
            RECT fr{}; GetWindowRect(texField,&fr);
            const POINT mid{(fr.left+fr.right)/2,(fr.top+fr.bottom)/2};
            Require(editor.DropAssetPathOnInspector(mid,"art/logo.png"),"asset-path drop onto the texture field was rejected");
            Require(tr->Texture()=="art/logo.png","texture field did not take the dropped asset path");
            editor.DeleteGameObject(tex.Id());
        }

        editor.Render(); // exercises the viewport UI emit path
        if(capture){ for(int i=0;i<4;++i) editor.Render(); CaptureScreen(window,L"ui-editor-qa.bmp"); }
    }
    CoUninitialize();
    std::cout<<"PASS: UI controls via the scene tree, GameObject2D + behavior, parenting, scene persistence, viewport emit\n";
}
void ViewTabsTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    TestDirectory test;
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const HWND window=editor.Create(SW_HIDE,test.path/"Project");
        editor.InitializeRenderer();
        Require(editor.CurrentViewTab()==EditorShell::ViewTab::Scene,"Editor did not start on the Scene tab");

        // Play switches to the Game tab; Stop returns to Scene.
        const auto script=editor.CreateScriptAsset(); (void)script;
        Require(editor.Play(),"Play failed"); editor.SetPaused(true);
        Require(editor.CurrentViewTab()==EditorShell::ViewTab::Game,"Play did not switch to the Game tab");
        editor.Render();
        if(capture) CaptureScreen(window,L"view-tabs-game-qa.bmp");
        editor.Stop();
        Require(editor.CurrentViewTab()==EditorShell::ViewTab::Scene,"Stop did not return to the Scene tab");

        // Script tab: opening a script switches to it and embeds the editor.
        editor.OpenScript(script);
        Require(editor.CurrentViewTab()==EditorShell::ViewTab::Script,"OpenScript did not switch to the Script tab");
        // The editor window is hidden in tests, so check the WS_VISIBLE style bit, not IsWindowVisible.
        const auto shown=[](HWND h){ return h && (GetWindowLongW(h,GWL_STYLE)&WS_VISIBLE)!=0; };
        const HWND inlineEditor=FindWindowExW(window,nullptr,L"zEngineScriptEditor",nullptr);
        Require(shown(inlineEditor),"Inline script editor is not shown on the Script tab");
        const HWND scriptList=GetDlgItem(window,EditorShell::ScriptListControl);
        const HWND functionList=GetDlgItem(window,EditorShell::FunctionListControl);
        Require(shown(scriptList) && shown(functionList),"Script tab side lists missing");
        Require(SendMessageW(scriptList,LB_GETCOUNT,0,0)>=1,"Script list did not list the project script");
        // Give the editor a couple of functions and confirm the function list picks them up.
        SetWindowTextW(GetDlgItem(inlineEditor,ScriptEditor::SourceControl),
            L"class NewBehavior : gameObject { func start(){} func update(float d){} func draw(){} }");
        SendMessageW(inlineEditor,WM_COMMAND,ScriptEditor::SaveCommand,0);
        Require(SendMessageW(functionList,LB_GETCOUNT,0,0)==3,"Function list did not enumerate the script functions");
        // Jumping to a function moves the caret.
        SendMessageW(functionList,LB_SETCURSEL,2,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(EditorShell::FunctionListControl,LBN_SELCHANGE),reinterpret_cast<LPARAM>(functionList));
        DWORD selStart=0; SendMessageW(GetDlgItem(inlineEditor,ScriptEditor::SourceControl),EM_GETSEL,reinterpret_cast<WPARAM>(&selStart),0);
        Require(selStart>0,"Clicking a function did not move the caret");
        editor.Render();
        if(capture) CaptureScreen(window,L"view-tabs-script-qa.bmp");

        // Switching back to Scene hides the script widgets and re-shows the viewport.
        editor.SetViewTab(EditorShell::ViewTab::Scene);
        Require(!shown(scriptList),"Script list stayed visible on the Scene tab");
    }
    CoUninitialize();
    std::cout<<"PASS: view panel tabs - Scene/Game auto-switch on Play/Stop, inline Script tab with script + function lists\n";
}
void CollisionBitsTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
    TestDirectory test;
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const HWND window = editor.Create(SW_HIDE, test.path / "Project");
        editor.InitializeRenderer();
        auto& body = editor.CreateGameObject(EditorShell::ObjectPreset::RigidBody);
        auto* rb = body.GetBehavior<zengine::physics::RigidBody>();
        Require(rb != nullptr, "RigidBody preset did not attach a body");
        Require(editor.SelectedGameObject() && editor.SelectedGameObject()->Id() == body.Id(), "New physics object was not selected");
        const HWND inspector = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
        Require(inspector != nullptr, "Inspector child missing");
        // The layer/mask rows are grids of toggle buttons, not edit fields.
        const auto layerBit = [&](int bit) { return GetDlgItem(inspector, InspectorPanel::FirstBehaviorBit + bit); };
        const auto maskBit = [&](int bit) { return GetDlgItem(inspector, InspectorPanel::FirstBehaviorBit + InspectorPanel::CollisionBits + bit); };
        Require(layerBit(0) && layerBit(InspectorPanel::CollisionBits - 1) && maskBit(0), "Collision toggle buttons were not created");
        const auto click = [&](HWND button, int id, bool check) {
            SendMessageW(button, BM_SETCHECK, check ? BST_CHECKED : BST_UNCHECKED, 0); // BS_AUTOCHECKBOX would flip this itself for a real click.
            SendMessageW(inspector, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
        };
        rb->SetLayer(0); rb->SetMask(0);
        click(layerBit(2), InspectorPanel::FirstBehaviorBit + 2, true);
        Require((rb->Layer() & (1u << 2)) != 0 && rb->Layer() == (1u << 2), "Toggling a layer button did not set exactly that bit");
        click(maskBit(5), InspectorPanel::FirstBehaviorBit + InspectorPanel::CollisionBits + 5, true);
        Require((rb->Mask() & (1u << 5)) != 0, "Toggling a mask button did not set the bit");
        Require(SendMessageW(layerBit(2), BM_GETCHECK, 0, 0) == BST_CHECKED, "Layer button did not stay pressed after toggling on");
        // Bits above the visible grid must survive an edit through the buttons.
        rb->SetLayer(0xFF00u | (1u << 2));
        click(layerBit(2), InspectorPanel::FirstBehaviorBit + 2, false);
        Require(rb->Layer() == 0xFF00u, "Toggling a low bit off disturbed the preserved high bits");
        // Re-selecting the object must sync the buttons to the stored bits.
        rb->SetLayer((1u << 1) | (1u << 9));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 140));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 167));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 167));
        if (editor.SelectedGameObject() && editor.SelectedGameObject()->Id() == body.Id())
        {
            const HWND inspector2 = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
            Require(SendMessageW(GetDlgItem(inspector2, InspectorPanel::FirstBehaviorBit + 1), BM_GETCHECK, 0, 0) == BST_CHECKED &&
                    SendMessageW(GetDlgItem(inspector2, InspectorPanel::FirstBehaviorBit + 9), BM_GETCHECK, 0, 0) == BST_CHECKED &&
                    SendMessageW(GetDlgItem(inspector2, InspectorPanel::FirstBehaviorBit + 0), BM_GETCHECK, 0, 0) == BST_UNCHECKED,
                    "Re-selecting did not sync collision buttons to stored bits");
        }
        // ZE-39: the per-behavior collapse toggle sits on the RIGHT of the header and hides
        // the behavior's rows when collapsed (leaving only the name + expand button).
        {
            const HWND ins = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
            // Preset order: Collider is behavior 0, RigidBody is behavior 1 (it owns the bit grid).
            const HWND toggle = GetDlgItem(ins, InspectorPanel::FirstBehaviorToggle + 1);
            Require(toggle != nullptr, "RigidBody collapse toggle missing");
            RECT tr{}, ir{}; GetWindowRect(toggle, &tr); GetClientRect(ins, &ir);
            POINT tl{ tr.left, tr.top }; ScreenToClient(ins, &tl);
            Require(tl.x > ir.right / 2, "Collapse toggle is not on the right side of the behavior header");
            const auto shown = [](HWND h) { return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; };
            Require(shown(GetDlgItem(ins, InspectorPanel::FirstBehaviorBit + 0)), "Collision bit row hidden before collapse");
            SendMessageW(ins, WM_COMMAND, MAKEWPARAM(InspectorPanel::FirstBehaviorToggle + 1, BN_CLICKED), reinterpret_cast<LPARAM>(toggle));
            Require(!shown(GetDlgItem(ins, InspectorPanel::FirstBehaviorBit + 0)), "Collapsing the behavior did not hide its rows");
            Require(shown(toggle), "Collapse toggle vanished when the behavior collapsed");
            SendMessageW(ins, WM_COMMAND, MAKEWPARAM(InspectorPanel::FirstBehaviorToggle + 1, BN_CLICKED), reinterpret_cast<LPARAM>(toggle));
            Require(shown(GetDlgItem(ins, InspectorPanel::FirstBehaviorBit + 0)), "Expanding the behavior did not restore its rows");
        }
        if (capture) {
            auto* rebound = editor.SelectedGameObject() ? const_cast<zengine::GameObject*>(editor.SelectedGameObject())->GetBehavior<zengine::physics::RigidBody>() : rb;
            rebound->SetLayer((1u<<0)|(1u<<3)); rebound->SetMask(0x7);
            const HWND ins = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
            for (int b : {0,3}) click(GetDlgItem(ins, InspectorPanel::FirstBehaviorBit + b), InspectorPanel::FirstBehaviorBit + b, true);
            for (int b : {0,1,2}) click(GetDlgItem(ins, InspectorPanel::FirstBehaviorBit + InspectorPanel::CollisionBits + b), InspectorPanel::FirstBehaviorBit + InspectorPanel::CollisionBits + b, true);
            for (int i=0;i<22;++i) SendMessageW(ins, WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN,0), 0);
            editor.Render();
            CaptureScreen(window, L"collision-bits-qa.bmp");
        }
    }
    CoUninitialize();
    std::cout << "PASS: collision layer/mask toggle buttons set/clear bits and preserve unseen high bits\n";
}
void ArrayInspectorTests(bool capture)
{
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
    TestDirectory test;
    {
        EditorShell editor(GetModuleHandleW(nullptr));
        const HWND window = editor.Create(SW_HIDE, test.path / "Project");
        editor.InitializeRenderer();
        const auto cube = editor.SelectedGameObject()->Id();
        // Row 2 in the scene tree: a RigidBody object to drag into an array slot.
        auto& rig = editor.CreateGameObject(EditorShell::ObjectPreset::RigidBody);
        const auto rigId = rig.Id();
        // Author a script whose exported arrays carry an initializer and an empty list.
        const auto path = editor.CreateScriptAsset();
        editor.OpenScript(path);
        const HWND scriptWindow = FindWindowExW(window, nullptr, L"zEngineScriptEditor", nullptr);
        Require(scriptWindow != nullptr, "Inline script editor did not open");
        SetWindowTextW(GetDlgItem(scriptWindow, ScriptEditor::SourceControl),
            LR"(class NewBehavior : gameObject {
                export array numbers=[10,20];
                export array refs;
                func start() {} func update(float dt) {}
            })");
        SendMessageW(scriptWindow, WM_COMMAND, ScriptEditor::SaveCommand, 0);
        // Re-select the cube and attach.
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 140));
        Require(editor.SelectedGameObject()->Id() == cube, "Could not re-select the cube");
        Require(editor.AttachScript(cube, path), "Attach script with arrays failed");
        const HWND inspector = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
        Require(inspector != nullptr, "Inspector child missing");
        const auto field = [&](int row) { return GetDlgItem(inspector, InspectorPanel::FirstBehaviorField + row); };
        const auto arrayButton = [&](int i) { return GetDlgItem(inspector, InspectorPanel::FirstBehaviorBit + i); };
        const auto text = [&](HWND h) { wchar_t b[64]{}; GetWindowTextW(h, b, 64); return std::wstring(b); };
        // Mesh header/priority/material (ZE-65), script header/priority, numbers[header], numbers[0], numbers[1], refs[header]
        Require(field(6) && field(7), "Array initializer did not surface element edit fields");
        Require(text(field(6)) == L"10" && text(field(7)) == L"20", "Array element fields show the wrong initializer values");
        Require(arrayButton(0) && arrayButton(1) && arrayButton(2) && arrayButton(3), "Array add/remove buttons were not created");
        // Edit an element.
        SetWindowTextW(field(7), L"99");
        SendMessageW(inspector, WM_COMMAND, MAKEWPARAM(InspectorPanel::FirstBehaviorField + 7, EN_KILLFOCUS), reinterpret_cast<LPARAM>(field(7)));
        // Remove numbers[0] via its "x" button (id FirstBehaviorBit+1).
        SendMessageW(inspector, WM_COMMAND, MAKEWPARAM(InspectorPanel::FirstBehaviorBit + 1, BN_CLICKED), reinterpret_cast<LPARAM>(arrayButton(1)));
        const HWND inspector2 = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
        const auto field2 = [&](int row) { return GetDlgItem(inspector2, InspectorPanel::FirstBehaviorField + row); };
        Require(field2(6) && !field2(7), "Removing an array element did not drop the trailing row");
        { wchar_t b[64]{}; GetWindowTextW(field2(6), b, 64); Require(std::wstring(b) == L"99", "Array element edit or removal kept the wrong value"); }
        // Dropping a scene object onto the "refs" array (empty) appends an auto-typed slot.
        // The InspectorPanel geometric hit-test routes an over-row drop to the array.
        for (int i = 0; i < 24; ++i) SendMessageW(inspector2, WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN, 0), 0);
        const HWND refsAdd = GetDlgItem(inspector2, InspectorPanel::FirstBehaviorBit + 2);
        Require(refsAdd != nullptr, "refs array add button missing");
        RECT rb{}; GetWindowRect(refsAdd, &rb);
        const POINT dropScreen{ (rb.left + rb.right) / 2, (rb.top + rb.bottom) / 2 };
        Require(editor.DropObjectOnInspector(dropScreen, rigId), "Drop onto the refs array row was not accepted");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 140));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 140));
        const HWND inspector4 = FindWindowExW(window, nullptr, L"zEngineInspector", nullptr);
        bool foundRefElement = false;
        for (int row = 0; row < 24; ++row)
        {
            const HWND h = GetDlgItem(inspector4, InspectorPanel::FirstBehaviorField + row);
            if (!h) continue;
            wchar_t b[128]{}; GetWindowTextW(h, b, 128);
            if (std::wstring(b).find(L"RigidBody") != std::wstring::npos) foundRefElement = true;
        }
        Require(foundRefElement, "Dragging a scene object onto an array did not append an auto-typed reference element");
        if (capture) { editor.Render(); CaptureScreen(window, L"array-inspector-qa.bmp"); }
    }
    CoUninitialize();
    std::cout << "PASS: exported script arrays render, add/remove/edit elements, and accept dragged auto-typed references\n";
}
int main(int argc, char** argv)
{
    try
    {
        if (argc > 1 && std::string(argv[1]) == "--gpu") GpuTests();
        else if (argc > 1 && std::string(argv[1]) == "--arrays") ArrayInspectorTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if(argc>1 && std::string(argv[1])=="--folders")FolderTests(argc>2);
        else if(argc>1 && std::string(argv[1])=="--hierarchy")HierarchyTests(argc>2);
        else if(argc>1 && std::string(argv[1])=="--text")TextInspectorTests(argc>2);
        else if(argc>1 && std::string(argv[1])=="--build")BuildTests();
        else if(argc>1 && std::string(argv[1])=="--editor-build")EditorBuildTests();
        else if (argc > 1 && std::string(argv[1]) == "--editor") EditorTests();
        else if (argc > 1 && std::string(argv[1]) == "--objects") GameObjectEditorTests();
        else if (argc > 1 && std::string(argv[1]) == "--collision-bits") CollisionBitsTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--picker") ObjectPickerTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--view-tabs") ViewTabsTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--camera") CameraTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--ui") UiEditorTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--meshes") MeshBehaviorTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--scripts") ScriptIntegrationEditorTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--scenes") SceneEditorTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--projects") ProjectTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--gizmos") GizmoTests(argc > 2 && std::string(argv[2]) == "--capture");
        else if (argc > 1 && std::string(argv[1]) == "--prefabs") PrefabTests();
        else if (argc > 1 && (std::string(argv[1]) == "--project-recovery" || std::string(argv[1]) == "--project-dialog" || std::string(argv[1]) == "--project-missing")) ProjectStartupTests(argv[1],argc > 2 && std::string(argv[2]) == "--capture");
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
