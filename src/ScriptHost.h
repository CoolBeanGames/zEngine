#pragma once
#include "core/ScriptBehavior.h"
#include "core/BehaviorLifecycle.h"
#include "zscript/Script.h"
#include "physics/PhysicsWorld.h"
#include <map>

namespace zengine
{
    // Engine adapter: no window, renderer, asset I/O or editor dependencies.
    // The ObjectStore and its behaviors must outlive this host. Stop before destroying them.
    // One element of an exported script `array` field. A reference element carries a
    // scene GameObjectId plus the reference type auto-picked from the assigned object
    // ("gameObject", "RigidBody", ...); a value element carries a plain script::Value.
    struct ScriptArrayElement
    {
        script::Value value;              // scalar payload for a value element
        GameObjectId reference = 0;       // non-zero => this element is an object reference
        std::string referenceType;        // reference element type name
        bool operator==(const ScriptArrayElement&) const = default;
    };

    class ScriptHost final
    {
    public:
        // array: header row for an exported `array` field (arrayCount elements follow).
        // arrayIndex>=0: one element row of the array named `name`.
        struct Field { std::string name, type, label, value; bool editable = false; bool multiline=false; bool reference=false;
            bool array=false; int arrayIndex=-1; std::size_t arrayCount=0;
            bool dataAsset=false; }; // ZE-129: value is a project-relative ".zdata" path bound to a data-object field
        // ZE-129: how the host reads/writes a ".zdata" file's text (project-relative path).
        // The reader is used to instantiate a bound data object at Play; the writer backs
        // data_object.save(). Both may be null (data-object fields then stay unbound).
        using DataAssetReader = std::function<std::string(std::string_view)>;
        using DataAssetWriter = std::function<void(std::string_view, std::string_view)>;
        void SetDataAssetIO(DataAssetReader reader, DataAssetWriter writer)
        { if(playing_)throw std::logic_error("Stop before changing data asset I/O."); dataReader_=std::move(reader); dataWriter_=std::move(writer); }
        // Element types the inspector's "add element" picker offers, value types first.
        static const std::vector<std::string>& ArrayElementTypes();
        void SetObjectStore(ObjectStore* objects) noexcept { objectStore_ = objects; }
        bool Prepare(ScriptBehavior&, std::string source, std::string className);
        std::vector<Field> Fields(ScriptBehavior&);
        void SetField(ScriptBehavior&, const std::string& name, const std::string& text);
        void SetObjectReference(ScriptBehavior&, const std::string& name, GameObjectId target);
        // Exported `array` field editing (edit mode only; call Stop before touching these).
        void AddArrayElement(ScriptBehavior&, const std::string& field, const std::string& elementType);
        void RemoveArrayElement(ScriptBehavior&, const std::string& field, std::size_t index);
        void SetArrayElement(ScriptBehavior&, const std::string& field, std::size_t index, const std::string& text);
        // index == current element count appends a new slot. The reference type is
        // auto-selected from the target's behaviors.
        void SetArrayElementReference(ScriptBehavior&, const std::string& field, std::size_t index, GameObjectId target);
        // True when the object could be assigned to a script reference field of this type
        // (e.g. type "RigidBody" needs a RigidBody behavior; "gameObject"/"Transform" match any).
        static bool ObjectMatchesReferenceType(const ObjectCore&, std::string_view type);
        std::string Error(const ScriptBehavior&) const;
        std::map<std::string,script::Value> AuthoredValues(const ScriptBehavior&) const;
        std::map<std::string,GameObjectId> AuthoredReferences(const ScriptBehavior&) const;
        std::map<std::string,std::vector<ScriptArrayElement>> AuthoredArrays(const ScriptBehavior&) const;
        void RestoreValues(ScriptBehavior&, std::map<std::string,script::Value> values);
        void RestoreReferences(ScriptBehavior&, std::map<std::string,GameObjectId> references);
        std::map<std::string,std::string> AuthoredDataAssets(const ScriptBehavior&) const; // ZE-129
        void RestoreDataAssets(ScriptBehavior&, std::map<std::string,std::string> dataAssets);
        void RestoreArrays(ScriptBehavior&, std::map<std::string,std::vector<ScriptArrayElement>> arrays);
        void Forget(ScriptBehavior& behavior) { if(playing_)throw std::logic_error("Stop before forgetting a script.");records_.erase(&behavior); }
        bool Play(ObjectStore&, physics::World* physicsWorld=nullptr);
        using PrefabSpawner=std::function<GameObjectId(std::string_view)>;
        void SetPrefabSpawner(PrefabSpawner spawner) { if(playing_)throw std::logic_error("Stop before changing the prefab spawner.");prefabSpawner_=std::move(spawner); }
        void SetPrintHandler(std::function<void(std::string_view)> handler) { if(playing_)throw std::logic_error("Stop before changing the print handler.");printHandler_=std::move(handler); }
        // Backs the script `Scene` service. `loader` switches the running scene by
        // name; `name` is what Scene.current() returns.
        using SceneLoader=std::function<void(std::string_view)>;
        void SetSceneLoader(SceneLoader loader) { if(playing_)throw std::logic_error("Stop before changing the scene loader.");sceneLoader_=std::move(loader); }
        void SetSceneName(std::string name) { sceneName_=std::move(name); }
        void DispatchPhysicsEvents(const std::vector<physics::ContactEvent>&);
        // Emits a bare signal (e.g. "clicked" from the UI system) on the running
        // script that owns `owner`, if any. No-op outside Play.
        void EmitSignal(GameObjectId owner, std::string_view signal);
        void Stop(ObjectStore&);
        void Tick(ObjectStore& objects, float delta) { if (playing_) lifecycle_.Tick(objects, delta); }
        void PhysicsTick(ObjectStore& objects, float delta) { if (playing_) lifecycle_.PhysicsTick(objects, delta); }
        void Draw(ObjectStore& objects, const std::function<bool(GameObjectId)>& visible) { if (playing_) lifecycle_.Draw(objects, visible); }
        bool Playing() const noexcept { return playing_; }
        void SetInput(script::InputFrame frame) { input_=std::move(frame); }
        void SetMouse(script::MouseFrame frame) { mouse_=frame; }
        // ZE-84: the mouse capture mode a script last requested via Input.set_mouse_mode
        // (0 visible / 1 hidden / 2 confined / 3 confined+hidden / 4 captured). The host
        // applies it. Reset to 0 on Stop.
        int MouseMode() const noexcept { return mouseMode_; }
    private:
        struct Record
        {
            std::string source, className, error;
            std::shared_ptr<const script::Program> program;
            std::unique_ptr<script::Runtime> preview;
            script::ObjectRef object;
            std::map<std::string, script::Value> overrides;
            std::map<std::string, GameObjectId> references;
            std::map<std::string, std::vector<ScriptArrayElement>> arrays;
            std::map<std::string, std::string> dataAssets; // ZE-129: data-object field name -> bound ".zdata" path
            std::map<GameObjectId, script::ObjectRef> previewProxies;
        };
        static bool IsDataField(const script::Program&, std::string_view type);
        void ApplyRecordDataAssets(Record&);
        static bool IsReferenceType(std::string_view type);
        script::ObjectRef PreviewReference(Record&, GameObjectId, std::string_view type);
        void ApplyPreviewReferences(Record&);
        void SyncPreviewArray(Record&, const std::string& field);
        Record& ArrayRecord(ScriptBehavior&, const std::string& field); // validates edit-mode + exported array field
        std::map<const ScriptBehavior*, Record> records_;
        ObjectStore* objectStore_ = nullptr;
        std::map<GameObjectId, Transform> transforms_;
        std::map<GameObjectId,GameObjectId> parents_;
        BehaviorLifecycle lifecycle_;
        bool playing_ = false;
        int mouseMode_ = 0; // ZE-84
        script::InputFrame input_;
        script::MouseFrame mouse_;
        PrefabSpawner prefabSpawner_;
        DataAssetReader dataReader_;
        DataAssetWriter dataWriter_;
        std::function<void(std::string_view)> printHandler_;
        SceneLoader sceneLoader_;
        std::string sceneName_;
        ObjectStore* playingObjects_=nullptr;
        physics::World* playingPhysics_=nullptr;
    };
}
