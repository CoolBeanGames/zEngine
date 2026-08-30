#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

class Renderer;

class EditorShell final
{
public:
    explicit EditorShell(HINSTANCE instance);
    ~EditorShell();

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    [[nodiscard]] HWND Create(int showCommand);
    void InitializeRenderer();
    void Render();

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
