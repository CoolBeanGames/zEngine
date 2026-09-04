#pragma once
#include "Scene.h"
#include "Project.h"
#include "input/InputMap.h"
#include "audio/AudioSystem.h"
#include <filesystem>
#include <memory>

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
    // ZE-66: button down / up signals (also used by other controls that opt in).
    void UiPressed(GameObjectId id) { scene_.scripts.EmitSignal(id, "pressed"); }
    void UiReleased(GameObjectId id) { scene_.scripts.EmitSignal(id, "released"); }
    // ZE-96: hover / focus / text-submit signals.
    void UiMouseEntered(GameObjectId id) { scene_.scripts.EmitSignal(id, "mouse_entered"); }
    void UiMouseExited(GameObjectId id) { scene_.scripts.EmitSignal(id, "mouse_exited"); }
    void UiFocusEntered(GameObjectId id) { scene_.scripts.EmitSignal(id, "focus_entered"); }
    void UiFocusExited(GameObjectId id) { scene_.scripts.EmitSignal(id, "focus_exited"); }
    void UiSubmitted(GameObjectId id) { scene_.scripts.EmitSignal(id, "submitted"); }
    const ObjectStore& Objects() const {return scene_.objects;}
    ObjectStore& Objects() {return scene_.objects;}
    const std::map<GameObjectId,std::filesystem::path>& Models() const {return models_;}
    const std::string& Scene() const {return reference_;}
    // ZE-63: request a switch to another scene (by project reference or by name).
    // The swap is deferred to the end of the current Tick. Duplicate names resolve
    // to the first matching scene with a warning.
    void ChangeScene(const std::string& scene);
    // Bumped every time ChangeScene actually swaps; hosts key their per-scene
    // caches (e.g. uploaded meshes) off this.
    unsigned SceneGeneration() const noexcept {return generation_;}
    // ZE-84: the mouse capture mode a script requested (0 visible..4 captured). The host applies it.
    int MouseMode() const noexcept {return scene_.scripts.MouseMode();}
private:
    void CheckErrors() const;
    void LoadScene(const std::string& reference); // (re)builds scene_ from the named asset
    void ApplyPendingSceneChange();
    std::string ResolveSceneReference(const std::string& name) const;
    GameObjectId SpawnPrefab(std::string_view asset);
    void PrepareObject(ObjectCore& object);
    const projects::Project* project_ = nullptr;
    scenes::Instance scene_;
    input::System input_;
    std::map<GameObjectId,std::filesystem::path> models_;
    std::filesystem::path assets_;
    std::string reference_;
    std::string pendingScene_;
    unsigned generation_ = 0;
    std::unique_ptr<physics::World> physics_;
    std::unique_ptr<audio::AudioSystem> audio_;
    void TickAudio(float delta);
};
}
