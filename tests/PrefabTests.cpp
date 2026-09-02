#include "EditorShell.h"
#include "PrefabAssets.h"
#include "SceneAssets.h"
#include "InspectorPanel.h"
#include "RenderTransform.h"
#include "core/MeshRenderer.h"
#include <objbase.h>
#include <windowsx.h>
#include <fstream>
#include <iostream>
#include <functional>

namespace
{
    void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
    void Reject(const std::function<void()>& action) { bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; } Check(rejected,"Invalid prefab operation accepted"); }
    struct Directory
    {
        std::filesystem::path path=std::filesystem::temp_directory_path()/(L"zEngine-prefab-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        Directory() { Check(std::filesystem::create_directory(path),"Reserve prefab test directory"); }
        ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); }
    };
    zengine::scenes::ObjectData Object(zengine::GameObjectId id,const char* name) { zengine::scenes::ObjectData o; o.id=id; o.name=name; return o; }
    void Put(const std::filesystem::path& path,const std::string& text) { std::ofstream file(path,std::ios::binary); file<<text; Check(static_cast<bool>(file),"Fixture write failed"); }
}
void PrefabTests()
{
    namespace p=zengine::prefabs; namespace s=zengine::scenes;
    Directory test;
    const auto assets=test.path/L"Assets"; std::filesystem::create_directory(assets);
    s::Document inner; inner.objects.push_back(Object(1,"Inner")); inner.objects[0].transform.SetPosition({2,0,0});
    auto mesh=zengine::scenes::BehaviorData{}; mesh.asset=zengine::MeshRenderer::CubeAsset; inner.objects[0].behaviors.push_back(mesh);
    const auto innerFile=p::Create(assets,inner);
    s::Document outer; outer.objects.push_back(Object(10,"Outer")); outer.objects[0].transform.SetPosition({5,0,0});
    auto nested=Object(20,"Nested"); nested.parent=10; nested.prefab=innerFile.filename().string(); outer.objects.push_back(nested);
    const auto outerFile=p::Create(assets,outer);
    s::Document scene; auto instance=Object(42,"Instance"); instance.prefab=outerFile.filename().string(); scene.objects.push_back(instance);
    instance.id=43; instance.transformOverride=true; instance.transform.SetPosition({11,0,0}); scene.objects.push_back(instance);
    auto expanded=p::ResolveScene(assets,scene);
    Check(expanded.scene.objects.size()==4 && expanded.generated.size()==2,"Nested expansion count/ownership wrong");
    auto runtime=s::Instantiate(expanded.scene);
    const auto& child=runtime.objects.At(1); const auto matrix=TransformMatrix(zengine::As3D(child).GetTransform())*ParentMatrix(runtime.objects,child);
    Check(std::abs(DirectX::XMVectorGetX(matrix.r[3])-7)<.001f,"Nested transform did not follow parent");
    auto before=s::Load(innerFile); inner.objects[0].name="Updated Inner"; p::Save(assets,innerFile,inner,&before);
    expanded=p::ResolveScene(assets,scene);
    Check(expanded.scene.objects[1].name=="Updated Inner" && expanded.scene.objects[3].name=="Updated Inner","Inner prefab changes did not propagate to every nested use");
    Check(expanded.scene.objects[2].transform.Position().x==11,"Instance placement override was lost");
    auto cycle=inner; auto ref=Object(2,"Cycle"); ref.parent=1; ref.prefab=outerFile.filename().string(); cycle.objects.push_back(ref);
    before=s::Load(innerFile); Reject([&] { p::Save(assets,innerFile,cycle,&before); }); Check(s::Load(innerFile)==before,"Cycle failure damaged prefab");
    Reject([&] { p::Resolve(assets,test.path/L"outside.zprefab"); });
    Reject([&] { p::Decode("ZENGINE_PREFAB 999\n"); });
    auto bad=outer; bad.objects[1].parent=20; Reject([&] { p::Encode(bad); });
    bad=outer; bad.objects[1].parent=0; Reject([&] { p::Encode(bad); });
    bad=outer; bad.objects[1].prefab="../outside.zprefab"; Reject([&] { p::Encode(bad); });
    Put(innerFile,before+"\n"); Reject([&] { p::Save(assets,innerFile,inner,&before); }); Put(innerFile,before);
    Check(s::Decode("ZENGINE_SCENE 1\nobjects 0\nend\n").objects.empty(),"Old scenes no longer load");
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM failed");
    {
        EditorShell editor(GetModuleHandleW(nullptr));Check(editor.Create(SW_HIDE,test.path)!=nullptr,"Prefab rename regression editor creation failed");editor.InitializeRenderer();
        Check(editor.SaveScene(assets/L"RenameRegression.zscene"),"Save prefab rename regression baseline");
        const auto first=editor.SelectedGameObject()->Id();auto& unsaved=editor.CreateEmptyGameObject();unsaved.SetName("Unsaved survivor");const auto unsavedId=unsaved.Id();
        const auto created=editor.CreatePrefab(first);editor.RenameAsset(created,L"RenamedPrefab");
        Check(editor.GameObjects().Size()==2 && editor.GameObjects().Find(unsavedId) && editor.GameObjects().Find(unsavedId)->Name()=="Unsaved survivor","Renaming a new prefab discarded the live scene");
        Check(editor.SaveScene(),"Save scene after renaming its new prefab");const auto captured=s::Decode(s::Load(editor.ScenePath()));
        Check(captured.objects.size()==2 && captured.objects[0].prefab.ends_with("RenamedPrefab.zprefab"),"Renaming a new prefab did not preserve/update its live scene link");
    }
    std::filesystem::path prefab,secondScene,firstScene,nestedPrefab;
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE,test.path); editor.InitializeRenderer();
        const auto rootId=editor.SelectedGameObject()->Id(); firstScene=assets/L"First.zscene"; Check(editor.SaveScene(firstScene),"Save first scene");
        // Actual scene-row drag into the asset library converts the original object into a linked instance.
        RECT client{}; GetClientRect(window,&client); const POINT from{60,32+30+69+12},to{400,client.bottom-100};
        SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
        SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y));
        Check(editor.GameObjects().Size()==1 && editor.SelectedGameObject()->Id()==rootId,"Prefab conversion changed original identity or duplicated object");
        Check(editor.SaveScene(),"Save linked scene");
        auto saved=s::Decode(s::Load(firstScene)); Check(!saved.objects[0].prefab.empty(),"Dragging object failed to create reference");
        prefab=assets/std::filesystem::path(std::u8string(saved.objects[0].prefab.begin(),saved.objects[0].prefab.end()));
        const auto instance2=editor.InstantiatePrefab(prefab); Check(editor.GameObjects().Size()==2,"Prefab placement failed");
        Check(!editor.AddMeshRenderer(instance2),"Duplicate Mesh Renderer accepted on editable prefab instance");
        const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
        Check(IsWindowEnabled(GetDlgItem(inspector,InspectorPanel::NameField)) && IsWindowEnabled(GetDlgItem(inspector,InspectorPanel::FirstTransformField)),"Prefab instance root is not editable");
        SetDlgItemTextW(inspector,InspectorPanel::NameField,L"Customized Cube");
        SetDlgItemTextW(inspector,InspectorPanel::FirstTransformField,L"9"); Check(editor.SaveScene(),"Save placement override");
        saved=s::Decode(s::Load(firstScene));Check(saved.objects[1].prefabDataMask==1 && saved.objects[1].name=="Customized Cube","Prefab name override was not persisted independently");
        Check(editor.OpenPrefab(prefab) && editor.EditingPrefab()==prefab,"Open prefab failed");
        SetDlgItemTextW(inspector,InspectorPanel::NameField,L"Shared Cube");
        SetDlgItemTextW(inspector,InspectorPanel::FirstTransformField+6,L"3");
        const auto script=editor.CreateScriptAsset();
        Put(script,"class NewBehavior : gameObject { export float speed=3; func update(float delta) { transform.position.x+=speed*delta; } }");
        Check(editor.AttachScript(rootId,script),"Attach prefab script"); Check(editor.SavePrefab(),"Save prefab data");
        Check(editor.ClosePrefab() && editor.GameObjects().Size()==2,"Return to scene failed");
        Check(editor.GameObjects().At(0).Name()=="Shared Cube" && editor.GameObjects().At(1).Name()=="Customized Cube","Prefab source overwrote a per-instance name override");
        Check(zengine::As3D(editor.GameObjects().Find(instance2))->GetTransform().Position().x==9,"Prefab update overwrote instance placement");
        Check(zengine::As3D(editor.GameObjects().At(0)).GetTransform().Scale().x==3 && zengine::As3D(editor.GameObjects().Find(instance2))->GetTransform().Scale().x==3,"Moved instance did not inherit prefab scale");
        Check(editor.Play(),"Linked prefab scripts cannot play"); editor.Step(); editor.Stop();
        Check(editor.SaveScene(),"Save updated scene"); Check(editor.NewScene(),"Create another scene"); secondScene=editor.ScenePath();
        auto& plain=editor.CreateEmptyGameObject(); plain.SetName("Outer prefab"); const auto outerId=plain.Id(); nestedPrefab=editor.CreatePrefab(outerId);
        Check(editor.SaveScene(),"Save second scene"); Check(editor.OpenPrefab(nestedPrefab),"Open outer prefab");
        editor.InstantiatePrefab(prefab); Check(editor.GameObjects().Size()==2,"Nested prefab placement failed");
        Reject([&] { editor.InstantiatePrefab(nestedPrefab); });
        Check(editor.SavePrefab() && editor.ClosePrefab(),"Nested prefab save/close failed");
        Check(editor.GameObjects().Size()==2 && editor.GameObjects().At(1).Parent()==outerId,"Nested runtime hierarchy missing");
        Check(editor.OpenPrefab(prefab),"Reopen shared prefab"); SetDlgItemTextW(inspector,InspectorPanel::NameField,L"Propagated Cube");
        Check(editor.SavePrefab() && editor.ClosePrefab(),"Save nested dependency failed");
        Check(editor.GameObjects().At(1).Name()=="Propagated Cube","Shared change did not propagate inside outer prefab");
        Check(editor.SaveScene() && editor.OpenScene(firstScene),"Reopen other scene");
        Check(editor.GameObjects().At(0).Name()=="Propagated Cube" && editor.GameObjects().At(1).Name()=="Customized Cube","Closed scene lost prefab propagation or instance override");
        Check(editor.SaveScene(),"Save final scene");
        const auto count=editor.GameObjects().Size(); auto source=s::Load(prefab); Put(prefab,"broken prefab");
        Reject([&] { editor.OpenScene(firstScene); }); Check(editor.GameObjects().Size()==count,"Broken prefab destroyed open scene"); Put(prefab,source);
    }
    {
        EditorShell editor(GetModuleHandleW(nullptr)); Check(editor.Create(SW_HIDE,test.path)!=nullptr,"Restart editor creation failed"); editor.InitializeRenderer();
        Check(editor.OpenScene(secondScene) && editor.GameObjects().Size()==2 && editor.GameObjects().At(1).Name()=="Propagated Cube","Nested prefab failed after editor restart");
    }
    CoUninitialize(); std::cout<<"PASS: prefab conversion, nesting, graph safety, local transforms, scene references, source propagation, independent instance overrides, scripts, Inspector, restart\n";
}
