#include "LauncherWindow.h"
#include "NewProjectDialog.h"
#include "Style.h"

#include <algorithm>
#include <stdexcept>

namespace zlauncher
{
namespace
{
constexpr wchar_t kMainClass[] = L"zLauncherMain";
constexpr wchar_t kListClass[] = L"zLauncherList";

constexpr int kTopBar = 58;
constexpr int kSectionBar = 26;      // uppercase accent section header strip
constexpr int kListTop = kTopBar + kSectionBar;
constexpr int kStatusBar = 26;
constexpr int kRowHeight = 86;
constexpr int kRowButtonW = 76;
constexpr int kRowButtonH = 30;
constexpr int kRowButtonGap = 8;
constexpr int kEdgePad = 16;

constexpr int ID_UPDATE = 100;
constexpr int ID_NEW = 101;
constexpr int kRowButtons = 4;    // Play, Edit, Build, Delete
constexpr int ID_ROW_BASE = 1000; // row i: play = base+i*4, edit +1, build +2, delete +3

std::wstring Widen(const char* text)
{
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

HFONT MakeFont(int height, int weight)
{
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Segoe UI");
}
} // namespace

LauncherWindow::LauncherWindow(HINSTANCE instance) : instance_(instance)
{
    titleFont_ = MakeFont(-20, FW_SEMIBOLD);
    nameFont_ = MakeFont(-14, FW_SEMIBOLD);
    bodyFont_ = MakeFont(-12, FW_NORMAL);
    headerFont_ = MakeFont(-11, FW_SEMIBOLD);
    status_ = L"Ready.";

#ifdef ZLAUNCHER_VERSION
    launcherVersion_ = Widen(ZLAUNCHER_VERSION);
#endif
}

HWND LauncherWindow::Create(int showCommand)
{
    WNDCLASSEXW main{ sizeof(main) };
    main.lpfnWndProc = &LauncherWindow::MainProc;
    main.hInstance = instance_;
    main.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main.hbrBackground = lstyle::Shared().panel;
    main.lpszClassName = kMainClass;
    RegisterClassExW(&main);

    WNDCLASSEXW list{ sizeof(list) };
    list.lpfnWndProc = &LauncherWindow::ListProc;
    list.hInstance = instance_;
    list.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    list.hbrBackground = lstyle::Shared().window;
    list.lpszClassName = kListClass;
    RegisterClassExW(&list);

    main_ = CreateWindowExW(0, kMainClass, L"zEngine Launcher",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 940, 620,
                            nullptr, nullptr, instance_, this);
    if (!main_) throw std::runtime_error("Cannot create the launcher window.");

    store_.Load();
    store_.RunFirstTimeSetup();

    // Make sure an editor is installed under "C:\Program Files\z engine\versions"
    // and the Start-menu / Desktop shortcuts point at it. A fresh download only
    // happens automatically when there is no editor anywhere yet (and the user
    // agrees); an already-installed editor just gets a quick, non-fatal check.
    {
        const bool haveEditor = ProjectStore::LocateEditor().has_value();
        bool allowDownload = false;
        if (!haveEditor)
            allowDownload = MessageBoxW(main_,
                                        L"No zEngine editor is installed yet.\r\n\r\nDownload the "
                                        L"latest editor release from GitHub now?",
                                        L"Install zEngine", MB_YESNO | MB_ICONQUESTION) == IDYES;
        std::wstring message;
        bool changed = false;
        HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        const bool ok = store_.EnsureEditorInstalled(message, changed, allowDownload);
        SetCursor(previous);
        // Only surface this at rest when it is actionable: we just installed
        // something, or there is no editor at all.
        if (!message.empty() && (changed || !haveEditor)) status_ = message;
        // Never fail an explicit download silently - report the outcome.
        if (allowDownload && !message.empty())
            MessageBoxW(main_, message.c_str(), L"Install zEngine",
                        (ok && changed) ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
    }

    editorVersion_ = ProjectStore::EditorVersion();
    RebuildRows();

    ShowWindow(main_, showCommand);
    UpdateWindow(main_);
    return main_;
}

bool LauncherWindow::PreTranslate(MSG& message)
{
    return main_ && IsDialogMessageW(main_, &message);
}

LRESULT CALLBACK LauncherWindow::MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    LauncherWindow* self = nullptr;
    if (m == WM_NCCREATE)
    {
        self = static_cast<LauncherWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->main_ = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(w, m, wp, lp);
    return self->OnMain(m, wp, lp);
}

LRESULT CALLBACK LauncherWindow::ListProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (!self) return DefWindowProcW(w, m, wp, lp);
    return self->OnList(m, wp, lp);
}

LRESULT LauncherWindow::OnMain(UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_CREATE:
    {
        list_ = CreateWindowExW(0, kListClass, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                                0, kListTop, 100, 100, main_, nullptr, instance_, nullptr);
        SetWindowLongPtrW(list_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        btnUpdate_ = CreateWindowExW(0, L"BUTTON", L"Update",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 90, 32, main_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_UPDATE)),
                                     instance_, nullptr);
        btnNew_ = CreateWindowExW(0, L"BUTTON", L"+ New Project",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 130, 32, main_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NEW)),
                                  instance_, nullptr);
        lstyle::Attach(btnUpdate_);
        lstyle::Attach(btnNew_);
        LayoutChildren();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren();
        RepositionRows();
        UpdateScrollBar();
        return 0;

    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = { 660, 440 };
        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        return lstyle::ControlColor(m, wp);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(main_, &ps);
        RECT client;
        GetClientRect(main_, &client);
        PaintTopBar(dc, client);
        PaintSection(dc, client);
        PaintStatus(dc, client);
        EndPaint(main_, &ps);
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id == ID_UPDATE) { OnUpdate(); return 0; }
        if (id == ID_NEW) { OnNewProject(); return 0; }
        return 0;
    }

    case WM_DESTROY:
        DeleteObject(titleFont_);
        DeleteObject(nameFont_);
        DeleteObject(bodyFont_);
        DeleteObject(headerFont_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(main_, m, wp, lp);
}

