#pragma once
#include "GameObject.h"
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace zengine
{
    // A phase snapshots candidates and priority before invoking any user code. Objects and
    // behaviors added from a callback are safe and join scheduling in the following phase.
    //
    // ZE-78: the priority-ordered list, plus the filtered "has update / physics / draw
    // code" sublists, are cached and only rebuilt when the store's structure revision
    // changes (an object or behavior added/removed, re-prioritised, or a script bound).
    // A frame of a mostly-static scene then does no per-phase allocation, shuffle or sort,
    // and skips the no-op behaviors entirely.
    class BehaviorLifecycle final
    {
    public:
        explicit BehaviorLifecycle(std::uint32_t seed = std::random_device{}()) : random_(seed) {}
        const std::vector<Behavior*>& Ordered(ObjectStore& objects);
        void Tick(ObjectStore& objects, float delta);
        void PhysicsTick(ObjectStore& objects, float delta);
        void Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& hasDraw);
    private:
        void Sync(ObjectStore& objects);
        std::mt19937 random_;
        std::uint64_t builtRevision_ = 0; // 0 never matches ObjectStore's (starts at 1)
        std::vector<Behavior*> ordered_;
        std::vector<Behavior*> updates_;
        std::vector<Behavior*> physics_;
        std::vector<Behavior*> draws_;
    };
}
