#include "ScriptEditor.h"
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <algorithm>
#include <stdexcept>
#ifdef ZENGINE_SCRIPT_COMPILER
#include "zscript/Script.h"
#endif

namespace
{
    constexpr wchar_t ClassName[] = L"zEngineScriptEditor";
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
ScriptEditor::ScriptEditor(HWND owner, const std::filesystem::path& assets, const std::filesystem::path& path)
    : assets_(assets), path_(zengine::scripts::Resolve(assets, path))
{
    // Validate before opening a window. No partial document on failed reads.
    loaded_ = zengine::scripts::Load(path_);
    richEdit_ = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit_) throw std::runtime_error("Windows RichEdit is unavailable.");
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.hInstance = instance; type.lpfnWndProc = WindowProcedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = ClassName;
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    { FreeLibrary(richEdit_); throw std::runtime_error("Cannot register script editor window."); }
    window_ = CreateWindowExW(0, ClassName, L"Script Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
        if (!source_ || !errors_ || !save_ || !reload_ || !jump_) throw std::runtime_error("Cannot create script editor controls.");
        SendMessageW(source_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(errors_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(source_, EM_SETBKGNDCOLOR, 0, RGB(30,32,36));
        SendMessageW(source_, EM_EXLIMITTEXT, 0, zengine::scripts::MaxSourceBytes);
        SendMessageW(source_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        // Syntax formatting needs rich-text mode; clipboard paste is restricted to plain text below.
        SendMessageW(source_, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
        SetWindowSubclass(source_, EditProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
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
    if (!dirty_) Reload();
    ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(source_);
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
void ScriptEditor::Reload()
{
    const auto disk = zengine::scripts::Load(path_);
    auto text = Wide(disk);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());
    formatting_ = true;
    SetWindowTextW(source_, text.c_str());
    SendMessageW(source_, EM_EMPTYUNDOBUFFER, 0, 0);
    formatting_ = false;
    loaded_ = disk; dirty_ = false; Title(); Highlight();
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
        zengine::scripts::Save(assets_, path_, bytes, &loaded_);
        loaded_ = bytes; dirty_ = false; Title(); Highlight();
        if (saved_) saved_();
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
    analysis_ = zengine::scripts::Analyze(text);
#ifdef ZENGINE_SCRIPT_COMPILER
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
    MoveWindow(save_, 8, 6, 125, 28, TRUE); MoveWindow(reload_, 140, 6, 135, 28, TRUE); MoveWindow(jump_, 282, 6, 140, 28, TRUE);
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
    case WM_SIZE: Layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {500,360}; return 0;
    case WM_CLOSE: if (ConfirmClose()) ShowWindow(window_, SW_HIDE); return 0;
    case WM_TIMER: if (w == 1) Highlight(); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == SourceControl && HIWORD(w) == EN_CHANGE && !formatting_)
        { dirty_ = true; Title(); SetTimer(window_, 1, 250, nullptr); return 0; }
        if (LOWORD(w) == SaveCommand) { Save(); return 0; }
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
    if (message == WM_PASTE) return SendMessageW(window, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
    if (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (w == 'S' || w == 'R') { SendMessageW(self->window_, WM_COMMAND, w == 'S' ? SaveCommand : ReloadCommand, 0); return 0; }
        if (w == 'A') { SendMessageW(window, EM_SETSEL, 0, -1); return 0; }
    }
    if (message == WM_CHAR && (w == 19 || w == 18 || w == 1)) return 0;
    if (message == WM_CHAR && w == VK_TAB) { SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    ")); return 0; }
    return DefSubclassProc(window,message,w,l);
}
