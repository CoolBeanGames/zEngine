#pragma once
#include "core/ScriptBehavior.h"
#include "core/BehaviorLifecycle.h"
#include "zscript/Script.h"
#include "physics/PhysicsWorld.h"
#include <map>

namespace zengine
{
    // Engine adapter: no window, renderer, asset I/O or editor dependencies.
    // The ObjectStore and its behaviors must outlive this host. Stop before destroying them.
    class ScriptHost final
    {
    public:
        struct Field { std::string name, type, label, value; bool editable = false; bool multiline=false; };
        bool Prepare(ScriptBehavior&, std::string source, std::string className);
        std::vector<Field> Fields(ScriptBehavior&);
        void SetField(ScriptBehavior&, const std::string& name, const std::string& text);
        std::string Error(const ScriptBehavior&) const;
        std::map<std::string,script::Value> AuthoredValues(const ScriptBehavior&) const;
        void RestoreValues(ScriptBehavior&, std::map<std::string,script::Value> values);
        void Forget(ScriptBehavior& behavior) { if(playing_)throw std::logic_error("Stop before forgetting a script.");records_.erase(&behavior); }
        bool Play(ObjectStore&, physics::World* physicsWorld=nullptr);
        using PrefabSpawner=std::function<GameObjectId(std::string_view)>;
        void SetPrefabSpawner(PrefabSpawner spawner) { if(playing_)throw std::logic_error("Stop before changing the prefab spawner.");prefabSpawner_=std::move(spawner); }
        void DispatchPhysicsEvents(const std::vector<physics::ContactEvent>&);
        void Stop(ObjectStore&);
        void Tick(ObjectStore& objects, float delta) { if (playing_) lifecycle_.Tick(objects, delta); }
        void PhysicsTick(ObjectStore& objects, float delta) { if (playing_) lifecycle_.PhysicsTick(objects, delta); }
        void Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& visible) { if (playing_) lifecycle_.Draw(objects, visible); }
        bool Playing() const noexcept { return playing_; }
        void SetInput(script::InputFrame frame) { input_=std::move(frame); }
    private:
        struct Record
        {
            std::string source, className, error;
            std::shared_ptr<const script::Program> program;
            std::unique_ptr<script::Runtime> preview;
            script::ObjectRef object;
            std::map<std::string, script::Value> overrides;
        };
        std::map<const ScriptBehavior*, Record> records_;
        std::map<GameObjectId, Transform> transforms_;
        std::map<GameObjectId,GameObjectId> parents_;
        BehaviorLifecycle lifecycle_;
        bool playing_ = false;
        script::InputFrame input_;
        PrefabSpawner prefabSpawner_;
        ObjectStore* playingObjects_=nullptr;
        physics::World* playingPhysics_=nullptr;
    };
}
