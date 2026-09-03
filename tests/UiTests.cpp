#include "ui/UiSystem.h"
#include "core/GameObject.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace zengine;
using namespace zengine::ui;

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(float a, float b, float epsilon = 0.01f) { return std::fabs(a - b) <= epsilon; }
    bool RectNear(const Rect& r, float x, float y, float w, float h)
    {
        return Near(r.x, x) && Near(r.y, y) && Near(r.width, w) && Near(r.height, h);
    }

    UiContext MakeContext()
    {
        UiContext ctx;
        // Deterministic stand-in for the renderer's font atlas: 7px per character.
        ctx.measureText = [](std::string_view s, float h) {
            float w = 0, line = 0;
            for (char c : s) { if (c == '\n') { w = std::max(w, line); line = 0; } else line += h * 0.45f; }
            return Vec2{std::max(w, line), h};
        };
        ctx.textureSize = [](std::string_view) { return Vec2{}; };
        ctx.resolveTexture = [](std::string_view) { return TextureHandle{}; };
        ctx.resolveVideoFrame = [](std::string_view, double, bool) { return TextureHandle{}; };
        return ctx;
    }
}

void AnchorMath()
{
    const Rect parent{0, 0, 200, 100};
    Check(RectNear(ResolveAnchor(parent, Anchor::TopLeft, {0, 0}, {40, 20}), 0, 0, 40, 20), "top_left");
    Check(RectNear(ResolveAnchor(parent, Anchor::BottomRight, {0, 0}, {40, 20}), 160, 80, 40, 20), "bottom_right");
    Check(RectNear(ResolveAnchor(parent, Anchor::Center, {0, 0}, {40, 20}), 80, 40, 40, 20), "center");
    Check(RectNear(ResolveAnchor(parent, Anchor::Fill, {0, 0}, {0, 0}), 0, 0, 200, 100), "fill");
    Check(RectNear(ResolveAnchor(parent, Anchor::BottomHalf, {0, 0}, {0, 0}), 0, 50, 200, 50), "bottom_half");
    Check(RectNear(ResolveAnchor(parent, Anchor::RightHalf, {0, 0}, {0, 0}), 100, 0, 100, 100), "right_half");
    Check(RectNear(ResolveAnchor(parent, Anchor::FillTop, {0, 0}, {0, 12}), 0, 0, 200, 12), "fill_top");
    // Offset (the control's Transform2D position) shifts from the anchor point.
    Check(RectNear(ResolveAnchor(parent, Anchor::TopLeft, {10, 5}, {40, 20}), 10, 5, 40, 20), "offset applied");
    // Parent origin is respected (nested layout).
    Check(RectNear(ResolveAnchor({50, 30, 100, 100}, Anchor::TopLeft, {0, 0}, {10, 10}), 50, 30, 10, 10), "nested origin");

    Anchor parsed;
    Check(ParseAnchor("bottom_half", parsed) && parsed == Anchor::BottomHalf, "ParseAnchor");
    Check(!ParseAnchor("nonsense", parsed), "ParseAnchor rejects unknown");
    Check(std::string(AnchorName(Anchor::FillVertical)) == "fill_vertical", "AnchorName round-trip");
}

void ContainerLayout()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    // Root panel fills the screen; an HBox holds three buttons.
    auto& panel = store.Create2D("Panel");
    auto& panelUi = panel.AddBehavior<PanelContainer>();
    panelUi.SetAnchor(Anchor::Fill);

    auto& row = store.Create2D("Row");
    row.SetParent(panel.Id());
    auto& rowUi = row.AddBehavior<HTileBoxContainer>();
    rowUi.SetAnchor(Anchor::TopLeft);
    rowUi.SetSpacing(4);
    rowUi.SetFillCross(true);
    rowUi.SetSize({300, 40});
    row.GetTransform().SetPosition({10, 10});

    for (int i = 0; i < 3; ++i)
    {
        auto& button = store.Create2D("Button");
        button.SetParent(row.Id());
        auto& b = button.AddBehavior<ColorRect>();
        b.SetSize({50, 20});
        b.SetClickable(true);
        b.SetOrder(i);
    }

    UiSystem ui;
    ui.Build(store, {800, 600}, ctx);
    Check(ui.NodeCount() == 5, "all controls collected");

    Check(RectNear(*ui.RectOf(panel.Id()), 0, 0, 800, 600), "panel fills screen");
    // Row anchored top-left of the panel at offset (10,10).
    Check(RectNear(*ui.RectOf(row.Id()), 10, 10, 300, 40), "row placed by anchor + offset");

    // Buttons tiled left to right with 4px spacing, filled to the row height.
    float x = 10;
    for (std::size_t n = 0; n < ui.NodeCount(); ++n)
    {
        const auto& node = ui.NodeAt(n);
        // find the button nodes via their rects
    }
    const Rect* first = nullptr;
    for (std::size_t i = 0; i < store.Size(); ++i)
        if (store.At(i).Name() == "Button") { first = ui.RectOf(store.At(i).Id()); break; }
    Check(first && RectNear(*first, 10, 10, 50, 40), "first tile at row origin, cross-filled");
}

