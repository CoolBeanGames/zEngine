#include "ui/UiControl.h"

#include <array>
#include <cstring>

namespace zengine::ui
{
    namespace
    {
        constexpr std::array<std::pair<Anchor, const char*>, 20> kAnchorNames{{
            {Anchor::TopLeft, "top_left"}, {Anchor::TopCenter, "top_center"}, {Anchor::TopRight, "top_right"},
            {Anchor::CenterLeft, "center_left"}, {Anchor::Center, "center"}, {Anchor::CenterRight, "center_right"},
            {Anchor::BottomLeft, "bottom_left"}, {Anchor::BottomCenter, "bottom_center"}, {Anchor::BottomRight, "bottom_right"},
            {Anchor::FillLeft, "fill_left"}, {Anchor::FillRight, "fill_right"}, {Anchor::FillTop, "fill_top"}, {Anchor::FillBottom, "fill_bottom"},
            {Anchor::FillHorizontal, "fill_horizontal"}, {Anchor::FillVertical, "fill_vertical"}, {Anchor::Fill, "fill"},
            {Anchor::TopHalf, "top_half"}, {Anchor::BottomHalf, "bottom_half"}, {Anchor::LeftHalf, "left_half"}, {Anchor::RightHalf, "right_half"},
        }};
    }

    const char* AnchorName(Anchor anchor)
    {
        for (const auto& [value, name] : kAnchorNames) if (value == anchor) return name;
        return "top_left";
    }

    bool ParseAnchor(std::string_view name, Anchor& out)
    {
        for (const auto& [value, text] : kAnchorNames)
            if (name == text) { out = value; return true; }
        return false;
    }

    Rect ResolveAnchor(const Rect& parent, Anchor anchor, Vec2 offset, Vec2 size)
    {
        const float pw = parent.width, ph = parent.height;
        const float w = size.x, h = size.y;
        // Column: 0 = left, 1 = centre, 2 = right (unless the anchor fills the axis).
        auto place = [&](float extent, float parentExtent, int column) {
            if (column == 0) return 0.0f;
            if (column == 2) return parentExtent - extent;
            return (parentExtent - extent) * 0.5f;
        };

        Rect r{};
        switch (anchor)
        {
        case Anchor::Fill:            r = {0, 0, pw, ph}; break;
        case Anchor::FillHorizontal:  r = {0, place(h, ph, 1), pw, h}; break;
        case Anchor::FillVertical:    r = {place(w, pw, 1), 0, w, ph}; break;
        case Anchor::FillLeft:        r = {0, 0, w, ph}; break;
        case Anchor::FillRight:       r = {pw - w, 0, w, ph}; break;
        case Anchor::FillTop:         r = {0, 0, pw, h}; break;
        case Anchor::FillBottom:      r = {0, ph - h, pw, h}; break;
        case Anchor::TopHalf:         r = {0, 0, pw, ph * 0.5f}; break;
        case Anchor::BottomHalf:      r = {0, ph * 0.5f, pw, ph * 0.5f}; break;
        case Anchor::LeftHalf:        r = {0, 0, pw * 0.5f, ph}; break;
        case Anchor::RightHalf:       r = {pw * 0.5f, 0, pw * 0.5f, ph}; break;
        default:
        {
            const int col = (anchor == Anchor::TopLeft || anchor == Anchor::CenterLeft || anchor == Anchor::BottomLeft) ? 0
                          : (anchor == Anchor::TopRight || anchor == Anchor::CenterRight || anchor == Anchor::BottomRight) ? 2 : 1;
            const int row = (anchor == Anchor::TopLeft || anchor == Anchor::TopCenter || anchor == Anchor::TopRight) ? 0
                          : (anchor == Anchor::BottomLeft || anchor == Anchor::BottomCenter || anchor == Anchor::BottomRight) ? 2 : 1;
            r = {place(w, pw, col), place(h, ph, row), w, h};
            break;
        }
        }
        r.x += parent.x + offset.x;
        r.y += parent.y + offset.y;
        return r;
    }

    Vec2 UiControl::LocalOffset() const
    {
        if (const auto* node = As2D(&Owner())) { const auto p = node->GetTransform().Position(); return {p.x, p.y}; }
        return {};
    }

