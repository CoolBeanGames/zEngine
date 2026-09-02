#include "core/BehaviorLifecycle.h"
#include "core/ScriptBehavior.h"
#include <iostream>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

using namespace zengine;
void Check(bool value,const char* error) { if (!value) throw std::runtime_error(error); }
struct Probe final : Behavior
{
    Probe(zengine::ObjectCore& owner,std::vector<std::string>& events,std::string name,float priority=0)
        : Behavior(owner), events(events), name(std::move(name)) { SetPriority(priority); }
    std::vector<std::string>& events;
    std::string name;
    bool update=true, draw=true, fail=false;
    std::function<void()> action;
    bool HasStart() const noexcept override { return true; }
    bool HasUpdate() const noexcept override { return update; }
    bool HasDraw() const noexcept override { return draw; }
    void OnStart() override
    {
        Check(Owner().BehaviorCount()>0,"Start ran before ownership was established");
        events.push_back(name+"S");
        Tick(0); Draw(); // Reentrant update/draw must wait for Start to finish.
    }
    void OnUpdate(float delta) override
    {
        Check(delta==0.25f,"Delta was not forwarded");
        events.push_back(name+"U");
        if (fail) throw std::runtime_error("test failure");
        if (action) action();
    }
    void OnDraw() override { events.push_back(name+"D"); }
};
struct Empty final : Behavior
{
    explicit Empty(zengine::ObjectCore& owner):Behavior(owner) {}
    void OnStart() override { throw std::runtime_error("Empty Start executed"); }
    void OnUpdate(float) override { throw std::runtime_error("Empty Update executed"); }
    void OnDraw() override { throw std::runtime_error("Empty Draw executed"); }
};
struct FakeScript final : ScriptInstance
{
    explicit FakeScript(int& starts):starts(starts) {}
    int& starts;
    bool HasStart() const noexcept override { return true; }
    bool HasUpdate() const noexcept override { return false; }
    bool HasDraw() const noexcept override { return false; }
    void Start(zengine::ObjectCore&) override { ++starts; }
    void Update(zengine::ObjectCore&,float) override { throw std::runtime_error("Empty script update executed"); }
    void Draw(zengine::ObjectCore&) override { throw std::runtime_error("Empty script draw executed"); }
};
int main()
{
    try
    {
        ObjectStore objects;
        BehaviorLifecycle lifecycle(12345);
        std::vector<std::string> events;
        auto& a=objects.Create().AddBehavior<Probe>(events,"a",0.0f);
        auto& b=objects.Create().AddBehavior<Probe>(events,"b",1.5f);
        auto& c=objects.Create().AddBehavior<Probe>(events,"c",0.0f);
        auto& d=objects.Create().AddBehavior<Probe>(events,"d",-1.0f);
        Check(events==std::vector<std::string>({"aS","bS","cS","dS"}),"Start must run immediately once per construction");
        a.Instantiate(); Check(events.size()==4,"Start ran twice");
        std::set<std::string> ties;
        for (int i=0;i<64;++i)
        {
            events.clear(); lifecycle.Tick(objects,0.25f);
            Check(events.size()==4 && events.front()=="bU" && events.back()=="dU","Priority must be descending, including negatives");
            ties.insert(events[1]);
        }
        Check(ties.size()==2,"Equal priorities must be randomly reordered");
        bool rejected=false;
        try { a.SetPriority(std::numeric_limits<float>::quiet_NaN()); } catch (...) { rejected=true; }
        Check(rejected && a.Priority()==0,"Nonfinite priorities must be rejected");
        events.clear(); b.SetEnabled(false); a.update=false;
        lifecycle.Tick(objects,0.25f);
        Check(events==std::vector<std::string>({"cU","dU"}),"Disabled/empty update was not skipped");
        events.clear(); lifecycle.Draw(objects,[&](GameObjectId id) { return id==a.Owner().Id(); });
        Check(events==std::vector<std::string>({"aD"}),"Draw must be gated by owner's render submission");
        b.SetEnabled(true); a.update=true;
        b.action=[&]() { d.SetPriority(10); };
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(events.front()=="bU" && events.back()=="dU","Priority changes must not reorder an active phase");
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(events.front()=="dU","Priority changes must apply in the following phase");
        b.action={}; b.fail=true;
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(b.Faulted() && b.Error()=="test failure","Runtime errors must fault only that behavior");
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(events.size()==3,"Faulted behavior kept executing or stopped other behaviors");
        auto& empty=objects.Create().AddBehavior<Empty>();
        lifecycle.Tick(objects,0.25f); lifecycle.Draw(objects,[](GameObjectId) { return true; });
        Check(!empty.Faulted(),"Empty lifecycle hook called");
        int starts=0;
        auto& script=objects.Create().AddBehavior<ScriptBehavior>("Test.zsh");
        script.BindInstance(std::make_unique<FakeScript>(starts));
        script.SetEnabled(false); script.SetEnabled(true); script.Instantiate();
        Check(starts==1,"Re-enable restarted existing instance");
        script.BindInstance(nullptr); script.BindInstance(std::make_unique<FakeScript>(starts));
        Check(starts==2,"Fresh runtime instance must run Start exactly once");
        bool added=false;
        a.action=[&]() { if (!added) { added=true; objects.Create().AddBehavior<Probe>(events,"new"); } };
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(std::find(events.begin(),events.end(),"newS")!=events.end() && std::find(events.begin(),events.end(),"newU")==events.end(),"New behavior must Start immediately but wait for the next tick snapshot");
        std::cout<<"PASS: immediate Start, empty hooks, priority/tie ordering, tick snapshots, draw gating, enable state, failure isolation\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