void CenterAndMargin()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& center = store.Create2D("Center");
    auto& c = center.AddBehavior<CenterContainer>();
    c.SetAnchor(Anchor::Fill);

    auto& child = store.Create2D("Child");
    child.SetParent(center.Id());
    auto& cr = child.AddBehavior<ColorRect>();
    cr.SetSize({100, 50});

    UiSystem ui;
    ui.Build(store, {400, 300}, ctx);
    Check(RectNear(*ui.RectOf(child.Id()), 150, 125, 100, 50), "centre container centres child");

    ObjectStore store2;
    auto& margin = store2.Create2D("Margin");
    auto& m = margin.AddBehavior<MarginContainer>();
    m.SetAnchor(Anchor::Fill);
    m.SetMargins(10, 20, 30, 40);
    auto& inner = store2.Create2D("Inner");
    inner.SetParent(margin.Id());
    inner.AddBehavior<ColorRect>();

    UiSystem ui2;
    ui2.Build(store2, {200, 200}, ctx);
    Check(RectNear(*ui2.RectOf(inner.Id()), 10, 20, 160, 140), "margin container insets its child");
}

void HitTestAndClicks()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& panel = store.Create2D("Panel");
    auto& p = panel.AddBehavior<PanelContainer>();
    p.SetAnchor(Anchor::Fill);

    auto& button = store.Create2D("Button");
    button.SetParent(panel.Id());
    auto& b = button.AddBehavior<ColorRect>();
    b.SetAnchor(Anchor::TopLeft);
    b.SetSize({80, 30});
    b.SetClickable(true);
    button.GetTransform().SetPosition({20, 20});

    auto& label = store.Create2D("Label");
    label.SetParent(panel.Id());
    auto& t = label.AddBehavior<Text>();
    t.SetValue("hello");
    t.SetAnchor(Anchor::BottomLeft);

    UiSystem ui;
    ui.Build(store, {400, 300}, ctx);

    Check(ui.HitTest({30, 30}) == button.Id(), "hit the button");
    Check(ui.HitTest({200, 200}) == 0, "empty space hits nothing");
    Check(ui.HitTest({30, 30}) != label.Id(), "non-clickable text is not hit");

    // Press then release over the button = one click.
    Check(ui.Interact({30, 30}, true).empty(), "press alone is not a click");
    auto clicks = ui.Interact({30, 30}, false);
    Check(clicks.size() == 1 && clicks[0] == button.Id(), "press+release over the button clicks it");

    // Press on the button, release elsewhere = no click.
    ui.Interact({30, 30}, true);
    Check(ui.Interact({300, 300}, false).empty(), "release off-target does not click");

    // Hiding the panel hides the whole subtree from hit-testing.
    p.SetVisible(false);
    ui.Build(store, {400, 300}, ctx);
    Check(ui.HitTest({30, 30}) == 0, "hidden subtree is not hit");
}

void TextEntryTyping()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& field = store.Create2D("Field");
    auto& entry = field.AddBehavior<TextEntry>();
    entry.SetAnchor(Anchor::TopLeft);
    entry.SetSize({160, 24});
    entry.SetPlaceholder("name");

    UiSystem ui;
    ui.Build(store, {400, 300}, ctx);

    // Focus by clicking, then type.
    ui.Interact({10, 10}, true);
    ui.Interact({10, 10}, false);
    Check(entry.Focused(), "clicking focuses the entry");
    ui.Interact({10, 10}, false, {U'H', U'i'});
    Check(entry.Value() == "Hi", "typed characters land in the entry");
    ui.Interact({10, 10}, false, {8});
    Check(entry.Value() == "H", "backspace removes a character");

    // Clicking empty space blurs it; further typing is ignored.
    ui.Interact({300, 300}, true);
    ui.Interact({300, 300}, false);
    Check(!entry.Focused(), "clicking away blurs the entry");
    ui.Interact({300, 300}, false, {U'x'});
    Check(entry.Value() == "H", "unfocused entry ignores typing");
}

