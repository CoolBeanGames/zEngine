#include "Scene.h"
#include "zscript/Text.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "physics/PhysicsBehavior.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace zengine::scenes
{
namespace
{
    const char* KindName(BehaviorData::Kind kind) {
        switch(kind){case BehaviorData::Kind::Mesh:return "mesh";case BehaviorData::Kind::Script:return "script";case BehaviorData::Kind::Collider:return "collider";case BehaviorData::Kind::RigidBody:return "rigid_body";case BehaviorData::Kind::KinematicBody:return "kinematic_body";case BehaviorData::Kind::StaticBody:return "static_body";case BehaviorData::Kind::Area:return "area";case BehaviorData::Kind::Camera:return "camera";}return "";
    }
    void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
    void Token(std::istream& in,const char* expected) { std::string word; Require(static_cast<bool>(in>>word) && word==expected,"Invalid scene structure."); }
    std::size_t Count(std::istream& in,std::size_t limit)
    { std::uint64_t n; Require(static_cast<bool>(in>>n) && n<=limit,"Invalid/excessive scene count."); return static_cast<std::size_t>(n); }
    std::string Text(std::istream& in)
    {
        in>>std::ws; Require(in.peek()=='"',"Expected quoted scene text.");
        std::string text; Require(static_cast<bool>(in>>std::quoted(text)) && text.size()<=1024*1024 && text.find('\0')==std::string::npos,"Invalid scene text."); return text;
    }
    double Number(std::istream& in)
    { double v; Require(static_cast<bool>(in>>v) && std::isfinite(v),"Scene number must be finite."); return v; }
    float Float(std::istream& in)
    { const double v=Number(in); Require(std::abs(v)<=std::numeric_limits<float>::max(),"Scene number exceeds float range."); return static_cast<float>(v); }
    Vec3 Vector(std::istream& in) { const float x=Float(in),y=Float(in),z=Float(in); return {x,y,z}; }
    bool Boolean(std::istream& in) { return Count(in,1)!=0; }
    void Asset(const std::string& value,bool mesh)
    {
        if (mesh && (value.empty() || value==MeshRenderer::CubeAsset)) return;
        Require(!value.empty() && value.front()!='/' && value.find_first_of("\\:\r\n")==std::string::npos,"Scene asset references must be project-relative.");
        std::istringstream parts(value); std::string part;
        while (std::getline(parts,part,'/')) Require(!part.empty() && part!=".." && part!=".","Invalid scene asset path.");
        Require(value.back()!='/',"Invalid scene asset path.");
    }
    void WriteValue(std::ostream& out,const script::Value& value)
    {
        if (const auto* v=std::get_if<std::int64_t>(&value)) out<<"int "<<*v;
        else if (const auto* v=std::get_if<double>(&value)) { Require(std::isfinite(*v),"Invalid script value."); out<<"float "<<*v; }
        else if (const auto* v=std::get_if<bool>(&value)) out<<"bool "<<(*v?1:0);
        else if (const auto* v=std::get_if<std::string>(&value)) out<<"string "<<std::quoted(*v);
        else if(const auto* v=std::get_if<char32_t>(&value)){Require(script::text::Scalar(*v),"Invalid character.");out<<"char "<<static_cast<std::uint32_t>(*v);}
        else if (const auto* v=std::get_if<script::Vector3>(&value))
        { Require(std::isfinite(v->x)&&std::isfinite(v->y)&&std::isfinite(v->z),"Invalid script vector."); out<<"Vector3 "<<v->x<<' '<<v->y<<' '<<v->z; }
        else if(const auto* v=std::get_if<script::PrefabRef>(&value)){if(!v->asset.empty()){Asset(v->asset,false);Require(v->asset.ends_with(".zprefab"),"Expected prefab script field asset.");}out<<"prefab "<<std::quoted(v->asset);}
        else throw std::runtime_error("Cannot persist a runtime object reference.");
    }
    script::Value ReadValue(std::istream& in)
    {
        std::string type; in>>type;
        if (type=="int") { std::int64_t v; Require(static_cast<bool>(in>>v),"Invalid script integer."); return v; }
        if (type=="float") return Number(in);
        if (type=="bool") return Boolean(in);
        if (type=="string") return Text(in);
        if(type=="char"){std::uint32_t value;Require(static_cast<bool>(in>>value) && script::text::Scalar(static_cast<char32_t>(value)),"Invalid character.");return static_cast<char32_t>(value);}
        if (type=="Vector3") { const double x=Number(in),y=Number(in),z=Number(in); return script::Vector3{x,y,z}; }
        if(type=="prefab"){auto asset=Text(in);if(!asset.empty()){Asset(asset,false);Require(asset.ends_with(".zprefab"),"Expected prefab script field asset.");}return script::PrefabRef{std::move(asset)};}
        throw std::runtime_error("Unsupported scene variable type.");
    }
}
Document Capture(const ObjectStore& objects,const ScriptHost& scripts)
{
    Require(!scripts.Playing(),"Stop Play before saving a scene.");
    Document result;
    for (std::size_t i=0;i<objects.Size();++i)
    {
        const auto& object=objects.At(i);
        ObjectData data{object.Id(),object.Name(),object.Tags(),object.GetTransform(),{}};
        data.parent=object.Parent();
        for (std::size_t j=0;j<object.BehaviorCount();++j)
        {
            const auto& behavior=object.BehaviorAt(j);
            BehaviorData b; b.enabled=behavior.Enabled(); b.priority=behavior.Priority();
            if (const auto* mesh=dynamic_cast<const MeshRenderer*>(&behavior)) b.asset=mesh->Asset();
            else if (const auto* script=dynamic_cast<const ScriptBehavior*>(&behavior))
            { b.kind=BehaviorData::Kind::Script; b.asset=script->Asset(); b.variables=scripts.AuthoredValues(*script); b.objectReferences=scripts.AuthoredReferences(*script); }
            else if(const auto* collider=dynamic_cast<const physics::Collider*>(&behavior)){b.kind=BehaviorData::Kind::Collider;b.shape=collider->Shape();b.colliderOffset=collider->Offset();b.colliderSize=collider->Size();}
            else if(const auto* camera=dynamic_cast<const Camera*>(&behavior)){b.kind=BehaviorData::Kind::Camera;b.cameraFov=camera->FieldOfView();b.cameraNear=camera->NearPlane();b.cameraFar=camera->FarPlane();}
            else if(const auto* body=dynamic_cast<const physics::Body*>(&behavior)) {
                b.layer=body->Layer();b.mask=body->Mask();b.friction=body->Friction();b.bounciness=body->Bounciness();
                if(const auto* rigid=dynamic_cast<const physics::RigidBody*>(body)){b.kind=BehaviorData::Kind::RigidBody;b.mass=rigid->Mass();b.gravityScale=rigid->GravityScale();}
                else if(dynamic_cast<const physics::KinematicBody*>(body))b.kind=BehaviorData::Kind::KinematicBody;
                else if(dynamic_cast<const physics::StaticBody*>(body))b.kind=BehaviorData::Kind::StaticBody;
                else b.kind=BehaviorData::Kind::Area;
                if(const auto* moving=dynamic_cast<const physics::MovingBody*>(body)){b.velocity=moving->Velocity();b.angularVelocity=moving->AngularVelocity();b.constantForce=moving->ConstantForce();b.constantTorque=moving->ConstantTorque();}
            }
            else throw std::runtime_error("This native behavior has no scene serializer.");
            data.behaviors.push_back(std::move(b));
        }
        result.objects.push_back(std::move(data));
    }
    return result;
}
std::string Encode(const Document& scene)
{
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(17);
    out<<"ZENGINE_SCENE 6\nobjects "<<scene.objects.size()<<'\n';
    for (const auto& object:scene.objects)
    {
        out<<"object "<<object.id<<' '<<std::quoted(object.name)<<"\ntags "<<object.tags.size();
        for (const auto& tag:object.tags) out<<' '<<std::quoted(tag);
        out<<"\ntransform";
        for (const auto v:{object.transform.Position(),object.transform.Rotation(),object.transform.Scale()}) out<<' '<<v.x<<' '<<v.y<<' '<<v.z;
        out<<"\nparent "<<object.parent<<"\nprefab "<<std::quoted(object.prefab)<<' '<<(object.transformMask?object.transformMask:object.transformOverride?511:0)<<' '<<object.prefabDataMask;
        out<<"\nbehaviors "<<object.behaviors.size()<<'\n';
        for (const auto& b:object.behaviors)
        {
            out<<KindName(b.kind)<<' '<<(b.enabled?1:0)<<' '<<b.priority<<' '<<std::quoted(b.asset)<<'\n';
            if(b.kind==BehaviorData::Kind::Collider)out<<"collider_shape "<<static_cast<unsigned>(b.shape)<<' '<<b.colliderOffset.x<<' '<<b.colliderOffset.y<<' '<<b.colliderOffset.z<<' '<<b.colliderSize.x<<' '<<b.colliderSize.y<<' '<<b.colliderSize.z<<'\n';
            else if(b.kind==BehaviorData::Kind::Camera)out<<"camera "<<b.cameraFov<<' '<<b.cameraNear<<' '<<b.cameraFar<<'\n';
            else if(b.kind!=BehaviorData::Kind::Mesh && b.kind!=BehaviorData::Kind::Script) {
                out<<"body "<<b.layer<<' '<<b.mask<<' '<<b.friction<<' '<<b.bounciness<<' '<<b.mass<<' '<<b.gravityScale;
                for(auto v:{b.velocity,b.angularVelocity,b.constantForce,b.constantTorque})out<<' '<<v.x<<' '<<v.y<<' '<<v.z;out<<'\n';
            }
            out<<"variables "<<b.variables.size()<<'\n';
            for (const auto& [name,value]:b.variables) { out<<"field "<<std::quoted(name)<<' '; WriteValue(out,value); out<<'\n'; }
            if (!b.objectReferences.empty())
            {
                out<<"references "<<b.objectReferences.size()<<'\n';
                for (const auto& [name,id]:b.objectReferences)
                {
                    Require(!name.empty() && id,"Invalid script object reference.");
                    out<<"reference "<<std::quoted(name)<<' '<<id<<'\n';
                }
            }
        }
    }
    out<<"end\n";
    auto text=out.str();
    Decode(text); // Apply identical limits/validation to generated and loaded scenes.
    return text;
}
Document Decode(std::string_view text)
{
    Require(text.size()<=MaxSceneBytes,"Scene exceeds the 8 MiB limit.");
    std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
    Token(in,"ZENGINE_SCENE"); const auto version=Count(in,6); Require(version>=1,"Unsupported scene version."); Token(in,"objects");
    Document scene; const auto count=Count(in,10000); std::set<GameObjectId> ids;
    for (std::size_t i=0;i<count;++i)
    {
        ObjectData object; Token(in,"object");
        Require(static_cast<bool>(in>>object.id) && object.id && object.id!=std::numeric_limits<GameObjectId>::max() && ids.insert(object.id).second,"Invalid/duplicate object ID.");
        object.name=Text(in); Require(object.name.find_first_not_of(" \t\r\n")!=std::string::npos,"Scene object needs a name.");
        Token(in,"tags"); const auto tags=Count(in,256);
        for (std::size_t j=0;j<tags;++j) object.tags.push_back(Text(in));
        Token(in,"transform"); object.transform.SetPosition(Vector(in)); object.transform.SetRotation(Vector(in)); object.transform.SetScale(Vector(in));
        if (version>=2)
        {
            Token(in,"parent"); Require(static_cast<bool>(in>>object.parent),"Invalid parent ID.");
            Token(in,"prefab"); object.prefab=Text(in);
            if(version>=3){object.transformMask=static_cast<unsigned>(Count(in,511));object.transformOverride=object.transformMask!=0;}
            else object.transformOverride=Boolean(in);
            if(version>=5)object.prefabDataMask=static_cast<unsigned>(Count(in,7));
            if (!object.prefab.empty()) { Asset(object.prefab,false); Require(object.prefab.ends_with(".zprefab"),"Expected prefab reference."); }
            else Require(!object.transformOverride && !object.prefabDataMask,"Only prefab instances can override inherited data.");
        }
        Token(in,"behaviors"); const auto behaviors=Count(in,256); bool mesh=false,collider=false,body=false,camera=false;
        for (std::size_t j=0;j<behaviors;++j)
        {
            BehaviorData b; std::string type; in>>type;
            Require(type=="mesh" || type=="script" || (version>=4 && (type=="collider"||type=="rigid_body"||type=="kinematic_body"||type=="static_body"||type=="area")) || (version>=6 && type=="camera"),"Unknown scene behavior type.");
            if (type=="mesh") { Require(!mesh,"Duplicate Mesh Renderer."); mesh=true; }
            else if(type=="script")b.kind=BehaviorData::Kind::Script;else if(type=="collider")b.kind=BehaviorData::Kind::Collider;else if(type=="camera")b.kind=BehaviorData::Kind::Camera;else if(type=="rigid_body")b.kind=BehaviorData::Kind::RigidBody;else if(type=="kinematic_body")b.kind=BehaviorData::Kind::KinematicBody;else if(type=="static_body")b.kind=BehaviorData::Kind::StaticBody;else b.kind=BehaviorData::Kind::Area;
            if(type=="collider"){Require(!collider,"Duplicate Collider.");collider=true;}else if(type=="camera"){Require(!camera,"Duplicate Camera.");camera=true;}else if(type=="rigid_body"||type=="kinematic_body"||type=="static_body"||type=="area"){Require(!body,"Duplicate physics body type.");body=true;}
            b.enabled=Boolean(in); b.priority=Float(in); b.asset=Text(in);if(type=="mesh"||type=="script")Asset(b.asset,type=="mesh");else Require(b.asset.empty(),"Native behaviors cannot reference assets.");
            if(type=="collider"){Token(in,"collider_shape");b.shape=static_cast<physics::ColliderShape>(Count(in,2));if(version>=5){b.colliderOffset=Vector(in);b.colliderSize=Vector(in);Require(b.colliderSize.x>0&&b.colliderSize.y>0&&b.colliderSize.z>0,"Invalid collider size.");}}
            else if(type=="camera"){Token(in,"camera");b.cameraFov=Float(in);b.cameraNear=Float(in);b.cameraFar=Float(in);Require(b.cameraFov>0&&b.cameraFov<180&&b.cameraNear>0&&b.cameraFar>b.cameraNear,"Invalid camera settings.");}
            else if(type!="mesh"&&type!="script"){Token(in,"body");b.layer=static_cast<std::uint32_t>(Count(in,0xffffffffu));b.mask=static_cast<std::uint32_t>(Count(in,0xffffffffu));b.friction=Float(in);b.bounciness=Float(in);b.mass=Float(in);b.gravityScale=Float(in);b.velocity=Vector(in);b.angularVelocity=Vector(in);b.constantForce=Vector(in);b.constantTorque=Vector(in);Require(b.friction>=0&&b.bounciness>=0&&b.bounciness<=1&&b.mass>0,"Invalid physics body settings.");}
            Token(in,"variables"); const auto fields=Count(in,1024); Require(type=="script" || fields==0,"Only scripts can contain fields.");
            for (std::size_t k=0;k<fields;++k)
            { Token(in,"field"); auto name=Text(in); auto value=ReadValue(in); Require(!name.empty() && b.variables.emplace(std::move(name),std::move(value)).second,"Duplicate/empty scene variable."); }
            in>>std::ws;
            const auto markerPosition=in.tellg(); std::string marker;
            if (in>>marker)
            {
                if (marker=="references")
                {
                    Require(type=="script","Only scripts can contain object references.");
                    const auto references=Count(in,1024);
                    for (std::size_t k=0;k<references;++k)
                    {
                        Token(in,"reference"); auto name=Text(in); GameObjectId id=0;
                        Require(static_cast<bool>(in>>id) && id,"Invalid script object reference.");
                        Require(!name.empty() && b.objectReferences.emplace(std::move(name),id).second,"Duplicate/empty script object reference.");
                    }
                }
                else { in.clear(); in.seekg(markerPosition); Require(static_cast<bool>(in),"Invalid scene stream position."); }
            }
            object.behaviors.push_back(std::move(b));
        }
        scene.objects.push_back(std::move(object));
    }
    Token(in,"end"); in>>std::ws; Require(in.eof(),"Unexpected trailing scene data.");
    std::map<GameObjectId,GameObjectId> parents;
    for (const auto& object:scene.objects)
    {
        Require(!object.parent || ids.contains(object.parent),"Missing parent object.");
        if (!object.prefab.empty()) {
            Require((object.prefabDataMask&2) || object.tags.empty(),"Prefab instance tags require an override flag.");
            Require((object.prefabDataMask&4) || object.behaviors.empty(),"Prefab instance behaviors require an override flag.");
        }
        parents.emplace(object.id,object.parent);
    }
    for (const auto& object:scene.objects)
    {
        auto parent=object.parent; std::size_t depth=0;
        while (parent) { Require(parent!=object.id && ++depth<=64,"Cyclic or excessively deep object hierarchy."); parent=parents.at(parent); }
    }
    return scene;
}
Instance Instantiate(const Document& scene)
{
    Instance instance;
    for (const auto& data:scene.objects)
    {
        auto& object=instance.objects.Restore(data.id,data.name); object.SetTags(data.tags); object.GetTransform()=data.transform;
        Require(data.prefab.empty(),"Resolve prefab references before instantiating scene data.");
        for (const auto& b:data.behaviors)
        {
            Behavior* behavior=nullptr;
            if (b.kind==BehaviorData::Kind::Mesh) behavior=&object.AddBehavior<MeshRenderer>(b.asset);
            else if(b.kind==BehaviorData::Kind::Script) { auto& script=object.AddBehavior<ScriptBehavior>(b.asset); instance.scripts.RestoreValues(script,b.variables); instance.scripts.RestoreReferences(script,b.objectReferences); behavior=&script; }
            else if(b.kind==BehaviorData::Kind::Collider){auto* existing=object.GetBehavior<physics::Collider>();auto& v=existing?*existing:object.AddBehavior<physics::Collider>();v.SetShape(b.shape);v.SetOffset(b.colliderOffset);v.SetSize(b.colliderSize);behavior=&v;}
            else if(b.kind==BehaviorData::Kind::Camera){auto& v=object.AddBehavior<Camera>();v.SetFieldOfView(b.cameraFov);v.SetNearPlane(b.cameraNear);v.SetFarPlane(b.cameraFar);behavior=&v;}
            else {physics::Body* body=nullptr;if(b.kind==BehaviorData::Kind::RigidBody){auto& v=object.AddBehavior<physics::RigidBody>();v.SetMass(b.mass);v.SetGravityScale(b.gravityScale);body=&v;}else if(b.kind==BehaviorData::Kind::KinematicBody)body=&object.AddBehavior<physics::KinematicBody>();else if(b.kind==BehaviorData::Kind::StaticBody)body=&object.AddBehavior<physics::StaticBody>();else body=&object.AddBehavior<physics::Area>();body->SetLayer(b.layer);body->SetMask(b.mask);body->SetFriction(b.friction);body->SetBounciness(b.bounciness);if(auto* moving=dynamic_cast<physics::MovingBody*>(body)){moving->SetVelocity(b.velocity);moving->SetAngularVelocity(b.angularVelocity);moving->SetConstantForce(b.constantForce);moving->SetConstantTorque(b.constantTorque);}behavior=body;}
            behavior->SetEnabled(b.enabled); behavior->SetPriority(b.priority);
        }
    }
    std::map<GameObjectId,GameObjectId> parents;for(const auto& data:scene.objects)parents[data.id]=data.parent;instance.objects.SetParents(parents);
    return instance;
}
GameObjectId Append(const Document& scene,ObjectStore& objects,ScriptHost& scripts,GameObjectId parent)
{
    Require(!scene.objects.empty(),"Cannot spawn an empty prefab.");
    Require(objects.Size()+scene.objects.size()<=10000,"The live scene object limit was exceeded.");
    if(parent)Require(objects.Find(parent)!=nullptr,"Cannot spawn under a missing parent.");
    std::map<GameObjectId,GameObjectId> remap;
    GameObjectId root=0;
    std::vector<std::pair<ScriptBehavior*,const std::map<std::string,GameObjectId>*>> pendingReferences;
    for(const auto& data:scene.objects)
    {
        Require(data.prefab.empty(),"Resolve prefab references before spawning scene data.");
        auto& object=objects.Create(data.name);object.SetTags(data.tags);object.GetTransform()=data.transform;
        remap.emplace(data.id,object.Id());
        if(!data.parent && !root)root=object.Id();
        for(const auto& b:data.behaviors)
        {
            Behavior* behavior=nullptr;
            if(b.kind==BehaviorData::Kind::Mesh)behavior=&object.AddBehavior<MeshRenderer>(b.asset);
            else if(b.kind==BehaviorData::Kind::Script){auto& value=object.AddBehavior<ScriptBehavior>(b.asset);scripts.RestoreValues(value,b.variables);pendingReferences.emplace_back(&value,&b.objectReferences);behavior=&value;}
            else if(b.kind==BehaviorData::Kind::Collider){auto* existing=object.GetBehavior<physics::Collider>();auto& value=existing?*existing:object.AddBehavior<physics::Collider>();value.SetShape(b.shape);value.SetOffset(b.colliderOffset);value.SetSize(b.colliderSize);behavior=&value;}
            else if(b.kind==BehaviorData::Kind::Camera){auto& value=object.AddBehavior<Camera>();value.SetFieldOfView(b.cameraFov);value.SetNearPlane(b.cameraNear);value.SetFarPlane(b.cameraFar);behavior=&value;}
            else {physics::Body* body=nullptr;if(b.kind==BehaviorData::Kind::RigidBody){auto& value=object.AddBehavior<physics::RigidBody>();value.SetMass(b.mass);value.SetGravityScale(b.gravityScale);body=&value;}else if(b.kind==BehaviorData::Kind::KinematicBody)body=&object.AddBehavior<physics::KinematicBody>();else if(b.kind==BehaviorData::Kind::StaticBody)body=&object.AddBehavior<physics::StaticBody>();else body=&object.AddBehavior<physics::Area>();body->SetLayer(b.layer);body->SetMask(b.mask);body->SetFriction(b.friction);body->SetBounciness(b.bounciness);if(auto* moving=dynamic_cast<physics::MovingBody*>(body)){moving->SetVelocity(b.velocity);moving->SetAngularVelocity(b.angularVelocity);moving->SetConstantForce(b.constantForce);moving->SetConstantTorque(b.constantTorque);}behavior=body;}
            behavior->SetEnabled(b.enabled);behavior->SetPriority(b.priority);
        }
    }
    // Prefab-internal object references point at the prefab's own object IDs; translate
    // them to the freshly created live IDs. References that leave the prefab are dropped.
    for(const auto& [behavior,references]:pendingReferences)
    {
        std::map<std::string,GameObjectId> remapped;
        for(const auto& [field,id]:*references)
            if(const auto it=remap.find(id);it!=remap.end())remapped.emplace(field,it->second);
        scripts.RestoreReferences(*behavior,std::move(remapped));
    }
    for(const auto& data:scene.objects)
    {
        const auto child=remap.at(data.id);
        const auto target=data.parent?remap.at(data.parent):parent;
        if(target)objects.Find(child)->SetParent(target);
    }
    return root;
}
}
