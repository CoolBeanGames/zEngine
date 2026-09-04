#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zengine::script {

// The single registry ("phone book") of native types the scripting system knows about.
//
// Everything that needs to reason about a native type - the compiler's canonical-name
// resolver, the built-in class prelude, the runtime component binding, script
// autocomplete, and the host's Inspector reference/bind tables - reads from this one
// list. To teach the engine a brand-new referenceable component (say an audio_player),
// add one row here, one row in ScriptHost's native binding table, and write the C++
// behavior class; autocomplete, drag-assignment and serialization then follow.
struct NativeType {
    std::string_view name;       // canonical spelling, e.g. "RigidBody"
    std::string_view base;       // script base class: "", "gameObject", "Behavior", "PhysicsBody"
    std::string_view accessor;   // read-only gameObject.<accessor> field; "" when there is none
    bool component = false;      // attachable native Behavior: drag-assignable, BindNativeBehavior
    bool physicsBody = false;    // uses the PhysicsBody owner / "physics" accessor wiring
    bool scriptClass = true;     // registered in Program.classes (false only for the Vector3 value type)
};

inline const std::vector<NativeType>& NativeTypes() {
    static const std::vector<NativeType> types = {
        {"gameObject",     "",            "",               false, false, true },
        {"gameObject2D",   "",            "",               false, false, true },
        {"Transform",      "",            "transform",      false, false, true },
        {"Transform2D",    "",            "",               false, false, true },
        // UI controls (ZE-61): a native class chain rooted at gameObject2D. A script
        // both inherits one (class Menu : uiPanel) and, since ZE-87, references one -
        // "export uiButton ok;" binds a scene object carrying that control, resolved
        // through the shared gameObject2D.ui_control accessor.
        {"uiControl",     "gameObject2D", "ui_control", true, false, true },
        {"uiContainer",   "uiControl",    "ui_control", true, false, true },
        {"uiHTileBox",    "uiContainer",  "ui_control", true, false, true },
        {"uiVTileBox",    "uiContainer",  "ui_control", true, false, true },
        {"uiCenterBox",   "uiContainer",  "ui_control", true, false, true },
        {"uiMarginBox",   "uiContainer",  "ui_control", true, false, true },
        {"uiPanel",       "uiContainer",  "ui_control", true, false, true },
        {"uiText",        "uiControl",    "ui_control", true, false, true },
        {"uiLongText",    "uiText",       "ui_control", true, false, true },
        {"uiTextEntry",   "uiControl",    "ui_control", true, false, true },
        {"uiTextureRect", "uiControl",    "ui_control", true, false, true },
        {"uiColorRect",   "uiControl",    "ui_control", true, false, true },
        {"uiProgressBar", "uiControl",    "ui_control", true, false, true },
        {"uiScroll",      "uiContainer",  "ui_control", true, false, true },
        {"uiButton",      "uiControl",    "ui_control", true, false, true },
        {"uiVideo",       "uiControl",    "ui_control", true, false, true },
        {"uiHtml",        "uiControl",    "ui_control", true, false, true },
        {"Behavior",       "gameObject",  "",               false, false, true },
        {"PhysicsBody",    "Behavior",    "physics",        false, false, true },
        {"RigidBody",      "PhysicsBody", "rigidbody",      true,  true,  true },
        {"KinematicBody",  "PhysicsBody", "kinematic_body", true,  true,  true },
        {"StaticBody",     "PhysicsBody", "static_body",    true,  true,  true },
        {"Area",           "PhysicsBody", "area",           true,  true,  true },
        {"Collider",       "Behavior",    "collider",       true,  false, true },
        {"Camera",         "Behavior",    "camera",         true,  false, true },
        {"Timer",          "gameObject",  "",               false, false, true },
        // ZE-116: also attachable/drag-assignable components - an "export audioPlayer x"
        // binds any object that has the matching behavior (AudioSource / Light / Decal /
        // AudioEffect), resolving through the accessor field.
        {"audioPlayer",    "gameObject",  "audio_player",    true,  false, true },
        {"audioArea",      "gameObject",  "audio_area",      true,  false, true },
        {"lightSource",    "gameObject",  "light_source",    true,  false, true },
        {"decalProjector", "gameObject",  "decal_projector", true,  false, true },
        {"prefab",         "",            "",               false, false, true },
        {"data_sheet",     "",            "",               false, false, true }, // ZE-92: a table of data objects, read sheet[row, column]
        {"InputService",   "",            "",               false, false, true },
        {"InputAction",    "",            "",               false, false, true },
        {"Mouse",          "",            "",               false, false, true },
        {"PhysicsService", "",            "",               false, false, true },
        {"SceneService",   "",            "",               false, false, true },
        {"Mathf",          "",            "",               false, false, true },
        {"Vector3",        "",            "",               false, false, false},
        {"Vector2",        "",            "",               false, false, false},
    };
    return types;
}

inline const NativeType* FindNativeType(std::string_view name) {
    for (const auto& type : NativeTypes())
        if (type.name == name) return &type;
    return nullptr;
}

} // namespace zengine::script
