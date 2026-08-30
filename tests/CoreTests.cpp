#include "core/GameObject.h"
#include "core/MeshRenderer.h"
#include <cmath>
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
    TestBehavior(zengine::GameObject& owner, bool& destroyed) : Behavior(owner), destroyed_(destroyed) {}
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
        std::cout << "PASS: platform-independent GameObject defaults, stable IDs, tags, transforms, behavior ownership\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
