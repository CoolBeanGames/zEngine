#include "ObjectPicker.h"
#include "EditorStyle.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <stdexcept>

namespace
{
    std::function<ObjectPicker::Choice(const ObjectPicker::Request&)> g_testResponder;

    struct Context
    {
        const ObjectPicker::Request* request = nullptr;
        ObjectPicker::Choice choice;
        bool dropdown = false;
        RECT anchor{};                      // screen rect the dropdown hangs below
        int rowHeight = 22;
        std::vector<std::wstring> groups;   // distinct, in first-seen order
        std::set<std::wstring> active;      // groups currently shown
        std::vector<int> filtered;          // request->items indices currently listed
    };

    std::wstring LowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    std::wstring ReadText(HWND control)
    {
        std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
        value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
        return value;
    }

    void Rebuild(HWND dialog, Context& context)
    {
        const auto list = GetDlgItem(dialog, ObjectPicker::ResultList);
        const auto search = LowerCopy(ReadText(GetDlgItem(dialog, ObjectPicker::SearchField)));
        context.filtered.clear();
        for (int i = 0; i < static_cast<int>(context.request->items.size()); ++i)
        {
            const auto& item = context.request->items[i];
            if (!item.group.empty() && !context.active.count(item.group)) continue;
            if (!search.empty())
            {
                const auto haystack = LowerCopy(item.label + L" " + item.detail + L" " + item.group);
                if (haystack.find(search) == std::wstring::npos) continue;
            }
            context.filtered.push_back(i);
        }
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int select = -1;
        for (int row = 0; row < static_cast<int>(context.filtered.size()); ++row)
        {
            const auto& item = context.request->items[context.filtered[row]];
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
            SendMessageW(list, LB_SETITEMDATA, row, context.filtered[row]);
            if (select < 0 && !context.request->current.empty() && item.value == context.request->current) select = row;
        }
        SendMessageW(list, LB_SETCURSEL, select < 0 ? 0 : select, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    void Finish(HWND dialog, Context& context, ObjectPicker::Choice choice)
    {
        context.choice = std::move(choice);
        EndDialog(dialog, context.choice.picked || context.choice.cleared ? IDOK : IDCANCEL);
    }

    void PickCurrent(HWND dialog, Context& context)
    {
        const auto list = GetDlgItem(dialog, ObjectPicker::ResultList);
        const auto row = SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (row == LB_ERR || row < 0 || row >= static_cast<int>(context.filtered.size())) return;
        Finish(dialog, context, ObjectPicker::Choice::Pick(context.request->items[context.filtered[row]].value));
    }

    // A frameless dropdown picks on a plain left click and cancels when it loses focus.
    LRESULT CALLBACK ListProc(HWND list, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
    {
        if (message == WM_NCDESTROY) RemoveWindowSubclass(list, ListProc, id);
        if (message == WM_LBUTTONUP)
        {
            const auto result = DefSubclassProc(list, message, w, l);
            const auto index = SendMessageW(list, LB_ITEMFROMPOINT, 0, l);
            if (HIWORD(index) == 0) { SendMessageW(list, LB_SETCURSEL, LOWORD(index), 0); PostMessageW(GetParent(list), WM_COMMAND, MAKEWPARAM(ObjectPicker::OkButton, BN_CLICKED), 0); }
            return result;
        }
        return DefSubclassProc(list, message, w, l);
    }

    HWND Make(HWND dialog, const wchar_t* type, const wchar_t* text, int id, int x, int y, int cx, int cy, DWORD style)
    {
        const auto window = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy,
                                            dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!window) throw std::runtime_error("Cannot create object picker control.");
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(editorStyle::Shared().font), TRUE);
        return window;
    }

    void BuildControls(HWND dialog, Context& context)
    {
        RECT client{}; GetClientRect(dialog, &client);
        const int width = client.right, height = client.bottom, pad = 8;
        // Distinct groups, first-seen order.
        for (const auto& item : context.request->items)
            if (!item.group.empty() && std::find(context.groups.begin(), context.groups.end(), item.group) == context.groups.end())
                context.groups.push_back(item.group);
        for (const auto& group : context.groups) context.active.insert(group);

        Make(dialog, L"EDIT", L"", ObjectPicker::SearchField, pad, pad, width - 2 * pad, 24, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
        SendMessageW(GetDlgItem(dialog, ObjectPicker::SearchField), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search"));

        int top = pad + 32;
        const bool chips = context.groups.size() > 1;
        if (chips)
        {
            int x = pad;
            for (std::size_t i = 0; i < context.groups.size(); ++i)
            {
                const int chipWidth = std::max<int>(52, 20 + 8 * static_cast<int>(context.groups[i].size()));
                if (x + chipWidth > width - pad && x > pad) { x = pad; top += 28; }
                auto chip = Make(dialog, L"BUTTON", context.groups[i].c_str(), ObjectPicker::FirstFilterChip + static_cast<int>(i),
                                 x, top, chipWidth, 24, WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE);
                SendMessageW(chip, BM_SETCHECK, BST_CHECKED, 0);
                x += chipWidth + 6;
            }
            top += 32;
        }

        const int buttonRow = context.dropdown && !context.request->allowClear ? 0 : 36;
        Make(dialog, L"LISTBOX", L"", ObjectPicker::ResultList, pad, top, width - 2 * pad, height - top - pad - buttonRow,
             WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
        if (context.dropdown) SetWindowSubclass(GetDlgItem(dialog, ObjectPicker::ResultList), ListProc, 1, 0);

        if (buttonRow)
        {
            const int by = height - pad - 27;
            if (context.request->allowClear)
                Make(dialog, L"BUTTON", L"None", ObjectPicker::ClearButton, pad, by, 84, 27, WS_TABSTOP);
            if (!context.dropdown)
            {
                Make(dialog, L"BUTTON", L"Cancel", ObjectPicker::CancelButton, width - pad - 90, by, 90, 27, WS_TABSTOP);
                Make(dialog, L"BUTTON", L"Select", ObjectPicker::OkButton, width - pad - 188, by, 92, 27, WS_TABSTOP | BS_DEFPUSHBUTTON);
            }
        }
        editorStyle::AttachChildren(dialog);
        Rebuild(dialog, context);
        SetFocus(GetDlgItem(dialog, ObjectPicker::SearchField));
    }

    void DrawRow(Context& context, DRAWITEMSTRUCT* draw)
    {
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        editorStyle::Fill(draw->hDC, draw->rcItem, selected ? editorStyle::Pressed : editorStyle::Panel);
        if (draw->itemID == static_cast<UINT>(-1)) return;
        const int index = static_cast<int>(SendMessageW(draw->hwndItem, LB_GETITEMDATA, draw->itemID, 0));
        if (index < 0 || index >= static_cast<int>(context.request->items.size())) return;
        const auto& item = context.request->items[index];
        assetLibrary::Icon(draw->hDC, item.icon, draw->rcItem.left + 6, draw->rcItem.top + (context.rowHeight - 20) / 2);
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, editorStyle::Text);
        RECT text{draw->rcItem.left + 32, draw->rcItem.top, draw->rcItem.right - 8, draw->rcItem.bottom};
        DrawTextW(draw->hDC, item.label.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (!item.detail.empty())
        {
            SetTextColor(draw->hDC, editorStyle::Muted);
            RECT detail = text; detail.left += 4;
            DrawTextW(draw->hDC, item.detail.c_str(), -1, &detail, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        }
        if ((draw->itemState & ODS_FOCUS) && !selected) DrawFocusRect(draw->hDC, &draw->rcItem);
    }

    INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM w, LPARAM l)
    {
        auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
        switch (message)
        {
        case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
            return editorStyle::ControlColor(message, w);
        case WM_INITDIALOG:
        {
            context = reinterpret_cast<Context*>(l);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
            SetWindowTextW(dialog, context->request->title.c_str());
            const bool drop = context->dropdown;
            const int cx = drop ? std::max<LONG>(300, context->anchor.right - context->anchor.left) : 470;
            const int cy = drop ? 280 : 490;
            RECT outer{0, 0, cx, cy};
            AdjustWindowRectEx(&outer, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
            int px, py;
            if (drop) { px = context->anchor.left; py = context->anchor.bottom; }
            else
            {
                RECT ow{}; GetWindowRect(GetWindow(dialog, GW_OWNER), &ow);
                if (IsRectEmpty(&ow)) { ow = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; }
                px = ow.left + (ow.right - ow.left - (outer.right - outer.left)) / 2;
                py = ow.top + (ow.bottom - ow.top - (outer.bottom - outer.top)) / 2;
            }
            SetWindowPos(dialog, nullptr, px, py, outer.right - outer.left, outer.bottom - outer.top, SWP_NOZORDER);
            try { BuildControls(dialog, *context); }
            catch (const std::exception&) { EndDialog(dialog, IDCANCEL); }
            return FALSE;
        }
        case WM_MEASUREITEM:
            if (context) reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight = context->rowHeight;
            return TRUE;
        case WM_DRAWITEM:
            if (context && reinterpret_cast<DRAWITEMSTRUCT*>(l)->CtlID == ObjectPicker::ResultList)
            { DrawRow(*context, reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; }
            return FALSE;
        case WM_ACTIVATE:
            if (context && context->dropdown && LOWORD(w) == WA_INACTIVE)
            {
                const auto next = reinterpret_cast<HWND>(l);
                if (next != dialog && !IsChild(dialog, next)) Finish(dialog, *context, ObjectPicker::Choice::Cancel());
            }
            return FALSE;
        case WM_CLOSE:
            if (context) Finish(dialog, *context, ObjectPicker::Choice::Cancel());
            return TRUE;
        case WM_COMMAND:
            if (!context) return FALSE;
            if (LOWORD(w) == IDCANCEL || LOWORD(w) == ObjectPicker::CancelButton) { Finish(dialog, *context, ObjectPicker::Choice::Cancel()); return TRUE; }
            if (LOWORD(w) == ObjectPicker::ClearButton) { Finish(dialog, *context, ObjectPicker::Choice::Cleared()); return TRUE; }
            if (LOWORD(w) == IDOK || LOWORD(w) == ObjectPicker::OkButton) { PickCurrent(dialog, *context); return TRUE; }
            if (LOWORD(w) == ObjectPicker::SearchField && HIWORD(w) == EN_CHANGE) { Rebuild(dialog, *context); return TRUE; }
            if (LOWORD(w) == ObjectPicker::ResultList && HIWORD(w) == LBN_DBLCLK) { PickCurrent(dialog, *context); return TRUE; }
            if (LOWORD(w) >= ObjectPicker::FirstFilterChip && LOWORD(w) < ObjectPicker::FirstFilterChip + 256 && HIWORD(w) == BN_CLICKED)
            {
                const std::size_t i = LOWORD(w) - ObjectPicker::FirstFilterChip;
                if (i < context->groups.size())
                {
                    if (SendMessageW(reinterpret_cast<HWND>(l), BM_GETCHECK, 0, 0) == BST_CHECKED) context->active.insert(context->groups[i]);
                    else context->active.erase(context->groups[i]);
                    Rebuild(dialog, *context);
                }
                return TRUE;
            }
            return FALSE;
        }
        return FALSE;
    }

    ObjectPicker::Choice Show(HWND owner, const ObjectPicker::Request& request, bool dropdown, RECT anchor)
    {
        if (g_testResponder) return g_testResponder(request);

        Context context;
        context.request = &request;
        context.dropdown = dropdown;
        context.anchor = anchor;

        struct Layout { DLGTEMPLATE dialog; WORD menu = 0, windowClass = 0, title = 0; } layout{};
        layout.dialog.style = dropdown ? (WS_POPUP | WS_BORDER) : (WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
        layout.dialog.cx = 100; layout.dialog.cy = 100;

        const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner,
                                                    Procedure, reinterpret_cast<LPARAM>(&context));
        if (result == -1) return ObjectPicker::Choice::Cancel();
        return context.choice;
    }
}

void ObjectPicker::SetTestResponder(std::function<Choice(const Request&)> responder)
{
    g_testResponder = std::move(responder);
}

ObjectPicker::Choice ObjectPicker::Window(HWND owner, const Request& request)
{
    return Show(owner, request, false, RECT{});
}

ObjectPicker::Choice ObjectPicker::Dropdown(HWND owner, RECT anchorScreen, const Request& request)
{
    return Show(owner, request, true, anchorScreen);
}
