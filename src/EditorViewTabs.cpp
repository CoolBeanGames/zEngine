#include "EditorShell.h"
#include "EditorStyle.h"
#include "ScriptEditor.h"
#include "ScriptAssets.h"

#include <algorithm>

// The view panel's Scene / Game / Script tab strip and the inline Script tab.

namespace
{
    constexpr int TabBarHeight = 30;
    constexpr int ScriptSidebarWidth = 210;

    std::wstring Wide(const std::string& text)
    {
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(count, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
        return out;
    }
}

RECT EditorShell::ViewTabRect(int index) const
{
    const int width = 62, gap = 2, top = viewportPanel_.top + 3, height = TabBarHeight - 6;
    const int left = viewportPanel_.left + 8 + index * (width + gap);
    return RECT{left, top, left + width, top + height};
}

int EditorShell::ViewTabHit(POINT point) const
{
    for (int i = 0; i < 3; ++i)
        if (const auto rect = ViewTabRect(i); PtInRect(&rect, point)) return i;
    return -1;
}

void EditorShell::EnsureScriptTab()
{
    if (scriptListBox_) return;
    const DWORD style = WS_CHILD | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT;
    scriptListBox_ = CreateWindowExW(0, L"LISTBOX", L"", style, 0, 0, 1, 1, window_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ScriptListControl)), instance_, nullptr);
    functionListBox_ = CreateWindowExW(0, L"LISTBOX", L"", style, 0, 0, 1, 1, window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(FunctionListControl)), instance_, nullptr);
    if (!scriptListBox_ || !functionListBox_) throw std::runtime_error("Cannot create the Script tab lists.");
    SendMessageW(scriptListBox_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), FALSE);
    SendMessageW(functionListBox_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), FALSE);
    RefreshScriptTabList();
}

void EditorShell::RefreshScriptTabList()
{
    if (!scriptListBox_) return;
    scriptTabPaths_.clear();
    SendMessageW(scriptListBox_, LB_RESETCONTENT, 0, 0);
    for (const auto& relative : ProjectScriptPaths())
    {
        scriptTabPaths_.push_back(assetsDirectory_ / std::filesystem::path(relative));
        SendMessageW(scriptListBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(relative.c_str()));
    }
    // Keep the open script highlighted after the list rebuilds.
    if (inlineEditor_)
        for (std::size_t i = 0; i < scriptTabPaths_.size(); ++i)
            if (_wcsicmp(scriptTabPaths_[i].c_str(), inlineEditor_->Path().c_str()) == 0)
                { SendMessageW(scriptListBox_, LB_SETCURSEL, i, 0); break; }
}

void EditorShell::RefreshFunctionList()
{
    if (!functionListBox_) return;
    functionOffsets_.clear();
    SendMessageW(functionListBox_, LB_RESETCONTENT, 0, 0);
    if (!inlineEditor_) return;
    for (const auto& [name, offset] : inlineEditor_->Functions())
    {
        functionOffsets_.push_back(offset);
        SendMessageW(functionListBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>((name + L"()").c_str()));
    }
}

void EditorShell::OpenInlineScript(const std::filesystem::path& path)
{
    EnsureScriptTab();
    std::filesystem::path resolved;
    try {
        resolved = zengine::shaders::IsShader(path) ? zengine::shaders::Resolve(assetsDirectory_, path)
                                                    : zengine::scripts::Resolve(assetsDirectory_, path);
    }
    catch (const std::exception& error) { status_ = L"Cannot open: " + Wide(error.what()); InvalidateRect(window_, &statusBar_, FALSE); return; }
    if (inlineEditor_ && _wcsicmp(inlineEditor_->Path().c_str(), resolved.c_str()) == 0) { RefreshFunctionList(); return; }
    if (inlineEditor_ && !inlineEditor_->ConfirmClose()) return;

    auto editor = std::make_unique<ScriptEditor>(window_, assetsDirectory_, resolved, window_);
    editor->SetCompletionContext([this]() {
        std::vector<std::wstring> values;
        for (std::size_t i = 0; i < objects_.Size(); ++i)
        {
            values.push_back(Wide(objects_.At(i).Name()));
            for (const auto& tag : objects_.At(i).Tags()) values.push_back(Wide(tag));
        }
        return values;
    });
    editor->SetSavedHandler([this]() {
        if (!Playing()) PrepareScripts();
        else { status_ = L"Script saved - Stop and Play to run the new code"; InvalidateRect(window_, &statusBar_, FALSE); }
        RefreshFunctionList();
    });
    inlineEditor_ = std::move(editor);
    LayoutScriptTab();
    inlineEditor_->Show();
    RefreshScriptTabList();
    RefreshFunctionList();
    status_ = L"Editing " + resolved.filename().wstring();
    InvalidateRect(window_, &statusBar_, FALSE);
}

void EditorShell::LayoutScriptTab()
{
    const bool active = viewTab_ == ViewTab::Script;
    for (HWND control : {scriptListBox_, functionListBox_})
        if (control) ShowWindow(control, active ? SW_SHOW : SW_HIDE);
    if (inlineEditor_ && inlineEditor_->Window()) ShowWindow(inlineEditor_->Window(), active ? SW_SHOW : SW_HIDE);
    if (!active) return;

    RECT area = viewportContent_;
    const int sidebar = std::min<int>(ScriptSidebarWidth, std::max<LONG>(0, area.right - area.left - 200));
    const int half = (area.bottom - area.top) / 2 - 2;
    if (scriptListBox_) MoveWindow(scriptListBox_, area.left, area.top, sidebar, half, TRUE);
    if (functionListBox_) MoveWindow(functionListBox_, area.left, area.top + half + 4, sidebar, area.bottom - area.top - half - 4, TRUE);
    if (inlineEditor_)
        inlineEditor_->SetBounds(RECT{area.left + sidebar + 4, area.top, area.right, area.bottom});
}

void EditorShell::SetViewTab(ViewTab tab)
{
    if (tab == ViewTab::Script) EnsureScriptTab();
    viewTab_ = tab;
    // The D3D viewport child only paints for Scene/Game.
    if (viewportWindow_) ShowWindow(viewportWindow_, tab == ViewTab::Script ? SW_HIDE : SW_SHOW);
    RECT client{}; GetClientRect(window_, &client);
    if (client.right > 0 && client.bottom > 0)
        Layout(static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom));
    else
        LayoutScriptTab();
    if (tab == ViewTab::Script && inlineEditor_) SetFocus(inlineEditor_->Window());
    InvalidateRect(window_, nullptr, FALSE);
}
