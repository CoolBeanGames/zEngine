#include "EditorShell.h"
#include "ProjectDialog.h"
#include "SceneAssets.h"
#include "ScriptAssets.h"
#include "ScriptEditor.h"
#include "FbxImporter.h"
#include "WindowCapture.h"
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <functional>
#include <deque>
#include <stdexcept>

namespace
{
    void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
    void Reject(const std::function<void()>& action)
    {
        bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; }
        Check(rejected,"Invalid operation was accepted");
    }
    struct Directory
    {
        std::filesystem::path path=std::filesystem::temp_directory_path()/(L"zEngine-project-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        Directory() { Check(std::filesystem::create_directory(path),"Cannot reserve project test folder"); }
        ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); } // Only this test's newly reserved folder.
    };
    void Put(const std::filesystem::path& path,const std::string& text)
    { std::ofstream out(path,std::ios::binary); out<<text; Check(static_cast<bool>(out),"Cannot write fixture"); }

    // Answer only dialogs owned by this test's editor (including its script editor windows).
    struct DialogAnswers
    {
        HWND owner; std::deque<int> answers; std::filesystem::path location; bool capture=false;
        unsigned handled=0; UINT_PTR timer=0; static inline DialogAnswers* active=nullptr;
        DialogAnswers(HWND window,std::initializer_list<int> responses):owner(window),answers(responses)
        {
            // This test constructs multiple editors on one thread. Closing a previous test editor
            // posts WM_QUIT, which would otherwise dismiss a later MessageBox before its timer runs.
            MSG quit{}; while (PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
            active=this;
            timer=SetTimer(nullptr,0,15,[](HWND,UINT,UINT_PTR,DWORD) {
                if (!active || active->answers.empty()) return;
                EnumThreadWindows(GetCurrentThreadId(),[](HWND dialog,LPARAM data)->BOOL {
                    auto& self=*reinterpret_cast<DialogAnswers*>(data);
                    wchar_t type[32]{}; GetClassNameW(dialog,type,32);
                    HWND parent=GetWindow(dialog,GW_OWNER);
                    while (parent && parent!=self.owner) parent=GetWindow(parent,GW_OWNER);
                    const int response=self.answers.front();
                    if (parent && std::wstring(type)==L"#32770" && GetDlgItem(dialog,response))
                    {
                        self.answers.pop_front(); ++self.handled;
                        if (GetDlgItem(dialog,ProjectDialog::NameField))
                        {
                            SetDlgItemTextW(dialog,ProjectDialog::NameField,L"Window Project");
                            SetDlgItemTextW(dialog,ProjectDialog::LocationField,self.location.c_str());
                            if (self.capture) { UpdateWindow(dialog); CaptureWindow(dialog,L"new-project-qa.bmp"); }
                        }
                        SendMessageW(dialog,WM_COMMAND,response,0); return FALSE;
                    }
                    return TRUE;
                },reinterpret_cast<LPARAM>(active));
            });
            Check(timer!=0,"Cannot automate modal test");
        }
        ~DialogAnswers() { KillTimer(nullptr,timer); active=nullptr; }
    };
}