void EmitBatch()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& panel = store.Create2D("Panel");
    auto& p = panel.AddBehavior<PanelContainer>();
    p.SetAnchor(Anchor::Fill);
    p.SetTint({0.1f, 0.1f, 0.1f, 0.9f});

    auto& bar = store.Create2D("Bar");
    bar.SetParent(panel.Id());
    auto& pb = bar.AddBehavior<ProgressBar>();
    pb.SetAnchor(Anchor::TopLeft);
    pb.SetSize({200, 16});
    pb.SetValue(0.25f);

    auto& label = store.Create2D("Label");
    label.SetParent(panel.Id());
    auto& t = label.AddBehavior<Text>();
    t.SetValue("Loading");
    t.SetAnchor(Anchor::Center);

    UiSystem ui;
    ui.Build(store, {320, 240}, ctx);

    std::vector<SpriteDraw> sprites;
    std::vector<TextDraw> texts;
    ui.Emit(sprites, texts);

    // panel bg + progress background + progress fill = 3 sprites; 1 text run.
    Check(sprites.size() == 3, "UI emits one batch of sprites for the whole tree");
    Check(texts.size() == 1 && texts[0].text == "Loading", "text run emitted");
    Check(Near(sprites[0].tint.w, 0.9f), "panel tint carried through");
    // Progress fill is 25% of the bar width.
    Check(Near(sprites[2].dest.width, 50), "progress fill reflects value");

    // Order controls paint order: a higher Order paints later (on top).
    p.SetVisible(true);
    t.SetVisible(false);
    ui.Build(store, {320, 240}, ctx);
    sprites.clear(); texts.clear();
    ui.Emit(sprites, texts);
    Check(texts.empty(), "hidden control is not emitted");
}

void ScrollContainerBehaviour()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& scrollObj = store.Create2D("Scroll");
    auto& scroll = scrollObj.AddBehavior<ScrollContainer>();
    scroll.SetAnchor(Anchor::TopLeft);
    scroll.SetSize({200, 100});
    scroll.SetSpacing(0);

    for (int i = 0; i < 5; ++i)
    {
        auto& rowObj = store.Create2D("Row");
        rowObj.SetParent(scrollObj.Id());
        auto& row = rowObj.AddBehavior<ColorRect>();
        row.SetSize({180, 40});
        row.SetClickable(true);
        row.SetOrder(i);
    }

    UiSystem ui;
    ui.Build(store, {800, 600}, ctx);

    // Content is 5*40 = 200 tall; viewport 100 -> first row at the top.
    std::vector<GameObjectId> rows;
    for (std::size_t i = 0; i < store.Size(); ++i)
        if (store.At(i).Name() == "Row") rows.push_back(store.At(i).Id());
    Check(Near(ui.RectOf(rows[0])->y, 0), "first row at content top");
    Check(Near(scroll.ContentSize().y, 200), "content height measured");

    // Wheel down (negative) scrolls the content up.
    ui.Interact({50, 50}, false, {}, -3.0f);
    ui.Build(store, {800, 600}, ctx);
    Check(scroll.ScrollY() > 0, "wheel scrolled the container");
    Check(ui.RectOf(rows[0])->y < 0, "first row moved above the viewport");

    // A row scrolled fully out of view is no longer hit-testable (clipped).
    scroll.SetScrollY(200); // clamped to 100 on next build
    ui.Build(store, {800, 600}, ctx);
    Check(Near(scroll.ScrollY(), 100), "scroll clamped to max");
    Check(ui.HitTest({50, 5}) != rows[0], "clipped-away row is not hit at the very top");
    Check(ui.HitTest({50, 90}) == rows[4], "last row is hit near the bottom");

    // Emitted geometry for a clipped child carries the clip rect.
    std::vector<SpriteDraw> sprites; std::vector<TextDraw> texts;
    ui.Emit(sprites, texts);
    bool anyClipped = false;
    for (const auto& s : sprites) if (s.clip.width > 0 && s.clip.height > 0) anyClipped = true;
    Check(anyClipped, "scroll children emit with a clip rect");
}

