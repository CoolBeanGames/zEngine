#pragma once
#include "ScriptAssets.h"
#include "ShaderAssets.h"
#include "ScriptCompletion.h"
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Native RichEdit window; no renderer or VM ownership. One document per window.
class ScriptEditor final
{
public:
    static constexpr int SourceControl = 2100, SaveCommand = 2101, ReloadCommand = 2102, ErrorCommand = 2103,FoldCommand=2104,ExpandCommand=2105;
    // Width in pixels of the right-hand line-number gutter reserved inside the source control.
    static constexpr int LineNumberGutter = 44;
    // Pass a non-null embedIn to build the editor as a borderless child of that window
    // (the inline Script tab) instead of a standalone top-level window.
    ScriptEditor(HWND owner, const std::filesystem::path& assets, const std::filesystem::path& path, HWND embedIn = nullptr);
    ~ScriptEditor();
    void Show();
    bool ConfirmClose();
    HWND Window() const { return window_; }
    const std::filesystem::path& Path() const { return path_; }
    bool Dirty() const noexcept { return dirty_; }
    void SetSavedHandler(std::function<void()> handler) { saved_ = std::move(handler); }
    void SetCompletionContext(std::function<std::vector<std::wstring>()> provider) { completionContext_=std::move(provider); }
    // Inline (embedded) editor helpers.
    void SetBounds(RECT bounds) const { if (window_) MoveWindow(window_, bounds.left, bounds.top, bounds.right-bounds.left, bounds.bottom-bounds.top, TRUE); }
    // Function declarations in the current buffer as (name, character offset), source order.
    std::vector<std::pair<std::wstring, std::size_t>> Functions() const;
    // Move the caret to a character offset and scroll it into view.
    void GoTo(std::size_t offset);
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
    bool ToggleFoldAt(POINT point);
    void PaintFoldMarkers(HWND);
    void PaintLineNumbers(HWND);
    bool ToggleFold(); void ExpandAll(); void SetHidden(std::size_t start,std::size_t end,bool hidden);
    static LRESULT CALLBACK CompletionProcedure(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    void UpdateHover(POINT clientPoint); // resolve + show the signature tooltip
    void HideHover();
    static constexpr UINT_PTR HoverTimer = 0x48; // 'H'
    scriptCompletion::Index completionIndex_;
    scriptCompletion::Result completion_;
    std::function<std::vector<std::wstring>()> completionContext_;
    HWND completions_=nullptr;
    HWND tooltip_=nullptr;
    POINT hoverPoint_{-1,-1};
    std::wstring hoverText_;
    int completionSelection_=0;
    bool suppressCompletion_=false;
    std::filesystem::path assets_, path_;
    std::string loaded_;
    HWND window_ = nullptr, source_ = nullptr, errors_ = nullptr;
    HWND save_ = nullptr, reload_ = nullptr, jump_ = nullptr,fold_ = nullptr,expand_ = nullptr;
    HMODULE richEdit_ = nullptr;
    HFONT font_ = nullptr;
    bool dirty_ = false, formatting_ = false, embedded_ = false;
    bool hlsl_ = false; // editing a .shader (HLSL) asset rather than a .zsh script
    std::string LoadSource() const;              // .shader vs .zsh loader
    void SaveSource(std::string_view bytes) const;
    std::set<std::size_t> foldedBlocks_;
    zengine::scripts::Analysis analysis_;
    std::function<void()> saved_;
};
