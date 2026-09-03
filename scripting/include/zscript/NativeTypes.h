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
        // UI controls (ZE-61): a native class chain rooted at gameObject2D. Not
        // drag-assignable components - a script inherits one, it is not referenced.
        {"uiControl",     "gameObject2D", "", false, false, true },
        {"uiContainer",   "uiControl",    "", false, false, true },
        {"uiHTileBox",    "uiContainer",  "", false, false, true },
        {"uiVTileBox",    "uiContainer",  "", false, false, true },
        {"uiCenterBox",   "uiContainer",  "", false, false, true },
        {"uiMarginBox",   "uiContainer",  "", false, false, true },
        {"uiPanel",       "uiContainer",  "", false, false, true },
        {"uiText",        "uiControl",    "", false, false, true },
        {"uiLongText",    "uiText",       "", false, false, true },
        {"uiTextEntry",   "uiControl",    "", false, false, true },
        {"uiTextureRect", "uiControl",    "", false, false, true },
        {"uiColorRect",   "uiControl",    "", false, false, true },
        {"uiProgressBar", "uiControl",    "", false, false, true },
        {"uiScroll",      "uiContainer",  "", false, false, true },
        {"uiButton",      "uiControl",    "", false, false, true },
        {"uiVideo",       "uiControl",    "", false, false, true },
        {"uiHtml",        "uiControl",    "", false, false, true },
        {"Behavior",       "gameObject",  "",               false, false, true },
        {"PhysicsBody",    "Behavior",    "physics",        false, false, true },
        {"RigidBody",      "PhysicsBody", "rigidbody",      true,  true,  true },
        {"KinematicBody",  "PhysicsBody", "kinematic_body", true,  true,  true },
        {"StaticBody",     "PhysicsBody", "static_body",    true,  true,  true },
        {"Area",           "PhysicsBody", "area",           true,  true,  true },
        {"Collider",       "Behavior",    "collider",       true,  false, true },
        {"Camera",         "Behavior",    "camera",         true,  false, true },
        {"Timer",          "gameObject",  "",               false, false, true },
        {"audioPlayer",    "gameObject",  "",               false, false, true },
        {"audioArea",      "gameObject",  "",               false, false, true },
        {"lightSource",    "gameObject",  "",               false, false, true },
        {"prefab",         "",            "",               false, false, true },
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
