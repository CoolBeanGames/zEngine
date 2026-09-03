#include "SceneAssets.h"
#include "core/MeshRenderer.h"
#include "ui/UiControl.h"
#include "audio/AudioSource.h"
#include "audio/AudioEffect.h"
#include "physics/PhysicsBehavior.h"
#include <windows.h>
#include <cmath>
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
        mesh.SetMaterial("Materials/Chrome.material"); // ZE-65: scene v10 mesh material reference
        auto& behavior=object.AddBehavior<ScriptBehavior>("Behaviors/Mover.zsh"); behavior.SetPriority(3.5f); behavior.SetEnabled(false);
        const std::string code=R"(class Mover : gameObject { export int count=1; export float speed=2; export bool active=true; export string title="default"; export Vector3 direction=Vector3(1,2,3); export Vector2 uv=Vector2(0,0); export prefab template; float hidden=7; func update(float dt) { transform.position.x+=speed*dt; } })";
        Check(host.Prepare(behavior,code,"Mover"),"Compile fixture failed");
        host.SetField(behavior,"count","-9223372036854775808"); host.SetField(behavior,"speed","0.12345678901234567");
        host.SetField(behavior,"active","false"); host.SetField(behavior,"title","hello \"world\"\nUTF-8: \xc3\xa9"); host.SetField(behavior,"direction","-3, 4.5, 0");host.SetField(behavior,"uv","0.5, 0.25");host.SetField(behavior,"template","Prefabs/Crate.zprefab");
        { bool found=false; for(const auto& f:host.Fields(behavior)) if(f.name=="uv") found = f.type=="Vector2" && f.value=="0.5, 0.25"; Check(found,"Vector2 script field not surfaced/parsed"); }
        auto& empty=objects.Create("Empty"); Check(empty.Id()==43,"Restored ID sequence incorrect");
        auto& missing=empty.AddBehavior<ScriptBehavior>("Missing.zsh");
        host.RestoreValues(missing,{{"stored",std::int64_t{123}},{"character",char32_t{0x1f642}}});
        const auto scene=scenes::Capture(objects,host);const auto encoded=scenes::Encode(scene);
        Check(encoded.find("prefab \"Prefabs/Crate.zprefab\"")!=std::string::npos,"Prefab script reference was not serialized");
        Check(encoded.find("hidden")==std::string::npos,"Hidden runtime fields were serialized");
        const auto decoded=scenes::Decode(encoded);
        auto copy=scenes::Instantiate(decoded);
        Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==encoded,"Scene round trip changed authored data");
        auto* restored=copy.objects.Find(42); Check(restored && restored->Name()==object.Name() && restored->Tags()==object.Tags(),"Object identity/name/tags lost");
        Check(encoded.find("mesh_material \"Materials/Chrome.material\"")!=std::string::npos && restored->GetBehavior<MeshRenderer>()->Material()=="Materials/Chrome.material","Mesh material reference lost across scene round trip");
        Check(As3D(restored)->GetTransform().Scale().y==0 && As3D(restored)->GetTransform().Scale().z==-1,"Zero/negative scale lost");
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
        for (const auto& text:{std::string(""),std::string("ZENGINE_SCENE 13\nobjects 0\nend\n"),encoded.substr(0,encoded.size()/2),encoded+"junk",std::string(scenes::MaxSceneBytes+1,'x')})
            Reject([&]{scenes::Decode(text);});
        auto bad=scene; bad.objects.push_back(scene.objects[0]); Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].id=0; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].asset="../outside.zsh"; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].priority=std::numeric_limits<float>::infinity(); Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors[1].variables["invalid"]=script::ObjectRef{}; Reject([&]{scenes::Encode(bad);});
        bad=scene; bad.objects[0].behaviors.push_back(bad.objects[0].behaviors[0]); Reject([&]{scenes::Encode(bad);});
        ObjectStore linked; ScriptHost linkedHost; linkedHost.SetObjectStore(&linked);
        auto& linkedOwner=linked.Create("Owner"); auto& linkedTarget=linked.Create("Target"); linkedTarget.AddBehavior<physics::RigidBody>();
        auto& linkedScript=linkedOwner.AddBehavior<ScriptBehavior>("References.zsh");
        const std::string linkedCode="class References : gameObject { export RigidBody body; }";
        Check(linkedHost.Prepare(linkedScript,linkedCode,"References"),"Scene reference fixture compile");
        linkedHost.SetObjectReference(linkedScript,"body",linkedTarget.Id());
        const auto linkedEncoded=scenes::Encode(scenes::Capture(linked,linkedHost));
        Check(linkedEncoded.find("references 1")!=std::string::npos && linkedEncoded.find("reference \"body\" 2")!=std::string::npos,"Scene object reference was not serialized");
        const auto linkedCopy=scenes::Decode(linkedEncoded); Check(linkedCopy.objects[0].behaviors[0].objectReferences.at("body")==linkedTarget.Id(),"Scene object reference was not decoded");
        auto linkedInstance=scenes::Instantiate(linkedCopy); linkedInstance.scripts.SetObjectStore(&linkedInstance.objects);
        auto* linkedRestored=linkedInstance.objects.Find(linkedOwner.Id())->GetBehavior<ScriptBehavior>();
        Check(linkedRestored && linkedInstance.scripts.Prepare(*linkedRestored,linkedCode,"References"),"Scene object reference was not restored");
        Check(linkedInstance.scripts.Fields(*linkedRestored).front().value=="Target (RigidBody)","Restored object reference target mismatch");
        // ZE-33: exported `array` fields — scalar + object-reference elements survive Encode/Decode/Instantiate.
        {
            ObjectStore bagStore; ScriptHost bagHost; bagHost.SetObjectStore(&bagStore);
            auto& bagOwner=bagStore.Create("Bag Owner"); auto& bagRig=bagStore.Create("Bag Rig"); bagRig.AddBehavior<physics::RigidBody>();
            auto& bagScript=bagOwner.AddBehavior<ScriptBehavior>("Bag.zsh");
            const std::string bagCode="class Bag : gameObject { export array nums; export array refs; }";
            Check(bagHost.Prepare(bagScript,bagCode,"Bag"),"Array scene fixture compile");
            bagHost.AddArrayElement(bagScript,"nums","int"); bagHost.AddArrayElement(bagScript,"nums","float");
            bagHost.SetArrayElement(bagScript,"nums",0,"7"); bagHost.SetArrayElement(bagScript,"nums",1,"2.5");
            bagHost.SetArrayElementReference(bagScript,"refs",0,bagRig.Id());
            const auto bagEncoded=scenes::Encode(scenes::Capture(bagStore,bagHost));
            Check(bagEncoded.find("arrays 2")!=std::string::npos && bagEncoded.find("element ref ")!=std::string::npos,"Array field was not serialized");
            auto bagCopy=scenes::Instantiate(scenes::Decode(bagEncoded)); bagCopy.scripts.SetObjectStore(&bagCopy.objects);
            auto* bagRestored=bagCopy.objects.Find(bagOwner.Id())->GetBehavior<ScriptBehavior>();
            Check(bagRestored && bagCopy.scripts.Prepare(*bagRestored,bagCode,"Bag"),"Array script restore failed");
            Check(scenes::Encode(scenes::Capture(bagCopy.objects,bagCopy.scripts))==bagEncoded,"Array round trip changed authored data");
            const auto bagArrays=bagCopy.scripts.AuthoredArrays(*bagRestored);
            Check(bagArrays.at("nums").size()==2 && bagArrays.at("refs").at(0).reference==bagRig.Id() && bagArrays.at("refs").at(0).referenceType=="RigidBody","Array elements not restored");
        }
        // ZE-59: spawning a prefab remaps its internal object references onto the fresh live IDs,
        // so a bullet's "export rigidbody rb" self-reference resolves instead of faulting the spawner.
        {
            scenes::Document prefab;
            scenes::ObjectData bulletData; bulletData.id=10; bulletData.name="BulletRoot";
            scenes::BehaviorData bodyData; bodyData.kind=scenes::BehaviorData::Kind::RigidBody;
            bodyData.mass=1; bodyData.friction=0.5f; bodyData.bounciness=0;
            bulletData.behaviors.push_back(bodyData);
            scenes::BehaviorData scriptData; scriptData.kind=scenes::BehaviorData::Kind::Script; scriptData.asset="Bullet.zsh";
            scriptData.objectReferences["rb"]=10; // the prefab's own object id
            bulletData.behaviors.push_back(scriptData);
            prefab.objects.push_back(bulletData);
            ObjectStore live; ScriptHost liveHost; liveHost.SetObjectStore(&live);
            (void)live.Create("Player");
            const auto spawnedRoot=scenes::Append(prefab,live,liveHost);
            Check(spawnedRoot!=0 && spawnedRoot!=10 && live.Find(spawnedRoot)!=nullptr,"Append did not create the prefab root");
            auto* spawnedScript=live.Find(spawnedRoot)->GetBehavior<ScriptBehavior>();
            Check(spawnedScript!=nullptr,"Append lost the spawned script");
            const auto spawnedRefs=liveHost.AuthoredReferences(*spawnedScript);
            Check(spawnedRefs.count("rb")==1 && spawnedRefs.at("rb")==spawnedRoot && spawnedRefs.at("rb")!=10,
                  "Prefab self-reference was not remapped to the live object ID");
        }
        // ZE-73: scene v8 serializes GameObject2D (transform2d) alongside 3D objects in one store.
        {
            ObjectStore flatStore; ScriptHost flatHost;
            auto& sprite=flatStore.Restore2D(7,"Sprite"); sprite.SetTags({"hud"});
            sprite.GetTransform().SetPosition({64,-32}); sprite.GetTransform().SetRotation(45); sprite.GetTransform().SetScale({2,3});
            auto& solid=flatStore.Create("Solid"); solid.GetTransform().SetPosition({0,1,0});
            const auto flatEncoded=scenes::Encode(scenes::Capture(flatStore,flatHost));
            Check(flatEncoded.find("transform2d ")!=std::string::npos,"GameObject2D transform2d was not serialized");
            auto flatCopy=scenes::Instantiate(scenes::Decode(flatEncoded));
            Check(scenes::Encode(scenes::Capture(flatCopy.objects,flatCopy.scripts))==flatEncoded,"2D scene round trip changed authored data");
            auto* flatRestored=flatCopy.objects.Find(7);
            Check(flatRestored && flatRestored->Is2D() && flatRestored->HasTag("hud"),"GameObject2D identity lost");
            const auto& t2d=As2D(flatRestored)->GetTransform();
            Check(t2d.Position()==Vec2{64,-32} && t2d.Rotation()==45 && t2d.Scale()==Vec2{2,3},"Transform2D values lost on restore");
            Check(As3D(flatCopy.objects.Find(solid.Id()))->GetTransform().Position().y==1,"3D object corrupted by 2D sibling");
        }
        // ZE-61: scene v9 serializes ui:: control behaviors on 2D objects, with a
        // property bag per control, surviving Encode / Decode / Instantiate.
        {
            ObjectStore uiStore; ScriptHost uiHost;
            auto& panel=uiStore.Restore2D(3,"Panel");
            auto& panelUi=ui::AddUiControl(panel,"panel");
            panelUi.SetAnchor(ui::Anchor::Fill);
            dynamic_cast<ui::PanelContainer&>(panelUi).SetTint({0.1f,0.1f,0.12f,0.9f});
            dynamic_cast<ui::PanelContainer&>(panelUi).SetSlice({6,6,6,6});
            auto& bar=uiStore.Create2D("Bar"); bar.SetParent(panel.Id());
            auto& barUi=ui::AddUiControl(bar,"progressBar");
            barUi.SetAnchor(ui::Anchor::TopLeft); barUi.SetSize({240,18}); barUi.SetOrder(2);
            dynamic_cast<ui::ProgressBar&>(barUi).SetValue(0.4f);
            auto& label=uiStore.Create2D("Label"); label.SetParent(panel.Id());
            auto& labelUi=ui::AddUiControl(label,"text");
            dynamic_cast<ui::Text&>(labelUi).SetValue("Score: \"9000\"");
            dynamic_cast<ui::Text&>(labelUi).SetAlignH(ui::HAlign::Center);
            dynamic_cast<ui::Text&>(labelUi).SetAlignV(ui::VAlign::Bottom);
            dynamic_cast<ui::Text&>(labelUi).SetWrap(true);
            labelUi.SetClickable(true);
            labelUi.SetEnabled(false); // ZE-97: enabled + text align/wrap round-trip through the scene

            const auto uiEncoded=scenes::Encode(scenes::Capture(uiStore,uiHost));
            Check(uiEncoded.find("ui \"panel\"")!=std::string::npos && uiEncoded.find("ui_prop \"value\" \"0.4\"")!=std::string::npos,"UI behavior was not serialized");
            auto uiCopy=scenes::Instantiate(scenes::Decode(uiEncoded));
            Check(scenes::Encode(scenes::Capture(uiCopy.objects,uiCopy.scripts))==uiEncoded,"UI scene round trip changed authored data");
            auto* restoredPanel=uiCopy.objects.Find(3);
            auto* rp=restoredPanel?restoredPanel->GetBehavior<ui::PanelContainer>():nullptr;
            Check(rp && rp->GetAnchor()==ui::Anchor::Fill && rp->Tint().w==0.9f && rp->Slice().left==6,"Panel props lost on restore");
            auto* rb=uiCopy.objects.Find(uiCopy.objects.At(1).Id())->GetBehavior<ui::ProgressBar>();
            Check(rb && rb->Value()==0.4f && rb->Order()==2 && rb->Size().x==240,"ProgressBar props lost on restore");
            auto* rt=uiCopy.objects.Find(uiCopy.objects.At(2).Id())->GetBehavior<ui::Text>();
            Check(rt && rt->Value()=="Score: \"9000\"" && rt->Clickable(),"Text value with quotes lost on restore");
        }
        // ZE-67: an AudioSource round-trips through scene v11.
        {
            ObjectStore s; ScriptHost h;
            auto& obj=s.Create("Speaker");
            auto& src=obj.AddBehavior<audio::AudioSource>();
            src.SetClip("sfx/hum.ogg"); src.SetSpatial(true); src.SetAutoplay(false); src.SetLoop(true);
            src.SetVolume(0.6f); src.SetPitch(1.5f); src.SetAttenuationModel(audio::Attenuation::Inverse);
            src.SetMinDistance(2); src.SetMaxDistance(40);
            const auto enc=scenes::Encode(scenes::Capture(s,h));
            Check(enc.find("audio \"sfx/hum.ogg\"")!=std::string::npos,"AudioSource was not serialized");
            auto copy=scenes::Instantiate(scenes::Decode(enc));
            Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==enc,"Audio scene round trip changed authored data");
            auto* rs=copy.objects.Find(copy.objects.At(0).Id())->GetBehavior<audio::AudioSource>();
            Check(rs && rs->Clip()=="sfx/hum.ogg" && rs->Loop() && !rs->Autoplay() && rs->Spatial()
                  && std::abs(rs->Volume()-0.6f)<0.001f && std::abs(rs->Pitch()-1.5f)<0.001f
                  && rs->AttenuationModel()==audio::Attenuation::Inverse && rs->MaxDistance()==40.0f,
                  "AudioSource props lost on restore");
        }
        // ZE-109: an AudioEffect on an Area round-trips through scene v12.
        {
            ObjectStore s; ScriptHost h;
            auto& zone=s.Create("Cave");
            zone.AddBehavior<physics::Collider>().SetSize({8,4,8});
            zone.AddBehavior<physics::Area>();
            auto& fx=zone.AddBehavior<audio::AudioEffect>();
            fx.SetDecay(3.2f); fx.SetWetMix(0.75f); fx.SetBlendDistance(1.5f);
            const auto enc=scenes::Encode(scenes::Capture(s,h));
            Check(enc.find("audio_effect ")!=std::string::npos,"AudioEffect was not serialized");
            auto copy=scenes::Instantiate(scenes::Decode(enc));
            Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==enc,"AudioEffect scene round trip changed authored data");
            auto* rfx=copy.objects.Find(copy.objects.At(0).Id())->GetBehavior<audio::AudioEffect>();
            Check(rfx && std::abs(rfx->Decay()-3.2f)<0.001f && std::abs(rfx->WetMix()-0.75f)<0.001f && std::abs(rfx->BlendDistance()-1.5f)<0.001f,
                  "AudioEffect props lost on restore");
        }
        // ZE-66: the new scroll / button / video / html controls round-trip too.
        {
            ObjectStore s; ScriptHost h;
            auto& scrollObj=s.Restore2D(11,"List");
            auto& scroll=dynamic_cast<ui::ScrollContainer&>(ui::AddUiControl(scrollObj,"scroll"));
            scroll.SetScrollY(24); scroll.SetHorizontal(false); scroll.SetFillCross(false);
            auto& btnObj=s.Create2D("Ok"); btnObj.SetParent(scrollObj.Id());
            auto& btn=dynamic_cast<ui::Button&>(ui::AddUiControl(btnObj,"button"));
            btn.SetText("Play \"now\""); btn.SetDisabled(true); btn.SetPressedColor({1,0,0,1});
            auto& vidObj=s.Create2D("Clip"); vidObj.SetParent(scrollObj.Id());
            auto& vid=dynamic_cast<ui::VideoTexture&>(ui::AddUiControl(vidObj,"video"));
            vid.SetVideo("media/intro.zvid"); vid.SetLoop(false); vid.SetSpeed(1.5f);
            auto& docObj=s.Create2D("Doc"); docObj.SetParent(scrollObj.Id());
            auto& doc=dynamic_cast<ui::UiHtml&>(ui::AddUiControl(docObj,"html"));
            doc.SetHtml("<h2>Hi</h2><p>there</p>");

            const auto enc=scenes::Encode(scenes::Capture(s,h));
            auto copy=scenes::Instantiate(scenes::Decode(enc));
            Check(scenes::Encode(scenes::Capture(copy.objects,copy.scripts))==enc,"ZE-66 UI scene round trip changed authored data");
            auto* rs=copy.objects.Find(11)->GetBehavior<ui::ScrollContainer>();
            Check(rs && rs->ScrollY()==24 && !rs->FillCross(),"ScrollContainer props lost");
            auto* rbtn=copy.objects.Find(copy.objects.At(1).Id())->GetBehavior<ui::Button>();
            Check(rbtn && rbtn->Text()=="Play \"now\"" && rbtn->Disabled() && !rbtn->Clickable() && rbtn->PressedColor().x==1,"Button props lost");
            auto* rvid=copy.objects.Find(copy.objects.At(2).Id())->GetBehavior<ui::VideoTexture>();
            Check(rvid && rvid->Video()=="media/intro.zvid" && !rvid->Loop() && rvid->Speed()==1.5f,"VideoTexture props lost");
            auto* rdoc=copy.objects.Find(copy.objects.At(3).Id())->GetBehavior<ui::UiHtml>();
            Check(rdoc && rdoc->Html()=="<h2>Hi</h2><p>there</p>" && rdoc->BlockCount()==2,"UiHtml markup lost");
        }
        Reject([&]{objects.Restore(42,"Duplicate");});
        Reject([&]{objects.Restore(std::numeric_limits<GameObjectId>::max(),"Overflow");});
        std::filesystem::remove_all(root); // Only this test's freshly reserved directory.
        std::cout<<"PASS: scene round trips, IDs, TRS, names/tags, mesh/script data, typed values, missing scripts, atomic saves, malformed input\n";
        return 0;
    }
    catch (const std::exception& e)
    { std::error_code ignored; std::filesystem::remove_all(root,ignored); std::cerr<<e.what()<<'\n'; return 1; }
}