void ProjectTests(bool capture)
{
    namespace p=zengine::projects;
    Directory test;
    for (const auto* name:{L"",L"..",L"../escape",L"CON",L"nul.txt",L"LPT1",L"bad.",L"bad ",L"bad:name"}) Reject([&] { p::Create(test.path,name); });
    auto project=p::Create(test.path,L"Codec Game");
    Check(std::filesystem::is_directory(p::Assets(project)),"Project did not create Assets");
    Reject([&] { p::Create(test.path,L"Codec Game"); });
    const auto a=zengine::scenes::Create(p::Assets(project));
    const auto b=zengine::scenes::Create(p::Assets(project));
    p::TrackScene(project,a); p::TrackScene(project,b); p::TrackScene(project,a); p::Save(project);
    Check(p::Open(project.file).config.scenes.size()==2 && project.config.lastScene=="Assets/NewScene.zscene","Scene list/last scene roundtrip failed");
    Reject([&] { p::TrackScene(project,test.path/L"outside.zscene"); });
    for (const auto& ref:{"../outside.zscene","Assets/../outside.zscene","Assets/C:/bad.zscene","Assets/script.zsh"})
        Reject([&] { p::ScenePath(project,ref); });
    Reject([&] { p::Decode("ZENGINE_PROJECT 100\n"); });
    auto invalid=project.config; invalid.scenes.push_back(invalid.scenes.front()); Reject([&] { p::Encode(invalid); });
    invalid=project.config; invalid.lastScene="Assets/missing.zscene"; Reject([&] { p::Encode(invalid); });
    Put(project.file,project.source+"\n"); p::TrackScene(project,b); Reject([&] { p::Save(project); });
    project=p::Open(project.file);
    const auto moved=test.path/L"Moved Game";
    std::filesystem::rename(project.file.parent_path(),moved);
    project=p::Open(moved/project.file.filename());
    Check(std::filesystem::is_regular_file(p::ScenePath(project,project.config.lastScene)),"Relative scenes did not survive project relocation");
    const auto session=test.path/L"settings"/L"editor.state";
    Check(!p::ReadRecent(session),"Missing session should be empty");
    p::WriteRecent(session,project.file); Check(p::ReadRecent(session)==project.file,"Recent project roundtrip failed");
    Put(session,"broken settings"); Reject([&] { p::ReadRecent(session); });
    p::WriteRecent(session,project.file);
    Put(session,std::string("\0broken",7)); Reject([&] { p::ReadRecent(session); });
    p::WriteRecent(session,project.file); Check(p::ReadRecent(session)==project.file,"Binary-corrupt preferences not repaired");

    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    std::filesystem::path first,second,firstScene,secondScene,script;
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE); editor.InitializeRenderer();
        Check(!editor.CurrentProject() && !editor.HasOpenScene() && !editor.Play(),"Editor needs a project and scene before Play");
        Reject([&] { editor.CreateScriptAsset(); }); Reject([&] { editor.NewScene(); });
        Check(editor.CreateProject(L"First Game",test.path),"Cannot create editor project"); first=editor.CurrentProject()->file;
        Check(!editor.HasOpenScene() && editor.NewScene(),"New project/scene state wrong"); firstScene=editor.ScenePath();
        auto& object=editor.CreateEmptyGameObject(); object.SetName("Persistent object"); object.GetTransform().SetPosition({3,4,5});
        script=editor.CreateScriptAsset(); Check(editor.AttachScript(object.Id(),script),"Cannot attach owned script");
        Check(editor.SaveScene(),"Cannot save first scene");
        Check(editor.NewScene(),"Cannot create second scene"); secondScene=editor.ScenePath();
        Check(editor.CurrentProject()->config.scenes.size()==2,"Project did not register scenes");
        Check(editor.OpenScene(firstScene),"Cannot open first scene");
        Check(editor.CreateProject(L"Second Game",test.path),"Cannot switch to new project"); second=editor.CurrentProject()->file;
        Check(editor.GameObjects().Size()==0 && !editor.HasOpenScene(),"Objects crossed project boundary");
        Reject([&] { editor.OpenScene(firstScene); }); Reject([&] { editor.OpenScript(script); });
        Check(editor.NewScene(),"Second project scene creation failed");
        const auto secondScript=editor.CreateScriptAsset();
        Check(secondScript.parent_path()==editor.AssetsDirectory() && secondScript!=script,"Script not owned by active project");
        editor.CreateEmptyGameObject();
        { DialogAnswers answers(window,{IDCANCEL}); Check(!editor.OpenProject(first) && answers.handled==1 && editor.CurrentProject()->file==second,"Cancel discarded project"); }
        { DialogAnswers answers(window,{IDYES}); Check(editor.OpenProject(first) && answers.handled==1,"Save-and-switch project failed"); }
        Check(editor.HasOpenScene() && editor.ScenePath()==firstScene && editor.GameObjects().Size()==1 && editor.SelectedGameObject()->Name()=="Persistent object","Project did not restore last scene");
        Check(editor.SelectedGameObject()->GetTransform().Position().x==3,"Project lost Inspector values");
        Check(editor.Play(),"Restored project cannot Play"); Reject([&] { editor.OpenProject(second); }); editor.Stop();
        editor.OpenScript(script);
        HWND scriptWindow=nullptr;
        EnumThreadWindows(GetCurrentThreadId(),[](HWND candidate,LPARAM data)->BOOL {
            if (GetDlgItem(candidate,ScriptEditor::SourceControl)) { *reinterpret_cast<HWND*>(data)=candidate; return FALSE; } return TRUE;
        },reinterpret_cast<LPARAM>(&scriptWindow));
        Check(scriptWindow && GetWindow(scriptWindow,GW_OWNER)==window,"Script editor missing");
        SetWindowTextW(GetDlgItem(scriptWindow,ScriptEditor::SourceControl),L"// unsaved edit");
        { DialogAnswers answers(window,{IDCANCEL}); Check(!editor.OpenProject(second) && answers.handled==1 && IsWindow(scriptWindow),"Unsaved script cancel failed"); }
        { DialogAnswers answers(window,{IDNO}); Check(editor.OpenProject(second) && answers.handled==1 && !IsWindow(scriptWindow),"Old script window survived project switch"); }
        Check(editor.GameObjects().Size()==1,"Save-and-switch lost second scene object");
        Check(editor.OpenProject(first),"Reopen first project failed");
        std::vector<std::string> warnings;
        const auto model=FbxImporter::Import(std::filesystem::path(TEST_FIXTURE_DIR)/"materials.fbx",editor.AssetsDirectory(),warnings);
        editor.QueueModel(model,editor.SelectedGameObject()->Id());
        Reject([&] { editor.OpenProject(second); });
        for (unsigned i=0;i<200;++i) { editor.Render(); Sleep(5); }
        Check(editor.SaveScene(),"Cannot save after model loading");
        Check(editor.OpenScene(secondScene),"Cannot record another last scene");
        p::WriteRecent(session,first);
    }
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE); editor.InitializeRenderer();
        editor.InitializeStartup(session);
        Check(editor.ScenePath()==secondScene && editor.GameObjects().Size()==0,"Startup did not restore exact last scene");
        Check(editor.OpenScene(firstScene),"Cannot update last scene during session");
        for (unsigned i=0;i<200;++i) { editor.Render(); Sleep(5); }
        Check(p::ReadRecent(session)==first,"Startup preferences not preserved");
        Check(p::Open(first).config.lastScene=="Assets/NewScene.zscene","Last scene did not update");
        if (capture) { editor.Render(); CaptureWindow(window,L"projects-qa.bmp"); }
    }
    CoUninitialize();
    std::cout<<"PASS: project config, names, ownership, relocation, conflicts, scene list, restore, unsaved scripts/scenes, async isolation\n";
}

