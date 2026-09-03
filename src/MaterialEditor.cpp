#include "MaterialEditor.h"
#include "EditorStyle.h"
#include "ShaderAssets.h"

#include <algorithm>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace
{
    std::wstring Wide(const std::string& s) { return {s.begin(), s.end()}; }

    std::string Text(HWND w)
    {
        const int n = GetWindowTextLengthW(w);
        std::wstring s(n + 1, L'\0');
        GetWindowTextW(w, s.data(), n + 1);
        s.resize(n);
        std::string out;
        for (wchar_t c : s) { if (c > 126) throw std::runtime_error("Use printable ASCII in material fields."); out.push_back(static_cast<char>(c)); }
        return out;
    }

    std::string Num(float v)
    {
        std::ostringstream o; o.imbue(std::locale::classic()); o << v; return o.str();
    }
    float ParseNum(const std::string& s)
    {
        std::istringstream in(s); in.imbue(std::locale::classic());
        float v = 0;
        if (!(in >> v)) throw std::runtime_error("Expected a number in a material value field.");
        in >> std::ws;
        if (!in.eof()) throw std::runtime_error("Expected a single number in a material value field.");
        return v;
    }

    int Axes(zengine::shaders::ParamType t)
    {
        using PT = zengine::shaders::ParamType;
        switch (t) { case PT::Float4: return 4; case PT::Float3: return 3; case PT::Float2: return 2; default: return 1; }
    }
    bool IsTexture(zengine::shaders::ParamType t) { return t == zengine::shaders::ParamType::Texture2D; }
}

MaterialEditor::MaterialEditor(HWND owner, std::filesystem::path assetsRoot, std::filesystem::path file)
    : assetsRoot_(std::move(assetsRoot)), file_(std::move(file))
{
    doc_ = loaded_ = zengine::materials::Load(file_);

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = L"zEngineMaterialEditor";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = editorStyle::Shared().panel;
    RegisterClassW(&wc);

    window_ = CreateWindowExW(0, wc.lpszClassName, L"Material", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 560, 620, owner, nullptr, wc.hInstance, this);
    if (!window_) throw std::runtime_error("Cannot open the Material editor.");

    const auto ctl = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
        HWND w = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
        SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return w;
    };
    ctl(L"STATIC", L"Shader (.shader path; blank = built-in Standard)", 4390, 0);
    ctl(L"EDIT", L"", Shader, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP);
    ctl(L"BUTTON", L"Save", Save, WS_TABSTOP | BS_PUSHBUTTON);
    ctl(L"BUTTON", L"Reload", Reload, WS_TABSTOP | BS_PUSHBUTTON);
    for (int r = 0; r < MaxRows; ++r)
    {
        ctl(L"STATIC", L"", Label0 + r, 0);
        for (int a = 0; a < 4; ++a)
            ctl(L"EDIT", L"", Field0 + r * 4 + a, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP);
    }
    status_ = ctl(L"STATIC", L"Values are pinned to the shader's parameters and saved explicitly.", 4300, 0);

    editorStyle::AttachChildren(window_);
    SetDlgItemTextW(window_, Shader, Wide(doc_.shader).c_str());
    Populate();
    Layout();
    Title();
}

MaterialEditor::~MaterialEditor() { if (IsWindow(window_)) DestroyWindow(window_); }

void MaterialEditor::Title()
{
    SetWindowTextW(window_, ((dirty_ ? L"* " : L"") + file_.filename().wstring() + L" - zEngine Material").c_str());
}

void MaterialEditor::Show()
{
    if (!dirty_) LoadFile();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
}

zengine::materials::Effective MaterialEditor::Resolve() const
{
    return zengine::materials::Resolve(doc_, [&](const std::string& path) {
        return zengine::shaders::Load(zengine::shaders::Resolve(assetsRoot_, std::filesystem::u8path(path)));
    });
}

void MaterialEditor::Populate()
{
    loading_ = true;
    zengine::materials::Effective eff;
    try { eff = Resolve(); }
    catch (const std::exception& e) { eff.ok = false; eff.error = e.what(); }

    params_.assign(eff.parameters.begin(),
                   eff.parameters.begin() + std::min<std::size_t>(eff.parameters.size(), MaxRows));

    for (int r = 0; r < MaxRows; ++r)
    {
        const bool used = r < static_cast<int>(params_.size());
        HWND label = GetDlgItem(window_, Label0 + r);
        ShowWindow(label, used ? SW_SHOW : SW_HIDE);
        int axes = 0;
        if (used)
        {
            const auto& p = params_[r];
            axes = IsTexture(p.type) ? 1 : Axes(p.type);
            SetWindowTextW(label, Wide(p.name + (IsTexture(p.type) ? "  (texture path)" : "")).c_str());
            for (int a = 0; a < 4; ++a)
            {
                HWND f = GetDlgItem(window_, Field0 + r * 4 + a);
                if (a < axes)
                {
                    SetWindowTextW(f, IsTexture(p.type) ? Wide(p.texture).c_str() : Wide(Num(p.numbers[a])).c_str());
                    EnableWindow(f, TRUE);
                }
                ShowWindow(f, a < axes ? SW_SHOW : SW_HIDE);
            }
        }
        else
            for (int a = 0; a < 4; ++a) ShowWindow(GetDlgItem(window_, Field0 + r * 4 + a), SW_HIDE);
    }
    SetWindowTextW(status_, eff.ok ? L"Values are pinned to the shader's parameters and saved explicitly."
                                   : (L"Shader problem: " + Wide(eff.error)).c_str());
    loading_ = false;
    Layout();
}

