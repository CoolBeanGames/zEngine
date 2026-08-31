#include "SceneAssets.h"
#include "core/MeshRenderer.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace zengine;
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action) { bool failed=false; try { action(); } catch (const std::exception&) { failed=true; } Check(failed,"Invalid scene operation accepted"); }
int main()
{
    const auto root=std::filesystem::temp_directory_path()/(L"zEngine-scene-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try
    {
        Check(std::filesystem::create_directory(root),"Reserve test directory failed");
        ObjectStore objects; ScriptHost host;
        auto& object=objects.Restore(42,"Player \"one\""); object.SetTags({"player","hero"});
        object.GetTransform().SetPosition({1.25f,-2.5f,9}); object.GetTransform().SetRotation({-90,180,45}); object.GetTransform().SetScale({2,0,-1});
        auto& mesh=object.AddBehavior<MeshRenderer>(MeshRenderer::CubeAsset); mesh.SetEnabled(false); mesh.SetPriority(-1.25f);
        auto& behavior=object.AddBehavior<ScriptBehavior>("Behaviors/Mover.zsh"); behavior.SetPriority(3.5f); behavior.SetEnabled(false);
        const std::string code=R"(class Mover : gameObject { export int count=1; export float speed=2; export bool active=true; export string title="default"; export Vector3 direction=Vector3(1,2,3); float hidden=7; func update(float dt) { transform.position.x+=speed*dt; } })";
        Check(host.Prepare(behavior,code,"Mover"),"Compile fixture failed");
        host.SetField(behavior,"count","-9223372036854775808"); host.SetField(behavior,"speed","0.12345678901234567");
        host.SetField(behavior,"active","false"); host.SetField(behavior,"title","hello \"world\"\nUTF-8: \xc3\xa9"); host.SetField(behavior,"direction","-3, 4.5, 0");
        auto& empty=objects.Create("Empty"); Check(empty.Id()==43,"Restored ID sequence incorrect");
        auto& missing=empty.AddBehavior<ScriptBehavior>("Missing.zsh");
        host.RestoreValues(missing,{{"stored",std::int64_t{123}},{"character",char32_t{0x1f642}}});
        const auto scene=scenes::Capture(objects,host);const auto encoded=scenes::Encode(scene);
        Check(encoded.find("hidden")==std::string::npos,"Hidden runtime fields were serialized");
        const auto decoded=scenes::Decode(encoded);
        auto copy=scenes::Instantiate(decoded);
        Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==encoded,"Scene round trip changed authored data");
        auto* restored=copy.objects.Find(42); Check(restored && restored->Name()==object.Name() && restored->Tags()==object.Tags(),"Object identity/name/tags lost");
        Check(restored->GetTransform().Scale().y==0 && restored->GetTransform().Scale().z==-1,"Zero/negative scale lost");
        auto* script=restored->GetBehavior<ScriptBehavior>();
        Check(script && !script->Enabled() && script->Priority()==3.5f,"Script flags/priority lost");
        Check(copy.scripts.Prepare(*script,code,"Mover"),"Restored script compile failed");
        Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==encoded,"Typed script values changed on restore");
        Check(!host.Prepare(behavior,"class Mover : gameObject { invalid }","Mover"),"Invalid script fixture accepted");
        Check(scenes::Encode(scenes::Capture(objects,host))==encoded,"A script compile error discarded saved Inspector values");
        Check(copy.objects.Create().Id()==44,"Next object reused a saved ID");
        const auto path=scenes::Create(root); const auto before=scenes::Load(path);
        scenes::Save(root,path,encoded,&before);
        Check(scenes::Load(path)==encoded,"Scene file save failed");
        Reject([&]{scenes::Save(root,path,before,&before);});
        Reject([&]{scenes::Save(root,path,before);});
        Check(scenes::Load(path)==encoded,"Conflict destroyed scene file");
        Check(scenes::Create(root)!=path,"New scene overwrote existing asset");
        Reject([&]{scenes::Resolve(root,root.parent_path()/"outside.zscene");});
        Reject([&]{scenes::Resolve(root,"wrong.zsh");});
        Check(scenes::Decode("ZENGINE_SCENE 1\nobjects 0\nend\n").objects.empty(),"Legacy scene compatibility failed");
        for (const auto& text:{std::string(""),std::string("ZENGINE_SCENE 4\nobjects 0\nend\n"),encoded.substr(0,encoded.size()/2),encoded+"junk",std::string(scenes::MaxSceneBytes+1,'x')})
            Reject([&]{scenes::Decode(text);});
        auto bad=scene; bad.objects.push_back(scene.objects[0]); Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].id=0; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].asset="../outside.zsh"; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].priority=std::numeric_limits<float>::infinity(); Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].variables["invalid"]=script::ObjectRef{}; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors.push_back(bad.objects[0].behaviors[0]); Reject([&]{scenes::Encode(bad);});
        Reject([&]{objects.Restore(42,"Duplicate");});
        Reject([&]{objects.Restore(std::numeric_limits<GameObjectId>::max(),"Overflow");});
        std::filesystem::remove_all(root); // Only this test's freshly reserved directory.
        std::cout<<"PASS: scene round trips, IDs, TRS, names/tags, mesh/script data, typed values, missing scripts, atomic saves, malformed input\n";
        return 0;
    }
    catch (const std::exception& e)
    { std::error_code ignored; std::filesystem::remove_all(root,ignored); std::cerr<<e.what()<<'\n'; return 1; }
}
