#include "physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <cstdarg>

namespace zengine::physics {
namespace {
constexpr JPH::ObjectLayer NonMoving = 0, Moving = 1, LayerCount = 2;
constexpr JPH::BroadPhaseLayer BPNonMoving(0), BPMoving(1);
constexpr float Pi = 3.14159265358979323846f;

struct JoltLifetime {
    static void Trace(const char* format,...){va_list args;va_start(args,format);std::vfprintf(stderr,format,args);std::fputc('\n',stderr);va_end(args);}
    static bool Assert(const char* expression,const char* message,const char* file,JPH::uint line){std::fprintf(stderr,"Jolt assertion %s:%u: %s (%s)\n",file,line,expression,message?message:"");return false;}
    JoltLifetime() { JPH::RegisterDefaultAllocator();JPH::Trace=Trace;JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed=Assert;) JPH::Factory::sInstance=new JPH::Factory; JPH::RegisterTypes(); }
    ~JoltLifetime() { JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance=nullptr; }
};
JoltLifetime& Lifetime() { static JoltLifetime lifetime; return lifetime; }

class BroadPhase final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return layer==NonMoving?BPNonMoving:BPMoving; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer==BPNonMoving?"NON_MOVING":"MOVING"; }
#endif
};
class ObjectBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};
class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};
JPH::Vec3 JV(Vec3 v) { return {v.x,v.y,v.z}; }
Vec3 ZV(JPH::Vec3Arg v) { return {v.GetX(),v.GetY(),v.GetZ()}; }
JPH::Quat Q(Vec3 degrees) { return JPH::Quat::sEulerAngles(JV(Vec3{degrees.x*Pi/180,degrees.y*Pi/180,degrees.z*Pi/180})); }
Vec3 Degrees(JPH::QuatArg q) { auto v=q.GetEulerAngles(); return {v.GetX()*180/Pi,v.GetY()*180/Pi,v.GetZ()*180/Pi}; }
Vec3 Mul(Vec3 a,Vec3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }

struct Pose { JPH::RVec3 position; JPH::Quat rotation; Vec3 scale{1,1,1}; };
Pose GlobalPose(const ObjectStore& objects,const GameObject& object,unsigned depth=0) {
    if(depth>64)throw std::runtime_error("Physics hierarchy exceeds 64 levels.");
    const auto& t=object.GetTransform();Pose local{JPH::RVec3(JV(t.Position())),Q(t.Rotation()),t.Scale()};
    if(!object.Parent())return local;
    const auto* parent=objects.Find(object.Parent());if(!parent)throw std::runtime_error("Physics object has a missing parent.");
    const auto p=GlobalPose(objects,*parent,depth+1);
    local.position=p.position+p.rotation*JV(Mul(t.Position(),p.scale));
    local.rotation=p.rotation*local.rotation;local.scale=Mul(p.scale,local.scale);return local;
}
void SetFromGlobal(const ObjectStore& objects,GameObject& object,JPH::RVec3Arg position,JPH::QuatArg rotation) {
    if(!object.Parent()){object.GetTransform().SetPosition(ZV(JPH::Vec3(position)));object.GetTransform().SetRotation(Degrees(rotation));return;}
    const auto* parent=objects.Find(object.Parent());if(!parent)return;const auto p=GlobalPose(objects,*parent);
    auto relative=p.rotation.Conjugated()*(position-p.position);Vec3 local=ZV(JPH::Vec3(relative));
    if(std::abs(p.scale.x)>1e-6f)local.x/=p.scale.x;if(std::abs(p.scale.y)>1e-6f)local.y/=p.scale.y;if(std::abs(p.scale.z)>1e-6f)local.z/=p.scale.z;
    object.GetTransform().SetPosition(local);object.GetTransform().SetRotation(Degrees(p.rotation.Conjugated()*rotation));
}
const Body* NativeBody(const GameObject& object) {
    if(auto* p=object.GetBehavior<RigidBody>())return p;if(auto* p=object.GetBehavior<KinematicBody>())return p;
    if(auto* p=object.GetBehavior<StaticBody>())return p;return object.GetBehavior<Area>();
}
Body* NativeBody(GameObject& object) { return const_cast<Body*>(NativeBody(std::as_const(object))); }
std::pair<GameObjectId,GameObjectId> Pair(GameObjectId a,GameObjectId b){return a<b?std::pair{a,b}:std::pair{b,a};}
}

