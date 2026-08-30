#include "BehaviorLifecycle.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

std::vector<zengine::Behavior*> zengine::BehaviorLifecycle::Ordered(ObjectStore& objects)
{
    struct Entry { Behavior* behavior; float priority; };
    std::vector<Entry> entries;
    for (std::size_t i=0; i<objects.Size(); ++i)
    {
        auto& object=objects.At(i);
        for (std::size_t j=0; j<object.BehaviorCount(); ++j)
        {
            auto& behavior=object.BehaviorAt(j);
            entries.push_back({&behavior,behavior.Priority()});
        }
    }
    std::shuffle(entries.begin(),entries.end(),random_);
    std::stable_sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b) { return a.priority>b.priority; });
    std::vector<Behavior*> result;
    result.reserve(entries.size());
    for (const auto& entry:entries) result.push_back(entry.behavior);
    return result;
}
void zengine::BehaviorLifecycle::Tick(ObjectStore& objects, float delta)
{
    if (!std::isfinite(delta) || delta<0) throw std::invalid_argument("Tick delta must be finite and nonnegative.");
    for (auto* behavior:Ordered(objects)) behavior->Tick(delta);
}
void zengine::BehaviorLifecycle::Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& hasDraw)
{
    for (auto* behavior:Ordered(objects))
        if (hasDraw(behavior->Owner().Id())) behavior->Draw();
}
