#pragma once
#include "Scene.h"
#include "Project.h"
#include "input/InputMap.h"
#include <filesystem>

namespace zengine::game {
// Reusable simulation host. No editor, window, renderer, or UI ownership.
class Session final {
public:
    Session(const projects::Project&,const std::string& scene={});
    ~Session();
    void Start();
    void Tick(float delta,const input::Hardware&,const script::MouseFrame& mouse={});
    void Draw(const std::function<bool(GameObjectId)>& visible);
    // Delivers a UI "clicked" signal to the running script that owns `id`, if any.
    void UiClicked(GameObjectId id) { scene_.scripts.EmitSignal(id, "clicked"); }
    const ObjectStore& Objects() const {return scene_.objects;}
    ObjectStore& Objects() {return scene_.objects;}
    const std::map<GameObjectId,std::filesystem::path>& Models() const {return models_;}
    const std::string& Scene() const {return reference_;}
private:
    void CheckErrors() const;
    GameObjectId SpawnPrefab(std::string_view asset);
    void PrepareObject(ObjectCore& object);
    scenes::Instance scene_;
    input::System input_;
    std::map<GameObjectId,std::filesystem::path> models_;
    std::filesystem::path assets_;
    std::string reference_;
    std::unique_ptr<physics::World> physics_;
};
}
