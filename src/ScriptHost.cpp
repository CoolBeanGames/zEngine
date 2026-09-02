#include "ScriptHost.h"
#include "zscript/Text.h"
#include "zscript/NativeTypes.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace zengine
{
namespace
{
    using namespace script;
    Vector3 ToScript(Vec3 v) { return {v.x,v.y,v.z}; }
    Vec3 ToNative(Vector3 v)
    {
        const auto valid = [](double n) { return std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(); };
        if (!valid(v.x) || !valid(v.y) || !valid(v.z)) throw std::runtime_error("Script transform exceeds native float range.");
        return {static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z)};
    }
    bool Editable(const std::string& type) { return type=="char" || type=="int" || type=="float" || type=="bool" || type=="string" || type=="Vector3" || type=="prefab"; }
    bool ReferenceTypeName(std::string_view type) { return !Editable(std::string(type)) && type!="array"; }

    // Engine half of the native-type phone book: maps a script type name to how the host
    // detects the component on a GameObject and which Runtime accessor field exposes it.
    // Add a new referenceable component here (one row) and in zscript/NativeTypes.h.
    struct NativeBinding {
        std::string_view type;                        // script type name, e.g. "RigidBody"
        std::string_view field;                       // Runtime.Get(proxy, field) accessor
        std::function<bool(const GameObject&)> present;
    };
    const std::vector<NativeBinding>& NativeBindings()
    {
        static const std::vector<NativeBinding> table = {
            {"PhysicsBody",   "physics",        [](const GameObject& o){ return o.GetBehavior<physics::Body>()!=nullptr; }},
            {"RigidBody",     "rigidbody",      [](const GameObject& o){ return o.GetBehavior<physics::RigidBody>()!=nullptr; }},
            {"KinematicBody", "kinematic_body", [](const GameObject& o){ return o.GetBehavior<physics::KinematicBody>()!=nullptr; }},
            {"StaticBody",    "static_body",    [](const GameObject& o){ return o.GetBehavior<physics::StaticBody>()!=nullptr; }},
            {"Area",          "area",           [](const GameObject& o){ return o.GetBehavior<physics::Area>()!=nullptr; }},
            {"Collider",      "collider",       [](const GameObject& o){ return o.GetBehavior<physics::Collider>()!=nullptr; }},
        };
        return table;
    }
    const NativeBinding* FindBinding(std::string_view type)
    {
        for (const auto& b : NativeBindings()) if (b.type==type) return &b;
        return nullptr;
    }
    std::string Format(const Value& value)
    {
        std::ostringstream text; text.imbue(std::locale::classic()); text<<std::setprecision(17);
        if (const auto* v=std::get_if<std::int64_t>(&value)) text<<*v;
        else if (const auto* v=std::get_if<double>(&value)) text<<*v;
        else if (const auto* v=std::get_if<bool>(&value)) text<<(*v?"true":"false");
        else if (const auto* v=std::get_if<std::string>(&value)) return *v;
        else if(const auto* v=std::get_if<char32_t>(&value))return *v?script::text::Encode(*v):"\\0";
        else if (const auto* v=std::get_if<Vector3>(&value)) text<<v->x<<", "<<v->y<<", "<<v->z;
        else if (std::holds_alternative<ArrayRef>(value)) return "(array - read only)";
        else if(const auto* v=std::get_if<PrefabRef>(&value))return v->asset;
        else return "(object reference - read only)";
        return text.str();
    }
    Value Parse(const std::string& type, const std::string& text)
    {
        if (type=="string") return text;
        if(type=="char")return text=="\\0"?char32_t{}:script::text::Character(text);
        if(type=="prefab")return PrefabRef{text};
        if (type=="bool") { if (text=="true") return true; if (text=="false") return false; throw std::invalid_argument("Use true or false."); }
        std::istringstream stream(text); stream.imbue(std::locale::classic());
        Value value;
        if (type=="int") { std::int64_t v; if (!(stream>>v)) throw std::invalid_argument("Invalid integer."); value=v; }
        else if (type=="float") { double v; if (!(stream>>v) || !std::isfinite(v)) throw std::invalid_argument("Invalid number."); value=v; }
        else if (type=="Vector3")
        {
            Vector3 v; char a,b;
            if (!(stream>>v.x>>a>>v.y>>b>>v.z) || a!=',' || b!=',' || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                throw std::invalid_argument("Use three finite numbers: x, y, z.");
            value=v;
        }
        else throw std::invalid_argument("Object reference editing is not supported yet.");
        stream>>std::ws;
        if (!stream.eof()) throw std::invalid_argument("Unexpected text after value.");
        return value;
    }
    class BoundScript final : public ScriptInstance
    {
    public:
        BoundScript(std::shared_ptr<const Program> p, const std::string& name, const std::map<std::string,Value>& overrides, const std::map<std::string,GameObjectId>& references, const InputFrame& inputFrame,ObjectStore& objects,GameObjectId owner,physics::World* physicsWorld,const ScriptHost::PrefabSpawner& prefabSpawner,const std::function<void(std::string_view)>& output)
            : program(std::move(p)), runtime(program), object(runtime.Create(name)),
              draw(program->HasCode(name,"draw")), physicsUpdate(program->HasCode(name,"physicsUpdate")), input(inputFrame),scene(objects)
        {
            runtime.SetInput(input,false);BindNative(owner,object);for(const auto& [field,value]:overrides)runtime.Set(object,field,std::holds_alternative<PrefabRef>(value)?Value{runtime.CreatePrefab(std::get<PrefabRef>(value).asset)}:value);
            printHandler=output;runtime.SetPrintCallback([this](std::string_view text){if(printHandler)printHandler(text);});
            proxies.emplace(owner,object);
            runtime.SetObjectLookup([this](std::string_view name){
                GameObjectId id=0;for(std::size_t i=0;i<scene.Size();++i)if(scene.At(i).Name()==name){if(id)throw std::runtime_error("Ambiguous object name; give scene objects unique names for find().");id=scene.At(i).Id();}
                return Proxy(id);
            });
            if(physicsWorld)runtime.SetPhysicsCallbacks(
                [this,physicsWorld](ObjectRef ownerRef,std::string_view method,const std::vector<Value>& arguments)->Value{
                    const auto id=NativeId(ownerRef);if(!physicsWorld->Contains(id))throw std::runtime_error("GameObject has no active physics body.");
                    if(method=="get_velocity")return ToScript(physicsWorld->Velocity(id));if(method=="get_angular_velocity")return ToScript(physicsWorld->AngularVelocity(id));
                    if(arguments.size()!=1 || !std::holds_alternative<Vector3>(arguments[0]))throw std::runtime_error("Physics body method requires one Vector3.");const auto value=ToNative(std::get<Vector3>(arguments[0]));
                    if(method=="add_force")physicsWorld->AddForce(id,value);else if(method=="add_impulse")physicsWorld->AddImpulse(id,value);else if(method=="add_torque")physicsWorld->AddTorque(id,value);else if(method=="add_angular_impulse")physicsWorld->AddAngularImpulse(id,value);else if(method=="set_velocity")physicsWorld->SetVelocity(id,value);else if(method=="set_angular_velocity")physicsWorld->SetAngularVelocity(id,value);else throw std::runtime_error("Unknown physics body command.");return {};
                },
                [this,physicsWorld](Vector3 from,Vector3 to,std::uint32_t mask){std::vector<ObjectRef> result;for(const auto& hit:physicsWorld->Cast(ToNative(from),ToNative(to),mask))result.push_back(Proxy(hit.object));return result;});
            if(prefabSpawner)runtime.SetPrefabSpawnCallback([this,prefabSpawner](std::string_view asset){return Proxy(prefabSpawner(asset));});
            for (const auto& [field, id] : references)
                if (id)
                    for (const auto& entry : program->InspectorLayout(name))
                        if (entry.kind == InspectorEntry::Kind::Field && entry.name == field && ReferenceTypeName(entry.type))
                        { runtime.Set(object, field, Reference(id, entry.type)); break; }
        }
        bool HasStart() const noexcept override { return true; }
        // Synchronize native transform signals even for a listener with no update body.
        bool HasUpdate() const noexcept override { return true; }
        bool HasDraw() const noexcept override { return draw; }
        bool HasPhysicsUpdate() const noexcept override { return physicsUpdate; }
        void Start(GameObject& owner) override { Invoke(owner,[&] { runtime.Start(object); }); }
        void Update(GameObject& owner,float delta) override { Invoke(owner,[&] { runtime.SetInput(input); runtime.Update(object,delta); }); }
        void Draw(GameObject& owner) override { Invoke(owner,[&] { runtime.Draw(object); }); }
        void PhysicsUpdate(GameObject& owner,float delta) override { Invoke(owner,[&] { runtime.PhysicsUpdate(object,delta); }); }
        void PhysicsEvent(GameObject& owner,const physics::ContactEvent& event) {
            Invoke(owner,[&]{const auto body=std::get<ObjectRef>(runtime.Get(object,"physics"));const char* phase=event.phase==physics::ContactPhase::Entered?"entered":event.phase==physics::ContactPhase::Stayed?"stayed":"exited";runtime.Emit({body,std::string(event.area?"area_":"collision_")+phase},{Proxy(event.other)});});
        }
        std::shared_ptr<const Program> program;
        Runtime runtime;
        ObjectRef object;
        std::function<void(std::string_view)> printHandler;
    private:
        ObjectRef Proxy(GameObjectId id) {
            if(!id)return {};
            if(auto it=proxies.find(id);it!=proxies.end())return it->second;
            if(!scene.Find(id))throw std::runtime_error("Scene object no longer exists.");
            const auto ref=runtime.Create("gameObject");proxies.emplace(id,ref);BindNative(id,ref);Synchronize(id,ref);return ref;
        }
        ObjectRef Reference(GameObjectId id, std::string_view type) {
            const auto* native=scene.Find(id); if(!native) throw std::runtime_error("Scene object no longer exists.");
            const auto proxy=Proxy(id);
            if(type=="gameObject")return proxy;
            if(type=="Transform")return std::get<ObjectRef>(runtime.Get(proxy,"transform"));
            if(const auto* binding=FindBinding(type))
                return binding->present(*native)?std::get<ObjectRef>(runtime.Get(proxy,std::string(binding->field))):ObjectRef{};
            if(type=="Behavior") {
                if(native->GetBehavior<physics::Body>())return std::get<ObjectRef>(runtime.Get(proxy,"physics"));
                if(native->GetBehavior<physics::Collider>())return std::get<ObjectRef>(runtime.Get(proxy,"collider"));
                return {};
            }
            throw std::runtime_error("Only native GameObject, Transform, and physics references can be assigned from the scene tree.");
        }
        void BindNative(GameObjectId id,ObjectRef ref) {
            const auto* native=scene.Find(id);if(!native)throw std::runtime_error("Scene object no longer exists.");
            bool boundBody=false;
            for(const auto& binding:NativeBindings()) {
                if(binding.type=="PhysicsBody" || !binding.present(*native))continue;
                const auto* info=script::FindNativeType(binding.type);
                if(info && info->physicsBody){ if(!boundBody){runtime.BindNativeBehavior(ref,std::string(binding.type));boundBody=true;} }
                else runtime.BindNativeBehavior(ref,std::string(binding.type));
            }
        }
        GameObjectId NativeId(ObjectRef ref) const {
            if(!ref.id)return 0;
            for(const auto& [id,proxy]:proxies)if(proxy==ref)return id;
            throw std::runtime_error("A parent must be a scene object: use parent or find(name), not gameObject().");
        }
        void Synchronize(GameObjectId id,ObjectRef proxy) {
            const auto* native=scene.Find(id);if(!native)throw std::runtime_error("Scene object no longer exists.");
            runtime.Set(proxy,"parent",Proxy(native->Parent()),false);
            const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));const auto& transform=native->GetTransform();
            runtime.Set(ref,"position",ToScript(transform.Position()),false);runtime.Set(ref,"rotation",ToScript(transform.Rotation()),false);runtime.Set(ref,"scale",ToScript(transform.Scale()),false);
        }
        template<class F> void Invoke(GameObject& owner, F callback)
        {
            (void)owner;
            // Break old proxy links before synchronizing a potentially reparented native graph.
            for(const auto& [id,proxy]:proxies)runtime.Set(proxy,"parent",ObjectRef{},false);
            for(const auto& [id,proxy]:proxies)Synchronize(id,proxy);
            for(const auto& [id,previous]:previousTransforms) {
                const auto ref=std::get<ObjectRef>(runtime.Get(proxies.at(id),"transform"));const auto& native=scene.Find(id)->GetTransform();
                const auto position=ToScript(native.Position()),rotation=ToScript(native.Rotation()),scale=ToScript(native.Scale());
                if(position!=ToScript(previous.Position()))runtime.Emit({ref,"was_moved"},{position});
                if(rotation!=ToScript(previous.Rotation()))runtime.Emit({ref,"was_rotated"},{rotation});
                if(scale!=ToScript(previous.Scale()))runtime.Emit({ref,"was_scaled"},{scale});
            }
            callback();
            std::map<GameObjectId,GameObjectId> parents;std::map<GameObjectId,Transform> transforms;
            for(const auto& [id,proxy]:proxies) {
                const auto parent=NativeId(std::get<ObjectRef>(runtime.Get(proxy,"parent")));
                if(parent!=scene.Find(id)->Parent())parents[id]=parent;
                const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));Transform next;
                next.SetPosition(ToNative(std::get<Vector3>(runtime.Get(ref,"position"))));next.SetRotation(ToNative(std::get<Vector3>(runtime.Get(ref,"rotation"))));next.SetScale(ToNative(std::get<Vector3>(runtime.Get(ref,"scale"))));transforms[id]=next;
            }
            scene.SetParents(parents); // Validate complete graph before committing any transform.
            for(const auto& [id,transform]:transforms)scene.Find(id)->GetTransform()=transform;
            previousTransforms=std::move(transforms);
        }
        bool draw,physicsUpdate;
        const InputFrame& input;
        ObjectStore& scene;
        std::map<GameObjectId,ObjectRef> proxies;
        std::map<GameObjectId,Transform> previousTransforms;
    };
}

