#include "BehaviorLifecycle.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

void zengine::BehaviorLifecycle::Sync(ObjectStore& objects)
{
    if (objects.StructureRevision() == builtRevision_) return;
    builtRevision_ = objects.StructureRevision();

    struct Entry { Behavior* behavior; float priority; };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < objects.Size(); ++i)
    {
        auto& object = objects.At(i);
        for (std::size_t j = 0; j < object.BehaviorCount(); ++j)
        {
            auto& behavior = object.BehaviorAt(j);
            entries.push_back({&behavior, behavior.Priority()});
        }
    }
    // Randomise, then stable-sort by priority: equal-priority behaviors run in an
    // arbitrary (but, until the next rebuild, fixed) order so scripts cannot depend on it.
    std::shuffle(entries.begin(), entries.end(), random_);
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.priority > b.priority; });

    ordered_.clear(); updates_.clear(); physics_.clear(); draws_.clear();
    ordered_.reserve(entries.size());
    for (const auto& entry : entries)
    {
        Behavior* b = entry.behavior;
        ordered_.push_back(b);
        if (b->HasUpdate()) updates_.push_back(b);
        if (b->HasPhysicsUpdate()) physics_.push_back(b);
        if (b->HasDraw()) draws_.push_back(b);
    }
}

const std::vector<zengine::Behavior*>& zengine::BehaviorLifecycle::Ordered(ObjectStore& objects)
{
    Sync(objects);
    return ordered_;
}

void zengine::BehaviorLifecycle::Tick(ObjectStore& objects, float delta)
{
    if (!std::isfinite(delta) || delta < 0) throw std::invalid_argument("Tick delta must be finite and nonnegative.");
    Sync(objects);
    // A copy: user code may add/remove behaviors mid-phase (which invalidates the cache);
    // the running phase still schedules exactly the snapshot it started with.
    const auto snapshot = updates_;
    for (auto* behavior : snapshot) behavior->Tick(delta);
}
void zengine::BehaviorLifecycle::PhysicsTick(ObjectStore& objects, float delta)
{
    if (!std::isfinite(delta) || delta < 0) throw std::invalid_argument("Physics tick delta must be finite and nonnegative.");
    Sync(objects);
    const auto snapshot = physics_;
    for (auto* behavior : snapshot) behavior->PhysicsTick(delta);
}
void zengine::BehaviorLifecycle::Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& hasDraw)
{
    Sync(objects);
    const auto snapshot = draws_;
    for (auto* behavior : snapshot)
        if (hasDraw(behavior->Owner().Id())) behavior->Draw();
}
