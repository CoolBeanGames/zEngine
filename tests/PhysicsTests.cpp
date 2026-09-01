#include "physics/PhysicsWorld.h"
#include "Scene.h"
#include <cassert>
#include <iostream>

using namespace zengine;
int main(){
    ObjectStore objects;
    auto& floor=objects.Create("Floor");floor.GetTransform().SetPosition({0,-1,0});floor.GetTransform().SetScale({10,1,10});floor.AddBehavior<physics::Collider>();floor.AddBehavior<physics::StaticBody>();
    auto& ball=objects.Create("Ball");ball.GetTransform().SetPosition({0,3,0});auto& c=ball.AddBehavior<physics::Collider>();c.SetShape(physics::ColliderShape::Sphere);auto& rigid=ball.AddBehavior<physics::RigidBody>();rigid.SetMass(2);rigid.SetBounciness(.25f);auto& script=ball.AddBehavior<ScriptBehavior>("PhysicsMover.zsh");
    ScriptHost host;const auto source=R"(class PhysicsMover : gameObject { export int contacts=0; func start() { physics.collision_entered.connect(on_collision); physics.add_impulse(transform.right*2); } func on_collision(gameObject other) { contacts+=1; } })";assert(host.Prepare(script,source,"PhysicsMover"));
    physics::World world;world.Build(objects);assert(world.Contains(ball.Id()));assert(host.Play(objects,&world));bool entered=false,stayed=false;
    for(int i=0;i<240;++i){host.Tick(objects,1.0f/60);world.Step(objects,1.0f/60);const auto events=world.DrainEvents();host.DispatchPhysicsEvents(events);for(const auto& e:events)if(e.receiver==ball.Id()&&e.other==floor.Id()){entered|=e.phase==physics::ContactPhase::Entered;stayed|=e.phase==physics::ContactPhase::Stayed;}}
    assert(entered&&stayed);assert(ball.GetTransform().Position().x>0);assert(ball.GetTransform().Position().y<1);
    const auto x=ball.GetTransform().Position().x;const auto hits=world.Cast({x,5,0},{x,-5,0});assert(!hits.empty());assert(hits.front().object==ball.Id());bool received=false;for(const auto& field:host.Fields(script))if(field.name=="contacts")received=std::stoll(field.value)>0;assert(received);host.Stop(objects);

    const auto scene=scenes::Capture(objects,host);const auto restored=scenes::Instantiate(scenes::Decode(scenes::Encode(scene)));
    assert(restored.objects.At(0).GetBehavior<physics::StaticBody>());assert(restored.objects.At(1).GetBehavior<physics::RigidBody>());assert(restored.objects.At(1).GetBehavior<physics::Collider>()->Shape()==physics::ColliderShape::Sphere);
    std::cout<<"Physics wrapper, contacts, rays, scripting, and persistence passed.\n";
}
