#pragma once
#include <string>
#include <vector>

#include <windows.h>

#include "ProjectStore.h"

namespace zlauncher
{
// The launcher's single top-level window: a top bar (Update / New Project) over a
// scrollable list of projects, each row offering Edit, Build and Delete.
class LauncherWindow
{
public:
    explicit LauncherWindow(HINSTANCE instance);

    HWND Create(int showCommand);
    bool PreTranslate(MSG& message);

private:
    static LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ListProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
    LRESULT OnMain(UINT m, WPARAM wp, LPARAM lp);
    LRESULT OnList(UINT m, WPARAM wp, LPARAM lp);

    void LayoutChildren();
    void RebuildRows();
    void RepositionRows();
    void UpdateScrollBar();
    void PaintTopBar(HDC dc, const RECT& client);
    void PaintSection(HDC dc, const RECT& client);
    void PaintStatus(HDC dc, const RECT& client);
    void PaintList(HDC dc);
    void SetStatus(std::wstring text);

    void OnNewProject();
    void OnUpdate();
    void OnRowCommand(int commandId);

    HINSTANCE instance_ = nullptr;
    HWND main_ = nullptr;
    HWND list_ = nullptr;
    HWND btnUpdate_ = nullptr;
    HWND btnNew_ = nullptr;

    HFONT titleFont_ = nullptr;
    HFONT nameFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT headerFont_ = nullptr;

    int hoverRow_ = -1;   // project row under the cursor, or -1

    ProjectStore store_;

    struct Row
    {
        HWND play = nullptr;
        HWND edit = nullptr;
        HWND build = nullptr;
        HWND tmpl = nullptr;
        HWND del = nullptr;
    };
    std::vector<Row> rows_;

    int scroll_ = 0;
    std::wstring status_;
    std::wstring launcherVersion_;
    std::wstring editorVersion_;
};
}
