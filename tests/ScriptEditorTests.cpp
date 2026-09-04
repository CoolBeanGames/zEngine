#include "ScriptAssets.h"
#include "ShaderAssets.h"
#include "MaterialAssets.h"
#include "LightmapAssets.h"
#include "ScriptEditor.h"
#include "ScriptTyping.h"
#include "ScriptCompletion.h"
#include "core/ScriptBehavior.h"
#include <windows.h>
#include <richedit.h>
#include <cmath>
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
            // ZE-79: a private member is offered inside its own class but not through another value.
            Check(has(complete(L"class Sec { private int _key; func f() { _"),L"_key"),"private member missing from its own class");
            Check(!has(complete(L"class Sec { private int _key; } class B { func f() { Sec s; s._"),L"_key"),"private member leaked through another value");
            Check(has(complete(L"class A { func f() { Other obj; obj.work()."),L"x"),"Return-type chain completion missing");
            Check(has(complete(L"class A { func f() { Input.action(\"move_left\")."),L"just_pressed"),"Input action chain missing");
            Check(has(complete(L"class A { func f() { Input.is_action_pressed(\"move_"),L"move_left"),"Input name completion missing");
            Check(has(complete(L"class A { func f() { string tag=\"En"),L"Enemy"),"Tag completion missing");
            Check(complete(L"// Input.").items.empty(),"Completion active in comment");
            Check(!has(complete(L"class A { func f(){ int hidden; } func g(){ hid"),L"hidden"),"Function-local name leaked to another function");
            Check(!has(complete(L"class A { func f(){ { int hidden; } hid"),L"hidden"),"Block-local name escaped its scope");
            // ZE-71: hover signatures for calls and signals.
            const auto hoverAt=[&](const std::wstring& text,const wchar_t* needle){
                const auto at=text.find(needle); return at==text.npos?std::wstring{}:index.Hover(text,at+1); };
            Check(hoverAt(L"class A : gameObject { func update(float delta){ update(0); } }",L"update(0")==L"update(float delta)","Own-function hover signature wrong");
            Check(hoverAt(L"class A : gameObject { func f(){ Mathf.lerp(0,1,0.5); } }",L"lerp(")==L"lerp(float from, float to, float weight) \x2192 float","Built-in call hover signature wrong");
            Check(hoverAt(L"class A : gameObject { func f(){ Input.mouse.clicked.connect(f); } }",L"clicked.")==L"clicked(int button)  (signal)","Signal hover signature wrong");
            Check(hoverAt(L"class A : gameObject { export int x=0; func f(){ x=1; } }",L"x=1").empty(),"Plain field should have no hover");
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
            // Line numbers: a left-hand gutter (numbers + fold strip) is reserved; repainting over it is safe. (ZE-89)
            const auto margins=SendMessageW(control,EM_GETMARGINS,0,0);
            Check(LOWORD(margins)==ScriptEditor::LeftGutter,"Left line-number gutter not reserved");
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
        // ZE-64: HLSL material shader (.shader) assets.
        {
            namespace sh = zengine::shaders;
            const auto shaderA = sh::Create(root), shaderB = sh::Create(root);
            Check(shaderA != shaderB && shaderA.extension() == ".shader", "Unique .shader creation failed");
            const auto templateSrc = sh::Load(shaderA);
            Check(templateSrc.find("PSMain") != std::string::npos, "Shader template missing an entry point");

            const auto parsed = sh::Parse(templateSrc);
            Check(parsed.compiled && parsed.errors.empty(), "Valid template HLSL did not compile");
            const auto hasParam = [&](const char* name, sh::ParamType type) {
                return std::any_of(parsed.parameters.begin(), parsed.parameters.end(),
                    [&](const sh::Parameter& p) { return p.name == name && p.type == type; });
            };
            Check(hasParam("albedo", sh::ParamType::Float4), "cbuffer float4 parameter not parsed");
            Check(hasParam("smoothness", sh::ParamType::Float), "cbuffer float parameter not parsed");
            Check(hasParam("albedoMap", sh::ParamType::Texture2D), "Texture2D slot not parsed");
            for (const auto& p : parsed.parameters)
                if (p.name == "albedo") Check(p.value[0] == 1 && p.value[3] == 1, "float4 default not read from comment");
                else if (p.name == "smoothness") Check(p.value[0] == 0.5f, "float default not read from comment");

            const auto broken = sh::Parse("float4 PSMain() : SV_TARGET { return notAThing(); }");
            Check(!broken.compiled && !broken.errors.empty(), "Invalid HLSL was reported as compiling");

            const std::wstring templateWide(templateSrc.begin(), templateSrc.end()); // template is ASCII
            Check(sh::Analyze(templateWide).errors.empty(), "Valid shader flagged by the analyzer");
            Check(!sh::Analyze(L"float4 PSMain() : SV_TARGET { return bogus(); }").errors.empty(),
                  "Analyzer missed an HLSL compile error");
            Check(!sh::Analyze(L"cbuffer P { float x; ").errors.empty(), "Analyzer missed an unclosed brace");

            bool rejected = false;
            try { sh::Resolve(root, root / "not-a-shader.txt"); } catch (...) { rejected = true; }
            Check(rejected, "Shader extension check failed");

            ScriptEditor shaderEditor(nullptr, root, shaderA);
            HWND shaderControl = GetDlgItem(shaderEditor.Window(), ScriptEditor::SourceControl);
            Check(shaderControl != nullptr, "Shader editor control missing");
            SetWindowTextW(shaderControl, L"float4 PSMain() : SV_TARGET { return float4(1,0,0,1); }");
            SendMessageW(shaderEditor.Window(), WM_TIMER, 1, 0);
            SendMessageW(shaderEditor.Window(), WM_COMMAND, ScriptEditor::SaveCommand, 0);
            Check(sh::Load(shaderA).find("float4(1,0,0,1)") != std::string::npos, "Shader editor save failed");
            Check(shaderEditor.ConfirmClose(), "Saved shader editor should close without prompting");

            // ZE-65: Material Instance (.material) assets.
            namespace mt = zengine::materials;
            const auto matA = mt::Create(root), matB = mt::Create(root);
            Check(matA != matB && matA.extension() == ".material", "Unique .material creation failed");
            auto doc = mt::Load(matA);
            Check(doc.shader.empty() && doc.values.size() == 4, "Material template should be the built-in Standard (tint, albedo, roughness, specular)");

            // Round-trip via encode/decode and via the disk save path.
            Check(mt::Decode(mt::Encode(doc)) == doc, "Material encode/decode round trip changed the document");
            doc.values[0].numbers = {{0.2f, 0.4f, 0.6f, 1.0f}};   // tint
            doc.values[1].texture = "Textures/wood.png";          // albedo
            const auto snapshot = mt::Load(matA);
            mt::Save(root, matA, doc, &snapshot);
            Check(mt::Load(matA) == doc, "Material save did not persist edited values");
            bool rejectedMat = false;
            try { mt::Save(root, matA, doc, &snapshot); } catch (...) { rejectedMat = true; }
            Check(rejectedMat, "Material save ignored a stale expected snapshot");

            // Built-in Standard resolves without a shader; a custom shader is compiled.
            const auto stdEff = mt::Resolve(doc, [](const std::string&) -> std::string { throw std::runtime_error("no shader"); });
            Check(stdEff.builtin && stdEff.ok && stdEff.Numbers("tint")[2] == 0.6f && stdEff.Texture("albedo") == "Textures/wood.png",
                  "Built-in material did not merge pinned values");
            mt::MaterialDoc custom; custom.shader = "Shaders/Fancy.shader";
            custom.values.push_back(mt::Value{"albedo", zengine::shaders::ParamType::Float4, {{1, 0, 0, 1}}, ""});
            const auto customEff = mt::Resolve(custom, [](const std::string&) {
                return std::string("cbuffer Parameters { float4 albedo; };\nfloat4 PSMain() : SV_TARGET { return albedo; }");
            });
            Check(customEff.ok && !customEff.builtin && customEff.Numbers("albedo")[0] == 1.0f && customEff.Numbers("albedo")[1] == 0.0f,
                  "Custom-shader material did not compile / merge");
            const auto brokenEff = mt::Resolve(custom, [](const std::string&) { return std::string("not hlsl at all"); });
            Check(!brokenEff.ok && !brokenEff.error.empty(), "Broken custom shader was not reported by the material resolver");

            bool rejectedResolve = false;
            try { mt::Resolve(root, root / "x.txt"); } catch (...) { rejectedResolve = true; }
            Check(rejectedResolve, "Material extension check failed");

            // ZE-113: static lightmap baking + .lightmap assets.
            namespace lm = zengine::lightmap;
            // Two unit quads: a floor at y=0 and a blocker slab just above it. A
            // single overhead directional light should light the blocker's top
            // and leave the floor beneath it in shadow.
            const auto quad = [](float y, float ox, float oz) {
                lm::BakeMesh m;
                for (float dx : {-1.0f, 1.0f}) for (float dz : {-1.0f, 1.0f})
                    { m.positions.push_back({ox + dx, y, oz + dz}); m.normals.push_back({0, 1, 0}); }
                m.indices = {0, 2, 1, 1, 2, 3};
                return m;
            };
            lm::BakeMesh floor = quad(0.0f, 0.0f, 0.0f); floor.object = 1;
            lm::BakeMesh slab = quad(0.5f, 0.0f, 0.0f); slab.object = 2;
            lm::BakeLight sun; sun.type = 0; sun.direction = {0, -1, 0}; sun.color = {1, 1, 1}; sun.intensity = 1;
            const auto baked = lm::Bake({floor, slab}, {sun}, {0.05f, 0.05f, 0.05f});
            Check(baked.entries.size() == 2 && baked.Find(1) && baked.Find(2), "Bake did not produce an entry per mesh");
            const float floorLum = baked.Find(1)->colors[0].x;
            const float slabLum = baked.Find(2)->colors[0].x;
            Check(slabLum > 0.9f, "Bake: unshadowed slab top should be near full brightness");
            Check(floorLum < 0.2f, "Bake: floor under the slab should be shadowed");
            Check(lm::Decode(lm::Encode(baked)) == baked, "Lightmap encode/decode round trip changed the document");

            // Apply multiplies the base model's vertex colours by the baked term.
            ModelData base; base.vertices.resize(4);
            for (auto& v : base.vertices) v.color = {1, 1, 1};
            const auto applied = lm::Apply(base, *baked.Find(2));
            Check(std::abs(applied.vertices[0].color.x - slabLum) < 1e-4f, "Apply did not fold in the baked term");
            base.vertices.resize(3); // vertex-count mismatch => unchanged
            Check(lm::Apply(base, *baked.Find(2)).vertices.size() == 3, "Apply should no-op on a vertex-count mismatch");

            const auto lmPath = lm::Resolve(root, root / "Bake.lightmap");
            lm::Save(root, lmPath, baked);
            Check(lm::Load(lmPath) == baked, "Lightmap disk save/load round trip changed the document");
            bool rejectedLm = false;
            try { lm::Resolve(root, root / "x.txt"); } catch (...) { rejectedLm = true; }
            Check(rejectedLm, "Lightmap extension check failed");
        }
        CoUninitialize();
        std::filesystem::remove_all(root);
        std::cout << "PASS: script assets, safe saves, diagnostics, behavior references, native editor save/load/undo, HLSL shaders, material instances\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; std::filesystem::remove_all(root); return 1; }
}
