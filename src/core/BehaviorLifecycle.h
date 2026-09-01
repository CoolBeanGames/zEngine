#pragma once
#include "GameObject.h"
#include <functional>
#include <random>

namespace zengine
{
    // A phase snapshots candidates and priority before invoking any user code. Objects and
    // behaviors added from a callback are safe and join scheduling in the following phase.
    class BehaviorLifecycle final
    {
    public:
        explicit BehaviorLifecycle(std::uint32_t seed = std::random_device{}()) : random_(seed) {}
        std::vector<Behavior*> Ordered(ObjectStore& objects);
        void Tick(ObjectStore& objects, float delta);
        void PhysicsTick(ObjectStore& objects, float delta);
        void Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& hasDraw);
    private:
        std::mt19937 random_;
    };
}