bool ScriptHost::IsReferenceType(std::string_view type)
{
    return ReferenceTypeName(type);
}

script::ObjectRef ScriptHost::PreviewReference(Record& record, GameObjectId id, std::string_view type)
{
    if (!objectStore_ || !record.preview) throw std::runtime_error("Script reference editing requires an active scene.");
    const auto* native = objectStore_->Find(id);
    if (!native) throw std::invalid_argument("The referenced GameObject no longer exists.");
    auto found = record.previewProxies.find(id);
    script::ObjectRef proxy;
    if (found != record.previewProxies.end()) proxy = found->second;
    else
    {
        proxy = record.preview->Create("gameObject");
        record.previewProxies.emplace(id, proxy);
        bool boundBody = false;
        for (const auto& binding : NativeBindings())
        {
            if (binding.type == "PhysicsBody" || !binding.present(*native)) continue;
            const auto* info = script::FindNativeType(binding.type);
            if (info && info->physicsBody) { if (!boundBody) { record.preview->BindNativeBehavior(proxy, binding.type); boundBody = true; } }
            else record.preview->BindNativeBehavior(proxy, binding.type);
        }
    }
    if (type == "gameObject") return proxy;
    if (type == "Transform") return std::get<script::ObjectRef>(record.preview->Get(proxy, "transform"));
    if (const auto* binding = FindBinding(type))
        return binding->present(*native) ? std::get<script::ObjectRef>(record.preview->Get(proxy, std::string(binding->field))) : script::ObjectRef{};
    if (type == "Behavior")
    {
        if (native->GetBehavior<physics::Body>()) return std::get<script::ObjectRef>(record.preview->Get(proxy, "physics"));
        if (native->GetBehavior<physics::Collider>()) return std::get<script::ObjectRef>(record.preview->Get(proxy, "collider"));
        return {};
    }
    throw std::invalid_argument("Only native GameObject, Transform, and physics references can be assigned from the scene tree.");
}

