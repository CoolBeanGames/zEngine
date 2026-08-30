#pragma once
#include "ScriptAssets.h"
#include <windows.h>
#include <commctrl.h>

// Native RichEdit window; no renderer or VM ownership. One document per window.
class ScriptEditor final
{
public:
    static constexpr int SourceControl = 2100, SaveCommand = 2101, ReloadCommand = 2102, ErrorCommand = 2103;
    ScriptEditor(HWND owner, const std::filesystem::path& assets, const std::filesystem::path& path);
    ~ScriptEditor();
    void Show();
    bool ConfirmClose();
    HWND Window() const { return window_; }
    const std::filesystem::path& Path() const { return path_; }
private:
    static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void Layout();
    void Reload();
    bool Save();
    void Highlight();
    void Title();
    std::wstring Text() const;
    std::filesystem::path assets_, path_;
    std::string loaded_;
    HWND window_ = nullptr, source_ = nullptr, errors_ = nullptr;
    HWND save_ = nullptr, reload_ = nullptr, jump_ = nullptr;
    HMODULE richEdit_ = nullptr;
    HFONT font_ = nullptr;
    bool dirty_ = false, formatting_ = false;
    zengine::scripts::Analysis analysis_;
};