struct World::Impl final : JPH::ContactListener {
    struct Entry { JPH::BodyID id; Body* body{}; Vec3 offset{}; bool area=false; };
    JoltLifetime& lifetime=Lifetime();
    BroadPhase broadPhase;ObjectBroadPhaseFilter bpFilter;ObjectPairFilter pairFilter;
    JPH::PhysicsSystem system;JPH::TempAllocatorImpl allocator{16*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    std::map<GameObjectId,Entry> entries;std::map<JPH::BodyID,GameObjectId> reverse;
    std::set<std::pair<GameObjectId,GameObjectId>> contacts,seen;
    std::vector<ContactEvent> events;mutable std::mutex mutex;
    Impl() { system.Init(16384,0,32768,10240,broadPhase,bpFilter,pairFilter);system.SetContactListener(this); }
    ~Impl(){auto& bi=system.GetBodyInterface();for(auto& [_,e]:entries){bi.RemoveBody(e.id);bi.DestroyBody(e.id);}}
    bool Allowed(GameObjectId a,GameObjectId b) const {
        const auto ia=entries.find(a),ib=entries.find(b);if(ia==entries.end()||ib==entries.end())return false;
        return (ia->second.body->Mask()&ib->second.body->Layer()) || (ib->second.body->Mask()&ia->second.body->Layer());
    }
    JPH::ValidateResult OnContactValidate(const JPH::Body& a,const JPH::Body& b,JPH::RVec3Arg,const JPH::CollideShapeResult&) override {
        return Allowed(a.GetUserData(),b.GetUserData())?JPH::ValidateResult::AcceptAllContactsForThisBodyPair:JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }
    void Mark(const JPH::Body& a,const JPH::Body& b){std::lock_guard lock(mutex);seen.insert(Pair(a.GetUserData(),b.GetUserData()));}
    void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&) override {Mark(a,b);}
    void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold&,JPH::ContactSettings&) override {Mark(a,b);}
    void Publish() {
        std::lock_guard lock(mutex);
        for(const auto& p:seen){const auto phase=contacts.contains(p)?ContactPhase::Stayed:ContactPhase::Entered;Emit(p.first,p.second,phase);}
        for(const auto& p:contacts)if(!seen.contains(p))Emit(p.first,p.second,ContactPhase::Exited);
        contacts=std::move(seen);seen.clear();
    }
    void Emit(GameObjectId a,GameObjectId b,ContactPhase phase) {
        const auto& ea=entries.at(a);const auto& eb=entries.at(b);
        if(ea.body->Mask()&eb.body->Layer())events.push_back({a,b,phase,ea.area||eb.area});
        if(eb.body->Mask()&ea.body->Layer())events.push_back({b,a,phase,ea.area||eb.area});
    }
};

