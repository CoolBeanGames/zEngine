#pragma once

#include "core/GameObject.h"
#include "Render2D.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace zengine::ui
{
    // Auto anchor modes. An anchor picks the reference point inside the parent rect
    // that a control is positioned relative to (with the control's Transform2D
    // position used as the offset); the Fill / Half modes also drive the size.
    enum class Anchor
    {
        TopLeft, TopCenter, TopRight,
        CenterLeft, Center, CenterRight,
        BottomLeft, BottomCenter, BottomRight,
        FillLeft, FillRight, FillTop, FillBottom, // fill one axis, pin to the named edge
        FillHorizontal, FillVertical, Fill,
        TopHalf, BottomHalf, LeftHalf, RightHalf
    };

    const char* AnchorName(Anchor anchor);
    bool ParseAnchor(std::string_view name, Anchor& out);

    struct Rect
    {
        float x = 0, y = 0, width = 0, height = 0;
        bool Contains(Vec2 point) const noexcept
        {
            return point.x >= x && point.y >= y && point.x <= x + width && point.y <= y + height;
        }
    };

    // Resolves a child rect inside `parent` for the given anchor. `offset` is the
    // control's local position (its GameObject2D Transform2D); `size` its desired size.
    Rect ResolveAnchor(const Rect& parent, Anchor anchor, Vec2 offset, Vec2 size);

    class UiControl;

    // Per-frame services the layout pass needs (text measurement, texture sizes).
    struct UiContext
    {
        std::function<Vec2(std::string_view, float)> measureText;   // (utf8, pixelHeight) -> px
        std::function<Vec2(std::string_view)> textureSize;          // asset name -> px (0,0 if unknown)
        std::function<TextureHandle(std::string_view)> resolveTexture;
    };

    // A UI node. Attached as a behavior to a GameObject2D; the GameObject2D parent
    // hierarchy is the UI tree. Layout runs top-down from the screen rect each frame.
    class UiControl : public Behavior
    {
    public:
        explicit UiControl(ObjectCore& owner) : Behavior(owner) {}

        Anchor GetAnchor() const noexcept { return anchor_; }
        void SetAnchor(Anchor value) noexcept { anchor_ = value; }
        Vec2 Size() const noexcept { return size_; }
        void SetSize(Vec2 value) noexcept { size_ = {std::max(0.0f, value.x), std::max(0.0f, value.y)}; }
        Vec2 MinSize() const noexcept { return minSize_; }
        void SetMinSize(Vec2 value) noexcept { minSize_ = {std::max(0.0f, value.x), std::max(0.0f, value.y)}; }
        int Order() const noexcept { return order_; }
        void SetOrder(int value) noexcept { order_ = value; }
        bool Visible() const noexcept { return visible_; }
        void SetVisible(bool value) noexcept { visible_ = value; }
        bool Clickable() const noexcept { return clickable_; }
        void SetClickable(bool value) noexcept { clickable_ = value; }

        const Rect& LayoutRect() const noexcept { return rect_; }
        void SetLayoutRect(const Rect& value) noexcept { rect_ = value; }   // UiSystem only
        void SetContext(const UiContext* context) noexcept { context_ = context; } // UiSystem only

        // The control's local offset, taken from its GameObject2D transform.
        Vec2 LocalOffset() const;

        // Stable identifier used by scene / prefab serialization and the editor.
        virtual const char* TypeName() const noexcept { return "control"; }

        // Natural size before a parent container constrains it.
        virtual Vec2 DesiredSize() const { return {std::max(size_.x, minSize_.x), std::max(size_.y, minSize_.y)}; }

        // Places each direct child's LayoutRect given this control's resolved `self`
        // rect. The base anchors every child independently; containers override.
        virtual void Arrange(const Rect& self, const std::vector<UiControl*>& children);

        // Appends this control's own geometry. The base control draws nothing.
        virtual void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const {}

    protected:
        const UiContext& Context() const noexcept { return *context_; }
        bool HasContext() const noexcept { return context_ != nullptr; }

        Anchor anchor_ = Anchor::TopLeft;
        Vec2 size_{100, 32};
        Vec2 minSize_{0, 0};
        int order_ = 0;
        bool visible_ = true;
        bool clickable_ = false;
        Rect rect_{};
        const UiContext* context_ = nullptr;
    };

    // ----- Containers --------------------------------------------------------

    // Base container: keeps a uniform inner padding and, by default, anchors each
    // child inside the padded area (same as UiControl but inset).
    class Container : public UiControl
    {
    public:
        explicit Container(ObjectCore& owner) : UiControl(owner) {}
        const char* TypeName() const noexcept override { return "container"; }
        float Padding() const noexcept { return padding_; }
        void SetPadding(float value) noexcept { padding_ = std::max(0.0f, value); }
        float Spacing() const noexcept { return spacing_; }
        void SetSpacing(float value) noexcept { spacing_ = std::max(0.0f, value); }
        void Arrange(const Rect& self, const std::vector<UiControl*>& children) override;
    protected:
        Rect Inner(const Rect& self) const;
        float padding_ = 0;
        float spacing_ = 0;
    };

    // Lays children out left to right. `fillCross` stretches them to the row height.
    class HTileBoxContainer final : public Container
    {
    public:
        explicit HTileBoxContainer(ObjectCore& owner) : Container(owner) {}
        const char* TypeName() const noexcept override { return "hbox"; }
        bool FillCross() const noexcept { return fillCross_; }
        void SetFillCross(bool value) noexcept { fillCross_ = value; }
        void Arrange(const Rect& self, const std::vector<UiControl*>& children) override;
        Vec2 DesiredSize() const override;
    private:
        bool fillCross_ = true;
    };

    // Lays children out top to bottom. `fillCross` stretches them to the column width.
    class VTileBoxContainer final : public Container
    {
    public:
        explicit VTileBoxContainer(ObjectCore& owner) : Container(owner) {}
        const char* TypeName() const noexcept override { return "vbox"; }
        bool FillCross() const noexcept { return fillCross_; }
        void SetFillCross(bool value) noexcept { fillCross_ = value; }
        void Arrange(const Rect& self, const std::vector<UiControl*>& children) override;
        Vec2 DesiredSize() const override;
    private:
        bool fillCross_ = true;
    };

    // Keeps every child exactly centred at its desired size.
    class CenterContainer final : public Container
    {
    public:
        explicit CenterContainer(ObjectCore& owner) : Container(owner) {}
        const char* TypeName() const noexcept override { return "center"; }
        void Arrange(const Rect& self, const std::vector<UiControl*>& children) override;
    };

    // Keeps a configurable margin around its (filled) children.
    class MarginContainer final : public Container
    {
    public:
        explicit MarginContainer(ObjectCore& owner) : Container(owner) {}
        const char* TypeName() const noexcept override { return "margin"; }
        void SetMargins(float left, float top, float right, float bottom) noexcept
        { left_ = left; top_ = top; right_ = right; bottom_ = bottom; }
        float Left() const noexcept { return left_; }
        float Top() const noexcept { return top_; }
        float Right() const noexcept { return right_; }
        float Bottom() const noexcept { return bottom_; }
        void Arrange(const Rect& self, const std::vector<UiControl*>& children) override;
    private:
        float left_ = 8, top_ = 8, right_ = 8, bottom_ = 8;
    };

    // A container that also paints a background sprite (optionally nine-sliced).
    class PanelContainer final : public Container
    {
    public:
        explicit PanelContainer(ObjectCore& owner) : Container(owner) {}
        const char* TypeName() const noexcept override { return "panel"; }
        const std::string& Texture() const noexcept { return texture_; }
        void SetTexture(std::string value) { texture_ = std::move(value); }
        Float4 Tint() const noexcept { return tint_; }
        void SetTint(Float4 value) noexcept { tint_ = value; }
        NineSlice Slice() const noexcept { return slice_; }
        void SetSlice(NineSlice value) noexcept { slice_ = value; }
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    private:
        std::string texture_;
        Float4 tint_{1, 1, 1, 1};
        NineSlice slice_{};
    };

    // ----- Leaf controls ---------------------------------------------------

    class Text : public UiControl
    {
    public:
        explicit Text(ObjectCore& owner) : UiControl(owner) { clickable_ = false; }
        const char* TypeName() const noexcept override { return "text"; }
        const std::string& Value() const noexcept { return text_; }
        void SetValue(std::string value) { text_ = std::move(value); }
        float PixelHeight() const noexcept { return pixelHeight_; }
        void SetPixelHeight(float value) noexcept { pixelHeight_ = std::max(1.0f, value); }
        Float4 Color() const noexcept { return color_; }
        void SetColor(Float4 value) noexcept { color_ = value; }
        Vec2 DesiredSize() const override;
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    protected:
        std::string text_;
        float pixelHeight_ = 16;
        Float4 color_{0.95f, 0.96f, 0.98f, 1};
    };

    // Multi-line text. Newlines in the value are honoured; the box does not wrap.
    class LongText final : public Text
    {
    public:
        explicit LongText(ObjectCore& owner) : Text(owner) {}
        const char* TypeName() const noexcept override { return "longText"; }
        Vec2 DesiredSize() const override;
    };

    // Editable single-line text. The UI system feeds it typed characters.
    class TextEntry final : public UiControl
    {
    public:
        explicit TextEntry(ObjectCore& owner) : UiControl(owner) { clickable_ = true; }
        const char* TypeName() const noexcept override { return "textEntry"; }
        const std::string& Value() const noexcept { return text_; }
        void SetValue(std::string value) { text_ = std::move(value); }
        const std::string& Placeholder() const noexcept { return placeholder_; }
        void SetPlaceholder(std::string value) { placeholder_ = std::move(value); }
        float PixelHeight() const noexcept { return pixelHeight_; }
        void SetPixelHeight(float value) noexcept { pixelHeight_ = std::max(1.0f, value); }
        bool Focused() const noexcept { return focused_; }
        void SetFocused(bool value) noexcept { focused_ = value; }
        // Applies a typed character (or 8 = backspace). Returns true if the text changed.
        bool Type(char32_t codepoint);
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    private:
        std::string text_;
        std::string placeholder_;
        float pixelHeight_ = 16;
        bool focused_ = false;
    };

    class TextureRect final : public UiControl
    {
    public:
        explicit TextureRect(ObjectCore& owner) : UiControl(owner) {}
        const char* TypeName() const noexcept override { return "textureRect"; }
        const std::string& Texture() const noexcept { return texture_; }
        void SetTexture(std::string value) { texture_ = std::move(value); }
        SpriteRegion Region() const noexcept { return region_; }
        void SetRegion(SpriteRegion value) noexcept { region_ = value; }
        Float4 Tint() const noexcept { return tint_; }
        void SetTint(Float4 value) noexcept { tint_ = value; }
        Vec2 DesiredSize() const override;
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    private:
        std::string texture_;
        SpriteRegion region_{};
        Float4 tint_{1, 1, 1, 1};
    };

    class ColorRect final : public UiControl
    {
    public:
        explicit ColorRect(ObjectCore& owner) : UiControl(owner) {}
        const char* TypeName() const noexcept override { return "colorRect"; }
        Float4 Color() const noexcept { return color_; }
        void SetColor(Float4 value) noexcept { color_ = value; }
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    private:
        Float4 color_{0.2f, 0.2f, 0.24f, 1};
    };

    class ProgressBar final : public UiControl
    {
    public:
        explicit ProgressBar(ObjectCore& owner) : UiControl(owner) {}
        const char* TypeName() const noexcept override { return "progressBar"; }
        float Value() const noexcept { return value_; }
        void SetValue(float value) noexcept { value_ = std::clamp(value, 0.0f, 1.0f); }
        bool Vertical() const noexcept { return vertical_; }
        void SetVertical(bool value) noexcept { vertical_ = value; }
        Float4 Background() const noexcept { return background_; }
        void SetBackground(Float4 value) noexcept { background_ = value; }
        Float4 Fill() const noexcept { return fill_; }
        void SetFill(Float4 value) noexcept { fill_ = value; }
        void Emit(std::vector<SpriteDraw>&, std::vector<TextDraw>&) const override;
    private:
        float value_ = 0.5f;
        bool vertical_ = false;
        Float4 background_{0.12f, 0.12f, 0.15f, 1};
        Float4 fill_{0.30f, 0.65f, 1.0f, 1};
    };
}
