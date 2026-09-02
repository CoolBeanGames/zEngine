#include "EditorShell.h"
#include "InspectorPanel.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "physics/PhysicsBehavior.h"
#include <algorithm>
#include <limits>

std::vector<zengine::GameObjectId> EditorShell::ObjectRows() const {
    auto rows=objects_.HierarchyOrder();
    std::erase_if(rows,[&](auto id){auto parent=objects_.Find(id)->Parent();while(parent){if(collapsedObjects_.contains(parent))return true;parent=objects_.Find(parent)->Parent();}return false;});return rows;
}
bool EditorShell::HasChildren(zengine::GameObjectId id) const {
    for(std::size_t i=0;i<objects_.Size();++i)if(objects_.At(i).Parent()==id)return true;return false;
}
void EditorShell::SetObjectParent(zengine::GameObjectId child,zengine::GameObjectId parent) {
    RequireScene();RequireEditable(child,true);
    if(Playing())throw std::runtime_error("Stop Play before reparenting objects in the editor.");
    if(parent)RequireEditable(parent,true);
    auto* object=objects_.Find(child);if(!object)throw std::runtime_error("Unknown child object.");
    if(!editingPrefab_.empty() && ((child==objects_.At(0).Id() && parent) || (child!=objects_.At(0).Id() && !parent)))throw std::runtime_error("A prefab must keep exactly one root object.");
    if(object->Parent()==parent)return;
    EndGizmoDrag(false);object->SetParent(parent);
    if(auto link=prefabLinks_.find(child);link!=prefabLinks_.end())link->second.parent=parent;
    collapsedObjects_.erase(parent);MarkSceneDirty();SelectGameObject(child);
    status_=parent?L"Object parent changed (local transform preserved)":L"Object moved to scene root";InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::RevertPrefabTransform(zengine::GameObjectId id) {
    RequireScene();RequireEditable(id,true);if(Playing())throw std::runtime_error("Stop Play before reverting overrides.");
    auto link=prefabLinks_.find(id);if(link==prefabLinks_.end())throw std::runtime_error("Select a prefab instance root.");
    auto document=CaptureDocument();
    for(auto& object:document.objects)if(object.id==id){object.transformMask=0;object.transformOverride=false;}
    RebuildDocument(document,id);MarkSceneDirty();
    status_=L"Prefab transform overrides reverted to the source asset";InvalidateRect(window_,nullptr,FALSE);
}

void EditorShell::SyncMainCamera(zengine::GameObjectId keepMain) {
    // Exactly one camera may carry the "main" tag. keepMain wins; otherwise the first
    // camera already tagged "main" keeps it and the rest lose it.
    zengine::GameObjectId main=keepMain;
    if(!main)for(std::size_t i=0;i<objects_.Size();++i){auto& o=objects_.At(i);if(o.GetBehavior<zengine::Camera>()&&o.HasTag(zengine::Camera::MainTag)){main=o.Id();break;}}
    for(std::size_t i=0;i<objects_.Size();++i) {
        auto& o=objects_.At(i);
        if(!o.GetBehavior<zengine::Camera>())continue;
        const bool wants=o.Id()==main,has=o.HasTag(zengine::Camera::MainTag);
        if(wants==has)continue;
        auto tags=o.Tags();
        if(wants)tags.push_back(zengine::Camera::MainTag);
        else tags.erase(std::remove(tags.begin(),tags.end(),std::string(zengine::Camera::MainTag)),tags.end());
        o.SetTags(std::move(tags));
    }
}
zengine::GameObject& EditorShell::CreateGameObject(ObjectPreset preset,zengine::GameObjectId parent) {
    if(parent)RequireEditable(parent);
    auto& object=CreateEmptyGameObject();
    if(parent)SetObjectParent(object.Id(),parent);
    const auto rename=[&](std::string base){std::string name=base;for(unsigned suffix=1;;++suffix){bool used=false;for(std::size_t i=0;i<objects_.Size();++i)if(objects_.At(i).Id()!=object.Id()&&objects_.At(i).Name()==name){used=true;break;}if(!used)break;name=base+" ("+std::to_string(suffix)+")";}object.SetName(std::move(name));};
    if(preset==ObjectPreset::Cube){rename("Cube");AssignCube(object.Id());}
    else if(preset==ObjectPreset::Camera){
        rename("Camera");
        object.AddBehavior<zengine::Camera>();
        object.SetTags({zengine::Camera::MainTag}); // a new camera takes over as the main camera
        SyncMainCamera(object.Id());
        inspectorPanel_->RefreshBehaviors(); OnObjectChanged();
    }
    else if(preset!=ObjectPreset::Empty){rename(preset==ObjectPreset::RigidBody?"RigidBody":preset==ObjectPreset::KinematicBody?"KinematicBody":preset==ObjectPreset::StaticBody?"StaticBody":"Area");object.AddBehavior<zengine::physics::Collider>();if(preset==ObjectPreset::RigidBody)object.AddBehavior<zengine::physics::RigidBody>();else if(preset==ObjectPreset::KinematicBody)object.AddBehavior<zengine::physics::KinematicBody>();else if(preset==ObjectPreset::StaticBody)object.AddBehavior<zengine::physics::StaticBody>();else object.AddBehavior<zengine::physics::Area>();inspectorPanel_->RefreshBehaviors();OnObjectChanged();}
    status_=preset==ObjectPreset::Camera?L"Created Camera GameObject - it is now the main camera":L"Created GameObject";
    BeginObjectRename(object.Id());
    return object;
}
void EditorShell::CopyGameObject(zengine::GameObjectId id) {
    RequireScene();RequireEditable(id);if(Playing())throw std::runtime_error("Stop Play before copying objects.");
    const auto source=CaptureDocument();std::set<zengine::GameObjectId> selected;
    for(const auto& object:source.objects){auto current=object.id;for(unsigned depth=0;current&&depth<=64;++depth){if(current==id){selected.insert(object.id);break;}const auto it=std::find_if(source.objects.begin(),source.objects.end(),[&](const auto& item){return item.id==current;});current=it==source.objects.end()?0:it->parent;}}
    zengine::scenes::Document copy;for(const auto& object:source.objects)if(selected.contains(object.id))copy.objects.push_back(object);
    if(copy.objects.empty())throw std::runtime_error("The selected object cannot be copied.");
    auto root=std::find_if(copy.objects.begin(),copy.objects.end(),[&](const auto& object){return object.id==id;});root->parent=0;objectClipboard_=std::move(copy);status_=L"Copied GameObject hierarchy";InvalidateRect(window_,&statusBar_,FALSE);
}
zengine::GameObjectId EditorShell::PasteGameObject(zengine::GameObjectId parent) {
    RequireScene();if(Playing())throw std::runtime_error("Stop Play before pasting objects.");if(!objectClipboard_)throw std::runtime_error("Copy a GameObject first.");
    if(!editingPrefab_.empty()&&!parent)parent=objects_.At(0).Id();if(parent){RequireEditable(parent);if(!objects_.Find(parent))throw std::runtime_error("Unknown paste parent.");}
    auto document=CaptureDocument();zengine::GameObjectId next=1;for(const auto& object:document.objects)next=std::max(next,object.id+1);
    std::map<zengine::GameObjectId,zengine::GameObjectId> ids;for(const auto& object:objectClipboard_->objects){if(next==std::numeric_limits<zengine::GameObjectId>::max())throw std::runtime_error("GameObject IDs exhausted.");ids[object.id]=next++;}
    const auto root=std::find_if(objectClipboard_->objects.begin(),objectClipboard_->objects.end(),[](const auto& object){return object.parent==0;});if(root==objectClipboard_->objects.end())throw std::runtime_error("Copied hierarchy has no root.");const auto oldRoot=root->id;for(auto object:objectClipboard_->objects){const auto old=object.id;object.id=ids.at(old);object.parent=old==oldRoot?parent:ids.at(object.parent);document.objects.push_back(std::move(object));}
    const auto pasted=ids.at(oldRoot);RebuildDocument(document,pasted);MarkSceneDirty();status_=L"Pasted GameObject hierarchy";return pasted;
}
void EditorShell::DeleteGameObject(zengine::GameObjectId id) {
    RequireScene();RequireEditable(id);if(Playing())throw std::runtime_error("Stop Play before deleting objects.");if(!editingPrefab_.empty()&&id==objects_.At(0).Id())throw std::runtime_error("A prefab must keep its root GameObject.");
    auto document=CaptureDocument();std::set<zengine::GameObjectId> removed{id};for(bool changed=true;changed;){changed=false;for(const auto& object:document.objects)if(removed.contains(object.parent)&&removed.insert(object.id).second)changed=true;}
    zengine::GameObjectId select=0;for(const auto& object:document.objects)if(object.id==id){select=object.parent;break;}std::erase_if(document.objects,[&](const auto& object){return removed.contains(object.id);});
    RebuildDocument(document,select);MarkSceneDirty();status_=L"Deleted GameObject hierarchy";
}
