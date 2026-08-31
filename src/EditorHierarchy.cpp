#include "EditorShell.h"
#include "InspectorPanel.h"

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