void ScriptHost::ApplyPreviewReferences(Record& record)
{
    if (!record.preview) return;
    for (const auto& [name, id] : record.references)
    {
        if (!id) continue;
        const auto& layout = record.program->InspectorLayout(record.className);
        const auto found = std::find_if(layout.begin(), layout.end(), [&](const script::InspectorEntry& entry) {
            return entry.kind == script::InspectorEntry::Kind::Field && entry.name == name;
        });
        if (found != layout.end() && IsReferenceType(found->type))
            record.preview->Set(record.object, name, PreviewReference(record, id, found->type));
    }
}

bool ScriptHost::Prepare(ScriptBehavior& behavior, std::string source, std::string className)
{
    if (playing_ && behavior.HasInstance()) throw std::logic_error("A running script is already prepared.");
    auto& record=records_[&behavior];
    if (record.program && record.source==source && record.className==className) return record.error.empty();
    auto previousValues=AuthoredValues(behavior);
    record.source=std::move(source); record.className=std::move(className);
    record.error.clear(); record.program.reset(); record.preview.reset(); record.previewProxies.clear();
    try
    {
        const auto compiled=script::Compiler::Compile(record.source,behavior.Asset());
        if (!compiled) { const auto& d=compiled.diagnostics.front(); throw std::runtime_error(d.source+":"+std::to_string(d.line)+":"+std::to_string(d.column)+": "+d.message); }
        if (!compiled.program->HasClass(record.className) || !compiled.program->IsGameObject(record.className))
            throw std::runtime_error("Script must define class "+record.className+" : gameObject (matching the filename).");
        auto preview=std::make_unique<script::Runtime>(compiled.program);
        const auto ref=preview->Create(record.className);
        std::map<std::string,script::Value> kept;
        for (const auto& entry:compiled.program->InspectorLayout(record.className))
            if (entry.kind==script::InspectorEntry::Kind::Field && Editable(entry.type))
                if (auto it=record.overrides.find(entry.name); it!=record.overrides.end())
                {
                    try { const auto value=std::holds_alternative<script::PrefabRef>(it->second)?script::Value{preview->CreatePrefab(std::get<script::PrefabRef>(it->second).asset)}:it->second;preview->Set(ref,entry.name,value);kept.emplace(entry.name,entry.type=="prefab"?script::Value{script::PrefabRef{preview->PrefabAsset(std::get<script::ObjectRef>(preview->Get(ref,entry.name)))}}:preview->Get(ref,entry.name)); }
                    catch (const script::ScriptError&) {} // Changed field type: use its new default.
                }
        std::map<std::string, GameObjectId> keptReferences;
        for (const auto& entry : compiled.program->InspectorLayout(record.className))
            if (entry.kind == script::InspectorEntry::Kind::Field && IsReferenceType(entry.type))
                if (const auto it = record.references.find(entry.name); it != record.references.end() && it->second)
                    keptReferences.emplace(entry.name, it->second);
        record.references=std::move(keptReferences);
        record.overrides=std::move(kept); record.object=ref;
        record.preview=std::move(preview); record.program=compiled.program;
        ApplyPreviewReferences(record);
        if(playing_){if(!playingObjects_)throw std::logic_error("Missing running object store.");behavior.BindInstance(std::make_unique<BoundScript>(record.program,record.className,record.overrides,record.references,input_,*playingObjects_,behavior.Owner().Id(),playingPhysics_,prefabSpawner_,printHandler_));}
        return true;
    }
    catch (const std::exception& e) { record.overrides=std::move(previousValues); record.error=e.what(); return false; }
}
std::vector<ScriptHost::Field> ScriptHost::Fields(ScriptBehavior& behavior)
{
    std::vector<Field> result;
    auto found=records_.find(&behavior);
    if (found==records_.end() || !found->second.program) return result;
    auto& r=found->second;
    auto* live=dynamic_cast<BoundScript*>(behavior.Instance());
    for (const auto& entry:r.program->InspectorLayout(r.className))
    {
        if (entry.kind==script::InspectorEntry::Kind::Label) result.push_back({{},{},entry.text,{},false});
        else if (IsReferenceType(entry.type))
        {
            std::string value="None";
            if (const auto it=r.references.find(entry.name); it!=r.references.end() && it->second)
            {
                if (objectStore_ && objectStore_->Find(it->second)) value=objectStore_->Find(it->second)->Name()+" ("+entry.type+")";
                else value="Missing GameObject ("+std::to_string(it->second)+")";
            }
            result.push_back({entry.name,entry.type,{},std::move(value),false,entry.multiline,true});
        }
        else {auto& runtime=live?live->runtime:*r.preview;const auto object=live?live->object:r.object;auto value=runtime.Get(object,entry.name);if(entry.type=="prefab")value=script::PrefabRef{runtime.PrefabAsset(std::get<script::ObjectRef>(value))};result.push_back({entry.name,entry.type,{},Format(value),Editable(entry.type),entry.multiline});}
    }
    return result;
}
void ScriptHost::SetObjectReference(ScriptBehavior& behavior, const std::string& name, GameObjectId target)
{
    if (playing_) throw std::logic_error("Stop Play before assigning a scene reference.");
    auto& record=records_.at(&behavior);
    if (!record.program || !record.preview) throw std::logic_error("Prepare the script before assigning a scene reference.");
    const auto& layout=record.program->InspectorLayout(record.className);
    const auto found=std::find_if(layout.begin(),layout.end(),[&](const script::InspectorEntry& entry){return entry.kind==script::InspectorEntry::Kind::Field&&entry.name==name;});
    if(found==layout.end()||!IsReferenceType(found->type))throw std::invalid_argument("Field is not an object reference.");
    script::ObjectRef value{};
    if(target)
    {
        value=PreviewReference(record,target,found->type);
        if(!value.id)throw std::invalid_argument("The selected GameObject does not have a compatible "+found->type+" component.");
    }
    const auto previous=record.references.find(name);const auto old=previous==record.references.end()?GameObjectId{}:previous->second;
    try { record.preview->Set(record.object,name,value); }
    catch (...) { if(old)record.references[name]=old; else record.references.erase(name); throw; }
    if(target)record.references[name]=target;else record.references.erase(name);
}
void ScriptHost::SetField(ScriptBehavior& behavior, const std::string& name, const std::string& text)
{
    auto& r=records_.at(&behavior);
    for (const auto& field:Fields(behavior)) if (field.name==name && field.editable)
    {
        const auto value=Parse(field.type,text);
        if (auto* live=dynamic_cast<BoundScript*>(behavior.Instance())) live->runtime.Set(live->object,name,std::holds_alternative<script::PrefabRef>(value)?script::Value{live->runtime.CreatePrefab(std::get<script::PrefabRef>(value).asset)}:value);
        else { r.preview->Set(r.object,name,std::holds_alternative<script::PrefabRef>(value)?script::Value{r.preview->CreatePrefab(std::get<script::PrefabRef>(value).asset)}:value); r.overrides[name]=value; }
        return;
    }
    throw std::invalid_argument("Field is not exported/editable.");
}
std::string ScriptHost::Error(const ScriptBehavior& behavior) const
{
    if (behavior.Faulted()) return behavior.Asset()+": "+behavior.Error();
    const auto it=records_.find(&behavior);
    return it==records_.end() ? "Script has not been loaded." : it->second.error;
}
std::map<std::string,script::Value> ScriptHost::AuthoredValues(const ScriptBehavior& behavior) const
{
    const auto it=records_.find(&behavior);
    if (it==records_.end()) return {};
    const auto& r=it->second;
    if (!r.program || !r.preview) return r.overrides; // Missing/broken scripts retain their saved data.
    std::map<std::string,script::Value> values;
    for (const auto& entry:r.program->InspectorLayout(r.className))
        if (entry.kind==script::InspectorEntry::Kind::Field && Editable(entry.type))
            if(entry.type=="prefab")values.emplace(entry.name,script::PrefabRef{r.preview->PrefabAsset(std::get<script::ObjectRef>(r.preview->Get(r.object,entry.name)))});else values.emplace(entry.name,r.preview->Get(r.object,entry.name));
    return values;
}
std::map<std::string,GameObjectId> ScriptHost::AuthoredReferences(const ScriptBehavior& behavior) const
{
    const auto it=records_.find(&behavior);
    return it==records_.end()?std::map<std::string,GameObjectId>{}:it->second.references;
}
void ScriptHost::RestoreValues(ScriptBehavior& behavior, std::map<std::string,script::Value> values)
{
    auto found=records_.find(&behavior);
    // Runtime-spawned scripts are authored from prefab data immediately before
    // Prepare binds them. Existing live scripts must remain immutable in Play.
    if (found!=records_.end() && found->second.program)
        throw std::logic_error("Restore scene variables before compiling or playing.");
    records_[&behavior].overrides=std::move(values);
}
void ScriptHost::RestoreReferences(ScriptBehavior& behavior, std::map<std::string,GameObjectId> references)
{
    auto found=records_.find(&behavior);
    if (found!=records_.end() && found->second.program)
        throw std::logic_error("Restore scene references before compiling or playing.");
    records_[&behavior].references=std::move(references);
}
bool ScriptHost::Play(ObjectStore& objects,physics::World* physicsWorld)
{
    if (playing_) return true;
    objectStore_=&objects;
    // Construct every VM before Start. A compile/initializer error cannot partially start a scene.
    std::vector<std::pair<ScriptBehavior*,std::unique_ptr<BoundScript>>> ready;
    for (auto* behavior:lifecycle_.Ordered(objects))
        if (auto* script=dynamic_cast<ScriptBehavior*>(behavior))
        {
            auto it=records_.find(script);
            if (it==records_.end() || !it->second.program || !it->second.error.empty()) return false;
            auto& r=it->second;
            try { ready.emplace_back(script,std::make_unique<BoundScript>(r.program,r.className,r.overrides,r.references,input_,objects,script->Owner().Id(),physicsWorld,prefabSpawner_,printHandler_)); }
            catch (const std::exception& e) { r.error=e.what(); return false; }
        }
    transforms_.clear();
    parents_.clear();
    for (std::size_t i=0;i<objects.Size();++i) transforms_.emplace(objects.At(i).Id(),objects.At(i).GetTransform());
    for (std::size_t i=0;i<objects.Size();++i) parents_.emplace(objects.At(i).Id(),objects.At(i).Parent());
    playing_=true;playingObjects_=&objects;playingPhysics_=physicsWorld;
    for (auto& [behavior,instance]:ready) behavior->BindInstance(std::move(instance));
    return true;
}
void ScriptHost::DispatchPhysicsEvents(const std::vector<physics::ContactEvent>& events)
{
    if(!playing_)return;
    for(const auto& event:events)for(const auto& [behavior,_]:records_)if(behavior->Owner().Id()==event.receiver)
        if(auto* live=dynamic_cast<BoundScript*>(const_cast<ScriptBehavior*>(behavior)->Instance()))live->PhysicsEvent(const_cast<GameObject&>(behavior->Owner()),event);
}
void ScriptHost::Stop(ObjectStore& objects)
{
    if (!playing_) return;
    for (std::size_t i=0;i<objects.Size();++i)
    {
        auto& object=objects.At(i);
        for (std::size_t j=0;j<object.BehaviorCount();++j)
            if (auto* script=dynamic_cast<ScriptBehavior*>(&object.BehaviorAt(j))) script->BindInstance(nullptr);
        if (auto it=transforms_.find(object.Id()); it!=transforms_.end()) object.GetTransform()=it->second;
    }
    objects.SetParents(parents_);parents_.clear();transforms_.clear(); playing_=false;playingObjects_=nullptr;playingPhysics_=nullptr;
}
}
