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
    void Tick(float delta,const input::Hardware&);
    void Draw(const std::function<bool(GameObjectId)>& visible);
    const ObjectStore& Objects() const {return scene_.objects;}
    const std::map<GameObjectId,std::filesystem::path>& Models() const {return models_;}
    const std::string& Scene() const {return reference_;}
private:
    void CheckErrors() const;
    scenes::Instance scene_;
    input::System input_;
    std::map<GameObjectId,std::filesystem::path> models_;
    std::string reference_;
};
}
