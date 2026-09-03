#include "GameSession.h"
#include "PrefabAssets.h"
#include "SceneAssets.h"
#include "ScriptAssets.h"
#include "AssetLibrary.h"
#include "input/InputAssets.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "audio/AudioClip.h"
#include "audio/AudioSource.h"

#include <iostream>

namespace zengine::game {
Session::Session(const projects::Project& project,const std::string& scene):project_(&project) {
    assets_=projects::Assets(project);
    input_.Configure(input::Decode(input::Load(assets_)));
    LoadScene(scene.empty()?project.config.lastScene:scene);
}
void Session::LoadScene(const std::string& reference) {
    const auto& list=project_->config.scenes;
    if(reference.empty() || std::find(list.begin(),list.end(),reference)==list.end())
        throw std::runtime_error("Startup scene must be in the project scene list.");
    reference_=reference;
    models_.clear();
    const auto document=scenes::Decode(scenes::Load(projects::ScenePath(*project_,reference_)));
    scene_=scenes::Instantiate(prefabs::ResolveScene(assets_,document).scene);
    // Scripts resolve their exported scene-tree references (e.g. an assigned rigidbody)
    // against this store; without it Prepare aborts the standalone build.
    scene_.scripts.SetObjectStore(&scene_.objects);
    scene_.scripts.SetSceneName(reference_);
    scene_.scripts.SetSceneLoader([this](std::string_view name){ ChangeScene(std::string(name)); });
    script::InputFrame initial;for(const auto& [name,state]:input_.Current())initial.emplace(name,script::InputState{});scene_.scripts.SetInput(std::move(initial));
    for(std::size_t i=0;i<scene_.objects.Size();++i) {
        PrepareObject(scene_.objects.At(i));
    }
    scene_.scripts.SetPrefabSpawner([this](std::string_view asset){return SpawnPrefab(asset);});
}
std::string Session::ResolveSceneReference(const std::string& name) const {
    const auto& list=project_->config.scenes;
    if(std::find(list.begin(),list.end(),name)!=list.end())return name; // exact project reference
    const auto wanted=std::filesystem::path(name).stem();
    std::vector<std::string> matches;
    for(const auto& ref:list)if(std::filesystem::path(ref).stem()==wanted)matches.push_back(ref);
    if(matches.empty())throw std::runtime_error("Scene.load: no scene named '"+name+"' in the project.");
    if(matches.size()>1)
        std::cerr<<"Scene.load(\""<<name<<"\"): "<<matches.size()
                 <<" scenes share that name; loading the first ("<<matches.front()<<").\n";
    return matches.front();
}
void Session::ChangeScene(const std::string& scene) {
    try { pendingScene_=ResolveSceneReference(scene); }
    catch(const std::exception& error) { std::cerr<<error.what()<<"\nScene switch ignored.\n"; }
}
void Session::ApplyPendingSceneChange() {
    if(pendingScene_.empty())return;
    const auto target=pendingScene_;
    pendingScene_.clear();
    if(scene_.scripts.Playing())scene_.scripts.Stop(scene_.objects);
    physics_.reset();
    if(audio_)audio_->StopAll();
    LoadScene(target);
    ++generation_;
    Start();
}
Session::~Session(){if(scene_.scripts.Playing())scene_.scripts.Stop(scene_.objects);physics_.reset();if(audio_)audio_->StopAll();}
void Session::CheckErrors() const {
    for(std::size_t i=0;i<scene_.objects.Size();++i)for(std::size_t j=0;j<scene_.objects.At(i).BehaviorCount();++j) {
        const auto& behavior=scene_.objects.At(i).BehaviorAt(j);
        if(behavior.Faulted())throw std::runtime_error(scene_.objects.At(i).Name()+": "+behavior.Error());
    }
}
void Session::Start(){
    physics_=std::make_unique<physics::World>();physics_->Build(scene_.objects);
    if(!audio_){audio_=std::make_unique<audio::AudioSystem>();audio_->SetClipLoader([this](const std::string& path){return audio::LoadFile(assetLibrary::Resolve(assets_,std::filesystem::u8path(path)));});}
    else audio_->StopAll();
    if(!scene_.scripts.Play(scene_.objects,physics_.get()))throw std::runtime_error("Could not start scene scripts.");CheckErrors();
}
void Session::TickAudio(float delta){
    if(!audio_)return;
    Vec3 listener{};
    for(std::size_t i=0;i<scene_.objects.Size();++i){
        auto& object=scene_.objects.At(i);
        if(object.GetBehavior<Camera>()){ if(auto* g=As3D(&object)) listener=g->GetTransform().Position(); break; }
    }
    audio_->SetListener(listener);
    audio_->Update(scene_.objects,delta);
    for(const auto id:audio_->Started())scene_.scripts.EmitSignal(id,"started");
    for(const auto id:audio_->Looped())scene_.scripts.EmitSignal(id,"looped");
    for(const auto id:audio_->Finished())scene_.scripts.EmitSignal(id,"finished");
}
void Session::Tick(float delta,const input::Hardware& hardware,const script::MouseFrame& mouse) {
    script::InputFrame frame;for(const auto& [name,s]:input_.Tick(hardware))frame.emplace(name,script::InputState{s.x,s.y,s.pressed,s.justPressed,s.justReleased});
    scene_.scripts.SetInput(std::move(frame));scene_.scripts.SetMouse(mouse);scene_.scripts.Tick(scene_.objects,delta);scene_.scripts.PhysicsTick(scene_.objects,delta);physics_->Step(scene_.objects,delta);scene_.scripts.DispatchPhysicsEvents(physics_->DrainEvents());TickAudio(delta);CheckErrors();
    ApplyPendingSceneChange(); // a Scene.load() during this tick swaps scenes now, before the next tick
}
void Session::Draw(const std::function<bool(GameObjectId)>& visible){scene_.scripts.Draw(scene_.objects,visible);CheckErrors();}
void Session::PrepareObject(ObjectCore& object) {
    if(const auto mesh=object.GetBehavior<MeshRenderer>();mesh && !mesh->Asset().empty() && mesh->Asset()!=MeshRenderer::CubeAsset) {
        const auto& value=mesh->Asset();const auto file=assetLibrary::Resolve(assets_,std::filesystem::path(std::u8string(value.begin(),value.end())));
        if(file.filename()!=L"model.fbx" || !assetLibrary::Package(file.parent_path()))throw std::runtime_error("Missing imported model package: "+mesh->Asset());
        models_[object.Id()]=file;
    }
    for(std::size_t j=0;j<object.BehaviorCount();++j)if(auto behavior=dynamic_cast<ScriptBehavior*>(&object.BehaviorAt(j))) {
        const auto& value=behavior->Asset();const auto file=scripts::Resolve(assets_,std::filesystem::path(std::u8string(value.begin(),value.end())));const auto name=file.stem().u8string();
        if(!scene_.scripts.Prepare(*behavior,scripts::Load(file),std::string(name.begin(),name.end())))throw std::runtime_error(scene_.scripts.Error(*behavior));
    }
}
GameObjectId Session::SpawnPrefab(std::string_view asset) {
    const auto document=prefabs::Load(assets_,std::filesystem::path(std::u8string(asset.begin(),asset.end())));const auto expanded=prefabs::ResolveScene(assets_,document);
    const auto first=scene_.objects.Size();const auto root=scenes::Append(expanded.scene,scene_.objects,scene_.scripts);
    for(std::size_t i=first;i<scene_.objects.Size();++i)PrepareObject(scene_.objects.At(i));
    return root;
}
}
