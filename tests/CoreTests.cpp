#include "core/GameObject.h"
#include "core/MeshRenderer.h"
#include "CrashHandler.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
class TestBehavior final : public zengine::Behavior
{
public:
    TestBehavior(zengine::ObjectCore& owner, bool& destroyed) : Behavior(owner), destroyed_(destroyed) {}
    ~TestBehavior() override { destroyed_ = true; }
private:
    bool& destroyed_;
};
int main()
{
    try
    {
        bool destroyed = false;
        {
            zengine::ObjectStore store;
            auto& object = store.Create();
            Check(object.Name() == "GameObject" && object.Tags().empty() && object.BehaviorCount() == 0, "Empty defaults failed");
            Check(object.GetTransform().Position().x == 0 && object.GetTransform().Rotation().y == 0 &&
                  object.GetTransform().Scale().z == 1, "Transform defaults failed");
            const auto id = object.Id();
            for (int i = 0; i < 200; ++i) store.Create("Other");
            Check(store.Find(id) == &object && store.Find(0) == nullptr, "Object references must remain stable");
            object.SetName("  Actor  ");
            Check(object.Name() == "Actor", "Name normalization failed");
            bool rejected = false;
            try { object.SetName("   "); } catch (const std::invalid_argument&) { rejected = true; }
            Check(rejected && object.Name() == "Actor", "Invalid rename must preserve the old name");
            object.SetTags({"enemy", " flying ", "enemy", "", "Enemy"});
            Check(object.Tags().size() == 3 && object.HasTag("flying") && !object.HasTag("missing"), "Tag list failed");
            object.GetTransform().SetPosition({1, 2, 3});
            object.GetTransform().SetRotation({10, 20, 30});
            object.GetTransform().SetScale({0, -2, 3});
            rejected = false;
            try { object.GetTransform().SetPosition({std::numeric_limits<float>::infinity(), 0, 0}); }
            catch (const std::invalid_argument&) { rejected = true; }
            Check(rejected && object.GetTransform().Position().x == 1, "Nonfinite transforms must be rejected atomically");
            auto& behavior = object.AddBehavior<TestBehavior>(destroyed);
            Check(&behavior.Owner() == &object && object.GetBehavior<TestBehavior>() == &behavior, "Behavior ownership failed");
            behavior.SetEnabled(false);
            Check(!behavior.Enabled() && object.BehaviorCount() == 1, "Behavior enabled state failed");
        }
        Check(destroyed, "Object must own the behavior lifetime");
        zengine::ObjectStore scene;
        auto& actor = scene.Create("Mesh Actor");
        auto& mesh = actor.AddBehavior<zengine::MeshRenderer>();
        Check(mesh.Asset().empty() && mesh.Enabled() && &mesh.Owner() == &actor, "Mesh renderer defaults/ownership failed");
        actor.GetTransform().SetPosition({2,3,4});
        mesh.SetAsset("Model/model.fbx");
        mesh.SetEnabled(false);
        const auto& constantActor = actor;
        Check(constantActor.GetBehavior<zengine::MeshRenderer>() == &mesh && !mesh.Enabled(), "Const lookup/visibility failed");
        mesh.SetAsset({});
        Check(actor.GetTransform().Position().x == 2 && mesh.Asset().empty(), "Mesh changes must preserve transform");
        auto& child=scene.Create("Child");auto& sibling=scene.Create("Sibling");auto& grandchild=scene.Create("Grandchild");
        child.SetParent(actor.Id());grandchild.SetParent(child.Id());
        Check(scene.HierarchyOrder()==std::vector<zengine::GameObjectId>{actor.Id(),child.Id(),grandchild.Id(),sibling.Id()},"Hierarchy must be stable depth-first order");
        const auto rejects=[&](auto action){bool failed=false;try{action();}catch(const std::invalid_argument&){failed=true;}Check(failed,"Invalid parenting accepted");};
        rejects([&]{actor.SetParent(grandchild.Id());});rejects([&]{child.SetParent(child.Id());});rejects([&]{child.SetParent(99999);});
        rejects([&]{scene.SetParents({{sibling.Id(),actor.Id()},{actor.Id(),grandchild.Id()}});});
        Check(sibling.Parent()==0 && actor.Parent()==0 && child.Parent()==actor.Id(),"Failed graph edit must be atomic");
        scene.SetParents({{child.Id(),0},{actor.Id(),child.Id()}});
        Check(actor.Parent()==child.Id() && child.Parent()==0,"Atomic parent swap failed");
        zengine::ObjectStore moved=std::move(scene);actor.SetParent(0);grandchild.SetParent(actor.Id());
        Check(moved.Find(actor.Id())==&actor && grandchild.Parent()==actor.Id(),"Moved store lost parent ownership");
        zengine::ObjectStore assigned;assigned=std::move(moved);grandchild.SetParent(0);
        Check(assigned.Find(grandchild.Id())->Parent()==0,"Move assignment lost parent ownership");
        zengine::ObjectStore deep;auto previous=deep.Create().Id();
        for(int i=0;i<64;++i){auto& next=deep.Create();next.SetParent(previous);previous=next.Id();}
        auto& tooDeep=deep.Create();rejects([&]{tooDeep.SetParent(previous);});
        zengine::ObjectStore removable;auto& kept=removable.Create("Kept");auto& removedRoot=removable.Create("Runtime root");auto& removedChild=removable.Create("Runtime child");removedChild.SetParent(removedRoot.Id());
        rejects([&]{removable.Remove({removedRoot.Id()});});Check(removable.Size()==3,"Failed group removal was not atomic");
        const auto* stable=&kept;removable.Remove({removedRoot.Id(),removedChild.Id()});Check(removable.Size()==1 && removable.Find(kept.Id())==stable,"Group removal invalidated surviving GameObjects");
        // GameObject2D shares the store, IDs, hierarchy, tags and behaviors with 3D objects.
        bool flatDestroyed = false;
        {
            zengine::ObjectStore mixed;
            auto& solid = mixed.Create("Solid");
            auto& sprite = mixed.Create2D("Sprite");
            Check(!solid.Is2D() && sprite.Is2D(), "Is2D flag failed");
            Check(sprite.Name() == "Sprite" && sprite.Tags().empty() && sprite.BehaviorCount() == 0, "GameObject2D defaults failed");
            Check(sprite.GetTransform().Position() == zengine::Vec2{} && sprite.GetTransform().Rotation() == 0 &&
                  sprite.GetTransform().Scale() == zengine::Vec2{1, 1}, "Transform2D defaults failed");
            sprite.GetTransform().SetPosition({64, 128});
            sprite.GetTransform().SetRotation(90);
            sprite.GetTransform().SetScale({2, 3});
            Check(sprite.GetTransform().Position() == zengine::Vec2{64, 128} && sprite.GetTransform().Rotation() == 90 &&
                  sprite.GetTransform().Scale() == zengine::Vec2{2, 3}, "Transform2D mutation failed");
            bool rejected2d = false;
            try { sprite.GetTransform().SetPosition({std::numeric_limits<float>::infinity(), 0}); }
            catch (const std::invalid_argument&) { rejected2d = true; }
            Check(rejected2d && sprite.GetTransform().Position() == zengine::Vec2{64, 128}, "Nonfinite Transform2D must be rejected atomically");
            sprite.SetTags({"hud", "hud", " overlay ", ""});
            Check(sprite.Tags().size() == 2 && sprite.HasTag("hud") && sprite.HasTag("overlay"), "GameObject2D tags failed");
            auto& flatBehavior = sprite.AddBehavior<TestBehavior>(flatDestroyed);
            Check(&flatBehavior.Owner() == &sprite && sprite.GetBehavior<TestBehavior>() == &flatBehavior, "GameObject2D behavior ownership failed");
            const auto spriteId = sprite.Id();
            for (int i = 0; i < 50; ++i) mixed.Create2D("Filler");
            Check(mixed.Find(spriteId) == &sprite && mixed.Find(spriteId)->Is2D(), "GameObject2D reference stability failed");
            sprite.SetParent(solid.Id());
            Check(sprite.Parent() == solid.Id() &&
                  mixed.HierarchyOrder().front() == solid.Id() && mixed.HierarchyOrder().at(1) == spriteId,
                  "Mixed 2D/3D hierarchy failed");
            Check(zengine::As2D(mixed.Find(spriteId)) == &sprite && zengine::As3D(mixed.Find(spriteId)) == nullptr, "As2D/As3D discrimination failed");
            Check(zengine::As3D(mixed.Find(solid.Id())) == &solid && zengine::As2D(mixed.Find(solid.Id())) == nullptr, "As3D/As2D discrimination failed");
        }
        Check(flatDestroyed, "GameObject2D must own the behavior lifetime");

        // ZE-127: the crash handler writes a discoverable report and surfaces it once.
        {
            namespace crash = zengine::crash;
            crash::Install("zEngineCoreTests", "test-build");
            crash::Breadcrumb("core tests: exercising the crash reporter");
            const auto report = crash::ReportHandledFatal("simulated fatal: std::bad_alloc");
            Check(!report.empty() && std::filesystem::is_regular_file(report), "Crash report file was not written");
            std::ifstream in(report);
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            Check(text.find("simulated fatal") != std::string::npos &&
                  text.find("exercising the crash reporter") != std::string::npos &&
                  text.find("build:    test-build") != std::string::npos,
                  "Crash report is missing the detail / breadcrumbs / build id");
            const auto first = crash::TakePreviousCrashReport();
            Check(first == report, "Previous-crash marker did not point at the report");
            Check(crash::TakePreviousCrashReport().empty(), "Previous-crash marker was not cleared after reading");
            std::error_code ec; std::filesystem::remove(report, ec);
        }
        std::cout << "PASS: platform-independent GameObject defaults, stable IDs, tags, transforms, behavior ownership, crash reporter\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
