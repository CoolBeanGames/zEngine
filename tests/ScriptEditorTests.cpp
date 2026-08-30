#include "ScriptAssets.h"
#include "ScriptEditor.h"
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
    const bool captured=PrintWindow(window,buffer,2)!=FALSE;
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
