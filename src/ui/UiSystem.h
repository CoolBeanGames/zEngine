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
        };

        // Rebuilds the layout tree. `context` must outlive the following Emit / Interact calls.
        void Build(ObjectStore& objects, Vec2 screenSize, const UiContext& context);

        // Appends the laid-out UI (respecting order and visibility) as one batch.
        void Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const;

        std::size_t NodeCount() const noexcept { return nodes_.size(); }
        const Node& NodeAt(std::size_t index) const { return nodes_.at(index); }
        const Rect* RectOf(GameObjectId id) const;

        // Topmost clickable, visible control under `pixel`; 0 if none.
        GameObjectId HitTest(Vec2 pixel) const;

        // One frame of pointer + keyboard interaction. `pixel` is the cursor in screen
        // pixels; `pressed` is the primary button state. `typed` are code points entered
        // this frame. Returns the controls that completed a click (press then release
        // over the same control) this frame, in hit order.
        std::vector<GameObjectId> Interact(Vec2 pixel, bool pressed, const std::vector<char32_t>& typed = {});

    private:
        void Layout(std::size_t index, const Rect& area);
        void SortChildren(std::vector<std::size_t>& children) const;

        std::vector<std::size_t> PaintOrder() const;

        std::vector<Node> nodes_;
        std::vector<std::size_t> roots_;
        std::unordered_map<GameObjectId, std::size_t> index_;
        Vec2 screen_{};
        GameObjectId pressedOn_ = 0;
        GameObjectId focused_ = 0;
    };
}
