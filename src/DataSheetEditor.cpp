#include "DataSheetEditor.h"
#include "EditorStyle.h"

#include <algorithm>
#include <stdexcept>

namespace
{
    std::wstring Wide(const std::string& s)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L' ');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    std::string Utf8(HWND w)
    {
        const int n = GetWindowTextLengthW(w);
        std::wstring s(n + 1, L'\0');
        GetWindowTextW(w, s.data(), n + 1); s.resize(n);
        const int m = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string o(m, ' ');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), m, nullptr, nullptr);
        return o;
    }
    constexpr int MaxRows = 200;
}

DataSheetEditor::DataSheetEditor(HWND owner, std::filesystem::path assetsRoot, std::filesystem::path file,
                                 std::vector<std::pair<std::string, std::string>> columns)
    : assetsRoot_(std::move(assetsRoot)), file_(std::move(file)), columns_(std::move(columns))
{
    doc_ = loaded_ = zengine::datasheet::Load(file_);

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = L"zEngineDataSheetEditor";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = editorStyle::Shared().panel;
    RegisterClassW(&wc);

    window_ = CreateWindowExW(0, wc.lpszClassName, L"Data Sheet", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 720, 520, owner, nullptr, wc.hInstance, this);
    if (!window_) throw std::runtime_error("Cannot open the Data Sheet editor.");

    const auto btn = [&](const wchar_t* text, int id) {
        HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return b;
    };
    btn(L"Add Row", AddRow);
    btn(L"Delete Row", DeleteRow);
    btn(L"Save", Save);
    btn(L"Reload", Reload);
    status_ = CreateWindowExW(0, L"STATIC", L"Rows are instances (keyed by name); columns are the struct's fields.",
                              WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window_, reinterpret_cast<HMENU>(4590), wc.hInstance, nullptr);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);

    Rebuild();
    Layout();
    Title();
}

DataSheetEditor::~DataSheetEditor() { if (IsWindow(window_)) DestroyWindow(window_); }

void DataSheetEditor::Title()
{
    SetWindowTextW(window_, ((dirty_ ? L"* " : L"") + file_.filename().wstring() + L" - " + Wide(doc_.type) + L" Data Sheet").c_str());
}

void DataSheetEditor::Show()
{
    if (!dirty_) LoadFile();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
}

void DataSheetEditor::Rebuild()
{
    loading_ = true;
    for (HWND h : cells_) if (h) DestroyWindow(h);
    for (HWND h : headers_) if (h) DestroyWindow(h);
    cells_.clear(); headers_.clear();
    HINSTANCE inst = GetModuleHandleW(nullptr);

    const auto header = [&](const std::wstring& text) {
        HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window_, nullptr, inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        headers_.push_back(h);
    };
    header(L"Row");
    for (const auto& [name, type] : columns_) header(Wide(name + "  (" + type + ")"));

    if (doc_.rows.size() > MaxRows) doc_.rows.resize(MaxRows);
    int id = FirstCell;
    for (std::size_t r = 0; r < doc_.rows.size(); ++r)
    {
        const auto& row = doc_.rows[r];
        const auto edit = [&](const std::wstring& text) {
            HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                     0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id++)), inst, nullptr);
            SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
            cells_.push_back(e);
        };
        edit(Wide(row.key));
        for (const auto& [name, type] : columns_)
        {
            std::string value;
            for (const auto& c : row.cells) if (c.name == name) { value = c.value; break; }
            edit(Wide(value));
        }
    }
    editorStyle::AttachChildren(window_);
    loading_ = false;
    Layout();
}

void DataSheetEditor::ReadGrid()
{
    const int stride = Stride();
    for (std::size_t r = 0; r < doc_.rows.size(); ++r)
    {
        auto& row = doc_.rows[r];
        row.key = Utf8(cells_[r * stride]);
        row.cells.clear();
        for (std::size_t c = 0; c < columns_.size(); ++c)
            row.cells.push_back({columns_[c].first, columns_[c].second, Utf8(cells_[r * stride + 1 + c])});
    }
    // Drop rows whose key was cleared; keep at least the remaining order.
    std::erase_if(doc_.rows, [](const zengine::datasheet::Row& r) { return r.key.empty(); });
}

void DataSheetEditor::LoadFile()
{
    doc_ = loaded_ = zengine::datasheet::Load(file_);
    dirty_ = false;
    Rebuild();
    Title();
}

