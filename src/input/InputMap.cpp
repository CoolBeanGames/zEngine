#include "InputMap.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace zengine::input {
namespace {
const std::map<std::string,int> keys = {{"Mouse Left",1},{"Mouse Right",2},{"Mouse Middle",4},{"Backspace",8},{"Tab",9},{"Enter",13},{"Shift",16},{"Ctrl",17},{"Alt",18},{"Escape",27},{"Space",32},{"Page Up",33},{"Page Down",34},{"End",35},{"Home",36},{"Left",37},{"Up",38},{"Right",39},{"Down",40},{"Insert",45},{"Delete",46}};
const std::map<std::string,int> padButtons = {{"Pad Up",1},{"Pad Down",2},{"Pad Left",4},{"Pad Right",8},{"Pad Start",16},{"Pad Back",32},{"Pad Left Stick",64},{"Pad Right Stick",128},{"Pad LB",256},{"Pad RB",512},{"Pad A",4096},{"Pad B",8192},{"Pad X",16384},{"Pad Y",32768}};
bool Down(const std::string& name, const Hardware& hardware, int controller) {
    if (name.empty()) return false;
    if (auto it=keys.find(name); it!=keys.end()) return hardware.keys[it->second];
    if (name.size()==1) return hardware.keys[static_cast<unsigned char>(name[0])];
    if (auto it=padButtons.find(name); it!=padButtons.end()) return hardware.pads[controller].connected && (hardware.pads[controller].buttons & it->second)!=0;
    return false;
}
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
}
const std::vector<std::string>& ButtonNames() {
    static const auto names=[] { std::vector<std::string> n{""}; for(char c='A';c<='Z';++c) n.emplace_back(1,c); for(char c='0';c<='9';++c)n.emplace_back(1,c); for(const auto& [k,v]:keys)n.push_back(k); for(const auto& [k,v]:padButtons)n.push_back(k); return n; }();
    return names;
}
std::optional<std::string> FirstPressedButton(const Hardware& current,const Hardware& previous,int controller) {
    if(controller<0 || controller>=4)throw std::runtime_error("Controller must be 1-4.");
    for(const auto& name:ButtonNames())if(!name.empty() && Down(name,current,controller) && !Down(name,previous,controller))return name;
    return std::nullopt;
}
void Validate(const Map& map) {
    Require(map.size()<=256,"Input Map allows at most 256 actions."); std::set<std::string> names;
    for(const auto& a:map) {
        Require(!a.name.empty() && a.name.size()<=80 && a.name.front()!=' ' && a.name.back()!=' ',"Action names must be 1-80 characters without surrounding spaces.");
        Require(std::all_of(a.name.begin(),a.name.end(),[](unsigned char c){return c>=32 && c<127;}),"Action names use printable ASCII.");
        Require(names.insert(a.name).second,"Action names must be unique (case-sensitive).");
        Require(static_cast<int>(a.kind)>=0 && static_cast<int>(a.kind)<=4,"Invalid action type.");
        Require(a.controller>=0 && a.controller<4,"Controller must be 1-4.");
        Require(a.analog>=0 && a.analog<6 && (a.kind!=Kind::AnalogAxis2D || a.analog<2),"Invalid analog source.");
        Require(std::isfinite(a.deadzone) && a.deadzone>=0 && a.deadzone<1,"Deadzone must be between 0 and 0.99.");
        for(const auto& button:a.buttons) Require(std::find(ButtonNames().begin(),ButtonNames().end(),button)!=ButtonNames().end(),"Unknown button binding.");
    }
}
std::string Encode(const Map& map) {
    Validate(map); std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(9)<<"ZENGINE_INPUT 1\n"<<map.size()<<'\n';
    for(const auto& a:map) { out<<std::quoted(a.name)<<' '<<static_cast<int>(a.kind); for(const auto& b:a.buttons)out<<' '<<std::quoted(b); out<<' '<<a.controller<<' '<<a.analog<<' '<<a.deadzone<<'\n'; }
    return out.str();
}
Map Decode(const std::string& text) {
    Require(text.size()<=256*1024 && text.find('\0')==std::string::npos,"Input Map is too large or not text.");
    std::istringstream in(text); in.imbue(std::locale::classic()); std::string magic; int version; std::size_t count;
    Require(static_cast<bool>(in>>magic>>version>>count) && magic=="ZENGINE_INPUT" && version==1 && count<=256,"Invalid Input Map header.");
    Map map; for(std::size_t i=0;i<count;++i) { Action a; int kind; Require(static_cast<bool>(in>>std::quoted(a.name)>>kind),"Truncated input action."); a.kind=static_cast<Kind>(kind); for(auto& b:a.buttons)Require(static_cast<bool>(in>>std::quoted(b)),"Truncated input binding."); Require(static_cast<bool>(in>>a.controller>>a.analog>>a.deadzone),"Truncated input settings."); map.push_back(std::move(a)); }
    in>>std::ws; Require(in.eof(),"Unexpected trailing Input Map data."); Validate(map); return map;
}
void System::Configure(Map map) { Validate(map); map_=std::move(map); states_.clear(); for(const auto& a:map_)states_.emplace(a.name,State{}); }
const States& System::Tick(const Hardware& hardware) {
    for(const auto& a:map_) {
        auto& state=states_.at(a.name); const bool old=state.pressed; float x=0,y=0;
        if(a.kind==Kind::Button) x=Down(a.buttons[0],hardware,a.controller)?1.0f:0.0f;
        else if(a.kind==Kind::ButtonAxis1D || a.kind==Kind::ButtonAxis2D) {
            x=static_cast<float>(Down(a.buttons[1],hardware,a.controller))-static_cast<float>(Down(a.buttons[0],hardware,a.controller));
            if(a.kind==Kind::ButtonAxis2D)y=static_cast<float>(Down(a.buttons[3],hardware,a.controller))-static_cast<float>(Down(a.buttons[2],hardware,a.controller));
        } else if(hardware.pads[a.controller].connected) {
            const auto& axes=hardware.pads[a.controller].axes;
            auto finite=[](float v){return std::isfinite(v)?std::clamp(v,-1.0f,1.0f):0.0f;};
            if(a.kind==Kind::AnalogAxis1D)x=finite(axes[a.analog]);
            else { x=finite(axes[a.analog*2]); y=finite(axes[a.analog*2+1]); }
            const float length=std::hypot(x,y);
            if(length<=a.deadzone){x=0;y=0;}
            else { const float magnitude=(std::min(length,1.0f)-a.deadzone)/(1-a.deadzone); x=x/length*magnitude; y=y/length*magnitude; }
        }
        const float length=std::hypot(x,y); if(length>1){x/=length;y/=length;}
        state={x,y,x!=0 || y!=0,!old && (x!=0 || y!=0),old && x==0 && y==0};
    }
    return states_;
}
}
