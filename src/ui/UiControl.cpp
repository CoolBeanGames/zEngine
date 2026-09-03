#include "ui/UiControl.h"

#include <array>
#include <cctype>
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

    const char* HAlignName(HAlign a)
    { return a == HAlign::Center ? "center" : a == HAlign::Right ? "right" : "left"; }
    const char* VAlignName(VAlign a)
    { return a == VAlign::Middle ? "middle" : a == VAlign::Bottom ? "bottom" : "top"; }
    bool ParseHAlign(std::string_view n, HAlign& out)
    {
        if (n == "left") { out = HAlign::Left; return true; }
        if (n == "center" || n == "centre") { out = HAlign::Center; return true; }
        if (n == "right") { out = HAlign::Right; return true; }
        return false;
    }
    bool ParseVAlign(std::string_view n, VAlign& out)
    {
        if (n == "top") { out = VAlign::Top; return true; }
        if (n == "middle" || n == "center" || n == "centre") { out = VAlign::Middle; return true; }
        if (n == "bottom") { out = VAlign::Bottom; return true; }
        return false;
    }
    const char* ScaleModeName(ScaleMode m)
    {
        switch (m)
        {
        case ScaleMode::Disabled:    return "disabled";
        case ScaleMode::KeepAspect:  return "keep_aspect";
        case ScaleMode::Stretch:     return "stretch";
        case ScaleMode::FixedWidth:  return "fixed_width";
        case ScaleMode::FixedHeight: return "fixed_height";
        }
        return "keep_aspect";
    }
    bool ParseScaleMode(std::string_view n, ScaleMode& out)
    {
        if (n == "disabled") { out = ScaleMode::Disabled; return true; }
        if (n == "keep_aspect") { out = ScaleMode::KeepAspect; return true; }
        if (n == "stretch") { out = ScaleMode::Stretch; return true; }
        if (n == "fixed_width") { out = ScaleMode::FixedWidth; return true; }
        if (n == "fixed_height") { out = ScaleMode::FixedHeight; return true; }
        return false;
    }

    namespace
    {
        // Greedy word-wrap of one already-newline-free line to `maxWidth` px.
        std::vector<std::string> WrapLine(const std::string& line, float maxWidth,
                                          const std::function<Vec2(std::string_view, float)>& measure, float pixelHeight)
        {
            std::vector<std::string> out;
            if (!measure || maxWidth <= 0 || measure(line, pixelHeight).x <= maxWidth) { out.push_back(line); return out; }
            std::string current;
            std::size_t i = 0;
            while (i < line.size())
            {
                std::size_t j = line.find(' ', i);
                const std::string word = line.substr(i, j == std::string::npos ? std::string::npos : j - i);
                const std::string candidate = current.empty() ? word : current + " " + word;
                if (!current.empty() && measure(candidate, pixelHeight).x > maxWidth)
                { out.push_back(current); current = word; }
                else current = candidate;
                if (j == std::string::npos) break;
                i = j + 1;
            }
            if (!current.empty() || out.empty()) out.push_back(current);
            return out;
        }
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
            // A child that asks to fill / stretch / take a half is honoured (same
            // result Container gives); only genuinely point-anchored children are
            // hard-centred at their desired size.
            if (child->GetAnchor() >= Anchor::FillLeft)
            {
                child->SetLayoutRect(ResolveAnchor(inner, child->GetAnchor(), child->LocalOffset(), child->DesiredSize()));
                continue;
            }
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

    std::vector<std::string> Text::LayoutLines(float width) const
    {
        std::vector<std::string> raw;
        std::string line;
        for (char c : text_)
        {
            if (c == '\n') { raw.push_back(line); line.clear(); }
            else if (c != '\r') line.push_back(c);
        }
        raw.push_back(line);
        if (!wrap_ || width <= 0) return raw;
        const auto measure = HasContext() ? Context().measureText : std::function<Vec2(std::string_view, float)>{};
        std::vector<std::string> wrapped;
        for (const auto& r : raw)
            for (auto& w : WrapLine(r, width, measure, pixelHeight_)) wrapped.push_back(std::move(w));
        return wrapped;
    }

    void Text::Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>& texts) const
    {
        if (text_.empty()) return;
        const auto lines = LayoutLines(rect_.width);
        const float lineHeight = pixelHeight_ * 1.3f;
        const float block = lineHeight * static_cast<float>(lines.size());
        float y = rect_.y;
        if (alignV_ == VAlign::Middle) y += std::max(0.0f, (rect_.height - block) * 0.5f);
        else if (alignV_ == VAlign::Bottom) y += std::max(0.0f, rect_.height - block);
        const auto measure = HasContext() ? Context().measureText : std::function<Vec2(std::string_view, float)>{};
        for (const auto& line : lines)
        {
            float x = rect_.x;
            if (alignH_ != HAlign::Left && measure)
            {
                const float w = measure(line, pixelHeight_).x;
                if (alignH_ == HAlign::Center) x += std::max(0.0f, (rect_.width - w) * 0.5f);
                else x += std::max(0.0f, rect_.width - w);
            }
            if (!line.empty()) texts.push_back({line, x, y, pixelHeight_, color_});
            y += lineHeight;
        }
    }

    Vec2 LongText::DesiredSize() const
    {
        const float wrapWidth = wrap_ ? std::max(size_.x, minSize_.x) : 0.0f;
        const auto lines = LayoutLines(wrapWidth);
        float widest = 0;
        if (HasContext() && Context().measureText)
            for (const auto& l : lines) widest = std::max(widest, Context().measureText(l, pixelHeight_).x);
        return {std::max({widest, size_.x, minSize_.x}),
                std::max({static_cast<float>(lines.size()) * pixelHeight_ * 1.3f, size_.y, minSize_.y})};
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

    // ----- ScrollContainer (ZE-66) -------------------------------------

    void ScrollContainer::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        const Rect inner = Inner(self);

        float total = 0;
        for (const auto* child : children)
        {
            const Vec2 d = child->DesiredSize();
            total += (horizontal_ ? d.x : d.y) + spacing_;
        }
        if (!children.empty()) total = std::max(0.0f, total - spacing_);
        content_ = horizontal_ ? Vec2{total, inner.height} : Vec2{inner.width, total};

        const float viewport = horizontal_ ? inner.width : inner.height;
        const float maxScroll = std::max(0.0f, total - viewport);
        float& scroll = horizontal_ ? scrollX_ : scrollY_;
        scroll = std::clamp(scroll, 0.0f, maxScroll);

        float cursor = (horizontal_ ? inner.x : inner.y) - scroll;
        for (auto* child : children)
        {
            const Vec2 d = child->DesiredSize();
            if (horizontal_)
            {
                const float h = fillCross_ ? inner.height : std::min(d.y, inner.height);
                const float y = fillCross_ ? inner.y : inner.y + (inner.height - h) * 0.5f;
                child->SetLayoutRect({cursor, y, d.x, h});
                cursor += d.x + spacing_;
            }
            else
            {
                const float w = fillCross_ ? inner.width : std::min(d.x, inner.width);
                const float x = fillCross_ ? inner.x : inner.x + (inner.width - w) * 0.5f;
                child->SetLayoutRect({x, cursor, w, d.y});
                cursor += d.y + spacing_;
            }
        }
    }

    // ----- Button (ZE-66) --------------------------------------------

    Vec2 Button::DesiredSize() const
    {
        float width = std::max(size_.x, minSize_.x);
        if (HasContext() && Context().measureText && !text_.empty())
            width = std::max(width, Context().measureText(text_, pixelHeight_).x + 20.0f);
        return {width, std::max({size_.y, minSize_.y, pixelHeight_ + 12.0f})};
    }

    void Button::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const
    {
        const Visual visual = CurrentVisual();
        Float4 colour = normal_;
        const std::string* tex = &normalTex_;
        switch (visual)
        {
        case Visual::Hover:    colour = hover_;         tex = hoverTex_.empty() ? &normalTex_ : &hoverTex_; break;
        case Visual::Pressed:  colour = pressed_;       tex = pressedTex_.empty() ? &normalTex_ : &pressedTex_; break;
        case Visual::Disabled: colour = disabledColor_; tex = &normalTex_; break;
        case Visual::Normal:   break;
        }

        SpriteDraw bg;
        bg.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        bg.tint = colour;
        bg.slice = slice_;
        if (!tex->empty() && HasContext() && Context().resolveTexture)
            bg.texture = Context().resolveTexture(*tex);
        sprites.push_back(bg);

        if (!text_.empty())
        {
            Vec2 measured{static_cast<float>(text_.size()) * pixelHeight_ * 0.5f, pixelHeight_};
            if (HasContext() && Context().measureText) measured = Context().measureText(text_, pixelHeight_);
            const Float4 tc = visual == Visual::Disabled ? Float4{textColor_.x, textColor_.y, textColor_.z, textColor_.w * 0.5f} : textColor_;
            texts.push_back({text_, rect_.x + (rect_.width - measured.x) * 0.5f,
                             rect_.y + (rect_.height - pixelHeight_) * 0.5f, pixelHeight_, tc});
        }
    }

    // ----- VideoTexture (ZE-66) ------------------------------------

    void VideoTexture::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>&) const
    {
        SpriteDraw sprite;
        sprite.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
        sprite.tint = tint_;
        if (!video_.empty() && HasContext() && Context().resolveVideoFrame)
            sprite.texture = Context().resolveVideoFrame(video_, time_, loop_);
        sprites.push_back(sprite);
    }

    // ----- UiHtml (ZE-66) ----------------------------------------

    namespace
    {
        Float4 ParseCssColor(std::string_view text, Float4 fallback)
        {
            auto trim = [](std::string_view s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                return s;
            };
            text = trim(text);
            if (text.empty()) return fallback;
            if (text.front() == '#')
            {
                const std::string_view hex = text.substr(1);
                auto nib = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    return -1;
                };
                auto valid = [&](std::size_t n) { for (std::size_t i = 0; i < n; ++i) if (nib(hex[i]) < 0) return false; return true; };
                if (hex.size() == 3 && valid(3))
                    return {nib(hex[0]) / 15.0f, nib(hex[1]) / 15.0f, nib(hex[2]) / 15.0f, 1};
                if (hex.size() == 6 && valid(6))
                    return {(nib(hex[0]) * 16 + nib(hex[1])) / 255.0f,
                            (nib(hex[2]) * 16 + nib(hex[3])) / 255.0f,
                            (nib(hex[4]) * 16 + nib(hex[5])) / 255.0f, 1};
                return fallback;
            }
            struct Named { const char* name; Float4 value; };
            static const Named names[] = {
                {"white", {1, 1, 1, 1}}, {"black", {0, 0, 0, 1}}, {"red", {0.85f, 0.2f, 0.2f, 1}},
                {"green", {0.2f, 0.7f, 0.3f, 1}}, {"blue", {0.25f, 0.5f, 0.95f, 1}},
                {"gray", {0.5f, 0.5f, 0.5f, 1}}, {"grey", {0.5f, 0.5f, 0.5f, 1}},
                {"yellow", {0.95f, 0.85f, 0.2f, 1}}, {"orange", {0.95f, 0.6f, 0.2f, 1}},
                {"transparent", {0, 0, 0, 0}},
            };
            for (const auto& n : names) if (text == n.name) return n.value;
            return fallback;
        }

        std::string DecodeEntities(std::string in)
        {
            struct E { const char* name; const char* value; };
            static const E table[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                                      {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}};
            for (const auto& e : table)
            {
                std::string::size_type pos = 0;
                while ((pos = in.find(e.name, pos)) != std::string::npos)
                { in.replace(pos, std::strlen(e.name), e.value); pos += std::strlen(e.value); }
            }
            return in;
        }

        std::string Attribute(const std::string& tag, const std::string& name)
        {
            const auto key = name + "=";
            auto pos = tag.find(key);
            if (pos == std::string::npos) return {};
            pos += key.size();
            if (pos >= tag.size()) return {};
            char quote = tag[pos];
            if (quote == '"' || quote == '\'')
            {
                const auto end = tag.find(quote, pos + 1);
                if (end == std::string::npos) return {};
                return tag.substr(pos + 1, end - pos - 1);
            }
            const auto end = tag.find_first_of(" \t>", pos);
            return tag.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }
    }

    void UiHtml::SetHtml(std::string value)
    {
        if (value.size() > 16384) value.resize(16384);
        html_ = std::move(value);
        Parse();
    }

    void UiHtml::Parse()
    {
        blocks_.clear();

        struct Style { Float4 color{0.95f, 0.96f, 0.98f, 1}; Float4 background{0, 0, 0, 0}; float fontSize = 16; bool bold = false; };
        auto applyStyle = [](Style& style, const std::string& css) {
            std::string::size_type start = 0;
            while (start < css.size())
            {
                auto semi = css.find(';', start);
                const std::string decl = css.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                start = semi == std::string::npos ? css.size() : semi + 1;
                const auto colon = decl.find(':');
                if (colon == std::string::npos) continue;
                auto trim = [](std::string s) {
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                    return s;
                };
                const std::string prop = trim(decl.substr(0, colon));
                const std::string val = trim(decl.substr(colon + 1));
                if (prop == "color") style.color = ParseCssColor(val, style.color);
                else if (prop == "background" || prop == "background-color") style.background = ParseCssColor(val, style.background);
                else if (prop == "font-size")
                {
                    try { style.fontSize = std::clamp(std::stof(val), 6.0f, 96.0f); } catch (...) {}
                }
                else if (prop == "font-weight") style.bold = (val == "bold" || val == "700");
            }
        };

        Block current;
        bool haveText = false;
        bool boldSpan = false;
        std::string linkTarget;
        bool inLink = false;

        auto flush = [&]() {
            if (!haveText) return;
            // Collapse runs of whitespace.
            std::string collapsed;
            bool space = false;
            for (char c : current.text)
            {
                if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
                if (space && !collapsed.empty()) collapsed.push_back(' ');
                space = false;
                collapsed.push_back(c);
            }
            current.text = collapsed;
            if (!current.text.empty() && blocks_.size() < 512)
            {
                if (inLink) { current.kind = Block::Kind::Button; current.link = true; current.src = linkTarget; }
                blocks_.push_back(current);
            }
            current = Block{};
            haveText = false;
        };

        std::size_t i = 0;
        Style style;
        while (i < html_.size())
        {
            if (html_[i] == '<')
            {
                const auto close = html_.find('>', i);
                if (close == std::string::npos) break;
                std::string tag = html_.substr(i + 1, close - i - 1);
                i = close + 1;
                if (tag.empty()) continue;
                const bool closing = tag.front() == '/';
                if (closing) tag.erase(tag.begin());
                // tag name = leading run of letters/digits
                std::string name;
                for (char c : tag) { if (std::isalnum(static_cast<unsigned char>(c))) name.push_back(static_cast<char>(std::tolower(c))); else break; }

                if (name == "br") { flush(); if (blocks_.size() < 512) { Block b; b.kind = Block::Kind::Text; b.text = " "; blocks_.push_back(b); } continue; }
                if (name == "hr") { flush(); if (blocks_.size() < 512) { Block b; b.kind = Block::Kind::Rule; b.color = {0.4f, 0.42f, 0.48f, 1}; blocks_.push_back(b); } continue; }
                if (name == "img" && !closing)
                {
                    flush();
                    if (blocks_.size() < 512) { Block b; b.kind = Block::Kind::Image; b.src = DecodeEntities(Attribute(tag, "src")); blocks_.push_back(b); }
                    continue;
                }
                if (name == "b" || name == "strong") { boldSpan = !closing; continue; }
                if (name == "i" || name == "em" || name == "span" || name == "u") continue;
                if (name == "a")
                {
                    flush();
                    if (!closing) { inLink = true; linkTarget = DecodeEntities(Attribute(tag, "href")); }
                    else { inLink = false; linkTarget.clear(); }
                    continue;
                }
                if (name == "button")
                {
                    flush();
                    if (!closing) { inLink = true; linkTarget = DecodeEntities(Attribute(tag, "id")); }
                    else { inLink = false; linkTarget.clear(); }
                    continue;
                }
                // Block-level: div p h1 h2 h3 li ul ol - start a fresh line.
                flush();
                if (!closing)
                {
                    style = Style{};
                    if (name == "h1") { style.fontSize = 30; style.bold = true; }
                    else if (name == "h2") { style.fontSize = 24; style.bold = true; }
                    else if (name == "h3") { style.fontSize = 19; style.bold = true; }
                    const std::string css = Attribute(tag, "style");
                    if (!css.empty()) applyStyle(style, css);
                }
                continue;
            }

            // Text content up to the next '<'.
            const auto next = html_.find('<', i);
            std::string chunk = html_.substr(i, next == std::string::npos ? std::string::npos : next - i);
            i = next == std::string::npos ? html_.size() : next;
            chunk = DecodeEntities(chunk);
            bool hasNonSpace = false;
            for (char c : chunk) if (!std::isspace(static_cast<unsigned char>(c))) { hasNonSpace = true; break; }
            if (!hasNonSpace && !haveText) continue;
            if (!haveText)
            {
                current = Block{};
                current.color = style.color;
                current.background = style.background;
                current.fontSize = style.fontSize;
                current.bold = style.bold || boldSpan;
            }
            current.text += chunk;
            haveText = true;
        }
        flush();
    }

    float UiHtml::BlockHeight(const Block& block) const
    {
        switch (block.kind)
        {
        case Block::Kind::Rule:   return 3.0f;
        case Block::Kind::Image:  return 120.0f;
        case Block::Kind::Button: return std::max(26.0f, block.fontSize + 12.0f);
        case Block::Kind::Text:   break;
        }
        int lines = 1;
        for (char c : block.text) if (c == '\n') ++lines;
        return static_cast<float>(lines) * block.fontSize * 1.35f;
    }

    Vec2 UiHtml::DesiredSize() const
    {
        float height = 8;
        for (const auto& block : blocks_) height += BlockHeight(block) + 4;
        return {std::max({size_.x, minSize_.x, 120.0f}),
                std::max({size_.y, minSize_.y, height})};
    }

    void UiHtml::Arrange(const Rect& self, const std::vector<UiControl*>& children)
    {
        (void)children; // HTML content is self-contained; any GameObject children are left unlaid.
        float y = self.y + 6;
        const float x = self.x + 6;
        const float width = std::max(0.0f, self.width - 12);
        const auto measure = HasContext() ? Context().measureText : std::function<Vec2(std::string_view, float)>{};
        for (auto& block : blocks_)
        {
            block.lines.clear();
            float h = BlockHeight(block);
            if (block.kind == Block::Kind::Text || block.kind == Block::Kind::Button)
            {
                block.lines = WrapLine(block.text, block.kind == Block::Kind::Text ? width : std::max(0.0f, width - 16),
                                       measure, block.fontSize);
                if (block.kind == Block::Kind::Text)
                    h = std::max(h, static_cast<float>(block.lines.size()) * block.fontSize * 1.35f);
            }
            block.rect = {x, y, width, h};
            y += h + 4;
        }
    }

    bool UiHtml::PointHitsLink(Vec2 point) const
    {
        for (const auto& block : blocks_)
            if ((block.kind == Block::Kind::Button || block.link) && block.rect.Contains(point))
                return true;
        return false;
    }

    void UiHtml::Emit(std::vector<SpriteDraw>& sprites, std::vector<TextDraw>& texts) const
    {
        if (background_.w > 0)
        {
            SpriteDraw bg;
            bg.dest = {rect_.x, rect_.y, rect_.width, rect_.height};
            bg.tint = background_;
            sprites.push_back(bg);
        }
        for (const auto& block : blocks_)
        {
            if (block.background.w > 0)
            {
                SpriteDraw bd;
                bd.dest = {block.rect.x, block.rect.y, block.rect.width, block.rect.height};
                bd.tint = block.background;
                sprites.push_back(bd);
            }
            switch (block.kind)
            {
            case Block::Kind::Rule:
            {
                SpriteDraw rule;
                rule.dest = {block.rect.x, block.rect.y + block.rect.height * 0.5f - 1, block.rect.width, 2};
                rule.tint = block.color;
                sprites.push_back(rule);
                break;
            }
            case Block::Kind::Image:
            {
                SpriteDraw img;
                img.dest = {block.rect.x, block.rect.y, block.rect.width, block.rect.height};
                if (!block.src.empty() && HasContext() && Context().resolveTexture)
                    img.texture = Context().resolveTexture(block.src);
                sprites.push_back(img);
                break;
            }
            case Block::Kind::Button:
            {
                SpriteDraw box;
                box.dest = {block.rect.x, block.rect.y, block.rect.width, block.rect.height};
                box.tint = {0.20f, 0.34f, 0.52f, 1};
                sprites.push_back(box);
                if (!block.text.empty())
                {
                    const std::string& label = block.lines.empty() ? block.text : block.lines.front();
                    texts.push_back({label, block.rect.x + 8, block.rect.y + (block.rect.height - block.fontSize) * 0.5f,
                                     block.fontSize, {0.97f, 0.98f, 1.0f, 1}});
                }
                break;
            }
            case Block::Kind::Text:
            {
                const float lh = block.fontSize * 1.35f;
                float ty = block.rect.y;
                if (block.lines.empty())
                {
                    if (!block.text.empty()) texts.push_back({block.text, block.rect.x, ty, block.fontSize, block.color});
                }
                else for (const auto& line : block.lines)
                {
                    if (!line.empty()) texts.push_back({line, block.rect.x, ty, block.fontSize, block.color});
                    ty += lh;
                }
                break;
            }
            }
        }
    }
}