    void UiControl::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        for (auto* child : children)
            child->SetLayoutRect(ResolveAnchor(self, child->GetAnchor(), child->LocalOffset(), child->DesiredSize()));
    }

    // ----- Container -------------------------------------------------------

    Rect Container::Inner(const Rect& self) const
    {
        return {self.x + padding_, self.y + padding_,
                std::max(0.0f, self.width - padding_ * 2), std::max(0.0f, self.height - padding_ * 2)};
    }

    void Container::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect inner = Inner(self);
        for (auto* child : children)
            child->SetLayoutRect(ResolveAnchor(inner, child->GetAnchor(), child->LocalOffset(), child->DesiredSize()));
    }

    void HTileBoxContainer::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect inner = Inner(self);
        float x = inner.x;
        for (auto* child : children)
        {
            const Vec2 desired = child->DesiredSize();
            const float h = fillCross_ ? inner.height : std::min(desired.y, inner.height);
            const float y = fillCross_ ? inner.y : inner.y + (inner.height - h) * 0.5f;
            child->SetLayoutRect({x, y, desired.x, h});
            x += desired.x + spacing_;
        }
    }

    Vec2 HTileBoxContainer::DesiredSize() const
    {
        return {std::max(size_.x, minSize_.x), std::max(size_.y, minSize_.y)};
    }

    void VTileBoxContainer::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect inner = Inner(self);
        float y = inner.y;
        for (auto* child : children)
        {
            const Vec2 desired = child->DesiredSize();
            const float w = fillCross_ ? inner.width : std::min(desired.x, inner.width);
            const float x = fillCross_ ? inner.x : inner.x + (inner.width - w) * 0.5f;
            child->SetLayoutRect({x, y, w, desired.y});
            y += desired.y + spacing_;
        }
    }

    Vec2 VTileBoxContainer::DesiredSize() const
    {
        return {std::max(size_.x, minSize_.x), std::max(size_.y, minSize_.y)};
    }

    void CenterContainer::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect inner = Inner(self);
        for (auto* child : children)
        {
            const Vec2 d = child->DesiredSize();
            child->SetLayoutRect({inner.x + (inner.width - d.x) * 0.5f, inner.y + (inner.height - d.y) * 0.5f, d.x, d.y});
        }
    }

    void MarginContainer::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect box{self.x + left_, self.y + top_,
                       std::max(0.0f, self.width - left_ - right_), std::max(0.0f, self.height - top_ - bottom_)};
        for (auto* child : children) child->SetLayoutRect(box);
    }

    void PanelContainer::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>&) const
    {
        SpriteDraw sprite;
        sprite.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        sprite.tint = tint_;
        sprite.slice = slice_;
        if (!texture_.empty() && HasContext() && Context().resolveTexture)
            sprite.texture = Context().resolveTexture(texture_);
        sprites.push_back(sprite);
    }

    // ----- Text ----------------------------------------------------------

    Vec2 Text::DesiredSize() const
    {
        if (HasContext() && Context().measureText)
        {
            const Vec2 measured = Context().measureText(text_, pixelHeight_);
            return {std::max({measured.x, size_.x, minSize_.x}), std::max({pixelHeight_ * 1.3f, size_.y, minSize_.y})};
        }
        return {std::max(size_.x, minSize_.x), std::max({pixelHeight_ * 1.3f, size_.y, minSize_.y})};
    }

    void Text::Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>& texts) const
    {
        if (text_.empty()) return;
        texts.push_back({text_, rect_.x, rect_.y, pixelHeight_, color_});
    }

    Vec2 LongText::DesiredSize() const
    {
        int lines = 1;
        for (char c : text_) if (c == '\n') ++lines;
        float widest = 0;
        if (HasContext() && Context().measureText)
        {
            std::string line;
            for (char c : text_)
            {
                if (c == '\n') { widest = std::max(widest, Context().measureText(line, pixelHeight_).x); line.clear(); }
                else line.push_back(c);
            }
            widest = std::max(widest, Context().measureText(line, pixelHeight_).x);
        }
        return {std::max({widest, size_.x, minSize_.x}),
                std::max({static_cast<float>(lines) * pixelHeight_ * 1.3f, size_.y, minSize_.y})};
    }

    // ----- TextEntry ----------------------------------------------------

    bool TextEntry::Type(char32_t codepoint)
    {
        if (codepoint == 8) // backspace: drop one UTF-8 code point
        {
            if (text_.empty()) return false;
            std::size_t cut = text_.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(text_[cut]) & 0xC0) == 0x80) --cut;
            text_.erase(cut);
            return true;
        }
        if (codepoint < 0x20 || codepoint == 0x7f) return false;
        if (codepoint < 0x80) text_.push_back(static_cast<char>(codepoint));
        else if (codepoint < 0x800)
        {
            text_.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            text_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            text_.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            text_.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            text_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        return true;
    }

    void TextEntry::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const
    {
        SpriteDraw box;
        box.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        box.tint = focused_ ? Float4{0.16f, 0.18f, 0.24f, 1} : Float4{0.10f, 0.11f, 0.14f, 1};
        sprites.push_back(box);

        const bool showPlaceholder = text_.empty() && !placeholder_.empty();
        const std::string& shown = showPlaceholder ? placeholder_ : text_;
        if (!shown.empty())
        {
            const Float4 colour = showPlaceholder ? Float4{0.55f, 0.57f, 0.62f, 1} : Float4{0.95f, 0.96f, 0.98f, 1};
            texts.push_back({shown, rect_.x + 4, rect_.y + (rect_.height - pixelHeight_) * 0.5f, pixelHeight_, colour});
        }
    }

    // ----- TextureRect --------------------------------------------------

    Vec2 TextureRect::DesiredSize() const
    {
        if ((size_.x == 0 || size_.y == 0) && HasContext() && Context().textureSize && !texture_.empty())
        {
            const Vec2 native = Context().textureSize(texture_);
            if (native.x > 0 && native.y > 0) return native;
        }
        return {std::max(size_.x, minSize_.x), std::max(size_.y, minSize_.y)};
    }

    void TextureRect::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>&) const
    {
        SpriteDraw sprite;
        sprite.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        sprite.region = region_;
        sprite.tint = tint_;
        if (!texture_.empty() && HasContext() && Context().resolveTexture)
            sprite.texture = Context().resolveTexture(texture_);
        sprites.push_back(sprite);
    }

    // ----- ColorRect --------------------------------------------------

    void ColorRect::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>&) const
    {
        SpriteDraw sprite;
        sprite.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        sprite.tint = color_;
        sprites.push_back(sprite);
    }

    // ----- ProgressBar -----------------------------------------------

    void ProgressBar::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>&) const
    {
        SpriteDraw back;
        back.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        back.tint = background_;
        sprites.push_back(back);

        SpriteDraw front;
        if (vertical_)
        {
            const float h = rect_.height * value_;
            front.dest = {rect_.x, rect_.y + rect_.height - h, rect_.width, h};
        }
        else
        {
            front.dest = {rect_.x, rect_.y, rect_.width * value_, rect_.height};
        }
        front.tint = fill_;
        if (front.dest.width > 0 && front.dest.height > 0) sprites.push_back(front);
    }
}