// Startup recovery scenarios run in fresh processes, just like a real editor launch.
void ProjectStartupTests(const std::string& mode,bool capture)
{
    namespace p=zengine::projects;
    Directory test;
    const auto session=test.path/L"editor.state";
    auto project=p::Create(test.path,L"Recovery Project");
    p::TrackScene(project,p::Assets(project)/L"Missing.zscene"); p::Save(project);
    p::WriteRecent(session,project.file);
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization failed");
    if (mode=="--project-recovery")
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE);
        DialogAnswers answers(window,{IDYES}); editor.InitializeStartup(session);
        Check(answers.handled==1,"Missing scene recovery did not prompt once");
        Check(editor.HasOpenScene(),"Missing scene recovery did not open a scene");
        Check(editor.GameObjects().Size()==0,"Missing scene recovery scene was not empty");
        Check(std::filesystem::exists(editor.ScenePath()),"Missing scene recovery did not create a file");
    }
    else if (mode=="--project-dialog")
    {
        Put(session,"bad recent config");
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE);
        { DialogAnswers answers(window,{IDCANCEL}); editor.InitializeStartup(session); Check(answers.handled==1 && !editor.CurrentProject(),"Corrupt session recovery failed"); }
        { DialogAnswers answers(window,{IDOK,IDYES}); answers.location=test.path; answers.capture=capture;
          SendMessageW(window,WM_COMMAND,EditorShell::NewProjectCommand,0);
          Check(answers.handled==2 && editor.HasOpenScene() && editor.CurrentProject()->config.name=="Window Project","New Project name/location dialog failed"); }
        Check(std::filesystem::is_directory(test.path/L"Window Project"/L"Assets"),"New Project created incorrect folder");
        Check(p::ReadRecent(session)==editor.CurrentProject()->file,"New project did not repair recent settings");
    }
    else
    {
        p::WriteRecent(session,test.path/L"deleted"/L"missing.zproject");
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE);
        DialogAnswers answers(window,{IDCANCEL}); editor.InitializeStartup(session);
        Check(answers.handled==1 && !editor.CurrentProject(),"Missing project recovery failed");
    }
    CoUninitialize();
    std::cout<<"PASS: "<<mode<<'\n';
}
