#include "EditorStyle.h"
#include <iostream>
#include <stdexcept>
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int clicked=0;
LRESULT CALLBACK Parent(HWND w,UINT m,WPARAM wp,LPARAM lp){if(m==WM_COMMAND && HIWORD(wp)==BN_CLICKED){++clicked;return 0;}return DefWindowProcW(w,m,wp,lp);}
}
int main(){try{
    WNDCLASSW wc{};wc.lpfnWndProc=Parent;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"zEngineStyleTest";RegisterClassW(&wc);
    HWND parent=CreateWindowW(wc.lpszClassName,L"Test",WS_POPUP,0,0,300,200,nullptr,nullptr,wc.hInstance,nullptr);
    HWND button=CreateWindowW(L"BUTTON",L"Play",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,140,28,parent,reinterpret_cast<HMENU>(1),wc.hInstance,nullptr);
    HWND check=CreateWindowW(L"BUTTON",L"Enabled",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,40,140,28,parent,reinterpret_cast<HMENU>(2),wc.hInstance,nullptr);
    HWND combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,0,80,140,150,parent,reinterpret_cast<HMENU>(3),wc.hInstance,nullptr);
    SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Button"));SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Axis"));
    editorStyle::AttachChildren(parent);editorStyle::Attach(button); // Idempotent, preserves the same native control.
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,140,28);auto old=SelectObject(dc,bitmap);ReleaseDC(nullptr,screen);
    auto pixel=[&](){SendMessageW(button,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);return GetPixel(dc,3,3);};
    Check(pixel()==editorStyle::Face,"Default button face differs from Play palette");
    SendMessageW(button,WM_MOUSEMOVE,0,MAKELPARAM(5,5));Check(pixel()==editorStyle::Hover,"Hover paint");
    SendMessageW(button,BM_SETSTATE,TRUE,0);Check(pixel()==editorStyle::Pressed,"Pressed paint");SendMessageW(button,BM_SETSTATE,FALSE,0);
    EnableWindow(button,FALSE);Check(pixel()==editorStyle::Disabled,"Disabled paint");EnableWindow(button,TRUE);
    SendMessageW(button,BM_CLICK,0,0);Check(clicked==1,"Native command activation lost");
    SendMessageW(check,BM_CLICK,0,0);Check(SendMessageW(check,BM_GETCHECK,0,0)==BST_CHECKED && clicked==2,"Native checkbox behavior lost");
    SendMessageW(combo,CB_SETCURSEL,1,0);SendMessageW(combo,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);Check(GetPixel(dc,3,3)==editorStyle::Face && SendMessageW(combo,CB_GETCURSEL,0,0)==1,"Combo style or native selection lost");
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);for(int i=0;i<500;++i)pixel();Check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=before+1,"Button painting leaked GDI resources");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(parent);
    std::cout<<"PASS shared button states, native activation, checkbox and GDI resources\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
