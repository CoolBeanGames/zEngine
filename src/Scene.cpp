#include "Scene.h"
#include "zscript/Text.h"
#include "core/MeshRenderer.h"
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
            { b.kind=BehaviorData::Kind::Script; b.asset=script->Asset(); b.variables=scripts.AuthoredValues(*script); }
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
    out<<"ZENGINE_SCENE 3\nobjects "<<scene.objects.size()<<'\n';
    for (const auto& object:scene.objects)
    {
        out<<"object "<<object.id<<' '<<std::quoted(object.name)<<"\ntags "<<object.tags.size();
        for (const auto& tag:object.tags) out<<' '<<std::quoted(tag);
        out<<"\ntransform";
        for (const auto v:{object.transform.Position(),object.transform.Rotation(),object.transform.Scale()}) out<<' '<<v.x<<' '<<v.y<<' '<<v.z;
        out<<"\nparent "<<object.parent<<"\nprefab "<<std::quoted(object.prefab)<<' '<<(object.transformMask?object.transformMask:object.transformOverride?511:0);
        out<<"\nbehaviors "<<object.behaviors.size()<<'\n';
        for (const auto& b:object.behaviors)
        {
            out<<(b.kind==BehaviorData::Kind::Mesh?"mesh ":"script ")<<(b.enabled?1:0)<<' '<<b.priority<<' '<<std::quoted(b.asset)<<'\n';
            out<<"variables "<<b.variables.size()<<'\n';
            for (const auto& [name,value]:b.variables) { out<<"field "<<std::quoted(name)<<' '; WriteValue(out,value); out<<'\n'; }
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
    Token(in,"ZENGINE_SCENE"); const auto version=Count(in,3); Require(version>=1,"Unsupported scene version."); Token(in,"objects");
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
            if (!object.prefab.empty()) { Asset(object.prefab,false); Require(object.prefab.ends_with(".zprefab"),"Expected prefab reference."); }
            else Require(!object.transformOverride,"Only prefab instances can override inherited transforms.");
        }
        Token(in,"behaviors"); const auto behaviors=Count(in,256); bool mesh=false;
        for (std::size_t j=0;j<behaviors;++j)
        {
            BehaviorData b; std::string type; in>>type;
            Require(type=="mesh" || type=="script","Unknown scene behavior type.");
            if (type=="mesh") { Require(!mesh,"Duplicate Mesh Renderer."); mesh=true; } else b.kind=BehaviorData::Kind::Script;
            b.enabled=Boolean(in); b.priority=Float(in); b.asset=Text(in); Asset(b.asset,type=="mesh");
            Token(in,"variables"); const auto fields=Count(in,1024); Require(type=="script" || fields==0,"Mesh cannot contain script fields.");
            for (std::size_t k=0;k<fields;++k)
            { Token(in,"field"); auto name=Text(in); auto value=ReadValue(in); Require(!name.empty() && b.variables.emplace(std::move(name),std::move(value)).second,"Duplicate/empty scene variable."); }
            object.behaviors.push_back(std::move(b));
        }
        scene.objects.push_back(std::move(object));
    }
    Token(in,"end"); in>>std::ws; Require(in.eof(),"Unexpected trailing scene data.");
    std::map<GameObjectId,GameObjectId> parents;
    for (const auto& object:scene.objects)
    {
        Require(!object.parent || ids.contains(object.parent),"Missing parent object.");
        if (!object.prefab.empty()) Require(object.behaviors.empty() && object.tags.empty(),"Prefab instances inherit their data from the asset.");
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
            else { auto& script=object.AddBehavior<ScriptBehavior>(b.asset); instance.scripts.RestoreValues(script,b.variables); behavior=&script; }
            behavior->SetEnabled(b.enabled); behavior->SetPriority(b.priority);
        }
    }
    std::map<GameObjectId,GameObjectId> parents;for(const auto& data:scene.objects)parents[data.id]=data.parent;instance.objects.SetParents(parents);
    return instance;
}
}
