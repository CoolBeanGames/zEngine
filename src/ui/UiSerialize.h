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
}
