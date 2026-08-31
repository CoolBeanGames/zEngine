#include "InputMapEditor.h"
#include "EditorStyle.h"
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <locale>
namespace {
std::wstring Wide(const std::string& s){return {s.begin(),s.end()};}
std::string Text(HWND w){const int size=GetWindowTextLengthW(w);std::wstring s(size+1,L'\0');GetWindowTextW(w,s.data(),size+1);s.resize(size);std::string result;for(wchar_t c:s){if(c<32 || c>126)throw std::runtime_error("Use printable ASCII for action names and settings.");result.push_back(static_cast<char>(c));}return result;}
void Combo(HWND w,const std::vector<std::string>& items){for(const auto& s:items)SendMessageW(w,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(s).c_str()));}
}
InputMapEditor::InputMapEditor(HWND owner,std::filesystem::path assets):assets_(std::move(assets)) {
    loaded_=zengine::input::Load(assets_);map_=zengine::input::Decode(loaded_);
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Procedure;wc.lpszClassName=L"zEngineInputMap";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=editorStyle::Shared().panel;RegisterClassW(&wc);
    window_=CreateWindowExW(0,wc.lpszClassName,L"Input Map",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,800,560,owner,nullptr,wc.hInstance,this);
    if(!window_)throw std::runtime_error("Cannot open Input Map editor.");
    auto control=[&](const wchar_t* cls,const wchar_t* label,int id,DWORD style){auto w=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);return w;};
    list_=control(L"LISTBOX",L"",List,LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP);
    for(const auto& pair:std::vector<std::pair<int,const wchar_t*>>{{Add,L"+ Action"},{Remove,L"Remove Action"},{Save,L"Save"},{Reload,L"Reload"},{Apply,L"Apply Action"}})control(L"BUTTON",pair.second,pair.first,WS_TABSTOP);
    control(L"EDIT",L"",Name,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP);SendDlgItemMessageW(window_,Name,EM_SETLIMITTEXT,80,0);
    auto combo=[&](int id){return control(L"COMBOBOX",L"",id,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);};
    Combo(combo(Type),{"Button","Button Axis 1D","Button Axis 2D","Analog Axis 1D","Analog Axis 2D"});
    for(int i=0;i<4;++i)Combo(combo(Binding0+i),zengine::input::ButtonNames());
    Combo(combo(Controller),{"1","2","3","4"});
    Combo(combo(Analog),{"Left X / Left Stick","Left Y / Right Stick","Right X","Right Y","Left Trigger","Right Trigger"});
    control(L"EDIT",L"0.2",Deadzone,ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP);SendDlgItemMessageW(window_,Deadzone,EM_SETLIMITTEXT,16,0);
    const wchar_t* labels[]={L"Action name (case-sensitive)",L"Action type",L"Button / Negative X",L"Positive X",L"Negative Y (down)",L"Positive Y (up)",L"Controller",L"Analog source",L"Deadzone (0 <= value < 1)"};
    for(int i=0;i<9;++i)control(L"STATIC",labels[i],4200+i,0);
    status_=control(L"STATIC",L"One Input Map per project. Actions are saved explicitly. Changes apply on next Play.",4300,0);
    editorStyle::AttachChildren(window_);Populate();Select(map_.empty()?-1:0);Layout();Title();
}
InputMapEditor::~InputMapEditor(){if(IsWindow(window_))DestroyWindow(window_);}
void InputMapEditor::Title(){SetWindowTextW(window_,(std::wstring(dirty_?L"* ":L"")+L"Input.zinput - zEngine Input Map").c_str());}
void InputMapEditor::Show(){if(!dirty_)Load();ShowWindow(window_,SW_SHOWNORMAL);SetForegroundWindow(window_);}
void InputMapEditor::Layout(){if(!list_)return;RECT r;GetClientRect(window_,&r);const int width=std::max(620L,r.right);MoveWindow(list_,12,48,220,std::max(1L,r.bottom-103),TRUE);MoveWindow(GetDlgItem(window_,Add),12,12,94,26,TRUE);MoveWindow(GetDlgItem(window_,Remove),112,12,120,26,TRUE);MoveWindow(GetDlgItem(window_,Save),width-178,12,76,26,TRUE);MoveWindow(GetDlgItem(window_,Reload),width-94,12,76,26,TRUE);
    const int ids[]={Name,Type,Binding0,Binding0+1,Binding0+2,Binding0+3,Controller,Analog,Deadzone};
    for(int i=0;i<9;++i){MoveWindow(GetDlgItem(window_,4200+i),250,52+i*40,175,24,TRUE);MoveWindow(GetDlgItem(window_,ids[i]),430,48+i*40,width-448,i==0||i==8?26:240,TRUE);}
    MoveWindow(GetDlgItem(window_,Apply),430,412,150,27,TRUE);MoveWindow(status_,12,std::max(445L,r.bottom-42),width-24,36,TRUE);
}
void InputMapEditor::Populate(){SendMessageW(list_,LB_RESETCONTENT,0,0);for(const auto& a:map_)SendMessageW(list_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(a.name).c_str()));}
void InputMapEditor::TypeControls(){
    const int kind=static_cast<int>(SendDlgItemMessageW(window_,Type,CB_GETCURSEL,0,0));
    for(int i=0;i<4;++i)EnableWindow(GetDlgItem(window_,Binding0+i),selected_>=0 && kind<3 && i<(kind==0?1:kind==1?2:4));
    EnableWindow(GetDlgItem(window_,Analog),selected_>=0 && kind>=3);EnableWindow(GetDlgItem(window_,Deadzone),selected_>=0 && kind>=3);
    const auto analog=GetDlgItem(window_,Analog);const int old=static_cast<int>(SendMessageW(analog,CB_GETCURSEL,0,0));SendMessageW(analog,CB_RESETCONTENT,0,0);
    if(kind==4)Combo(analog,{"Left Stick","Right Stick"});else Combo(analog,{"Left Stick X","Left Stick Y","Right Stick X","Right Stick Y","Left Trigger","Right Trigger"});
    SendMessageW(analog,CB_SETCURSEL,std::clamp(old,0,kind==4?1:5),0);
}
void InputMapEditor::Select(int index){loading_=true;selected_=index;SendMessageW(list_,LB_SETCURSEL,index,0);const zengine::input::Action empty;const auto& a=index>=0?map_.at(index):empty;
    SetDlgItemTextW(window_,Name,Wide(a.name).c_str());SendDlgItemMessageW(window_,Type,CB_SETCURSEL,static_cast<int>(a.kind),0);TypeControls();
    for(int i=0;i<4;++i){const auto& names=zengine::input::ButtonNames();SendDlgItemMessageW(window_,Binding0+i,CB_SETCURSEL,std::find(names.begin(),names.end(),a.buttons[i])-names.begin(),0);}
    SendDlgItemMessageW(window_,Controller,CB_SETCURSEL,a.controller,0);SendDlgItemMessageW(window_,Analog,CB_SETCURSEL,a.analog,0);SetDlgItemTextW(window_,Deadzone,Wide(std::to_string(a.deadzone)).c_str());
    for(int id:{Name,Type,Binding0,Binding0+1,Binding0+2,Binding0+3,Controller,Analog,Deadzone,Apply,Remove})EnableWindow(GetDlgItem(window_,id),index>=0);TypeControls();loading_=false;
}
void InputMapEditor::ApplyFields(){if(selected_<0)return;auto next=map_;auto& a=next.at(selected_);a.name=Text(GetDlgItem(window_,Name));a.kind=static_cast<zengine::input::Kind>(SendDlgItemMessageW(window_,Type,CB_GETCURSEL,0,0));
    for(int i=0;i<4;++i){const auto index=SendDlgItemMessageW(window_,Binding0+i,CB_GETCURSEL,0,0);if(index<0)throw std::runtime_error("Select a binding.");a.buttons[i]=zengine::input::ButtonNames().at(index);}
    a.controller=static_cast<int>(SendDlgItemMessageW(window_,Controller,CB_GETCURSEL,0,0));a.analog=static_cast<int>(SendDlgItemMessageW(window_,Analog,CB_GETCURSEL,0,0));
    std::istringstream number(Text(GetDlgItem(window_,Deadzone)));number.imbue(std::locale::classic());if(!(number>>a.deadzone))throw std::runtime_error("Invalid deadzone.");number>>std::ws;if(!number.eof())throw std::runtime_error("Invalid deadzone.");
    zengine::input::Validate(next);if(zengine::input::Encode(next)!=zengine::input::Encode(map_))dirty_=true;map_=std::move(next);Populate();SendMessageW(list_,LB_SETCURSEL,selected_,0);Title();
}
void InputMapEditor::Load(){const auto text=zengine::input::Load(assets_);auto next=zengine::input::Decode(text);map_=std::move(next);loaded_=text;dirty_=false;Populate();Select(map_.empty()?-1:0);Title();}
void InputMapEditor::SaveFile(){ApplyFields();zengine::input::Save(assets_,map_,&loaded_);loaded_=zengine::input::Encode(map_);dirty_=false;Title();SetWindowTextW(status_,L"Saved Input Map. Changes apply on next Play.");}
bool InputMapEditor::ConfirmClose(){if(!dirty_)return true;const auto answer=MessageBoxW(window_,L"Save changes to the project's Input Map?",L"Unsaved Input Map",MB_YESNOCANCEL|MB_ICONQUESTION);if(answer==IDCANCEL)return false;try{if(answer==IDYES)SaveFile();else Load();return true;}catch(const std::exception& e){SetWindowTextW(status_,Wide(e.what()).c_str());return false;}}
LRESULT CALLBACK InputMapEditor::Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<InputMapEditor*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<InputMapEditor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));self->window_=w;}if(!self)return DefWindowProcW(w,m,wp,lp);
    try {
        if(m==WM_CTLCOLORSTATIC || m==WM_CTLCOLOREDIT || m==WM_CTLCOLORLISTBOX || m==WM_CTLCOLORBTN)return editorStyle::ControlColor(m,wp);
        if(m==WM_GETMINMAXINFO){auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={680,555};return 0;}
        if(m==WM_SIZE){self->Layout();return 0;}
        if(m==WM_CLOSE){if(self->ConfirmClose())ShowWindow(w,SW_HIDE);return 0;}
        if(m==WM_COMMAND && !self->loading_){const int id=LOWORD(wp),event=HIWORD(wp);
            if((event==EN_CHANGE && (id==Name || id==Deadzone)) || (event==CBN_SELCHANGE && (id==Type || (id>=Binding0 && id<Binding0+4) || id==Controller || id==Analog))){if(id==Type)self->TypeControls();self->dirty_=true;self->Title();return 0;}
            if(id==List && event==LBN_SELCHANGE){const int next=static_cast<int>(SendMessageW(self->list_,LB_GETCURSEL,0,0));try{self->ApplyFields();self->Select(next);}catch(...){SendMessageW(self->list_,LB_SETCURSEL,self->selected_,0);throw;}return 0;}
            if(id==Apply)self->ApplyFields();
            if(id==Save)self->SaveFile();
            if(id==Reload && (!self->dirty_ || MessageBoxW(w,L"Discard unsaved Input Map edits and reload?",L"Reload Input Map",MB_YESNO|MB_ICONQUESTION)==IDYES))self->Load();
            if(id==Add){self->ApplyFields();auto next=self->map_;zengine::input::Action a;unsigned n=1;do{a.name="action_"+std::to_string(n++);}while(std::any_of(next.begin(),next.end(),[&](const auto& v){return v.name==a.name;}));next.push_back(a);zengine::input::Validate(next);self->map_=std::move(next);self->dirty_=true;self->Populate();self->Select(static_cast<int>(self->map_.size()-1));self->Title();}
            if(id==Remove && self->selected_>=0){self->map_.erase(self->map_.begin()+self->selected_);self->dirty_=true;self->Populate();self->Select(self->map_.empty()?-1:0);self->Title();}
            return 0;
        }
    }catch(const std::exception& e){SetWindowTextW(self->status_,Wide(e.what()).c_str());return 0;}
    return DefWindowProcW(w,m,wp,lp);
}
