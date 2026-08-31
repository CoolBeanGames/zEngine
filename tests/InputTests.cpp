#include "input/InputAssets.h"
#include "input/InputMapEditor.h"
#include "zscript/Script.h"
#include "ScriptHost.h"
#include <iostream>
#include <cmath>
#include <functional>
using namespace zengine;
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Reject(const std::function<void()>& f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection");}
int main(){try{
    input::Action jump{"jump",input::Kind::Button,{"Space"}};
    input::Action move{"move",input::Kind::ButtonAxis2D,{"A","D","S","W"}};
    input::Action horizontal{"horizontal",input::Kind::ButtonAxis1D,{"Left","Right"}};
    input::Action stick{"stick",input::Kind::AnalogAxis2D};
    input::Action trigger{"trigger",input::Kind::AnalogAxis1D};trigger.analog=4;
    input::Map map{jump,move,horizontal,stick,trigger};Check(input::Encode(input::Decode(input::Encode(map)))==input::Encode(map),"Input roundtrip");
    Reject([&]{auto bad=map;bad.push_back(jump);input::Validate(bad);});Reject([&]{input::Decode("ZENGINE_INPUT 2\n0");});Reject([&]{auto bad=map;bad[0].deadzone=1;input::Validate(bad);});Reject([&]{input::Decode(input::Encode(map)+"junk");});
    input::System system;system.Configure(map);input::Hardware h;
    h.keys[32]=true;auto states=system.Tick(h);Check(states.at("jump").justPressed && states.at("jump").pressed,"Press transition");
    states=system.Tick(h);Check(!states.at("jump").justPressed && states.at("jump").pressed,"Held transition");
    h.keys[32]=false;states=system.Tick(h);Check(states.at("jump").justReleased && !states.at("jump").pressed,"Release transition");
    h.keys['A']=true;h.keys['D']=true;states=system.Tick(h);Check(states.at("move").x==0,"Opposing keys");
    h.keys['A']=false;h.keys['W']=true;states=system.Tick(h);Check(std::abs(states.at("move").x-0.70710678f)<0.0001f && states.at("move").y>0,"Normalized diagonal");
    h.keys[37]=true;states=system.Tick(h);Check(states.at("horizontal").x==-1,"1D button axis");
    h.pads[0].connected=true;h.pads[0].axes[0]=0.1f;states=system.Tick(h);Check(states.at("stick").x==0,"Analog deadzone");
    h.pads[0].axes[0]=0.6f;h.pads[0].axes[4]=1;states=system.Tick(h);Check(std::abs(states.at("stick").x-0.5f)<0.0001f && states.at("trigger").x==1,"Analog rescale/trigger");
    h.pads[0].connected=false;states=system.Tick(h);Check(states.at("stick").justReleased,"Controller disconnect release");
    states=system.Tick({});Check(states.at("move").justReleased,"Focus release");
    auto compiled=script::Compiler::Compile(R"(class Test : gameObject {
        export int presses; export int releases; export int held; export bool polled; export Vector3 movement;
        func start(){Input.action("jump").just_pressed.connect(press);Input.action("jump").just_released.connect(release);Input.action("jump").is_pressed.connect(holding);}
        func press(){presses+=1;transform.position.y+=1;polled=Input.is_action_just_pressed("jump");}
        func release(){releases+=1;}func holding(){held+=1;}
        func update(float dt){movement=Input.get_vector("move");}
    })");Check(static_cast<bool>(compiled),compiled.diagnostics.empty()?"Input compile":compiled.diagnostics[0].message.c_str());
    script::Runtime vm(compiled.program);auto object=vm.Create("Test");script::InputFrame frame{{"jump",{}},{"move",{}}};vm.SetInput(frame,false);vm.Start(object);
    frame["jump"]={1,0,true,true,false};frame["move"]={0.5,0.5,true,true,false};vm.SetInput(frame);vm.Update(object,0.1);
    Check(std::get<std::int64_t>(vm.Get(object,"presses"))==1 && std::get<bool>(vm.Get(object,"polled")),"Input signals/poll disagree");
    Check(std::get<script::Vector3>(vm.Get(object,"movement")).y==0.5,"Input vector");
    frame["jump"].justPressed=false;vm.SetInput(frame);Check(std::get<std::int64_t>(vm.Get(object,"held"))==2,"Held signal each tick");
    frame["jump"]={0,0,false,false,true};vm.SetInput(frame);Check(std::get<std::int64_t>(vm.Get(object,"releases"))==1,"Release signal");
    ObjectStore objects;ScriptHost host;auto& owner=objects.Create();auto& behavior=owner.AddBehavior<ScriptBehavior>("InputMover.zsh");
    Check(host.Prepare(behavior,"class InputMover : gameObject {func start(){Input.action(\"jump\").just_pressed.connect(jump);}func jump(){transform.position.y+=2;}}","InputMover"),"Host Input prepare");
    host.SetInput({{"jump",{}}});Check(host.Play(objects),"Host Input Play");host.SetInput({{"jump",{1,0,true,true,false}}});host.Tick(objects,0.1f);
    Check(owner.GetTransform().Position().y==2 && !behavior.Faulted(),"Input callback did not move native object");host.Stop(objects);Check(owner.GetTransform().Position().y==0,"Input callback state leaked after Stop");
    Reject([&]{vm.Call(object,"missing");});
    Check(!script::Compiler::Compile("class A {func f(){Input.action(\"jump\").axis=3;}}"),"Input state writable");
    auto dir=std::filesystem::temp_directory_path()/(L"zEngineInputTest-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(dir);
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}} cleanup{dir};
    input::Ensure(dir);auto old=input::Load(dir);input::Save(dir,map,&old);Check(input::Decode(input::Load(dir)).size()==5,"Input persistence");Reject([&]{input::Save(dir,{},&old);});input::Ensure(dir);Check(input::Decode(input::Load(dir)).size()==5,"Ensure overwrote map");
    {InputMapEditor editor(nullptr,dir);auto window=editor.Window();Check(GetDlgItem(window,InputMapEditor::Save)!=nullptr,"Input editor controls");SendMessageW(window,WM_COMMAND,InputMapEditor::Add,0);Check(editor.Dirty(),"Add action UI");SetDlgItemTextW(window,InputMapEditor::Name,L"ui_action");SendMessageW(window,WM_COMMAND,InputMapEditor::Save,0);Check(!editor.Dirty() && input::Decode(input::Load(dir)).back().name=="ui_action","Input editor save");SendMessageW(window,WM_COMMAND,InputMapEditor::Remove,0);SendMessageW(window,WM_COMMAND,InputMapEditor::Save,0);Check(input::Decode(input::Load(dir)).size()==5,"Remove action UI");}
    std::cout<<"PASS Input Map, axes, signals, persistence and editor\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
