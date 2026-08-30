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
#include "ModelData.h"
#include "core/GameObject.h"

class Renderer;
class InspectorPanel;
class ScriptEditor;

class EditorShell final
{
public:
    explicit EditorShell(HINSTANCE instance);
    ~EditorShell();

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    [[nodiscard]] HWND Create(int showCommand, const std::filesystem::path& projectDirectory = {});
    void InitializeRenderer();
    void Render();
    zengine::GameObject& CreateEmptyGameObject();
    std::filesystem::path CreateScriptAsset();
    void OpenScript(const std::filesystem::path& path);
    bool AttachScript(zengine::GameObjectId object, const std::filesystem::path& path);
    const zengine::ObjectStore& GameObjects() const noexcept { return objects_; }
    const zengine::GameObject* SelectedGameObject() const noexcept { return objects_.Find(selectedObject_); }

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
    void RefreshAssets();
    void ReceiveFiles(HDROP drop);
    void PollAssetWork();
    void BeginAssetDrag(POINT point);
    void FinishAssetDrag(POINT point);
    void SelectGameObject(zengine::GameObjectId id);
    void OnObjectChanged();
    RECT CreateObjectRectangle() const;
    RECT ObjectListRectangle() const;
    zengine::GameObjectId ScriptDropTarget(POINT point) const;
    void ChooseScript();
    bool ConfirmScriptClose();
    RECT CreateScriptRectangle() const;

    struct AssetJob { std::filesystem::path path; bool replacePreview = false; };
    struct AssetResult
    {
        std::filesystem::path path;
        bool replacePreview = false;
        ModelData model;
        std::vector<std::string> warnings;
    };
    std::filesystem::path assetsDirectory_;
    std::vector<std::filesystem::path> assets_;
    std::deque<AssetJob> assetJobs_;
    std::future<AssetResult> assetWork_;
    std::wstring status_ = L"Ready - drop an FBX into the Media Library";
    zengine::ObjectStore objects_;
    zengine::GameObjectId selectedObject_ = 0;
    zengine::GameObjectId previewObject_ = 0;
    int firstObject_ = 0;
    std::unique_ptr<InspectorPanel> inspectorPanel_;
    std::vector<std::unique_ptr<ScriptEditor>> scriptEditors_;
    int firstAsset_ = 0;
    int selectedAsset_ = -1;
    int draggedAsset_ = -1;
    POINT assetDragStart_{};
    bool assetDragMoved_ = false;
    ULONGLONG lastBusyPaint_ = 0;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND viewportWindow_ = nullptr;
    std::unique_ptr<Renderer> renderer_;

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
