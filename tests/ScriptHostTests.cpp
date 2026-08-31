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
        Check(second.Owner().GetTransform().Position().x==1 && owner.GetTransform().Position().x==13.5f,"Instances share variables or transforms");
        host.Stop(objects);
        Check(host.Prepare(b,source+"\n// saved edit","Mover") && Value(host,b,"speed")=="3.5","Save discarded authoring overrides");
        Check(!host.Prepare(b,"class Mover { export Missing x; }","Mover") && !host.Play(objects),"Invalid compile allowed partial Play");
        Check(owner.GetTransform().Position().y==0 && !second.HasInstance(),"Failed Play partially started scene");
        Check(!host.Prepare(b,"class Other : gameObject {}","Mover"),"Filename mismatch not diagnosed");
        Check(!host.Prepare(b,"class Mover {}","Mover"),"Non-GameObject class attached");
        Check(host.Prepare(b,R"(class Mover : gameObject { func update(float dt) { transform.position.x=99; int zero=0; int fail=1/zero; } })","Mover"),"Failure fixture compile failed");
        Check(host.Play(objects),"Failure fixture Play failed"); host.Tick(objects,0.5f);
        Check(b.Faulted() && !host.Error(b).empty() && owner.GetTransform().Position().x==10,"Failure isolation/atomic transform sync failed");
        Check(second.Owner().GetTransform().Position().x==1,"Failed script stopped other scripts");
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
        std::cout<<"PASS: Play/Stop, movement, values, signals, hierarchy, script global transforms and text metadata\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
