#pragma once
#include "GameObject.h"
#include <stdexcept>

namespace zengine
{
    // Runtime bridge contract: no language/compiler types leak into the core or renderer.
    class ScriptInstance
    {
    public:
        virtual ~ScriptInstance() = default;
        virtual bool HasStart() const noexcept = 0;
        virtual bool HasUpdate() const noexcept = 0;
        virtual bool HasDraw() const noexcept = 0;
        virtual bool HasPhysicsUpdate() const noexcept { return false; }
        virtual void Start(GameObject&) = 0;
        virtual void Update(GameObject&, float delta) = 0;
        virtual void Draw(GameObject&) = 0;
        virtual void PhysicsUpdate(GameObject&, float) {}
    };
    // Project-relative asset reference with an optional live instance during Play.
    class ScriptBehavior final : public Behavior
    {
    public:
        ScriptBehavior(GameObject& owner, std::string asset) : Behavior(owner), asset_(std::move(asset))
        {
            if (asset_.empty()) throw std::invalid_argument("Script asset reference cannot be empty.");
        }
        const std::string& Asset() const noexcept { return asset_; }
        bool HasInstance() const noexcept { return instance_ != nullptr; }
        ScriptInstance* Instance() noexcept { return instance_.get(); }
        void BindInstance(std::unique_ptr<ScriptInstance> instance)
        {
            instance_ = std::move(instance);
            ResetLifecycle();
            if (instance_) Instantiate();
        }
    protected:
        bool HasStart() const noexcept override { return instance_ && instance_->HasStart(); }
        bool HasUpdate() const noexcept override { return instance_ && instance_->HasUpdate(); }
        bool HasDraw() const noexcept override { return instance_ && instance_->HasDraw(); }
        bool HasPhysicsUpdate() const noexcept override { return instance_ && instance_->HasPhysicsUpdate(); }
        void OnStart() override { instance_->Start(Owner()); }
        void OnUpdate(float delta) override { instance_->Update(Owner(),delta); }
        void OnDraw() override { instance_->Draw(Owner()); }
        void OnPhysicsUpdate(float delta) override { instance_->PhysicsUpdate(Owner(),delta); }
    private:
        std::string asset_;
        std::unique_ptr<ScriptInstance> instance_;
    };
}
