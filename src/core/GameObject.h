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
#include <unordered_map>

namespace zengine
{
    struct Vec3 { float x = 0, y = 0, z = 0; bool operator==(const Vec3&) const = default; };
    struct Vec2 { float x = 0, y = 0; bool operator==(const Vec2&) const = default; };

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

    // 2D counterpart used by GameObject2D. Position/scale are 2D; rotation is a
    // single z-axis angle in degrees. Coordinates are screen-space (set by the caller).
    class Transform2D
    {
    public:
        const Vec2& Position() const noexcept { return position_; }
        float Rotation() const noexcept { return rotation_; }
        const Vec2& Scale() const noexcept { return scale_; }
        void SetPosition(Vec2 value);
        void SetRotation(float degrees);
        void SetScale(Vec2 value);
    private:
        Vec2 position_{};
        float rotation_ = 0;
        Vec2 scale_{1, 1};
    };

    class ObjectCore;
    class GameObject;
    class GameObject2D;
    using GameObjectId = std::uint64_t;

    // Native and script behaviors share scheduling data; the core never owns a scripting VM.
    // A behavior's owner is an ObjectCore, so behaviors work on 2D and 3D objects alike.
    class Behavior
    {
    public:
        virtual ~Behavior() = default;
        Behavior(const Behavior&) = delete;
        Behavior& operator=(const Behavior&) = delete;
        ObjectCore& Owner() noexcept { return owner_; }
        const ObjectCore& Owner() const noexcept { return owner_; }
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
        explicit Behavior(ObjectCore& owner) : owner_(owner) {}
        // Convenience for 3D-only behaviors (mesh, physics, camera): they are never
        // attached to a GameObject2D, so this cast is always valid.
        GameObject& Owner3D() noexcept;
        const GameObject& Owner3D() const noexcept;
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
        ObjectCore& owner_;
        bool enabled_ = true;
        float priority_ = 0;
        bool started_ = false;
        bool starting_ = false;
        std::string error_;
    };

    class ObjectStore;

    // Shared identity/hierarchy/behavior machinery for every scene object. GameObject
    // (3D) and GameObject2D differ only in which transform they carry.
    class ObjectCore
    {
    public:
        ObjectCore(const ObjectCore&) = delete;
        ObjectCore& operator=(const ObjectCore&) = delete;
        virtual ~ObjectCore() = default;
        GameObjectId Id() const noexcept { return id_; }
        GameObjectId Parent() const noexcept { return parent_; }
        void SetParent(GameObjectId parent);
        const std::string& Name() const noexcept { return name_; }
        void SetName(std::string name);
        const std::vector<std::string>& Tags() const noexcept { return tags_; }
        void SetTags(std::vector<std::string> tags);
        bool HasTag(std::string_view tag) const;
        std::size_t BehaviorCount() const noexcept { return behaviors_.size(); }
        const Behavior& BehaviorAt(std::size_t index) const { return *behaviors_.at(index); }
        Behavior& BehaviorAt(std::size_t index) { return *behaviors_.at(index); }
        bool RemoveBehavior(Behavior& behavior);
        bool Is2D() const noexcept { return is2D_; }

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
    protected:
        ObjectCore(ObjectStore& store, GameObjectId id, std::string name, bool is2D);
    private:
        friend class ObjectStore;
        void EnsureCollider();
        ObjectStore* store_;
        GameObjectId id_;
        GameObjectId parent_ = 0;
        std::string name_;
        std::vector<std::string> tags_;
        bool is2D_ = false;
        std::vector<std::unique_ptr<Behavior>> behaviors_;
    };

    class GameObject final : public ObjectCore
    {
    public:
        Transform& GetTransform() noexcept { return transform_; }
        const Transform& GetTransform() const noexcept { return transform_; }
    private:
        friend class ObjectStore;
        GameObject(ObjectStore& store, GameObjectId id, std::string name) : ObjectCore(store, id, std::move(name), false) {}
        Transform transform_;
    };

    class GameObject2D final : public ObjectCore
    {
    public:
        Transform2D& GetTransform() noexcept { return transform_; }
        const Transform2D& GetTransform() const noexcept { return transform_; }
    private:
        friend class ObjectStore;
        GameObject2D(ObjectStore& store, GameObjectId id, std::string name) : ObjectCore(store, id, std::move(name), true) {}
        Transform2D transform_;
    };

    inline GameObject& Behavior::Owner3D() noexcept { return static_cast<GameObject&>(owner_); }
    inline const GameObject& Behavior::Owner3D() const noexcept { return static_cast<const GameObject&>(owner_); }

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
        GameObject2D& Create2D(std::string name = "GameObject2D");
        GameObject2D& Restore2D(GameObjectId id, std::string name);
        // Atomically removes a known group; surviving children may not reference removed parents.
        void Remove(const std::set<GameObjectId>& ids);
        ObjectCore* Find(GameObjectId id) noexcept;
        const ObjectCore* Find(GameObjectId id) const noexcept;
        std::size_t Size() const noexcept { return objects_.size(); }
        ObjectCore& At(std::size_t index) { return *objects_.at(index); }
        const ObjectCore& At(std::size_t index) const { return *objects_.at(index); }
    private:
        template<class T> T& Add(GameObjectId id, std::string name);
        GameObjectId nextId_ = 1;
        std::vector<std::unique_ptr<ObjectCore>> objects_;
        // ID -> object lookup. Objects are heap-owned by objects_, so these raw
        // pointers stay valid across vector growth and store moves.
        std::unordered_map<GameObjectId, ObjectCore*> index_;
    };

    // Checked downcasts for the 3D-only systems (physics, renderer, 3D scene code).
    inline GameObject* As3D(ObjectCore* core) noexcept { return dynamic_cast<GameObject*>(core); }
    inline const GameObject* As3D(const ObjectCore* core) noexcept { return dynamic_cast<const GameObject*>(core); }
    inline GameObject& As3D(ObjectCore& core) noexcept { return static_cast<GameObject&>(core); }
    inline const GameObject& As3D(const ObjectCore& core) noexcept { return static_cast<const GameObject&>(core); }
    inline GameObject2D* As2D(ObjectCore* core) noexcept { return dynamic_cast<GameObject2D*>(core); }
    inline const GameObject2D* As2D(const ObjectCore* core) noexcept { return dynamic_cast<const GameObject2D*>(core); }
    inline GameObject2D& As2D(ObjectCore& core) noexcept { return static_cast<GameObject2D&>(core); }
    inline const GameObject2D& As2D(const ObjectCore& core) noexcept { return static_cast<const GameObject2D&>(core); }
}
