#include "GameSession.h"
#include "PrefabAssets.h"
#include "SceneAssets.h"
#include "ScriptAssets.h"
#include "AssetLibrary.h"
#include "input/InputAssets.h"
#include "core/MeshRenderer.h"

namespace zengine::game {
Session::Session(const projects::Project& project,const std::string& scene):reference_(scene.empty()?project.config.lastScene:scene) {
    if(reference_.empty() || std::find(project.config.scenes.begin(),project.config.scenes.end(),reference_)==project.config.scenes.end())throw std::runtime_error("Startup scene must be in the project scene list.");
    const auto assets=projects::Assets(project);
    input_.Configure(input::Decode(input::Load(assets)));
    const auto document=scenes::Decode(scenes::Load(projects::ScenePath(project,reference_)));
    scene_=scenes::Instantiate(prefabs::ResolveScene(assets,document).scene);
    script::InputFrame initial;for(const auto& [name,state]:input_.Current())initial.emplace(name,script::InputState{});scene_.scripts.SetInput(std::move(initial));
    for(std::size_t i=0;i<scene_.objects.Size();++i) {
        auto& object=scene_.objects.At(i);
        if(const auto mesh=object.GetBehavior<MeshRenderer>();mesh && !mesh->Asset().empty() && mesh->Asset()!=MeshRenderer::CubeAsset) {
            const auto file=assetLibrary::Resolve(assets,std::filesystem::u8path(mesh->Asset()));
            if(file.filename()!=L"model.fbx" || !assetLibrary::Package(file.parent_path()))throw std::runtime_error("Missing imported model package: "+mesh->Asset());
            models_[object.Id()]=file;
        }
        for(std::size_t j=0;j<object.BehaviorCount();++j)if(auto behavior=dynamic_cast<ScriptBehavior*>(&object.BehaviorAt(j))) {
            const auto file=scripts::Resolve(assets,std::filesystem::u8path(behavior->Asset()));const auto name=file.stem().u8string();
            if(!scene_.scripts.Prepare(*behavior,scripts::Load(file),std::string(name.begin(),name.end())))throw std::runtime_error(scene_.scripts.Error(*behavior));
        }
    }
}
Session::~Session(){if(scene_.scripts.Playing())scene_.scripts.Stop(scene_.objects);physics_.reset();}
void Session::CheckErrors() const {
    for(std::size_t i=0;i<scene_.objects.Size();++i)for(std::size_t j=0;j<scene_.objects.At(i).BehaviorCount();++j) {
        const auto& behavior=scene_.objects.At(i).BehaviorAt(j);
        if(behavior.Faulted())throw std::runtime_error(scene_.objects.At(i).Name()+": "+behavior.Error());
    }
}
void Session::Start(){physics_=std::make_unique<physics::World>();physics_->Build(scene_.objects);if(!scene_.scripts.Play(scene_.objects,physics_.get()))throw std::runtime_error("Could not start scene scripts.");CheckErrors();}
void Session::Tick(float delta,const input::Hardware& hardware) {
    script::InputFrame frame;for(const auto& [name,s]:input_.Tick(hardware))frame.emplace(name,script::InputState{s.x,s.y,s.pressed,s.justPressed,s.justReleased});
    scene_.scripts.SetInput(std::move(frame));scene_.scripts.Tick(scene_.objects,delta);physics_->Step(scene_.objects,delta);scene_.scripts.DispatchPhysicsEvents(physics_->DrainEvents());CheckErrors();
}
void Session::Draw(const std::function<bool(GameObjectId)>& visible){scene_.scripts.Draw(scene_.objects,visible);CheckErrors();}
}
