#include "GameObject.h"
#include "physics/PhysicsBehavior.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

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

void zengine::Behavior::SetPriority(float value)
{
    if (!std::isfinite(value)) throw std::invalid_argument("Behavior priority must be finite.");
    priority_ = value;
}
void zengine::Behavior::Instantiate()
{
    if (started_) return;
    started_ = true; // Reentrant calls cannot run Start twice.
    starting_ = true;
    try { if (HasStart()) OnStart(); }
    catch (const std::exception& e) { error_ = e.what(); if (error_.empty()) error_ = "Behavior start failed."; }
    catch (...) { error_ = "Unknown behavior start failure."; }
    starting_ = false;
}
void zengine::Behavior::Tick(float delta)
{
    if (!std::isfinite(delta) || delta < 0) throw std::invalid_argument("Tick delta must be finite and nonnegative.");
    if (!enabled_ || Faulted() || starting_) return;
    Instantiate();
    if (Faulted()) return;
    try { if (HasUpdate()) OnUpdate(delta); }
    catch (const std::exception& e) { error_ = e.what(); if (error_.empty()) error_ = "Behavior update failed."; }
    catch (...) { error_ = "Unknown behavior update failure."; }
}
void zengine::Behavior::PhysicsTick(float delta)
{
    if (!std::isfinite(delta) || delta < 0) throw std::invalid_argument("Physics tick delta must be finite and nonnegative.");
    if (!enabled_ || Faulted() || starting_) return;
    Instantiate();
    if (Faulted()) return;
    try { if (HasPhysicsUpdate()) OnPhysicsUpdate(delta); }
    catch (const std::exception& e) { error_ = e.what(); if (error_.empty()) error_ = "Behavior physics update failed."; }
    catch (...) { error_ = "Unknown behavior physics update failure."; }
}
void zengine::Behavior::Draw()
{
    if (!enabled_ || Faulted() || starting_) return;
    Instantiate();
    if (Faulted()) return;
    try { if (HasDraw()) OnDraw(); }
    catch (const std::exception& e) { error_ = e.what(); if (error_.empty()) error_ = "Behavior draw failed."; }
    catch (...) { error_ = "Unknown behavior draw failure."; }
}

zengine::GameObject::GameObject(ObjectStore& store,GameObjectId id, std::string name) : store_(&store),id_(id) { SetName(std::move(name)); }
void zengine::GameObject::EnsureCollider()
{
    if (!GetBehavior<physics::Collider>()) AddBehavior<physics::Collider>();
}
bool zengine::GameObject::RemoveBehavior(Behavior& behavior)
{
    const auto found=std::find_if(behaviors_.begin(),behaviors_.end(),[&](const auto& candidate){return candidate.get()==&behavior;});
    if(found==behaviors_.end())return false;
    behaviors_.erase(found); return true;
}
void zengine::GameObject::SetParent(GameObjectId parent){store_->SetParents({{id_,parent}});}
zengine::ObjectStore::ObjectStore(ObjectStore&& other) noexcept {*this=std::move(other);}
zengine::ObjectStore& zengine::ObjectStore::operator=(ObjectStore&& other) noexcept {
    if(this!=&other){objects_=std::move(other.objects_);nextId_=other.nextId_;other.nextId_=1;for(auto& object:objects_)object->store_=this;}return *this;
}
void zengine::ObjectStore::SetParents(const std::map<GameObjectId,GameObjectId>& changes) {
    if(changes.empty())return;
    std::map<GameObjectId,GameObjectId> parents;for(const auto& object:objects_)parents[object->Id()]=object->Parent();
    for(const auto& [id,parent]:changes){if(!parents.contains(id) || (parent && !parents.contains(parent)))throw std::invalid_argument("Parent and child must belong to this scene.");parents[id]=parent;}
    for(const auto& [id,parent]:parents){auto current=parent;unsigned depth=0;while(current){if(current==id || ++depth>64)throw std::invalid_argument("Parenting would create a cycle or exceed 64 levels.");current=parents.at(current);}}
    for(auto& object:objects_)if(auto it=changes.find(object->Id());it!=changes.end())object->parent_=it->second;
}
std::vector<zengine::GameObjectId> zengine::ObjectStore::HierarchyOrder() const {
    std::map<GameObjectId,std::vector<GameObjectId>> children;for(const auto& object:objects_)children[object->Parent()].push_back(object->Id());
    std::vector<GameObjectId> result;result.reserve(objects_.size());
    const auto visit=[&](auto&& self,GameObjectId parent)->void{const auto it=children.find(parent);if(it==children.end())return;for(auto id:it->second){result.push_back(id);self(self,id);}};
    visit(visit,0);return result;
}
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
    return Restore(nextId_,std::move(name));
}
zengine::GameObject& zengine::ObjectStore::Restore(GameObjectId id, std::string name)
{
    if (!id || id==std::numeric_limits<GameObjectId>::max() || Find(id)) throw std::invalid_argument("Invalid or duplicate GameObject ID.");
    auto object = std::unique_ptr<GameObject>(new GameObject(*this,id, std::move(name)));
    auto& result = *object;
    objects_.push_back(std::move(object));
    nextId_=std::max(nextId_,id+1);
    return result;
}
void zengine::ObjectStore::Remove(const std::set<GameObjectId>& ids)
{
    for(const auto id:ids)if(!Find(id))throw std::invalid_argument("Cannot remove a missing GameObject.");
    for(const auto& object:objects_)if(object->Parent() && ids.contains(object->Parent()) && !ids.contains(object->Id()))throw std::invalid_argument("Cannot remove a parent without its children.");
    std::erase_if(objects_,[&](const auto& object){return ids.contains(object->Id());});
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
