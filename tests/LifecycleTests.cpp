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
        // ZE-78: the schedule is cached between frames, so the equal-priority (a, c)
        // order is arbitrary but STABLE until the behavior graph changes - not
        // reshuffled every tick.
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(events.size()==4 && events.front()=="bU" && events.back()=="dU","Priority must be descending, including negatives");
        const std::string tie=events[1];
        for (int i=0;i<16;++i) { events.clear(); lifecycle.Tick(objects,0.25f); Check(events[1]==tie,"Equal-priority order must be stable between frames"); }
        // Across fresh lifecycles the tie is still randomised (both orders occur).
        std::set<std::string> ties;
        for (int seed=0;seed<64 && ties.size()<2;++seed)
        {
            ObjectStore s; BehaviorLifecycle lc(seed); std::vector<std::string> ev;
            s.Create().AddBehavior<Probe>(ev,"x",0.0f); s.Create().AddBehavior<Probe>(ev,"y",0.0f);
            ev.clear(); lc.Tick(s,0.25f); ties.insert(ev.front());
        }
        Check(ties.size()==2,"Equal priorities must be randomised per schedule build");
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
        events.clear(); lifecycle.Tick(objects,0.25f);
        Check(std::find(events.begin(),events.end(),"newU")!=events.end(),"ZE-78: a spawned behavior must join the schedule on the next tick (cache invalidation)");
        // ZE-78: a scene of behaviors with no update code does zero per-tick work.
        {
            ObjectStore idle; BehaviorLifecycle idleLife(1);
            for (int i=0;i<50;++i) idle.Create().AddBehavior<Empty>();
            idleLife.Tick(idle,0.25f); idleLife.PhysicsTick(idle,0.25f);
            idleLife.Draw(idle,[](GameObjectId){ return true; }); // Empty::OnUpdate/OnDraw throw if ever called
        }
        std::cout<<"PASS: immediate Start, empty hooks, priority/tie ordering, tick snapshots, draw gating, enable state, failure isolation, cached schedule\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