void DataSheetEditor::SaveFile()
{
    ReadGrid();
    zengine::datasheet::Save(assetsRoot_, file_, doc_, &loaded_);
    loaded_ = doc_;
    dirty_ = false;
    Rebuild();
    Title();
    SetWindowTextW(status_, L"Saved.");
}

bool DataSheetEditor::ConfirmClose()
{
    if (!dirty_) return true;
    const auto answer = MessageBoxW(window_, L"Save changes to this data sheet?", L"Unsaved data sheet",
                                    MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    try { if (answer == IDYES) SaveFile(); else LoadFile(); return true; }
    catch (const std::exception& e) { SetWindowTextW(status_, Wide(e.what()).c_str()); return false; }
}

void DataSheetEditor::Layout()
{
    if (!window_) return;
    RECT r; GetClientRect(window_, &r);
    const int width = std::max(520L, r.right);
    const auto move = [&](int id, int x, int y, int w, int h) { if (HWND c = GetDlgItem(window_, id)) MoveWindow(c, x, y, w, h, TRUE); };
    move(AddRow, 12, 12, 84, 26);
    move(DeleteRow, 104, 12, 96, 26);
    move(Save, width - 178, 12, 76, 26);
    move(Reload, width - 94, 12, 76, 26);

    const int cols = static_cast<int>(columns_.size());
    const int keyW = 120;
    const int cellW = std::max(70, (width - 24 - keyW - 8) / std::max(1, cols));
    int y = 48;
    for (std::size_t h = 0; h < headers_.size(); ++h)
    {
        const int x = 12 + (h == 0 ? 0 : keyW + 8 + static_cast<int>(h - 1) * (cellW + 4));
        MoveWindow(headers_[h], x, y, h == 0 ? keyW : cellW, 20, TRUE);
    }
    y += 24;
    const int stride = Stride();
    for (std::size_t i = 0; i < cells_.size(); ++i)
    {
        const int rowIndex = static_cast<int>(i) / stride;
        const int colIndex = static_cast<int>(i) % stride; // 0 = key
        const int x = 12 + (colIndex == 0 ? 0 : keyW + 8 + (colIndex - 1) * (cellW + 4));
        MoveWindow(cells_[i], x, y + rowIndex * 26, colIndex == 0 ? keyW : cellW, 22, TRUE);
    }
    const int rows = stride ? static_cast<int>(cells_.size()) / stride : 0;
    move(4590, 12, std::max(y + rows * 26 + 10, static_cast<int>(r.bottom) - 30), width - 24, 24);
}

LRESULT CALLBACK DataSheetEditor::Procedure(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<DataSheetEditor*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<DataSheetEditor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = w;
    }
    if (!self) return DefWindowProcW(w, m, wp, lp);
    try
    {
        if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORBTN)
            return editorStyle::ControlColor(m, wp);
        if (m == WM_GETMINMAXINFO) { reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {480, 300}; return 0; }
        if (m == WM_SIZE) { self->Layout(); return 0; }
        if (m == WM_CLOSE) { if (self->ConfirmClose()) ShowWindow(w, SW_HIDE); return 0; }
        if (m == WM_COMMAND && !self->loading_)
        {
            const int id = LOWORD(wp), event = HIWORD(wp);
            if (event == EN_CHANGE && id >= FirstCell) { self->dirty_ = true; self->Title(); return 0; }
            if (event != BN_CLICKED) return 0;
            if (id == AddRow)
            {
                self->ReadGrid();
                zengine::datasheet::Row row; row.key = "row" + std::to_string(self->doc_.rows.size() + 1);
                self->doc_.rows.push_back(std::move(row));
                self->dirty_ = true; self->Rebuild(); self->Title(); return 0;
            }
            if (id == DeleteRow)
            {
                self->ReadGrid();
                if (!self->doc_.rows.empty()) { self->doc_.rows.pop_back(); self->dirty_ = true; self->Rebuild(); self->Title(); }
                return 0;
            }
            if (id == Save) { self->SaveFile(); return 0; }
            if (id == Reload && (!self->dirty_ || MessageBoxW(w, L"Discard unsaved edits and reload?", L"Reload data sheet",
                                                             MB_YESNO | MB_ICONQUESTION) == IDYES))
            { self->LoadFile(); return 0; }
        }
    }
    catch (const std::exception& e) { if (self->status_) SetWindowTextW(self->status_, Wide(e.what()).c_str()); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}
