#include "EditorShell.h"
#include "PrefabAssets.h"
#include "SceneAssets.h"
#include "InspectorPanel.h"
#include "TransformOverrides.h"
#include <algorithm>

namespace
{
    std::string AssetReference(const std::filesystem::path& file,const std::filesystem::path& assets)
    {
        const auto value=std::filesystem::relative(file,assets).generic_u8string();
        return {reinterpret_cast<const char*>(value.data()),value.size()};
    }
}
bool EditorShell::CanEdit(zengine::GameObjectId id,bool transformOnly) const
{
    (void)transformOnly;
    return !prefabGenerated_.contains(id);
}
void EditorShell::RecordPrefabDataOverride(zengine::GameObjectId id)
{
    auto link=prefabLinks_.find(id);if(link==prefabLinks_.end())return;
    const auto live=zengine::scenes::Capture(objects_,scriptHost_);
    const auto current=std::find_if(live.objects.begin(),live.objects.end(),[&](const auto& object){return object.id==id;});
    if(current==live.objects.end())return;
    const auto source=zengine::prefabs::Load(assetsDirectory_,std::filesystem::path(std::u8string(link->second.prefab.begin(),link->second.prefab.end())));
    const auto& root=source.objects.front();
    link->second.prefabDataMask=0;
    if(current->name!=root.name){link->second.prefabDataMask|=1;link->second.name=current->name;}else link->second.name="Prefab instance";
    if(current->tags!=root.tags){link->second.prefabDataMask|=2;link->second.tags=current->tags;}else link->second.tags.clear();
    if(current->behaviors!=root.behaviors){link->second.prefabDataMask|=4;link->second.behaviors=current->behaviors;}else link->second.behaviors.clear();
}
void EditorShell::RequireEditable(zengine::GameObjectId id,bool transformOnly) const
{
    if (!CanEdit(id,transformOnly)) throw std::runtime_error("Inherited prefab data is read-only. Double-click its prefab asset to edit the source.");
}
void EditorShell::RecordTransformOverride(zengine::GameObjectId id)
{
    if (auto it=prefabLinks_.find(id);it!=prefabLinks_.end()) if(const auto* object=objects_.Find(id)) {
        it->second.transformMask|=TransformDifference(it->second.transform,object->GetTransform());
        it->second.transformOverride=it->second.transformMask!=0;
        it->second.transform=object->GetTransform();
    }
}
int EditorShell::ObjectDepth(zengine::GameObjectId id) const
{
    int depth=0; const auto* object=objects_.Find(id);
    while (object && object->Parent() && depth<64) { ++depth; object=objects_.Find(object->Parent()); }
    return depth;
}
zengine::scenes::Document EditorShell::CaptureDocument() const
{
    auto document=zengine::scenes::Capture(objects_,scriptHost_);
    std::erase_if(document.objects,[&](const auto& object) { return prefabGenerated_.contains(object.id); });
    for (auto& object:document.objects)
        if (const auto link=prefabLinks_.find(object.id);link!=prefabLinks_.end())
        {
            const auto transform=object.transform;
            object=link->second;
            if(!(object.prefabDataMask&1))object.name="Prefab instance";
            if(!(object.prefabDataMask&2))object.tags.clear();
            if(!(object.prefabDataMask&4))object.behaviors.clear();
            object.transform=OverrideTransform(zengine::Transform{},transform,object.transformMask);
        }
    return document;
}
void EditorShell::RebuildDocument(const zengine::scenes::Document& document,zengine::GameObjectId select)
{
    const auto baseline=sceneBaseline_; const bool dirty=sceneDirty_;
    ApplyScene(scenePath_,sceneSource_,document);
    sceneBaseline_=baseline; sceneDirty_=dirty;
    if (select && objects_.Find(select)) SelectGameObject(select);
    UpdateSceneTitle();
}
void EditorShell::RefreshPrefabInstances()
{
    if (prefabLinks_.empty()) return;
    if (Playing()) throw std::runtime_error("Stop Play before refreshing prefab instances.");
    RebuildDocument(CaptureDocument(),selectedObject_);
}
std::filesystem::path EditorShell::CreatePrefab(zengine::GameObjectId id)
{
    RequireScene(); RequireEditable(id);
    if (Playing() || PendingModels()) throw std::runtime_error("Stop Play and wait for models before creating a prefab.");
    EndGizmoDrag(false); SetFocus(window_);
    auto document=CaptureDocument(); const auto root=std::find_if(document.objects.begin(),document.objects.end(),[&](const auto& o) { return o.id==id; });
    if (root==document.objects.end()) throw std::runtime_error("Select a GameObject to create a prefab.");
    if (!editingPrefab_.empty() && root->parent==0) throw std::runtime_error("Save the open prefab to update it; its root is already a prefab asset.");
    const auto belongs=[&](zengine::GameObjectId child) {
        for (int depth=0;child && depth<=64;++depth)
        {
            if (child==id) return true;
            const auto it=std::find_if(document.objects.begin(),document.objects.end(),[&](const auto& o) { return o.id==child; });
            child=it==document.objects.end()?0:it->parent;
        }
        return false;
    };
    zengine::scenes::Document prefab; prefab.objects.push_back(*root); prefab.objects.front().parent=0;
    for (const auto& object:document.objects) if (object.id!=id && belongs(object.id)) prefab.objects.push_back(object);
    const auto file=zengine::prefabs::Create(assetsDirectory_,prefab,AssetFolder());
    zengine::scenes::ObjectData reference; reference.id=id; reference.parent=root->parent; reference.name="Prefab instance";
    reference.prefab=AssetReference(file,assetsDirectory_);
    std::set<zengine::GameObjectId> children; for (const auto& object:prefab.objects) if (object.id!=id) children.insert(object.id);
    *root=reference; std::erase_if(document.objects,[&](const auto& o) { return children.contains(o.id); });
    RebuildDocument(document,id); MarkSceneDirty(); RefreshAssets();BeginAssetRename(file);
    status_=L"Created prefab and linked the scene object: "+file.filename().wstring(); InvalidateRect(window_,nullptr,FALSE);
    return file;
}
zengine::GameObjectId EditorShell::InstantiatePrefab(const std::filesystem::path& path,zengine::GameObjectId parent)
{
    RequireScene(); if (Playing()) throw std::runtime_error("Stop Play before placing prefabs.");
    const auto file=zengine::prefabs::Resolve(assetsDirectory_,path);
    auto document=CaptureDocument();
    if (!editingPrefab_.empty() && !parent) parent=document.objects.front().id;
    if (parent) { RequireEditable(parent); if (!objects_.Find(parent)) throw std::runtime_error("Unknown prefab parent."); }
    zengine::scenes::ObjectData ref; ref.id=1; ref.name="Prefab instance"; ref.parent=parent; ref.prefab=AssetReference(file,assetsDirectory_);
    for (std::size_t i=0;i<objects_.Size();++i) ref.id=std::max(ref.id,objects_.At(i).Id()+1);
    document.objects.push_back(ref);
    if (!editingPrefab_.empty())
    {
        // Validate the proposed stage against its own asset, without writing anything.
        const auto self=editingPrefab_;
        zengine::scenes::Document graph; zengine::scenes::ObjectData stage; stage.id=1; stage.name="Stage"; stage.prefab=AssetReference(self,assetsDirectory_); graph.objects.push_back(stage);
        zengine::prefabs::Expand(graph,[&](const std::string& asset) {
            const auto target=zengine::prefabs::Resolve(assetsDirectory_,std::filesystem::path(std::u8string(asset.begin(),asset.end())));
            return _wcsicmp(target.c_str(),self.c_str())==0?document:zengine::prefabs::Load(assetsDirectory_,target);
        });
    }
    RebuildDocument(document,ref.id); MarkSceneDirty(); status_=L"Placed linked prefab: "+file.filename().wstring();
    return ref.id;
}
bool EditorShell::OpenPrefab(const std::filesystem::path& path)
{
    RequireProject(); if (Playing() || PendingModels()) throw std::runtime_error("Stop Play and wait for models before editing a prefab.");
    const auto file=zengine::prefabs::Resolve(assetsDirectory_,path);
    const auto source=zengine::scenes::Load(file); const auto document=zengine::prefabs::Decode(source);
    zengine::prefabs::ResolveScene(assetsDirectory_,document); // Validate before changing editor context.
    if (!editingPrefab_.empty() && !ClosePrefab()) return false;
    EndGizmoDrag(false); SetFocus(window_);
    SceneSnapshot previous{CaptureDocument(),scenePath_,sceneSource_,sceneBaseline_,sceneDirty_,sceneOpen_,selectedObject_};
    editingPrefab_=file;
    try { ApplyScene(file,source,document); }
    catch (...) { editingPrefab_.clear(); throw; }
    prefabReturn_=std::move(previous);
    status_=L"Editing prefab asset - Ctrl+S saves to all linked instances; File > Close Prefab returns to the scene";
    InvalidateRect(window_,nullptr,FALSE); return true;
}
bool EditorShell::SavePrefab()
{
    if (editingPrefab_.empty()) throw std::runtime_error("Open a prefab asset first.");
    if (Playing() || PendingModels(true)) throw std::runtime_error("Stop Play and wait for model assignments before saving a prefab.");
    EndGizmoDrag(false); SetFocus(window_); const auto document=CaptureDocument();
    zengine::prefabs::Save(assetsDirectory_,editingPrefab_,document,&sceneSource_);
    sceneSource_=zengine::prefabs::Encode(document); sceneBaseline_=zengine::scenes::Encode(document); sceneDirty_=false;
    status_=L"Saved prefab - instances resolve the updated asset, including nested uses";
    RefreshAssets(); UpdateSceneTitle(); InvalidateRect(window_,nullptr,FALSE); return true;
}
bool EditorShell::ClosePrefab()
{
    if (editingPrefab_.empty()) return true;
    if (!ConfirmSceneClose()) return false;
    if (!prefabReturn_) throw std::runtime_error("Missing prefab return context.");
    const auto previous=*prefabReturn_;
    zengine::prefabs::ResolveScene(assetsDirectory_,previous.document); // A failed restoration leaves the stage open.
    editingPrefab_.clear();
    ApplyScene(previous.path,previous.source,previous.document);
    sceneOpen_=previous.open; sceneBaseline_=previous.baseline; sceneDirty_=previous.dirty;
    if (objects_.Find(previous.selected)) SelectGameObject(previous.selected);
    prefabReturn_.reset(); UpdateSceneTitle(); status_=L"Returned to scene - prefab instances refreshed";
    InvalidateRect(window_,nullptr,FALSE); return true;
}
