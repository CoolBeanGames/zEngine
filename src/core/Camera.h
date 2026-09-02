#pragma once
#include "GameObject.h"
#include <algorithm>

namespace zengine
{
    // Native behavior data only. The renderer turns a main Camera into the game view;
    // the editor draws its frustum. "Main" is driven by the owner's "main" tag, of which
    // only one camera may hold at a time (EditorShell enforces this).
    class Camera final : public Behavior
    {
    public:
        static constexpr const char* MainTag = "main";
        explicit Camera(GameObject& owner) : Behavior(owner) {}
        float FieldOfView() const noexcept { return fieldOfView_; }
        void SetFieldOfView(float degrees) { fieldOfView_ = std::clamp(degrees, 1.0f, 179.0f); }
        float NearPlane() const noexcept { return nearPlane_; }
        void SetNearPlane(float value) { nearPlane_ = std::clamp(value, 0.001f, 1.0e6f); }
        float FarPlane() const noexcept { return farPlane_; }
        void SetFarPlane(float value) { farPlane_ = std::clamp(value, nearPlane_ + 0.01f, 1.0e7f); }
        bool IsMain() const { return Owner().HasTag(MainTag); }
    private:
        float fieldOfView_ = 60.0f;
        float nearPlane_ = 0.1f;
        float farPlane_ = 1000.0f;
    };
}