World::World():impl_(std::make_unique<Impl>()){}
World::~World()=default;
void World::Build(ObjectStore& objects) {
    if(!impl_->entries.empty())throw std::logic_error("Physics world has already been built.");
    auto& bi=impl_->system.GetBodyInterface();
    for(std::size_t i=0;i<objects.Size();++i){auto& object=objects.At(i);auto* body=NativeBody(object);if(!body||!body->Enabled())continue;
        auto* collider=object.GetBehavior<Collider>();if(!collider||!collider->Enabled())throw std::runtime_error(object.Name()+" has a physics body but no enabled Collider.");
        unsigned kinds=!!object.GetBehavior<RigidBody>()+!!object.GetBehavior<KinematicBody>()+!!object.GetBehavior<StaticBody>()+!!object.GetBehavior<Area>();
        if(kinds!=1)throw std::runtime_error(object.Name()+" must have exactly one physics body type.");
        const auto pose=GlobalPose(objects,object);const auto colliderScale=Mul(pose.scale,collider->Size());const Vec3 s{std::max(.002f,std::abs(colliderScale.x)),std::max(.002f,std::abs(colliderScale.y)),std::max(.002f,std::abs(colliderScale.z))};const auto offset=Mul(collider->Offset(),pose.scale);JPH::ShapeRefC shape;
        if(collider->Shape()==ColliderShape::Box)shape=new JPH::BoxShape(JV(Vec3{s.x*.5f,s.y*.5f,s.z*.5f}));
        else if(collider->Shape()==ColliderShape::Sphere)shape=new JPH::SphereShape(std::max({s.x,s.y,s.z})*.5f);
        else {const float r=std::max(s.x,s.z)*.5f;shape=new JPH::CapsuleShape(std::max(.001f,s.y*.5f-r),r);}
        const bool area=object.GetBehavior<Area>()!=nullptr;const bool kinematic=object.GetBehavior<KinematicBody>()!=nullptr||area;const auto motion=object.GetBehavior<RigidBody>()?JPH::EMotionType::Dynamic:kinematic?JPH::EMotionType::Kinematic:JPH::EMotionType::Static;
        JPH::BodyCreationSettings settings(shape,pose.position+pose.rotation*JV(offset),pose.rotation,motion,motion==JPH::EMotionType::Static?NonMoving:Moving);settings.mUserData=object.Id();settings.mFriction=body->Friction();settings.mRestitution=body->Bounciness();settings.mIsSensor=area;settings.mCollideKinematicVsNonDynamic=kinematic;
        if(auto* rigid=object.GetBehavior<RigidBody>()){settings.mGravityFactor=rigid->GravityScale();settings.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;settings.mMassPropertiesOverride.mMass=rigid->Mass();settings.mAllowSleeping=false;}
        auto id=bi.CreateAndAddBody(settings,motion==JPH::EMotionType::Static?JPH::EActivation::DontActivate:JPH::EActivation::Activate);if(id.IsInvalid())throw std::runtime_error("Physics body capacity exceeded.");
        impl_->entries.emplace(object.Id(),Impl::Entry{id,body,offset,area});impl_->reverse.emplace(id,object.Id());
        if(auto* moving=dynamic_cast<MovingBody*>(body)){bi.SetLinearVelocity(id,JV(moving->Velocity()));bi.SetAngularVelocity(id,JV(moving->AngularVelocity()));}
    }
    impl_->system.OptimizeBroadPhase();
}
void World::Step(ObjectStore& objects,float delta) {
    if(!std::isfinite(delta)||delta<=0)throw std::invalid_argument("Physics delta must be finite and positive.");auto& bi=impl_->system.GetBodyInterface();
    for(auto& [id,e]:impl_->entries)if(auto* moving=dynamic_cast<MovingBody*>(e.body)){
        if(dynamic_cast<RigidBody*>(moving)){bi.AddForce(e.id,JV(moving->ConstantForce()));bi.AddTorque(e.id,JV(moving->ConstantTorque()));}
        else if(auto* object=objects.Find(id)){const auto pose=GlobalPose(objects,*object);bi.MoveKinematic(e.id,pose.position+pose.rotation*JV(e.offset),pose.rotation,delta);}
    }
    const int steps=std::clamp(static_cast<int>(std::ceil(delta*60)),1,4);impl_->system.Update(delta,steps,&impl_->allocator,&impl_->jobs);impl_->Publish();
    for(auto& [id,e]:impl_->entries)if(dynamic_cast<RigidBody*>(e.body)){auto* object=objects.Find(id);if(!object)continue;JPH::RVec3 p;JPH::Quat q;bi.GetPositionAndRotation(e.id,p,q);SetFromGlobal(objects,*object,p-q*JV(e.offset),q);auto* moving=dynamic_cast<MovingBody*>(e.body);moving->SetVelocity(ZV(bi.GetLinearVelocity(e.id)));moving->SetAngularVelocity(ZV(bi.GetAngularVelocity(e.id)));}
}
bool World::Contains(GameObjectId id) const{return impl_->entries.contains(id);}
void World::AddForce(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().AddForce(impl_->entries.at(id).id,JV(v));}
void World::AddImpulse(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().AddImpulse(impl_->entries.at(id).id,JV(v));}
void World::AddTorque(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().AddTorque(impl_->entries.at(id).id,JV(v));}
void World::AddAngularImpulse(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().AddAngularImpulse(impl_->entries.at(id).id,JV(v));}
void World::SetVelocity(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().SetLinearVelocity(impl_->entries.at(id).id,JV(v));}
void World::SetAngularVelocity(GameObjectId id,Vec3 v){impl_->system.GetBodyInterface().SetAngularVelocity(impl_->entries.at(id).id,JV(v));}
Vec3 World::Velocity(GameObjectId id)const{return ZV(impl_->system.GetBodyInterface().GetLinearVelocity(impl_->entries.at(id).id));}
Vec3 World::AngularVelocity(GameObjectId id)const{return ZV(impl_->system.GetBodyInterface().GetAngularVelocity(impl_->entries.at(id).id));}
std::vector<RayHit> World::Cast(Vec3 from,Vec3 to,std::uint32_t mask)const{
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;impl_->system.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(JPH::RVec3(JV(from)),JV(Vec3{to.x-from.x,to.y-from.y,to.z-from.z})),JPH::RayCastSettings{},collector);
    std::vector<RayHit> result;for(const auto& hit:collector.mHits){auto found=impl_->reverse.find(hit.mBodyID);if(found==impl_->reverse.end())continue;const auto& entry=impl_->entries.at(found->second);if(!(mask&entry.body->Layer()))continue;const auto p=from;result.push_back({found->second,{p.x+(to.x-p.x)*hit.mFraction,p.y+(to.y-p.y)*hit.mFraction,p.z+(to.z-p.z)*hit.mFraction},hit.mFraction});}
    std::ranges::sort(result,{},&RayHit::fraction);return result;
}
std::vector<ContactEvent> World::DrainEvents(){std::lock_guard lock(impl_->mutex);auto out=std::move(impl_->events);impl_->events.clear();return out;}
} // namespace zengine::physics
