#include "ScriptHost.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace zengine;
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
std::string Value(ScriptHost& host, ScriptBehavior& b,const std::string& name)
{ for (const auto& f:host.Fields(b)) if (f.name==name) return f.value; throw std::runtime_error("Missing field"); }
template<class F> void Rejected(F f) { bool failed=false; try { f(); } catch (const std::exception&) { failed=true; } Check(failed,"Invalid edit accepted"); }
int main()
{
    try
    {
        ObjectStore objects; ScriptHost host;
        auto& owner=objects.Create("Moving cube"); owner.GetTransform().SetPosition({10,0,0});
        auto& b=owner.AddBehavior<ScriptBehavior>("Mover.zsh");
        const std::string source=R"(class Mover : gameObject {
            label("Movement"); export float speed=2; export Vector3 direction=Vector3(1,0,0);
            export int starts=0; export int updates=0; export int draws=0;
            export bool moving=true; export string title="hello"; export GameObject target;
            float hidden=0;
            func start() { starts+=1; transform.position.y+=speed; }
            func update(float delta) { updates+=1; hidden+=delta; if(moving) { transform.position+=direction*speed*delta; } }
            func draw() { draws+=1; transform.rotation.z+=1; }
        })";
        Check(host.Prepare(b,source,"Mover"),"Prepare failed");
        Check(!b.HasInstance() && owner.GetTransform().Position().y==0,"Authoring executed Start");
        const auto fields=host.Fields(b);
        Check(fields.front().label=="Movement" && std::none_of(fields.begin(),fields.end(),[](const auto& f) { return f.name=="hidden"; }),"Inspector labels/hidden fields broken");
        host.SetField(b,"speed","3.5"); host.SetField(b,"direction","2, 0, 0");
        host.SetField(b,"title","edited"); host.SetField(b,"moving","false"); host.SetField(b,"moving","true");
        for (const auto& text:{"nan","inf","1x",""}) Rejected([&]{host.SetField(b,"speed",text);});
        Rejected([&]{host.SetField(b,"updates","1.2");});
        Rejected([&]{host.SetField(b,"direction","1, 2");});
        Rejected([&]{host.SetField(b,"moving","yes");});
        Rejected([&]{host.SetField(b,"hidden","1");});
        Rejected([&]{host.SetField(b,"target","anything");});
        Check(Value(host,b,"speed")=="3.5","Invalid input damaged authoring value");
        Check(host.Play(objects),"Play failed");
        Check(owner.GetTransform().Position().y==3.5f && Value(host,b,"starts")=="1","Overrides must precede Start");
        host.Tick(objects,0.5f);
        Check(owner.GetTransform().Position().x==13.5f && Value(host,b,"updates")=="1","Code did not move native transform");
        host.Draw(objects,[](GameObjectId) { return false; }); Check(Value(host,b,"draws")=="0","Non-rendered object received Draw");
        host.Draw(objects,[](GameObjectId) { return true; }); Check(Value(host,b,"draws")=="1" && owner.GetTransform().Rotation().z==1,"Draw bridge failed");
        host.SetField(b,"speed","8"); host.Tick(objects,0.5f);
        Check(owner.GetTransform().Position().x==21.5f && Value(host,b,"starts")=="1","Live variable edit or Start once failed");
        host.Stop(objects);
        Check(owner.GetTransform().Position().x==10 && owner.GetTransform().Position().y==0 && owner.GetTransform().Rotation().z==0,"Stop did not restore transforms");
        Check(Value(host,b,"speed")=="3.5" && Value(host,b,"updates")=="0" && !b.HasInstance(),"Runtime state leaked into authoring");
        auto& second=objects.Create().AddBehavior<ScriptBehavior>("Mover.zsh");
        Check(host.Prepare(second,source,"Mover"),"Second instance failed");
        Check(host.Play(objects),"Replay failed"); host.Tick(objects,0.5f);
        Check(As3D(second.Owner()).GetTransform().Position().x==1 && owner.GetTransform().Position().x==13.5f,"Instances share variables or transforms");
        host.Stop(objects);
        Check(host.Prepare(b,source+"\n// saved edit","Mover") && Value(host,b,"speed")=="3.5","Save discarded authoring overrides");
        Check(!host.Prepare(b,"class Mover { export Missing x; }","Mover") && !host.Play(objects),"Invalid compile allowed partial Play");
        Check(owner.GetTransform().Position().y==0 && !second.HasInstance(),"Failed Play partially started scene");
        Check(!host.Prepare(b,"class Other : gameObject {}","Mover"),"Filename mismatch not diagnosed");
        Check(!host.Prepare(b,"class Mover {}","Mover"),"Non-GameObject class attached");
        Check(host.Prepare(b,R"(class Mover : gameObject { func update(float dt) { transform.position.x=99; int zero=0; int fail=1/zero; } })","Mover"),"Failure fixture compile failed");
        Check(host.Play(objects),"Failure fixture Play failed"); host.Tick(objects,0.5f);
        Check(b.Faulted() && !host.Error(b).empty() && owner.GetTransform().Position().x==10,"Failure isolation/atomic transform sync failed");
        Check(As3D(second.Owner()).GetTransform().Position().x==1,"Failed script stopped other scripts");
        host.Stop(objects);
        Check(host.Prepare(b,"class Mover : gameObject { func update(float dt) { transform.position.x=9999999999999999999999999999999999999999.0; } }","Mover"),"Overflow fixture compile");
        Check(host.Play(objects),"Overflow fixture Play"); host.Tick(objects,0.1f);
        Check(b.Faulted() && owner.GetTransform().Position().x==10,"Out-of-range native transform accepted"); host.Stop(objects);
        // Native priority ordering: both scripts act on exactly the same owner transform.
        ObjectStore ordered; ScriptHost orderedHost;
        auto& object=ordered.Create(); auto& add=object.AddBehavior<ScriptBehavior>("Add.zsh"); auto& multiply=object.AddBehavior<ScriptBehavior>("Multiply.zsh");
        add.SetPriority(1.5f); multiply.SetPriority(-1);
        Check(orderedHost.Prepare(add,"class Add : gameObject { func start() { transform.position.x+=1; } func update(float dt) { transform.position.x+=1; } }","Add"),"Add compile");
        Check(orderedHost.Prepare(multiply,"class Multiply : gameObject { func start() { transform.position.x*=2; } func update(float dt) { transform.position.x*=2; } }","Multiply"),"Multiply compile");
        Check(orderedHost.Play(ordered) && object.GetTransform().Position().x==2,"Start priority order failed");
        orderedHost.Tick(ordered,0.1f); Check(object.GetTransform().Position().x==6,"Scripts did not synchronize shared owner in priority order");
        orderedHost.Stop(ordered);
        auto empty=script::Compiler::Compile("class Base : gameObject { func start() { int a=1; } } class Empty : Base { func start() {} func update(float dt) {} }");
        Check(empty && empty.program->HasCode("Base","start") && !empty.program->HasCode("Empty","start") && !empty.program->HasCode("Empty","update"),"Empty override must suppress inherited hooks");
        ObjectStore signals; ScriptHost signalHost; auto& signalObject=signals.Create();
        auto& listener=signalObject.AddBehavior<ScriptBehavior>("Listener.zsh");
        Check(signalHost.Prepare(listener,R"(class Listener : gameObject {
            export int moves; export int rotates; export int scales; export int custom;
            signal test;
            func start() { transform.was_moved.connect(moved); transform.was_rotated.connect(rotated); transform.was_scaled.connect(scaled); test.connect(receive); test.emit(7); }
            func moved(Vector3 v) { moves += 1; }
            func rotated(Vector3 v) { rotates += 1; }
            func scaled(Vector3 v) { scales += 1; }
            func receive(int n) { custom += n; }
        })","Listener"),"Signal listener compile");
        Check(signalHost.Play(signals) && Value(signalHost,listener,"custom")=="7","Custom signal on Start");
        signalObject.GetTransform().SetPosition({0.1f,0,0}); signalObject.GetTransform().SetRotation({0,12,0}); signalObject.GetTransform().SetScale({2,2,2});
        signalHost.Tick(signals,0.1f); signalHost.Tick(signals,0.1f);
        Check(Value(signalHost,listener,"moves")=="1" && Value(signalHost,listener,"rotates")=="1" && Value(signalHost,listener,"scales")=="1","Native signals without update body or duplicate float notification");
        signalHost.Stop(signals); Check(signalHost.Play(signals),"Signal restart");
        Check(Value(signalHost,listener,"custom")=="7" && Value(signalHost,listener,"moves")=="0","Connections leaked across Play"); signalHost.Stop(signals);
        ObjectStore hierarchy;ScriptHost hierarchyHost;
        auto& root=hierarchy.Create("Root");auto& child=hierarchy.Create("Child");auto& target=hierarchy.Create("Target");
        child.SetParent(root.Id());root.GetTransform().SetPosition({5,0,0});
        auto& parenting=child.AddBehavior<ScriptBehavior>("Parenting.zsh");
        Check(hierarchyHost.Prepare(parenting,R"(class Parenting : gameObject {
            gameObject saved; export bool missing=false;
            func start(){ saved=parent; parent.transform.position.x+=2; parent=find("Target"); missing=find("Missing")==null; }
            func update(float dt){ parent=null; saved.transform.rotation.y+=10; transform.position.x+=1; }
        })","Parenting"),"Parenting script compile");
        Check(hierarchyHost.Play(hierarchy),"Parenting Play failed");
        Check(!parenting.Faulted() && child.Parent()==target.Id() && root.GetTransform().Position().x==7 && Value(hierarchyHost,parenting,"missing")=="true","Parent lookup, transform proxy, or reparent failed");
        hierarchyHost.Tick(hierarchy,0.1f);
        Check(child.Parent()==0 && root.GetTransform().Rotation().y==10 && child.GetTransform().Position().x==1,"Unparent or persistent object reference failed");
        hierarchyHost.Stop(hierarchy);
        Check(child.Parent()==root.Id() && root.GetTransform().Position().x==5 && root.GetTransform().Rotation().y==0 && child.GetTransform().Position().x==0,"Stop failed to restore complete hierarchy");
        for(const auto& body:{"parent=find(\"Child\");","find(\"Root\").parent=find(\"Child\");","parent=gameObject();"}){
            Check(hierarchyHost.Prepare(parenting,std::string("class Parenting : gameObject {func update(float dt){transform.position.x=99;")+body+"}}","Parenting"),"Invalid parenting fixture compile");
            Check(hierarchyHost.Play(hierarchy),"Invalid parenting fixture Play");hierarchyHost.Tick(hierarchy,0.1f);
            Check(parenting.Faulted() && child.Parent()==root.Id() && root.Parent()==0 && child.GetTransform().Position().x==0,"Invalid parenting must fault without partial native edits");hierarchyHost.Stop(hierarchy);
        }
        auto& duplicate=hierarchy.Create("Target");
        Check(hierarchyHost.Prepare(parenting,"class Parenting : gameObject {func update(float dt){parent=find(\"Target\");}}","Parenting"),"Ambiguous lookup fixture");
        Check(hierarchyHost.Play(hierarchy),"Ambiguous lookup Play");hierarchyHost.Tick(hierarchy,0.1f);
        Check(parenting.Faulted() && hierarchyHost.Error(parenting).find("Ambiguous")!=std::string::npos,"Duplicate names silently picked a parent");hierarchyHost.Stop(hierarchy);
        duplicate.SetName("Unique");
        auto& observer=root.AddBehavior<ScriptBehavior>("Observer.zsh");parenting.SetPriority(1);observer.SetPriority(0);
        Check(hierarchyHost.Prepare(parenting,"class Parenting : gameObject {func update(float dt){parent=find(\"Target\");parent.transform.position.x+=4;}}","Parenting"),"Ordered parenting fixture");
        Check(hierarchyHost.Prepare(observer,"class Observer : gameObject {func update(float dt){find(\"Child\").parent.transform.position.x*=2;}}","Observer"),"Cross-VM hierarchy observer fixture");
        Check(hierarchyHost.Play(hierarchy),"Cross-VM parenting Play");hierarchyHost.Tick(hierarchy,0.1f);
        Check(!parenting.Faulted() && !observer.Faulted() && target.GetTransform().Position().x==8,"Later behavior did not observe earlier reparent/transform changes");
        hierarchyHost.Tick(hierarchy,0.1f);Check(target.GetTransform().Position().x==24,"Scene proxy synchronization clobbered another behavior's edits");
        hierarchyHost.Stop(hierarchy);Check(child.Parent()==root.Id() && target.GetTransform().Position().x==0,"Cross-VM Play state was not restored");
        ObjectStore directions; ScriptHost directionHost; auto& sourceObject=directions.Create("Source"); auto& targetObject=directions.Create("Target"); sourceObject.GetTransform().SetRotation({17,42,9}); auto& directionBehavior=sourceObject.AddBehavior<ScriptBehavior>("Directions.zsh");
        Check(directionHost.Prepare(directionBehavior,R"(class Directions : gameObject { func start(){ gameObject target=find("Target"); target.transform.forward=this.transform.forward; } })","Directions"),"Direction assignment compile");
        Check(directionHost.Play(directions) && !directionBehavior.Faulted(),"Direction assignment Play");
        const auto expectedForward=sourceObject.GetTransform().Rotation(); const auto actualForward=targetObject.GetTransform().Rotation(); directionHost.Stop(directions);
        Check(actualForward!=zengine::Vec3{},"Forward direction assignment did not update rotation");
        ObjectStore references; ScriptHost referenceHost; referenceHost.SetObjectStore(&references);
        auto& referenceOwner=references.Create("Reference Owner"); auto& referenceTarget=references.Create("Reference Target");
        referenceTarget.AddBehavior<physics::Collider>(); referenceTarget.AddBehavior<physics::RigidBody>();
        auto& referenceBehavior=referenceOwner.AddBehavior<ScriptBehavior>("References.zsh");
        const std::string referenceSource=R"(class References : gameObject {
            export RigidBody body;
            export Collider shape;
            export GameObject object;
            func start() { body = body; shape = shape; object = object; }
        })";
        Check(referenceHost.Prepare(referenceBehavior,referenceSource,"References"),"Native reference fixture compile");
        referenceHost.SetObjectReference(referenceBehavior,"body",referenceTarget.Id());
        referenceHost.SetObjectReference(referenceBehavior,"shape",referenceTarget.Id());
        referenceHost.SetObjectReference(referenceBehavior,"object",referenceTarget.Id());
        Check(Value(referenceHost,referenceBehavior,"body")=="Reference Target (RigidBody)","RigidBody reference was not assigned");
        Check(Value(referenceHost,referenceBehavior,"shape")=="Reference Target (Collider)","Collider reference was not assigned");
        Check(Value(referenceHost,referenceBehavior,"object")=="Reference Target (gameObject)","GameObject reference was not assigned");
        Check(referenceHost.AuthoredReferences(referenceBehavior).at("body")==referenceTarget.Id(),"Native reference was not retained as authored data");
        Check(referenceHost.Play(references) && !referenceBehavior.Faulted(),"Native reference Play failed"); referenceHost.Stop(references);
        referenceHost.SetObjectReference(referenceBehavior,"body",0); Check(Value(referenceHost,referenceBehavior,"body")=="None","Native reference was not clearable");
        ObjectStore globals;ScriptHost globalsHost;auto& platform=globals.Create("Platform");auto& actor=globals.Create("Actor");
        platform.GetTransform().SetPosition({10,0,0});platform.GetTransform().SetRotation({0,0,90});platform.GetTransform().SetScale({2,3,4});
        actor.GetTransform().SetPosition({1,0,0});auto& reader=actor.AddBehavior<ScriptBehavior>("Reader.zsh");
        Check(globalsHost.Prepare(reader,R"(class Reader : gameObject {
            export Vector3 position; export Vector3 rotation; export Vector3 scale;
            multiline export string notes="one\ntwo"; export char letter='A';
            func start(){parent=find("Platform");parent.transform.position.x+=1;position=transform.global_position;rotation=transform.global_rotation;scale=transform.global_scale;}
        })","Reader"),"Global reader compile");
        globalsHost.SetField(reader,"letter","\xc3\xa9");Rejected([&]{globalsHost.SetField(reader,"letter","ab");});
        Check(Value(globalsHost,reader,"letter")=="\xc3\xa9","Character inspector parse lost Unicode");
        const auto exported=globalsHost.Fields(reader);Check(exported.size()==5 && exported[3].multiline,"Global fields leaked into Inspector or multiline tag missing");
        Check(globalsHost.Play(globals) && !reader.Faulted(),"Global reader Play");
        Check(Value(globalsHost,reader,"position").starts_with("11, 2,"),"Same-callback reparent/global transform read stale");
        Check(Value(globalsHost,reader,"rotation")=="0, -0, 90" || Value(globalsHost,reader,"rotation")=="0, 0, 90","Global rotation mismatch");
        Check(Value(globalsHost,reader,"scale")=="2, 3, 4","Global scale mismatch");globalsHost.Stop(globals);
        ObjectStore spawning;ScriptHost spawningHost;auto& spawner=spawning.Create("Spawner");auto& spawnScript=spawner.AddBehavior<ScriptBehavior>("Spawner.zsh");
        Check(spawningHost.Prepare(spawnScript,R"(class Spawner : gameObject {export prefab template;export bool returned=false;func start(){gameObject made=template.spawn();made.transform.position.x=7;returned=made!=null;}})","Spawner"),"Prefab spawner compile");
        spawningHost.SetField(spawnScript,"template","Prefabs/Crate.zprefab");std::string requested;
        spawningHost.SetPrefabSpawner([&](std::string_view asset){requested=asset;return spawning.Create("Crate").Id();});
        Check(spawningHost.Play(spawning) && !spawnScript.Faulted(),"Prefab spawn failed during Start");
        Check(requested=="Prefabs/Crate.zprefab" && spawning.Size()==2 && As3D(spawning.At(1)).GetTransform().Position().x==7 && Value(spawningHost,spawnScript,"returned")=="true","Prefab spawn did not return/control the new native object");spawningHost.Stop(spawning);
        ObjectStore liveSpawn; ScriptHost liveSpawnHost; auto& liveOwner=liveSpawn.Create("Player"); auto& liveBehavior=liveOwner.AddBehavior<ScriptBehavior>("Player.zsh");
        Check(liveSpawnHost.Prepare(liveBehavior,R"(class Player : gameObject { export prefab template; export int updates=0; export bool spawned=false; func update(float dt){ updates+=1; if(!spawned){ template.spawn(); spawned=true; } } })","Player"),"Runtime spawn fixture compile");
        liveSpawnHost.SetField(liveBehavior,"template","Prefabs/Crate.zprefab"); liveSpawnHost.SetPrefabSpawner([&](std::string_view){ auto& bullet=liveSpawn.Create("Bullet"); auto& bulletBehavior=bullet.AddBehavior<ScriptBehavior>("Bullet.zsh"); liveSpawnHost.RestoreValues(bulletBehavior,{}); liveSpawnHost.RestoreReferences(bulletBehavior,{}); Check(liveSpawnHost.Prepare(bulletBehavior,"class Bullet : gameObject { export int updates=0; func update(float dt){ updates+=1; } }","Bullet"),"Spawned script prepare"); return bullet.Id(); });
        Check(liveSpawnHost.Play(liveSpawn),"Runtime spawn fixture Play"); liveSpawnHost.Tick(liveSpawn,0.1f); liveSpawnHost.Tick(liveSpawn,0.1f);
        Check(Value(liveSpawnHost,liveBehavior,"updates")=="2" && liveSpawn.Size()==2,"Spawning during update stopped the existing player behavior"); liveSpawnHost.Stop(liveSpawn);
        ObjectStore logging;ScriptHost loggingHost;std::string logged;loggingHost.SetPrintHandler([&](std::string_view text){logged=std::string(text);});auto& logger=logging.Create("Logger");auto& logScript=logger.AddBehavior<ScriptBehavior>("Logger.zsh");
        Check(loggingHost.Prepare(logScript,R"(class Logger : gameObject { export int count=7; export float ratio=1.5; func start(){ print("hello " + {count} + " " + {ratio}); print({transform.forward * 2}); } })","Logger"),"Print script compile failed");
        Check(loggingHost.Play(logging)&&logged=="0, 0, 2","Print callback did not receive interpolated vector output");loggingHost.Stop(logging);
        ObjectStore inputObjects; ScriptHost inputHost; auto& inputObject=inputObjects.Create("Input"); auto& inputBehavior=inputObject.AddBehavior<ScriptBehavior>("Shooter.zsh");
        Check(inputHost.Prepare(inputBehavior,R"(class Shooter : gameObject { export int shots=0; func start(){ Input.action("shoot").just_released.connect(shoot); } func update(float dt){} func shoot(){ shots+=1; } })","Shooter"),"Input signal fixture compile");
        inputHost.SetInput({{"shoot",{0,0,false,false,false}}}); Check(inputHost.Play(inputObjects),"Input signal Play");
        inputHost.SetInput({{"shoot",{0,0,true,true,false}}}); inputHost.Tick(inputObjects,0.1f);
        inputHost.SetInput({{"shoot",{0,0,false,false,true}}}); inputHost.Tick(inputObjects,0.1f);
        inputHost.SetInput({{"shoot",{0,0,true,true,false}}}); inputHost.Tick(inputObjects,0.1f);
        inputHost.SetInput({{"shoot",{0,0,false,false,true}}}); inputHost.Tick(inputObjects,0.1f);
        Check(Value(inputHost,inputBehavior,"shots")=="2","Input just_released signal did not fire on every release"); inputHost.Stop(inputObjects);
        // ZE-33: exported `array` fields are visible, editable, drag-assignable and reconciled.
        ObjectStore arr; ScriptHost arrHost; arrHost.SetObjectStore(&arr);
        auto& arrOwner=arr.Create("Bag Owner");
        auto& arrRig=arr.Create("Rig"); arrRig.AddBehavior<physics::Collider>(); arrRig.AddBehavior<physics::RigidBody>();
        auto& bag=arrOwner.AddBehavior<ScriptBehavior>("Bag.zsh");
        Check(arrHost.Prepare(bag,"class Bag : gameObject { export array numbers; export array things; func start(){} }","Bag"),"Array fixture compile");
        const auto arrayField=[&](const std::string& name){ std::vector<std::string> v; for(const auto& f:arrHost.Fields(bag)) if(f.name==name && f.arrayIndex>=0) v.push_back(f.value); return v; };
        { int headers=0; for(const auto& f:arrHost.Fields(bag)) if(f.array) ++headers; Check(headers==2,"Array headers not surfaced"); }
        arrHost.AddArrayElement(bag,"numbers","int"); arrHost.AddArrayElement(bag,"numbers","int");
        arrHost.SetArrayElement(bag,"numbers",0,"5"); arrHost.SetArrayElement(bag,"numbers",1,"9");
        Rejected([&]{ arrHost.SetArrayElement(bag,"numbers",0,"nan"); });
        Rejected([&]{ arrHost.AddArrayElement(bag,"numbers","banana"); });
        Check(arrayField("numbers")==(std::vector<std::string>{"5","9"}),"Array scalar elements not visible/editable");
        arrHost.RemoveArrayElement(bag,"numbers",0);
        Check(arrayField("numbers")==(std::vector<std::string>{"9"}),"Array element removal failed");
        arrHost.SetArrayElementReference(bag,"things",static_cast<std::size_t>(-1),arrRig.Id()); // append via clamp
        Check(arrayField("things")==(std::vector<std::string>{"Rig (RigidBody)"}),"Dragged array element not auto-typed");
        Check(arrHost.AuthoredArrays(bag).at("things").at(0).reference==arrRig.Id(),"Array reference not authored");
        Check(arrHost.Play(arr) && !bag.Faulted(),"Array Play failed"); arrHost.Stop(arr);
        Check(arrHost.Prepare(bag,"class Bag : gameObject { export array numbers; func start(){} }","Bag"),"Array recompile");
        Check(arrHost.AuthoredArrays(bag).count("numbers")==1 && arrHost.AuthoredArrays(bag).count("things")==0,"Array reconcile after field removal failed");
        // ZE-62: Input.mouse is fed from the host each Update; buttons + delta reach scripts.
        ObjectStore mouseStore; ScriptHost mouseHost;
        auto& cursorObject=mouseStore.Create("Cursor");
        auto& cursor=cursorObject.AddBehavior<ScriptBehavior>("Cursor.zsh");
        Check(mouseHost.Prepare(cursor,R"(class Cursor : gameObject {
            export int clicks=0; export float px=0; export float dx=0;
            func start(){ Input.mouse.clicked.connect(hit); }
            func update(float d){ px = Input.mouse.position.x; dx = Input.mouse.delta.x; }
            func hit(int b){ clicks += 1; }
        })","Cursor"),"Mouse fixture compile");
        script::MouseFrame m; m.x=0.5; mouseHost.SetMouse(m);
        Check(mouseHost.Play(mouseStore) && !cursor.Faulted(),"Mouse fixture Play");
        m.x=0.75; m.buttons[0]={true,true,false}; mouseHost.SetMouse(m); mouseHost.Tick(mouseStore,0.1f);
        Check(Value(mouseHost,cursor,"clicks")=="1","Mouse click signal did not reach the script");
        Check(Value(mouseHost,cursor,"px")=="0.75" && Value(mouseHost,cursor,"dx")=="0.25","Mouse position/delta not exposed to the script");
        mouseHost.Stop(mouseStore);
        // ZE-70 / ZE-69: find_by_type scans the live store; get_tags/has_tag read the owner's tags.
        ObjectStore tagStore; ScriptHost tagHost; tagHost.SetObjectStore(&tagStore);
        auto& seeker=tagStore.Create("Seeker"); seeker.SetTags({"player","alive"});
        auto& prey=tagStore.Create("Prey"); prey.AddBehavior<physics::RigidBody>();
        auto& seekScript=seeker.AddBehavior<ScriptBehavior>("Seeker.zsh");
        Check(tagHost.Prepare(seekScript,R"(class Seeker : gameObject {
            export bool found_prey=false; export bool is_player=false; export bool is_enemy=false; export int tag_count=0;
            func start(){
                found_prey = find_by_type(RigidBody) != null;
                is_player = has_tag("player"); is_enemy = has_tag("enemy");
                tag_count = get_tags().size();
            }
        })","Seeker"),"Tag/find fixture compile");
        Check(tagHost.Play(tagStore) && !seekScript.Faulted(),"Tag/find fixture Play");
        Check(Value(tagHost,seekScript,"found_prey")=="true","find_by_type did not locate the RigidBody object");
        Check(Value(tagHost,seekScript,"is_player")=="true" && Value(tagHost,seekScript,"is_enemy")=="false","has_tag membership wrong");
        Check(Value(tagHost,seekScript,"tag_count")=="2","get_tags count wrong");
        tagHost.Stop(tagStore);

        // ZE-61: EmitSignal delivers the UI "clicked" signal to a running script
        // that inherits a ui control.
        {
            ObjectStore uiStore; ScriptHost uiHost; uiHost.SetObjectStore(&uiStore);
            auto& button=uiStore.Create2D("Button");
            const auto buttonId=button.Id();
            auto& script=button.AddBehavior<ScriptBehavior>("Button.zsh");
            Check(uiHost.Prepare(script,R"(class Button : uiColorRect {
                export int hits = 0;
                func start() { clicked.connect(on_click); color = Vector3(0.2, 0.5, 1); }
                func on_click() { hits += 1; }
            })","Button"),"UI button fixture compile");
            uiHost.EmitSignal(buttonId,"clicked"); // ignored before Play
            Check(uiHost.Play(uiStore) && !script.Faulted(),"UI button fixture Play");
            Check(Value(uiHost,script,"hits")=="0","clicked fired before any click");
            uiHost.EmitSignal(buttonId,"clicked");
            uiHost.EmitSignal(buttonId,"clicked");
            Check(Value(uiHost,script,"hits")=="2","EmitSignal did not reach the clicked handler");
            uiHost.EmitSignal(999,"clicked"); // unknown owner: no-op, no throw
            uiHost.Stop(uiStore);
        }
        std::cout<<"PASS: Play/Stop, movement, values, signals, hierarchy, prefab spawning, script global transforms and text metadata\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
