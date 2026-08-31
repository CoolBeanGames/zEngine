#include "EditorShell.h"
#include "input/InputMapEditor.h"
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
    const bool active=GetForegroundWindow()==window_ && (focus==window_ || focus==viewportWindow_);
    const auto& states=inputSystem_.Tick(zengine::input::PollWindows(active));
    zengine::script::InputFrame frame;
    for(const auto& [name,s]:states)frame.emplace(name,zengine::script::InputState{s.x,s.y,s.pressed,s.justPressed,s.justReleased});
    scriptHost_.SetInput(std::move(frame));
}
