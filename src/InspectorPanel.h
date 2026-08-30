#pragma once

#include "core/GameObject.h"
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
    ~InspectorPanel();
    void Create(HWND parent, HINSTANCE instance, HFONT font, std::function<void()> changed);
    void Bind(zengine::GameObject* object);
    HWND Window() const noexcept { return window_; }
private:
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

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    HBRUSH invalidBrush_ = nullptr;
    std::array<Field, 11> fields_{};
    zengine::GameObject* object_ = nullptr;
    std::function<void()> changed_;
    bool updating_ = false;
    int scroll_ = 0;
    int pressed_ = -1;
    bool scrubbing_ = false;
    POINT startPoint_{};
    float startValue_ = 0;
    float scrubStep_ = 0.01f;
};
