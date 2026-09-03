#pragma once

#include "ui/UiControl.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace zengine::ui
{
    // Walks a GameObject2D hierarchy, lays out every UiControl from the screen rect,
    // emits the whole UI as one batch of sprites / text, and routes pointer input.
    class UiSystem
    {
    public:
        struct Node
        {
            GameObjectId id = 0;
            UiControl* control = nullptr;
            std::vector<std::size_t> children; // indices into nodes_
            bool visible = true;               // effective (self AND ancestors)
            bool enabled = true;               // effective (self AND ancestors)
        };

        // Rebuilds the layout tree. `context` must outlive the following Emit / Interact
        // calls. `deltaSeconds` (> 0) advances animated controls (video playback).
        void Build(ObjectStore& objects, Vec2 screenSize, const UiContext& context, float deltaSeconds = 0);

        // Appends the laid-out UI (respecting order and visibility) as one batch.
        void Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const;

        std::size_t NodeCount() const noexcept { return nodes_.size(); }
        const Node& NodeAt(std::size_t index) const { return nodes_.at(index); }
        const Rect* RectOf(GameObjectId id) const;

        // Topmost clickable, visible control under `pixel`; 0 if none.
        GameObjectId HitTest(Vec2 pixel) const;

        // One frame of pointer + keyboard interaction. `pixel` is the cursor in screen
        // pixels; `pressed` is the primary button state. `typed` are code points entered
        // this frame - printable text goes to the focused TextEntry; Tab(9) / Enter(13) /
        // Esc(27) / Space(32) drive the focus model. `wheelDelta` is mouse-wheel notches
        // (positive = scroll up); `shiftHeld` makes Tab move focus backwards. Returns the
        // controls that completed a click (press then release over the same control), in
        // hit order (a keyboard-activated focused Button is included).
        std::vector<GameObjectId> Interact(Vec2 pixel, bool pressed,
                                           const std::vector<char32_t>& typed = {}, float wheelDelta = 0,
                                           bool shiftHeld = false);

        // Controls whose primary button went down / came up over them this frame.
        const std::vector<GameObjectId>& Presses() const noexcept { return presses_; }
        const std::vector<GameObjectId>& Releases() const noexcept { return releases_; }
        // ZE-96 signals: the pointer moved onto / off a control; a focused TextEntry
        // got Enter; the keyboard focus moved to / away from a control (0 = none).
        const std::vector<GameObjectId>& Entered() const noexcept { return entered_; }
        const std::vector<GameObjectId>& Exited() const noexcept { return exited_; }
        const std::vector<GameObjectId>& Submissions() const noexcept { return submissions_; }
        GameObjectId FocusEntered() const noexcept { return focusEntered_; }
        GameObjectId FocusExited() const noexcept { return focusExited_; }
        GameObjectId Focused() const noexcept { return focused_; }
        // Whether the UI consumed the pointer / keyboard this frame - the game input
        // layer should ignore clicks / keys when true.
        bool TookPointer() const noexcept { return tookPointer_; }
        bool TookKeyboard() const noexcept { return tookKeyboard_; }
        // Programmatic focus (0 = clear). Fires focus_entered / focus_exited via the
        // accessors above on the next Interact only if changed here between frames -
        // callers that need the signals should route through Tab instead.
        void SetFocus(GameObjectId id);

        // Reference-resolution scaling resolved by the last Build: emitted geometry
        // is authored-space * Scale() + Offset(); Interact maps the cursor back.
        Vec2 Scale() const noexcept { return scale_; }
        Vec2 Offset() const noexcept { return offset_; }

    private:
        void Layout(std::size_t index, const Rect& area, const Rect* clip);
        void SortChildren(std::vector<std::size_t>& children) const;

        std::vector<std::size_t> PaintOrder() const;
        std::vector<GameObjectId> FocusableOrder() const;
        void ChangeFocus(GameObjectId next); // updates focused_, TextEntry flags, focus signals

        std::vector<Node> nodes_;
        std::vector<std::size_t> roots_;
        std::unordered_map<GameObjectId, std::size_t> index_;
        Vec2 screen_{};
        Vec2 scale_{1, 1};
        Vec2 offset_{0, 0};
        GameObjectId pressedOn_ = 0;
        bool pressActive_ = false;     // a primary-button press is in progress
        GameObjectId focused_ = 0;
        GameObjectId hovered_ = 0;
        std::vector<GameObjectId> presses_;
        std::vector<GameObjectId> releases_;
        std::vector<GameObjectId> entered_;
        std::vector<GameObjectId> exited_;
        std::vector<GameObjectId> submissions_;
        GameObjectId focusEntered_ = 0;
        GameObjectId focusExited_ = 0;
        bool tookPointer_ = false;
        bool tookKeyboard_ = false;
    };
}
