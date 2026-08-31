#include "ProjectDialog.h"
#include "EditorStyle.h"
#include "Project.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>

namespace
{
    struct Context { ProjectDialog::Request request; };
    std::wstring ReadText(HWND dialog,int id)
    {
        const auto control=GetDlgItem(dialog,id);
        std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control))+1,L'\0');
        value.resize(GetWindowTextW(control,value.data(),static_cast<int>(value.size()))); return value;
    }
    std::wstring Wide(const char* text)
    {
        const int count=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
        std::wstring out(count,L'\0'); MultiByteToWideChar(CP_UTF8,0,text,-1,out.data(),count); return out;
    }
    INT_PTR CALLBACK Procedure(HWND dialog,UINT message,WPARAM w,LPARAM l)
    {
        auto* context=reinterpret_cast<Context*>(GetWindowLongPtrW(dialog,DWLP_USER));
        try
        {
            if(message==WM_CTLCOLORDLG || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORBTN)
                return editorStyle::ControlColor(message,w);
            if (message==WM_INITDIALOG)
            {
                context=reinterpret_cast<Context*>(l); SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(context));
                SetWindowTextW(dialog,L"New Project");
                RECT outer{0,0,570,240}; AdjustWindowRectEx(&outer,static_cast<DWORD>(GetWindowLongPtrW(dialog,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(dialog,GWL_EXSTYLE)));
                RECT owner{}; GetWindowRect(GetWindow(dialog,GW_OWNER),&owner);
                SetWindowPos(dialog,nullptr,owner.left+(owner.right-owner.left-(outer.right-outer.left))/2,owner.top+(owner.bottom-owner.top-(outer.bottom-outer.top))/2,
                    outer.right-outer.left,outer.bottom-outer.top,SWP_NOZORDER);
                const auto control=[&](const wchar_t* type,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style=0) {
                    const auto window=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
                    if (!window) throw std::runtime_error("Cannot create project dialog control.");
                    SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE); return window;
                };
                control(L"STATIC",L"Project name",-1,14,12,530,18);
                const auto name=control(L"EDIT",context->request.name.c_str(),ProjectDialog::NameField,14,32,540,24,WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL);
                SendMessageW(name,EM_SETLIMITTEXT,80,0);
                control(L"STATIC",L"Location (parent folder)",-1,14,68,530,18);
                const auto location=control(L"EDIT",context->request.location.c_str(),ProjectDialog::LocationField,14,88,448,24,WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL);
                SendMessageW(location,EM_SETLIMITTEXT,32760,0);
                control(L"BUTTON",L"Browse...",ProjectDialog::BrowseButton,472,88,82,24,WS_TABSTOP);
                control(L"STATIC",L"Creates a new folder here using the project name.\nThe project config and its Assets folder stay together.",-1,14,122,540,32);
                control(L"STATIC",L"",ProjectDialog::ErrorLabel,14,157,540,34);
                control(L"BUTTON",L"Create",IDOK,362,199,90,27,WS_TABSTOP|BS_DEFPUSHBUTTON);
                control(L"BUTTON",L"Cancel",IDCANCEL,464,199,90,27,WS_TABSTOP);
                editorStyle::AttachChildren(dialog);
                SetFocus(name); SendMessageW(name,EM_SETSEL,0,-1); return FALSE;
            }
            if (message==WM_CLOSE) { EndDialog(dialog,IDCANCEL); return TRUE; }
            if (message==WM_COMMAND && LOWORD(w)==IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
            if (message==WM_COMMAND && LOWORD(w)==ProjectDialog::BrowseButton)
            {
                IFileOpenDialog* picker=nullptr;
                if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker)))) throw std::runtime_error("Cannot open folder picker.");
                DWORD options=0; picker->GetOptions(&options); picker->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
                picker->SetTitle(L"Choose the parent folder for your new project");
                if (SUCCEEDED(picker->Show(dialog)))
                {
                    IShellItem* item=nullptr;
                    if (SUCCEEDED(picker->GetResult(&item)))
                    {
                        PWSTR path=nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) { SetDlgItemTextW(dialog,ProjectDialog::LocationField,path); CoTaskMemFree(path); }
                        item->Release();
                    }
                }
                picker->Release(); return TRUE;
            }
            if (message==WM_COMMAND && LOWORD(w)==IDOK && context)
            {
                context->request.name=ReadText(dialog,ProjectDialog::NameField);
                context->request.location=ReadText(dialog,ProjectDialog::LocationField);
                zengine::projects::ValidateName(context->request.name);
                if (!context->request.location.is_absolute() || !std::filesystem::is_directory(context->request.location)) throw std::runtime_error("Choose an existing absolute parent-folder location.");
                if (std::filesystem::exists(context->request.location/context->request.name)) throw std::runtime_error("That project folder already exists. Choose another name or location.");
                EndDialog(dialog,IDOK); return TRUE;
            }
        }
        catch (const std::exception& e) { SetDlgItemTextW(dialog,ProjectDialog::ErrorLabel,Wide(e.what()).c_str()); return TRUE; }
        return FALSE;
    }
}
std::optional<ProjectDialog::Request> ProjectDialog::Show(HWND owner,const std::filesystem::path& initialLocation)
{
    Context context; context.request.name=L"MyGame"; context.request.location=initialLocation;
    if (context.request.location.empty())
    {
        PWSTR path=nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_DEFAULT,nullptr,&path))) { context.request.location=path; CoTaskMemFree(path); }
    }
    struct Template { DLGTEMPLATE dialog; WORD menu=0, windowClass=0, title=0; } layout{};
    layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    layout.dialog.cx=300; layout.dialog.cy=150;
    const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,owner,Procedure,reinterpret_cast<LPARAM>(&context));
    if (result==-1) throw std::runtime_error("Cannot open New Project dialog.");
    if (result!=IDOK) return {}; return context.request;
}
