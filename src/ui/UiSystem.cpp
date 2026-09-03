#include "ui/UiSystem.h"

#include <algorithm>

namespace zengine::ui
{
    namespace
    {
        Rect Intersect(const Rect& a, const Rect& b)
        {
            const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
            const float x1 = std::min(a.x + a.width, b.x + b.width);
            const float y1 = std::min(a.y + a.height, b.y + b.height);
            return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        }
    }

    void UiSystem::Build(ObjectStore& objects, Vec2 screenSize, const UiContext& context, float deltaSeconds)
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
            if (deltaSeconds > 0) control->Tick(deltaSeconds);
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
            Layout(r, nodes_[r].control->LayoutRect(), nullptr);
        }
    }

    void UiSystem::SortChildren(std::vector<std::size_t>& children) const
    {
        std::stable_sort(children.begin(), children.end(), [&](std::size_t a, std::size_t b) {
            return nodes_[a].control->Order() < nodes_[b].control->Order();
        });
    }

    void UiSystem::Layout(std::size_t index, const Rect& area, const Rect* clip)
    {
        Node& node = nodes_[index];
        node.control->SetLayoutRect(area);
        node.control->SetClip(clip);

        std::vector<UiControl*> childControls;
        childControls.reserve(node.children.size());
        for (std::size_t c : node.children) childControls.push_back(nodes_[c].control);
        node.control->Arrange(area, childControls);

        Rect ownClip{};
        Rect childClipStorage{};
        const Rect* childClip = clip;
        if (node.control->ClipsChildren(ownClip, area))
        {
            childClipStorage = clip ? Intersect(*clip, ownClip) : ownClip;
            childClip = &childClipStorage;
        }

        for (std::size_t c : node.children)
        {
            nodes_[c].visible = node.visible && nodes_[c].control->Visible();
            Layout(c, nodes_[c].control->LayoutRect(), childClip);
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
        {
            const std::size_t firstSprite = sprites.size(), firstText = texts.size();
            nodes_[n].control->Emit(sprites, texts);
            if (const Rect* clip = nodes_[n].control->ClipRect())
            {
                const SpriteRect r{clip->x, clip->y, clip->width, clip->height};
                for (std::size_t i = firstSprite; i < sprites.size(); ++i) sprites[i].clip = r;
                for (std::size_t i = firstText; i < texts.size(); ++i) texts[i].clip = r;
            }
        }
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
            if (node.control->Clickable() && node.control->AcceptsPoint(pixel))
                return node.id;
        }
        return 0;
    }

    std::vector<GameObjectId> UiSystem::Interact(Vec2 pixel, bool pressed,
                                                const std::vector<char32_t>& typed, float wheelDelta)
    {
        std::vector<GameObjectId> clicks;
        presses_.clear();
        releases_.clear();
        const GameObjectId under = HitTest(pixel);

        // Mouse wheel -> nearest scrolling container under the cursor.
        if (wheelDelta != 0.0f)
        {
            const auto painted = PaintOrder();
            for (auto it = painted.rbegin(); it != painted.rend(); ++it)
            {
                Node& node = nodes_[*it];
                auto* scroll = dynamic_cast<ScrollContainer*>(node.control);
                if (!scroll || !node.control->AcceptsPoint(pixel)) continue;
                const float step = 32.0f * wheelDelta;
                if (scroll->Horizontal()) scroll->SetScrollX(scroll->ScrollX() - step);
                else scroll->SetScrollY(scroll->ScrollY() - step);
                break;
            }
        }

        if (pressed && !pressActive_)
        {
            pressActive_ = true;
            pressedOn_ = under; // 0 == press began over empty space
            if (under) presses_.push_back(under);
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
        else if (!pressed && pressActive_)
        {
            if (pressedOn_ && under == pressedOn_)
            {
                clicks.push_back(under);
                releases_.push_back(under);
            }
            pressActive_ = false;
            pressedOn_ = 0;
        }

        if (focused_ && !typed.empty())
        {
            const auto it = index_.find(focused_);
            if (it != index_.end())
                if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control))
                    for (char32_t codepoint : typed) entry->Type(codepoint);
        }

        // Refresh Button pointer state for visuals (after pressedOn_ is updated).
        for (auto& node : nodes_)
            if (auto* button = dynamic_cast<Button*>(node.control))
                button->SetInteraction(node.id == under, pressed && pressedOn_ == node.id);

        return clicks;
    }
}
