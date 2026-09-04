#include "ScriptEditor.h"
#include "ScriptTyping.h"
#include "EditorStyle.h"
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <windowsx.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#ifdef ZENGINE_SCRIPT_COMPILER
#include "zscript/Script.h"
#endif

namespace
{
    constexpr wchar_t ClassName[] = L"zEngineScriptEditor";
    std::filesystem::path ResolveAsset(const std::filesystem::path& assets, const std::filesystem::path& path)
    {
        return zengine::shaders::IsShader(path) ? zengine::shaders::Resolve(assets, path)
                                                : zengine::scripts::Resolve(assets, path);
    }
    std::wstring Wide(const std::string& text)
    {
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!size && !text.empty()) throw std::runtime_error("Script is not valid UTF-8.");
        std::wstring result(size, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }
    std::string Utf8(const std::wstring& text)
    {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (!size && !text.empty()) throw std::runtime_error("Script contains invalid Unicode.");
        std::string result(size, ' ');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }
}
ScriptEditor::ScriptEditor(HWND owner, const std::filesystem::path& assets, const std::filesystem::path& path, HWND embedIn)
    : assets_(assets), path_(ResolveAsset(assets, path)), embedded_(embedIn != nullptr)
{
    hlsl_ = zengine::shaders::IsShader(path_);
    // Validate before opening a window. No partial document on failed reads.
    loaded_ = LoadSource();
    richEdit_ = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit_) throw std::runtime_error("Windows RichEdit is unavailable.");
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.hInstance = instance; type.lpfnWndProc = WindowProcedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = ClassName;
    type.hbrBackground = editorStyle::Shared().panel;
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    { FreeLibrary(richEdit_); throw std::runtime_error("Cannot register script editor window."); }
    window_ = embedded_
        ? CreateWindowExW(0, ClassName, L"Script Editor", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 1, 1, embedIn, nullptr, instance, this)
        : CreateWindowExW(0, ClassName, L"Script Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 900, 700, owner, nullptr, instance, this);
    if (!window_) { FreeLibrary(richEdit_); throw std::runtime_error("Cannot open script editor."); }
    try
    {
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        source_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SourceControl)), instance, nullptr);
        errors_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 1, 1, window_, nullptr, instance, nullptr);
        auto button = [&](const wchar_t* label, int id) { return CreateWindowExW(0, L"BUTTON", label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr); };
        save_ = button(L"Save (Ctrl+S)", SaveCommand);
        reload_ = button(L"Reload (Ctrl+R)", ReloadCommand);
        jump_ = button(L"Go to first error", ErrorCommand);
        fold_ = button(L"Toggle block",FoldCommand);expand_=button(L"Expand all",ExpandCommand);
        editorStyle::AttachChildren(window_);
        if (!source_ || !errors_ || !save_ || !reload_ || !jump_ || !fold_ || !expand_) throw std::runtime_error("Cannot create script editor controls.");
        SendMessageW(source_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(errors_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(source_, EM_SETBKGNDCOLOR, 0, RGB(30,32,36));
        SendMessageW(source_, EM_EXLIMITTEXT, 0, zengine::scripts::MaxSourceBytes);
        SendMessageW(source_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        // Reserve a narrow left gutter for per-block +/- fold controls and a right
        // gutter for line numbers. RichEdit keeps text and caret clear of both strips.
        SendMessageW(source_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(LeftGutter, 4));
        // Syntax formatting needs rich-text mode; clipboard paste is restricted to plain text below.
        SendMessageW(source_, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
        SetWindowSubclass(source_, EditProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        completions_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"LISTBOX",L"",WS_POPUP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,
            0,0,1,1,window_,nullptr,instance,nullptr);
        if(!completions_)throw std::runtime_error("Cannot create completion list.");
        SendMessageW(completions_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        SetWindowSubclass(completions_,CompletionProcedure,1,reinterpret_cast<DWORD_PTR>(this));
        // Hover tooltip: shows a call/signal signature when the pointer rests on an identifier.
        tooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASS,nullptr,WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,
            0,0,0,0,window_,nullptr,instance,nullptr);
        if(tooltip_) {
            SendMessageW(tooltip_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,600);
            TOOLINFOW info{sizeof(info)}; info.uFlags=TTF_IDISHWND|TTF_TRACK|TTF_ABSOLUTE;
            info.hwnd=window_; info.uId=reinterpret_cast<UINT_PTR>(source_); info.lpszText=const_cast<wchar_t*>(L"");
            SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
        Reload(); Layout();
    }
    catch (...)
    {
        DestroyWindow(window_); window_ = nullptr;
        if (font_) DeleteObject(font_);
        FreeLibrary(richEdit_); throw;
    }
}
ScriptEditor::~ScriptEditor()
{
    if (IsWindow(window_)) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
    if (richEdit_) FreeLibrary(richEdit_);
}
void ScriptEditor::Show()
{
    RefreshCompletionIndex();HideCompletion();
    if (!dirty_) Reload();
    if (embedded_) { ShowWindow(window_, SW_SHOW); SetFocus(source_); return; }
    ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(source_);
}
std::vector<std::pair<std::wstring, std::size_t>> ScriptEditor::Functions() const
{
    std::vector<std::pair<std::wstring, std::size_t>> result;
    const auto source = Text();
    const auto code = scriptTyping::Code(source); // comments and strings blanked out
    for (std::size_t i = 0; i + 4 < code.size(); ++i)
    {
        if (code.compare(i, 4, L"func") != 0) continue;
        if (i && (std::iswalnum(code[i-1]) || code[i-1] == L'_')) continue;
        std::size_t j = i + 4;
        while (j < code.size() && std::iswspace(code[j])) ++j;
        const std::size_t nameStart = j;
        while (j < code.size() && (std::iswalnum(code[j]) || code[j] == L'_')) ++j;
        if (j == nameStart) continue;
        result.emplace_back(source.substr(nameStart, j - nameStart), i);
        i = j;
    }
    return result;
}
void ScriptEditor::GoTo(std::size_t offset)
{
    if (!source_) return;
    CHARRANGE range{static_cast<LONG>(offset), static_cast<LONG>(offset)};
    SendMessageW(source_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    // Put the target line a few rows below the top rather than pinned to the caret.
    const auto line = SendMessageW(source_, EM_EXLINEFROMCHAR, 0, static_cast<LPARAM>(offset));
    const auto first = SendMessageW(source_, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(source_, EM_LINESCROLL, 0, static_cast<LPARAM>(line - first - 3));
    SendMessageW(source_, EM_SCROLLCARET, 0, 0);
    SetFocus(source_);
}
void ScriptEditor::Title()
{
    SetWindowTextW(window_, ((dirty_ ? L"* " : L"") + path_.filename().wstring() + L" - zEngine Script Editor").c_str());
}
std::wstring ScriptEditor::Text() const
{
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    const auto count = SendMessageW(source_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    std::wstring text(static_cast<std::size_t>(count) + 1, L'\0');
    GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    const auto read = SendMessageW(source_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(read));
    return text;
}
std::string ScriptEditor::LoadSource() const
{
    return hlsl_ ? zengine::shaders::Load(path_) : zengine::scripts::Load(path_);
}
void ScriptEditor::SaveSource(std::string_view bytes) const
{
    if (hlsl_) zengine::shaders::Save(assets_, path_, bytes, &loaded_);
    else zengine::scripts::Save(assets_, path_, bytes, &loaded_);
}
void ScriptEditor::Reload()
{
    const auto disk = LoadSource();
    auto text = Wide(disk);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());
    foldedBlocks_.clear();
    formatting_ = true;
    SetWindowTextW(source_, text.c_str());
    SendMessageW(source_, EM_EMPTYUNDOBUFFER, 0, 0);
    formatting_ = false;
    loaded_ = disk; dirty_ = false; Title(); Highlight();
    RefreshCompletionIndex();HideCompletion();
}
bool ScriptEditor::Save()
{
    try
    {
        const auto raw = Text();
        // RichEdit uses CR internally. Store conventional CRLF UTF-8 source on disk.
        std::wstring text;
        for (wchar_t c : raw) { if (c == L'\r') text += L"\r\n"; else text += c; }
        const auto bytes = Utf8(text);
        SaveSource(bytes);
        loaded_ = bytes; dirty_ = false; Title(); Highlight();
        if (saved_) saved_();
        RefreshCompletionIndex();HideCompletion();
        return true;
    }
    catch (const std::exception& e) { MessageBoxW(window_, Wide(e.what()).c_str(), L"Script save failed", MB_OK | MB_ICONERROR); return false; }
}
bool ScriptEditor::ConfirmClose()
{
    if (!dirty_) return true;
    const auto answer = MessageBoxW(window_, (L"Save changes to " + path_.filename().wstring() + L"?").c_str(),
        L"Unsaved script", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) return Save();
    // Restore the loaded snapshot even if the file was deleted externally. This also
    // leaves a consistent clean buffer if another document cancels the parent's close.
    auto snapshot = Wide(loaded_);
    if (!snapshot.empty() && snapshot.front() == 0xfeff) snapshot.erase(snapshot.begin());
    foldedBlocks_.clear();
    formatting_ = true;
    SetWindowTextW(source_, snapshot.c_str());
    SendMessageW(source_, EM_EMPTYUNDOBUFFER, 0, 0);
    formatting_ = false;
    dirty_ = false; Title(); Highlight(); return true;
}
void ScriptEditor::Highlight()
{
    KillTimer(window_, 1);
    const auto text = Text();
    analysis_ = hlsl_ ? zengine::shaders::Analyze(text) : zengine::scripts::Analyze(text);
#ifdef ZENGINE_SCRIPT_COMPILER
  if (!hlsl_) {
    // Only link to the compiler in this checkout; never to another chat's unfinished worktree.
    auto normalized = text;
    std::replace(normalized.begin(), normalized.end(), L'\r', L'\n');
    const auto compiled = zengine::script::Compiler::Compile(Utf8(normalized), path_.filename().string());
    for (const auto& diagnostic : compiled.diagnostics)
    {
        std::size_t start = 0, line = 1;
        while (start < normalized.size() && line < diagnostic.line) if (normalized[start++] == L'\n') ++line;
        // Compiler columns count UTF-8 bytes; translate to RichEdit UTF-16 character indices.
        const auto lineEnd = normalized.find(L'\n', start);
        const auto lineText = Utf8(normalized.substr(start, lineEnd == std::wstring::npos ? lineEnd : lineEnd-start));
        std::size_t columnBytes = std::min(diagnostic.column > 0 ? diagnostic.column - 1 : 0, lineText.size());
        while (columnBytes && columnBytes < lineText.size() && (static_cast<unsigned char>(lineText[columnBytes]) & 0xc0) == 0x80) --columnBytes;
        start += Wide(lineText.substr(0, columnBytes)).size();
        analysis_.errors.push_back({start, 1, diagnostic.line, diagnostic.column, diagnostic.message});
    }
  }
#endif
    formatting_ = true;
    CHARRANGE selection{}; POINT scroll{};
    SendMessageW(source_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(source_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    const auto modified = SendMessageW(source_, EM_GETMODIFY, 0, 0);
    // Suspend undo recording for decorations; Ctrl+Z should only undo actual text edits.
    IRichEditOle* ole = nullptr; ITextDocument* document = nullptr; long unused = 0;
    SendMessageW(source_, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&ole));
    if (ole) { ole->QueryInterface(__uuidof(ITextDocument), reinterpret_cast<void**>(&document)); ole->Release(); }
    if (document) document->Undo(tomSuspend, &unused);
    SendMessageW(source_, WM_SETREDRAW, FALSE, 0);
    auto format = [&](std::size_t start, std::size_t length, COLORREF color, bool error)
    {
        CHARRANGE range{static_cast<LONG>(start), static_cast<LONG>(start+length)};
        SendMessageW(source_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        CHARFORMAT2W style{}; style.cbSize = sizeof(style);
        style.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_UNDERLINE;
        style.crTextColor = color; style.crBackColor = error ? RGB(90,35,40) : RGB(30,32,36);
        style.dwEffects = error ? CFE_UNDERLINE : 0;
        SendMessageW(source_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&style));
    };
    format(0, text.size(), RGB(220,220,220), false);
    // Bound RichEdit format operations on pathological/token-dense files. Diagnostics still
    // scan the entire document; most small game behaviors are far below this budget.
    const auto coloredSpans = std::min<std::size_t>(analysis_.spans.size(), 8000);
    for (std::size_t i = 0; i < coloredSpans; ++i)
    {
        const auto& span = analysis_.spans[i];
        using enum zengine::scripts::TokenKind;
        const auto color = span.kind == Keyword ? RGB(195,135,220) : span.kind == Type ? RGB(90,195,195) :
            span.kind == Number ? RGB(175,210,155) : span.kind == String ? RGB(220,165,125) : RGB(105,165,100);
        format(span.start, span.length, color, false);
    }
    for (const auto& error : analysis_.errors) format(error.start, std::min<std::size_t>(error.length, text.size()-std::min(error.start,text.size())), RGB(255,190,190), true);
    SendMessageW(source_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(source_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    SendMessageW(source_, EM_SETMODIFY, modified, 0);
    if (document) { document->Undo(tomResume, &unused); document->Release(); }
    SendMessageW(source_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(source_, nullptr, FALSE);
    formatting_ = false;
#ifdef ZENGINE_SCRIPT_COMPILER
    std::wstring report = analysis_.errors.empty() ? L"No compiler errors.\r\n" : L"Script diagnostics:\r\n";
#else
    std::wstring report = L"Basic syntax checks only; compiler diagnostics available after the scripting module is merged.\r\n";
    if (analysis_.errors.empty()) report += L"No string, comment, or bracket errors found.\r\n";
#endif
    if (coloredSpans < analysis_.spans.size()) report += L"Large document: syntax coloring limited to the first 8,000 tokens.\r\n";
    for (const auto& error : analysis_.errors)
        report += L"Line " + std::to_wstring(error.line) + L", column " + std::to_wstring(error.column) + L": " + Wide(error.message) + L"\r\n";
    SetWindowTextW(errors_, report.c_str());
    EnableWindow(jump_, !analysis_.errors.empty());
}
void ScriptEditor::Layout()
{
    RECT r{}; GetClientRect(window_, &r);
    const int bottom = std::max(120L, r.bottom - 145);
    MoveWindow(save_, 8, 6, 125, 28, TRUE); MoveWindow(reload_, 140, 6, 135, 28, TRUE); MoveWindow(jump_, 282, 6, 140, 28, TRUE);MoveWindow(fold_,429,6,120,28,TRUE);MoveWindow(expand_,556,6,110,28,TRUE);
    MoveWindow(source_, 8, 42, std::max(1L,r.right-16), bottom-42, TRUE);
    MoveWindow(errors_, 8, bottom+8, std::max(1L,r.right-16), std::max(1L,r.bottom-bottom-16), TRUE);
}
LRESULT CALLBACK ScriptEditor::WindowProcedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<ScriptEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<ScriptEditor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    try { return self ? self->HandleMessage(message,w,l) : DefWindowProcW(window,message,w,l); }
    catch (const std::exception& e) { MessageBoxW(window,Wide(e.what()).c_str(),L"Script editor",MB_OK|MB_ICONERROR); return 0; }
}
LRESULT ScriptEditor::HandleMessage(UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(w),RGB(225,228,232));SetBkColor(reinterpret_cast<HDC>(w),RGB(30,32,36));return reinterpret_cast<LRESULT>(editorStyle::Shared().panel);
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        return editorStyle::ControlColor(message,w);
    case WM_SIZE: HideCompletion();Layout(); return 0;
    case WM_ACTIVATE: if(LOWORD(w)==WA_INACTIVE)HideCompletion();break;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {500,360}; return 0;
    case WM_CLOSE: if (ConfirmClose()) ShowWindow(window_, SW_HIDE); return 0;
    case WM_TIMER: if (w == 1) Highlight(); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == SourceControl && HIWORD(w) == EN_CHANGE && !formatting_)
        { if(!foldedBlocks_.empty()){formatting_=true;ExpandAll();formatting_=false;}dirty_ = true; Title(); SetTimer(window_, 1, 250, nullptr); return 0; }
        if (LOWORD(w) == SaveCommand) { Save(); return 0; }
        if(LOWORD(w)==FoldCommand){ToggleFold();return 0;}
        if(LOWORD(w)==ExpandCommand){ExpandAll();return 0;}
        if (LOWORD(w) == ReloadCommand)
        {
            if (!dirty_ || MessageBoxW(window_, L"Discard unsaved edits and reload from disk?", L"Reload script", MB_YESNO|MB_ICONWARNING) == IDYES) Reload();
            return 0;
        }
        if (LOWORD(w) == ErrorCommand)
        {
            Highlight();
            if (!analysis_.errors.empty())
            {
                CHARRANGE range{static_cast<LONG>(analysis_.errors.front().start), static_cast<LONG>(analysis_.errors.front().start+1)};
                SendMessageW(source_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
                SendMessageW(source_, EM_SCROLLCARET, 0, 0); SetFocus(source_);
            }
            return 0;
        }
        break;
    case WM_NCDESTROY: SetWindowLongPtrW(window_, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(window_,message,w,l);
}
LRESULT CALLBACK ScriptEditor::EditProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<ScriptEditor*>(data);
    if(message==WM_PAINT){const auto result=DefSubclassProc(window,message,w,l);self->PaintFoldMarkers(window);self->PaintLineNumbers(window);self->PaintCompletion(window);return result;}
    if(message==WM_LBUTTONDOWN && GET_X_LPARAM(l)>=LineNumberGutter && GET_X_LPARAM(l)<LeftGutter) {
        if(self->ToggleFoldAt({GET_X_LPARAM(l),GET_Y_LPARAM(l)})) return 0;
    }
    if(message==WM_KILLFOCUS || message==WM_LBUTTONDOWN || message==WM_VSCROLL || message==WM_HSCROLL || message==WM_MOUSEWHEEL)self->HideCompletion();
    if(message==WM_KILLFOCUS || message==WM_LBUTTONDOWN || message==WM_KEYDOWN || message==WM_VSCROLL || message==WM_HSCROLL || message==WM_MOUSEWHEEL || message==WM_MOUSELEAVE)self->HideHover();
    if(message==WM_MOUSEMOVE) {
        const POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(point.x!=self->hoverPoint_.x || point.y!=self->hoverPoint_.y) {
            self->hoverPoint_=point; self->HideHover();
            KillTimer(window,HoverTimer); SetTimer(window,HoverTimer,350,nullptr);
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        }
    }
    if(message==WM_TIMER && w==HoverTimer){KillTimer(window,HoverTimer);self->UpdateHover(self->hoverPoint_);return 0;}
    if(message==WM_KEYDOWN) {
        if(w==VK_ESCAPE && !self->completion_.items.empty()){self->HideCompletion();return 0;}
        if(w==VK_RETURN){self->HideCompletion();return 0;}
        if((w==VK_UP || w==VK_DOWN) && !self->completion_.items.empty() && self->completion_.members) {
            self->completionSelection_=(self->completionSelection_+(w==VK_DOWN?1:static_cast<int>(self->completion_.items.size())-1))%self->completion_.items.size();
            SendMessageW(self->completions_,LB_SETCURSEL,self->completionSelection_,0);InvalidateRect(window,nullptr,FALSE);return 0;
        }
        if(w==VK_LEFT || w==VK_RIGHT || w==VK_HOME || w==VK_END)self->HideCompletion();
    }
    if (message == WM_PASTE) return SendMessageW(window, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
    if (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (w == 'S' || w == 'R') { SendMessageW(self->window_, WM_COMMAND, w == 'S' ? SaveCommand : ReloadCommand, 0); return 0; }
        if (w == 'A') { SendMessageW(window, EM_SETSEL, 0, -1); return 0; }
        if(w==VK_OEM_MINUS){self->ToggleFold();return 0;}
        if(w==VK_OEM_PLUS){self->ExpandAll();return 0;}
    }
    if (message == WM_CHAR && (w == 19 || w == 18 || w == 1)) return 0;
    if (message == WM_CHAR && w == VK_TAB) {
        if(self->AcceptCompletion())return 0;
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    ")); return 0;
    }
    if (message == WM_CHAR && (w == VK_RETURN || w == '(' || w == ')' || w == '[' || w == ']' || w == '{' || w == '}' || w == ' ')) {
        CHARRANGE range{}; SendMessageW(window,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
        if (const auto edit=scriptTyping::OnCharacter(self->Text(),range.cpMin,range.cpMax,static_cast<wchar_t>(w))) {
            range={static_cast<LONG>(edit->start),static_cast<LONG>(edit->end)};
            SendMessageW(window,EM_STOPGROUPTYPING,0,0);
            SendMessageW(window,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
            SendMessageW(window,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(edit->text.c_str()));
            range.cpMin=range.cpMax=static_cast<LONG>(edit->start+edit->caret);
            SendMessageW(window,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
            SendMessageW(window,EM_SCROLLCARET,0,0);return 0;
        }
    }
    const auto result=DefSubclassProc(window,message,w,l);
    if(message==WM_CHAR && w>=32)self->UpdateCompletion();
    else if(message==WM_CHAR && w==VK_BACK)self->UpdateCompletion();
    return result;
}

bool ScriptEditor::ToggleFoldAt(POINT point)
{
    const auto source=Text(), code=scriptTyping::Code(source);
    std::vector<std::pair<std::size_t,std::size_t>> blocks; std::vector<std::size_t> stack;
    for(std::size_t i=0;i<code.size();++i)
    {
        if(code[i]==L'{') stack.push_back(i);
        else if(code[i]==L'}' && !stack.empty()) { const auto open=stack.back(); stack.pop_back(); if(i>open+1) blocks.push_back({open,i}); }
    }
    std::pair<std::size_t,std::size_t> selected{}; LONG selectedY=0; bool found=false;
    for(const auto& block:blocks)
    {
        POINTL location{}; SendMessageW(source_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&location),block.first);
        if(point.y<location.y-2 || point.y>location.y+22) continue;
        if(!found || location.y>=selectedY) { selected=block; selectedY=location.y; found=true; }
    }
    if(!found) return false;
    const bool hidden=foldedBlocks_.contains(selected.first);
    SetHidden(selected.first+1,selected.second,!hidden);
    if(hidden)foldedBlocks_.erase(selected.first);else foldedBlocks_.insert(selected.first);
    InvalidateRect(source_,nullptr,FALSE); return true;
}
void ScriptEditor::PaintFoldMarkers(HWND window)
{
    RECT client{}; GetClientRect(window,&client);
    HBRUSH gutter=CreateSolidBrush(RGB(30,32,36)); RECT area{LineNumberGutter,0,LeftGutter,client.bottom};
    // The fold strip sits between the line numbers and the text; repainted separately from RichEdit.
    HDC dc=GetDC(window); FillRect(dc,&area,gutter); DeleteObject(gutter);
    const int fx=LineNumberGutter;
    const auto source=Text(), code=scriptTyping::Code(source);
    std::vector<std::pair<std::size_t,std::size_t>> blocks; std::vector<std::size_t> stack;
    for(std::size_t i=0;i<code.size();++i)
    {
        if(code[i]==L'{') stack.push_back(i);
        else if(code[i]==L'}' && !stack.empty()) { const auto open=stack.back(); stack.pop_back(); if(i>open+1) blocks.push_back({open,i}); }
    }
    auto pen=CreatePen(PS_SOLID,1,RGB(150,156,168)); auto oldPen=SelectObject(dc,pen);
    for(const auto& block:blocks)
    {
        POINTL location{}; SendMessageW(window,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&location),block.first);
        if(location.y<-20 || location.y>client.bottom) continue;
        const int top=location.y+4; Rectangle(dc,fx+6,top,fx+20,top+14);
        MoveToEx(dc,fx+9,top+7,nullptr); LineTo(dc,fx+17,top+7);
        if(foldedBlocks_.contains(block.first)){MoveToEx(dc,fx+13,top+3,nullptr);LineTo(dc,fx+13,top+11);}
    }
    SelectObject(dc,oldPen); DeleteObject(pen); ReleaseDC(window,dc);
}
void ScriptEditor::PaintLineNumbers(HWND window)
{
    RECT client{}; GetClientRect(window,&client);
    const LONG left=0;
    HDC dc=GetDC(window);
    RECT area{left,0,LineNumberGutter,client.bottom};
    HBRUSH background=CreateSolidBrush(RGB(30,32,36)); FillRect(dc,&area,background); DeleteObject(background);
    // Thin low-contrast separator between the gutters and the text, matching the editor's other borders.
    HPEN pen=CreatePen(PS_SOLID,1,RGB(60,63,68)); auto oldPen=SelectObject(dc,pen);
    MoveToEx(dc,LeftGutter-1,0,nullptr); LineTo(dc,LeftGutter-1,client.bottom);
    SelectObject(dc,oldPen); DeleteObject(pen);
    auto oldFont=SelectObject(dc,font_); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(120,126,136));
    const auto lineCount=static_cast<LONG>(SendMessageW(window,EM_GETLINECOUNT,0,0));
    const auto firstVisible=static_cast<LONG>(SendMessageW(window,EM_GETFIRSTVISIBLELINE,0,0));
    LONG lastY=0; bool havePrevious=false;
    for(LONG line=firstVisible;line<lineCount;++line)
    {
        const auto lineStart=static_cast<LONG>(SendMessageW(window,EM_LINEINDEX,line,0));
        if(lineStart<0) break;
        POINTL location{}; SendMessageW(window,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&location),lineStart);
        if(location.y>client.bottom) break;
        if(havePrevious && location.y<=lastY) continue; // Folded/hidden lines collapse onto the previous row.
        lastY=location.y; havePrevious=true;
        wchar_t number[16]{}; const int length=wsprintfW(number,L"%d",line+1);
        RECT row{left+2,location.y,LineNumberGutter-4,location.y+64};
        DrawTextW(dc,number,length,&row,DT_RIGHT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
    }
    SelectObject(dc,oldFont); ReleaseDC(window,dc);
}
void ScriptEditor::SetHidden(std::size_t start,std::size_t end,bool hidden) {
    if(start>=end)return;CHARRANGE selection{};SendMessageW(source_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    IRichEditOle* ole=nullptr;ITextDocument* document=nullptr;long unused=0;SendMessageW(source_,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(&ole));if(ole){ole->QueryInterface(__uuidof(ITextDocument),reinterpret_cast<void**>(&document));ole->Release();}if(document)document->Undo(tomSuspend,&unused);
    CHARRANGE range{static_cast<LONG>(start),static_cast<LONG>(end)};SendMessageW(source_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_HIDDEN;format.dwEffects=hidden?CFE_HIDDEN:0;SendMessageW(source_,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&format));SendMessageW(source_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));if(document){document->Undo(tomResume,&unused);document->Release();}InvalidateRect(source_,nullptr,FALSE);
}
bool ScriptEditor::ToggleFold() {
    const auto source=Text(),code=scriptTyping::Code(source);CHARRANGE selection{};SendMessageW(source_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));const auto caret=static_cast<std::size_t>(std::max<LONG>(0,selection.cpMin));
    std::vector<std::size_t> stack;for(std::size_t i=0;i<std::min(caret+1,code.size());++i){if(code[i]==L'{')stack.push_back(i);else if(code[i]==L'}'&&!stack.empty())stack.pop_back();}
    std::size_t open=stack.empty()?code.find(L'{',std::min(caret,code.size())):stack.back();if(open==code.npos)return false;int depth=1;std::size_t close=open+1;for(;close<code.size()&&depth;++close){if(code[close]==L'{')++depth;else if(code[close]==L'}')--depth;}if(depth||close<=open+2)return false;--close;
    const bool hidden=foldedBlocks_.contains(open);SetHidden(open+1,close,!hidden);if(hidden)foldedBlocks_.erase(open);else foldedBlocks_.insert(open);return true;
}
void ScriptEditor::ExpandAll(){SetHidden(0,Text().size(),false);foldedBlocks_.clear();InvalidateRect(source_,nullptr,FALSE);}

void ScriptEditor::RefreshCompletionIndex() {
    completionIndex_=scriptCompletion::Index{};std::size_t bytes=0,count=0;
    if (hlsl_) return; // HLSL has no zEngine-script autocomplete index
    // Filesystem reads happen on open/save, not every keystroke. Stay project-contained.
    std::error_code error;
    for(std::filesystem::recursive_directory_iterator it(assets_,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end && !error;it.increment(error)) {
        if(it->is_symlink(error)){it.disable_recursion_pending();continue;}
        if(!it->is_regular_file(error)||!zengine::scripts::IsScript(it->path())||it->path()==path_)continue;
        if(++count>512)break;
        try {const auto text=zengine::scripts::Load(zengine::scripts::Resolve(assets_,it->path()));bytes+=text.size();if(bytes>8*1024*1024)break;completionIndex_.AddSource(Wide(text));}catch(const std::exception&){}
    }
    if(completionContext_)for(auto& value:completionContext_())completionIndex_.AddString(std::move(value));
}
void ScriptEditor::HideCompletion() {
    completion_={};completionSelection_=0;if(completions_)ShowWindow(completions_,SW_HIDE);if(source_)InvalidateRect(source_,nullptr,FALSE);
}
void ScriptEditor::HideHover() {
    if(!tooltip_ || hoverText_.empty())return;
    hoverText_.clear();
    TOOLINFOW info{sizeof(info)}; info.hwnd=window_; info.uId=reinterpret_cast<UINT_PTR>(source_);
    SendMessageW(tooltip_,TTM_TRACKACTIVATE,FALSE,reinterpret_cast<LPARAM>(&info));
}
void ScriptEditor::UpdateHover(POINT clientPoint) {
    if(!tooltip_ || !source_ || (completions_ && IsWindowVisible(completions_)))return;
    POINTL pt{clientPoint.x,clientPoint.y};
    const auto index=static_cast<LONG>(SendMessageW(source_,EM_CHARFROMPOS,0,reinterpret_cast<LPARAM>(&pt)));
    if(index<0)return;
    // Only show when the pointer is actually over the character cell, not past line end.
    POINTL cell{}; SendMessageW(source_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&cell),index);
    if(clientPoint.x<cell.x-2)return;
    const auto tip=completionIndex_.Hover(Text(),static_cast<std::size_t>(index));
    if(tip.empty() || tip==hoverText_)return;
    hoverText_=tip;
    TOOLINFOW info{sizeof(info)}; info.hwnd=window_; info.uId=reinterpret_cast<UINT_PTR>(source_);
    info.lpszText=hoverText_.data();
    SendMessageW(tooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));
    POINT screen=clientPoint; ClientToScreen(source_,&screen);
    SendMessageW(tooltip_,TTM_TRACKPOSITION,0,MAKELPARAM(screen.x+14,screen.y+20));
    SendMessageW(tooltip_,TTM_TRACKACTIVATE,TRUE,reinterpret_cast<LPARAM>(&info));
}
void ScriptEditor::UpdateCompletion() {
    if(suppressCompletion_ || formatting_ || hlsl_)return;
    CHARRANGE range{};SendMessageW(source_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
    if(range.cpMin!=range.cpMax){HideCompletion();return;}
    completion_=completionIndex_.Complete(Text(),range.cpMin);completionSelection_=0;
    ShowWindow(completions_,SW_HIDE);
    if(completion_.members && !completion_.items.empty()) {
        SendMessageW(completions_,LB_RESETCONTENT,0,0);
        for(const auto& item:completion_.items){const auto label=item.name+(item.function?L"()":L"")+L"  : "+item.type;SendMessageW(completions_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
        SendMessageW(completions_,LB_SETCURSEL,0,0);
        POINTL caret{};SendMessageW(source_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&caret),range.cpMin);
        POINT pos{caret.x,caret.y+22};ClientToScreen(source_,&pos);
        MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(pos,MONITOR_DEFAULTTONEAREST),&info);
        const int height=std::min(8,static_cast<int>(completion_.items.size()))*22+4;
        pos.x=std::clamp(pos.x,info.rcWork.left,std::max(info.rcWork.left,info.rcWork.right-340));
        pos.y=std::clamp(pos.y,info.rcWork.top,std::max(info.rcWork.top,info.rcWork.bottom-height));
        SetWindowPos(completions_,HWND_TOP,pos.x,pos.y,340,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    InvalidateRect(source_,nullptr,FALSE);
}
bool ScriptEditor::AcceptCompletion() {
    if(completion_.items.empty())return false;
    CHARRANGE range{};SendMessageW(source_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
    if(range.cpMin!=range.cpMax || range.cpMin!=completion_.start+completion_.prefix.size()){HideCompletion();return false;}
    const auto text=Text();if(text.substr(completion_.start,completion_.prefix.size())!=completion_.prefix){HideCompletion();return false;}
    const auto value=completion_.items.at(completionSelection_).name;
    range.cpMin=static_cast<LONG>(completion_.start);suppressCompletion_=true;
    SendMessageW(source_,EM_STOPGROUPTYPING,0,0);SendMessageW(source_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
    SendMessageW(source_,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(value.c_str()));
    suppressCompletion_=false;HideCompletion();return true;
}
void ScriptEditor::PaintCompletion(HWND window) {
    if(completion_.items.empty() || GetFocus()!=window)return;
    CHARRANGE range{};SendMessageW(window,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
    if(range.cpMin!=range.cpMax || range.cpMin!=completion_.start+completion_.prefix.size())return;
    const auto text=Text();if(range.cpMin<text.size() && text[range.cpMin]!=L'\r' && text[range.cpMin]!=L'\n')return;
    const auto suffix=completion_.items.at(completionSelection_).name.substr(completion_.prefix.size());
    POINTL point{};SendMessageW(window,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&point),range.cpMin);
    HDC dc=GetDC(window);auto old=SelectObject(dc,font_);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(130,136,146));
    RECT clip{};SendMessageW(window,EM_GETRECT,0,reinterpret_cast<LPARAM>(&clip));IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
    TextOutW(dc,point.x,point.y,suffix.c_str(),static_cast<int>(suffix.size()));SelectObject(dc,old);ReleaseDC(window,dc);
}
LRESULT CALLBACK ScriptEditor::CompletionProcedure(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
    auto* self=reinterpret_cast<ScriptEditor*>(data);
    if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_LBUTTONDOWN) {
        const auto index=SendMessageW(window,LB_ITEMFROMPOINT,0,l);
        if(!HIWORD(index))SendMessageW(window,LB_SETCURSEL,LOWORD(index),0);
        return 0; // Keep keyboard focus/caret in RichEdit.
    }
    if(message==WM_LBUTTONUP) {
        const auto index=SendMessageW(window,LB_ITEMFROMPOINT,0,l);
        if(!HIWORD(index) && LOWORD(index)<self->completion_.items.size()){self->completionSelection_=LOWORD(index);self->AcceptCompletion();}
        return 0;
    }
    return DefSubclassProc(window,message,w,l);
}
