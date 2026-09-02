#include "physics/PhysicsWorld.h"
#include "Scene.h"
#include "core/MeshRenderer.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace zengine;
int main(){
    ObjectStore objects;
    auto& floor=objects.Create("Floor");floor.GetTransform().SetPosition({0,-1,0});floor.GetTransform().SetScale({10,1,10});floor.AddBehavior<physics::Collider>();floor.AddBehavior<physics::StaticBody>();
    auto& ball=objects.Create("Ball");ball.GetTransform().SetPosition({0,3,0});ball.AddBehavior<MeshRenderer>(MeshRenderer::CubeAsset);auto& c=ball.AddBehavior<physics::Collider>();c.SetShape(physics::ColliderShape::Sphere);c.SetOffset({0,.25f,0});c.SetSize({1,1.5f,1});auto& rigid=ball.AddBehavior<physics::RigidBody>();rigid.SetMass(2);rigid.SetBounciness(.25f);auto& script=ball.AddBehavior<ScriptBehavior>("PhysicsMover.zsh");
    auto& floating=objects.Create("Floating");floating.GetTransform().SetPosition({100,3,0});auto& floatingBody=floating.AddBehavior<physics::RigidBody>();floatingBody.SetGravityScale(0);
    ScriptHost host;const auto source=R"(class PhysicsMover : RigidBody { export int contacts=0; export int physics_ticks=0; func start() { rigidbody.collision_entered.connect(on_collision); launch(rigidbody); } func launch(RigidBody body){body.add_impulse(transform.right*2);} func physicsUpdate(float delta) { physics_ticks+=1; } func on_collision(gameObject other) { contacts+=1; } })";assert(host.Prepare(script,source,"PhysicsMover"));
    physics::World world;world.Build(objects);assert(world.Contains(ball.Id()));assert(host.Play(objects,&world));bool entered=false,stayed=false;
    for(int i=0;i<240;++i){host.Tick(objects,1.0f/60);host.PhysicsTick(objects,1.0f/60);world.Step(objects,1.0f/60);const auto events=world.DrainEvents();host.DispatchPhysicsEvents(events);for(const auto& e:events)if(e.receiver==ball.Id()&&e.other==floor.Id()){entered|=e.phase==physics::ContactPhase::Entered;stayed|=e.phase==physics::ContactPhase::Stayed;}}
    assert(entered&&stayed);assert(ball.GetTransform().Position().x>0);assert(ball.GetTransform().Position().y<1);assert(std::abs(floating.GetTransform().Position().y-3)<.001f);
    const auto x=ball.GetTransform().Position().x;const auto hits=world.Cast({x,5,0},{x,-5,0});assert(!hits.empty());assert(hits.front().object==ball.Id());bool received=false,fixed=false;for(const auto& field:host.Fields(script)){if(field.name=="contacts")received=std::stoll(field.value)>0;if(field.name=="physics_ticks")fixed=std::stoll(field.value)==240;}assert(received&&fixed);assert(ball.GetBehavior<MeshRenderer>() && ball.GetBehavior<MeshRenderer>()->Owner().GetTransform().Position()==ball.GetTransform().Position());host.Stop(objects);

    const auto scene=scenes::Capture(objects,host);const auto restored=scenes::Instantiate(scenes::Decode(scenes::Encode(scene)));
    assert(restored.objects.At(0).GetBehavior<physics::StaticBody>());assert(restored.objects.At(1).GetBehavior<physics::RigidBody>());const auto* restoredCollider=restored.objects.At(1).GetBehavior<physics::Collider>();const Vec3 expectedOffset{0,.25f,0},expectedSize{1,1.5f,1};assert(restoredCollider->Shape()==physics::ColliderShape::Sphere&&restoredCollider->Offset()==expectedOffset&&restoredCollider->Size()==expectedSize);
    std::cout<<"Physics wrapper, contacts, rays, scripting, and persistence passed.\n";
}
