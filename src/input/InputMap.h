#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace zengine::input {
enum class Kind { Button, ButtonAxis1D, ButtonAxis2D, AnalogAxis1D, AnalogAxis2D };
struct Action {
    std::string name;
    Kind kind = Kind::Button;
    // Button: first binding. Axes: negative X, positive X, negative Y, positive Y.
    std::array<std::string,4> buttons{};
    int controller = 0;
    int analog = 0; // 1D: LX, LY, RX, RY, LT, RT. 2D: left/right stick.
    float deadzone = 0.2f;
};
using Map = std::vector<Action>;
struct Pad { bool connected=false; std::uint16_t buttons=0; std::array<float,6> axes{}; };
struct Hardware { std::array<bool,256> keys{}; std::array<Pad,4> pads{}; };
struct State { float x=0,y=0; bool pressed=false,justPressed=false,justReleased=false; };
using States = std::map<std::string,State>;
const std::vector<std::string>& ButtonNames();
std::optional<std::string> FirstPressedButton(const Hardware& current,const Hardware& previous,int controller=0);
void Validate(const Map&);
std::string Encode(const Map&);
Map Decode(const std::string&);
class System {
public:
    void Configure(Map map);
    const States& Tick(const Hardware&);
    const States& Current() const { return states_; }
private:
    Map map_;
    States states_;
};
}
