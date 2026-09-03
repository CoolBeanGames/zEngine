#include "ScriptHost.h"
#include "core/Camera.h"
#include "ui/UiControl.h"
#include "ui/UiSerialize.h"
#include "audio/AudioSource.h"
#include "audio/AudioEffect.h"
#include "zscript/Text.h"
#include "zscript/NativeTypes.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>

namespace zengine
{
namespace
{
    using namespace script;
    Vector3 ToScript(Vec3 v) { return {v.x,v.y,v.z}; }
    Vector2 ToScript(Vec2 v) { return {v.x,v.y}; }
    Vec2 ToNative2(Vector2 v)
    {
        const auto valid = [](double n) { return std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(); };
        if (!valid(v.x) || !valid(v.y)) throw std::runtime_error("Script transform exceeds native float range.");
        return {static_cast<float>(v.x), static_cast<float>(v.y)};
    }

    // ZE-96: mirror one ui:: control's serialized fields between the engine control
    // and its script proxy. `toScript` = engine -> proxy (before a callback);
    // otherwise proxy -> engine (after). Field names match the uiControl.. script
    // class fields (Script.cpp) and the engine keys (UiSerialize.cpp), with the
    // colour Vector3 <-> RGBA conversions and the split margin_* floats handled here.
    void SyncUiControl(Runtime& runtime, ObjectRef proxy, ui::UiControl& control, bool toScript)
    {
        const auto pull = [&](const char* field) { return runtime.Get(proxy, field); };
        const auto push = [&](const char* field, Value v) { runtime.Set(proxy, field, std::move(v), false); };
        const auto num  = [](float v) { return Value{static_cast<double>(v)}; };
        const auto getNum = [&](const char* f) { const auto v = pull(f); return std::holds_alternative<double>(v) ? static_cast<float>(std::get<double>(v)) : std::holds_alternative<std::int64_t>(v) ? static_cast<float>(std::get<std::int64_t>(v)) : 0.0f; };
        const auto getVec2 = [&](const char* f) { const auto v = pull(f); return std::holds_alternative<Vector2>(v) ? ToNative2(std::get<Vector2>(v)) : Vec2{}; };
        const auto getBool = [&](const char* f) { const auto v = pull(f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
        const auto getStr  = [&](const char* f) { const auto v = pull(f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string{}; };
        const auto getRgb  = [&](const char* f, Float4 keep) { const auto v = pull(f); if (!std::holds_alternative<Vector3>(v)) return keep; const auto c = std::get<Vector3>(v); return Float4{static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), keep.w}; };
        const auto rgb = [](Float4 c) { return Value{Vector3{c.x, c.y, c.z}}; };

        if (toScript)
        {
            push("anchor", Value{std::string(ui::AnchorName(control.GetAnchor()))});
            push("size", Value{ToScript(control.Size())});
            push("min_size", Value{ToScript(control.MinSize())});
            push("order", Value{static_cast<std::int64_t>(control.Order())});
            push("visible", Value{control.Visible()});
            push("clickable", Value{control.Clickable()});
        }
        else
        {
            if (ui::Anchor anchor; ui::ParseAnchor(getStr("anchor"), anchor)) control.SetAnchor(anchor);
            control.SetSize(getVec2("size"));
            control.SetMinSize(getVec2("min_size"));
            if (const auto v = pull("order"); std::holds_alternative<std::int64_t>(v)) control.SetOrder(static_cast<int>(std::get<std::int64_t>(v)));
            control.SetVisible(getBool("visible"));
            control.SetClickable(getBool("clickable"));
        }

        if (auto* c = dynamic_cast<ui::Container*>(&control))
        {
            if (toScript) { push("padding", num(c->Padding())); push("spacing", num(c->Spacing())); }
            else { c->SetPadding(getNum("padding")); c->SetSpacing(getNum("spacing")); }
        }
        if (auto* c = dynamic_cast<ui::HTileBoxContainer*>(&control))
        { if (toScript) push("fill_cross", Value{c->FillCross()}); else c->SetFillCross(getBool("fill_cross")); }
        if (auto* c = dynamic_cast<ui::VTileBoxContainer*>(&control))
        { if (toScript) push("fill_cross", Value{c->FillCross()}); else c->SetFillCross(getBool("fill_cross")); }
        if (auto* c = dynamic_cast<ui::ScrollContainer*>(&control))
        {
            if (toScript) { push("scroll_x", num(c->ScrollX())); push("scroll_y", num(c->ScrollY())); push("horizontal", Value{c->Horizontal()}); push("fill_cross", Value{c->FillCross()}); }
            else { c->SetScrollX(getNum("scroll_x")); c->SetScrollY(getNum("scroll_y")); c->SetHorizontal(getBool("horizontal")); c->SetFillCross(getBool("fill_cross")); }
        }
        if (auto* c = dynamic_cast<ui::MarginContainer*>(&control))
        {
            if (toScript) { push("margin_left", num(c->Left())); push("margin_top", num(c->Top())); push("margin_right", num(c->Right())); push("margin_bottom", num(c->Bottom())); }
            else c->SetMargins(getNum("margin_left"), getNum("margin_top"), getNum("margin_right"), getNum("margin_bottom"));
        }
        if (auto* c = dynamic_cast<ui::PanelContainer*>(&control))
        {
            if (toScript) { push("texture", Value{c->Texture()}); push("tint", rgb(c->Tint())); }
            else { c->SetTexture(getStr("texture")); c->SetTint(getRgb("tint", c->Tint())); }
        }
        if (auto* c = dynamic_cast<ui::TextEntry*>(&control))
        {
            if (toScript) { push("text", Value{c->Value()}); push("placeholder", Value{c->Placeholder()}); push("pixel_height", num(c->PixelHeight())); }
            else { c->SetValue(getStr("text")); c->SetPlaceholder(getStr("placeholder")); c->SetPixelHeight(getNum("pixel_height")); }
        }
        else if (auto* c = dynamic_cast<ui::Text*>(&control)) // Text / LongText
        {
            if (toScript) { push("text", Value{c->Value()}); push("pixel_height", num(c->PixelHeight())); push("color", rgb(c->Color())); }
            else { c->SetValue(getStr("text")); c->SetPixelHeight(getNum("pixel_height")); c->SetColor(getRgb("color", c->Color())); }
        }
        if (auto* c = dynamic_cast<ui::TextureRect*>(&control))
        {
            if (toScript) { push("texture", Value{c->Texture()}); push("tint", rgb(c->Tint())); }
            else { c->SetTexture(getStr("texture")); c->SetTint(getRgb("tint", c->Tint())); }
        }
        if (auto* c = dynamic_cast<ui::ColorRect*>(&control))
        { if (toScript) push("color", rgb(c->Color())); else c->SetColor(getRgb("color", c->Color())); }
        if (auto* c = dynamic_cast<ui::ProgressBar*>(&control))
        {
            if (toScript) { push("value", num(c->Value())); push("vertical", Value{c->Vertical()}); push("fill_color", rgb(c->Fill())); push("background_color", rgb(c->Background())); }
            else { c->SetValue(getNum("value")); c->SetVertical(getBool("vertical")); c->SetFill(getRgb("fill_color", c->Fill())); c->SetBackground(getRgb("background_color", c->Background())); }
        }
        if (auto* c = dynamic_cast<ui::Button*>(&control))
        {
            if (toScript) { push("text", Value{c->Text()}); push("pixel_height", num(c->PixelHeight())); push("disabled", Value{c->Disabled()}); }
            else { c->SetText(getStr("text")); c->SetPixelHeight(getNum("pixel_height")); c->SetDisabled(getBool("disabled")); }
        }
        if (auto* c = dynamic_cast<ui::VideoTexture*>(&control))
        {
            if (toScript) { push("video", Value{c->Video()}); push("playing", Value{c->Playing()}); push("loop", Value{c->Loop()}); push("speed", num(c->Speed())); push("tint", rgb(c->Tint())); }
            else { c->SetVideo(getStr("video")); c->SetPlaying(getBool("playing")); c->SetLoop(getBool("loop")); c->SetSpeed(getNum("speed")); c->SetTint(getRgb("tint", c->Tint())); }
        }
        if (auto* c = dynamic_cast<ui::UiHtml*>(&control))
        {
            if (toScript) { push("html", Value{c->Html()}); push("background", rgb(c->Background())); }
            else { c->SetHtml(getStr("html")); c->SetBackground(getRgb("background", c->Background())); }
        }
    }
    Vec3 ToNative(Vector3 v)
    {
        const auto valid = [](double n) { return std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(); };
        if (!valid(v.x) || !valid(v.y) || !valid(v.z)) throw std::runtime_error("Script transform exceeds native float range.");
        return {static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z)};
    }
    bool Editable(const std::string& type) { return type=="char" || type=="int" || type=="float" || type=="bool" || type=="string" || type=="Vector3" || type=="Vector2" || type=="prefab"; }
    bool ReferenceTypeName(std::string_view type) { return !Editable(std::string(type)) && type!="array"; }

    // Runtime variant tag -> editor value-type name (empty for object references / void).
    std::string ValueTypeName(const Value& value)
    {
        if (std::holds_alternative<std::int64_t>(value)) return "int";
        if (std::holds_alternative<double>(value)) return "float";
        if (std::holds_alternative<bool>(value)) return "bool";
        if (std::holds_alternative<std::string>(value)) return "string";
        if (std::holds_alternative<char32_t>(value)) return "char";
        if (std::holds_alternative<Vector3>(value)) return "Vector3";
        if (std::holds_alternative<Vector2>(value)) return "Vector2";
        if (std::holds_alternative<PrefabRef>(value)) return "prefab";
        return {};
    }
    Value DefaultForType(const std::string& type)
    {
        if (type=="int") return std::int64_t{0};
        if (type=="float") return 0.0;
        if (type=="bool") return false;
        if (type=="string") return std::string{};
        if (type=="char") return char32_t{0};
        if (type=="Vector3") return Vector3{};
        if (type=="Vector2") return Vector2{};
        if (type=="prefab") return PrefabRef{};
        throw std::invalid_argument("Unknown array element value type.");
    }
    // Most-specific reference type an object satisfies, for auto-typing a dragged element.
    std::string BestReferenceType(const ObjectCore& object)
    {
        for (const char* type : {"RigidBody","KinematicBody","StaticBody","Area","Camera","Collider","PhysicsBody"})
            if (ScriptHost::ObjectMatchesReferenceType(object, type)) return type;
        return "gameObject";
    }

    // Engine half of the native-type phone book: maps a script type name to how the host
    // detects the component on a GameObject and which Runtime accessor field exposes it.
    // Add a new referenceable component here (one row) and in zscript/NativeTypes.h.
    struct NativeBinding {
        std::string_view type;                        // script type name, e.g. "RigidBody"
        std::string_view field;                       // Runtime.Get(proxy, field) accessor
        std::function<bool(const ObjectCore&)> present;
    };
    const std::vector<NativeBinding>& NativeBindings()
    {
        static const std::vector<NativeBinding> table = {
            {"PhysicsBody",   "physics",        [](const ObjectCore& o){ return o.GetBehavior<physics::Body>()!=nullptr; }},
            {"RigidBody",     "rigidbody",      [](const ObjectCore& o){ return o.GetBehavior<physics::RigidBody>()!=nullptr; }},
            {"KinematicBody", "kinematic_body", [](const ObjectCore& o){ return o.GetBehavior<physics::KinematicBody>()!=nullptr; }},
            {"StaticBody",    "static_body",    [](const ObjectCore& o){ return o.GetBehavior<physics::StaticBody>()!=nullptr; }},
            {"Area",          "area",           [](const ObjectCore& o){ return o.GetBehavior<physics::Area>()!=nullptr; }},
            {"Collider",      "collider",       [](const ObjectCore& o){ return o.GetBehavior<physics::Collider>()!=nullptr; }},
            {"Camera",        "camera",         [](const ObjectCore& o){ return o.GetBehavior<Camera>()!=nullptr; }},
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
        else if (const auto* v=std::get_if<Vector2>(&value)) text<<v->x<<", "<<v->y;
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
        else if (type=="Vector2")
        {
            Vector2 v; char a;
            if (!(stream>>v.x>>a>>v.y) || a!=',' || !std::isfinite(v.x) || !std::isfinite(v.y))
                throw std::invalid_argument("Use two finite numbers: x, y.");
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
        BoundScript(std::shared_ptr<const Program> p, const std::string& name, const std::map<std::string,Value>& overrides, const std::map<std::string,GameObjectId>& references, const std::map<std::string,std::vector<ScriptArrayElement>>& arrays, const InputFrame& inputFrame,const MouseFrame& mouseFrame,ObjectStore& objects,GameObjectId owner,physics::World* physicsWorld,const ScriptHost::PrefabSpawner& prefabSpawner,const std::function<void(std::string_view)>& output,const ScriptHost::SceneLoader& sceneLoader,std::string sceneName)
            : program(std::move(p)), runtime(program), object(runtime.Create(name)),
              draw(program->HasCode(name,"draw")), physicsUpdate(program->HasCode(name,"physicsUpdate")), input(inputFrame),mouse(mouseFrame),scene(objects),ownerId(owner)
        {
            runtime.SetInput(input,false);runtime.SetMouse(mouse,false);BindNative(owner,object);for(const auto& [field,value]:overrides)runtime.Set(object,field,std::holds_alternative<PrefabRef>(value)?Value{runtime.CreatePrefab(std::get<PrefabRef>(value).asset)}:value);
            printHandler=output;runtime.SetPrintCallback([this](std::string_view text){if(printHandler)printHandler(text);});
            proxies.emplace(owner,object);
            runtime.SetObjectLookup([this](std::string_view name){
                GameObjectId id=0;for(std::size_t i=0;i<scene.Size();++i)if(scene.At(i).Name()==name){if(id)throw std::runtime_error("Ambiguous object name; give scene objects unique names for find().");id=scene.At(i).Id();}
                return Proxy(id);
            });
            runtime.SetTypeLookup([this](std::string_view type){
                for(std::size_t i=0;i<scene.Size();++i)if(ScriptHost::ObjectMatchesReferenceType(scene.At(i),type))return Proxy(scene.At(i).Id());
                return ObjectRef{};
            });
            runtime.SetTagLookup([this](ObjectRef ref)->std::vector<std::string>{
                const auto id=NativeId(ref);const auto* native=scene.Find(id);
                return native?native->Tags():std::vector<std::string>{};
            });
            runtime.SetChildrenLookup([this](ObjectRef ref)->std::vector<ObjectRef>{
                const auto id=NativeId(ref); std::vector<ObjectRef> out;
                if(id) for(std::size_t i=0;i<scene.Size();++i) if(scene.At(i).Parent()==id) out.push_back(Proxy(scene.At(i).Id()));
                return out;
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
            runtime.SetAudioCallback([this](ObjectRef ref,std::string_view method){
                const auto id=NativeId(ref); auto* native=id?scene.Find(id):nullptr;
                auto* source=native?native->GetBehavior<audio::AudioSource>():nullptr;
                if(!source)return;
                if(method=="play")source->Play(); else if(method=="stop")source->Stop();
            });
            runtime.SetAudioAreaCallback([this](ObjectRef ref,std::string_view method,float a,float b){
                const auto id=NativeId(ref); auto* native=id?scene.Find(id):nullptr;
                auto* effect=native?native->GetBehavior<audio::AudioEffect>():nullptr;
                if(!effect)return;
                if(method=="enable")effect->SetEnabled(true);
                else if(method=="disable")effect->SetEnabled(false);
                else if(method=="set_reverb"){effect->SetDecay(a);effect->SetWetMix(b);}
            });
            runtime.SetSceneCallbacks(
                sceneLoader ? std::function<void(std::string_view)>([sceneLoader](std::string_view scene){sceneLoader(scene);}) : std::function<void(std::string_view)>{},
                [scene=std::move(sceneName)]{return scene;});
            for (const auto& [field, id] : references)
                if (id)
                    for (const auto& entry : program->InspectorLayout(name))
                        if (entry.kind == InspectorEntry::Kind::Field && entry.name == field && ReferenceTypeName(entry.type))
                        { runtime.Set(object, field, Reference(id, entry.type)); break; }
            for (const auto& entry : program->InspectorLayout(name))
            {
                if (entry.kind != InspectorEntry::Kind::Field || entry.type != "array") continue;
                const auto found = arrays.find(entry.name);
                if (found == arrays.end() || found->second.empty()) continue;
                const auto arrayRef = std::get<ArrayRef>(runtime.Get(object, entry.name));
                for (const auto& element : found->second)
                {
                    if (!element.referenceType.empty())
                    {
                        script::ObjectRef value{};
                        if (element.reference) try { value = Reference(element.reference, element.referenceType); } catch (const std::exception&) {}
                        runtime.AppendArrayElement(arrayRef, value);
                    }
                    else if (std::holds_alternative<PrefabRef>(element.value))
                        runtime.AppendArrayElement(arrayRef, Value{runtime.CreatePrefab(std::get<PrefabRef>(element.value).asset)});
                    else
                        runtime.AppendArrayElement(arrayRef, element.value);
                }
            }
        }
        bool HasStart() const noexcept override { return true; }
        // Synchronize native transform signals even for a listener with no update body.
        bool HasUpdate() const noexcept override { return true; }
        bool HasDraw() const noexcept override { return draw; }
        bool HasPhysicsUpdate() const noexcept override { return physicsUpdate; }
        void Start(ObjectCore& owner) override { Invoke(owner,[&] { runtime.Start(object); }); }
        void Update(ObjectCore& owner,float delta) override { Invoke(owner,[&] { runtime.SetInput(input); runtime.SetMouse(mouse); runtime.Update(object,delta); }); }
        void Draw(ObjectCore& owner) override { Invoke(owner,[&] { runtime.Draw(object); }); }
        void PhysicsUpdate(ObjectCore& owner,float delta) override { Invoke(owner,[&] { runtime.PhysicsUpdate(object,delta); }); }
        void EmitOwnSignal(std::string_view name) {
            auto* owner=scene.Find(ownerId); if(!owner) return;
            Invoke(*owner,[&]{ runtime.Emit({object,std::string(name)},{}); });
        }
        void PhysicsEvent(ObjectCore& owner,const physics::ContactEvent& event) {
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
            auto* native=scene.Find(id);if(!native)throw std::runtime_error("Scene object no longer exists.");
            // 2D objects (ui controls) get a gameObject2D proxy so their Transform2D
            // and their `parent` chain (also gameObject2D) coerce correctly.
            const auto ref=runtime.Create(As2D(native)?"gameObject2D":"gameObject");proxies.emplace(id,ref);BindNative(id,ref);Synchronize(id,ref);return ref;
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
            auto* native=scene.Find(id);if(!native)throw std::runtime_error("Scene object no longer exists.");
            // A 2D owner parented to another 2D object: its `parent` field is typed
            // gameObject2D but referenced objects get a plain gameObject proxy. Cross-
            // object 2D reparenting from script is descoped for 1.0, so a coercion
            // failure here is expected - don't let it fault the script.
            try { runtime.Set(proxy,"parent",Proxy(native->Parent()),false); } catch(const std::exception&) {}
            if(auto* g=As3D(native)) {
                const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));const auto& transform=g->GetTransform();
                runtime.Set(ref,"position",ToScript(transform.Position()),false);runtime.Set(ref,"rotation",ToScript(transform.Rotation()),false);runtime.Set(ref,"scale",ToScript(transform.Scale()),false);
                return;
            }
            // ZE-96: a 2D object's proxy - Transform2D + its ui:: control fields. Only
            // meaningful when the proxy is a gameObject2D (the script's own owner);
            // a referenced 2D object gets a plain gameObject proxy, so this is skipped.
            if(auto* g2=As2D(native)) {
                try {
                    const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));const auto& t=g2->GetTransform();
                    runtime.Set(ref,"position",Value{ToScript(t.Position())},false);
                    runtime.Set(ref,"rotation",Value{static_cast<double>(t.Rotation())},false);
                    runtime.Set(ref,"scale",Value{ToScript(t.Scale())},false);
                    if(auto* control=native->GetBehavior<ui::UiControl>()) SyncUiControl(runtime,proxy,*control,true);
                } catch(const std::exception&) {} // 3D-transform proxy or control/class mismatch: skip
            }
        }
        template<class F> void Invoke(ObjectCore& owner, F callback)
        {
            (void)owner;
            // Break old proxy links before synchronizing a potentially reparented native graph.
            for(const auto& [id,proxy]:proxies)runtime.Set(proxy,"parent",ObjectRef{},false);
            for(const auto& [id,proxy]:proxies)Synchronize(id,proxy);
            for(const auto& [id,previous]:previousTransforms) {
                const auto* g=As3D(scene.Find(id)); if(!g)continue;
                const auto ref=std::get<ObjectRef>(runtime.Get(proxies.at(id),"transform"));const auto& native=g->GetTransform();
                const auto position=ToScript(native.Position()),rotation=ToScript(native.Rotation()),scale=ToScript(native.Scale());
                if(position!=ToScript(previous.Position()))runtime.Emit({ref,"was_moved"},{position});
                if(rotation!=ToScript(previous.Rotation()))runtime.Emit({ref,"was_rotated"},{rotation});
                if(scale!=ToScript(previous.Scale()))runtime.Emit({ref,"was_scaled"},{scale});
            }
            callback();
            std::map<GameObjectId,GameObjectId> parents;std::map<GameObjectId,Transform> transforms;std::map<GameObjectId,Transform2D> transforms2D;
            for(const auto& [id,proxy]:proxies) {
                auto* obj=scene.Find(id); if(!obj)continue;
                const auto parent=NativeId(std::get<ObjectRef>(runtime.Get(proxy,"parent")));
                if(parent!=obj->Parent())parents[id]=parent;
                if(As3D(obj)) {
                    const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));Transform next;
                    next.SetPosition(ToNative(std::get<Vector3>(runtime.Get(ref,"position"))));next.SetRotation(ToNative(std::get<Vector3>(runtime.Get(ref,"rotation"))));next.SetScale(ToNative(std::get<Vector3>(runtime.Get(ref,"scale"))));transforms[id]=next;
                }
                else if(auto* g2=As2D(obj)) {
                    try {
                        const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));
                        if(std::holds_alternative<Vector2>(runtime.Get(ref,"position"))) {
                            Transform2D next;
                            next.SetPosition(ToNative2(std::get<Vector2>(runtime.Get(ref,"position"))));
                            if(const auto r=runtime.Get(ref,"rotation");std::holds_alternative<double>(r))next.SetRotation(static_cast<float>(std::get<double>(r)));
                            next.SetScale(ToNative2(std::get<Vector2>(runtime.Get(ref,"scale"))));
                            transforms2D[id]=next;
                        }
                        if(auto* control=g2->GetBehavior<ui::UiControl>()) SyncUiControl(runtime,proxy,*control,false);
                    } catch(const std::exception&) {}
                }
            }
            scene.SetParents(parents); // Validate complete graph before committing any transform.
            for(const auto& [id,transform]:transforms)As3D(*scene.Find(id)).GetTransform()=transform;
            for(const auto& [id,transform]:transforms2D) {
                auto& live=As2D(*scene.Find(id)).GetTransform();
                const auto proxy=proxies.at(id);const auto ref=std::get<ObjectRef>(runtime.Get(proxy,"transform"));
                if(const auto it=previousTransforms2D.find(id);it!=previousTransforms2D.end()) {
                    if(transform.Position()!=it->second.Position())runtime.Emit({ref,"was_moved"},{Value{ToScript(transform.Position())}});
                    if(transform.Rotation()!=it->second.Rotation())runtime.Emit({ref,"was_rotated"},{Value{static_cast<double>(transform.Rotation())}});
                    if(transform.Scale()!=it->second.Scale())runtime.Emit({ref,"was_scaled"},{Value{ToScript(transform.Scale())}});
                }
                live=transform;
            }
            previousTransforms=std::move(transforms);
            previousTransforms2D=std::move(transforms2D);
        }
        bool draw,physicsUpdate;
        const InputFrame& input;
        const MouseFrame& mouse;
        ObjectStore& scene;
        GameObjectId ownerId;
        std::map<GameObjectId,ObjectRef> proxies;
        std::map<GameObjectId,Transform> previousTransforms;
        std::map<GameObjectId,Transform2D> previousTransforms2D;
    };
}

