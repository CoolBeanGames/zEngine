#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zengine
{
    struct Vec3 { float x = 0, y = 0, z = 0; };

    // World-space for now. Euler angles in degrees; parenting comes with scene management.
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

    // Ownership/extension point only: no scripting VM or execution lifecycle in this milestone.
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
    protected:
        explicit Behavior(GameObject& owner) : owner_(owner) {}
    private:
        GameObject& owner_;
        bool enabled_ = true;
    };

    class ObjectStore;
    class GameObject final
    {
    public:
        GameObject(const GameObject&) = delete;
        GameObject& operator=(const GameObject&) = delete;
        GameObjectId Id() const noexcept { return id_; }
        const std::string& Name() const noexcept { return name_; }
        void SetName(std::string name);
        const std::vector<std::string>& Tags() const noexcept { return tags_; }
        void SetTags(std::vector<std::string> tags);
        bool HasTag(std::string_view tag) const;
        Transform& GetTransform() noexcept { return transform_; }
        const Transform& GetTransform() const noexcept { return transform_; }
        std::size_t BehaviorCount() const noexcept { return behaviors_.size(); }

        template<class T, class... Args> T& AddBehavior(Args&&... args)
        {
            static_assert(std::is_base_of_v<Behavior, T>);
            auto behavior = std::make_unique<T>(*this, std::forward<Args>(args)...);
            T& result = *behavior;
            behaviors_.push_back(std::move(behavior));
            return result;
        }
        template<class T> T* GetBehavior() noexcept
        {
            for (const auto& behavior : behaviors_)
                if (auto* match = dynamic_cast<T*>(behavior.get())) return match;
            return nullptr;
        }
    private:
        friend class ObjectStore;
        GameObject(GameObjectId id, std::string name);
        GameObjectId id_;
        std::string name_;
        std::vector<std::string> tags_;
        Transform transform_;
        std::vector<std::unique_ptr<Behavior>> behaviors_;
    };

    // In-memory ownership with stable IDs/pointers, deliberately not a scene file format.
    class ObjectStore final
    {
    public:
        GameObject& Create(std::string name = "GameObject");
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
