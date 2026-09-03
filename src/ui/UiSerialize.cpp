#include "ui/UiSerialize.h"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace zengine::ui
{
    namespace
    {
        std::string Vec2Text(Vec2 v)
        {
            std::ostringstream out; out.imbue(std::locale::classic());
            out << v.x << ' ' << v.y; return out.str();
        }
        std::string Vec4Text(Float4 v)
        {
            std::ostringstream out; out.imbue(std::locale::classic());
            out << v.x << ' ' << v.y << ' ' << v.z << ' ' << v.w; return out.str();
        }
        std::string FloatText(float v)
        {
            std::ostringstream out; out.imbue(std::locale::classic()); out << v; return out.str();
        }
        Vec2 ParseVec2(std::string_view text)
        {
            std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
            Vec2 v{};
            if (!(in >> v.x >> v.y) || !std::isfinite(v.x) || !std::isfinite(v.y))
                throw std::invalid_argument("Expected two finite numbers.");
            return v;
        }
        Float4 ParseVec4(std::string_view text)
        {
            std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
            Float4 v{};
            if (!(in >> v.x >> v.y >> v.z >> v.w))
                throw std::invalid_argument("Expected four numbers.");
            return v;
        }
        float ParseFloat(std::string_view text)
        {
            std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
            float v = 0;
            if (!(in >> v) || !std::isfinite(v)) throw std::invalid_argument("Expected a finite number.");
            return v;
        }
        int ParseInt(std::string_view text)
        {
            std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
            int v = 0;
            if (!(in >> v)) throw std::invalid_argument("Expected an integer.");
            return v;
        }
        bool ParseBool(std::string_view text) { return text == "1" || text == "true"; }
    }

    const std::vector<std::string>& UiControlTypes()
    {
        static const std::vector<std::string> types = {
            "control", "container", "hbox", "vbox", "center", "margin", "panel",
            "text", "longText", "textEntry", "textureRect", "colorRect", "progressBar",
            "scroll", "button", "video", "html",
        };
        return types;
    }

    bool IsUiControlType(std::string_view type)
    {
        for (const auto& known : UiControlTypes()) if (known == type) return true;
        return false;
    }

    UiControl& AddUiControl(ObjectCore& owner, std::string_view type)
    {
        if (type == "control")      return owner.AddBehavior<UiControl>();
        if (type == "container")    return owner.AddBehavior<Container>();
        if (type == "hbox")         return owner.AddBehavior<HTileBoxContainer>();
        if (type == "vbox")         return owner.AddBehavior<VTileBoxContainer>();
        if (type == "center")       return owner.AddBehavior<CenterContainer>();
        if (type == "margin")       return owner.AddBehavior<MarginContainer>();
        if (type == "panel")        return owner.AddBehavior<PanelContainer>();
        if (type == "text")         return owner.AddBehavior<Text>();
        if (type == "longText")     return owner.AddBehavior<LongText>();
        if (type == "textEntry")    return owner.AddBehavior<TextEntry>();
        if (type == "textureRect")  return owner.AddBehavior<TextureRect>();
        if (type == "colorRect")    return owner.AddBehavior<ColorRect>();
        if (type == "progressBar")  return owner.AddBehavior<ProgressBar>();
        if (type == "scroll")       return owner.AddBehavior<ScrollContainer>();
        if (type == "button")       return owner.AddBehavior<Button>();
        if (type == "video")        return owner.AddBehavior<VideoTexture>();
        if (type == "html")         return owner.AddBehavior<UiHtml>();
        throw std::invalid_argument("Unknown UI control type '" + std::string(type) + "'.");
    }

    std::vector<std::pair<std::string, std::string>> SaveUiControl(const UiControl& control)
    {
        std::vector<std::pair<std::string, std::string>> props;
        const auto add = [&](std::string key, std::string value) { props.emplace_back(std::move(key), std::move(value)); };

        add("anchor", AnchorName(control.GetAnchor()));
        add("size", Vec2Text(control.Size()));
        add("min_size", Vec2Text(control.MinSize()));
        add("order", std::to_string(control.Order()));
        add("visible", control.Visible() ? "1" : "0");
        add("clickable", control.Clickable() ? "1" : "0");

        if (const auto* c = dynamic_cast<const Container*>(&control))
        {
            add("padding", FloatText(c->Padding()));
            add("spacing", FloatText(c->Spacing()));
        }
        if (const auto* c = dynamic_cast<const HTileBoxContainer*>(&control)) add("fill_cross", c->FillCross() ? "1" : "0");
        if (const auto* c = dynamic_cast<const VTileBoxContainer*>(&control)) add("fill_cross", c->FillCross() ? "1" : "0");
        if (const auto* c = dynamic_cast<const MarginContainer*>(&control))
            add("margins", FloatText(c->Left()) + " " + FloatText(c->Top()) + " " + FloatText(c->Right()) + " " + FloatText(c->Bottom()));
        if (const auto* c = dynamic_cast<const PanelContainer*>(&control))
        {
            add("texture", c->Texture());
            add("tint", Vec4Text(c->Tint()));
            const NineSlice s = c->Slice();
            add("slice", FloatText(s.left) + " " + FloatText(s.top) + " " + FloatText(s.right) + " " + FloatText(s.bottom));
        }
        if (const auto* c = dynamic_cast<const Text*>(&control))
        {
            add("text", c->Value());
            add("pixel_height", FloatText(c->PixelHeight()));
            add("color", Vec4Text(c->Color()));
        }
        if (const auto* c = dynamic_cast<const TextEntry*>(&control))
        {
            add("text", c->Value());
            add("placeholder", c->Placeholder());
            add("pixel_height", FloatText(c->PixelHeight()));
        }
        if (const auto* c = dynamic_cast<const TextureRect*>(&control))
        {
            add("texture", c->Texture());
            const SpriteRegion r = c->Region();
            add("region", FloatText(r.u0) + " " + FloatText(r.v0) + " " + FloatText(r.u1) + " " + FloatText(r.v1));
            add("tint", Vec4Text(c->Tint()));
        }
        if (const auto* c = dynamic_cast<const ColorRect*>(&control)) add("color", Vec4Text(c->Color()));
        if (const auto* c = dynamic_cast<const ProgressBar*>(&control))
        {
            add("value", FloatText(c->Value()));
            add("vertical", c->Vertical() ? "1" : "0");
            add("fill_color", Vec4Text(c->Fill()));
            add("background_color", Vec4Text(c->Background()));
        }
        if (const auto* c = dynamic_cast<const ScrollContainer*>(&control))
        {
            add("scroll_x", FloatText(c->ScrollX()));
            add("scroll_y", FloatText(c->ScrollY()));
            add("horizontal", c->Horizontal() ? "1" : "0");
            add("fill_cross", c->FillCross() ? "1" : "0");
        }
        if (const auto* c = dynamic_cast<const Button*>(&control))
        {
            add("text", c->Text());
            add("pixel_height", FloatText(c->PixelHeight()));
            add("disabled", c->Disabled() ? "1" : "0");
            add("normal_color", Vec4Text(c->NormalColor()));
            add("hover_color", Vec4Text(c->HoverColor()));
            add("pressed_color", Vec4Text(c->PressedColor()));
            add("disabled_color", Vec4Text(c->DisabledColor()));
            add("text_color", Vec4Text(c->TextColor()));
            add("normal_texture", c->NormalTexture());
            add("hover_texture", c->HoverTexture());
            add("pressed_texture", c->PressedTexture());
            const NineSlice s = c->Slice();
            add("slice", FloatText(s.left) + " " + FloatText(s.top) + " " + FloatText(s.right) + " " + FloatText(s.bottom));
        }
        if (const auto* c = dynamic_cast<const VideoTexture*>(&control))
        {
            add("video", c->Video());
            add("playing", c->Playing() ? "1" : "0");
            add("loop", c->Loop() ? "1" : "0");
            add("speed", FloatText(c->Speed()));
            add("tint", Vec4Text(c->Tint()));
        }
        if (const auto* c = dynamic_cast<const UiHtml*>(&control))
        {
            add("html", c->Html());
            add("background", Vec4Text(c->Background()));
        }
        return props;
    }

    void LoadUiProperty(UiControl& control, std::string_view key, std::string_view value)
    {
        if (key == "anchor")
        {
            Anchor anchor;
            if (!ParseAnchor(value, anchor)) throw std::invalid_argument("Unknown anchor '" + std::string(value) + "'.");
            control.SetAnchor(anchor);
        }
        else if (key == "size") control.SetSize(ParseVec2(value));
        else if (key == "min_size") control.SetMinSize(ParseVec2(value));
        else if (key == "order") control.SetOrder(ParseInt(value));
        else if (key == "visible") control.SetVisible(ParseBool(value));
        else if (key == "clickable") control.SetClickable(ParseBool(value));
        else if (key == "padding") { if (auto* c = dynamic_cast<Container*>(&control)) c->SetPadding(ParseFloat(value)); }
        else if (key == "spacing") { if (auto* c = dynamic_cast<Container*>(&control)) c->SetSpacing(ParseFloat(value)); }
        else if (key == "fill_cross")
        {
            if (auto* h = dynamic_cast<HTileBoxContainer*>(&control)) h->SetFillCross(ParseBool(value));
            else if (auto* v = dynamic_cast<VTileBoxContainer*>(&control)) v->SetFillCross(ParseBool(value));
            else if (auto* s = dynamic_cast<ScrollContainer*>(&control)) s->SetFillCross(ParseBool(value));
        }
        else if (key == "scroll_x") { if (auto* s = dynamic_cast<ScrollContainer*>(&control)) s->SetScrollX(ParseFloat(value)); }
        else if (key == "scroll_y") { if (auto* s = dynamic_cast<ScrollContainer*>(&control)) s->SetScrollY(ParseFloat(value)); }
        else if (key == "horizontal") { if (auto* s = dynamic_cast<ScrollContainer*>(&control)) s->SetHorizontal(ParseBool(value)); }
        else if (key == "margins")
        {
            if (auto* c = dynamic_cast<MarginContainer*>(&control))
            {
                const Float4 m = ParseVec4(value);
                c->SetMargins(m.x, m.y, m.z, m.w);
            }
        }
        else if (key == "texture")
        {
            if (auto* p = dynamic_cast<PanelContainer*>(&control)) p->SetTexture(std::string(value));
            else if (auto* t = dynamic_cast<TextureRect*>(&control)) t->SetTexture(std::string(value));
        }
        else if (key == "tint")
        {
            if (auto* p = dynamic_cast<PanelContainer*>(&control)) p->SetTint(ParseVec4(value));
            else if (auto* t = dynamic_cast<TextureRect*>(&control)) t->SetTint(ParseVec4(value));
            else if (auto* v = dynamic_cast<VideoTexture*>(&control)) v->SetTint(ParseVec4(value));
        }
        else if (key == "slice")
        {
            const Float4 s = ParseVec4(value);
            if (auto* p = dynamic_cast<PanelContainer*>(&control)) p->SetSlice({s.x, s.y, s.z, s.w});
            else if (auto* b = dynamic_cast<Button*>(&control)) b->SetSlice({s.x, s.y, s.z, s.w});
        }
        else if (key == "disabled") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetDisabled(ParseBool(value)); }
        else if (key == "normal_color") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetNormalColor(ParseVec4(value)); }
        else if (key == "hover_color") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetHoverColor(ParseVec4(value)); }
        else if (key == "pressed_color") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetPressedColor(ParseVec4(value)); }
        else if (key == "disabled_color") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetDisabledColor(ParseVec4(value)); }
        else if (key == "text_color") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetTextColor(ParseVec4(value)); }
        else if (key == "normal_texture") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetNormalTexture(std::string(value)); }
        else if (key == "hover_texture") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetHoverTexture(std::string(value)); }
        else if (key == "pressed_texture") { if (auto* b = dynamic_cast<Button*>(&control)) b->SetPressedTexture(std::string(value)); }
        else if (key == "video") { if (auto* v = dynamic_cast<VideoTexture*>(&control)) v->SetVideo(std::string(value)); }
        else if (key == "playing") { if (auto* v = dynamic_cast<VideoTexture*>(&control)) v->SetPlaying(ParseBool(value)); }
        else if (key == "loop") { if (auto* v = dynamic_cast<VideoTexture*>(&control)) v->SetLoop(ParseBool(value)); }
        else if (key == "speed") { if (auto* v = dynamic_cast<VideoTexture*>(&control)) v->SetSpeed(ParseFloat(value)); }
        else if (key == "html") { if (auto* h = dynamic_cast<UiHtml*>(&control)) h->SetHtml(std::string(value)); }
        else if (key == "background") { if (auto* h = dynamic_cast<UiHtml*>(&control)) h->SetBackground(ParseVec4(value)); }
        else if (key == "text")
        {
            if (auto* t = dynamic_cast<Text*>(&control)) t->SetValue(std::string(value));
            else if (auto* e = dynamic_cast<TextEntry*>(&control)) e->SetValue(std::string(value));
            else if (auto* b = dynamic_cast<Button*>(&control)) b->SetText(std::string(value));
        }
        else if (key == "placeholder") { if (auto* e = dynamic_cast<TextEntry*>(&control)) e->SetPlaceholder(std::string(value)); }
        else if (key == "pixel_height")
        {
            if (auto* t = dynamic_cast<Text*>(&control)) t->SetPixelHeight(ParseFloat(value));
            else if (auto* e = dynamic_cast<TextEntry*>(&control)) e->SetPixelHeight(ParseFloat(value));
            else if (auto* b = dynamic_cast<Button*>(&control)) b->SetPixelHeight(ParseFloat(value));
        }
        else if (key == "color")
        {
            if (auto* t = dynamic_cast<Text*>(&control)) t->SetColor(ParseVec4(value));
            else if (auto* r = dynamic_cast<ColorRect*>(&control)) r->SetColor(ParseVec4(value));
        }
        else if (key == "region") { if (auto* t = dynamic_cast<TextureRect*>(&control)) { const Float4 r = ParseVec4(value); t->SetRegion({r.x, r.y, r.z, r.w}); } }
        else if (key == "value") { if (auto* b = dynamic_cast<ProgressBar*>(&control)) b->SetValue(ParseFloat(value)); }
        else if (key == "vertical") { if (auto* b = dynamic_cast<ProgressBar*>(&control)) b->SetVertical(ParseBool(value)); }
        else if (key == "fill_color") { if (auto* b = dynamic_cast<ProgressBar*>(&control)) b->SetFill(ParseVec4(value)); }
        else if (key == "background_color") { if (auto* b = dynamic_cast<ProgressBar*>(&control)) b->SetBackground(ParseVec4(value)); }
        // Unknown keys are ignored for forward compatibility.
    }
}
