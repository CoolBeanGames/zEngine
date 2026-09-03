#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <memory>
#include <filesystem>
#include <future>
#include <deque>
#include <string>
#include <vector>
#include <map>
#include "RenderScene.h"
#include "ModelData.h"
#include "ui/UiSystem.h"
#include "UiAssetBinding.h"
#include "core/GameObject.h"
#include "ScriptHost.h"
#include "Scene.h"
#include "Project.h"
#include "MaterialAssets.h"
#include "Prefab.h"
#include "input/InputMap.h"
#include "ObjectPicker.h"
#include <optional>
#include <chrono>
#include <mutex>

class Renderer;
class InspectorPanel;
class ScriptEditor;
class InputMapEditor;
class MaterialEditor;

class EditorShell final
{
public:
    explicit EditorShell(HINSTANCE instance);
    ~EditorShell();

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    [[nodiscard]] HWND Create(int showCommand, const std::filesystem::path& projectDirectory = {});
    void InitializeRenderer();
    void InitializeStartup(const std::filesystem::path& sessionFile = {});
    bool CreateProject(const std::wstring& name, const std::filesystem::path& location);
    bool OpenProject(const std::filesystem::path& file);
    const zengine::projects::Project* CurrentProject() const noexcept { return project_ ? &*project_ : nullptr; }
    const std::filesystem::path& AssetsDirectory() const noexcept { return assetsDirectory_; }
    std::filesystem::path AssetFolder() const;
    void OpenAssetFolder(const std::filesystem::path&);
    std::filesystem::path CreateAssetFolder(const std::wstring&);
    void MoveAsset(const std::filesystem::path& source,const std::filesystem::path& folder);
    void RenameAsset(const std::filesystem::path& source,const std::wstring& name);
    static constexpr int NewFolderCommand=3400,UpFolderCommand=3401,RenameAssetCommand=3402,RenameObjectCommand=3403;
    static constexpr int BuildProjectCommand=3500;
    bool BuildProject(const std::filesystem::path& outputParent);
    bool Building() const {return buildWork_.valid();}
    const std::filesystem::path& LastBuild() const {return lastBuild_;}
    const std::string& BuildError() const {return buildError_;}
    void SetObjectParent(zengine::GameObjectId child,zengine::GameObjectId parent);
    void RevertPrefabTransform(zengine::GameObjectId);
    static constexpr int UnparentCommand=3600,RevertPrefabTransformCommand=3601;
    bool HasOpenScene() const noexcept { return sceneOpen_; }
    static constexpr int SavePrefabCommand=3300, ClosePrefabCommand=3301;
    std::filesystem::path CreatePrefab(zengine::GameObjectId);
    bool OpenPrefab(const std::filesystem::path&);
    bool SavePrefab();
    bool ClosePrefab();
    zengine::GameObjectId InstantiatePrefab(const std::filesystem::path&,zengine::GameObjectId parent=0);
    const std::filesystem::path& EditingPrefab() const { return editingPrefab_; }
    static constexpr int MoveToolCommand=3200, RotateToolCommand=3201, ScaleToolCommand=3202;
    void SetTransformTool(gizmo::Mode mode);
    gizmo::Mode TransformTool() const { return transformTool_; }
    void Render();
    bool Play();
    void Stop();
    void SetPaused(bool paused);
    void Step();
    bool Playing() const noexcept { return scriptHost_.Playing(); }
    static constexpr int NewSceneCommand=3100, SaveSceneCommand=3101, SaveSceneAsCommand=3102, OpenSceneCommand=3103;
    static constexpr int NewProjectCommand=3104, OpenProjectCommand=3105;
    bool NewScene();
    bool OpenScene(const std::filesystem::path& path);
    bool SaveScene(const std::filesystem::path& path = {});
    bool TranslateShortcut(const MSG& message);
    const std::filesystem::path& ScenePath() const noexcept { return scenePath_; }
    bool SceneDirty() const noexcept { return sceneDirty_; }
    zengine::GameObject& CreateEmptyGameObject();
    enum class ObjectPreset { Empty, Cube, Camera, RigidBody, KinematicBody, StaticBody, Area };
    zengine::GameObject& CreateGameObject(ObjectPreset,zengine::GameObjectId parent=0);
    // Creates a GameObject2D carrying the named ui:: control (ui::UiControlTypes()).
    zengine::GameObject2D& CreateUiControl(std::string_view type,zengine::GameObjectId parent=0);
    void CopyGameObject(zengine::GameObjectId);
    zengine::GameObjectId PasteGameObject(zengine::GameObjectId parent=0);
    void DeleteGameObject(zengine::GameObjectId);
    static constexpr int AddEmptyCommand=3700,AddCubeCommand=3701,AddCameraCommand=3702,AddRigidCommand=3703,AddKinematicCommand=3704,AddStaticCommand=3705,AddAreaObjectCommand=3706,CopyObjectCommand=3710,PasteObjectCommand=3711,DeleteObjectCommand=3712;
    static constexpr int AddUiControlBase=3740,AddUiControlLast=3759; // one per ui::UiControlTypes()
    std::filesystem::path CreateScriptAsset();
    void OpenScript(const std::filesystem::path& path);
    // ZE-64: HLSL material shader assets (.shader).
    std::filesystem::path CreateShaderAsset();
    void OpenShader(const std::filesystem::path& path);
    // ZE-102: standalone editor for a .material asset (shader ref + pinned values).
    void OpenMaterial(const std::filesystem::path& path);
    // Test seam: open the material editor and return its window (nullptr on failure).
    HWND OpenMaterialEditor(const std::filesystem::path& path);
    // ZE-65: material instance assets (.material).
    std::filesystem::path CreateMaterialAsset();
    // ZE-97: encode the images in a folder (sorted by name) into a .zvid clip.
    std::filesystem::path BuildVideoClipFromImages();
    bool AttachScript(zengine::GameObjectId object, const std::filesystem::path& path);
    // Routes a completed object drag onto the Inspector (screen point + dragged object).
    // Same effect as releasing a scene-tree drag over the Inspector; used by tests.
    bool DropObjectOnInspector(POINT screenPoint, zengine::GameObjectId object);
    // ZE-97: drop a project-relative image/video path onto a UI texture field.
    bool DropAssetPathOnInspector(POINT screenPoint, const std::string& asset);
    // Test seam: the Inspector edit/combo control for UI property `key` (component
    // `axis` for Vec2 / Colour rows) of the currently selected UI control.
    HWND InspectorUiField(const std::string& key, int axis = 0) const;
    bool AddMeshRenderer(zengine::GameObjectId object);
    void AssignCube(zengine::GameObjectId object);
    void ClearMesh(zengine::GameObjectId object);
    void QueueModel(const std::filesystem::path& path, zengine::GameObjectId object = 0);
    ViewportFrame BuildSceneFrame() const;
    const zengine::ObjectStore& GameObjects() const noexcept { return objects_; }
    const zengine::GameObject* SelectedGameObject() const noexcept { return zengine::As3D(objects_.Find(selectedObject_)); }
    zengine::GameObject* SelectedGameObject() noexcept { return zengine::As3D(objects_.Find(selectedObject_)); }

