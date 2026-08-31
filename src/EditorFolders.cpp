#include "EditorShell.h"
#include "AssetLibrary.h"
#include "EditorStyle.h"
#include "Project.h"

namespace {
struct FolderPrompt { std::wstring name=L"New Folder"; };
INT_PTR CALLBACK Prompt(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* state=reinterpret_cast<FolderPrompt*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT||message==WM_CTLCOLORBTN)return editorStyle::ControlColor(message,w);
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<FolderPrompt*>(l);SetWindowLongPtrW(window,DWLP_USER,l);SetWindowTextW(window,L"New Asset Folder");
        SetWindowPos(window,nullptr,0,0,390,165,SWP_NOMOVE|SWP_NOZORDER);
        auto add=[&](const wchar_t* type,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style){auto h=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return h;};
        add(L"STATIC",L"Folder name",-1,12,8,350,20,0);
        auto edit=add(L"EDIT",state->name.c_str(),100,12,30,350,24,WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL);SendMessageW(edit,EM_SETLIMITTEXT,80,0);
        add(L"BUTTON",L"Create",IDOK,174,75,90,26,WS_TABSTOP|BS_DEFPUSHBUTTON);add(L"BUTTON",L"Cancel",IDCANCEL,272,75,90,26,WS_TABSTOP);
        editorStyle::AttachChildren(window);SetFocus(edit);SendMessageW(edit,EM_SETSEL,0,-1);return FALSE;
    }
    if(message==WM_CLOSE || (message==WM_COMMAND&&LOWORD(w)==IDCANCEL)){EndDialog(window,IDCANCEL);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(w)==IDOK) {
        wchar_t name[81]{};GetDlgItemTextW(window,100,name,81);
        try{zengine::projects::ValidateName(name);state->name=name;EndDialog(window,IDOK);}catch(const std::exception&){MessageBoxW(window,L"Use a nonempty folder name without path separators or reserved filename characters.",L"Invalid folder name",MB_OK|MB_ICONWARNING);}return TRUE;
    }
    return FALSE;
}
}
std::filesystem::path EditorShell::AssetFolder() const {return assetFolder_.empty()?assetsDirectory_:assetLibrary::Resolve(assetsDirectory_,assetFolder_);}
void EditorShell::OpenAssetFolder(const std::filesystem::path& path) {
    RequireProject();const auto folder=assetLibrary::Resolve(assetsDirectory_,path);
    if(!std::filesystem::is_directory(folder)||assetLibrary::Package(folder))throw std::runtime_error("Choose an asset folder.");
    assetFolder_=folder;firstAsset_=0;RefreshAssets();InvalidateRect(window_,nullptr,FALSE);
}
std::filesystem::path EditorShell::CreateAssetFolder(const std::wstring& name) {
    RequireProject();zengine::projects::ValidateName(name);
    const auto folder=assetLibrary::Resolve(assetsDirectory_,AssetFolder()/name);
    if(!std::filesystem::create_directory(folder))throw std::runtime_error("A folder with this name already exists.");
    RefreshAssets();InvalidateRect(window_,nullptr,FALSE);return folder;
}
void EditorShell::NewAssetFolderDialog() {
    RequireProject();FolderPrompt context;
    struct Template {DLGTEMPLATE dialog;WORD menu=0,windowClass=0,title=0;} layout{};
    layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;layout.dialog.cx=220;layout.dialog.cy=100;
    if(DialogBoxIndirectParamW(instance_,&layout.dialog,window_,Prompt,reinterpret_cast<LPARAM>(&context))==IDOK)CreateAssetFolder(context.name);
}
