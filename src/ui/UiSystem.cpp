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

        // Reference-resolution scaling: lay out against the authored size, then
        // Emit / Interact map to / from real window pixels.
        scale_ = {1, 1};
        offset_ = {0, 0};
        Vec2 design = screenSize;
        const Vec2 ref = context.referenceResolution;
        if (ref.x > 0 && ref.y > 0 && screenSize.x > 0 && screenSize.y > 0 && context.scaleMode != ScaleMode::Disabled)
        {
            design = ref;
            const float sx = screenSize.x / ref.x, sy = screenSize.y / ref.y;
            switch (context.scaleMode)
            {
            case ScaleMode::Stretch:     scale_ = {sx, sy}; break;
            case ScaleMode::FixedWidth:  scale_ = {sx, sx}; break;
            case ScaleMode::FixedHeight: scale_ = {sy, sy}; break;
            case ScaleMode::KeepAspect:
            default:                     { const float s = std::min(sx, sy); scale_ = {s, s}; } break;
            }
            offset_ = {(screenSize.x - ref.x * scale_.x) * 0.5f, (screenSize.y - ref.y * scale_.y) * 0.5f};
        }

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

        // Drop focus / hover / press targets that no longer exist.
        for (GameObjectId* id : {&focused_, &hovered_, &pressedOn_})
            if (*id && index_.find(*id) == index_.end()) *id = 0;

        for (auto& node : nodes_) SortChildren(node.children);
        SortChildren(roots_);

        // Effective visibility, then top-down layout from the (authored) screen rect.
        const Rect screenRect{0, 0, design.x, design.y};
        for (std::size_t r : roots_)
        {
            nodes_[r].visible = nodes_[r].control->Visible();
            nodes_[r].enabled = nodes_[r].control->Enabled();
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
            nodes_[c].enabled = node.enabled && nodes_[c].control->Enabled();
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
        const bool scaled = scale_.x != 1.0f || scale_.y != 1.0f || offset_.x != 0.0f || offset_.y != 0.0f;
        const auto mapRect = [&](SpriteRect r) {
            return SpriteRect{offset_.x + r.x * scale_.x, offset_.y + r.y * scale_.y, r.width * scale_.x, r.height * scale_.y};
        };
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
            if (!nodes_[n].enabled) // dim a disabled subtree
            {
                for (std::size_t i = firstSprite; i < sprites.size(); ++i) sprites[i].tint.w *= 0.4f;
                for (std::size_t i = firstText; i < texts.size(); ++i) texts[i].color.w *= 0.4f;
            }
            if (scaled)
            {
                for (std::size_t i = firstSprite; i < sprites.size(); ++i)
                {
                    sprites[i].dest = mapRect(sprites[i].dest);
                    if (sprites[i].clip.width > 0 && sprites[i].clip.height > 0) sprites[i].clip = mapRect(sprites[i].clip);
                }
                for (std::size_t i = firstText; i < texts.size(); ++i)
                {
                    texts[i].x = offset_.x + texts[i].x * scale_.x;
                    texts[i].y = offset_.y + texts[i].y * scale_.y;
                    texts[i].pixelHeight *= scale_.y;
                    if (texts[i].clip.width > 0 && texts[i].clip.height > 0) texts[i].clip = mapRect(texts[i].clip);
                }
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
            if (node.enabled && node.control->Clickable() && node.control->AcceptsPoint(pixel))
                return node.id;
        }
        return 0;
    }

    std::vector<GameObjectId> UiSystem::FocusableOrder() const
    {
        // Paint order = front-to-back visually; Tab walks it front to back.
        std::vector<GameObjectId> order;
        for (std::size_t n : PaintOrder())
            if (nodes_[n].visible && nodes_[n].enabled && nodes_[n].control->Clickable()) order.push_back(nodes_[n].id);
        return order;
    }

    void UiSystem::ChangeFocus(GameObjectId next)
    {
        if (next == focused_) return;
        if (focused_)
        {
            if (const auto it = index_.find(focused_); it != index_.end())
            {
                if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control)) entry->SetFocused(false);
                if (auto* button = dynamic_cast<Button*>(nodes_[it->second].control)) button->SetFocused(false);
                focusExited_ = focused_;
            }
        }
        focused_ = next;
        if (focused_)
        {
            if (const auto it = index_.find(focused_); it != index_.end())
            {
                if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control)) entry->SetFocused(true);
                if (auto* button = dynamic_cast<Button*>(nodes_[it->second].control)) button->SetFocused(true);
                focusEntered_ = focused_;
            }
        }
    }

    void UiSystem::SetFocus(GameObjectId id)
    {
        if (id && index_.find(id) == index_.end()) return;
        ChangeFocus(id);
    }

    std::vector<GameObjectId> UiSystem::Interact(Vec2 pixel, bool pressed,
                                                const std::vector<char32_t>& typed, float wheelDelta, bool shiftHeld)
    {
        std::vector<GameObjectId> clicks;
        presses_.clear(); releases_.clear(); entered_.clear(); exited_.clear(); submissions_.clear();
        focusEntered_ = focusExited_ = 0;
        // Map the real-window cursor back into authored (layout) space.
        if (scale_.x != 0.0f) pixel.x = (pixel.x - offset_.x) / scale_.x;
        if (scale_.y != 0.0f) pixel.y = (pixel.y - offset_.y) / scale_.y;
        const GameObjectId under = HitTest(pixel);

        // Hover enter / exit.
        if (under != hovered_)
        {
            if (hovered_ && index_.find(hovered_) != index_.end()) exited_.push_back(hovered_);
            if (under) entered_.push_back(under);
            hovered_ = under;
        }

        // Mouse wheel -> nearest scrolling container under the cursor.
        bool wheelConsumed = false;
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
                wheelConsumed = true;
                break;
            }
        }

        if (pressed && !pressActive_)
        {
            pressActive_ = true;
            pressedOn_ = under; // 0 == press began over empty space
            if (under) presses_.push_back(under);
            // Clicking focuses a clickable control (blurs otherwise).
            ChangeFocus(under && nodes_[index_.at(under)].control->Clickable() ? under : GameObjectId{0});
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

        // Keyboard focus model: Tab / Shift-Tab cycle, Esc blurs, Enter submits a
        // focused TextEntry or activates a focused Button, Space activates a Button.
        bool navHandled = false;
        for (char32_t c : typed)
        {
            if (c == 9) // Tab
            {
                const auto focusable = FocusableOrder();
                if (!focusable.empty())
                {
                    auto pos = std::find(focusable.begin(), focusable.end(), focused_);
                    std::size_t index = pos == focusable.end() ? 0 : static_cast<std::size_t>(pos - focusable.begin());
                    if (pos == focusable.end()) index = shiftHeld ? focusable.size() - 1 : 0;
                    else index = shiftHeld ? (index + focusable.size() - 1) % focusable.size()
                                           : (index + 1) % focusable.size();
                    ChangeFocus(focusable[index]);
                }
                navHandled = true;
            }
            else if (c == 27) { ChangeFocus(0); navHandled = true; } // Esc
            else if ((c == 13 || c == 32) && focused_) // Enter / Space
            {
                if (const auto it = index_.find(focused_); it != index_.end())
                {
                    if (dynamic_cast<TextEntry*>(nodes_[it->second].control))
                    { if (c == 13) { submissions_.push_back(focused_); navHandled = true; } }
                    else if (dynamic_cast<Button*>(nodes_[it->second].control))
                    { clicks.push_back(focused_); navHandled = true; }
                }
            }
        }

        // Printable text into the focused TextEntry (skip the nav codepoints).
        if (focused_ && !typed.empty())
            if (const auto it = index_.find(focused_); it != index_.end())
                if (auto* entry = dynamic_cast<TextEntry*>(nodes_[it->second].control))
                    for (char32_t codepoint : typed)
                        if (codepoint != 9 && codepoint != 27 && codepoint != 13) entry->Type(codepoint);

        // Refresh Button pointer state for visuals (after pressedOn_ is updated).
        for (auto& node : nodes_)
            if (auto* button = dynamic_cast<Button*>(node.control))
                button->SetInteraction(node.id == under, pressed && pressedOn_ == node.id);

        tookPointer_ = under != 0 || pressActive_ || wheelConsumed;
        tookKeyboard_ = focused_ != 0 || navHandled;
        return clicks;
    }
}
