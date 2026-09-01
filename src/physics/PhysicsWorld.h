#pragma once

#include "physics/PhysicsBehavior.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace zengine::physics {

enum class ContactPhase { Entered, Stayed, Exited };
struct ContactEvent {
    GameObjectId receiver = 0;
    GameObjectId other = 0;
    ContactPhase phase = ContactPhase::Entered;
    bool area = false;
};
struct RayHit {
    GameObjectId object = 0;
    Vec3 point{};
    float fraction = 0;
};

// Small engine-owned facade. Jolt types are intentionally absent from this API.
class World final {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    void Build(ObjectStore& objects);
    void Step(ObjectStore& objects, float delta);
    void AddForce(GameObjectId object, Vec3 force);
    void AddImpulse(GameObjectId object, Vec3 impulse);
    void AddTorque(GameObjectId object, Vec3 torque);
    void AddAngularImpulse(GameObjectId object, Vec3 impulse);
    void SetVelocity(GameObjectId object, Vec3 velocity);
    void SetAngularVelocity(GameObjectId object, Vec3 velocity);
    Vec3 Velocity(GameObjectId object) const;
    Vec3 AngularVelocity(GameObjectId object) const;
    std::vector<RayHit> Cast(Vec3 from, Vec3 to, std::uint32_t mask = 0xffffffffu) const;
    std::vector<ContactEvent> DrainEvents();
    bool Contains(GameObjectId object) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zengine::physics