void MaterialEditor::ReadFields()
{
    for (std::size_t r = 0; r < params_.size(); ++r)
    {
        auto& p = params_[r];
        if (IsTexture(p.type)) { p.texture = Text(GetDlgItem(window_, Field0 + static_cast<int>(r) * 4)); continue; }
        const int axes = Axes(p.type);
        for (int a = 0; a < 4; ++a)
            p.numbers[a] = a < axes ? ParseNum(Text(GetDlgItem(window_, Field0 + static_cast<int>(r) * 4 + a))) : 0.0f;
    }
}

void MaterialEditor::LoadFile()
{
    doc_ = loaded_ = zengine::materials::Load(file_);
    loading_ = true;
    SetDlgItemTextW(window_, Shader, Wide(doc_.shader).c_str());
    loading_ = false;
    dirty_ = false;
    Populate();
    Title();
}

void MaterialEditor::SaveFile()
{
    doc_.shader = Text(GetDlgItem(window_, Shader));
    ReadFields();
    doc_.values.assign(params_.begin(), params_.end());
    zengine::materials::Save(assetsRoot_, file_, doc_, &loaded_);
    loaded_ = doc_;
    dirty_ = false;
    Title();
    SetWindowTextW(status_, L"Saved. Assigned meshes pick up the change on the next frame.");
}

bool MaterialEditor::ConfirmClose()
{
    if (!dirty_) return true;
    const auto answer = MessageBoxW(window_, L"Save changes to this material?", L"Unsaved material",
                                    MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return false;
    try { if (answer == IDYES) SaveFile(); else LoadFile(); return true; }
    catch (const std::exception& e) { SetWindowTextW(status_, Wide(e.what()).c_str()); return false; }
}

void MaterialEditor::Layout()
{
    if (!window_) return;
    RECT r; GetClientRect(window_, &r);
    const int width = std::max(420L, r.right);
    const auto move = [&](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(window_, id), x, y, w, h, TRUE); };
    move(Save, width - 178, 12, 76, 26);
    move(Reload, width - 94, 12, 76, 26);
    move(4390, 12, 16, width - 200, 20);
    move(Shader, 12, 40, width - 24, 24);

    int y = 78;
    for (int rIndex = 0; rIndex < MaxRows; ++rIndex)
    {
        const bool used = rIndex < static_cast<int>(params_.size());
        if (!used) continue;
        move(Label0 + rIndex, 12, y, 150, 22);
        const int axes = IsTexture(params_[rIndex].type) ? 1 : Axes(params_[rIndex].type);
        const int cell = std::max(40, (width - 176) / std::max(1, axes));
        for (int a = 0; a < axes; ++a) move(Field0 + rIndex * 4 + a, 168 + a * (cell + 4), y, cell, 22);
        y += 30;
    }
    move(4300, 12, std::max(y + 8, static_cast<int>(r.bottom) - 34), width - 24, 26);
}

LRESULT CALLBACK MaterialEditor::Procedure(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<MaterialEditor*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<MaterialEditor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = w;
    }
    if (!self) return DefWindowProcW(w, m, wp, lp);
    try
    {
        if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORBTN)
            return editorStyle::ControlColor(m, wp);
        if (m == WM_GETMINMAXINFO) { reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {440, 320}; return 0; }
        if (m == WM_SIZE) { self->Layout(); return 0; }
        if (m == WM_CLOSE) { if (self->ConfirmClose()) ShowWindow(w, SW_HIDE); return 0; }
        if (m == WM_COMMAND && !self->loading_)
        {
            const int id = LOWORD(wp), event = HIWORD(wp);
            if (event == EN_CHANGE && (id == Shader || (id >= Field0 && id < Field0 + MaxRows * 4)))
            { self->dirty_ = true; self->Title(); return 0; }
            if (event == EN_KILLFOCUS && id == Shader)
            { self->doc_.shader = Text(GetDlgItem(w, Shader)); self->Populate(); return 0; }
            if (id == Save && event == BN_CLICKED) { self->SaveFile(); return 0; }
            if (id == Reload && event == BN_CLICKED &&
                (!self->dirty_ || MessageBoxW(w, L"Discard unsaved edits and reload?", L"Reload material",
                                              MB_YESNO | MB_ICONQUESTION) == IDYES))
            { self->LoadFile(); return 0; }
        }
    }
    catch (const std::exception& e) { if (self->status_) SetWindowTextW(self->status_, Wide(e.what()).c_str()); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}