bool ScriptHost::IsReferenceType(std::string_view type)
{
    return ReferenceTypeName(type);
}

bool ScriptHost::ObjectMatchesReferenceType(const ObjectCore& object, std::string_view type)
{
    if (type == "gameObject" || type == "Transform") return true;
    if (type == "Behavior") return object.GetBehavior<physics::Body>() != nullptr || object.GetBehavior<physics::Collider>() != nullptr;
    if (const auto* binding = FindBinding(type)) return binding->present(object);
    return false;
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
        std::map<std::string,std::vector<ScriptArrayElement>> keptArrays;
        for (const auto& entry : compiled.program->InspectorLayout(record.className))
            if (entry.kind==script::InspectorEntry::Kind::Field && entry.type=="array")
                if (const auto it=record.arrays.find(entry.name); it!=record.arrays.end() && !it->second.empty())
                    keptArrays.emplace(entry.name, it->second);
        record.arrays=std::move(keptArrays);
        // Seed authoring data from the script's own array initializer (export array a=[1,2,3];)
        // so the inspector shows those elements the way it shows scalar defaults.
        for (const auto& entry : compiled.program->InspectorLayout(record.className))
        {
            if (entry.kind!=script::InspectorEntry::Kind::Field || entry.type!="array" || record.arrays.count(entry.name)) continue;
            const auto arrayRef = std::get<script::ArrayRef>(preview->Get(ref, entry.name));
            const auto n = preview->ArrayLength(arrayRef);
            if (!n) continue;
            std::vector<ScriptArrayElement> seed;
            for (std::size_t i=0;i<n;++i)
            {
                const auto v = preview->ArrayElement(arrayRef, i);
                ScriptArrayElement element;
                if (std::holds_alternative<script::ObjectRef>(v)) element.referenceType = "gameObject"; // runtime object from a literal: an empty slot
                else if (!ValueTypeName(v).empty()) element.value = v;
                else continue;
                seed.push_back(std::move(element));
            }
            if (!seed.empty()) record.arrays.emplace(entry.name, std::move(seed));
        }
        record.overrides=std::move(kept); record.object=ref;
        record.preview=std::move(preview); record.program=compiled.program;
        ApplyPreviewReferences(record);
        for (const auto& [field, elements] : record.arrays) { (void)elements; SyncPreviewArray(record, field); }
        if(playing_){if(!playingObjects_)throw std::logic_error("Missing running object store.");behavior.BindInstance(std::make_unique<BoundScript>(record.program,record.className,record.overrides,record.references,record.arrays,input_,mouse_,*playingObjects_,behavior.Owner().Id(),playingPhysics_,prefabSpawner_,printHandler_,sceneLoader_,sceneName_));}
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
        else if (entry.type=="array")
        {
            const auto& elements = r.arrays[entry.name];
            const auto count = elements.size();
            result.push_back({entry.name,"array",entry.name,std::to_string(count)+(count==1?" element":" elements"),false,false,false,true,-1,count});
            for (std::size_t i=0;i<count;++i)
            {
                const auto& element = elements[i];
                if (!element.referenceType.empty())
                {
                    std::string value = "None";
                    if (element.reference)
                    {
                        if (objectStore_ && objectStore_->Find(element.reference))
                            value = objectStore_->Find(element.reference)->Name()+" ("+element.referenceType+")";
                        else value = "Missing GameObject ("+std::to_string(element.reference)+")";
                    }
                    result.push_back({entry.name,element.referenceType,entry.name,std::move(value),false,false,true,false,static_cast<int>(i),count});
                }
                else
                {
                    auto type = ValueTypeName(element.value);
                    if (type.empty()) type = "int";
                    result.push_back({entry.name,type,entry.name,Format(element.value),true,false,false,false,static_cast<int>(i),count});
                }
            }
        }
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
const std::vector<std::string>& ScriptHost::ArrayElementTypes()
{
    static const std::vector<std::string> types = {
        "int","float","bool","string","char","Vector3","Vector2","prefab",
        "gameObject","Transform","RigidBody","KinematicBody","StaticBody","Area","Collider","Camera","PhysicsBody","Behavior",
    };
    return types;
}
ScriptHost::Record& ScriptHost::ArrayRecord(ScriptBehavior& behavior, const std::string& field)
{
    if (playing_) throw std::logic_error("Stop Play before editing a script array.");
    auto& record = records_.at(&behavior);
    if (!record.program || !record.preview) throw std::logic_error("Prepare the script before editing an array.");
    const auto& layout = record.program->InspectorLayout(record.className);
    const auto found = std::find_if(layout.begin(), layout.end(), [&](const script::InspectorEntry& e){
        return e.kind==script::InspectorEntry::Kind::Field && e.name==field && e.type=="array"; });
    if (found == layout.end()) throw std::invalid_argument("Field is not an exported array.");
    return record;
}
void ScriptHost::SyncPreviewArray(Record& record, const std::string& field)
{
    if (!record.preview || !record.program) return;
    const auto arrayRef = std::get<script::ArrayRef>(record.preview->Get(record.object, field));
    while (record.preview->ArrayLength(arrayRef) > 0)
        record.preview->RemoveArrayElement(arrayRef, record.preview->ArrayLength(arrayRef) - 1);
    const auto found = record.arrays.find(field);
    if (found == record.arrays.end()) return;
    for (const auto& element : found->second)
    {
        if (!element.referenceType.empty())
        {
            script::ObjectRef value{};
            if (element.reference) try { value = PreviewReference(record, element.reference, element.referenceType); } catch (const std::exception&) {}
            record.preview->AppendArrayElement(arrayRef, value);
        }
        else if (std::holds_alternative<script::PrefabRef>(element.value))
            record.preview->AppendArrayElement(arrayRef, script::Value{record.preview->CreatePrefab(std::get<script::PrefabRef>(element.value).asset)});
        else
            record.preview->AppendArrayElement(arrayRef, element.value);
    }
}
void ScriptHost::AddArrayElement(ScriptBehavior& behavior, const std::string& field, const std::string& elementType)
{
    auto& record = ArrayRecord(behavior, field);
    const auto& known = ArrayElementTypes();
    if (std::find(known.begin(), known.end(), elementType) == known.end())
        throw std::invalid_argument("Unknown array element type '" + elementType + "'.");
    ScriptArrayElement element;
    if (elementType=="gameObject" || IsReferenceType(elementType)) element.referenceType = elementType;
    else element.value = DefaultForType(elementType);
    auto& elements = record.arrays[field];
    elements.push_back(std::move(element));
    try { SyncPreviewArray(record, field); }
    catch (...) { elements.pop_back(); if (elements.empty()) record.arrays.erase(field); else SyncPreviewArray(record, field); throw; }
}
void ScriptHost::RemoveArrayElement(ScriptBehavior& behavior, const std::string& field, std::size_t index)
{
    auto& record = ArrayRecord(behavior, field);
    auto& elements = record.arrays[field];
    if (index >= elements.size()) throw std::out_of_range("Array element index is out of range.");
    elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(index));
    if (elements.empty()) record.arrays.erase(field);
    SyncPreviewArray(record, field);
}
void ScriptHost::SetArrayElement(ScriptBehavior& behavior, const std::string& field, std::size_t index, const std::string& text)
{
    auto& record = ArrayRecord(behavior, field);
    auto& elements = record.arrays[field];
    if (index >= elements.size()) throw std::out_of_range("Array element index is out of range.");
    auto& element = elements[index];
    if (!element.referenceType.empty()) throw std::invalid_argument("This element is an object reference; assign it by dragging a scene object.");
    auto type = ValueTypeName(element.value);
    if (type.empty()) type = "int";
    const auto parsed = Parse(type, text);
    const auto previous = element.value;
    element.value = parsed;
    try { SyncPreviewArray(record, field); }
    catch (...) { element.value = previous; SyncPreviewArray(record, field); throw; }
}
void ScriptHost::SetArrayElementReference(ScriptBehavior& behavior, const std::string& field, std::size_t index, GameObjectId target)
{
    auto& record = ArrayRecord(behavior, field);
    auto& elements = record.arrays[field];
    if (index > elements.size()) index = elements.size(); // clamp: append a new slot
    std::string type;
    if (target)
    {
        if (!objectStore_ || !objectStore_->Find(target)) throw std::invalid_argument("The dragged GameObject no longer exists.");
        // Keep the slot's declared type if it already matches; otherwise auto-pick.
        if (index < elements.size() && !elements[index].referenceType.empty()
            && ObjectMatchesReferenceType(*objectStore_->Find(target), elements[index].referenceType))
            type = elements[index].referenceType;
        else type = BestReferenceType(*objectStore_->Find(target));
    }
    else if (index < elements.size()) type = elements[index].referenceType;
    if (type.empty()) type = "gameObject";

    ScriptArrayElement element; element.reference = target; element.referenceType = type;
    std::optional<ScriptArrayElement> previous;
    if (index < elements.size()) { previous = elements[index]; elements[index] = std::move(element); }
    else elements.push_back(std::move(element));
    try { SyncPreviewArray(record, field); }
    catch (...)
    {
        if (previous) elements[index] = *previous; else elements.pop_back();
        if (elements.empty()) record.arrays.erase(field); else SyncPreviewArray(record, field);
        throw;
    }
}
void ScriptHost::SetField(ScriptBehavior& behavior, const std::string& name, const std::string& text)
{
    auto& r=records_.at(&behavior);
    for (const auto& field:Fields(behavior)) if (field.name==name && field.editable && field.arrayIndex<0 && !field.array)
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
std::map<std::string,std::vector<ScriptArrayElement>> ScriptHost::AuthoredArrays(const ScriptBehavior& behavior) const
{
    const auto it=records_.find(&behavior);
    if (it==records_.end()) return {};
    std::map<std::string,std::vector<ScriptArrayElement>> result;
    for (const auto& [field,elements] : it->second.arrays) if (!elements.empty()) result.emplace(field,elements);
    return result;
}
void ScriptHost::RestoreArrays(ScriptBehavior& behavior, std::map<std::string,std::vector<ScriptArrayElement>> arrays)
{
    auto found=records_.find(&behavior);
    if (found!=records_.end() && found->second.program)
        throw std::logic_error("Restore scene arrays before compiling or playing.");
    records_[&behavior].arrays=std::move(arrays);
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
            try { ready.emplace_back(script,std::make_unique<BoundScript>(r.program,r.className,r.overrides,r.references,r.arrays,input_,mouse_,objects,script->Owner().Id(),physicsWorld,prefabSpawner_,printHandler_,sceneLoader_,sceneName_)); }
            catch (const std::exception& e) { r.error=e.what(); return false; }
        }
    transforms_.clear();
    parents_.clear();
    for (std::size_t i=0;i<objects.Size();++i) if(auto* g=As3D(&objects.At(i))) transforms_.emplace(g->Id(),g->GetTransform());
    for (std::size_t i=0;i<objects.Size();++i) parents_.emplace(objects.At(i).Id(),objects.At(i).Parent());
    playing_=true;playingObjects_=&objects;playingPhysics_=physicsWorld;
    for (auto& [behavior,instance]:ready) behavior->BindInstance(std::move(instance));
    return true;
}
void ScriptHost::DispatchPhysicsEvents(const std::vector<physics::ContactEvent>& events)
{
    if(!playing_)return;
    for(const auto& event:events)for(const auto& [behavior,_]:records_)if(behavior->Owner().Id()==event.receiver)
        if(auto* live=dynamic_cast<BoundScript*>(const_cast<ScriptBehavior*>(behavior)->Instance()))live->PhysicsEvent(const_cast<ObjectCore&>(behavior->Owner()),event);
}
void ScriptHost::EmitSignal(GameObjectId owner, std::string_view signal)
{
    if(!playing_) return;
    for(const auto& [behavior,_]:records_)
        if(behavior->Owner().Id()==owner)
            if(auto* live=dynamic_cast<BoundScript*>(const_cast<ScriptBehavior*>(behavior)->Instance()))
                live->EmitOwnSignal(signal);
}
void ScriptHost::Stop(ObjectStore& objects)
{
    if (!playing_) return;
    for (std::size_t i=0;i<objects.Size();++i)
    {
        auto& object=objects.At(i);
        for (std::size_t j=0;j<object.BehaviorCount();++j)
            if (auto* script=dynamic_cast<ScriptBehavior*>(&object.BehaviorAt(j))) script->BindInstance(nullptr);
        if (auto* g=As3D(&object)) if (auto it=transforms_.find(g->Id()); it!=transforms_.end()) g->GetTransform()=it->second;
    }
    objects.SetParents(parents_);parents_.clear();transforms_.clear(); playing_=false;playingObjects_=nullptr;playingPhysics_=nullptr;
}
}
