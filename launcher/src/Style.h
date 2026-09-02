#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <string_view>
#include <algorithm>

// Self-contained dark visuals for the launcher. The palette and the flat button
// treatment deliberately mirror the engine editor's shared style so the two
// applications look like one product, but nothing here depends on the engine.
namespace lstyle
{
inline constexpr COLORREF Window = RGB(32, 33, 37);
inline constexpr COLORREF Panel  = RGB(40, 42, 47);
inline constexpr COLORREF Face   = RGB(52, 54, 60);
inline constexpr COLORREF Border = RGB(20, 21, 24);
inline constexpr COLORREF Text   = RGB(214, 216, 221);
inline constexpr COLORREF Muted  = RGB(145, 149, 158);
inline constexpr COLORREF Accent = RGB(108, 166, 232);
inline constexpr COLORREF Hover  = RGB(65, 69, 77);
inline constexpr COLORREF Pressed = RGB(42, 64, 91);
inline constexpr COLORREF Disabled = RGB(44, 46, 51);
inline constexpr COLORREF RowLine = RGB(58, 60, 66);

struct Resources
{
    HBRUSH window = CreateSolidBrush(Window);
    HBRUSH panel  = CreateSolidBrush(Panel);
    HBRUSH field  = CreateSolidBrush(Face);
    HFONT  font   = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");
    ~Resources() { DeleteObject(window); DeleteObject(panel); DeleteObject(field); DeleteObject(font); }
};
inline Resources& Shared() { static Resources r; return r; }

inline void Fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
inline void Outline(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FrameRect(dc, &r, b); DeleteObject(b); }

inline void Button(HDC dc, RECT r, std::wstring_view label, bool enabled = true, bool hot = false,
                   bool down = false, bool selected = false, bool focused = false)
{
    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? (selected ? Accent : Text) : Muted);
    Fill(dc, r, !enabled ? Disabled : down ? Pressed : hot ? Hover : Face);
    Outline(dc, r, focused ? Accent : Border);
    RECT text = r; InflateRect(&text, -4, -1);
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &text,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    if (focused) { RECT f = r; InflateRect(&f, -3, -3); DrawFocusRect(dc, &f); }
    RestoreDC(dc, saved);
}

struct ButtonState { bool hot = false; bool combo = false; };

inline void PaintButton(HWND w, HDC dc, const ButtonState& state)
{
    RECT r; GetClientRect(w, &r);
    const bool enabled = IsWindowEnabled(w) != FALSE;
    auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(w, WM_GETFONT, 0, 0)));

    if (state.combo)
    {
        const bool down = SendMessageW(w, CB_GETDROPPEDSTATE, 0, 0) != 0;
        Fill(dc, r, !enabled ? Disabled : down ? Pressed : state.hot ? Hover : Face);
        Outline(dc, r, GetFocus() == w ? Accent : Border);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? Text : Muted);
        const auto index = SendMessageW(w, CB_GETCURSEL, 0, 0);
        if (index != CB_ERR)
        {
            const auto length = SendMessageW(w, CB_GETLBTEXTLEN, index, 0);
            if (length >= 0 && length < 32768)
            {
                std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
                SendMessageW(w, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
                RECT label{ r.left + 8, r.top + 1, r.right - 22, r.bottom - 1 };
                DrawTextW(dc, text.c_str(), static_cast<int>(length), &label,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        const int x = r.right - 12, y = (r.top + r.bottom) / 2;
        POINT points[]{ {x - 4, y - 2}, {x + 4, y - 2}, {x, y + 3} };
        auto brush = CreateSolidBrush(enabled ? Text : Muted);
        auto oldBrush = SelectObject(dc, brush);
        auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, 3);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
        SelectObject(dc, oldFont);
        return;
    }

    const auto bits = SendMessageW(w, BM_GETSTATE, 0, 0);
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(w)) + 1, L'\0');
    text.resize(GetWindowTextW(w, text.data(), static_cast<int>(text.size())));
    Button(dc, r, text, enabled, state.hot, (bits & BST_PUSHED) != 0, false, GetFocus() == w);
    SelectObject(dc, oldFont);
}

inline LRESULT CALLBACK ButtonProcedure(HWND w, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    auto* state = reinterpret_cast<ButtonState*>(data);
    if (m == WM_NCDESTROY) { RemoveWindowSubclass(w, ButtonProcedure, id); delete state; return DefSubclassProc(w, m, wp, lp); }
    if (m == WM_PAINT) { PAINTSTRUCT p; auto dc = BeginPaint(w, &p); PaintButton(w, dc, *state); EndPaint(w, &p); return 0; }
    if (m == WM_PRINTCLIENT) { PaintButton(w, reinterpret_cast<HDC>(wp), *state); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_MOUSEMOVE && !state->hot)
    {
        state->hot = true;
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, w, 0 };
        TrackMouseEvent(&track);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (m == WM_MOUSELEAVE) { state->hot = false; InvalidateRect(w, nullptr, FALSE); }
    const auto result = DefSubclassProc(w, m, wp, lp);
    if (IsWindow(w) && (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_KEYDOWN || m == WM_KEYUP ||
                        m == WM_ENABLE || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == CB_SETCURSEL ||
                        m == CB_SHOWDROPDOWN || m == WM_SETTEXT || m == WM_UPDATEUISTATE || m == WM_CAPTURECHANGED))
        InvalidateRect(w, nullptr, FALSE);
    return result;
}

inline void Attach(HWND w)
{
    wchar_t name[32]{};
    GetClassNameW(w, name, 32);
    const bool combo = _wcsicmp(name, L"COMBOBOX") == 0 && (GetWindowLongPtrW(w, GWL_STYLE) & 3) == CBS_DROPDOWNLIST;
    if (!combo && _wcsicmp(name, L"BUTTON") != 0) return;
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(w, ButtonProcedure, 0x5a4c4e, &existing)) return;
    auto* state = new ButtonState;
    state->combo = combo;
    if (!SetWindowSubclass(w, ButtonProcedure, 0x5a4c4e, reinterpret_cast<DWORD_PTR>(state))) { delete state; return; }
    SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(Shared().font), FALSE);
    InvalidateRect(w, nullptr, FALSE);
}

inline void AttachChildren(HWND parent)
{
    EnumChildWindows(parent, [](HWND w, LPARAM) -> BOOL { Attach(w); return TRUE; }, 0);
}

inline LRESULT ControlColor(UINT message, WPARAM wp)
{
    auto dc = reinterpret_cast<HDC>(wp);
    const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
    SetTextColor(dc, Text);
    SetBkColor(dc, field ? Face : Panel);
    return reinterpret_cast<LRESULT>(field ? Shared().field : Shared().panel);
}
}
