#pragma once

#include "core/GameObject.h"
#include "ScriptHost.h"
#include "physics/PhysicsBehavior.h"
#include "ui/UiSerialize.h"
#include "MaterialAssets.h"
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>

// Native editor widget, independent of renderer/importer and reusable for other inspector hosts.
class InspectorPanel final
{
public:
    static constexpr int NameField = 1100;
    static constexpr int TagsField = 1101;
    static constexpr int FirstTransformField = 1102; // Position XYZ, Rotation XYZ, Scale XYZ.
    static constexpr int AddScriptButton = 1120;
    static constexpr int FirstBehaviorField = 1200;
    static constexpr int FirstBehaviorToggle = 5000;
    static constexpr int FirstBehaviorBit = 6000; // Collision layer/mask toggle buttons.
    static constexpr int CollisionBits = 16;      // Bits exposed as buttons; higher bits are preserved untouched.
    static constexpr int AddScriptSubFirst = 7000; // "Add Behavior > Add Script >" submenu items.
    static constexpr int AddBehaviorButton = 1121, MeshEnabled = 1122, ChooseMeshButton = 1123,
        CubeMeshButton = 1124, ClearMeshButton = 1125, AddMeshCommand = 1126, AddScriptCommand = 1127,
        AddColliderCommand=1128,AddRigidBodyCommand=1129,AddKinematicBodyCommand=1130,AddStaticBodyCommand=1131,AddAreaCommand=1132,AddCameraCommand=1134,
        AddAudioSourceCommand=1135,AddAudioEffectCommand=1136,AddLightCommand=1137,AddEnvironmentCommand=1138,AddDecalCommand=1139;
    static constexpr int RemoveBehaviorCommand = 1133;
    enum class MeshAction { Add, Choose, Cube, Clear };
    ~InspectorPanel();
    void Create(HWND parent, HINSTANCE instance, HFONT font, std::function<void()> changed);
    void Bind(zengine::ObjectCore* object,bool editData=true,bool editTransform=true);
    void RefreshBehaviors();
    void SetScriptHost(zengine::ScriptHost* host) { scriptHost_ = host; }
    void RefreshLiveValues();
    void SetAddScriptHandler(std::function<void()> handler) { addScript_ = std::move(handler); }
    // Populates the "Add Behavior > Add Script" submenu and attaches the chosen script by project-relative path.
    void SetScriptMenu(std::function<std::vector<std::wstring>()> list, std::function<void(const std::wstring&)> attach)
    { scriptList_ = std::move(list); attachScript_ = std::move(attach); }
    void SetMeshHandler(std::function<void(MeshAction)> handler) { meshAction_ = std::move(handler); }
    void SetPrefabHandler(std::function<std::optional<std::string>(const std::string&)> handler) { choosePrefab_ = std::move(handler); }
    // Click on a script object-reference field -> show the scene-object picker for that
    // reference type. Returns the picked GameObjectId (0 clears, nullopt cancels).
    void SetObjectPicker(std::function<std::optional<zengine::GameObjectId>(const std::string& referenceType, zengine::GameObjectId current, RECT anchorScreen)> handler)
    { pickObject_ = std::move(handler); }
    // ZE-65: resolve a MeshRenderer's ".material" to its effective parameters, and
    // write one edited parameter value back to the .material asset.
    void SetMaterialHandlers(std::function<zengine::materials::Effective(const std::string&)> resolve,
                             std::function<void(const std::string&, zengine::materials::Value)> setValue)
    { resolveMaterial_ = std::move(resolve); setMaterialValue_ = std::move(setValue); }
    bool AssignPrefabAt(POINT screenPoint,const std::string& asset);
    // Drop a project-relative image / video asset path onto a UI control's
    // texture field (K::Texture). Returns true if a field accepted it.
    bool AssignAssetPathAt(POINT screenPoint,const std::string& asset);
    bool AssignObjectReferenceAt(POINT screenPoint, zengine::GameObjectId target);
    HWND Window() const noexcept { return window_; }
    // Test seam: the edit/combo control that edits property `key` (component `axis`
    // for Vec2 / Color rows) of `behavior`, or nullptr. Used by the UI-inspector tests.
    HWND FieldWindowForProperty(const zengine::Behavior* behavior, const std::string& key, int axis = 0) const;
private:
    bool editData_=true,editTransform_=true;
    struct Field { HWND window = nullptr; bool valid = true; std::wstring focusText; };
    static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleEdit(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void Paint(HDC into = nullptr);
    void ChangeField(int index);
    void FinishField(int index, bool cancel);
    void RefreshFields();
    void EndScrub(bool cancel);
    float Value(int index) const;
    void SetValue(int index, float value);
    std::wstring FieldValue(int index) const;
    void SetText(int index, const std::wstring& value);
    struct BehaviorField
    {
        enum class Style { Normal, BehaviorHeader, ScriptLabel };
        zengine::Behavior* behavior = nullptr;
        std::string name;
        std::string type;   // script field type, for reference fields ("RigidBody", "gameObject", ...)
        std::wstring label;
        Field field;
        bool priority = false;
        bool multiline = false;
        bool combo = false;
        bool prefab = false;
        bool objectReference = false;
        bool bitmask = false; // Collision layer/mask row: a grid of toggle buttons instead of an edit field.
        std::vector<HWND> bits; // Bitmask toggle buttons (low bit first), or array aux buttons ([0] = add/remove).
        int axis = -1; // Consecutive fields share one vector row (axis 0..axisCount-1).
        int axisCount = 3; // 3 for Vector3/Vec3 rows, 2 for a script Vector2 field.
        bool arrayHeader = false; // "+ Add element" row for an exported script array field.
        int arrayIndex = -1;      // >=0: one element row of the exported array named `name`.
        Style style = Style::Normal;
        // ZE-81: rows that edit a ui::UiControl property. `name` holds the property
        // key; the value is exchanged as the string SaveUiControl / LoadUiProperty use.
        bool uiControl = false;
        zengine::ui::UiPropertyKind uiKind = zengine::ui::UiPropertyKind::Line;
        std::vector<std::wstring> comboItems; // generic combo choices (bool / anchor)
        // ZE-65: a MeshRenderer material row. `materialPath` = the ".material" path
        // field itself; `materialParam` = one parameter of the resolved material.
        bool materialPath = false;
        bool materialParam = false;
        zengine::shaders::ParamType materialType = zengine::shaders::ParamType::Float4;
    };
    std::wstring BehaviorValue(std::size_t index);
    void ChangeBehaviorField(std::size_t index);
    void ShowAddArrayElementMenu(std::size_t fieldIndex);
    void RemoveArrayElementAt(std::size_t fieldIndex);
    void ToggleCollisionBit(std::size_t fieldIndex, int bit);
    void RefreshCollisionBits(const BehaviorField& entry) const;
    int bitButtonCount_ = 0; // Running WM_COMMAND id offset for collision-bit buttons.
    void FinishBehaviorField(std::size_t index, bool cancel);
    bool IsBehaviorCollapsed(const BehaviorField& entry) const;
    void ShowBehaviorMenu(POINT screenPoint);
    std::vector<BehaviorField> behaviorFields_;
    struct BehaviorToggle { zengine::Behavior* behavior = nullptr; HWND window = nullptr; };
    std::vector<BehaviorToggle> behaviorToggles_;
    std::set<zengine::Behavior*> collapsedBehaviors_;
    zengine::Behavior* contextBehavior_ = nullptr;
    zengine::ScriptHost* scriptHost_ = nullptr;
    int BehaviorHeight() const;
    int RowHeight(const BehaviorField& entry) const;

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HFONT behaviorFont_ = nullptr, labelFont_ = nullptr;
    int behaviorHeaderHeight_ = 32;
    HBRUSH background_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH invalidBrush_ = nullptr;
    std::array<Field, 11> fields_{};
    zengine::ObjectCore* object_ = nullptr;
    bool TransformFieldUsed(int index) const; // 2D objects use only pos.xy / rot / scale.xy
    std::function<void()> changed_;
    std::function<void()> addScript_;
    std::function<std::vector<std::wstring>()> scriptList_;
    std::function<void(const std::wstring&)> attachScript_;
    HWND addBehaviorButton_ = nullptr, meshEnabled_ = nullptr, chooseMesh_ = nullptr, cubeMesh_ = nullptr, clearMesh_ = nullptr;
    std::function<void(MeshAction)> meshAction_;
    std::function<std::optional<std::string>(const std::string&)> choosePrefab_;
    std::function<std::optional<zengine::GameObjectId>(const std::string&, zengine::GameObjectId, RECT)> pickObject_;
    std::function<zengine::materials::Effective(const std::string&)> resolveMaterial_;
    std::function<void(const std::string&, zengine::materials::Value)> setMaterialValue_;
    bool updating_ = false;
    int scroll_ = 0;
    int wheelRemainder_ = 0;
    int pressed_ = -1;
    bool scrubbing_ = false;
    POINT startPoint_{};
    float startValue_ = 0;
    float scrubStep_ = 0.01f;
};
