#pragma once

#include "core/GameObject.h"
#include <cstdint>

namespace zengine::physics {

enum class ColliderShape { Box, Sphere, Capsule };

class Collider final : public Behavior {
public:
    explicit Collider(GameObject& owner) : Behavior(owner) {}
    ColliderShape Shape() const noexcept { return shape_; }
    const Vec3& Offset() const noexcept { return offset_; }
    const Vec3& Size() const noexcept { return size_; }
    void SetShape(ColliderShape value) noexcept { shape_ = value; }
    void SetOffset(Vec3 value);
    void SetSize(Vec3 value);
private:
    ColliderShape shape_ = ColliderShape::Box;
    Vec3 offset_{};
    Vec3 size_{1, 1, 1};
};

class Body : public Behavior {
public:
    std::uint32_t Layer() const noexcept { return layer_; }
    std::uint32_t Mask() const noexcept { return mask_; }
    float Friction() const noexcept { return friction_; }
    float Bounciness() const noexcept { return bounciness_; }
    void SetLayer(std::uint32_t value) noexcept { layer_ = value; }
    void SetMask(std::uint32_t value) noexcept { mask_ = value; }
    void SetFriction(float value);
    void SetBounciness(float value);
protected:
    explicit Body(GameObject& owner) : Behavior(owner) {}
private:
    std::uint32_t layer_ = 1;
    std::uint32_t mask_ = 0xffffffffu;
    float friction_ = 0.5f;
    float bounciness_ = 0.0f;
};

class MovingBody : public Body {
public:
    const Vec3& Velocity() const noexcept { return velocity_; }
    const Vec3& AngularVelocity() const noexcept { return angularVelocity_; }
    const Vec3& ConstantForce() const noexcept { return constantForce_; }
    const Vec3& ConstantTorque() const noexcept { return constantTorque_; }
    void SetVelocity(Vec3 value);
    void SetAngularVelocity(Vec3 value);
    void SetConstantForce(Vec3 value);
    void SetConstantTorque(Vec3 value);
protected:
    explicit MovingBody(GameObject& owner) : Body(owner) {}
private:
    Vec3 velocity_{};
    Vec3 angularVelocity_{}; // radians per second
    Vec3 constantForce_{};
    Vec3 constantTorque_{};
};

class RigidBody final : public MovingBody {
public:
    explicit RigidBody(GameObject& owner) : MovingBody(owner) {}
    float Mass() const noexcept { return mass_; }
    float GravityScale() const noexcept { return gravityScale_; }
    void SetMass(float value);
    void SetGravityScale(float value);
private:
    float mass_ = 1.0f;
    float gravityScale_ = 1.0f;
};

class KinematicBody final : public MovingBody {
public:
    explicit KinematicBody(GameObject& owner) : MovingBody(owner) {}
};

class StaticBody final : public Body {
public:
    explicit StaticBody(GameObject& owner) : Body(owner) {}
};

class Area final : public Body {
public:
    explicit Area(GameObject& owner) : Body(owner) {}
};

} // namespace zengine::physics