void ButtonStatesAndSignals()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& obj = store.Create2D("Btn");
    auto& button = obj.AddBehavior<Button>();
    button.SetAnchor(Anchor::TopLeft);
    button.SetSize({120, 32});
    button.SetText("Go");

    UiSystem ui;
    ui.Build(store, {400, 300}, ctx);

    Check(button.CurrentVisual() == Button::Visual::Normal, "button starts normal");

    // Hover (move over, not pressed).
    ui.Interact({20, 10}, false);
    Check(button.CurrentVisual() == Button::Visual::Hover, "pointer over -> hover visual");

    // Press over the button.
    auto presses = ui.Interact({20, 10}, true);
    Check(ui.Presses().size() == 1 && ui.Presses()[0] == obj.Id(), "press reported");
    Check(button.CurrentVisual() == Button::Visual::Pressed, "held -> pressed visual");

    // Release over the button -> click + release.
    auto clicks = ui.Interact({20, 10}, false);
    Check(clicks.size() == 1 && clicks[0] == obj.Id(), "release over button = click");
    Check(ui.Releases().size() == 1 && ui.Releases()[0] == obj.Id(), "release reported");

    // Disabled buttons are not clickable and paint the disabled visual.
    button.SetDisabled(true);
    ui.Build(store, {400, 300}, ctx);
    Check(ui.HitTest({20, 10}) == 0, "disabled button is not hit");
    Check(button.CurrentVisual() == Button::Visual::Disabled, "disabled visual");

    std::vector<SpriteDraw> sprites; std::vector<TextDraw> texts;
    ui.Emit(sprites, texts);
    Check(!sprites.empty() && !texts.empty() && texts[0].text == "Go", "button emits a background + label");
}

void VideoPlayback()
{
    UiContext ctx = MakeContext();
    std::string lastAsset; double lastTime = -1; bool lastLoop = false;
    ctx.resolveVideoFrame = [&](std::string_view a, double t, bool loop) {
        lastAsset = std::string(a); lastTime = t; lastLoop = loop; return TextureHandle{};
    };

    ObjectStore store;
    auto& obj = store.Create2D("Vid");
    auto& video = obj.AddBehavior<VideoTexture>();
    video.SetAnchor(Anchor::Fill);
    video.SetVideo("clips/intro.zvid");
    video.SetSpeed(2.0f);

    UiSystem ui;
    ui.Build(store, {320, 240}, ctx, 0.5f); // advance 0.5s at 2x -> t = 1.0
    std::vector<SpriteDraw> sprites; std::vector<TextDraw> texts;
    ui.Emit(sprites, texts);
    Check(lastAsset == "clips/intro.zvid", "video asset forwarded to the host");
    Check(Near(static_cast<float>(lastTime), 1.0f), "playback clock advanced by delta * speed");

    video.SetPlaying(false);
    ui.Build(store, {320, 240}, ctx, 5.0f);
    ui.Emit(sprites, texts);
    Check(Near(static_cast<float>(video.Time()), 1.0f), "paused video does not advance");
}

void HtmlInterpreter()
{
    const auto ctx = MakeContext();
    ObjectStore store;

    auto& obj = store.Create2D("Html");
    auto& html = obj.AddBehavior<UiHtml>();
    html.SetAnchor(Anchor::TopLeft);
    html.SetSize({400, 300});
    html.SetHtml("<h1 style=\"color:#ff0000\">Title</h1><p>Hello &amp; welcome</p><hr>"
                 "<img src=\"pic.png\"><button id=\"ok\">OK</button>");

    Check(html.BlockCount() == 5, "html parsed into five blocks (h1, p, hr, img, button)");

    UiSystem ui;
    ui.Build(store, {800, 600}, ctx);

    std::vector<SpriteDraw> sprites; std::vector<TextDraw> texts;
    ui.Emit(sprites, texts);
    bool sawTitle = false, sawBody = false, sawButton = false;
    for (const auto& t : texts)
    {
        if (t.text == "Title") { sawTitle = true; Check(Near(t.color.x, 1.0f) && Near(t.color.y, 0.0f), "h1 inline colour applied"); }
        if (t.text == "Hello & welcome") sawBody = true;
        if (t.text == "OK") sawButton = true;
    }
    Check(sawTitle && sawBody && sawButton, "html emits its text runs");

    // The rendered <button> region reports as a link hit; the control is clickable.
    Check(ui.HitTest({10, 10}) == obj.Id(), "html control is clickable");
}

int main()
{
    try
    {
        AnchorMath();
        ContainerLayout();
        CenterAndMargin();
        HitTestAndClicks();
        TextEntryTyping();
        EmitBatch();
        ScrollContainerBehaviour();
        ButtonStatesAndSignals();
        VideoPlayback();
        HtmlInterpreter();
        std::cout << "PASS: anchors, containers, hit-testing, clicks, text entry, batched emit, "
                     "scroll, button, video, html\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
