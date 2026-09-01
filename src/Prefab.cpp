#include "Prefab.h"
#include "TransformOverrides.h"
#include <algorithm>
#include <limits>

namespace zengine::prefabs
{
void Validate(const scenes::Document& document)
{
    scenes::Encode(document);
    if (document.objects.empty() || std::count_if(document.objects.begin(),document.objects.end(),[](const auto& o) { return o.parent==0; })!=1)
        throw std::runtime_error("A prefab must contain exactly one root GameObject.");
    if (!document.objects.front().prefab.empty() || document.objects.front().parent)
        throw std::runtime_error("The first prefab object must be its editable root.");
}
std::string Encode(const scenes::Document& document)
{
    Validate(document); const auto text="ZENGINE_PREFAB 1\n"+scenes::Encode(document);
    if (text.size()>scenes::MaxSceneBytes) throw std::runtime_error("Prefab exceeds the 8 MiB limit."); return text;
}
scenes::Document Decode(std::string_view text)
{
    constexpr std::string_view header="ZENGINE_PREFAB 1\n";
    if (text.size()>scenes::MaxSceneBytes || !text.starts_with(header)) throw std::runtime_error("Invalid/unsupported prefab file.");
    auto document=scenes::Decode(text.substr(header.size())); Validate(document); return document;
}
Expansion Expand(const scenes::Document& document,const Loader& load)
{
    scenes::Encode(document);
    Expansion out; GameObjectId next=1;
    for (const auto& object:document.objects) next=std::max(next,object.id+1);
    std::map<std::string,scenes::Document> cache;
    std::size_t loadedBytes=0;
    std::set<std::string> active;
    std::function<void(scenes::ObjectData,bool,std::string,unsigned)> append;
    append=[&](scenes::ObjectData object,bool generated,std::string source,unsigned depth)
    {
        if (depth>32 || out.scene.objects.size()>=10000) throw std::runtime_error("Prefab nesting/expanded object limit exceeded.");
        if (generated) out.generated.insert(object.id);
        if (object.prefab.empty())
        {
            if (!source.empty()) out.sources[object.id]=source;
            out.scene.objects.push_back(std::move(object)); return;
        }
        source=object.prefab;
        std::string key=source; std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!active.insert(key).second) throw std::runtime_error("Circular prefab nesting is not allowed.");
        auto it=cache.find(key);
        if (it==cache.end())
        {
            if (cache.size()>=128) throw std::runtime_error("Too many distinct prefab dependencies.");
            auto asset=load(source); Validate(asset); loadedBytes+=scenes::Encode(asset).size();
            if (loadedBytes>32*1024*1024) throw std::runtime_error("Prefab dependency data exceeds 32 MiB.");
            it=cache.emplace(key,std::move(asset)).first;
        }
        const auto& asset=it->second;
        std::map<GameObjectId,GameObjectId> ids; ids[asset.objects.front().id]=object.id;
        for (std::size_t i=1;i<asset.objects.size();++i)
        {
            if (next==std::numeric_limits<GameObjectId>::max()) throw std::runtime_error("Prefab object IDs exhausted.");
            ids[asset.objects[i].id]=next++;
        }
        for (std::size_t i=0;i<asset.objects.size();++i)
        {
            auto child=asset.objects[i]; child.id=ids.at(child.id);
            child.parent=i?ids.at(child.parent):object.parent;
            if (!i && (object.transformOverride || object.transformMask)) child.transform=OverrideTransform(child.transform,object.transform,object.transformMask?object.transformMask:511);
            if (!i) {
                if(object.prefabDataMask&1)child.name=object.name;
                if(object.prefabDataMask&2)child.tags=object.tags;
                if(object.prefabDataMask&4)child.behaviors=object.behaviors;
            }
            append(std::move(child),generated || i!=0,source,depth+1);
        }
        active.erase(key);
    };
    for (const auto& object:document.objects) append(object,false,{},0);
    scenes::Encode(out.scene);
    std::map<GameObjectId,std::vector<scenes::ObjectData>> children;
    for (auto& object:out.scene.objects) children[object.parent].push_back(std::move(object));
    out.scene.objects.clear();
    std::function<void(GameObjectId)> order=[&](GameObjectId parent) {
        for (auto& child:children[parent]) { const auto id=child.id; out.scene.objects.push_back(std::move(child)); order(id); }
    };
    order(0); return out;
}
}
