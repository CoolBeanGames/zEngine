#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <map>
#include <set>

namespace zengine
{
    struct Vec3 { float x = 0, y = 0, z = 0; bool operator==(const Vec3&) const = default; };

    namespace physics { class Collider; class RigidBody; class KinematicBody; class StaticBody; }

    // Local to the parent. Euler angles are in degrees.
    class Transform
    {
    public:
        const Vec3& Position() const noexcept { return position_; }
        const Vec3& Rotation() const noexcept { return rotation_; }
        const Vec3& Scale() const noexcept { return scale_; }
        void SetPosition(Vec3 value);
        void SetRotation(Vec3 value);
        void SetScale(Vec3 value);
    private:
        Vec3 position_{};
        Vec3 rotation_{};
        Vec3 scale_{1, 1, 1};
    };

    class GameObject;
    using GameObjectId = std::uint64_t;

    // Native and script behaviors share scheduling data; the core never owns a scripting VM.
    class Behavior
    {
    public:
        virtual ~Behavior() = default;
        Behavior(const Behavior&) = delete;
        Behavior& operator=(const Behavior&) = delete;
        GameObject& Owner() noexcept { return owner_; }
        const GameObject& Owner() const noexcept { return owner_; }
        bool Enabled() const noexcept { return enabled_; }
        void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
        float Priority() const noexcept { return priority_; }
        void SetPriority(float priority);
        bool Started() const noexcept { return started_; }
        bool Faulted() const noexcept { return !error_.empty(); }
        const std::string& Error() const noexcept { return error_; }
        // Called only after full construction/ownership, never from a base constructor.
        void Instantiate();
        void Tick(float delta);
        void PhysicsTick(float delta);
        void Draw();
    protected:
        explicit Behavior(GameObject& owner) : owner_(owner) {}
        virtual bool HasStart() const noexcept { return false; }
        virtual bool HasUpdate() const noexcept { return false; }
        virtual bool HasPhysicsUpdate() const noexcept { return false; }
        virtual bool HasDraw() const noexcept { return false; }
        virtual void OnStart() {}
        virtual void OnUpdate(float) {}
        virtual void OnPhysicsUpdate(float) {}
        virtual void OnDraw() {}
        void ResetLifecycle() { started_ = false; error_.clear(); }
    private:
        friend class BehaviorLifecycle;
        GameObject& owner_;
        bool enabled_ = true;
        float priority_ = 0;
        bool started_ = false;
        bool starting_ = false;
        std::string error_;
    };

    class ObjectStore;
    class GameObject final
    {
    public:
        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;
        GameObjectId Id() const noexcept { return id_; }
        GameObjectId Parent() const noexcept { return parent_; }
        void SetParent(GameObjectId parent);
        const std::string& Name() const noexcept { return name_; }
        void SetName(std::string name);
        const std::vector<std::string>& Tags() const noexcept { return tags_; }
        void SetTags(std::vector<std::string> tags);
        bool HasTag(std::string_view tag) const;
        Transform& GetTransform() noexcept { return transform_; }
        const Transform& GetTransform() const noexcept { return transform_; }
        std::size_t BehaviorCount() const noexcept { return behaviors_.size(); }
        const Behavior& BehaviorAt(std::size_t index) const { return *behaviors_.at(index); }
        Behavior& BehaviorAt(std::size_t index) { return *behaviors_.at(index); }
        bool RemoveBehavior(Behavior& behavior);

        template<class T, class... Args> T& AddBehavior(Args&&... args)
        {
            static_assert(std::is_base_of_v<Behavior, T>);
            auto behavior = std::make_unique<T>(*this, std::forward<Args>(args)...);
            T& result = *behavior;
            behaviors_.push_back(std::move(behavior));
            if constexpr (std::is_same_v<T, physics::RigidBody> || std::is_same_v<T, physics::KinematicBody> || std::is_same_v<T, physics::StaticBody>)
                EnsureCollider();
            result.Instantiate();
            return result;
        }
        template<class T> T* GetBehavior() noexcept
        {
            for (const auto& behavior : behaviors_)
                if (auto* match = dynamic_cast<T*>(behavior.get())) return match;
            return nullptr;
        }
        template<class T> const T* GetBehavior() const noexcept
        {
            for (const auto& behavior : behaviors_)
                if (auto* match = dynamic_cast<const T*>(behavior.get())) return match;
            return nullptr;
        }
    private:
        friend class ObjectStore;
        void EnsureCollider();
        GameObject(ObjectStore& store,GameObjectId id, std::string name);
        ObjectStore* store_;
        GameObjectId id_;
        GameObjectId parent_=0;
        std::string name_;
        std::vector<std::string> tags_;
        Transform transform_;
        std::vector<std::unique_ptr<Behavior>> behaviors_;
    };

    // In-memory ownership with stable IDs/pointers, deliberately not a scene file format.
    class ObjectStore final
    {
    public:
        ObjectStore()=default;
        ObjectStore(ObjectStore&&) noexcept;
        ObjectStore& operator=(ObjectStore&&) noexcept;
        ObjectStore(const ObjectStore&)=delete;
        ObjectStore& operator=(const ObjectStore&)=delete;
        // Validate the entire proposed graph before committing any parent changes.
        void SetParents(const std::map<GameObjectId,GameObjectId>&);
        std::vector<GameObjectId> HierarchyOrder() const;
        GameObject& Create(std::string name = "GameObject");
        GameObject& Restore(GameObjectId id, std::string name);
        // Atomically removes a known group; surviving children may not reference removed parents.
        void Remove(const std::set<GameObjectId>& ids);
        GameObject* Find(GameObjectId id) noexcept;
        const GameObject* Find(GameObjectId id) const noexcept;
        std::size_t Size() const noexcept { return objects_.size(); }
        GameObject& At(std::size_t index) { return *objects_.at(index); }
        const GameObject& At(std::size_t index) const { return *objects_.at(index); }
    private:
        GameObjectId nextId_ = 1;
        std::vector<std::unique_ptr<GameObject>> objects_;
    };
}
