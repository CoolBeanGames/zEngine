#include "physics/PhysicsBehavior.h"
#include <cmath>
#include <stdexcept>

namespace zengine::physics {
namespace {
void Finite(Vec3 v) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
        throw std::invalid_argument("Physics vector values must be finite.");
}
}
void Body::SetFriction(float value) {
    if (!std::isfinite(value) || value < 0) throw std::invalid_argument("Friction must be finite and nonnegative.");
    friction_ = value;
}
void Body::SetBounciness(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Bounciness must be between 0 and 1.");
    bounciness_ = value;
}
void MovingBody::SetVelocity(Vec3 value) { Finite(value); velocity_ = value; }
void MovingBody::SetAngularVelocity(Vec3 value) { Finite(value); angularVelocity_ = value; }
void MovingBody::SetConstantForce(Vec3 value) { Finite(value); constantForce_ = value; }
void MovingBody::SetConstantTorque(Vec3 value) { Finite(value); constantTorque_ = value; }
void RigidBody::SetMass(float value) {
    if (!std::isfinite(value) || value <= 0) throw std::invalid_argument("Mass must be finite and greater than zero.");
    mass_ = value;
}
void RigidBody::SetGravityScale(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Gravity scale must be finite.");
    gravityScale_ = value;
}
} // namespace zengine::physics
