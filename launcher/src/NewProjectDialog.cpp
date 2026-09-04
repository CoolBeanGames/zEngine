#include "NewProjectDialog.h"
#include "Resource.h"
#include "Style.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <stdexcept>

namespace zlauncher
{
namespace
{
enum Control : int
{
    NameField = 1001,
    LocationField,
    BrowseButton,
    TemplateCombo,
    ErrorLabel,
};

struct Context
{
    const std::vector<std::wstring>* templates = nullptr;
    NewProjectRequest request;
};

std::wstring ReadText(HWND dialog, int id)
{
    const HWND control = GetDlgItem(dialog, id);
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    value.resize(GetWindowTextW(control, value.data(), static_cast<int>(value.size())));
    return value;
}

std::wstring Wide(const char* text)
{
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

HWND MakeControl(HWND dialog, const wchar_t* type, const wchar_t* text, int id, int x, int y,
                 int w, int h, DWORD style = 0)
{
    const HWND window = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                        dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                        GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Cannot create a dialog control.");
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(lstyle::Shared().font), TRUE);
    return window;
}

void BrowseForFolder(HWND dialog)
{
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&picker))))
        return;
    DWORD options = 0;
    picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Choose the folder to create the project in");
    if (SUCCEEDED(picker->Show(dialog)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                SetDlgItemTextW(dialog, LocationField, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    picker->Release();
}

INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM w, LPARAM l)
{
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try
    {
        switch (message)
        {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return lstyle::ControlColor(message, w);

        case WM_INITDIALOG:
        {
            context = reinterpret_cast<Context*>(l);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
            SetWindowTextW(dialog, L"New Project");
            if (HICON icon = static_cast<HICON>(
                    LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0,
                               0, LR_DEFAULTSIZE | LR_SHARED)))
            {
                SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
                SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            }

            RECT outer{ 0, 0, 560, 290 };
            AdjustWindowRectEx(&outer, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
            RECT owner{};
            if (GetWindowRect(GetWindow(dialog, GW_OWNER), &owner))
                SetWindowPos(dialog, nullptr,
                             owner.left + (owner.right - owner.left - (outer.right - outer.left)) / 2,
                             owner.top + (owner.bottom - owner.top - (outer.bottom - outer.top)) / 2,
                             outer.right - outer.left, outer.bottom - outer.top, SWP_NOZORDER);

            MakeControl(dialog, L"STATIC", L"Project name", -1, 16, 14, 520, 18);
            const HWND name = MakeControl(dialog, L"EDIT", context->request.name.c_str(), NameField,
                                          16, 34, 520, 24, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
            SendMessageW(name, EM_SETLIMITTEXT, 80, 0);

            MakeControl(dialog, L"STATIC", L"Location", -1, 16, 70, 520, 18);
            MakeControl(dialog, L"EDIT", context->request.location.c_str(), LocationField, 16, 90,
                        428, 24, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
            MakeControl(dialog, L"BUTTON", L"Browse…", BrowseButton, 454, 90, 82, 24, WS_TABSTOP);

            MakeControl(dialog, L"STATIC", L"Template", -1, 16, 126, 520, 18);
            const HWND combo = MakeControl(dialog, L"COMBOBOX", L"", TemplateCombo, 16, 146, 520, 240,
                                           WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Blank project"));
            if (context->templates)
                for (const auto& t : *context->templates)
                    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
            SendMessageW(combo, CB_SETCURSEL, 0, 0);

            MakeControl(dialog, L"STATIC",
                        L"A folder named after the project is created here, then the editor opens it.",
                        -1, 16, 182, 520, 34);
            MakeControl(dialog, L"STATIC", L"", ErrorLabel, 16, 214, 520, 34);

            MakeControl(dialog, L"BUTTON", L"Create", IDOK, 348, 250, 90, 28,
                        WS_TABSTOP | BS_DEFPUSHBUTTON);
            MakeControl(dialog, L"BUTTON", L"Cancel", IDCANCEL, 446, 250, 90, 28, WS_TABSTOP);

            lstyle::AttachChildren(dialog);
            SetFocus(name);
            SendMessageW(name, EM_SETSEL, 0, -1);
            return FALSE;
        }

        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(w) == IDCANCEL)
            {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(w) == BrowseButton)
            {
                BrowseForFolder(dialog);
                return TRUE;
            }
            if (LOWORD(w) == IDOK && context)
            {
                context->request.name = ReadText(dialog, NameField);
                std::wstring location = ReadText(dialog, LocationField);
                // Trim surrounding whitespace the picker or a paste can leave behind.
                while (!context->request.name.empty() && context->request.name.back() == L' ')
                    context->request.name.pop_back();
                while (!context->request.name.empty() && context->request.name.front() == L' ')
                    context->request.name.erase(context->request.name.begin());

                if (context->request.name.empty())
                    throw std::runtime_error("Enter a project name.");
                std::filesystem::path parent(location);
                if (!parent.is_absolute() || !std::filesystem::is_directory(parent))
                    throw std::runtime_error("Choose an existing folder in Location.");
                if (std::filesystem::exists(parent / context->request.name))
                    throw std::runtime_error("A folder with that name already exists there.");

                context->request.location = parent;
                const auto sel = SendMessageW(GetDlgItem(dialog, TemplateCombo), CB_GETCURSEL, 0, 0);
                context->request.templateIndex = (sel == CB_ERR || sel == 0)
                                                     ? -1
                                                     : static_cast<int>(sel) - 1;
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            return FALSE;
        }
    }
    catch (const std::exception& e)
    {
        SetDlgItemTextW(dialog, ErrorLabel, Wide(e.what()).c_str());
        return TRUE;
    }
    return FALSE;
}
} // namespace

std::optional<NewProjectRequest> ShowNewProjectDialog(HWND owner,
                                                      const std::vector<std::wstring>& templateNames)
{
    Context context;
    context.templates = &templateNames;
    context.request.name = L"MyGame";
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path)))
        {
            context.request.location = path;
            CoTaskMemFree(path);
        }
    }

    struct Layout
    {
        DLGTEMPLATE dialog{};
        WORD menu = 0;
        WORD windowClass = 0;
        WORD title = 0;
    } layout{};
    layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    layout.dialog.cx = 300;
    layout.dialog.cy = 160;

    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner,
                                                   Procedure, reinterpret_cast<LPARAM>(&context));
    if (result == -1) throw std::runtime_error("Cannot open the New Project dialog.");
    if (result != IDOK) return std::nullopt;
    return context.request;
}
}
