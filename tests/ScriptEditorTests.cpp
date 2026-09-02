#include "ScriptAssets.h"
#include "ScriptEditor.h"
#include "ScriptTyping.h"
#include "ScriptCompletion.h"
#include "core/ScriptBehavior.h"
#include <windows.h>
#include <richedit.h>
#include <iostream>
#include <fstream>
#include <stdexcept>

void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// Optional visual QA artifact, captured from the actual RichEdit window (not a mockup).
void Capture(HWND window)
{
    ShowWindow(window, SW_SHOWNOACTIVATE);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
    RECT r{}; GetWindowRect(window,&r);
    HDC dc=GetDC(window), buffer=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right-r.left,r.bottom-r.top);
    auto old=SelectObject(buffer,bitmap);
    const bool captured=PrintWindow(window,buffer,0)!=FALSE;
    SelectObject(buffer,old);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=r.right-r.left; info.bmiHeader.biHeight=-(r.bottom-r.top);
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    std::vector<char> pixels(static_cast<std::size_t>(r.right-r.left)*(r.bottom-r.top)*4);
    const bool read=GetDIBits(buffer,bitmap,0,r.bottom-r.top,pixels.data(),&info,DIB_RGB_COLORS)!=0;
    DeleteObject(bitmap); DeleteDC(buffer); ReleaseDC(window,dc);
    Check(captured && read,"Screenshot capture failed");
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream file("script-editor-qa.bmp",std::ios::binary);
    file.write(reinterpret_cast<char*>(&header),sizeof(header));
    file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    file.write(pixels.data(),static_cast<std::streamsize>(pixels.size()));
    Check(static_cast<bool>(file),"Screenshot write failed");
}
int main(int argc, char**)
{
    const auto root = std::filesystem::temp_directory_path() / (L"zEngine-script-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        using namespace zengine::scripts;
        {
            scriptCompletion::Index index;
            index.AddSource(L"class Base { signal ping; int count; func work():Vector3 { return Vector3(); } } class Other : Base { float speed; }");
            index.AddString(L"move_left");index.AddString(L"Enemy");
            const auto complete=[&](const std::wstring& text){return index.Complete(text,text.size());};
            const auto has=[](const auto& result,const wchar_t* name){return std::any_of(result.items.begin(),result.items.end(),[&](const auto& item){return item.name==name;});};
            Check(has(complete(L"class A : gameObject { func upd"),L"update"),"Lifecycle completion missing");
            Check(has(complete(L"class A : gameObject { func physics"),L"physicsUpdate"),"Physics lifecycle completion missing");
            Check(has(complete(L"class A : gameObject { func f(){ transform.global_"),L"global_position"),"Global transform completion missing");
            Check(has(complete(L"class A { func f(){ string s; s."),L"truncate"),"String method completion missing");
            Check(has(complete(L"class A { func f(){ prefab p; p."),L"spawn"),"Prefab spawn completion missing");
            Check(complete(L"class A { char c='a").items.empty(),"String suggestions leaked into character literal");
            Check(has(complete(L"class A : gameObject { func f(){ parent."),L"transform"),"Parent member completion missing");
            Check(has(complete(L"class A : RigidBody { func f(){ rigidbody."),L"add_impulse"),"RigidBody reference completion missing");
            Check(has(complete(L"class A { func f(){ Mathf."),L"cross"),"Mathf completion missing");
            Check(has(complete(L"class A : gameObject { func f(){ make_timer(1)."),L"finished"),"Timer signal completion missing");
            Check(has(complete(L"class A : gameObject { func f(){ rigidbody body; body."),L"add_force"),"Lowercase native type completion missing");
            Check(has(complete(L"class A : gameObject { func f(){ find(\"Root\")."),L"parent"),"Object lookup return completion missing");
            Check(has(complete(L"class A { func f() { Other obj; obj."),L"speed"),"Typed member completion missing");
            Check(has(complete(L"class A { func f() { Other obj; obj."),L"ping"),"Inherited signal missing");
            Check(!has(complete(L"class A { func f() { Vector3 v; v."),L"speed"),"Unrelated member leaked into list");
            Check(has(complete(L"class A { func f() { Other obj; obj.work()."),L"x"),"Return-type chain completion missing");
            Check(has(complete(L"class A { func f() { Input.action(\"move_left\")."),L"just_pressed"),"Input action chain missing");
            Check(has(complete(L"class A { func f() { Input.is_action_pressed(\"move_"),L"move_left"),"Input name completion missing");
            Check(has(complete(L"class A { func f() { string tag=\"En"),L"Enemy"),"Tag completion missing");
            Check(complete(L"// Input.").items.empty(),"Completion active in comment");
            Check(!has(complete(L"class A { func f(){ int hidden; } func g(){ hid"),L"hidden"),"Function-local name leaked to another function");
            Check(!has(complete(L"class A { func f(){ { int hidden; } hid"),L"hidden"),"Block-local name escaped its scope");
        }
        {
            const std::wstring source=L"class A {\r    func update(float delta";
            auto edit=scriptTyping::OnCharacter(source,source.size(),source.size(),L')');
            Check(edit && edit->text==L")\r    {\r        \r    }" && edit->caret==16,"Function block completion/indent incorrect");
            Check(!scriptTyping::OnCharacter(L"foo(",4,4,L')'),"Function call completed as declaration");
            Check(!scriptTyping::OnCharacter(L"// func foo(",12,12,L')'),"Comment completed as declaration");
            Check(!scriptTyping::OnCharacter(L"func foo( {}",9,9,L')'),"Duplicate function body");
            edit=scriptTyping::OnCharacter(L"{ /* } */ {",11,11,L'\r');
            Check(edit && edit->text==L"\r        ","Comment affected indentation");
            edit=scriptTyping::OnCharacter(L"{}",1,1,L'\r');
            Check(edit && edit->text==L"\r    \r","Empty brace pair indentation failed");
            edit=scriptTyping::OnCharacter(L"    if",6,6,L' ');
            Check(edit && edit->text==L" ()\r{\r    \r}" && edit->caret==2,"If block completion incorrect");
            edit=scriptTyping::OnCharacter(L"    while",9,9,L' ');
            Check(edit && edit->text==L" ()\r{\r    \r}" && edit->caret==2,"Loop block completion incorrect");
            edit=scriptTyping::OnCharacter(L"    else",8,8,L' ');
            Check(edit && edit->text==L"\r{\r    \r}" && edit->caret==7,"Else block completion incorrect");
            Check(!scriptTyping::OnCharacter(L"// if",5,5,L' '),"Comment control flow completed");
            edit=scriptTyping::OnCharacter(L"value",5,5,L'(');Check(edit&&edit->text==L"()"&&edit->caret==1,"Manual parenthesis did not pair");
            edit=scriptTyping::OnCharacter(L"array",5,5,L'[');Check(edit&&edit->text==L"[]"&&edit->caret==1,"Manual square bracket did not pair");
            edit=scriptTyping::OnCharacter(L"body",4,4,L'{');Check(edit&&edit->text==L"{\r    \r}"&&edit->caret==5,"Manual brace did not create an editable block");
            edit=scriptTyping::OnCharacter(L"name",0,4,L'(');Check(edit&&edit->text==L"(name)"&&edit->caret==5,"Selected text was not wrapped by bracket pair");
            edit=scriptTyping::OnCharacter(L"()",1,1,L')');Check(edit&&edit->text.empty()&&edit->caret==1,"Closing parenthesis did not advance over its pair");
            Check(!scriptTyping::OnCharacter(L"\"text",5,5,L'('),"Bracket pairing leaked into a string");
        }
        const auto first = Create(root), second = Create(root);
        Check(first != second && first.extension() == ".zsh", "Unique .zsh creation failed");
        const auto original = Load(first);
        Check(original.find("class NewBehavior : gameObject") != std::string::npos, "Behavior template missing");
        Check(Analyze(L"class Test : gameObject { func start() {} }").errors.empty(), "Valid template marked invalid");
        Check(Analyze(L"// ignored {\r\nclass A { string text = \"}\"; }").errors.empty(), "Comments/string bracket handling failed");
        Check(!Analyze(L"class A { func start( } ").errors.empty(), "Bracket errors not found");
        Check(!Analyze(L"\"unfinished").errors.empty() && !Analyze(L"/* unfinished").errors.empty(), "Unterminated token errors missing");
        Check(!Analyze(L"@invalid").errors.empty(), "Invalid character errors missing");
        Check(Analyze(std::wstring(10000,L'@')).errors.size()==100,"Diagnostic count must remain bounded");
        bool rejected = false;
        try { Resolve(root, root.parent_path()/"outside.zsh"); } catch (...) { rejected=true; }
        Check(rejected, "Project containment failed");
        rejected=false;
        try { Resolve(root, root/"not-script.txt"); } catch (...) { rejected=true; }
        Check(rejected, "Extension check failed");
        Save(root, first, "class Changed {}", &original);
        rejected=false;
        try { Save(root, first, "overwrite", &original); } catch (...) { rejected=true; }
        Check(rejected && Load(first)=="class Changed {}", "External changes must not be overwritten");
        rejected=false;
        try { Save(root, first, std::string(MaxSourceBytes+1,'x')); } catch (...) { rejected=true; }
        Check(rejected && Load(first)=="class Changed {}", "Oversized save must preserve original");
        zengine::ObjectStore objects;
        auto& object=objects.Create("Actor");
        auto& behavior=object.AddBehavior<zengine::ScriptBehavior>("NewBehavior.zsh");
        Check(&behavior.Owner()==&object && &object.BehaviorAt(0)==&behavior && behavior.Asset()=="NewBehavior.zsh", "Script attachment ownership failed");
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            ScriptEditor editor(nullptr,root,first);
            HWND control=GetDlgItem(editor.Window(),ScriptEditor::SourceControl);
            Check(control!=nullptr,"Source editor control missing");
            SetWindowTextW(control,L"class A : gameObject { func up");SendMessageW(control,EM_SETSEL,-1,-1);
            SendMessageW(control,WM_CHAR,'d',0);
            wchar_t suggestion[200]{};GetWindowTextW(control,suggestion,200);
            Check(std::wstring(suggestion).ends_with(L"upd"),"Ghost completion modified source before acceptance");
            SendMessageW(control,WM_CHAR,VK_TAB,0);GetWindowTextW(control,suggestion,200);
            Check(std::wstring(suggestion).ends_with(L"update"),"Tab did not accept inline suggestion");
            SendMessageW(control,EM_UNDO,0,0);GetWindowTextW(control,suggestion,200);
            Check(std::wstring(suggestion).ends_with(L"upd"),"Completion undo was not atomic");
            SendMessageW(control,WM_KEYDOWN,VK_RETURN,0);SendMessageW(control,WM_CHAR,VK_RETURN,0);SendMessageW(control,WM_CHAR,VK_TAB,0);GetWindowTextW(control,suggestion,200);
            Check(std::wstring(suggestion).find(L"update")==std::wstring::npos,"Enter did not end autocomplete");
            SetWindowTextW(control,L"first");SendMessageW(control,EM_SETSEL,-1,-1);
            SendMessageW(control,WM_KEYDOWN,VK_RETURN,0);SendMessageW(control,WM_CHAR,VK_RETURN,0);
            Check(SendMessageW(control,EM_GETLINECOUNT,0,0)==2,"Enter inserted more than one line");
            SetWindowTextW(control,L"class A { func f() { Vector3 v; v");SendMessageW(control,EM_SETSEL,-1,-1);SendMessageW(control,WM_CHAR,'.',0);
            SendMessageW(control,WM_KEYDOWN,VK_DOWN,0);SendMessageW(control,WM_CHAR,VK_TAB,0);GetWindowTextW(control,suggestion,200);
            Check(std::wstring(suggestion).ends_with(L"v.y"),"Member dropdown selection/Tab failed");
            SetWindowTextW(control,L"class A {\r    func update(float delta");
            SendMessageW(control,EM_SETSEL,-1,-1);SendMessageW(control,WM_CHAR,')',0);
            wchar_t typed[200]{};GetWindowTextW(control,typed,200);
            Check(std::wstring(typed).find(L"{\r\n        \r\n    }")!=std::wstring::npos,"Native function typing did not create body");
            SendMessageW(control,EM_UNDO,0,0);GetWindowTextW(control,typed,200);
            Check(std::wstring(typed).find(L"delta)")==std::wstring::npos,"Auto-inserted body was not one undo operation");
            SetWindowTextW(control,L"class Folded {\r\n    func work()\r\n    {\r\n        int value=1;\r\n    }\r\n}");SendMessageW(control,EM_SETSEL,0,0);SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::FoldCommand,0);
            SendMessageW(control,EM_SETSEL,14,15);CHARFORMAT2W folded{};folded.cbSize=sizeof(folded);SendMessageW(control,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&folded));Check((folded.dwEffects&CFE_HIDDEN)!=0,"Class block did not collapse");
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::ExpandCommand,0);SendMessageW(control,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&folded));Check((folded.dwEffects&CFE_HIDDEN)==0,"Expand all left code hidden");
            SetWindowTextW(control,L"class Edited : gameObject\r\n{\r\n    int value = 3;\r\n}\r\n");
            SendMessageW(editor.Window(),WM_TIMER,1,0);
            // Timer-based syntax formatting must preserve selection and undo history.
            SendMessageW(control,EM_SETSEL,5,11);
            SendMessageW(editor.Window(),WM_TIMER,1,0);
            DWORD start=0,end=0; SendMessageW(control,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
            Check(start==5 && end==11,"Highlighting changed selection");
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::SaveCommand,0);
            Check(Load(first).find("class Edited")!=std::string::npos, "Editor save failed");
            const auto disk=Load(first);
            Save(root,first,"class Reloaded {}",&disk);
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::ReloadCommand,0);
            wchar_t buffer[100]{}; GetWindowTextW(control,buffer,100);
            Check(std::wstring(buffer)==L"class Reloaded {}","Editor reload failed");
            SendMessageW(control,EM_SETSEL,0,-1);
            SendMessageW(control,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"class UndoTest {}"));
            SendMessageW(editor.Window(),WM_TIMER,1,0);
            SendMessageW(control,EM_UNDO,0,0);
            GetWindowTextW(control,buffer,100);
            Check(std::wstring(buffer)==L"class Reloaded {}","Syntax colors polluted text undo");
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::SaveCommand,0);
            Check(editor.ConfirmClose(),"Saved editor should close without prompting");
            SetWindowTextW(control,L"class Broken {");
            SendMessageW(editor.Window(),WM_TIMER,1,0);
            SendMessageW(control,EM_SETSEL,13,14);
            CHARFORMAT2W format{}; format.cbSize=sizeof(format);
            SendMessageW(control,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&format));
            Check((format.dwEffects&CFE_UNDERLINE)!=0,"Syntax error is not highlighted in source");
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::ErrorCommand,0);
            SendMessageW(control,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
            Check(start==13,"Go to error did not navigate to diagnostic");
            // Line numbers: a right-hand gutter is reserved and repainting over it is safe.
            const auto margins=SendMessageW(control,EM_GETMARGINS,0,0);
            Check(HIWORD(margins)==ScriptEditor::LineNumberGutter && LOWORD(margins)==28,"Line number gutter not reserved");
            SetWindowTextW(control,L"one\r\ntwo\r\nthree\r\nfour\r\nfive");
            SendMessageW(editor.Window(),WM_TIMER,1,0);
            Check(SendMessageW(control,EM_GETLINECOUNT,0,0)==5,"Line count wrong for gutter test");
            RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            if (argc > 1)
            {
                SetWindowTextW(control,L"class PlayerBehavior : gameObject\r\n{\r\n    // Per-instance behavior data\r\n    float speed = 3.5;\r\n    string label = \"Player\";\r\n\r\n    func start()\r\n    {\r\n    }\r\n\r\n    func update(float delta)\r\n    {\r\n        speed = speed + delta;\r\n    }\r\n\r\n    func draw()\r\n    {\r\n    }\r\n// Missing closing brace: error highlighting demo\r\n");
                SendMessageW(editor.Window(),WM_TIMER,1,0);
                SendMessageW(control,EM_SETSEL,0,0);
                Capture(editor.Window());
            }
            SendMessageW(editor.Window(),WM_COMMAND,ScriptEditor::SaveCommand,0);
            Check(editor.ConfirmClose(),"Invalid source should still be savable");
        }
        CoUninitialize();
        std::filesystem::remove_all(root);
        std::cout << "PASS: script assets, safe saves, diagnostics, behavior references, native editor save/load/undo\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; std::filesystem::remove_all(root); return 1; }
}