LRESULT LauncherWindow::OnList(UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        return lstyle::ControlColor(m, wp);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(list_, &ps);
        PaintList(dc);
        EndPaint(list_, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const int y = static_cast<short>(HIWORD(lp));
        const int count = static_cast<int>(store_.Projects().size());
        int row = (y + scroll_) / kRowHeight;
        if (row < 0 || row >= count) row = -1;
        if (row != hoverRow_)
        {
            hoverRow_ = row;
            InvalidateRect(list_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, list_, 0 };
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hoverRow_ != -1)
        {
            hoverRow_ = -1;
            InvalidateRect(list_, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SCROLLINFO si{ sizeof(si), SIF_POS | SIF_RANGE | SIF_PAGE };
        GetScrollInfo(list_, SB_VERT, &si);
        int pos = si.nPos - delta / 120 * (kRowHeight / 2);
        pos = std::clamp(pos, si.nMin, std::max(si.nMin, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos != si.nPos)
        {
            scroll_ = pos;
            SetScrollPos(list_, SB_VERT, pos, TRUE);
            RepositionRows();
            InvalidateRect(list_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_VSCROLL:
    {
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(list_, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp))
        {
        case SB_LINEUP: pos -= kRowHeight / 2; break;
        case SB_LINEDOWN: pos += kRowHeight / 2; break;
        case SB_PAGEUP: pos -= si.nPage; break;
        case SB_PAGEDOWN: pos += si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        pos = std::clamp(pos, si.nMin, std::max(si.nMin, si.nMax - static_cast<int>(si.nPage) + 1));
        if (pos != si.nPos)
        {
            scroll_ = pos;
            SetScrollPos(list_, SB_VERT, pos, TRUE);
            RepositionRows();
            InvalidateRect(list_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id >= ID_ROW_BASE) OnRowCommand(id);
        return 0;
    }
    }
    return DefWindowProcW(list_, m, wp, lp);
}

void LauncherWindow::LayoutChildren()
{
    RECT client;
    GetClientRect(main_, &client);
    const int width = client.right;
    const int height = client.bottom;

    const int by = (kTopBar - 32) / 2;
    int right = width - kEdgePad;
    MoveWindow(btnNew_, right - 130, by, 130, 32, TRUE);
    right -= 130 + kRowButtonGap;
    MoveWindow(btnUpdate_, right - 90, by, 90, 32, TRUE);

    MoveWindow(list_, 0, kListTop, width, std::max(0, height - kListTop - kStatusBar), TRUE);
}

void LauncherWindow::RebuildRows()
{
    for (auto& row : rows_)
    {
        DestroyWindow(row.play);
        DestroyWindow(row.edit);
        DestroyWindow(row.build);
        DestroyWindow(row.del);
    }
    rows_.clear();

    const auto& projects = store_.Projects();
    rows_.reserve(projects.size());
    for (std::size_t i = 0; i < projects.size(); ++i)
    {
        const int base = ID_ROW_BASE + static_cast<int>(i) * kRowButtons;
        auto make = [&](const wchar_t* label, int id) {
            HWND button = CreateWindowExW(0, L"BUTTON", label,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          0, 0, kRowButtonW, kRowButtonH, list_,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                          instance_, nullptr);
            lstyle::Attach(button);
            return button;
        };
        Row row;
        row.play = make(L"Play", base + 0);
        row.edit = make(L"Edit", base + 1);
        row.build = make(L"Build", base + 2);
        row.del = make(L"Delete", base + 3);

        // Play is available only when there is a build to run.
        EnableWindow(row.play, ProjectStore::PlayableExe(projects[i]).has_value());
        if (!projects[i].valid)
        {
            EnableWindow(row.edit, FALSE);
            EnableWindow(row.build, FALSE);
        }
        rows_.push_back(row);
    }

    scroll_ = 0;
    SetScrollPos(list_, SB_VERT, 0, FALSE);
    RepositionRows();
    UpdateScrollBar();
    InvalidateRect(list_, nullptr, TRUE);
}

void LauncherWindow::RepositionRows()
{
    RECT client;
    GetClientRect(list_, &client);
    const int width = client.right;

    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const int top = static_cast<int>(i) * kRowHeight - scroll_;
        const int y = top + (kRowHeight - kRowButtonH) / 2;
        int x = width - kEdgePad - kRowButtonW;
        MoveWindow(rows_[i].del, x, y, kRowButtonW, kRowButtonH, TRUE);
        x -= kRowButtonW + kRowButtonGap;
        MoveWindow(rows_[i].build, x, y, kRowButtonW, kRowButtonH, TRUE);
        x -= kRowButtonW + kRowButtonGap;
        MoveWindow(rows_[i].edit, x, y, kRowButtonW, kRowButtonH, TRUE);
        x -= kRowButtonW + kRowButtonGap;
        MoveWindow(rows_[i].play, x, y, kRowButtonW, kRowButtonH, TRUE);
    }
}

void LauncherWindow::UpdateScrollBar()
{
    RECT client;
    GetClientRect(list_, &client);
    const int page = std::max(1, static_cast<int>(client.bottom));
    const int content = static_cast<int>(store_.Projects().size()) * kRowHeight;

    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0;
    si.nMax = std::max(0, content - 1);
    si.nPage = static_cast<UINT>(page);
    si.nPos = std::clamp(scroll_, 0, std::max(0, content - page));
    scroll_ = si.nPos;
    SetScrollInfo(list_, SB_VERT, &si, TRUE);
    RepositionRows();
}

void LauncherWindow::PaintTopBar(HDC dc, const RECT& client)
{
    RECT bar{ 0, 0, client.right, kTopBar };
    lstyle::Fill(dc, bar, lstyle::Panel);
    RECT edge{ 0, kTopBar - 1, client.right, kTopBar };
    lstyle::Fill(dc, edge, lstyle::Border);

    SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, titleFont_);
    SetTextColor(dc, lstyle::Text);
    RECT title{ kEdgePad, 0, client.right, kTopBar };
    DrawTextW(dc, L"zEngine", -1, &title, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    SIZE size{};
    GetTextExtentPoint32W(dc, L"zEngine", 7, &size);
    SelectObject(dc, bodyFont_);
    SetTextColor(dc, lstyle::Muted);
    RECT sub{ kEdgePad + size.cx + 10, 0, client.right, kTopBar };
    DrawTextW(dc, L"Launcher", -1, &sub, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, old);
}

void LauncherWindow::PaintSection(HDC dc, const RECT& client)
{
    RECT bar{ 0, kTopBar, client.right, kListTop };
    lstyle::Fill(dc, bar, lstyle::Window);
    // The "underline rule" beneath the header, drawn in the accent colour.
    RECT rule{ 0, kListTop - 1, client.right, kListTop };
    lstyle::Fill(dc, rule, lstyle::Accent);

    SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, headerFont_);
    SetTextColor(dc, lstyle::Accent);
    RECT label{ kEdgePad, kTopBar, client.right - kEdgePad, kListTop - 1 };
    const int count = static_cast<int>(store_.Projects().size());
    std::wstring header = L"PROJECTS";
    if (count > 0) header += L"  (" + std::to_wstring(count) + L")";
    DrawTextW(dc, header.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, old);
}

void LauncherWindow::PaintStatus(HDC dc, const RECT& client)
{
    RECT bar{ 0, client.bottom - kStatusBar, client.right, client.bottom };
    lstyle::Fill(dc, bar, lstyle::Panel);
    RECT edge{ 0, bar.top, client.right, bar.top + 1 };
    lstyle::Fill(dc, edge, lstyle::Border);

    SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, bodyFont_);
    SetTextColor(dc, lstyle::Muted);

    // Right side: launcher + editor versions.
    std::wstring versions;
    if (!launcherVersion_.empty()) versions = L"zLauncher " + launcherVersion_;
    if (!editorVersion_.empty())
        versions += (versions.empty() ? L"" : L"   \x2022   ") + std::wstring(L"Editor ") + editorVersion_;

    int versionsWidth = 0;
    if (!versions.empty())
    {
        SIZE size{};
        GetTextExtentPoint32W(dc, versions.c_str(), static_cast<int>(versions.size()), &size);
        versionsWidth = size.cx;
        RECT vr{ client.right - kEdgePad - versionsWidth, bar.top, client.right - kEdgePad, bar.bottom };
        DrawTextW(dc, versions.c_str(), -1, &vr,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    RECT text{ kEdgePad, bar.top,
               client.right - kEdgePad - (versionsWidth ? versionsWidth + 24 : 0), bar.bottom };
    DrawTextW(dc, status_.c_str(), -1, &text,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void LauncherWindow::PaintList(HDC target)
{
    RECT client;
    GetClientRect(list_, &client);
    const int width = client.right;
    const int height = client.bottom;

    // Double buffer: scrolling repaints the whole column.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1, width), std::max(1, height));
    auto oldBmp = SelectObject(dc, bmp);

    RECT all{ 0, 0, width, height };
    lstyle::Fill(dc, all, lstyle::Window);
    SetBkMode(dc, TRANSPARENT);

    const auto& projects = store_.Projects();
    if (projects.empty())
    {
        auto old = SelectObject(dc, bodyFont_);
        SetTextColor(dc, lstyle::Muted);
        RECT text = all;
        DrawTextW(dc, L"No projects yet. Click “+ New Project” to create one.", -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    for (std::size_t i = 0; i < projects.size(); ++i)
    {
        const int top = static_cast<int>(i) * kRowHeight - scroll_;
        if (top + kRowHeight < 0 || top > height) continue;

        const auto& project = projects[i];

        // Blue-tinted selection with a left accent bar for the hovered row.
        if (static_cast<int>(i) == hoverRow_)
        {
            RECT rowRect{ 0, top, width, top + kRowHeight - 1 };
            lstyle::Fill(dc, rowRect, lstyle::Selection);
            RECT accentBar{ 0, top, 3, top + kRowHeight - 1 };
            lstyle::Fill(dc, accentBar, lstyle::Accent);
        }

        const int textRight = width - kEdgePad - kRowButtons * (kRowButtonW + kRowButtonGap);

        auto old = SelectObject(dc, nameFont_);
        SetTextColor(dc, project.valid ? lstyle::Text : lstyle::Muted);
        RECT name{ kEdgePad, top + 10, textRight, top + 32 };
        std::wstring label = project.name;
        if (!project.valid) label += L"   — .zproject not found";
        DrawTextW(dc, label.c_str(), -1, &name, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

        SelectObject(dc, bodyFont_);
        SetTextColor(dc, lstyle::Muted);
        RECT path{ name.left, top + 33, textRight, top + 51 };
        DrawTextW(dc, project.folder.wstring().c_str(), -1, &path,
                  DT_SINGLELINE | DT_LEFT | DT_PATH_ELLIPSIS | DT_NOPREFIX);

        // Metadata line: only the pieces we actually have.
        std::wstring meta;
        auto add = [&](const std::wstring& piece) {
            if (piece.empty()) return;
            if (!meta.empty()) meta += L"   \x2022   ";
            meta += piece;
        };
        auto dateOnly = [](const std::wstring& stamp) {
            return stamp.size() >= 10 ? stamp.substr(0, 10) : stamp;
        };
        if (!project.version.empty()) add(L"v" + project.version);
        if (!project.created.empty()) add(L"Created " + dateOnly(project.created));
        if (!project.launched.empty()) add(L"Launched " + dateOnly(project.launched));
        if (!project.built.empty()) add(L"Built " + dateOnly(project.built));
        if (!meta.empty())
        {
            RECT metaRect{ name.left, top + 52, textRight, top + 70 };
            DrawTextW(dc, meta.c_str(), -1, &metaRect,
                      DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, old);

        RECT line{ 0, top + kRowHeight - 1, width, top + kRowHeight };
        lstyle::Fill(dc, line, lstyle::RowLine);
    }

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void LauncherWindow::SetStatus(std::wstring text)
{
    status_ = std::move(text);
    RECT client;
    GetClientRect(main_, &client);
    RECT bar{ 0, client.bottom - kStatusBar, client.right, client.bottom };
    InvalidateRect(main_, &bar, FALSE);
}

void LauncherWindow::OnUpdate()
{
    // The launcher updates itself first: a stale launcher should be replaced
    // before it touches anything else.
    SetStatus(L"Checking for a launcher update…");
    UpdateWindow(main_);
    {
        HCURSOR wait = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::wstring message;
        bool started = false;
        const bool ok = store_.LauncherUpdate(message, started);
        SetCursor(wait);

        if (started)
        {
            SetStatus(message);
            MessageBoxW(main_, L"The launcher will now close, update, and reopen.", L"Update Launcher",
                        MB_OK | MB_ICONINFORMATION);
            DestroyWindow(main_);
            return;
        }
        if (!ok)
        {
            SetStatus(message);
            MessageBoxW(main_, message.c_str(), L"Update Launcher", MB_OK | MB_ICONWARNING);
            return;
        }
        // ok && !started: launcher is current -> fall through to the editor.
    }

    if (MessageBoxW(main_,
                    L"The launcher is up to date. Check GitHub for a newer editor build and "
                    L"install it to \"C:\\Program Files\\zEngine\" (or your user folder if that "
                    L"needs administrator rights)?\r\n\r\nClose the editor first.",
                    L"Update Editor", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        SetStatus(L"Ready.");
        return;
    }

    SetStatus(L"Contacting GitHub and downloading the latest editor release…");
    UpdateWindow(main_);

    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring message;
    bool changed = false;
    const bool ok = store_.EnsureEditorInstalled(message, changed, true);
    SetCursor(previous);

    editorVersion_ = ProjectStore::EditorVersion();
    InvalidateRect(main_, nullptr, FALSE);
    SetStatus(message);
    if (!ok)
        MessageBoxW(main_, message.c_str(), L"Update Editor", MB_OK | MB_ICONWARNING);
}

void LauncherWindow::OnNewProject()
{
    try
    {
        auto templates = store_.Templates();
        std::vector<std::wstring> names;
        names.reserve(templates.size());
        for (const auto& t : templates) names.push_back(t.name);

        auto request = ShowNewProjectDialog(main_, names);
        if (!request) return;

        const ProjectTemplate* tmpl = nullptr;
        if (request->templateIndex >= 0 &&
            request->templateIndex < static_cast<int>(templates.size()))
            tmpl = &templates[static_cast<std::size_t>(request->templateIndex)];

        ProjectEntry entry = store_.Create(request->location, request->name, tmpl);
        const std::size_t index = store_.Projects().size() - 1;
        RebuildRows();

        std::wstring error;
        if (store_.LaunchEditor(entry, error))
        {
            store_.MarkLaunched(index);
            InvalidateRect(list_, nullptr, FALSE);
            SetStatus(L"Created “" + entry.name + L"” and opened the editor.");
        }
        else
        {
            SetStatus(L"Created “" + entry.name + L"”. " + error);
        }
    }
    catch (const std::exception& e)
    {
        MessageBoxW(main_, Widen(e.what()).c_str(), L"New Project", MB_OK | MB_ICONERROR);
    }
}

void LauncherWindow::OnRowCommand(int commandId)
{
    const int offset = commandId - ID_ROW_BASE;
    const std::size_t index = static_cast<std::size_t>(offset / kRowButtons);
    const int action = offset % kRowButtons;
    if (index >= store_.Projects().size()) return;

    const ProjectEntry entry = store_.Projects()[index];

    if (action == 0) // Play
    {
        std::wstring error;
        if (ProjectStore::PlayProject(entry, error))
            SetStatus(L"Running the current build of “" + entry.name + L"”…");
        else
            MessageBoxW(main_, error.c_str(), L"Play", MB_OK | MB_ICONWARNING);
    }
    else if (action == 1) // Edit
    {
        std::wstring error;
        if (store_.LaunchEditor(entry, error))
        {
            store_.MarkLaunched(index);
            InvalidateRect(list_, nullptr, FALSE);
            SetStatus(L"Opening “" + entry.name + L"” in the editor…");
        }
        else
        {
            MessageBoxW(main_, error.c_str(), L"Open Project", MB_OK | MB_ICONWARNING);
        }
    }
    else if (action == 2) // Build
    {
        std::wstring error;
        if (store_.BuildProject(entry, error))
            SetStatus(L"Built “" + entry.name + L"”.");
        else
            SetStatus(std::move(error));
    }
    else if (action == 3) // Delete
    {
        std::wstring prompt = L"Remove “" + entry.name +
                              L"” from the launcher?\r\n\r\nThe project files on disk are not deleted.";
        if (MessageBoxW(main_, prompt.c_str(), L"Remove Project", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            store_.Remove(index);
            RebuildRows();
            SetStatus(L"Removed “" + entry.name + L"” from the list.");
        }
    }
}
}
