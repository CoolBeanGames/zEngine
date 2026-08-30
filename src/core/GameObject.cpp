#include "GameObject.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    void Validate(zengine::Vec3 value)
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            throw std::invalid_argument("Transform values must be finite.");
    }
    std::string Trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }
}

void zengine::Transform::SetPosition(Vec3 value) { Validate(value); position_ = value; }
void zengine::Transform::SetRotation(Vec3 value) { Validate(value); rotation_ = value; }
void zengine::Transform::SetScale(Vec3 value) { Validate(value); scale_ = value; }

zengine::GameObject::GameObject(GameObjectId id, std::string name) : id_(id) { SetName(std::move(name)); }
void zengine::GameObject::SetName(std::string name)
{
    name = Trim(std::move(name));
    if (name.empty()) throw std::invalid_argument("GameObject name cannot be empty.");
    name_ = std::move(name);
}
void zengine::GameObject::SetTags(std::vector<std::string> tags)
{
    std::vector<std::string> unique;
    for (auto& tag : tags)
    {
        tag = Trim(std::move(tag));
        if (!tag.empty() && std::find(unique.begin(), unique.end(), tag) == unique.end())
            unique.push_back(std::move(tag));
    }
    tags_ = std::move(unique);
}
bool zengine::GameObject::HasTag(std::string_view tag) const
{
    return std::find(tags_.begin(), tags_.end(), tag) != tags_.end();
}
zengine::GameObject& zengine::ObjectStore::Create(std::string name)
{
    auto object = std::unique_ptr<GameObject>(new GameObject(nextId_, std::move(name)));
    auto& result = *object;
    objects_.push_back(std::move(object));
    ++nextId_;
    return result;
}
zengine::GameObject* zengine::ObjectStore::Find(GameObjectId id) noexcept
{
    for (const auto& object : objects_) if (object->Id() == id) return object.get();
    return nullptr;
}
const zengine::GameObject* zengine::ObjectStore::Find(GameObjectId id) const noexcept
{
    for (const auto& object : objects_) if (object->Id() == id) return object.get();
    return nullptr;
}