    // The view panel is tabbed: Scene (edit), Game (Play), Script (inline editor + browser).
    enum class ViewTab { Scene, Game, Script };
    ViewTab CurrentViewTab() const noexcept { return viewTab_; }
    void SetViewTab(ViewTab tab);
    // Enforce a single "main"-tagged Camera (keepMain wins ties). Call after the tag changes.
    void SyncMainCamera(zengine::GameObjectId keepMain = 0);
    static constexpr int ScriptListControl = 3730, FunctionListControl = 3731;

private:
    enum class DragTarget
    {
        None,
        SceneBrowser,
        Inspector,
        MediaLibrary,
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ViewportProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateViewport();
    void Layout(std::uint32_t width, std::uint32_t height);
    void Paint();
    void BeginDrag(POINT position);
    void UpdateDrag(POINT position);
    void EndDrag();
    [[nodiscard]] DragTarget HitTestSplitter(POINT position) const;
    RECT AssetListRectangle() const;
    int AssetColumns() const;
    int AssetAt(POINT) const;
    RECT AssetCellRectangle(int index) const;
    std::wstring AssetDisplayName(const std::filesystem::path&) const;
    void RefreshAssets();
    void NewAssetFolderDialog();
    void ChooseBuildFolder();
    void PollBuild();
    struct BuildProgress {std::mutex mutex;unsigned percent=0;std::string message;};
    std::shared_ptr<BuildProgress> buildProgress_;
    std::future<std::filesystem::path> buildWork_;
    std::filesystem::path lastBuild_;
    std::string buildError_;
    void ReceiveFiles(HDROP drop);
    void PollAssetWork();
    void BeginAssetDrag(POINT point);
    void FinishAssetDrag(POINT point);
    void BeginAssetRename(const std::filesystem::path&);
    void BeginObjectRename(zengine::GameObjectId);
    void FinishRename(bool cancel);
    static LRESULT CALLBACK RenameProcedure(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    void SelectGameObject(zengine::GameObjectId id);
    void OnObjectChanged();
    RECT CreateObjectRectangle() const;
    RECT ObjectListRectangle() const;
    zengine::GameObjectId ScriptDropTarget(POINT point) const;
    void ChooseScript();
    std::vector<std::wstring> ProjectScriptPaths() const;
    std::vector<ObjectPicker::Item> AssetPickerItems(std::vector<assetLibrary::Kind> kinds) const;
    std::optional<zengine::GameObjectId> PickSceneObject(const std::string& referenceType, zengine::GameObjectId current, RECT anchorScreen) const;
    std::optional<std::string> ChoosePrefabReference(const std::string& current);
    zengine::GameObjectId SpawnPrefab(std::string_view asset);
    void ChooseModel();
    std::filesystem::path ResolveModel(const std::filesystem::path& path) const;
    bool ConfirmScriptClose();
    RECT CreateScriptRectangle() const;
    bool PrepareScripts();
    void ReportScriptErrors();
    bool ConfirmSceneClose();
    bool SaveSceneAs();
    void ChooseScene();
    void ApplyScene(const std::filesystem::path&,std::string source,const zengine::scenes::Document&);
    void MarkSceneDirty();
    void UpdateSceneTitle();
    void ConfigureScriptOutput();
    std::wstring SceneName() const;
    RECT CreateSceneRectangle() const;
    bool PendingModels(bool assignmentsOnly=false) const;
    zengine::scenes::Document CaptureDocument() const;
    void RefreshPrefabInstances();
    void RebuildDocument(const zengine::scenes::Document&,zengine::GameObjectId select=0);
    void RequireEditable(zengine::GameObjectId,bool transformOnly=false) const;
    bool CanEdit(zengine::GameObjectId,bool transformOnly=false) const;
    void RecordTransformOverride(zengine::GameObjectId);
    void RecordPrefabDataOverride(zengine::GameObjectId);
    void RefreshOpenDocumentAfterAssetMove(const std::filesystem::path& source,const std::filesystem::path& destination,const std::string& from,const std::string& to);
    int ObjectDepth(zengine::GameObjectId) const;
    std::vector<zengine::GameObjectId> ObjectRows() const;
    bool HasChildren(zengine::GameObjectId) const;
    std::set<zengine::GameObjectId> collapsedObjects_;
    std::map<zengine::GameObjectId,zengine::scenes::ObjectData> prefabLinks_;
    std::map<zengine::GameObjectId,std::string> prefabSources_;
    std::set<zengine::GameObjectId> prefabGenerated_;
    struct SceneSnapshot { zengine::scenes::Document document; std::filesystem::path path; std::string source,baseline; bool dirty=false,open=false; zengine::GameObjectId selected=0; };
    std::optional<SceneSnapshot> prefabReturn_;
    std::optional<zengine::scenes::Document> objectClipboard_;
    std::filesystem::path editingPrefab_;
    zengine::GameObjectId draggedObject_=0;
    POINT objectDragStart_{};
    bool objectDragMoved_=false;
    LRESULT HandleViewportMessage(HWND,UINT,WPARAM,LPARAM);
    void EndGizmoDrag(bool cancel);
    void UpdateGizmoDrag(gizmo::Point);
    RECT ToolRectangle(int index) const;
    RECT ChromeRectangle(int index) const;
    int ChromeHit(POINT) const;
    bool ChromeEnabled(int index) const;
    int hoveredChrome_=-1,pressedChrome_=-1;
    gizmo::Mode transformTool_=gizmo::Mode::Move;
    std::optional<gizmo::Drag> gizmoDrag_;
    zengine::GameObjectId gizmoObject_=0;
    bool gizmoWasDirty_=false;
    SceneCamera sceneCamera_;
    enum class CameraDrag { None,Orbit,Pan,Fly };
    CameraDrag cameraDrag_=CameraDrag::None;
    POINT cameraPoint_{};
    void CameraMotion(POINT);
    void CameraTick(float delta);
    void EndCameraDrag();
    int hoveredAxis_=-1;
    void RequireProject() const;
    void RequireScene() const;
    bool ConfirmProjectClose();
    void ActivateProject(zengine::projects::Project project);
    void NewProjectDialog();
    void ChooseProject();
    void PromptForScene();
    void RememberProjectScene();
    void OpenInputMap();
    void TickInput();
    std::unique_ptr<InputMapEditor> inputEditor_;
    std::unique_ptr<MaterialEditor> materialEditor_;
    zengine::input::System inputSystem_;
    std::optional<zengine::projects::Project> project_;
    std::filesystem::path recentSessionFile_;
    bool sceneOpen_ = false;

    struct AssetJob
    {
        std::filesystem::path path;
        bool loadMesh = false;
        zengine::GameObjectId object = 0; // Zero creates a new object after a successful load.
        std::uint64_t revision = 0;
        std::uint64_t scene = 0;
        bool restoreOnly = false;
        std::filesystem::path destination;
    };
    struct AssetResult
    {
        std::filesystem::path path;
        bool loadMesh = false;
        zengine::GameObjectId object = 0;
        std::uint64_t revision = 0;
        std::uint64_t scene = 0;
        bool restoreOnly = false;
        ModelData model;
        MeshHandle cachedMesh;
        std::vector<std::string> warnings;
    };
    std::filesystem::path assetsDirectory_;
    std::filesystem::path assetFolder_;
    std::vector<std::filesystem::path> assets_;
    std::deque<AssetJob> assetJobs_;
    std::future<AssetResult> assetWork_;
    std::optional<AssetJob> activeAssetJob_;
    std::uint64_t sceneGeneration_ = 1;
    std::filesystem::path scenePath_;
    std::string sceneSource_, sceneBaseline_;
    bool sceneDirty_ = false;
    std::optional<zengine::scenes::Document> playScene_;
    std::set<zengine::GameObjectId> playObjects_;
    std::wstring status_ = L"Create or open a project from the File menu";
    zengine::ObjectStore objects_;
    zengine::ScriptHost scriptHost_;
    std::unique_ptr<zengine::physics::World> physicsWorld_;
    bool paused_ = false, stepDraw_ = false;
    double tickAccumulator_ = 0;
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
    ULONGLONG lastInspectorRefresh_ = 0;
    zengine::GameObjectId selectedObject_ = 0;
    struct MeshBinding { std::string asset; MeshHandle mesh; };
    std::map<zengine::GameObjectId, MeshBinding> meshBindings_;
    // ZE-65: resolved Material Instances, keyed by project-relative ".material" path.
    mutable std::map<std::string, MaterialHandle> materialCache_;
    MaterialHandle ResolveMaterial(const std::string& materialAsset) const;
    zengine::materials::Effective ResolveMaterialEffective(const std::string& materialAsset) const;
    void ApplyMaterialValue(const std::string& materialAsset, zengine::materials::Value value);
    std::map<std::filesystem::path, std::weak_ptr<const RenderMesh>> meshCache_;
    std::map<zengine::GameObjectId, std::uint64_t> meshRevisions_;
    int firstObject_ = 0;
    std::unique_ptr<InspectorPanel> inspectorPanel_;
    std::vector<std::unique_ptr<ScriptEditor>> scriptEditors_;
    // Script tab: inline editor plus a script list (top) and function list (bottom).
    ViewTab viewTab_ = ViewTab::Scene;
    int hoveredTab_ = -1;
    HWND scriptListBox_ = nullptr, functionListBox_ = nullptr;
    std::unique_ptr<ScriptEditor> inlineEditor_;
    std::vector<std::filesystem::path> scriptTabPaths_;
    std::vector<std::size_t> functionOffsets_;
    RECT ViewTabRect(int index) const;
    int ViewTabHit(POINT point) const;
    void EnsureScriptTab();
    void RefreshScriptTabList();
    void RefreshFunctionList();
    void OpenInlineScript(const std::filesystem::path& path);
    void LayoutScriptTab();
    int firstAsset_ = 0;
    int selectedAsset_ = -1;
    int draggedAsset_ = -1;
    POINT assetDragStart_{};
    bool assetDragMoved_ = false;
    enum class RenameTarget { None,Asset,Object };
    RenameTarget renameTarget_=RenameTarget::None;
    std::filesystem::path renameAsset_;
    zengine::GameObjectId renameObject_=0;
    std::string renameObjectOriginal_;
    HWND renameEdit_=nullptr;
    bool finishingRename_=false;
    ULONGLONG lastBusyPaint_ = 0;
    bool showFps_=true;
    unsigned currentFps_=0,framesSinceFps_=0;
    std::chrono::steady_clock::time_point fpsSample_=std::chrono::steady_clock::now();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND consoleWindow_ = nullptr;
    HWND viewportWindow_ = nullptr;
    bool mouseButtonsPrev_[3] = {}; // L/R/M pressed last TickInput, for just_pressed/just_released
    std::unique_ptr<Renderer> renderer_;
    zengine::ui::UiSystem uiViewport_; // screen-space UI preview for the viewport
    std::optional<zengine::ui::UiAssetBinding> uiViewportAssets_; // image / video cache for the preview
    // ZE-96: viewport UI input, accumulated by the WndProc and applied in Render()
    // while Playing so scripted UI is testable in the editor without a build.
    struct { POINT cursor{-1,-1}; bool primary=false; float wheel=0; std::vector<char32_t> typed; } uiInput_;
    bool RouteUiViewportPress(POINT clientPoint); // true if a UI control took the press

    HFONT uiFont_ = nullptr;
    HFONT headerFont_ = nullptr;
    DragTarget dragTarget_ = DragTarget::None;

    RECT optionsBar_{};
    RECT sceneBrowser_{};
    RECT viewportPanel_{};
    RECT viewportContent_{};
    RECT inspector_{};
    RECT mediaLibrary_{};
    RECT statusBar_{};
    RECT sceneSplitter_{};
    RECT inspectorSplitter_{};
    RECT mediaSplitter_{};

    int sceneBrowserWidth_ = 225;
    int inspectorWidth_ = 290;
    int mediaLibraryHeight_ = 280;
    std::uint32_t requestedViewportWidth_ = 0;
    std::uint32_t requestedViewportHeight_ = 0;
    std::uint32_t rendererWidth_ = 0;
    std::uint32_t rendererHeight_ = 0;
};
