#include "EditorShell.h"
#include "input/InputMapEditor.h"
#include <algorithm>
#include <stdexcept>
void EditorShell::OpenInputMap() {
    RequireProject();
    if(Playing())throw std::runtime_error("Stop Play before editing the Input Map.");
    zengine::input::Ensure(assetsDirectory_);
    if(!inputEditor_)inputEditor_=std::make_unique<InputMapEditor>(window_,assetsDirectory_);
    inputEditor_->Show();
}
void EditorShell::TickInput() {
    // Never consume gameplay keys while typing in an Inspector/script/tool window.
    const auto focus=GetFocus();
    const bool active=cameraDrag_==CameraDrag::None && GetForegroundWindow()==window_ && (focus==window_ || focus==viewportWindow_);
    const auto& states=inputSystem_.Tick(zengine::input::PollWindows(active));
    zengine::script::InputFrame frame;
    for(const auto& [name,s]:states)frame.emplace(name,zengine::script::InputState{s.x,s.y,s.pressed,s.justPressed,s.justReleased});
    scriptHost_.SetInput(std::move(frame));

    // Input.mouse: viewport-relative normalized position (-1..1, +y up) plus L/R/M buttons.
    zengine::script::MouseFrame mouse;
    RECT vp{};
    if(viewportWindow_ && GetClientRect(viewportWindow_,&vp) && vp.right>0 && vp.bottom>0) {
        POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(viewportWindow_,&cursor);
        // ZE-84: apply a script-requested capture mode, but only while the Play preview owns the viewport.
        const bool viewportFocus = Playing() && active && cursor.x>=0 && cursor.y>=0 && cursor.x<vp.right && cursor.y<vp.bottom;
        zengine::game::ApplyMouseMode(Playing()?scriptHost_.MouseMode():0,viewportWindow_,vp,cursor,viewportFocus,mouseCapture_,mouse);
    } else mouseCapture_.Release();
    const int vks[3]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON};
    for(int i=0;i<3;++i) {
        const bool down = active && (GetKeyState(vks[i]) & 0x8000)!=0;
        auto& b=mouse.buttons[static_cast<std::size_t>(i)];
        b.pressed=down; b.justPressed=down && !mouseButtonsPrev_[i]; b.justReleased=!down && mouseButtonsPrev_[i];
        mouseButtonsPrev_[i]=down;
    }
    scriptHost_.SetMouse(mouse);
}
