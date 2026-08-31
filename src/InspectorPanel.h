#pragma once

#include "core/GameObject.h"
#include "ScriptHost.h"
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <functional>
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
    static constexpr int AddBehaviorButton = 1121, MeshEnabled = 1122, ChooseMeshButton = 1123,
        CubeMeshButton = 1124, ClearMeshButton = 1125, AddMeshCommand = 1126, AddScriptCommand = 1127;
    enum class MeshAction { Add, Choose, Cube, Clear };
    ~InspectorPanel();
    void Create(HWND parent, HINSTANCE instance, HFONT font, std::function<void()> changed);
    void Bind(zengine::GameObject* object,bool editData=true,bool editTransform=true);
    void RefreshBehaviors();
    void SetScriptHost(zengine::ScriptHost* host) { scriptHost_ = host; }
    void RefreshLiveValues();
    void SetAddScriptHandler(std::function<void()> handler) { addScript_ = std::move(handler); }
    void SetMeshHandler(std::function<void(MeshAction)> handler) { meshAction_ = std::move(handler); }
    HWND Window() const noexcept { return window_; }
private:
    bool editData_=true,editTransform_=true;
    struct Field { HWND window = nullptr; bool valid = true; std::wstring focusText; };
    static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleEdit(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void Paint();
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
        std::wstring label;
        Field field;
        bool priority = false;
        int axis = -1; // Three consecutive fields share one Vector3 row.
        Style style = Style::Normal;
    };
    std::wstring BehaviorValue(std::size_t index);
    void ChangeBehaviorField(std::size_t index);
    void FinishBehaviorField(std::size_t index, bool cancel);
    std::vector<BehaviorField> behaviorFields_;
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
    zengine::GameObject* object_ = nullptr;
    std::function<void()> changed_;
    std::function<void()> addScript_;
    HWND addScriptButton_ = nullptr;
    HWND addBehaviorButton_ = nullptr, meshEnabled_ = nullptr, chooseMesh_ = nullptr, cubeMesh_ = nullptr, clearMesh_ = nullptr;
    std::function<void(MeshAction)> meshAction_;
    bool updating_ = false;
    int scroll_ = 0;
    int pressed_ = -1;
    bool scrubbing_ = false;
    POINT startPoint_{};
    float startValue_ = 0;
    float scrubStep_ = 0.01f;
};
