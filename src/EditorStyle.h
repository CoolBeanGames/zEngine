#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <string_view>
#include <algorithm>

// Shared native editor visuals. Buttons keep their native activation, keyboard,
// focus, accessibility and checkbox behavior; only their painting is replaced.
namespace editorStyle {
inline constexpr COLORREF Panel=RGB(40,42,47), Face=RGB(52,54,60), Border=RGB(20,21,24);
inline constexpr COLORREF Text=RGB(214,216,221), Muted=RGB(145,149,158), Accent=RGB(108,166,232);
inline constexpr COLORREF Hover=RGB(65,69,77), Pressed=RGB(42,64,91), Disabled=RGB(44,46,51);
struct Resources {
    HBRUSH panel=CreateSolidBrush(Panel),field=CreateSolidBrush(Face);
    HFONT font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    ~Resources(){DeleteObject(panel);DeleteObject(field);DeleteObject(font);}
};
inline Resources& Shared(){static Resources r;return r;}
inline void Fill(HDC dc,RECT r,COLORREF c){HBRUSH b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);}
inline void Outline(HDC dc,RECT r,COLORREF c){HBRUSH b=CreateSolidBrush(c);FrameRect(dc,&r,b);DeleteObject(b);}
inline void Button(HDC dc,RECT r,std::wstring_view label,bool enabled=true,bool hot=false,bool down=false,bool selected=false,bool focused=false,bool checkbox=false,bool checked=false) {
    const int saved=SaveDC(dc);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,enabled?(selected?Accent:Text):Muted);
    Fill(dc,r,checkbox?Panel:!enabled?Disabled:down?Pressed:hot?Hover:Face);
    if(checkbox) {
        const int side=std::min(16L,r.bottom-r.top-4);RECT box{r.left+2,r.top+(r.bottom-r.top-side)/2,r.left+2+side,r.top+(r.bottom-r.top+side)/2};
        Fill(dc,box,!enabled?Disabled:down?Pressed:hot?Hover:Face);Outline(dc,box,focused?Accent:Border);
        if(checked){auto pen=CreatePen(PS_SOLID,2,enabled?Accent:Muted);auto old=SelectObject(dc,pen);MoveToEx(dc,box.left+3,box.top+side/2,nullptr);LineTo(dc,box.left+side/2-1,box.bottom-4);LineTo(dc,box.right-3,box.top+4);SelectObject(dc,old);DeleteObject(pen);}
        r.left=box.right+7;
    }else Outline(dc,r,focused?Accent:Border);
    RECT text=r;InflateRect(&text,-3,-1);DrawTextW(dc,label.data(),static_cast<int>(label.size()),&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|(checkbox?DT_LEFT:DT_CENTER));
    if(focused){RECT focus=r;InflateRect(&focus,-3,-3);DrawFocusRect(dc,&focus);}
    RestoreDC(dc,saved);
}
struct ButtonState {bool hot=false,combo=false;};
inline void PaintButton(HWND w,HDC dc,const ButtonState& state) {
    if(state.combo){
        RECT r;GetClientRect(w,&r);const bool enabled=IsWindowEnabled(w)!=FALSE;
        const int saved=SaveDC(dc);SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(w,WM_GETFONT,0,0)));
        const bool down=SendMessageW(w,CB_GETDROPPEDSTATE,0,0)!=0;Fill(dc,r,!enabled?Disabled:down?Pressed:state.hot?Hover:Face);Outline(dc,r,GetFocus()==w?Accent:Border);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,enabled?Text:Muted);
        const auto index=SendMessageW(w,CB_GETCURSEL,0,0);
        if(index!=CB_ERR){const auto length=SendMessageW(w,CB_GETLBTEXTLEN,index,0);if(length>=0 && length<32768){std::wstring text(static_cast<std::size_t>(length)+1,L'\0');SendMessageW(w,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(text.data()));RECT label{r.left+6,r.top+1,r.right-23,r.bottom-1};DrawTextW(dc,text.c_str(),static_cast<int>(length),&label,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);}}
        const int x=r.right-12,y=(r.top+r.bottom)/2;POINT points[]={{x-4,y-2},{x+4,y-2},{x,y+3}};auto brush=CreateSolidBrush(enabled?Text:Muted);auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));Polygon(dc,points,3);SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);RestoreDC(dc,saved);return;
    }
    RECT r;GetClientRect(w,&r);const auto bits=SendMessageW(w,BM_GETSTATE,0,0);const auto style=GetWindowLongPtrW(w,GWL_STYLE)&BS_TYPEMASK;
    const bool check=style==BS_CHECKBOX || style==BS_AUTOCHECKBOX || style==BS_3STATE || style==BS_AUTO3STATE;
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(w))+1,L'\0');text.resize(GetWindowTextW(w,text.data(),static_cast<int>(text.size())));
    auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(w,WM_GETFONT,0,0)));
    Button(dc,r,text,IsWindowEnabled(w)!=FALSE,state.hot,(bits&BST_PUSHED)!=0,false,GetFocus()==w,check,(bits&BST_CHECKED)!=0);
    SelectObject(dc,old);
}
inline LRESULT CALLBACK ButtonProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    auto* state=reinterpret_cast<ButtonState*>(data);
    if(m==WM_NCDESTROY){RemoveWindowSubclass(w,ButtonProcedure,id);delete state;return DefSubclassProc(w,m,wp,lp);}
    if(m==WM_PAINT){PAINTSTRUCT paint;auto dc=BeginPaint(w,&paint);PaintButton(w,dc,*state);EndPaint(w,&paint);return 0;}
    if(m==WM_PRINTCLIENT){PaintButton(w,reinterpret_cast<HDC>(wp),*state);return 0;}
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_MOUSEMOVE && !state->hot){state->hot=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);InvalidateRect(w,nullptr,FALSE);}
    if(m==WM_MOUSELEAVE){state->hot=false;InvalidateRect(w,nullptr,FALSE);}
    const auto result=DefSubclassProc(w,m,wp,lp);
    if(IsWindow(w) && (m==WM_LBUTTONDOWN || m==WM_LBUTTONUP || m==WM_KEYDOWN || m==WM_KEYUP || m==WM_ENABLE || m==WM_SETFOCUS || m==WM_KILLFOCUS || m==BM_SETCHECK || m==BM_SETSTATE || m==CB_SETCURSEL || m==CB_SHOWDROPDOWN || m==WM_SETTEXT || m==WM_UPDATEUISTATE || m==WM_CAPTURECHANGED))InvalidateRect(w,nullptr,FALSE);
    return result;
}
inline void Attach(HWND w) {
    wchar_t name[32]{};GetClassNameW(w,name,32);const bool combo=_wcsicmp(name,L"COMBOBOX")==0 && (GetWindowLongPtrW(w,GWL_STYLE)&3)==CBS_DROPDOWNLIST;if(!combo && _wcsicmp(name,L"BUTTON")!=0)return;
    DWORD_PTR existing=0;if(GetWindowSubclass(w,ButtonProcedure,0x5a454e,&existing))return;
    auto* state=new ButtonState;
    state->combo=combo;
    if(!SetWindowSubclass(w,ButtonProcedure,0x5a454e,reinterpret_cast<DWORD_PTR>(state))){delete state;return;}
    SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(Shared().font),FALSE);InvalidateRect(w,nullptr,FALSE);
}
inline void AttachChildren(HWND parent){EnumChildWindows(parent,[](HWND w,LPARAM)->BOOL{Attach(w);return TRUE;},0);}
inline LRESULT ControlColor(UINT message,WPARAM wp){auto dc=reinterpret_cast<HDC>(wp);const bool field=message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX;SetTextColor(dc,Text);SetBkColor(dc,field?Face:Panel);return reinterpret_cast<LRESULT>(field?Shared().field:Shared().panel);}
}
