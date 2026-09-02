#include "ui/UiSystem.h"

#include <algorithm>

namespace zengine::ui
{
    void UiSystem::Build(ObjectStore& objects, Vec2 screenSize, const UiContext& context)
    {
        nodes_.clear();
        roots_.clear();
        index_.clear();
        screen_ = screenSize;

        // Collect every GameObject2D that carries a UiControl.
        for (std::size_t i = 0; i < objects.Size(); ++i)
        {
            auto& object = objects.At(i);
            auto* control = object.GetBehavior<UiControl>();
            if (!control) continue;
            control->SetContext(&context);
            index_.emplace(object.Id(), nodes_.size());
            nodes_.push_back(Node{object.Id(), control, {}, control->Visible()});
        }

        // Link children to the nearest UiControl ancestor.
        for (std::size_t n = 0; n < nodes_.size(); ++n)
        {
            const auto* object = objects.Find(nodes_[n].id);
            GameObjectId parent = object ? object->Parent() : 0;
            while (parent && index_.find(parent) == index_.end())
                parent = objects.Find(parent) ? objects.Find(parent)->Parent() : 0;
            if (parent) nodes_[index_.at(parent)].children.push_back(n);
            else roots_.push_back(n);
        }

        for (auto& node : nodes_) SortChildren(node.children);
        SortChildren(roots_);

        // Effective visibility, then top-down layout from the screen rect.
        const Rect screenRect{0, 0, screen_.x, screen_.y};
        for (std::size_t r : roots_)
        {
            nodes_[r].control->SetLayoutRect(ResolveAnchor(
                screenRect, nodes_[r].control->GetAnchor(), nodes_[r].control->LocalOffset(),
                nodes_[r].control->DesiredSize()));
            Layout(r, nodes_[r].control->LayoutRect());
        }
    }

    void UiSystem::SortChildren(std::vector<std::size_t>& children) const
    {
        std::stable_sort(children.begin(), children.end(), [&](std::size_t a, std::size_t b) {
            return nodes_[a].control->Order() < nodes_[b].control->Order();
        });
    }

    void UiSystem::Layout(std::size_t index, const Rect& area)
    {
        Node& node = nodes_[index];
        node.control->SetLayoutRect(area);

        std::vector<UiControl*> childControls;
        childControls.reserve(node.children.size());
        for (std::size_t c : node.children) childControls.push_back(nodes_[c].control);
        node.control->Arrange(area, childControls);

        for (std::size_t c : node.children)
        {
            nodes_[c].visible = node.visible && nodes_[c].control->Visible();
            Layout(c, nodes_[c].control->LayoutRect());
        }
    }

    std::vector<std::size_t> UiSystem::PaintOrder() const
    {
        // Depth-first in child order == painter's order (parent behind its children).
        std::vector<std::size_t> order;
        std::vector<std::size_t> stack(roots_.rbegin(), roots_.rend());
        while (!stack.empty())
        {
            const std::size_t n = stack.back();
            stack.pop_back();
            if (!nodes_[n].visible) continue;
            order.push_back(n);
            for (auto it = nodes_[n].children.rbegin(); it != nodes_[n].children.rend(); ++it)
                stack.push_back(*it);
        }
        return order;
    }

    void UiSystem::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const
    {
        for (std::size_t n : PaintOrder())
            nodes_[n].control->Emit(sprites, texts);
    }

    const Rect* UiSystem::RectOf(GameObjectId id) const
    {
        const auto it = index_.find(id);
        if (it == index_.end()) return nullptr;
        return &nodes_[it->second].control->LayoutRect();
    }

    GameObjectId UiSystem::HitTest(Vec2 pixel) const
    {
        const auto painted = PaintOrder();
        for (auto it = painted.rbegin(); it != painted.rend(); ++it)
        {
            const Node& node = nodes_[*it];
            if (node.control->Clickable() && node.control->LayoutRect().Contains(pixel))
                return node.id;
        }
        return 0;
    }

    std::vector<GameObjectId> UiSystem::Interact(Vec2 pixel, bool pressed, const std::vector<char32_t>& typed)
    {
        std::vector<GameObjectId> clicks;
        const GameObjectId under = HitTest(pixel);

        if (pressed && !pressedOn_)
        {
            pressedOn_ = under ? under : GameObjectId{1}; // sentinel: press began on empty space
            focused_ = 0;
            if (under)
            {
                const auto it = index_.find(under);
                if (it != index_.end())
                    if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control))
                    { focused_ = under; entry->SetFocused(true); }
            }
            for (auto& node : nodes_)
                if (node.id != focused_)
                    if (auto* entry = dynamic_cast<TextEntry*>(node.control)) entry->SetFocused(false);
        }
        else if (!pressed && pressedOn_)
        {
            if (pressedOn_ != GameObjectId{1} && under == pressedOn_) clicks.push_back(under);
            pressedOn_ = 0;
        }

        if (focused_ && !typed.empty())
        {
            const auto it = index_.find(focused_);
            if (it != index_.end())
                if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control))
                    for (char32_t codepoint : typed) entry->Type(codepoint);
        }
        return clicks;
    }
}
