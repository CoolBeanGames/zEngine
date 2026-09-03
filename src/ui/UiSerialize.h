#pragma once

#include "ui/UiControl.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zengine::ui
{
    // Scene / prefab serialization support. A UiControl is stored as a type name
    // (UiControl::TypeName()) plus an ordered list of "key value" property strings;
    // the value payload is opaque to the scene codec (which just quotes it).

    // The 13 control type names, for editor menus and Decode validation.
    const std::vector<std::string>& UiControlTypes();

    bool IsUiControlType(std::string_view type);

    // Adds the matching UiControl behavior to `owner` and returns it. Throws for an
    // unknown type name.
    UiControl& AddUiControl(ObjectCore& owner, std::string_view type);

    // Every persisted property of `control`, in a stable order, as (key, value).
    std::vector<std::pair<std::string, std::string>> SaveUiControl(const UiControl& control);

    // Applies one property. Unknown keys are ignored (forward compatibility).
    // Throws std::invalid_argument on a malformed value.
    void LoadUiProperty(UiControl& control, std::string_view key, std::string_view value);

    // ----- Inspector schema ----------------------------------------------
    // How an editor should present one property. Values are still exchanged as
    // the plain strings SaveUiControl / LoadUiProperty use.
    enum class UiPropertyKind
    {
        Line,       // single-line text / asset name
        Multiline,  // multi-line text (HTML markup)
        Float,
        Int,
        Bool,       // "0"/"1"; present as a two-item choice
        Vec2,       // two space-separated floats
        Color,      // four space-separated floats (RGBA / region / margins / slice)
        Anchor,     // one of AnchorName()
    };

    struct UiPropertyField
    {
        const char* key;
        const char* label;
        UiPropertyKind kind;
    };

    // The editable properties of `control`, in display order (base fields first,
    // then the concrete type's own fields). Read the current value from
    // SaveUiControl(); write a new one with LoadUiProperty(control, key, value).
    std::vector<UiPropertyField> UiControlSchema(const UiControl& control);
}
