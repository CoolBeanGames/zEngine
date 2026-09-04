#include "EditorShell.h"
#include "EditorStyle.h"
#include "input/InputMapEditor.h"
#include "MaterialEditor.h"
#include "DataSheetEditor.h"

#include "Renderer.h"
#include "FbxImporter.h"
#include "InspectorPanel.h"
#include "ScriptAssets.h"
#include "ScriptEditor.h"
#include "SceneAssets.h"
#include "PrefabAssets.h"
#include "RenderTransform.h"
#include "TransformOverrides.h"
#include "AssetLibrary.h"
#include "core/ScriptBehavior.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "core/Light.h"
#include "core/Environment.h"
#include "core/Decal.h"
#include "SceneLights.h"
#include "LightmapAssets.h"
#include "DataAssets.h"
#include "DataSheetAssets.h"
#include "zscript/Script.h"
#include "CubeModel.h"
#include "ui/UiSerialize.h"
#include "ui/VideoClip.h"
#include <commdlg.h>
#include <commctrl.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <chrono>

namespace
{
    std::string Utf8Text(const std::wstring& text)
    {
        const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(count, ' ');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
        return result;
    }
    std::wstring WideText(const std::string& text)
    {
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(count, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
        return result;
    }
    const wchar_t* KindLabel(assetLibrary::Kind kind)
    {
        switch (kind)
        {
        case assetLibrary::Kind::Script: return L"Script";
        case assetLibrary::Kind::Prefab: return L"Prefab";
        case assetLibrary::Kind::Scene: return L"Scene";
        case assetLibrary::Kind::Input: return L"Input Map";
        case assetLibrary::Kind::Image: return L"Image";
        case assetLibrary::Kind::Model: return L"Model";
        case assetLibrary::Kind::Shader: return L"Material Shader";
        case assetLibrary::Kind::Material: return L"Material";
        case assetLibrary::Kind::Audio: return L"Audio";
        case assetLibrary::Kind::Data: return L"Data Object";
        case assetLibrary::Kind::DataSheet: return L"Data Sheet";
        case assetLibrary::Kind::Folder: return L"Folder";
        default: return L"File";
        }
    }
    void RestoreBehaviorState(zengine::Behavior& behavior,const zengine::scenes::BehaviorData& saved)
    {
        behavior.SetPriority(saved.priority);behavior.SetEnabled(saved.enabled);
        if(auto* collider=dynamic_cast<zengine::physics::Collider*>(&behavior))
        {collider->SetShape(saved.shape);collider->SetOffset(saved.colliderOffset);collider->SetSize(saved.colliderSize);}
        if(auto* body=dynamic_cast<zengine::physics::Body*>(&behavior))
        {
            body->SetLayer(saved.layer);body->SetMask(saved.mask);body->SetFriction(saved.friction);body->SetBounciness(saved.bounciness);
            if(auto* rigid=dynamic_cast<zengine::physics::RigidBody*>(body)){rigid->SetMass(saved.mass);rigid->SetGravityScale(saved.gravityScale);}
            if(auto* moving=dynamic_cast<zengine::physics::MovingBody*>(body))
            {moving->SetVelocity(saved.velocity);moving->SetAngularVelocity(saved.angularVelocity);moving->SetConstantForce(saved.constantForce);moving->SetConstantTorque(saved.constantTorque);}
        }
    }
    constexpr wchar_t EditorWindowClass[] = L"zEngineEditorWindow";
    constexpr wchar_t ViewportWindowClass[] = L"zEngineViewportWindow";

    constexpr int OptionsBarHeight = 32;
    constexpr int StatusBarHeight = 27;
    constexpr int PanelHeaderHeight = 30;
    constexpr int SplitterSize = 5;
    constexpr int MinimumSideWidth = 150;
    constexpr int MinimumViewportWidth = 260;
    constexpr int MinimumMediaHeight = 120;
    constexpr int MinimumUpperHeight = 180;

    constexpr COLORREF EditorBackground = RGB(32, 34, 38);
    constexpr COLORREF PanelBackground = RGB(40, 42, 47);
    constexpr COLORREF HeaderBackground = RGB(47, 49, 55);
    constexpr COLORREF BorderColor = editorStyle::Border;
    constexpr COLORREF TextColor = editorStyle::Text;
    constexpr COLORREF MutedTextColor = RGB(145, 149, 158);
    constexpr COLORREF FieldColor = editorStyle::Face;
    constexpr COLORREF SelectionColor = RGB(54, 83, 119);

    void FillRectangle(HDC deviceContext, const RECT& rectangle, const COLORREF color)
    {
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(deviceContext, &rectangle, brush);
        DeleteObject(brush);
    }

    void DrawBorder(HDC deviceContext, const RECT& rectangle, const COLORREF color)
    {
        const HPEN pen = CreatePen(PS_SOLID, 1, color);
        const HGDIOBJ previousPen = SelectObject(deviceContext, pen);
        const HGDIOBJ previousBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
        Rectangle(deviceContext, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(deviceContext, previousBrush);
        SelectObject(deviceContext, previousPen);
        DeleteObject(pen);
    }

    void DrawTextLabel(
        HDC deviceContext,
        const std::wstring_view text,
        RECT rectangle,
        const COLORREF color,
        const UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER)
    {
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, color);
        DrawTextW(deviceContext, text.data(), static_cast<int>(text.size()), &rectangle, format);
    }

    void DrawPanel(HDC deviceContext, const RECT& rectangle, const std::wstring_view title)
    {
        FillRectangle(deviceContext, rectangle, PanelBackground);
        RECT header = rectangle;
        header.bottom = std::min(header.bottom, header.top + PanelHeaderHeight);
        FillRectangle(deviceContext, header, HeaderBackground);
        DrawBorder(deviceContext, rectangle, BorderColor);
        header.left += 11;
        DrawTextLabel(deviceContext, title, header, TextColor);
    }

}

EditorShell::EditorShell(const HINSTANCE instance)
    : instance_(instance)
{
    // ZE-82 follow-up: the app icon (IDI_APP_ICON == 1, from src/zEngine.rc).
    const HICON appIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    const HICON appIconSmall = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    WNDCLASSEXW editorClass{};
    editorClass.cbSize = sizeof(editorClass);
    editorClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    editorClass.lpfnWndProc = WindowProcedure;
    editorClass.hInstance = instance_;
    editorClass.hIcon = appIcon;
    editorClass.hIconSm = appIconSmall;
    editorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    editorClass.hbrBackground = nullptr;
    editorClass.lpszClassName = EditorWindowClass;
    if (!RegisterClassExW(&editorClass))
    {
        throw std::runtime_error("Could not register the editor window class.");
    }

    WNDCLASSEXW viewportClass{};
    viewportClass.cbSize = sizeof(viewportClass);
    viewportClass.style = CS_OWNDC;
    viewportClass.lpfnWndProc = ViewportProcedure;
    viewportClass.hInstance = instance_;
    viewportClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    viewportClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    viewportClass.lpszClassName = ViewportWindowClass;
    if (!RegisterClassExW(&viewportClass))
    {
        UnregisterClassW(EditorWindowClass, instance_);
        throw std::runtime_error("Could not register the viewport window class.");
    }

    uiFont_ = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    headerFont_ = CreateFontW(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

EditorShell::~EditorShell()
{
    inlineEditor_.reset();
    scriptEditors_.clear();
    inspectorPanel_.reset();
    renderer_.reset();
    if (window_ && IsWindow(window_))
    {
        DestroyWindow(window_);
    }
    if (uiFont_)
    {
        DeleteObject(uiFont_);
    }
    if (headerFont_)
    {
        DeleteObject(headerFont_);
    }
    UnregisterClassW(ViewportWindowClass, instance_);
    UnregisterClassW(EditorWindowClass, instance_);
}

HWND EditorShell::Create(const int showCommand, const std::filesystem::path& projectDirectory)
{
    RECT windowArea{0, 0, 1440, 900};
    AdjustWindowRectEx(&windowArea, WS_OVERLAPPEDWINDOW, FALSE, 0);
    window_ = CreateWindowExW(
        0, EditorWindowClass, L"zEngine Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowArea.right - windowArea.left, windowArea.bottom - windowArea.top,
        nullptr, nullptr, instance_, this);
    if (!window_)
    {
        throw std::runtime_error("Could not create the zEngine editor window.");
    }

    CreateViewport();
    inspectorPanel_ = std::make_unique<InspectorPanel>();
    inspectorPanel_->Create(window_, instance_, uiFont_, [this]() { OnObjectChanged(); });
    inspectorPanel_->SetScriptHost(&scriptHost_);
    scriptHost_.SetObjectStore(&objects_);
    // ZE-129: data-object fields read/write their bound .zdata under the project's Assets.
    scriptHost_.SetDataAssetIO(
        [this](std::string_view rel)->std::string{ const std::filesystem::path rp(rel);
            if(zengine::datasheet::IsSheet(rp)) return zengine::datasheet::Encode(zengine::datasheet::Load(zengine::datasheet::Resolve(assetsDirectory_, rp)));
            return zengine::dataobj::Encode(zengine::dataobj::Load(zengine::dataobj::Resolve(assetsDirectory_, rp))); },
        [this](std::string_view rel, std::string_view text){ const std::filesystem::path wp(rel);
            if(zengine::datasheet::IsSheet(wp)) zengine::datasheet::Save(assetsDirectory_, wp, zengine::datasheet::Decode(text));
            else zengine::dataobj::Save(assetsDirectory_, wp, zengine::dataobj::Decode(text)); });
    ConfigureScriptOutput();
    inspectorPanel_->SetAddScriptHandler([this]() {
        try { ChooseScript(); }
        catch (const std::exception& error) { status_ = L"Cannot attach script: " + WideText(error.what()); InvalidateRect(window_, &statusBar_, FALSE); }
    });
    inspectorPanel_->SetScriptMenu(
        [this]() { return ProjectScriptPaths(); },
        [this](const std::wstring& relative) {
            try { AttachScript(selectedObject_, assetsDirectory_ / std::filesystem::path(relative)); }
            catch (const std::exception& error) { status_ = L"Cannot attach script: " + WideText(error.what()); InvalidateRect(window_, &statusBar_, FALSE); }
        });
    inspectorPanel_->SetMeshHandler([this](InspectorPanel::MeshAction action) {
        try
        {
            if (action == InspectorPanel::MeshAction::Add) AddMeshRenderer(selectedObject_);
            else if (action == InspectorPanel::MeshAction::Choose) ChooseModel();
            else if (action == InspectorPanel::MeshAction::Cube) AssignCube(selectedObject_);
            else ClearMesh(selectedObject_);
        }
        catch (const std::exception& error) { status_ = L"Mesh operation failed: " + WideText(error.what()); InvalidateRect(window_, &statusBar_, FALSE); }
    });
    inspectorPanel_->SetPrefabHandler([this](const std::string& current) {
        try{return ChoosePrefabReference(current);}
        catch(const std::exception& error){status_=L"Cannot assign prefab: "+WideText(error.what());InvalidateRect(window_,&statusBar_,FALSE);return std::optional<std::string>{};}
    });
    inspectorPanel_->SetObjectPicker([this](const std::string& referenceType, zengine::GameObjectId current, RECT anchorScreen) {
        try{return PickSceneObject(referenceType,current,anchorScreen);}
        catch(const std::exception& error){status_=L"Cannot pick object: "+WideText(error.what());InvalidateRect(window_,&statusBar_,FALSE);return std::optional<zengine::GameObjectId>{};}
    });
    inspectorPanel_->SetMaterialHandlers(
        [this](const std::string& asset){ return ResolveMaterialEffective(asset); },
        [this](const std::string& asset, zengine::materials::Value value){
            try { ApplyMaterialValue(asset, std::move(value)); }
            catch(const std::exception& error){ status_=L"Cannot save material: "+WideText(error.what()); InvalidateRect(window_,&statusBar_,FALSE); }
        });
    // Explicit directories support embedding/legacy projects; normal startup uses the recent-project config.
    if (!projectDirectory.empty())
    {
        project_=zengine::projects::InitializeDirectory(projectDirectory);
        assetsDirectory_=zengine::projects::Assets(*project_);
        sceneOpen_=true;
        auto& cube=objects_.Create("Color Cube");
        cube.AddBehavior<zengine::MeshRenderer>(zengine::MeshRenderer::CubeAsset);
        SelectGameObject(cube.Id());
    }
    RefreshAssets();
    DragAcceptFiles(window_, TRUE);
    sceneBaseline_=zengine::scenes::Encode(CaptureDocument());
    UpdateSceneTitle();
    RECT client{};
    GetClientRect(window_, &client);
    Layout(static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom));
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return window_;
}

void EditorShell::CreateViewport()
{
    viewportWindow_ = CreateWindowExW(
        0, ViewportWindowClass, L"Scene Viewport",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 1, 1, window_, nullptr, instance_, this);
    if (!viewportWindow_)
    {
        throw std::runtime_error("Could not create the Direct3D viewport window.");
    }
}

void EditorShell::InitializeRenderer()
{
    if (!viewportWindow_ || requestedViewportWidth_ == 0 || requestedViewportHeight_ == 0)
    {
        throw std::runtime_error("The editor viewport has no drawable area.");
    }
    renderer_ = std::make_unique<Renderer>();
    renderer_->Initialize(viewportWindow_, requestedViewportWidth_, requestedViewportHeight_);
    for (std::size_t i = 0; i < objects_.Size(); ++i)
        if (const auto* mesh = objects_.At(i).GetBehavior<zengine::MeshRenderer>(); mesh && mesh->Asset() == zengine::MeshRenderer::CubeAsset)
            meshBindings_[objects_.At(i).Id()] = {mesh->Asset(), renderer_->Cube()};
    rendererWidth_ = requestedViewportWidth_;
    rendererHeight_ = requestedViewportHeight_;
}

void EditorShell::Render()
{
    const auto now=std::chrono::steady_clock::now();
    const double elapsed=std::min(0.1,std::chrono::duration<double>(now-lastTick_).count());
    lastTick_=now;
    ++framesSinceFps_;const auto fpsElapsed=std::chrono::duration<double>(now-fpsSample_).count();if(fpsElapsed>=.5){currentFps_=static_cast<unsigned>(std::lround(framesSinceFps_/fpsElapsed));framesSinceFps_=0;fpsSample_=now;InvalidateRect(window_,&optionsBar_,FALSE);}
    CameraTick(static_cast<float>(elapsed));
    PollBuild();
    PollAssetWork();
    for (auto it = meshCache_.begin(); it != meshCache_.end();)
        if (it->second.expired()) it = meshCache_.erase(it); else ++it;
    // While a material editor is open its file may be saved to disk from under us;
    // drop its cached handle each frame so edits show live on assigned meshes.
    if (materialEditor_ && IsWindowVisible(materialEditor_->Window()))
    {
        const auto rel = std::filesystem::relative(materialEditor_->File(), assetsDirectory_).generic_u8string();
        materialCache_.erase(std::string(reinterpret_cast<const char*>(rel.data()), rel.size()));
    }
    if (!renderer_ || requestedViewportWidth_ == 0 || requestedViewportHeight_ == 0)
    {
        return;
    }

    if (requestedViewportWidth_ != rendererWidth_ || requestedViewportHeight_ != rendererHeight_)
    {
        if (requestedViewportWidth_ > 0 && requestedViewportHeight_ > 0)
        {
            renderer_->Resize(requestedViewportWidth_, requestedViewportHeight_);
            rendererWidth_ = requestedViewportWidth_;
            rendererHeight_ = requestedViewportHeight_;
        }
    }
    if (Playing())
    {
        if (!paused_)
        {
            tickAccumulator_+=elapsed;
            while (tickAccumulator_>=1.0/60.0) { TickInput(); scriptHost_.Tick(objects_,1.0f/60.0f);scriptHost_.PhysicsTick(objects_,1.0f/60.0f);physicsWorld_->Step(objects_,1.0f/60.0f);scriptHost_.DispatchPhysicsEvents(physicsWorld_->DrainEvents());TickPreviewAudio(1.0f/60.0f);tickAccumulator_-=1.0/60.0; }
        }
        if (!paused_ || stepDraw_)
            scriptHost_.Draw(objects_,[&](zengine::GameObjectId id) {
                const auto* object=objects_.Find(id);
                const auto* mesh=object?object->GetBehavior<zengine::MeshRenderer>():nullptr;
                const auto bound=meshBindings_.find(id);
                return mesh && mesh->Enabled() && !mesh->Asset().empty() && bound!=meshBindings_.end() && bound->second.mesh && bound->second.asset==mesh->Asset();
            });
        stepDraw_=false;
        if (GetTickCount64()-lastInspectorRefresh_>=100)
        { inspectorPanel_->RefreshLiveValues(); ReportScriptErrors(); InvalidateRect(window_,&sceneBrowser_,FALSE); lastInspectorRefresh_=GetTickCount64(); }
    }
    auto frame=BuildSceneFrame();if(showFps_)frame.fps=currentFps_;
    {
        zengine::ui::UiContext uiContext;
        uiContext.measureText=[&](std::string_view s,float h){return renderer_->MeasureText(s,h);};
        if(!uiViewportAssets_) uiViewportAssets_.emplace(*renderer_,assetsDirectory_);
        uiViewportAssets_->Bind(uiContext);
        uiViewport_.Build(objects_,renderer_->ViewportSize(),uiContext);
        if (Playing()) // ZE-96: route accumulated viewport input into the live UI
        {
            const zengine::Vec2 cursor{
                static_cast<float>(uiInput_.cursor.x - viewportContent_.left),
                static_cast<float>(uiInput_.cursor.y - viewportContent_.top)};
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const auto clicks = uiViewport_.Interact(cursor, uiInput_.primary, uiInput_.typed, uiInput_.wheel, shift);
            uiInput_.typed.clear(); uiInput_.wheel = 0;
            const auto emit = [&](const auto& ids, const char* signal) { for (auto id : ids) scriptHost_.EmitSignal(id, signal); };
            emit(uiViewport_.Presses(), "pressed");
            emit(uiViewport_.Releases(), "released");
            emit(uiViewport_.Entered(), "mouse_entered");
            emit(uiViewport_.Exited(), "mouse_exited");
            emit(uiViewport_.Submissions(), "submitted");
            if (uiViewport_.FocusEntered()) scriptHost_.EmitSignal(uiViewport_.FocusEntered(), "focus_entered");
            if (uiViewport_.FocusExited()) scriptHost_.EmitSignal(uiViewport_.FocusExited(), "focus_exited");
            emit(clicks, "clicked");
        }
        uiViewport_.Emit(frame.sprites,frame.texts);
    }
    renderer_->Render(frame);
}
bool EditorShell::RouteUiViewportPress(POINT clientPoint)
{
    if (!Playing() || !renderer_ || !PtInRect(&viewportContent_, clientPoint)) return false;
    const zengine::Vec2 vp{static_cast<float>(clientPoint.x - viewportContent_.left),
                           static_cast<float>(clientPoint.y - viewportContent_.top)};
    if (uiViewport_.HitTest(vp) == 0) return false;
    uiInput_.primary = true; // the release + click are applied in Render()
    return true;
}

bool EditorShell::PrepareScripts()
{
    bool valid=true;
    for (std::size_t i=0;i<objects_.Size();++i)
        for (std::size_t j=0;j<objects_.At(i).BehaviorCount();++j)
            if (auto* behavior=dynamic_cast<zengine::ScriptBehavior*>(&objects_.At(i).BehaviorAt(j)))
            {
                try
                {
                    const auto file=zengine::scripts::Resolve(assetsDirectory_,std::filesystem::path(WideText(behavior->Asset())));
                    if (!scriptHost_.Prepare(*behavior,zengine::scripts::Load(file),Utf8Text(file.stem().wstring())))
                    { valid=false; status_=WideText(scriptHost_.Error(*behavior)); }
                }
                catch (const std::exception& e) { valid=false; status_=WideText(e.what()); }
            }
    inspectorPanel_->RefreshBehaviors();
    if (!sceneBaseline_.empty())
    {
        sceneDirty_=zengine::scenes::Encode(CaptureDocument())!=sceneBaseline_;
        UpdateSceneTitle();
    }
    InvalidateRect(window_,nullptr,FALSE);
    return valid;
}
bool EditorShell::Play()
{
    if (!editingPrefab_.empty()) { status_=L"Close the prefab editing view before playing the scene."; InvalidateRect(window_,nullptr,FALSE); return false; }
    EndGizmoDrag(false);
    if (!sceneOpen_) { status_=L"Create or open a scene before Play."; InvalidateRect(window_,nullptr,FALSE); return false; }
    if (Playing()) return true;
    if(inputEditor_ && inputEditor_->Dirty()){status_=L"Save or reload Input Map edits before Play.";InvalidateRect(window_,nullptr,FALSE);return false;}
    try { zengine::input::Ensure(assetsDirectory_);inputSystem_.Configure(zengine::input::Decode(zengine::input::Load(assetsDirectory_))); }
    catch(const std::exception& e){status_=WideText(e.what());InvalidateRect(window_,nullptr,FALSE);return false;}
    zengine::script::InputFrame input;for(const auto& [name,state]:inputSystem_.Current())input.emplace(name,zengine::script::InputState{});scriptHost_.SetInput(std::move(input));
    if (PendingModels()) { status_=L"Wait for this scene's models to finish loading before Play."; InvalidateRect(window_,nullptr,FALSE); return false; }
    for (const auto& editor:scriptEditors_) if (editor->Dirty())
    { status_=L"Save your script edits (Ctrl+S) before Play."; InvalidateRect(window_,nullptr,FALSE); return false; }
    if (inlineEditor_ && inlineEditor_->Dirty())
    { status_=L"Save your script edits (Ctrl+S) before Play."; InvalidateRect(window_,nullptr,FALSE); return false; }
    SetFocus(window_); // Finish Inspector edits before snapshotting values/transforms.
    if (!PrepareScripts()) { ReportScriptErrors(); return false; }
    auto authored=zengine::scenes::Capture(objects_,scriptHost_);playObjects_.clear();for(std::size_t i=0;i<objects_.Size();++i)playObjects_.insert(objects_.At(i).Id());
    physicsWorld_=std::make_unique<zengine::physics::World>();try{physicsWorld_->Build(objects_);}catch(const std::exception& e){physicsWorld_.reset();status_=L"Physics: "+WideText(e.what());InvalidateRect(window_,nullptr,FALSE);return false;}
    scriptHost_.SetPrefabSpawner([this](std::string_view asset){return SpawnPrefab(asset);});
    audioPreview_=std::make_unique<zengine::audio::AudioSystem>();
    audioPreview_->SetClipLoader([this](const std::string& p){return zengine::audio::LoadFile(assetLibrary::Resolve(assetsDirectory_,std::filesystem::u8path(p)));});
    // Scene.load in the editor's Play preview reports rather than switching - the
    // live scene document is what is being previewed. It applies in a built game.
    scriptHost_.SetSceneName(sceneOpen_ ? Utf8Text(SceneName()) : std::string{});
    scriptHost_.SetSceneLoader([this](std::string_view name){
        status_=L"Scene.load(\""+WideText(std::string(name))+L"\") switches scenes only in a built game";
        InvalidateRect(window_,&statusBar_,FALSE);
    });
    // The console docks into the bottom of the Game tab, not a separate window.
    consoleWindow_=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL,0,0,1,1,window_,nullptr,instance_,nullptr);
    if(consoleWindow_)SendMessageW(consoleWindow_,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont_),TRUE);
    if (!scriptHost_.Play(objects_,physicsWorld_.get())) { if(consoleWindow_){DestroyWindow(consoleWindow_);consoleWindow_=nullptr;} physicsWorld_.reset();ReportScriptErrors(); return false; }
    playScene_=std::move(authored);
    paused_=false; stepDraw_=false; tickAccumulator_=0; lastTick_=std::chrono::steady_clock::now();
    status_=L"Playing - Stop restores transforms and discards runtime variable changes";
    inspectorPanel_->RefreshBehaviors(); inspectorPanel_->RefreshLiveValues(); ReportScriptErrors();
    SetViewTab(ViewTab::Game); // play inside the view panel, not a new window
    InvalidateRect(window_,nullptr,FALSE);
    return true;
}
void EditorShell::TickPreviewAudio(float delta)
{
    if (!audioPreview_) return;
    zengine::Vec3 listener{};
    for (std::size_t i = 0; i < objects_.Size(); ++i)
        if (objects_.At(i).GetBehavior<zengine::Camera>())
        { if (auto* g = zengine::As3D(&objects_.At(i))) listener = g->GetTransform().Position(); break; }
    audioPreview_->SetListener(listener);
    audioPreview_->Update(objects_, delta);
    for (const auto id : audioPreview_->Started())  scriptHost_.EmitSignal(id, "started");
    for (const auto id : audioPreview_->Looped())   scriptHost_.EmitSignal(id, "looped");
    for (const auto id : audioPreview_->Finished()) scriptHost_.EmitSignal(id, "finished");
}
void EditorShell::Stop()
{
    SetFocus(window_);
    mouseCapture_.Release(); // ZE-84: restore the OS cursor
    if(consoleWindow_){DestroyWindow(consoleWindow_);consoleWindow_=nullptr;}
    scriptHost_.Stop(objects_);physicsWorld_.reset();if(audioPreview_){audioPreview_->StopAll();audioPreview_.reset();} paused_=false; stepDraw_=false; tickAccumulator_=0;
    std::set<zengine::GameObjectId> spawned;for(std::size_t i=0;i<objects_.Size();++i)if(!playObjects_.contains(objects_.At(i).Id()))spawned.insert(objects_.At(i).Id());
    for(const auto id:spawned)if(auto* object=objects_.Find(id))for(std::size_t i=0;i<object->BehaviorCount();++i)if(auto* script=dynamic_cast<zengine::ScriptBehavior*>(&object->BehaviorAt(i)))scriptHost_.Forget(*script);
    if(!spawned.empty()){objects_.Remove(spawned);for(const auto id:spawned){meshBindings_.erase(id);meshRevisions_.erase(id);}if(spawned.contains(selectedObject_)){selectedObject_=0;inspectorPanel_->Bind(nullptr);}}
    if(playScene_)for(const auto& saved:playScene_->objects)if(auto* object=objects_.Find(saved.id)){
        object->SetName(saved.name);object->SetTags(saved.tags);if(auto* g=zengine::As3D(object))g->GetTransform()=saved.transform;
        for(std::size_t i=0;i<saved.behaviors.size()&&i<object->BehaviorCount();++i)RestoreBehaviorState(object->BehaviorAt(i),saved.behaviors[i]);
    }
    playScene_.reset();playObjects_.clear();
    status_=L"Stopped - authored scene state restored";
    PrepareScripts(); inspectorPanel_->RefreshLiveValues();
    if (const auto* selected=SelectedGameObject()) SelectGameObject(selected->Id());
    if (viewTab_ == ViewTab::Game) SetViewTab(ViewTab::Scene); // ending the game returns to Scene
    InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::SetPaused(bool paused)
{
    if (!Playing()) return;
    paused_=paused; tickAccumulator_=0; lastTick_=std::chrono::steady_clock::now();
    status_=paused?L"Paused - Step advances one 1/60 second tick":L"Playing";
    InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::Step()
{
    if (!Playing()) return;
    SetPaused(true); TickInput(); scriptHost_.Tick(objects_,1.0f/60.0f);scriptHost_.PhysicsTick(objects_,1.0f/60.0f);physicsWorld_->Step(objects_,1.0f/60.0f);scriptHost_.DispatchPhysicsEvents(physicsWorld_->DrainEvents()); stepDraw_=true;
    inspectorPanel_->RefreshLiveValues(); ReportScriptErrors();
}
void EditorShell::ReportScriptErrors()
{
    for (std::size_t i=0;i<objects_.Size();++i)
        for (std::size_t j=0;j<objects_.At(i).BehaviorCount();++j)
            if (const auto* script=dynamic_cast<const zengine::ScriptBehavior*>(&objects_.At(i).BehaviorAt(j)))
            {
                const auto error=scriptHost_.Error(*script);
                if (!error.empty())
                {
                    const auto text=WideText(objects_.At(i).Name()+": "+error);
                    if (status_!=text) { status_=text; InvalidateRect(window_,&statusBar_,FALSE); }
                    return;
                }
            }
}

ViewportFrame EditorShell::BuildSceneFrame() const
{
    ViewportFrame frame;
    frame.camera=sceneCamera_;
    frame.showEditorGuides = true;
    frame.tool=transformTool_; frame.highlightedAxis=hoveredAxis_;
    for (std::size_t i = 0; i < objects_.Size(); ++i)
    {
        const auto* object3d = zengine::As3D(&objects_.At(i)); if (!object3d) continue; // 2D objects draw in the 2D pass (ZE-60)
        const auto& object = *object3d;
        const auto* mesh = object.GetBehavior<zengine::MeshRenderer>();
        if(!Playing())if(const auto* collider=object.GetBehavior<zengine::physics::Collider>();collider&&collider->Enabled()){DirectX::XMFLOAT4X4 parent;DirectX::XMStoreFloat4x4(&parent,ParentMatrix(objects_,object));const bool audioZone=object.GetBehavior<zengine::audio::AudioEffect>()!=nullptr;frame.colliders.push_back({collider->Shape(),object.GetTransform(),collider->Offset(),collider->Size(),parent,object.Id()==selectedObject_,audioZone});}
        // ZE-111: audible-range spheres for a selected 3D (positional) AudioSource; global/2D sources show nothing.
        if(!Playing() && object.Id()==selectedObject_)
            if(const auto* audioSrc=object.GetBehavior<zengine::audio::AudioSource>();audioSrc && audioSrc->Enabled() && audioSrc->Spatial()){
                DirectX::XMFLOAT4X4 parent;DirectX::XMStoreFloat4x4(&parent,ParentMatrix(objects_,object));
                frame.audioRanges.push_back({object.GetTransform(),parent,audioSrc->MinDistance(),audioSrc->MaxDistance(),true});
            }
        if(const auto* camera=object.GetBehavior<zengine::Camera>())
        {
            DirectX::XMFLOAT4X4 parent; DirectX::XMStoreFloat4x4(&parent,ParentMatrix(objects_,object));
            CameraView view{object.GetTransform(),parent,camera->FieldOfView(),camera->NearPlane(),camera->FarPlane(),object.Id()==selectedObject_,camera->IsMain()};
            if(!Playing()) frame.cameraGizmos.push_back(view);
            if(view.main && (viewTab_==ViewTab::Game || Playing()) && camera->Enabled()) frame.gameView=view;
        }
        const auto bound = meshBindings_.find(object.Id());
        if (mesh && mesh->Enabled() && !mesh->Asset().empty() && bound != meshBindings_.end() && bound->second.asset == mesh->Asset())
        {
            DirectX::XMFLOAT4X4 parent; DirectX::XMStoreFloat4x4(&parent,ParentMatrix(objects_,object));
            const auto resolved = ResolveLightmapMesh(object, *mesh, bound->second.mesh);
            frame.meshes.push_back({resolved.mesh, object.GetTransform(),parent, ResolveMaterial(mesh->Material()), resolved.lit});
        }
        if (const auto* env = object.GetBehavior<zengine::Environment>(); env && env->Enabled() && !frame.environment)
            frame.environment = MakeEnvironment(*env);
        if (const auto* light = object.GetBehavior<zengine::Light>(); light && light->Enabled())
        {
            const LightData ld = MakeLight(*light, object.GetTransform(), ParentMatrix(objects_, object));
            if (frame.lights.size() < 8) frame.lights.push_back(ld);
            if (!Playing())
                frame.lightGizmos.push_back({ld.type, ld.position, ld.direction, ld.color, ld.range,
                                             light->SpotOuter(), object.Id() == selectedObject_});
        }
        if (const auto* decal = object.GetBehavior<zengine::Decal>(); decal && decal->Enabled())
        {
            DirectX::XMFLOAT4X4 parent; DirectX::XMStoreFloat4x4(&parent, ParentMatrix(objects_, object));
            DecalData dd;
            dd.transform = object.GetTransform();
            dd.parentMatrix = parent;
            dd.texture = ResolveDecalTexture(decal->Texture());
            dd.tint = {decal->Tint().x, decal->Tint().y, decal->Tint().z};
            dd.opacity = decal->Opacity();
            dd.angleFadeCos = std::cos(decal->AngleFade() * 3.14159265f / 180.0f);
            frame.decals.push_back(dd);
            if (!Playing())
                frame.colliders.push_back({zengine::physics::ColliderShape::Box, object.GetTransform(),
                                           {0, 0, 0}, {1, 1, 1}, parent, object.Id() == selectedObject_, false, true});
        }
    }
    if (const auto* object = SelectedGameObject(); object && CanEdit(object->Id(),true))
    {
        const auto parent=ParentMatrix(objects_,*object);
        if (std::abs(DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(parent)))>1e-10f)
        {
            frame.selectionTransform = object->GetTransform(); DirectX::XMFLOAT4X4 value;
            DirectX::XMStoreFloat4x4(&value,parent); frame.selectionParent=value;
        }
    }
    return frame;
}

void EditorShell::Layout(const std::uint32_t width, const std::uint32_t height)
{
    EndGizmoDrag(true);
    const int clientWidth = static_cast<int>(width);
    const int clientHeight = static_cast<int>(height);
    if (clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

    optionsBar_ = RECT{0, 0, clientWidth, std::min(OptionsBarHeight, clientHeight)};
    statusBar_ = RECT{0, std::max(0, clientHeight - StatusBarHeight), clientWidth, clientHeight};

    const int workspaceTop = optionsBar_.bottom;
    const int workspaceBottom = statusBar_.top;
    const int workspaceHeight = std::max(0, workspaceBottom - workspaceTop);
    mediaLibraryHeight_ = std::clamp(
        mediaLibraryHeight_, MinimumMediaHeight,
        std::max(MinimumMediaHeight, workspaceHeight - MinimumUpperHeight - SplitterSize));

    mediaLibrary_ = RECT{
        0, std::max(workspaceTop, workspaceBottom - mediaLibraryHeight_), clientWidth, workspaceBottom};
    mediaSplitter_ = RECT{0, std::max<LONG>(workspaceTop, mediaLibrary_.top - SplitterSize),
                          clientWidth, mediaLibrary_.top};
    const int upperBottom = mediaSplitter_.top;

    const int maximumSideSpace = std::max(0, clientWidth - MinimumViewportWidth - (SplitterSize * 2));
    sceneBrowserWidth_ = std::clamp(sceneBrowserWidth_, MinimumSideWidth,
                                   std::max(MinimumSideWidth, maximumSideSpace - MinimumSideWidth));
    inspectorWidth_ = std::clamp(inspectorWidth_, MinimumSideWidth,
                                 std::max(MinimumSideWidth, maximumSideSpace - sceneBrowserWidth_));
    if (sceneBrowserWidth_ + inspectorWidth_ > maximumSideSpace)
    {
        inspectorWidth_ = std::max(MinimumSideWidth, maximumSideSpace - sceneBrowserWidth_);
    }

    sceneBrowser_ = RECT{0, workspaceTop, std::min(sceneBrowserWidth_, clientWidth), upperBottom};
    sceneSplitter_ = RECT{sceneBrowser_.right, workspaceTop,
                          std::min<LONG>(sceneBrowser_.right + SplitterSize, clientWidth), upperBottom};
    inspector_ = RECT{std::max<LONG>(sceneSplitter_.right, clientWidth - inspectorWidth_), workspaceTop,
                      clientWidth, upperBottom};
    inspectorSplitter_ = RECT{std::max(sceneSplitter_.right, inspector_.left - SplitterSize), workspaceTop,
                              inspector_.left, upperBottom};
    viewportPanel_ = RECT{sceneSplitter_.right, workspaceTop, inspectorSplitter_.left, upperBottom};
    viewportContent_ = viewportPanel_;
    viewportContent_.top = std::min(viewportContent_.bottom, viewportContent_.top + PanelHeaderHeight);

    // A running game docks its console into the bottom of the Game tab.
    const bool showConsole = consoleWindow_ && viewTab_ == ViewTab::Game && Playing();
    RECT consoleRect{};
    if (showConsole)
    {
        const int consoleHeight = std::clamp<int>((viewportContent_.bottom - viewportContent_.top) / 3, 60, 220);
        consoleRect = RECT{viewportContent_.left, viewportContent_.bottom - consoleHeight, viewportContent_.right, viewportContent_.bottom};
        viewportContent_.bottom = std::max(viewportContent_.top, consoleRect.top - 2);
    }
    if (consoleWindow_)
    {
        ShowWindow(consoleWindow_, showConsole ? SW_SHOW : SW_HIDE);
        if (showConsole)
            SetWindowPos(consoleWindow_, HWND_TOP, consoleRect.left, consoleRect.top,
                         consoleRect.right - consoleRect.left, consoleRect.bottom - consoleRect.top, SWP_NOACTIVATE);
    }

    const int viewportWidth = std::max<LONG>(0, viewportContent_.right - viewportContent_.left);
    const int viewportHeight = std::max<LONG>(0, viewportContent_.bottom - viewportContent_.top);
    requestedViewportWidth_ = static_cast<std::uint32_t>(viewportWidth);
    requestedViewportHeight_ = static_cast<std::uint32_t>(viewportHeight);
    if (viewportWindow_)
    {
        SetWindowPos(viewportWindow_, nullptr,
                     viewportContent_.left, viewportContent_.top, viewportWidth, viewportHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (inspectorPanel_)
    {
        SetWindowPos(inspectorPanel_->Window(), nullptr, inspector_.left + 1, inspector_.top + PanelHeaderHeight,
            std::max<LONG>(0, inspector_.right - inspector_.left - 2),
            std::max<LONG>(0, inspector_.bottom - inspector_.top - PanelHeaderHeight - 1), SWP_NOACTIVATE | SWP_NOZORDER);
    }
    LayoutScriptTab();

    InvalidateRect(window_, nullptr, FALSE);
}

void EditorShell::Paint()
{
    PAINTSTRUCT paint{};
    const HDC windowContext = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max<LONG>(1, client.right - client.left);
    const int height = std::max<LONG>(1, client.bottom - client.top);

    const HDC bufferContext = CreateCompatibleDC(windowContext);
    const HBITMAP bufferBitmap = CreateCompatibleBitmap(windowContext, width, height);
    const HGDIOBJ previousBitmap = SelectObject(bufferContext, bufferBitmap);
    SelectObject(bufferContext, uiFont_);
    FillRectangle(bufferContext, client, EditorBackground);
    const auto button=[&](int index,std::wstring_view label,bool selected=false){editorStyle::Button(bufferContext,ChromeRectangle(index),label,ChromeEnabled(index),hoveredChrome_==index,pressedChrome_==index,selected);};

    // Options bar
    FillRectangle(bufferContext, optionsBar_, RGB(35, 37, 41));
    RECT brand = optionsBar_;
    brand.left += 10;
    brand.right = brand.left + 26;
    SelectObject(bufferContext, headerFont_);
    DrawTextLabel(bufferContext, L"z", brand, RGB(108, 166, 232), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(bufferContext, uiFont_);
    constexpr std::array<std::wstring_view, 5> menus{L"File", L"Edit", L"View", L"Window", L"Help"};
    int menuX = 44;
    for (const std::wstring_view menu : menus)
    {
        RECT menuRectangle{menuX, optionsBar_.top, menuX + 62, optionsBar_.bottom};
        DrawTextLabel(bufferContext, menu, menuRectangle, TextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        menuX += (menu == L"Window") ? 70 : 55;
    }
    RECT projectName{optionsBar_.right - 370, optionsBar_.top, optionsBar_.right - 140, optionsBar_.bottom};
    const wchar_t* toolNames[]={L"Move W",L"Rotate E",L"Scale R"};
    for (int i=0;i<3;++i)
    {
        button(6+i,toolNames[i],static_cast<int>(transformTool_)==i);
    }
    DrawTextLabel(bufferContext, project_?WideText(project_->config.name):L"No project open", projectName, MutedTextColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    button(11,(showFps_?L"☑ FPS ":L"☐ FPS ")+std::to_wstring(currentFps_),showFps_);
    RECT optionLine{0, optionsBar_.bottom - 1, optionsBar_.right, optionsBar_.bottom};
    FillRectangle(bufferContext, optionLine, BorderColor);

    // Main panels
    SelectObject(bufferContext, headerFont_);
    DrawPanel(bufferContext, sceneBrowser_, L"Scene Browser");
    DrawPanel(bufferContext, inspector_, L"Inspector");
    DrawPanel(bufferContext, mediaLibrary_, L"Media Library");
    SelectObject(bufferContext, uiFont_);

    // View panel: a Scene / Game / Script tab strip instead of a plain header label.
    {
        FillRectangle(bufferContext, viewportPanel_, PanelBackground);
        RECT vpHeader = viewportPanel_; vpHeader.bottom = std::min<LONG>(vpHeader.bottom, vpHeader.top + PanelHeaderHeight);
        FillRectangle(bufferContext, vpHeader, HeaderBackground);
        DrawBorder(bufferContext, viewportPanel_, BorderColor);
        const wchar_t* tabNames[3]{editingPrefab_.empty() ? L"Scene" : L"Prefab", L"Game", L"Script"};
        for (int i = 0; i < 3; ++i)
        {
            const RECT tab = ViewTabRect(i);
            const bool on = static_cast<int>(viewTab_) == i;
            FillRectangle(bufferContext, tab, on ? PanelBackground : (hoveredTab_ == i ? RGB(58,62,70) : HeaderBackground));
            if (on) { RECT underline{tab.left, tab.bottom - 2, tab.right, tab.bottom}; FillRectangle(bufferContext, underline, RGB(108,166,232)); }
            DrawTextLabel(bufferContext, tabNames[i], tab, on ? TextColor : MutedTextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        if (viewTab_ == ViewTab::Game && !Playing())
            DrawTextLabel(bufferContext, L"Press Play to run the scene here.  Camera view arrives with the camera system.",
                          viewportContent_, MutedTextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // Live GameObject scene tree, indented for prefab hierarchies.
    const int sceneX = sceneBrowser_.left + 13;
    const RECT create = CreateObjectRectangle();
    button(0,L"+ Create Empty");
    int sceneY = create.bottom + 5;
    RECT sceneRoot{sceneX, sceneY, sceneBrowser_.right - 8, sceneY + 25};
    DrawTextLabel(bufferContext, L"\u25be  "+SceneName()+(sceneDirty_?L" *":L""), sceneRoot, TextColor);
    const RECT objectList = ObjectListRectangle();
    const int treeClip = SaveDC(bufferContext);
    IntersectClipRect(bufferContext, objectList.left, objectList.top, objectList.right, objectList.bottom);
    const auto objectRows=ObjectRows();
    for (int index = firstObject_; index < static_cast<int>(objectRows.size()); ++index)
    {
        RECT row{objectList.left, objectList.top + (index - firstObject_) * 27,
                 objectList.right, objectList.top + (index - firstObject_ + 1) * 27};
        if (row.top >= objectList.bottom) break;
        const auto& object = *objects_.Find(objectRows[index]);
        if (object.Id() == selectedObject_) FillRectangle(bufferContext, row, SelectionColor);
        row.left += 12+ObjectDepth(object.Id())*12;
        if(HasChildren(object.Id())) {RECT arrow=row;arrow.right=arrow.left+12;DrawTextLabel(bufferContext,collapsedObjects_.contains(object.Id())?L"\u25b8":L"\u25be",arrow,TextColor);}
        row.left+=14;
        const std::wstring icon = prefabSources_.contains(object.Id())?L"[P] ":object.GetBehavior<zengine::MeshRenderer>() ? L"\u25a0  " : L"\u25c7  ";
        DrawTextLabel(bufferContext, icon + WideText(object.Name()), row, TextColor,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    RestoreDC(bufferContext, treeClip);

    // Viewport header controls. Rendering occurs in the child window below this strip.
    if (viewTab_ != ViewTab::Script)
    {
        button(3,Playing()?L"\u25a0":L"\u25b6",Playing());
        button(4,L"\u2016",paused_);
        button(5,L"\u25b8|");
    }

    // Inspector content and edit controls are hosted by the reusable InspectorPanel child.

    // Project asset library
    const int mediaTop = mediaLibrary_.top + PanelHeaderHeight;
    RECT breadcrumb{mediaLibrary_.left+12,mediaTop+8,mediaLibrary_.right-12,mediaTop+36};
    if(project_ && AssetFolder()!=assetsDirectory_){button(10,L"\u2191");breadcrumb.left+=40;}
    const auto relative=project_?std::filesystem::relative(AssetFolder(),assetsDirectory_).wstring():L"";
    DrawTextLabel(bufferContext,L"Assets"+(relative.empty()||relative==L"."?L"":L" / "+relative),breadcrumb,MutedTextColor,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    const RECT list = AssetListRectangle();
    const int saved = SaveDC(bufferContext);
    IntersectClipRect(bufferContext, list.left, list.top, list.right, list.bottom);
    if (assets_.empty())
        DrawTextLabel(bufferContext, L"No assets - create a script or import an FBX", list, MutedTextColor, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    for (int index = firstAsset_; index < static_cast<int>(assets_.size()); ++index)
    {
        RECT cell=AssetCellRectangle(index);if(cell.top>=list.bottom)break;
        if(index==selectedAsset_)FillRectangle(bufferContext,cell,SelectionColor);
        const auto kind=assetLibrary::Type(assets_[index]);const int iconX=(cell.left+cell.right)/2-9;assetLibrary::Icon(bufferContext,kind,iconX,cell.top+12);
        RECT name{cell.left+4,cell.top+39,cell.right-4,cell.bottom-3};DrawTextLabel(bufferContext,AssetDisplayName(assets_[index]),name,TextColor,DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS);
    }
    RestoreDC(bufferContext, saved);

    // Status bar and dormant progress indicator
    FillRectangle(bufferContext, statusBar_, RGB(30, 32, 36));
    RECT statusText{statusBar_.left + 10, statusBar_.top, statusBar_.right - 370, statusBar_.bottom};
    DrawTextLabel(bufferContext, status_, statusText, TextColor, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT taskText{statusBar_.right - 360, statusBar_.top, statusBar_.right - 190, statusBar_.bottom};
    DrawTextLabel(bufferContext,buildWork_.valid()?L"Building game...":assetWork_.valid() ? L"Processing asset..." : L"No tasks running", taskText,
                  MutedTextColor, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT progressTrack{statusBar_.right - 176, statusBar_.top + 8, statusBar_.right - 12, statusBar_.bottom - 8};
    FillRectangle(bufferContext, progressTrack, FieldColor);
    DrawBorder(bufferContext, progressTrack, BorderColor);
    if(buildWork_.valid() && buildProgress_) {
        std::lock_guard lock(buildProgress_->mutex);RECT progress=progressTrack;InflateRect(&progress,-2,-2);
        progress.right=progress.left+static_cast<LONG>((progress.right-progress.left)*buildProgress_->percent/100);FillRectangle(bufferContext,progress,RGB(63,126,201));
    }
    else if (assetWork_.valid())
    {
        const LONG offset = static_cast<LONG>((GetTickCount64() / 15) % 120);
        RECT activity{progressTrack.left + 2 + offset, progressTrack.top + 2,
                      progressTrack.left + 30 + offset, progressTrack.bottom - 2};
        FillRectangle(bufferContext, activity, RGB(63, 126, 201));
    }

    BitBlt(windowContext, 0, 0, width, height, bufferContext, 0, 0, SRCCOPY);
    SelectObject(bufferContext, previousBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferContext);
    EndPaint(window_, &paint);
}

EditorShell::DragTarget EditorShell::HitTestSplitter(const POINT position) const
{
    if (PtInRect(&sceneSplitter_, position))
    {
        return DragTarget::SceneBrowser;
    }
    if (PtInRect(&inspectorSplitter_, position))
    {
        return DragTarget::Inspector;
    }
    if (PtInRect(&mediaSplitter_, position))
    {
        return DragTarget::MediaLibrary;
    }
    return DragTarget::None;
}

void EditorShell::BeginDrag(const POINT position)
{
    dragTarget_ = HitTestSplitter(position);
    if (dragTarget_ != DragTarget::None)
    {
        SetCapture(window_);
    }
}

RECT EditorShell::CreateObjectRectangle() const
{
    return {sceneBrowser_.left + 8, sceneBrowser_.top + PanelHeaderHeight + 8,
            sceneBrowser_.right - 8, sceneBrowser_.top + PanelHeaderHeight + 34};
}
RECT EditorShell::ObjectListRectangle() const
{
    return {sceneBrowser_.left + 5, sceneBrowser_.top + PanelHeaderHeight + 69,
            sceneBrowser_.right - 5, sceneBrowser_.bottom - 5};
}
zengine::GameObject& EditorShell::CreateEmptyGameObject()
{
    RequireScene();
    if (Playing()) throw std::runtime_error("Stop Play before creating objects.");
    std::string name = "GameObject";
    for (unsigned suffix = 1;; ++suffix)
    {
        bool exists = false;
        for (std::size_t index = 0; index < objects_.Size(); ++index)
            exists = exists || objects_.At(index).Name() == name;
        if (!exists) break;
        name = "GameObject (" + std::to_string(suffix) + ")";
    }
    auto& object = objects_.Create(std::move(name));
    if (!editingPrefab_.empty() && objects_.Size()>1) object.SetParent(objects_.At(0).Id());
    MarkSceneDirty();
    SelectGameObject(object.Id());
    status_ = L"Created empty GameObject - edit its transform in the Inspector";
    InvalidateRect(window_, nullptr, FALSE);
    BeginObjectRename(object.Id());
    return object;
}
void EditorShell::SelectGameObject(zengine::GameObjectId id)
{
    EndGizmoDrag(true);
    auto* object = objects_.Find(id);
    if (!object) return;
    for(auto parent=object->Parent();parent;parent=objects_.Find(parent)->Parent())collapsedObjects_.erase(parent);
    const auto rows=ObjectRows();const auto found=std::find(rows.begin(),rows.end(),id);
    const auto list=ObjectListRectangle();const int visible=std::max(1,static_cast<int>((list.bottom-list.top)/27));
    const int index=static_cast<int>(found-rows.begin());
    firstObject_=std::clamp(firstObject_,0,std::max(0,static_cast<int>(rows.size())-visible));
    if(index<firstObject_)firstObject_=index;else if(index>=firstObject_+visible)firstObject_=index-visible+1;
    if (inspectorPanel_) inspectorPanel_->Bind(object,CanEdit(id),CanEdit(id,true));
    selectedObject_ = id;
    selectedAsset_ = -1; // ZE-82: keep asset / object selection mutually exclusive so Delete is unambiguous
    if (prefabLinks_.contains(id)) status_=L"Prefab instance root: edits are saved as instance overrides; double-click the asset to edit every non-overridden instance";
    else if (prefabSources_.contains(id)) status_=L"Nested prefab content is inherited; edit its prefab asset to change it";
    InvalidateRect(window_, &sceneBrowser_, FALSE);
}
void EditorShell::OnObjectChanged()
{
    if (!Playing()) { RecordTransformOverride(selectedObject_); RecordPrefabDataOverride(selectedObject_); }
    // Keep the "main" camera unique when the selected camera's tag was just changed.
    if (const auto* selected = SelectedGameObject(); selected && selected->GetBehavior<zengine::Camera>() && selected->HasTag(zengine::Camera::MainTag))
        SyncMainCamera(selected->Id());
    MarkSceneDirty();
    if (const auto* selected = SelectedGameObject())
        SetWindowTextW(viewportWindow_, (L"Scene Viewport - " + WideText(selected->Name())).c_str());
    InvalidateRect(window_, &sceneBrowser_, FALSE);
}

RECT EditorShell::AssetListRectangle() const
{
    return {mediaLibrary_.left+12,mediaLibrary_.top+PanelHeaderHeight+42,mediaLibrary_.right-12,mediaLibrary_.bottom-12};
}
int EditorShell::AssetColumns() const {const auto list=AssetListRectangle();return std::max(1,static_cast<int>(list.right-list.left)/112);}
RECT EditorShell::AssetCellRectangle(int index) const {const auto list=AssetListRectangle();const int columns=AssetColumns(),slot=index-firstAsset_,column=slot%columns,row=slot/columns;return {list.left+column*112,list.top+row*84,list.left+column*112+104,list.top+row*84+78};}
int EditorShell::AssetAt(POINT point) const {const auto list=AssetListRectangle();if(!PtInRect(&list,point))return -1;const int column=(point.x-list.left)/112,row=(point.y-list.top)/84;if(column<0||column>=AssetColumns())return -1;const int index=firstAsset_+row*AssetColumns()+column;if(index<0||index>=static_cast<int>(assets_.size()))return -1;const auto cell=AssetCellRectangle(index);return PtInRect(&cell,point)?index:-1;}
std::wstring EditorShell::AssetDisplayName(const std::filesystem::path& asset) const {const auto kind=assetLibrary::Type(asset);return kind==assetLibrary::Kind::Input?L"Input Map":kind==assetLibrary::Kind::Model&&assetLibrary::Package(asset.parent_path())?asset.parent_path().filename().wstring():asset.filename().wstring();}
LRESULT CALLBACK EditorShell::RenameProcedure(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data)
{
    auto* editor=reinterpret_cast<EditorShell*>(data);if(message==WM_KEYDOWN&&(w==VK_RETURN||w==VK_ESCAPE)){editor->FinishRename(w==VK_ESCAPE);return 0;}
    if(message==WM_KILLFOCUS){editor->FinishRename(false);return 0;}
    return DefSubclassProc(window,message,w,l);
}
void EditorShell::BeginAssetRename(const std::filesystem::path& asset)
{
    FinishRename(true);RefreshAssets();const auto storage=assetLibrary::Storage(asset);int index=-1;for(int i=0;i<static_cast<int>(assets_.size());++i)if(assetLibrary::Storage(assets_[i])==storage){index=i;break;}if(index<0)return;
    selectedAsset_=index;const auto cell=AssetCellRectangle(index);RECT edit{cell.left+2,cell.top+39,cell.right-2,cell.bottom-2};
    renameTarget_=RenameTarget::Asset;renameAsset_=assets_[index];renameEdit_=CreateWindowExW(0,L"EDIT",AssetDisplayName(assets_[index]).c_str(),WS_CHILD|WS_VISIBLE|WS_BORDER|ES_CENTER|ES_AUTOHSCROLL,edit.left,edit.top,edit.right-edit.left,edit.bottom-edit.top,window_,reinterpret_cast<HMENU>(3900),instance_,nullptr);
    SendMessageW(renameEdit_,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont_),TRUE);SetWindowSubclass(renameEdit_,RenameProcedure,1,reinterpret_cast<DWORD_PTR>(this));SetFocus(renameEdit_);SendMessageW(renameEdit_,EM_SETSEL,0,-1);InvalidateRect(window_,&mediaLibrary_,FALSE);
}
void EditorShell::BeginObjectRename(zengine::GameObjectId id)
{
    FinishRename(true);RequireEditable(id);if(Playing())throw std::runtime_error("Stop Play before renaming objects.");SelectGameObject(id);const auto rows=ObjectRows();const auto found=std::find(rows.begin(),rows.end(),id);if(found==rows.end())return;const int row=static_cast<int>(found-rows.begin())-firstObject_;const auto list=ObjectListRectangle();RECT edit{list.left+28+ObjectDepth(id)*12,list.top+row*27+2,list.right-4,list.top+row*27+25};
    renameTarget_=RenameTarget::Object;renameObject_=id;renameObjectOriginal_=objects_.Find(id)->Name();renameEdit_=CreateWindowExW(0,L"EDIT",WideText(renameObjectOriginal_).c_str(),WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,edit.left,edit.top,edit.right-edit.left,edit.bottom-edit.top,window_,reinterpret_cast<HMENU>(3900),instance_,nullptr);
    SendMessageW(renameEdit_,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont_),TRUE);SetWindowSubclass(renameEdit_,RenameProcedure,1,reinterpret_cast<DWORD_PTR>(this));SetFocus(renameEdit_);SendMessageW(renameEdit_,EM_SETSEL,0,-1);
}
void EditorShell::FinishRename(bool cancel)
{
    if(!renameEdit_||finishingRename_)return;finishingRename_=true;wchar_t value[512]{};GetWindowTextW(renameEdit_,value,512);RemoveWindowSubclass(renameEdit_,RenameProcedure,1);DestroyWindow(renameEdit_);renameEdit_=nullptr;
    const auto target=renameTarget_;const auto asset=renameAsset_;const auto object=renameObject_;const auto original=renameObjectOriginal_;renameTarget_=RenameTarget::None;renameAsset_.clear();renameObject_=0;renameObjectOriginal_.clear();
    if(!cancel)try{if(target==RenameTarget::Asset)RenameAsset(asset,value);else if(target==RenameTarget::Object){RequireEditable(object);auto* current=objects_.Find(object);if(current&&current->Name()==original){current->SetName(Utf8Text(value));OnObjectChanged();}}}catch(const std::exception& error){status_=L"Rename failed: "+WideText(error.what());InvalidateRect(window_,nullptr,FALSE);}
    finishingRename_=false;
}

void EditorShell::RefreshAssets()
{
    draggedAsset_ = -1;
    if (GetCapture() == window_ && dragTarget_ == DragTarget::None) ReleaseCapture();
    assets_.clear();
    if (project_ && std::filesystem::exists(assetsDirectory_))
    {
        zengine::input::Ensure(assetsDirectory_);
        try {if(!std::filesystem::is_directory(AssetFolder()))assetFolder_.clear();}catch(const std::exception&){assetFolder_.clear();}
        assets_=assetLibrary::List(assetsDirectory_,AssetFolder());
    }
    const int columns=AssetColumns();firstAsset_=std::clamp(firstAsset_/columns*columns,0,std::max(0,(static_cast<int>(assets_.size())-1)/columns*columns));
    selectedAsset_ = -1;
    RefreshScriptTabList();
}

void EditorShell::ReceiveFiles(HDROP drop)
{
    struct DropOwner { HDROP handle; ~DropOwner() { DragFinish(handle); } } owner{drop};
    RequireProject();
    POINT location{};
    if (!DragQueryPoint(drop, &location) || !PtInRect(&mediaLibrary_, location))
    {
        status_ = L"Drop FBX files into the Media Library first.";
        InvalidateRect(window_, &statusBar_, FALSE);
        return;
    }
    const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT index = 0; index < count; ++index)
    {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::vector<wchar_t> name(static_cast<std::size_t>(length) + 1);
        if (DragQueryFileW(drop, index, name.data(), static_cast<UINT>(name.size()))) {
            AssetJob job;job.path=name.data();job.destination=AssetFolder();assetJobs_.push_back(std::move(job));
        }
    }
}

void EditorShell::PollAssetWork()
{
    if (assetWork_.valid() && assetWork_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const auto completedJob=activeAssetJob_;
        activeAssetJob_.reset();
        try
        {
            auto result = assetWork_.get();
            if (result.loadMesh)
            {
                if (result.scene!=sceneGeneration_) return; // Never apply a previous scene's asynchronous assignment.
                if (result.object && meshRevisions_[result.object] != result.revision) return; // Superseded assignment.
                if (!renderer_) throw std::runtime_error("Renderer is not initialized.");
                MeshHandle handle = result.cachedMesh ? result.cachedMesh : meshCache_[result.path].lock();
                if (!handle)
                {
                    std::vector<std::string> textureWarnings;
                    handle = renderer_->UploadModel(result.model, textureWarnings);
                    result.warnings = std::move(result.model.warnings);
                    result.warnings.insert(result.warnings.end(), textureWarnings.begin(), textureWarnings.end());
                    meshCache_[result.path] = handle;
                }
                const auto name = result.path.parent_path().filename().wstring();
                auto* object = result.object ? objects_.Find(result.object) : &objects_.Create(Utf8Text(name));
                if (!object) throw std::runtime_error("The target GameObject no longer exists.");
                auto* mesh = object->GetBehavior<zengine::MeshRenderer>();
                if (!mesh) mesh = &object->AddBehavior<zengine::MeshRenderer>();
                const auto relative = std::filesystem::relative(result.path,assetsDirectory_).generic_u8string();
                const std::string asset(reinterpret_cast<const char*>(relative.data()),relative.size());
                meshBindings_[object->Id()] = {asset,handle};
                mesh->SetAsset(asset);
                // ZE-126: adopt the package's default .material (written on import) unless one is already set.
                if (mesh->Material().empty())
                {
                    const auto defaultMaterial = result.path.parent_path() / (result.path.parent_path().filename().wstring() + L".material");
                    if (std::filesystem::is_regular_file(defaultMaterial))
                    {
                        const auto rel = std::filesystem::relative(defaultMaterial, assetsDirectory_).generic_u8string();
                        mesh->SetMaterial(std::string(reinterpret_cast<const char*>(rel.data()), rel.size()));
                    }
                }
                if (!result.object) SelectGameObject(object->Id());
                else if (selectedObject_ == object->Id()) inspectorPanel_->RefreshBehaviors();
                if (!result.restoreOnly) OnObjectChanged();
                status_ = L"Assigned " + name + L" to " + WideText(object->Name());
            }
            else
            {
                // Cancel a row drag before rebuilding the library order.
                draggedAsset_ = -1;
                if (GetCapture() == window_ && dragTarget_ == DragTarget::None) ReleaseCapture();
                RefreshAssets();
                status_ = L"Imported " + result.path.parent_path().filename().wstring();
                BeginAssetRename(result.path);
            }
            if (!result.warnings.empty())
                status_ += L" (albedo fallback: " + WideText(result.warnings.front()) + L")";
        }
        catch (const std::exception& error)
        {
            if (completedJob && completedJob->loadMesh && completedJob->scene!=sceneGeneration_) return;
            status_ = L"Asset operation failed: " + WideText(error.what());
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    if (!assetWork_.valid() && !assetJobs_.empty())
    {
        const auto job = assetJobs_.front();
        assetJobs_.pop_front();
        activeAssetJob_=job;
        status_ = (job.loadMesh ? L"Loading " : L"Importing ") + job.path.filename().wstring();
        const auto directory = job.destination.empty()?assetsDirectory_:assetLibrary::Resolve(assetsDirectory_,job.destination);
        const MeshHandle cached = job.loadMesh ? meshCache_[job.path].lock() : nullptr;
        assetWork_ = std::async(std::launch::async, [job, directory, cached]() {
            AssetResult result;
            result.loadMesh = job.loadMesh;
            result.object = job.object;
            result.revision = job.revision;
            result.scene=job.scene; result.restoreOnly=job.restoreOnly;
            result.path = job.path;
            result.cachedMesh = cached;
            if (job.loadMesh) { if (!cached) result.model = FbxImporter::Load(job.path, true); }
            else if(assetLibrary::Type(job.path)==assetLibrary::Kind::Model)result.path = FbxImporter::Import(job.path, directory, result.warnings);
            else {
                const auto kind=assetLibrary::Type(job.path);
                if(kind!=assetLibrary::Kind::Image && kind!=assetLibrary::Kind::Script && kind!=assetLibrary::Kind::Audio)throw std::runtime_error("Import FBX models, images, audio (.wav/.mp3/.ogg/.flac), or .zsh scripts. Create folders in the library.");
                if(kind==assetLibrary::Kind::Script)zengine::scripts::Load(job.path);
                result.path=directory/job.path.filename();
                if(!std::filesystem::copy_file(job.path,result.path,std::filesystem::copy_options::none))throw std::runtime_error("Asset already exists; original preserved.");
            }
            return result;
        });
    }
    if (assetWork_.valid() && GetTickCount64() - lastBusyPaint_ > 100)
    {
        lastBusyPaint_ = GetTickCount64();
        InvalidateRect(window_, &statusBar_, FALSE);
    }
}

void EditorShell::BeginAssetDrag(POINT point)
{
    const int index=AssetAt(point);if(index<0)return;
    const auto kind=assetLibrary::Type(assets_[index]);
    if(kind==assetLibrary::Kind::Input){selectedAsset_=index;draggedAsset_=-1;InvalidateRect(window_,&mediaLibrary_,FALSE);return;}
    selectedAsset_ = draggedAsset_ = index;
    assetDragStart_ = point;
    assetDragMoved_ = false;
    SetCapture(window_);
    InvalidateRect(window_, &mediaLibrary_, FALSE);
}

void EditorShell::FinishAssetDrag(POINT point)
{
    if (draggedAsset_ < 0) return;
    const auto path = assets_.at(draggedAsset_);
    const bool moved = assetDragMoved_;
    draggedAsset_ = -1;
    assetDragMoved_ = false;
    ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!moved) return;
    const auto destinationIndex=AssetAt(point);
    if(destinationIndex>=0 && destinationIndex<static_cast<int>(assets_.size()) && assetLibrary::Type(assets_[destinationIndex])==assetLibrary::Kind::Folder && destinationIndex!=selectedAsset_){MoveAsset(path,assets_[destinationIndex]);return;}
    if(path.extension()==L".zinput")return;
    if (zengine::prefabs::IsPrefab(path))
    {
        POINT screen=point;ClientToScreen(window_,&screen);
        const auto relative=std::filesystem::relative(path,assetsDirectory_).generic_u8string();
        const std::string asset(reinterpret_cast<const char*>(relative.data()),relative.size());
        if(inspectorPanel_->AssignPrefabAt(screen,asset))return;
        const auto parent=ScriptDropTarget(point);
        if (parent || PtInRect(&viewportContent_,point) || PtInRect(&sceneBrowser_,point)) InstantiatePrefab(path,parent);
        return;
    }
    if (zengine::scenes::IsScene(path)) { status_=L"Double-click a scene asset to open it."; InvalidateRect(window_,&statusBar_,FALSE); return; }
    if (zengine::materials::IsMaterial(path))
    {
        const auto relative=std::filesystem::relative(path,assetsDirectory_).generic_u8string();
        const std::string asset(reinterpret_cast<const char*>(relative.data()),relative.size());
        // A scene-tree model row, or the selected object when dropped on the Inspector.
        if (!AssignMaterialToObject(ScriptDropTarget(point),asset))
            status_=L"Drop a material onto a model's scene-tree row (or select the model and drop on its Inspector).";
        InvalidateRect(window_,nullptr,FALSE);
        return;
    }
    {
        const auto kind=assetLibrary::Type(path);
        if(kind==assetLibrary::Kind::Image || path.extension()==L".zvid")
        {
            POINT screen=point;ClientToScreen(window_,&screen);
            const auto relative=std::filesystem::relative(path,assetsDirectory_).generic_u8string();
            const std::string asset(reinterpret_cast<const char*>(relative.data()),relative.size());
            if(inspectorPanel_ && inspectorPanel_->AssignAssetPathAt(screen,asset))return;
        }
    }
    if (zengine::scripts::IsScript(path))
    {
        if (const auto id = ScriptDropTarget(point)) AttachScript(id, path);
        else { status_ = L"Drop a script onto a GameObject row or the selected object's Inspector."; InvalidateRect(window_, &statusBar_, FALSE); }
    }
    else if (const auto id = ScriptDropTarget(point)) QueueModel(path,id);
    else if (PtInRect(&viewportContent_, point)) QueueModel(path);
}

RECT EditorShell::CreateScriptRectangle() const
{
    return {mediaLibrary_.right-236, mediaLibrary_.top+4, mediaLibrary_.right-122, mediaLibrary_.top+26};
}
RECT EditorShell::CreateSceneRectangle() const
{
    return {mediaLibrary_.right-356,mediaLibrary_.top+4,mediaLibrary_.right-242,mediaLibrary_.top+26};
}
std::wstring EditorShell::SceneName() const
{
    if (!editingPrefab_.empty()) return editingPrefab_.stem().wstring()+L" (Prefab)";
    if (!sceneOpen_) return L"No scene open";
    return scenePath_.empty()?L"Untitled Scene":scenePath_.stem().wstring();
}
void EditorShell::UpdateSceneTitle()
{
    SetWindowTextW(window_,(L"zEngine Editor - "+(project_?WideText(project_->config.name):L"No project")+L" - "+SceneName()+(sceneDirty_?L" *":L"")).c_str());
    InvalidateRect(window_,&sceneBrowser_,FALSE);
}
void EditorShell::ConfigureScriptOutput()
{
    scriptHost_.SetPrintHandler([this](std::string_view text) {
        const auto message=std::string(text)+"\n"; OutputDebugStringA("[zEngine] "); OutputDebugStringA(message.c_str());
        status_=L"Console: "+WideText(std::string(text));
        if(consoleWindow_){const auto line=WideText(std::string(text)+"\r\n");const auto end=GetWindowTextLengthW(consoleWindow_);SendMessageW(consoleWindow_,EM_SETSEL,end,end);SendMessageW(consoleWindow_,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));}
        InvalidateRect(window_,&statusBar_,FALSE);
    });
}
void EditorShell::MarkSceneDirty()
{
    if (sceneOpen_ && !Playing()) { sceneDirty_=true; UpdateSceneTitle(); }
}
bool EditorShell::PendingModels(bool assignmentsOnly) const
{
    const auto pending=[&](const AssetJob& job) { return job.loadMesh && job.scene==sceneGeneration_ && (!assignmentsOnly || !job.restoreOnly); };
    return (activeAssetJob_ && pending(*activeAssetJob_)) || std::any_of(assetJobs_.begin(),assetJobs_.end(),pending);
}
bool EditorShell::ConfirmSceneClose()
{
    EndGizmoDrag(false);
    if (!sceneOpen_) return true;
    if (Playing()) throw std::runtime_error("Stop Play before switching scenes.");
    SetFocus(window_);
    // Compare actual authored data so canceling/reverting an edit doesn't cause a spurious prompt.
    sceneDirty_=zengine::scenes::Encode(CaptureDocument())!=sceneBaseline_;
    UpdateSceneTitle();
    if (!sceneDirty_ && !PendingModels(true)) return true;
    if (PendingModels(true)) throw std::runtime_error("Wait for pending model assignments before switching scenes.");
    const int answer=MessageBoxW(window_,(L"Save changes to "+SceneName()+L" before continuing?").c_str(),L"Unsaved scene",MB_YESNOCANCEL|MB_ICONQUESTION);
    return answer==IDNO || (answer==IDYES && SaveScene());
}
bool EditorShell::SaveScene(const std::filesystem::path& path)
{
    if (!editingPrefab_.empty()) return SavePrefab();
    EndGizmoDrag(false);
    RequireScene();
    if (Playing()) throw std::runtime_error("Stop Play before saving a scene.");
    if (PendingModels(true)) throw std::runtime_error("Wait for pending model assignments before saving the scene.");
    SetFocus(window_);
    if (path.empty() && scenePath_.empty()) return SaveSceneAs();
    const auto file=zengine::scenes::Resolve(assetsDirectory_,path.empty()?scenePath_:path);
    const auto data=zengine::scenes::Encode(CaptureDocument());
    // A different target is create-only. Save As asks explicitly before replacing an existing asset.
    zengine::scenes::Save(assetsDirectory_,file,data,file==scenePath_?&sceneSource_:nullptr);
    scenePath_=file; sceneSource_=data; sceneBaseline_=data; sceneDirty_=false;
    RefreshAssets(); UpdateSceneTitle(); status_=L"Saved scene: "+SceneName(); RememberProjectScene(); InvalidateRect(window_,nullptr,FALSE); return true;
}
bool EditorShell::SaveSceneAs()
{
    if (!editingPrefab_.empty()) return SavePrefab();
    EndGizmoDrag(false);
    RequireScene();
    if (Playing()) throw std::runtime_error("Stop Play before saving a scene.");
    if (PendingModels(true)) throw std::runtime_error("Wait for pending model assignments before saving the scene.");
    SetFocus(window_); std::filesystem::create_directories(assetsDirectory_);
    std::array<wchar_t,32768> filename{};
    const auto proposed=(assetsDirectory_/(SceneName()+L".zscene")).wstring();
    std::copy_n(proposed.c_str(),std::min(proposed.size(),filename.size()-1),filename.data());
    OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner=window_; dialog.lpstrFile=filename.data(); dialog.nMaxFile=static_cast<DWORD>(filename.size());
    dialog.lpstrFilter=L"zEngine scenes (*.zscene)\0*.zscene\0\0"; dialog.lpstrDefExt=L"zscene";
    dialog.lpstrTitle=L"Save scene in the current project's Assets directory"; dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return false;
    const auto file=zengine::scenes::Resolve(assetsDirectory_,filename.data());
    if (file==scenePath_) return SaveScene();
    const auto data=zengine::scenes::Encode(CaptureDocument());
    std::optional<std::string> expected;
    if (std::filesystem::exists(file)) expected=zengine::scenes::Load(file);
    zengine::scenes::Save(assetsDirectory_,file,data,expected?&*expected:nullptr);
    scenePath_=file; sceneSource_=data; sceneBaseline_=data; sceneDirty_=false;
    RefreshAssets(); UpdateSceneTitle(); status_=L"Saved scene: "+SceneName(); RememberProjectScene(); InvalidateRect(window_,nullptr,FALSE); return true;
}
bool EditorShell::NewScene()
{
    if (!editingPrefab_.empty() && !ClosePrefab()) return false;
    RequireProject();
    if (!ConfirmSceneClose()) return false;
    const auto file=zengine::scenes::Create(AssetFolder());
    const auto source=zengine::scenes::Load(file);
    ApplyScene(file,source,zengine::scenes::Decode(source));BeginAssetRename(file); return true;
}
bool EditorShell::OpenScene(const std::filesystem::path& path)
{
    if (!editingPrefab_.empty() && !ClosePrefab()) return false;
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before opening another scene.");
    // Read and validate before prompting or changing the active scene.
    const auto file=zengine::scenes::Resolve(assetsDirectory_,path);
    const auto source=zengine::scenes::Load(file);
    const auto scene=zengine::scenes::Decode(source);
    if (!ConfirmSceneClose()) return false;
    // Saving the old scene may have updated this same file while the prompt was open.
    const auto current=zengine::scenes::Load(file);
    ApplyScene(file,current,current==source?scene:zengine::scenes::Decode(current)); return true;
}
void EditorShell::ApplyScene(const std::filesystem::path& file,std::string source,const zengine::scenes::Document& scene)
{
    // An inline editor belongs to the scene that is about to be replaced.  A
    // delayed WM_KILLFOCUS must never rename an object in the newly opened scene.
    FinishRename(true);
    EndGizmoDrag(true);
    auto authored=scene;
    for(auto& object:authored.objects)if(!object.prefab.empty() && object.transformOverride && !object.transformMask){
        const auto sourcePrefab=zengine::prefabs::Load(assetsDirectory_,std::filesystem::path(std::u8string(object.prefab.begin(),object.prefab.end())));
        object.transformMask=TransformDifference(object.transform,sourcePrefab.objects.front().transform);object.transformOverride=object.transformMask!=0;
    }
    const auto expanded=zengine::prefabs::ResolveScene(assetsDirectory_,authored);
    auto next=zengine::scenes::Instantiate(expanded.scene); // Build first; bad data cannot destroy the current scene.
    collapsedObjects_.clear();
    inspectorPanel_->Bind(nullptr);
    ++sceneGeneration_;
    std::erase_if(assetJobs_,[](const AssetJob& job) { return job.loadMesh; });
    meshBindings_.clear(); meshRevisions_.clear(); materialCache_.clear(); lightmapBindings_.clear(); decalTextureCache_.clear();
    prefabLinks_.clear(); for (const auto& object:authored.objects) if (!object.prefab.empty()) {prefabLinks_[object.id]=object;if(auto* g=zengine::As3D(next.objects.Find(object.id)))prefabLinks_[object.id].transform=g->GetTransform();}
    prefabGenerated_=expanded.generated; prefabSources_=expanded.sources;
    scriptHost_=std::move(next.scripts); objects_=std::move(next.objects); scriptHost_.SetObjectStore(&objects_); ConfigureScriptOutput();
    scriptHost_.SetDataAssetIO( // ZE-129: rebind after the ScriptHost move
        [this](std::string_view rel)->std::string{ const std::filesystem::path rp(rel);
            if(zengine::datasheet::IsSheet(rp)) return zengine::datasheet::Encode(zengine::datasheet::Load(zengine::datasheet::Resolve(assetsDirectory_, rp)));
            return zengine::dataobj::Encode(zengine::dataobj::Load(zengine::dataobj::Resolve(assetsDirectory_, rp))); },
        [this](std::string_view rel, std::string_view text){ const std::filesystem::path wp(rel);
            if(zengine::datasheet::IsSheet(wp)) zengine::datasheet::Save(assetsDirectory_, wp, zengine::datasheet::Decode(text));
            else zengine::dataobj::Save(assetsDirectory_, wp, zengine::dataobj::Decode(text)); });
    scenePath_=file; sceneSource_=std::move(source); firstObject_=0; selectedObject_=0;
    sceneOpen_=true;
    status_=L"Opened scene: "+SceneName();
    PrepareScripts();
    for (std::size_t i=0;i<objects_.Size();++i)
    {
        auto& object=objects_.At(i);
        if (const auto* mesh=object.GetBehavior<zengine::MeshRenderer>(); mesh && !mesh->Asset().empty())
        {
            if (mesh->Asset()==zengine::MeshRenderer::CubeAsset)
            { if (renderer_) meshBindings_[object.Id()]={mesh->Asset(),renderer_->Cube()}; }
            else try
            {
                const auto model=ResolveModel(std::filesystem::path(WideText(mesh->Asset())));
                assetJobs_.push_back({model,true,object.Id(),++meshRevisions_[object.Id()],sceneGeneration_,true});
            }
            catch (const std::exception& e) { status_=L"Scene opened; model unavailable: "+WideText(e.what()); }
        }
    }
    if (objects_.Size()) SelectGameObject(objects_.At(0).Id());
    sceneBaseline_=zengine::scenes::Encode(CaptureDocument()); sceneDirty_=false;
    RefreshAssets(); RememberProjectScene(); UpdateSceneTitle(); InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::ChooseScene()
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before opening another scene.");
    ObjectPicker::Request request;
    request.title = L"Open a scene";
    request.items = AssetPickerItems({assetLibrary::Kind::Scene});
    request.current = scenePath_.empty() ? std::wstring{} : std::filesystem::relative(scenePath_, assetsDirectory_).generic_wstring();
    request.emptyText = L"This project has no .zscene files yet.";
    const auto choice = ObjectPicker::Window(window_, request);
    if (choice.picked) OpenScene(assetsDirectory_ / std::filesystem::path(choice.value));
}
bool EditorShell::TranslateShortcut(const MSG& message)
{
    if(inputEditor_ && (message.hwnd==inputEditor_->Window() || IsChild(inputEditor_->Window(),message.hwnd))) {
        if(message.message==WM_KEYDOWN && (GetKeyState(VK_CONTROL)&0x8000) && message.wParam=='S') {
            SendMessageW(inputEditor_->Window(),WM_COMMAND,InputMapEditor::Save,0);return true;
        }
        MSG copy=message;return IsDialogMessageW(inputEditor_->Window(),&copy)!=FALSE;
    }
    if(materialEditor_ && (message.hwnd==materialEditor_->Window() || IsChild(materialEditor_->Window(),message.hwnd))) {
        if(message.message==WM_KEYDOWN && (GetKeyState(VK_CONTROL)&0x8000) && message.wParam=='S') {
            SendMessageW(materialEditor_->Window(),WM_COMMAND,MAKEWPARAM(MaterialEditor::Save,BN_CLICKED),0);return true;
        }
        MSG copy=message;return IsDialogMessageW(materialEditor_->Window(),&copy)!=FALSE;
    }
    if(dataSheetEditor_ && (message.hwnd==dataSheetEditor_->Window() || IsChild(dataSheetEditor_->Window(),message.hwnd))) {
        if(message.message==WM_KEYDOWN && (GetKeyState(VK_CONTROL)&0x8000) && message.wParam=='S') {
            SendMessageW(dataSheetEditor_->Window(),WM_COMMAND,MAKEWPARAM(DataSheetEditor::Save,BN_CLICKED),0);return true;
        }
        MSG copy=message;return IsDialogMessageW(dataSheetEditor_->Window(),&copy)!=FALSE;
    }
    if (message.message!=WM_KEYDOWN || message.wParam!='S' || !(GetKeyState(VK_CONTROL)&0x8000) ||
        (message.hwnd!=window_ && !IsChild(window_,message.hwnd))) return false;
    SendMessageW(window_,WM_COMMAND,(GetKeyState(VK_SHIFT)&0x8000)?SaveSceneAsCommand:SaveSceneCommand,0); return true;
}
std::filesystem::path EditorShell::CreateScriptAsset()
{
    RequireProject();
    const auto path = zengine::scripts::Create(AssetFolder());
    RefreshAssets();
    selectedAsset_ = static_cast<int>(std::find(assets_.begin(),assets_.end(),path)-assets_.begin());
    const auto list=AssetListRectangle();const int visible=std::max(1,static_cast<int>(list.bottom-list.top)/84)*AssetColumns();firstAsset_=std::max(0,(selectedAsset_-visible+AssetColumns())/AssetColumns()*AssetColumns());
    status_ = L"Created " + path.filename().wstring() + L" - double-click to edit";
    InvalidateRect(window_, nullptr, FALSE);
    BeginAssetRename(path);
    return path;
}
void EditorShell::OpenScript(const std::filesystem::path& path)
{
    RequireProject();
    // Validate up front (throws for a script outside this project), then open it
    // inline in the Script tab rather than as a separate window.
    const auto resolved = zengine::scripts::Resolve(assetsDirectory_, path);
    OpenInlineScript(resolved);
    SetViewTab(ViewTab::Script);
}
std::filesystem::path EditorShell::CreateShaderAsset()
{
    RequireProject();
    const auto path = zengine::shaders::Create(AssetFolder());
    RefreshAssets();
    selectedAsset_ = static_cast<int>(std::find(assets_.begin(),assets_.end(),path)-assets_.begin());
    const auto list=AssetListRectangle();const int visible=std::max(1,static_cast<int>(list.bottom-list.top)/84)*AssetColumns();firstAsset_=std::max(0,(selectedAsset_-visible+AssetColumns())/AssetColumns()*AssetColumns());
    status_ = L"Created " + path.filename().wstring() + L" - double-click to edit its HLSL";
    InvalidateRect(window_, nullptr, FALSE);
    BeginAssetRename(path);
    return path;
}
void EditorShell::OpenShader(const std::filesystem::path& path)
{
    RequireProject();
    const auto resolved = zengine::shaders::Resolve(assetsDirectory_, path);
    OpenInlineScript(resolved); // the inline editor detects the .shader extension and switches to HLSL mode
    SetViewTab(ViewTab::Script);
}
std::filesystem::path EditorShell::CreateMaterialAsset()
{
    RequireProject();
    const auto path = zengine::materials::Create(AssetFolder());
    RefreshAssets();
    selectedAsset_ = static_cast<int>(std::find(assets_.begin(),assets_.end(),path)-assets_.begin());
    const auto list=AssetListRectangle();const int visible=std::max(1,static_cast<int>(list.bottom-list.top)/84)*AssetColumns();firstAsset_=std::max(0,(selectedAsset_-visible+AssetColumns())/AssetColumns()*AssetColumns());
    status_ = L"Created " + path.filename().wstring() + L" - assign it to a Model's Mesh Renderer, then tweak its values in the Inspector";
    InvalidateRect(window_, nullptr, FALSE);
    BeginAssetRename(path);
    return path;
}
namespace {
// ZE-128: the compiled program of the project script that declares data object `type`.
std::shared_ptr<const zengine::script::Program> ProgramDefiningStruct(
    const std::filesystem::path& assetsDir, const std::vector<std::wstring>& scriptPaths, const std::string& type)
{
    for (const auto& rel : scriptPaths)
    {
        std::string source;
        try { source = zengine::scripts::Load(assetsDir / rel); }
        catch (const std::exception&) { continue; }
        const auto result = zengine::script::Compiler::Compile(source, std::filesystem::path(rel).string());
        if (result && result.program->IsDataObject(type)) return result.program;
    }
    return nullptr;
}
}
std::vector<std::string> EditorShell::ProjectDataObjectTypes() const
{
    std::set<std::string> types;
    for (const auto& rel : ProjectScriptPaths())
    {
        std::string source;
        try { source = zengine::scripts::Load(assetsDirectory_ / rel); }
        catch (const std::exception&) { continue; }
        const auto result = zengine::script::Compiler::Compile(source, std::filesystem::path(rel).string());
        if (result) for (const auto& t : result.program->DataObjectTypes()) types.insert(t);
    }
    return {types.begin(), types.end()};
}
std::filesystem::path EditorShell::CreateDataObject(const std::string& structType)
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before creating a data object.");
    const auto path = zengine::dataobj::Create(AssetFolder(), structType);
    RefreshAssets();
    selectedAsset_ = static_cast<int>(std::find(assets_.begin(), assets_.end(), path) - assets_.begin());
    status_ = L"Created " + path.filename().wstring() + L" - double-click to edit its fields";
    InvalidateRect(window_, nullptr, FALSE);
    BeginAssetRename(path);
    return path;
}
void EditorShell::OpenDataObject(const std::filesystem::path& path)
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before editing a data object.");
    const auto file = zengine::dataobj::Resolve(assetsDirectory_, path);
    auto doc = std::make_shared<zengine::dataobj::DataDoc>(zengine::dataobj::Load(file));
    const auto program = ProgramDefiningStruct(assetsDirectory_, ProjectScriptPaths(), doc->type);
    if (!program) throw std::runtime_error("No script defines the data object 'struct " + doc->type + "'.");

    // Merge: one row per declared field, seeded from the file (or its default).
    std::vector<InspectorPanel::DataFieldRow> rows;
    std::vector<zengine::dataobj::FieldValue> merged;
    for (const auto& [name, type] : program->DataObjectFields(doc->type))
    {
        if (!zengine::dataobj::IsStorableType(type)) continue;
        std::string value;
        for (const auto& f : doc->fields) if (f.name == name) { value = f.value; break; }
        rows.push_back({name, type, value});
        merged.push_back({name, type, value});
    }
    doc->fields = std::move(merged);
    zengine::dataobj::Save(assetsDirectory_, file, *doc); // normalise the file to the current schema

    const auto root = assetsDirectory_;
    inspectorPanel_->BindDataObject(WideText(doc->type + "  (" + file.filename().string() + ")"), std::move(rows),
        [doc, file, root](const std::string& name, const std::string& value)
        {
            for (auto& f : doc->fields) if (f.name == name) { f.value = value; break; }
            try { zengine::dataobj::Save(root, file, *doc); } catch (const std::exception&) {}
        });
    selectedObject_ = 0;
    status_ = L"Editing data object " + file.filename().wstring();
    InvalidateRect(window_, nullptr, FALSE);
}
std::filesystem::path EditorShell::CreateDataSheet(const std::string& structType)
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before creating a data sheet.");
    const auto path = zengine::datasheet::Create(AssetFolder(), structType);
    RefreshAssets();
    selectedAsset_ = static_cast<int>(std::find(assets_.begin(), assets_.end(), path) - assets_.begin());
    status_ = L"Created " + path.filename().wstring() + L" - double-click to edit its rows";
    InvalidateRect(window_, nullptr, FALSE);
    BeginAssetRename(path);
    return path;
}
void EditorShell::OpenDataSheet(const std::filesystem::path& path)
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before editing a data sheet.");
    const auto file = zengine::datasheet::Resolve(assetsDirectory_, path);
    const auto doc = zengine::datasheet::Load(file);
    const auto program = ProgramDefiningStruct(assetsDirectory_, ProjectScriptPaths(), doc.type);
    if (!program) throw std::runtime_error("No script defines the data object 'struct " + doc.type + "'.");
    std::vector<std::pair<std::string, std::string>> columns;
    for (const auto& [name, type] : program->DataObjectFields(doc.type))
        if (zengine::dataobj::IsStorableType(type)) columns.push_back({name, type});

    if (dataSheetEditor_ && dataSheetEditor_->File() != file)
    {
        if (!dataSheetEditor_->ConfirmClose()) return;
        dataSheetEditor_.reset();
    }
    if (!dataSheetEditor_) dataSheetEditor_ = std::make_unique<DataSheetEditor>(window_, assetsDirectory_, file, std::move(columns));
    dataSheetEditor_->Show();
}
void EditorShell::OpenMaterial(const std::filesystem::path& path)
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before editing a material.");
    const auto file = zengine::materials::Resolve(assetsDirectory_, path);
    if (materialEditor_ && materialEditor_->File() != file)
    {
        if (!materialEditor_->ConfirmClose()) return;
        materialEditor_.reset();
    }
    if (!materialEditor_) materialEditor_ = std::make_unique<MaterialEditor>(window_, assetsDirectory_, file);
    materialEditor_->Show();
}
HWND EditorShell::OpenMaterialEditor(const std::filesystem::path& path)
{
    try { OpenMaterial(path); } catch (const std::exception&) { return nullptr; }
    return materialEditor_ ? materialEditor_->Window() : nullptr;
}
bool EditorShell::AssignMaterialToObject(zengine::GameObjectId id, const std::string& materialAsset)
{
    if (Playing()) return false;
    auto* object = id ? objects_.Find(id) : nullptr;
    auto* mesh = object ? object->GetBehavior<zengine::MeshRenderer>() : nullptr;
    if (!mesh) return false;
    mesh->SetMaterial(materialAsset);
    materialCache_.erase(materialAsset);
    SelectGameObject(id);
    if (sceneOpen_) { sceneDirty_ = true; UpdateSceneTitle(); }
    status_ = L"Assigned material to " + WideText(object->Name());
    return true;
}
std::filesystem::path EditorShell::BuildVideoClipFromImages()
{
    RequireProject();
    if (Playing()) throw std::runtime_error("Stop Play before building a video clip.");
    SetFocus(window_);
    std::array<wchar_t,32768> filename{};
    OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner=window_; dialog.lpstrFile=filename.data();
    dialog.nMaxFile=static_cast<DWORD>(filename.size());
    dialog.lpstrFilter=L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.gif\0\0";
    dialog.lpstrTitle=L"Pick any image in the frame-sequence folder (all images in it, sorted by name, become the clip)";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return {};
    const std::filesystem::path picked(filename.data());
    const auto folder=picked.parent_path();
    assetLibrary::Resolve(assetsDirectory_,folder); // must be inside the project

    auto isImage=[](const std::filesystem::path& p){
        auto e=p.extension().wstring(); std::transform(e.begin(),e.end(),e.begin(),::towlower);
        return e==L".png"||e==L".jpg"||e==L".jpeg"||e==L".bmp"||e==L".tif"||e==L".tiff"||e==L".gif";
    };
    std::vector<std::filesystem::path> images;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.is_regular_file() && isImage(entry.path())) images.push_back(entry.path());
    std::sort(images.begin(),images.end());
    if (images.size()<2) throw std::runtime_error("Need at least two images in the folder to build a clip.");
    if (images.size()>4096) throw std::runtime_error("A clip is limited to 4096 frames.");

    unsigned width=0,height=0;
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(images.size());
    for (const auto& image : images)
    {
        unsigned w=0,h=0;
        auto pixels=DecodeImageFileRGBA(image.wstring(),w,h);
        if (frames.empty()) { width=w; height=h; }
        else if (w!=width || h!=height)
            throw std::runtime_error("All frames must share one size; " + image.filename().string() + " differs.");
        if (static_cast<std::uint64_t>(width)*height>16u*1024*1024)
            throw std::runtime_error("Frames are too large for a clip.");
        frames.push_back(std::move(pixels));
    }
    const auto encoded=zengine::ui::VideoClip::Encode(static_cast<int>(width),static_cast<int>(height),12.0f,frames);

    const auto out=folder/(folder.filename().wstring()+L".zvid");
    { std::ofstream stream(out,std::ios::binary); stream.write(encoded.data(),static_cast<std::streamsize>(encoded.size()));
      if (!stream) throw std::runtime_error("Could not write the .zvid file."); }
    RefreshAssets();
    status_=L"Built "+out.filename().wstring()+L" ("+std::to_wstring(frames.size())+L" frames @ 12 fps) - assign it to a UI Video control";
    InvalidateRect(window_,nullptr,FALSE);
    return out;
}
zengine::materials::Effective EditorShell::ResolveMaterialEffective(const std::string& materialAsset) const
{
    if (materialAsset.empty()) return {};
    const auto file = zengine::materials::Resolve(assetsDirectory_, std::filesystem::u8path(materialAsset));
    const auto doc = zengine::materials::Load(file);
    return zengine::materials::Resolve(doc, [&](const std::string& shaderPath) {
        return zengine::shaders::Load(zengine::shaders::Resolve(assetsDirectory_, std::filesystem::u8path(shaderPath)));
    });
}
void EditorShell::ApplyMaterialValue(const std::string& materialAsset, zengine::materials::Value value)
{
    if (materialAsset.empty()) return;
    const auto file = zengine::materials::Resolve(assetsDirectory_, std::filesystem::u8path(materialAsset));
    auto doc = zengine::materials::Load(file);
    const auto it = std::find_if(doc.values.begin(), doc.values.end(),
        [&](const zengine::materials::Value& v) { return v.name == value.name; });
    if (it != doc.values.end()) *it = std::move(value); else doc.values.push_back(std::move(value));
    zengine::materials::Save(assetsDirectory_, file, doc);
    materialCache_.erase(materialAsset);
    InvalidateRect(window_, nullptr, FALSE);
}
TextureHandle EditorShell::ResolveDecalTexture(const std::string& asset) const
{
    if (asset.empty() || !renderer_) return {};
    if (const auto it = decalTextureCache_.find(asset); it != decalTextureCache_.end()) return it->second;
    TextureHandle handle;
    try { handle = renderer_->UploadImage(assetLibrary::Resolve(assetsDirectory_, std::filesystem::u8path(asset))); }
    catch (...) { handle = {}; }
    decalTextureCache_[asset] = handle;
    return handle;
}
MaterialHandle EditorShell::ResolveMaterial(const std::string& materialAsset) const
{
    if (materialAsset.empty() || !renderer_) return {};
    if (const auto it = materialCache_.find(materialAsset); it != materialCache_.end()) return it->second;
    MaterialHandle handle;
    try
    {
        const auto effective = ResolveMaterialEffective(materialAsset);
        const auto tint = effective.Numbers("tint", {{1, 1, 1, 1}});
        TextureHandle albedo;
        if (const auto texture = effective.Texture("albedo"); !texture.empty())
            try { albedo = renderer_->UploadImage(assetLibrary::Resolve(assetsDirectory_, std::filesystem::u8path(texture))); }
            catch (...) {}
        handle = renderer_->UploadMaterial(albedo, Float4{tint[0], tint[1], tint[2], tint[3]}, effective.lit,
                                           effective.Numbers("roughness", {{0.5f,0,0,0}})[0], effective.Numbers("specular", {{0,0,0,0}})[0]);
    }
    catch (...) { handle = {}; }
    materialCache_[materialAsset] = handle;
    return handle;
}
EditorShell::ResolvedMesh EditorShell::ResolveLightmapMesh(const zengine::GameObject& object,
                                                          const zengine::MeshRenderer& mesh, MeshHandle fallback) const
{
    if (!mesh.Static() || mesh.Lightmap().empty() || !renderer_) return {fallback, true};
    const auto it = lightmapBindings_.find(object.Id());
    if (it != lightmapBindings_.end() && it->second.lightmap == mesh.Lightmap())
        return it->second.mesh ? ResolvedMesh{it->second.mesh, false} : ResolvedMesh{fallback, true};
    MeshHandle handle;
    try
    {
        const auto file = zengine::lightmap::Resolve(assetsDirectory_, std::filesystem::u8path(mesh.Lightmap()));
        const auto doc = zengine::lightmap::Load(file);
        const auto* entry = doc.Find(object.Id());
        if (entry)
        {
            ModelData base = mesh.Asset() == zengine::MeshRenderer::CubeAsset
                ? UnitCubeModel()
                : FbxImporter::Load(ResolveModel(std::filesystem::path(WideText(mesh.Asset()))), true);
            if (entry->colors.size() == base.vertices.size())
            {
                std::vector<std::string> warnings;
                handle = renderer_->UploadModel(zengine::lightmap::Apply(std::move(base), *entry), warnings);
            }
        }
    }
    catch (...) { handle = {}; }
    lightmapBindings_[object.Id()] = {mesh.Lightmap(), handle};
    return handle ? ResolvedMesh{handle, false} : ResolvedMesh{fallback, true};
}
void EditorShell::BakeLightmaps()
{
    if (Playing()) { status_ = L"Stop Play before baking lightmaps."; InvalidateRect(window_, &statusBar_, FALSE); return; }
    if (!renderer_ || !sceneOpen_ || assetsDirectory_.empty()) return;
    if (scenePath_.empty()) { status_ = L"Save the scene before baking lightmaps."; InvalidateRect(window_, &statusBar_, FALSE); return; }

    std::vector<zengine::lightmap::BakeMesh> bakeMeshes;
    std::vector<zengine::lightmap::BakeLight> bakeLights;
    for (std::size_t i = 0; i < objects_.Size(); ++i)
    {
        const auto* object3d = zengine::As3D(&objects_.At(i)); if (!object3d) continue;
        const auto& object = *object3d;
        if (const auto* light = object.GetBehavior<zengine::Light>(); light && light->Enabled() && light->Static())
        {
            const LightData ld = MakeLight(*light, object.GetTransform(), ParentMatrix(objects_, object));
            bakeLights.push_back({ld.type, {ld.position.x, ld.position.y, ld.position.z},
                                  {ld.direction.x, ld.direction.y, ld.direction.z},
                                  {ld.color.x, ld.color.y, ld.color.z}, ld.intensity, ld.range, ld.falloff,
                                  ld.spotCosInner, ld.spotCosOuter});
        }
        const auto* mesh = object.GetBehavior<zengine::MeshRenderer>();
        if (!mesh || !mesh->Enabled() || !mesh->Static() || mesh->Asset().empty()) continue;
        ModelData model;
        try
        {
            model = mesh->Asset() == zengine::MeshRenderer::CubeAsset
                ? UnitCubeModel()
                : FbxImporter::Load(ResolveModel(std::filesystem::path(WideText(mesh->Asset()))), true);
        }
        catch (const std::exception& e) { status_ = L"Bake failed: " + WideText(e.what()); InvalidateRect(window_, &statusBar_, FALSE); return; }

        const DirectX::XMMATRIX world = TransformMatrix(object.GetTransform()) * ParentMatrix(objects_, object);
        zengine::lightmap::BakeMesh bm;
        bm.object = object.Id();
        bm.positions.reserve(model.vertices.size());
        bm.normals.reserve(model.vertices.size());
        for (const auto& v : model.vertices)
        {
            DirectX::XMFLOAT3 p, n;
            DirectX::XMStoreFloat3(&p, DirectX::XMVector3TransformCoord(
                DirectX::XMVectorSet(v.position.x, v.position.y, v.position.z, 1), world));
            DirectX::XMStoreFloat3(&n, DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(
                DirectX::XMVectorSet(v.normal.x, v.normal.y, v.normal.z, 0), world)));
            bm.positions.push_back({p.x, p.y, p.z});
            bm.normals.push_back({n.x, n.y, n.z});
        }
        bm.indices = model.indices;
        bakeMeshes.push_back(std::move(bm));
    }
    if (bakeMeshes.empty()) { status_ = L"Bake: no enabled static meshes in the scene."; InvalidateRect(window_, &statusBar_, FALSE); return; }

    const auto doc = zengine::lightmap::Bake(bakeMeshes, bakeLights);
    auto lightmapPath = scenePath_; lightmapPath.replace_extension(L".lightmap");
    try { zengine::lightmap::Save(assetsDirectory_, zengine::lightmap::Resolve(assetsDirectory_, lightmapPath), doc); }
    catch (const std::exception& e) { status_ = L"Bake failed: " + WideText(e.what()); InvalidateRect(window_, &statusBar_, FALSE); return; }

    const auto relative = std::filesystem::relative(lightmapPath, assetsDirectory_).generic_u8string();
    const std::string relativeStr(reinterpret_cast<const char*>(relative.data()), relative.size());
    for (const auto& bm : bakeMeshes)
        if (auto* object = objects_.Find(bm.object))
            if (auto* mesh = object->GetBehavior<zengine::MeshRenderer>()) mesh->SetLightmap(relativeStr);

    lightmapBindings_.clear();
    if (selectedObject_) inspectorPanel_->RefreshBehaviors();
    OnObjectChanged();
    RefreshAssets();
    status_ = L"Baked lighting for " + std::to_wstring(bakeMeshes.size()) + L" static mesh(es).";
    InvalidateRect(window_, nullptr, FALSE);
}
bool EditorShell::AttachScript(zengine::GameObjectId id, const std::filesystem::path& path)
{
    RequireEditable(id);
    if (Playing()) { status_=L"Stop Play before attaching scripts."; InvalidateRect(window_,&statusBar_,FALSE); return false; }
    auto* object = objects_.Find(id);
    if (!object) return false;
    const auto file = zengine::scripts::Resolve(assetsDirectory_,path);
    zengine::scripts::Load(file); // Reject missing, binary, or oversized assets before changing the object.
    const auto relative = std::filesystem::relative(file,assetsDirectory_).generic_u8string();
    const std::string asset(reinterpret_cast<const char*>(relative.data()), relative.size());
    for (std::size_t i = 0; i < object->BehaviorCount(); ++i)
        if (const auto* behavior = dynamic_cast<const zengine::ScriptBehavior*>(&object->BehaviorAt(i));
            behavior && _wcsicmp(WideText(behavior->Asset()).c_str(),WideText(asset).c_str()) == 0)
        { status_ = L"That script is already attached."; InvalidateRect(window_, &statusBar_, FALSE); return false; }
    object->AddBehavior<zengine::ScriptBehavior>(asset);
    MarkSceneDirty();
    SelectGameObject(id);
    status_ = L"Attached " + file.filename().wstring() + L" to " + WideText(object->Name()) + L" - press Play to run";
    PrepareScripts();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}
bool EditorShell::DropObjectOnInspector(POINT screenPoint, zengine::GameObjectId object)
{
    return inspectorPanel_ && inspectorPanel_->AssignObjectReferenceAt(screenPoint, object);
}
bool EditorShell::DropAssetPathOnInspector(POINT screenPoint, const std::string& asset)
{
    return inspectorPanel_ && inspectorPanel_->AssignAssetPathAt(screenPoint, asset);
}
HWND EditorShell::InspectorUiField(const std::string& key, int axis) const
{
    if (!inspectorPanel_ || !selectedObject_) return nullptr;
    const auto* object = objects_.Find(selectedObject_);
    if (!object) return nullptr;
    const auto* control = object->GetBehavior<zengine::ui::UiControl>();
    return control ? inspectorPanel_->FieldWindowForProperty(control, key, axis) : nullptr;
}
zengine::GameObjectId EditorShell::ScriptDropTarget(POINT point) const
{
    const auto list = ObjectListRectangle();
    if (PtInRect(&list,point))
    {
        const auto index = firstObject_ + (point.y-list.top)/27;
        const auto rows=ObjectRows();if(index>=0 && index<static_cast<LONG>(rows.size()))return rows[index];
    }
    if (PtInRect(&inspector_,point)) return selectedObject_;
    return 0; // No ambiguous picking: attach to an explicit tree row or selected Inspector.
}
std::vector<ObjectPicker::Item> EditorShell::AssetPickerItems(std::vector<assetLibrary::Kind> kinds) const
{
    std::vector<ObjectPicker::Item> items;
    if (!project_ || !std::filesystem::exists(assetsDirectory_)) return items;
    const auto wanted = [&](assetLibrary::Kind k) { return std::find(kinds.begin(), kinds.end(), k) != kinds.end(); };
    const auto walk = [&](auto&& self, const std::filesystem::path& folder) -> void {
        std::vector<std::filesystem::path> entries;
        try { entries = assetLibrary::List(assetsDirectory_, folder); } catch (const std::exception&) { return; }
        for (const auto& entry : entries)
        {
            const auto kind = assetLibrary::Type(entry);
            if (kind == assetLibrary::Kind::Folder) { self(self, std::filesystem::relative(entry, assetsDirectory_)); continue; }
            if (!wanted(kind) || items.size() >= 2000) continue;
            const auto storage = assetLibrary::Storage(entry);
            ObjectPicker::Item item;
            item.label = (kind == assetLibrary::Kind::Model ? entry.parent_path().filename() : entry.stem()).wstring();
            item.detail = std::filesystem::relative(storage, assetsDirectory_).generic_wstring();
            item.group = KindLabel(kind);
            item.value = std::filesystem::relative(entry, assetsDirectory_).generic_wstring();
            item.icon = kind;
            items.push_back(std::move(item));
        }
    };
    walk(walk, ".");
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.group != b.group ? a.group < b.group : _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return items;
}
std::optional<zengine::GameObjectId> EditorShell::PickSceneObject(const std::string& referenceType, zengine::GameObjectId current, RECT anchorScreen) const
{
    ObjectPicker::Request request;
    request.title = L"Assign " + WideText(referenceType);
    request.allowClear = true;
    request.current = std::to_wstring(current);
    request.emptyText = L"No scene object has a " + WideText(referenceType) + L".";
    for (const auto id : objects_.HierarchyOrder())
    {
        const auto* object = objects_.Find(id);
        if (!object || !zengine::ScriptHost::ObjectMatchesReferenceType(*object, referenceType)) continue;
        ObjectPicker::Item item;
        item.label = WideText(object->Name());
        item.detail = L"scene object";
        item.group = WideText(referenceType);
        item.value = std::to_wstring(id);
        item.icon = assetLibrary::Kind::Prefab;
        request.items.push_back(std::move(item));
    }
    const auto choice = ObjectPicker::Dropdown(window_, anchorScreen, request);
    if (choice.cleared) return zengine::GameObjectId{0};
    if (!choice.picked) return std::nullopt;
    try { return static_cast<zengine::GameObjectId>(std::stoull(choice.value)); }
    catch (const std::exception&) { return std::nullopt; }
}
void EditorShell::ChooseScript()
{
    RequireScene();
    ObjectPicker::Request request;
    request.title = L"Attach a script";
    request.items = AssetPickerItems({assetLibrary::Kind::Script});
    request.emptyText = L"This project has no .zsh scripts yet.";
    const auto choice = ObjectPicker::Window(window_, request);
    if (choice.picked) AttachScript(selectedObject_, assetsDirectory_ / std::filesystem::path(choice.value));
}
std::vector<std::wstring> EditorShell::ProjectScriptPaths() const
{
    std::vector<std::wstring> result;
    if (!project_ || !std::filesystem::exists(assetsDirectory_)) return result;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(assetsDirectory_,std::filesystem::directory_options::skip_permission_denied,error),end; it!=end && !error; it.increment(error))
    {
        if (result.size()>=400) break;
        if (!it->is_regular_file(error) || !zengine::scripts::IsScript(it->path())) continue;
        auto relative=std::filesystem::relative(it->path(),assetsDirectory_,error);
        if (!error) result.push_back(relative.generic_wstring());
    }
    std::sort(result.begin(),result.end());
    return result;
}
std::optional<std::string> EditorShell::ChoosePrefabReference(const std::string& current)
{
    RequireProject();
    ObjectPicker::Request request;
    request.title = L"Assign a prefab";
    request.items = AssetPickerItems({assetLibrary::Kind::Prefab});
    request.current = WideText(current);
    request.allowClear = true;
    request.emptyText = L"This project has no .zprefab files yet.";
    const auto choice = ObjectPicker::Window(window_, request);
    if (choice.cleared) return std::string{};
    if (!choice.picked) return std::nullopt;
    return Utf8Text(choice.value);
}
zengine::GameObjectId EditorShell::SpawnPrefab(std::string_view asset)
{
    if(!Playing())throw std::runtime_error("Prefabs can only be spawned while a scene is playing.");
    const auto file=zengine::prefabs::Resolve(assetsDirectory_,std::filesystem::path(std::u8string(asset.begin(),asset.end())));
    const auto expanded=zengine::prefabs::ResolveScene(assetsDirectory_,zengine::prefabs::Load(assetsDirectory_,file));
    const auto first=objects_.Size();const auto root=zengine::scenes::Append(expanded.scene,objects_,scriptHost_);
    for(std::size_t i=first;i<objects_.Size();++i)
    {
        auto& object=objects_.At(i);
        if(const auto* mesh=object.GetBehavior<zengine::MeshRenderer>();mesh&&!mesh->Asset().empty()){
            if(mesh->Asset()==zengine::MeshRenderer::CubeAsset){if(renderer_)meshBindings_[object.Id()]={mesh->Asset(),renderer_->Cube()};}
            else {const auto& value=mesh->Asset();const auto model=ResolveModel(std::filesystem::path(std::u8string(value.begin(),value.end())));assetJobs_.push_back({model,true,object.Id(),++meshRevisions_[object.Id()],sceneGeneration_,true});}
        }
        for(std::size_t j=0;j<object.BehaviorCount();++j)if(auto* behavior=dynamic_cast<zengine::ScriptBehavior*>(&object.BehaviorAt(j))){
            const auto& value=behavior->Asset();const auto script=zengine::scripts::Resolve(assetsDirectory_,std::filesystem::path(std::u8string(value.begin(),value.end())));
            // A broken spawned script must not fault the script that spawned it: report it,
            // leave the behavior inert, and let the rest of the prefab run.
            try { if(!scriptHost_.Prepare(*behavior,zengine::scripts::Load(script),Utf8Text(script.stem().wstring())))
                { status_=L"Spawned "+file.filename().wstring()+L": "+WideText(scriptHost_.Error(*behavior)); InvalidateRect(window_,&statusBar_,FALSE); } }
            catch(const std::exception& error) { status_=L"Spawned "+file.filename().wstring()+L": "+WideText(error.what()); InvalidateRect(window_,&statusBar_,FALSE); }
        }
    }
    status_=L"Spawned prefab: "+file.filename().wstring();InvalidateRect(window_,&sceneBrowser_,FALSE);return root;
}
bool EditorShell::AddMeshRenderer(zengine::GameObjectId id)
{
    RequireEditable(id);
    if (Playing()) throw std::runtime_error("Stop Play before adding behaviors.");
    auto* object = objects_.Find(id);
    if (!object || object->GetBehavior<zengine::MeshRenderer>()) return false;
    object->AddBehavior<zengine::MeshRenderer>();
    MarkSceneDirty();
    if (id == selectedObject_) inspectorPanel_->RefreshBehaviors();
    status_ = L"Added Mesh Renderer - assign an imported model or use the built-in cube";
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}
void EditorShell::AssignCube(zengine::GameObjectId id)
{
    if (Playing()) throw std::runtime_error("Stop Play before changing model assignments.");
    auto* object = objects_.Find(id);
    if (!object || !renderer_) throw std::runtime_error("Select an object and initialize the renderer first.");
    AddMeshRenderer(id);
    ++meshRevisions_[id];
    meshBindings_[id] = {zengine::MeshRenderer::CubeAsset,renderer_->Cube()};
    object->GetBehavior<zengine::MeshRenderer>()->SetAsset(zengine::MeshRenderer::CubeAsset);
    MarkSceneDirty();
    if (id == selectedObject_) inspectorPanel_->RefreshBehaviors();
    status_ = L"Assigned built-in cube";
    InvalidateRect(window_, nullptr, FALSE);
}
void EditorShell::ClearMesh(zengine::GameObjectId id)
{
    RequireEditable(id);
    if (Playing()) throw std::runtime_error("Stop Play before changing model assignments.");
    auto* object = objects_.Find(id);
    auto* mesh = object ? object->GetBehavior<zengine::MeshRenderer>() : nullptr;
    if (!mesh) return;
    ++meshRevisions_[id];
    mesh->SetAsset({}); meshBindings_.erase(id);
    MarkSceneDirty();
    if (id == selectedObject_) inspectorPanel_->RefreshBehaviors();
    status_ = L"Cleared model; Mesh Renderer remains attached";
    InvalidateRect(window_, nullptr, FALSE);
}
std::filesystem::path EditorShell::ResolveModel(const std::filesystem::path& path) const
{
    const auto file = assetLibrary::Resolve(assetsDirectory_,path);
    if (_wcsicmp(file.filename().c_str(),L"model.fbx") != 0 ||
        !std::filesystem::is_regular_file(file) || !std::filesystem::is_regular_file(file.parent_path()/"asset.ready"))
        throw std::runtime_error("Choose an imported model.fbx from this project's Assets library. Import external FBX files first.");
    return file;
}
void EditorShell::QueueModel(const std::filesystem::path& path, zengine::GameObjectId id)
{
    if (id) RequireEditable(id);
    RequireScene();
    if (Playing()) throw std::runtime_error("Stop Play before changing model assignments.");
    if (id && !objects_.Find(id)) throw std::runtime_error("Unknown target GameObject.");
    const auto file = ResolveModel(path);
    assetJobs_.push_back({file,true,id,id ? ++meshRevisions_[id] : 0,sceneGeneration_,false});
    status_ = L"Queued model assignment";
    InvalidateRect(window_, &statusBar_, FALSE);
}
void EditorShell::ChooseModel()
{
    ObjectPicker::Request request;
    request.title = L"Choose a model";
    request.items = AssetPickerItems({assetLibrary::Kind::Model});
    const auto* mesh = selectedObject_ ? objects_.Find(selectedObject_) : nullptr;
    if (mesh) if (const auto* renderer = mesh->GetBehavior<zengine::MeshRenderer>(); renderer && !renderer->Asset().empty())
        request.current = WideText(renderer->Asset());
    request.emptyText = L"Import an FBX model into the library first.";
    const auto choice = ObjectPicker::Window(window_, request);
    if (choice.picked) QueueModel(std::filesystem::path(choice.value), selectedObject_);
}
bool EditorShell::ConfirmScriptClose()
{
    if(inputEditor_ && !inputEditor_->ConfirmClose())return false;
    if(materialEditor_ && !materialEditor_->ConfirmClose())return false;
    if(inlineEditor_ && !inlineEditor_->ConfirmClose())return false;
    for (const auto& editor : scriptEditors_) if (!editor->ConfirmClose()) return false;
    return true;
}

void EditorShell::UpdateDrag(const POINT position)
{
    if (dragTarget_ == DragTarget::None)
    {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    switch (dragTarget_)
    {
    case DragTarget::SceneBrowser:
        sceneBrowserWidth_ = position.x;
        break;
    case DragTarget::Inspector:
        inspectorWidth_ = client.right - position.x;
        break;
    case DragTarget::MediaLibrary:
        mediaLibraryHeight_ = statusBar_.top - position.y;
        break;
    default:
        break;
    }
    Layout(static_cast<std::uint32_t>(std::max(0L, client.right)),
           static_cast<std::uint32_t>(std::max(0L, client.bottom)));
}

void EditorShell::EndDrag()
{
    if (dragTarget_ != DragTarget::None)
    {
        dragTarget_ = DragTarget::None;
        ReleaseCapture();
    }
}

LRESULT CALLBACK EditorShell::WindowProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    EditorShell* editor = reinterpret_cast<EditorShell*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<EditorShell*>(creation->lpCreateParams);
        editor->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    try
    {
        return editor ? editor->HandleMessage(window, message, wParam, lParam)
                      : DefWindowProcW(window, message, wParam, lParam);
    }
    catch (const std::exception& error)
    {
        // Never let C++ exceptions cross a Windows callback boundary.
        if (editor)
        {
            editor->status_ = L"Editor error: " + WideText(error.what());
            InvalidateRect(window, &editor->statusBar_, FALSE);
        }
        return 0;
    }
}

LRESULT CALLBACK EditorShell::ViewportProcedure(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    auto* editor=reinterpret_cast<EditorShell*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE)
    {
        editor=static_cast<EditorShell*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(editor));
    }
    try { return editor?editor->HandleViewportMessage(window,message,wParam,lParam):DefWindowProcW(window,message,wParam,lParam); }
    catch (const std::exception& error)
    {
        if (editor) { editor->EndGizmoDrag(true); editor->status_=L"Transform tool error: "+WideText(error.what()); InvalidateRect(editor->window_,nullptr,FALSE); }
        return 0;
    }
}

LRESULT EditorShell::HandleMessage(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        if(buildWork_.valid())throw std::runtime_error("Wait for the game build to finish before closing the editor.");
        EndGizmoDrag(true);
        if (!editingPrefab_.empty() && !ClosePrefab()) return 0;
        if (ConfirmScriptClose()) { if (Playing()) Stop(); if (ConfirmSceneClose()) DestroyWindow(window); }
        return 0;
    case WM_CTLCOLORLISTBOX:
        return editorStyle::ControlColor(message, wParam);
    case WM_CTLCOLORSTATIC:
        if (consoleWindow_ && reinterpret_cast<HWND>(lParam) == consoleWindow_) return editorStyle::ControlColor(WM_CTLCOLORLISTBOX, wParam);
        break;
    case WM_COMMAND:
        if(LOWORD(wParam)>=AddEmptyCommand&&LOWORD(wParam)<=AddAreaObjectCommand){CreateGameObject(static_cast<ObjectPreset>(LOWORD(wParam)-AddEmptyCommand),selectedObject_);return 0;}
        if(LOWORD(wParam)==AddAudioPlayerCommand){CreateGameObject(ObjectPreset::AudioPlayer,selectedObject_);return 0;}
        if(LOWORD(wParam)==AddDecalCommand){CreateGameObject(ObjectPreset::Decal,selectedObject_);return 0;}
        if(LOWORD(wParam)>=AddLightBase&&LOWORD(wParam)<=AddLightLast){
            const int k=LOWORD(wParam)-AddLightBase;
            CreateGameObject(k==0?ObjectPreset::PointLight:k==2?ObjectPreset::SpotLight:ObjectPreset::DirectionalLight,selectedObject_);
            return 0;
        }
        if(HIWORD(wParam)==0&&LOWORD(wParam)>=AddUiControlBase&&LOWORD(wParam)<=AddUiControlLast){const auto& types=zengine::ui::UiControlTypes();const std::size_t index=LOWORD(wParam)-AddUiControlBase;if(index<types.size())CreateUiControl(types[index],selectedObject_);return 0;}
        if(LOWORD(wParam)==CopyObjectCommand){if(selectedObject_)CopyGameObject(selectedObject_);return 0;}
        if(LOWORD(wParam)==PasteObjectCommand){PasteGameObject(selectedObject_);return 0;}
        if(LOWORD(wParam)==DeleteObjectCommand){if(selectedObject_)DeleteGameObject(selectedObject_);return 0;}
        if(LOWORD(wParam)==RenameObjectCommand){if(selectedObject_)BeginObjectRename(selectedObject_);return 0;}
        if(LOWORD(wParam)==RenameAssetCommand){if(selectedAsset_>=0&&selectedAsset_<static_cast<int>(assets_.size()))BeginAssetRename(assets_[selectedAsset_]);return 0;}
        if(LOWORD(wParam)==DeleteAssetCommand){if(selectedAsset_>=0&&selectedAsset_<static_cast<int>(assets_.size()))try{DeleteAsset(assets_[selectedAsset_]);}catch(const std::exception& e){status_=WideText(e.what());InvalidateRect(window_,&statusBar_,FALSE);}return 0;}
        if(LOWORD(wParam)==UnparentCommand){if(selectedObject_)SetObjectParent(selectedObject_,0);return 0;}
        if(LOWORD(wParam)==RevertPrefabTransformCommand){if(selectedObject_)RevertPrefabTransform(selectedObject_);return 0;}
        if(LOWORD(wParam)==BuildProjectCommand){ChooseBuildFolder();return 0;}
        if(LOWORD(wParam)==BakeLightmapsCommand){BakeLightmaps();return 0;}
        if(LOWORD(wParam)==ToggleStaticMeshCommand){
            if(auto* o=selectedObject_?objects_.Find(selectedObject_):nullptr)
                if(auto* mr=o->GetBehavior<zengine::MeshRenderer>()){
                    const bool on=!mr->Static();mr->SetStatic(on);if(!on)mr->SetLightmap({});
                    lightmapBindings_.erase(selectedObject_);OnObjectChanged();
                    status_=on?L"Marked static for lightmapping.":L"Cleared static flag.";
                }
            return 0;
        }
        if(LOWORD(wParam)==NewFolderCommand){NewAssetFolderDialog();return 0;}
        if(LOWORD(wParam)==UpFolderCommand){if(AssetFolder()!=assetsDirectory_)OpenAssetFolder(AssetFolder().parent_path());return 0;}
        if (LOWORD(wParam)==SavePrefabCommand) { SavePrefab(); return 0; }
        if (LOWORD(wParam)==ClosePrefabCommand) { ClosePrefab(); return 0; }
        if (LOWORD(wParam)>=MoveToolCommand && LOWORD(wParam)<=ScaleToolCommand) { SetTransformTool(static_cast<gizmo::Mode>(LOWORD(wParam)-MoveToolCommand)); return 0; }
        if (LOWORD(wParam)==NewProjectCommand) { NewProjectDialog(); return 0; }
        if (LOWORD(wParam)==OpenProjectCommand) { ChooseProject(); return 0; }
        if (LOWORD(wParam)==NewSceneCommand) { NewScene(); return 0; }
        if (LOWORD(wParam)==SaveSceneCommand) { SaveScene(); return 0; }
        if (LOWORD(wParam)==SaveSceneAsCommand) { SaveSceneAs(); return 0; }
        if (LOWORD(wParam)==OpenSceneCommand) { ChooseScene(); return 0; }
        if (LOWORD(wParam)==ScriptListControl && (HIWORD(wParam)==LBN_SELCHANGE || HIWORD(wParam)==LBN_DBLCLK))
        {
            const auto row=SendMessageW(scriptListBox_,LB_GETCURSEL,0,0);
            if (row>=0 && row<static_cast<LRESULT>(scriptTabPaths_.size())) OpenInlineScript(scriptTabPaths_[row]);
            return 0;
        }
        if (LOWORD(wParam)==FunctionListControl && (HIWORD(wParam)==LBN_SELCHANGE || HIWORD(wParam)==LBN_DBLCLK))
        {
            const auto row=SendMessageW(functionListBox_,LB_GETCURSEL,0,0);
            if (inlineEditor_ && row>=0 && row<static_cast<LRESULT>(functionOffsets_.size())) inlineEditor_->GoTo(functionOffsets_[row]);
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
    {
        POINT screen{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}, point=screen;
        ScreenToClient(window_, &point);
        if(PtInRect(&sceneBrowser_,point)) {
            const auto target=ScriptDropTarget(point);if(target)SelectGameObject(target);else {selectedObject_=0;inspectorPanel_->Bind(nullptr);InvalidateRect(window_,&sceneBrowser_,FALSE);}
            HMENU menu=CreatePopupMenu(),add=CreatePopupMenu(),physics=CreatePopupMenu(),ui=CreatePopupMenu();const UINT addFlags=MF_STRING|((Playing()||!sceneOpen_||(target&&!CanEdit(target)))?MF_GRAYED:0);
            AppendMenuW(add,addFlags,AddEmptyCommand,L"Empty GameObject");AppendMenuW(add,addFlags,AddCubeCommand,L"Cube");AppendMenuW(add,addFlags,AddCameraCommand,L"Camera");AppendMenuW(add,addFlags,AddAudioPlayerCommand,L"Audio Player");AppendMenuW(add,addFlags,AddDecalCommand,L"Decal");
            {HMENU lights=CreatePopupMenu();AppendMenuW(lights,addFlags,AddLightBase+0,L"Point");AppendMenuW(lights,addFlags,AddLightBase+1,L"Directional");AppendMenuW(lights,addFlags,AddLightBase+2,L"Spot");AppendMenuW(add,MF_POPUP,reinterpret_cast<UINT_PTR>(lights),L"Light");}
            AppendMenuW(physics,addFlags,AddRigidCommand,L"Rigid Body + Collider");AppendMenuW(physics,addFlags,AddKinematicCommand,L"Kinematic Body + Collider");AppendMenuW(physics,addFlags,AddStaticCommand,L"Static Body + Collider");AppendMenuW(physics,addFlags,AddAreaObjectCommand,L"Area + Collider");
            {int uid=AddUiControlBase;for(const auto& uiType:zengine::ui::UiControlTypes()){const std::wstring label(uiType.begin(),uiType.end());AppendMenuW(ui,addFlags,static_cast<UINT>(uid++),label.c_str());}}
            AppendMenuW(add,MF_POPUP,reinterpret_cast<UINT_PTR>(physics),L"Physics");AppendMenuW(add,MF_POPUP,reinterpret_cast<UINT_PTR>(ui),L"UI");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(add),target?L"Add Child":L"Add");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            const UINT objectFlags=MF_STRING|((Playing()||!target||!CanEdit(target))?MF_GRAYED:0);const UINT pasteFlags=MF_STRING|((Playing()||!objectClipboard_||(target&&!CanEdit(target)))?MF_GRAYED:0);
            AppendMenuW(menu,objectFlags,CopyObjectCommand,L"Copy");AppendMenuW(menu,pasteFlags,PasteObjectCommand,target?L"Paste as Child":L"Paste");AppendMenuW(menu,objectFlags,DeleteObjectCommand,L"Delete");AppendMenuW(menu,objectFlags,RenameObjectCommand,L"Rename");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);const UINT transformFlags=MF_STRING|((Playing()||!target||!CanEdit(target,true))?MF_GRAYED:0);
            AppendMenuW(menu,transformFlags,UnparentCommand,L"Move to Scene Root");
            AppendMenuW(menu,transformFlags|(!prefabLinks_.contains(target)?MF_GRAYED:0),RevertPrefabTransformCommand,L"Revert Prefab Transform Overrides");
            if(const auto* t=target?zengine::As3D(objects_.Find(target)):nullptr){
                if(const auto* mr=t->GetBehavior<zengine::MeshRenderer>()){
                    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
                    AppendMenuW(menu,MF_STRING|(Playing()?MF_GRAYED:0)|(mr->Static()?MF_CHECKED:0),ToggleStaticMeshCommand,L"Static (Lightmapped Mesh)");
                }
            }
            const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,0,window_,nullptr);DestroyMenu(menu);if(command)SendMessageW(window_,WM_COMMAND,command,0);return 0;
        }
        if (!PtInRect(&mediaLibrary_, point)) break;
        const int assetIndex=AssetAt(point);selectedAsset_=assetIndex;InvalidateRect(window_,&mediaLibrary_,FALSE);
        HMENU menu=CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Create Behavior Script (.zsh)");
        AppendMenuW(menu, MF_STRING, 3, L"Create Material Shader (.shader)");
        AppendMenuW(menu, MF_STRING, 4, L"Create Material (.material)");
        { // ZE-128: Create Data Object > <struct type>
            const auto types = Playing() ? std::vector<std::string>{} : ProjectDataObjectTypes();
            HMENU sub = CreatePopupMenu();
            for (std::size_t i = 0; i < types.size() && i < 90; ++i)
                AppendMenuW(sub, MF_STRING, 6000 + static_cast<UINT>(i), WideText(types[i]).c_str());
            AppendMenuW(menu, MF_POPUP|((Playing()||types.empty())?MF_GRAYED:0), reinterpret_cast<UINT_PTR>(sub),
                        types.empty() ? L"Create Data Object (define a `struct` first)" : L"Create Data Object (.zdata)");
            HMENU sheet = CreatePopupMenu(); // ZE-92: Create Data Sheet > <struct type>
            for (std::size_t i = 0; i < types.size() && i < 90; ++i)
                AppendMenuW(sheet, MF_STRING, 6100 + static_cast<UINT>(i), WideText(types[i]).c_str());
            AppendMenuW(menu, MF_POPUP|((Playing()||types.empty())?MF_GRAYED:0), reinterpret_cast<UINT_PTR>(sheet),
                        L"Create Data Sheet (.zsheet)");
        }
        AppendMenuW(menu, MF_STRING|(Playing()?MF_GRAYED:0), 5, L"Build Video Clip (.zvid) from Images...");
        AppendMenuW(menu, MF_STRING, 2, L"Refresh Assets");
        AppendMenuW(menu,MF_STRING|(Playing()?MF_GRAYED:0),NewFolderCommand,L"New Folder...");
        AppendMenuW(menu, MF_STRING|(Playing()?MF_GRAYED:0),NewSceneCommand,L"Create Scene (.zscene)");
        if(assetIndex>=0){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);const UINT protect=(Playing()||assetLibrary::Type(assets_[assetIndex])==assetLibrary::Kind::Input)?MF_GRAYED:0u;AppendMenuW(menu,MF_STRING|protect,RenameAssetCommand,L"Rename");AppendMenuW(menu,MF_STRING|protect,DeleteAssetCommand,L"Delete");}
        const auto command=TrackPopupMenu(menu, TPM_RETURNCMD|TPM_RIGHTBUTTON, screen.x,screen.y,0,window_,nullptr);
        DestroyMenu(menu);
        if (command == 1) CreateScriptAsset();
        if (command == 3) CreateShaderAsset();
        if (command == 4) CreateMaterialAsset();
        if (command >= 6000 && command < 6090) { const auto types=ProjectDataObjectTypes(); const std::size_t i=command-6000;
            if (i<types.size()) try { CreateDataObject(types[i]); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } }
        if (command >= 6100 && command < 6190) { const auto types=ProjectDataObjectTypes(); const std::size_t i=command-6100;
            if (i<types.size()) try { CreateDataSheet(types[i]); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } }
        if (command == 5) { try { BuildVideoClipFromImages(); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } }
        if (command == 2) { RefreshAssets(); InvalidateRect(window_, &mediaLibrary_, FALSE); }
        if (command==NewSceneCommand) NewScene();
        if(command==NewFolderCommand)NewAssetFolderDialog();
        if(command==RenameAssetCommand)BeginAssetRename(assets_[assetIndex]);
        if(command==DeleteAssetCommand && assetIndex>=0 && assetIndex<static_cast<int>(assets_.size()))
            try { DeleteAsset(assets_[assetIndex]); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        const POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        const auto index=AssetAt(point);
        if(index>=0)
        {
            if (index >= 0 && index < static_cast<LONG>(assets_.size()))
            {
                const auto asset=assets_[index];
                if(std::filesystem::is_directory(asset))OpenAssetFolder(asset);
                else if (asset.extension()==L".zinput") OpenInputMap();
                else if (zengine::scripts::IsScript(asset)) OpenScript(asset);
                else if (zengine::shaders::IsShader(asset)) OpenShader(asset);
                else if (zengine::materials::IsMaterial(asset)) OpenMaterial(asset);
                else if (zengine::dataobj::IsData(asset)) { try { OpenDataObject(asset); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } }
                else if (zengine::datasheet::IsSheet(asset)) { try { OpenDataSheet(asset); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } }
                else if (zengine::prefabs::IsPrefab(asset)) OpenPrefab(asset);
                else if (zengine::scenes::IsScene(asset)) OpenScene(asset);
            }
        }
        return 0;
    }
    case WM_DROPFILES:
        ReceiveFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            requestedViewportWidth_ = 0;
            requestedViewportHeight_ = 0;
        }
        else
        {
            Layout(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto* sizing = reinterpret_cast<MINMAXINFO*>(lParam);
        sizing->ptMinTrackSize.x = 800;
        sizing->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    {
        if (draggedObject_) { draggedObject_=0; ReleaseCapture(); }
        SetFocus(window_);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (const int tab=ViewTabHit(point); tab>=0)
        {
            SetViewTab(static_cast<ViewTab>(tab));
            return 0;
        }
        hoveredChrome_=ChromeHit(point);pressedChrome_=hoveredChrome_;
        if(pressedChrome_>=0){InvalidateRect(window_,nullptr,FALSE);if(!ChromeEnabled(pressedChrome_)){pressedChrome_=-1;return 0;}}
        if(pressedChrome_==10){SendMessageW(window_,WM_COMMAND,UpFolderCommand,0);return 0;}
        if(pressedChrome_==11){showFps_=!showFps_;InvalidateRect(window_,&optionsBar_,FALSE);return 0;}
        const RECT assetRoot{mediaLibrary_.left+12,mediaLibrary_.top+PanelHeaderHeight+12,mediaLibrary_.left+150,mediaLibrary_.top+PanelHeaderHeight+36};
        if(project_ && PtInRect(&assetRoot,point)){OpenAssetFolder(assetsDirectory_);return 0;}
        for (int i=0;i<3;++i) { const auto rectangle=ToolRectangle(i); if (PtInRect(&rectangle,point)) { SetTransformTool(static_cast<gizmo::Mode>(i)); return 0; } }
        const RECT fileMenu{44,optionsBar_.top,99,optionsBar_.bottom};
        if (PtInRect(&fileMenu,point))
        {
            HMENU menu=CreatePopupMenu(); const UINT projectFlags=MF_STRING|(Playing()?MF_GRAYED:0);
            AppendMenuW(menu,projectFlags,NewProjectCommand,L"New Project..."); AppendMenuW(menu,projectFlags,OpenProjectCommand,L"Open Project...");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            const UINT flags=MF_STRING|((Playing() || !project_)?MF_GRAYED:0);
            AppendMenuW(menu,flags,NewSceneCommand,L"New Scene"); AppendMenuW(menu,flags,OpenSceneCommand,L"Open Scene...");
            const UINT saveFlags=flags|(!sceneOpen_?MF_GRAYED:0);
            AppendMenuW(menu,saveFlags,SaveSceneCommand,L"Save Scene\tCtrl+S"); AppendMenuW(menu,saveFlags,SaveSceneAsCommand,L"Save Scene As...\tCtrl+Shift+S");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,saveFlags|((Building()||!editingPrefab_.empty())?MF_GRAYED:0),BuildProjectCommand,L"Build Standalone Game...");
            AppendMenuW(menu,saveFlags|((Playing()||!editingPrefab_.empty())?MF_GRAYED:0),BakeLightmapsCommand,L"Bake Static Lightmaps");
            if (!editingPrefab_.empty()) { AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,SavePrefabCommand,L"Save Prefab\tCtrl+S"); AppendMenuW(menu,MF_STRING,ClosePrefabCommand,L"Close Prefab / Return to Scene"); }
            POINT at{44,optionsBar_.bottom}; ClientToScreen(window_,&at);
            const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,at.x,at.y,0,window_,nullptr); DestroyMenu(menu);
            if (command) SendMessageW(window_,WM_COMMAND,command,0); return 0;
        }
        const int centerX=(viewportPanel_.left+viewportPanel_.right)/2;
        const RECT play{centerX-42,viewportPanel_.top+4,centerX-14,viewportPanel_.top+26};
        const RECT pause{centerX-12,viewportPanel_.top+4,centerX+16,viewportPanel_.top+26};
        const RECT step{centerX+18,viewportPanel_.top+4,centerX+46,viewportPanel_.top+26};
        if (PtInRect(&play,point)) { if (Playing()) Stop(); else Play(); return 0; }
        if (PtInRect(&pause,point)) { SetPaused(!paused_); return 0; }
        if (PtInRect(&step,point)) { Step(); return 0; }
        const RECT create = CreateObjectRectangle(), list = ObjectListRectangle();
        if (PtInRect(&create, point)) { CreateEmptyGameObject(); return 0; }
        if (PtInRect(&list, point))
        {
            const auto index = firstObject_ + (point.y - list.top) / 27;
            const auto rows=ObjectRows();
            if (index >= 0 && index < static_cast<LONG>(rows.size()))
            {
                const auto id=rows[index];const int arrow=list.left+12+ObjectDepth(id)*12;
                if(HasChildren(id) && point.x>=arrow && point.x<arrow+12){if(!collapsedObjects_.erase(id))collapsedObjects_.insert(id);firstObject_=std::min(firstObject_,std::max(0,static_cast<int>(ObjectRows().size())-1));InvalidateRect(window_,&sceneBrowser_,FALSE);return 0;}
                draggedObject_=id; objectDragStart_=point; objectDragMoved_=false; SetCapture(window_);
            }
            return 0;
        }
        if (RouteUiViewportPress(point)) return 0; // ZE-96: a UI control under the cursor takes the press
        BeginDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (dragTarget_ == DragTarget::None) BeginAssetDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        uiInput_.cursor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; // ZE-96
        const int hovered=ChromeHit({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});
        if(hovered!=hoveredChrome_){hoveredChrome_=hovered;InvalidateRect(window_,nullptr,FALSE);}
        if(const int tab=ViewTabHit({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}); tab!=hoveredTab_){hoveredTab_=tab;InvalidateRect(window_,&viewportPanel_,FALSE);}
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window_,0};TrackMouseEvent(&tracking);
        if (draggedObject_)
        {
            const POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            objectDragMoved_=objectDragMoved_ || std::abs(point.x-objectDragStart_.x)>=GetSystemMetrics(SM_CXDRAG) || std::abs(point.y-objectDragStart_.y)>=GetSystemMetrics(SM_CYDRAG);
            if (objectDragMoved_) SetCursor(LoadCursorW(nullptr,(PtInRect(&mediaLibrary_,point)||PtInRect(&sceneBrowser_,point))?IDC_HAND:IDC_NO));
            return 0;
        }
        if (draggedAsset_ >= 0)
        {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            assetDragMoved_ = assetDragMoved_ || std::abs(point.x - assetDragStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                              std::abs(point.y - assetDragStart_.y) >= GetSystemMetrics(SM_CYDRAG);
            if (assetDragMoved_)
            {
                const int target=AssetAt(point);const bool folder=target>=0&&assetLibrary::Type(assets_[target])==assetLibrary::Kind::Folder&&target!=draggedAsset_;
                const bool valid = folder || (!zengine::scenes::IsScene(assets_.at(draggedAsset_)) && (ScriptDropTarget(point) != 0 || (!zengine::scripts::IsScript(assets_.at(draggedAsset_)) && PtInRect(&viewportContent_,point))));
                SetCursor(LoadCursorW(nullptr, valid ? IDC_HAND : IDC_NO));
            }
            return 0;
        }
        if (dragTarget_ != DragTarget::None)
        {
            UpdateDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        }
        break;
    }
    case WM_MOUSELEAVE: case WM_KILLFOCUS:
        hoveredChrome_=pressedChrome_=-1;InvalidateRect(window_,nullptr,FALSE);break;
    case WM_LBUTTONUP:
        pressedChrome_=-1;InvalidateRect(window_,nullptr,FALSE);
        uiInput_.primary=false; // ZE-96: release reaches the viewport UI in Render()
        if (draggedObject_)
        {
            const auto object=draggedObject_; const bool moved=objectDragMoved_; draggedObject_=0; ReleaseCapture();
            const POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            if (moved)
            {
                POINT screen=point; ClientToScreen(window_,&screen);
                if (inspectorPanel_ && inspectorPanel_->AssignObjectReferenceAt(screen,object)) return 0;
            }
            if (!moved) { SelectGameObject(object); return 0; }
            if (moved && PtInRect(&mediaLibrary_,point)) CreatePrefab(object);
            else if(moved && PtInRect(&sceneBrowser_,point))SetObjectParent(object,ScriptDropTarget(point));
            return 0;
        }
        FinishAssetDrag(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        EndDrag();
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        if (PtInRect(&sceneBrowser_, point))
        {
            const auto list = ObjectListRectangle();
            const int rows = std::max(1, static_cast<int>(list.bottom - list.top) / 27);
            firstObject_ = std::clamp(firstObject_ - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 3,
                0, std::max(0, static_cast<int>(ObjectRows().size()) - rows));
            InvalidateRect(window_, &sceneBrowser_, FALSE);
            return 0;
        }
        if (PtInRect(&mediaLibrary_, point) && draggedAsset_ < 0)
        {
            const RECT list = AssetListRectangle();
            const int columns=AssetColumns(),visibleRows=std::max(1,static_cast<int>(list.bottom-list.top)/84);
            firstAsset_=std::clamp(firstAsset_-GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA*columns,0,std::max(0,((static_cast<int>(assets_.size())-visibleRows*columns+columns-1)/columns)*columns));
            InvalidateRect(window_, &mediaLibrary_, FALSE);
            return 0;
        }
        if (Playing() && PtInRect(&viewportContent_, point)) // ZE-96: scroll the live UI
        { uiInput_.wheel += GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<float>(WHEEL_DELTA); return 0; }
        return 0;
    }
    case WM_CHAR:
        if (Playing() && uiInput_.typed.size() < 256) { uiInput_.typed.push_back(static_cast<char32_t>(wParam)); return 0; } // ZE-96
        break;
    case WM_CAPTURECHANGED:
        pressedChrome_=-1;InvalidateRect(window_,nullptr,FALSE);
        draggedObject_=0; objectDragMoved_=false;
        dragTarget_ = DragTarget::None;
        draggedAsset_ = -1;
        assetDragMoved_ = false;
        return 0;
    case WM_SETCURSOR:
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window_, &cursor);
        const DragTarget target = HitTestSplitter(cursor);
        if (target == DragTarget::MediaLibrary)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
        if (target == DragTarget::SceneBrowser || target == DragTarget::Inspector)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam=='W' || wParam=='E' || wParam=='R') { SetTransformTool(wParam=='W'?gizmo::Mode::Move:wParam=='E'?gizmo::Mode::Rotate:gizmo::Mode::Scale); return 0; }
        if (wParam == VK_DELETE && !Playing()) // ZE-82 / object delete
        {
            if (selectedAsset_ >= 0 && selectedAsset_ < static_cast<int>(assets_.size()))
            { try { DeleteAsset(assets_[selectedAsset_]); } catch (const std::exception& e) { status_=WideText(e.what()); InvalidateRect(window_,&statusBar_,FALSE); } return 0; }
            if (selectedObject_ && CanEdit(selectedObject_)) { DeleteGameObject(selectedObject_); return 0; }
        }
        if (wParam == VK_ESCAPE)
        {
            if (draggedObject_) { draggedObject_=0; ReleaseCapture(); return 0; }
            if (draggedAsset_ >= 0)
            {
                draggedAsset_ = -1;
                ReleaseCapture();
                return 0;
            }
            SendMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_ACTIVATEAPP:
        if (!wParam) EndGizmoDrag(true);
        break;
    case WM_DESTROY:
        viewportWindow_ = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        window_ = nullptr;
        return DefWindowProcW(window, message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
