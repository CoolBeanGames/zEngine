#pragma once
#include "ScriptAssets.h"
#include "ScriptCompletion.h"
#include <windows.h>
#include <commctrl.h>
#include <functional>

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
    bool Dirty() const noexcept { return dirty_; }
    void SetSavedHandler(std::function<void()> handler) { saved_ = std::move(handler); }
    void SetCompletionContext(std::function<std::vector<std::wstring>()> provider) { completionContext_=std::move(provider); }
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
    void RefreshCompletionIndex();
    void UpdateCompletion();
    void HideCompletion();
    bool AcceptCompletion();
    void PaintCompletion(HWND);
    static LRESULT CALLBACK CompletionProcedure(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    scriptCompletion::Index completionIndex_;
    scriptCompletion::Result completion_;
    std::function<std::vector<std::wstring>()> completionContext_;
    HWND completions_=nullptr;
    int completionSelection_=0;
    bool suppressCompletion_=false;
    std::filesystem::path assets_, path_;
    std::string loaded_;
    HWND window_ = nullptr, source_ = nullptr, errors_ = nullptr;
    HWND save_ = nullptr, reload_ = nullptr, jump_ = nullptr;
    HMODULE richEdit_ = nullptr;
    HFONT font_ = nullptr;
    bool dirty_ = false, formatting_ = false;
    zengine::scripts::Analysis analysis_;
    std::function<void()> saved_;
};
