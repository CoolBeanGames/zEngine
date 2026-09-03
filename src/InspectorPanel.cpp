#include "InspectorPanel.h"
#include "EditorStyle.h"
#include "core/ScriptBehavior.h"
#include "core/MeshRenderer.h"
#include "core/Camera.h"
#include "audio/AudioSource.h"
#include "audio/AudioEffect.h"
#include "core/Light.h"
#include "physics/PhysicsBehavior.h"
#include <windowsx.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr wchar_t ClassName[] = L"zEngineInspector";
    constexpr COLORREF Background = RGB(40, 42, 47), Text = RGB(214, 216, 221), BehaviorBorder = RGB(83, 87, 98), SectionBorder = RGB(112, 118, 132);
    constexpr COLORREF AxisColor(int axis) { return axis==0?RGB(239,92,92):axis==1?RGB(104,205,120):RGB(100,156,244); }
    std::wstring ReadText(HWND window)
    {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
        text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
        return text;
    }
    std::wstring Wide(const std::string& text)
    {
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring output(count, L' ');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count);
        return output;
    }
    std::string Utf8(const std::wstring& text)
    {
        const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string output(count, ' ');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr);
        return output;
    }
    bool ParseNumber(const std::wstring& text, float& value)
    {
        wchar_t* end = nullptr;
        errno = 0;
        value = std::wcstof(text.c_str(), &end);
        if (end == text.c_str()) return false;
        while (*end && std::iswspace(*end)) ++end;
        return !*end && errno != ERANGE && std::isfinite(value) && std::abs(value) <= 1'000'000.0f;
    }
}

InspectorPanel::~InspectorPanel()
{
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    if (behaviorFont_) DeleteObject(behaviorFont_);
    if (labelFont_) DeleteObject(labelFont_);
    if (background_) DeleteObject(background_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    if (invalidBrush_) DeleteObject(invalidBrush_);
    if (instance_) UnregisterClassW(ClassName, instance_);
}

void InspectorPanel::Create(HWND parent, HINSTANCE instance, HFONT font, std::function<void()> changed)
{
    instance_ = instance; font_ = font; changed_ = std::move(changed);
    LOGFONTW description{};
    if (!GetObjectW(font_,sizeof(description),&description)) throw std::runtime_error("Cannot read Inspector font.");
    description.lfWeight=FW_BOLD;
    labelFont_=CreateFontIndirectW(&description); // Script labels retain the body font size.
    const HDC dc=GetDC(parent);
    const int extraPixels=MulDiv(3,GetDeviceCaps(dc,LOGPIXELSY),72);
    description.lfHeight+=description.lfHeight<0 ? -extraPixels : extraPixels;
    behaviorFont_=CreateFontIndirectW(&description);
    if (behaviorFont_)
    {
        const auto old=SelectObject(dc,behaviorFont_);
        TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
        behaviorHeaderHeight_=std::max(32,static_cast<int>(metrics.tmHeight)+10);
        SelectObject(dc,old);
    }
    ReleaseDC(parent,dc);
    if (!labelFont_ || !behaviorFont_) throw std::runtime_error("Cannot create Inspector heading fonts.");
    background_ = CreateSolidBrush(Background);
    fieldBrush_ = CreateSolidBrush(RGB(52, 54, 60));
    invalidBrush_ = CreateSolidBrush(RGB(92, 45, 45));
    WNDCLASSW type{};
    type.hInstance = instance; type.lpfnWndProc = WindowProcedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = ClassName;
    if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register inspector panel.");
    window_ = CreateWindowExW(0, ClassName, L"GameObject Inspector", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                              0, 0, 1, 1, parent, nullptr, instance, this);
    if (!window_) throw std::runtime_error("Cannot create inspector panel.");
    for (int index = 0; index < static_cast<int>(fields_.size()); ++index)
    {
        auto& field = fields_[index];
        field.window = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(NameField + index)), instance, nullptr);
        if (!field.window) throw std::runtime_error("Cannot create inspector field.");
        SendMessageW(field.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(field.window, EM_SETLIMITTEXT, index < 2 ? 512 : 48, 0);
        SetWindowSubclass(field.window, EditProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    const auto button = [&](const wchar_t* title, int id, DWORD style = 0) {
        const auto result = CreateWindowExW(0, L"BUTTON", title, WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,
            0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if (!result) throw std::runtime_error("Cannot create behavior control.");
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        return result;
    };
    addBehaviorButton_ = button(L"+ Add Behavior", AddBehaviorButton);
    meshEnabled_ = button(L"Mesh Renderer enabled", MeshEnabled, BS_AUTOCHECKBOX);
    chooseMesh_ = button(L"Choose Model...", ChooseMeshButton);
    cubeMesh_ = button(L"Use Cube", CubeMeshButton);
    clearMesh_ = button(L"Clear", ClearMeshButton);
    editorStyle::AttachChildren(window_);
    Bind(nullptr);
    Layout();
}

void InspectorPanel::Bind(zengine::ObjectCore* object,bool editData,bool editTransform)
{
    if (object_ != object) collapsedBehaviors_.clear();
    EndScrub(true);
    for (int index = 0; index < static_cast<int>(fields_.size()); ++index)
        if (GetFocus() == fields_[index].window) { FinishField(index, false); SetFocus(window_); }
    for (std::size_t index=0;index<behaviorFields_.size();++index)
        if (behaviorFields_[index].field.window && GetFocus()==behaviorFields_[index].field.window)
        { FinishBehaviorField(index,false); SetFocus(window_); }
    object_ = object;
    editData_=editData; editTransform_=editTransform;
    RefreshFields();
    RefreshBehaviors();
}
void InspectorPanel::RefreshBehaviors()
{
    // Rebuild only at structural changes/save/session boundaries, never on each tick.
    updating_=true;
    for (auto& entry:behaviorFields_) { if (entry.field.window) DestroyWindow(entry.field.window); for (auto bit:entry.bits) if (bit) DestroyWindow(bit); }
    for (auto& toggle:behaviorToggles_) if (toggle.window) DestroyWindow(toggle.window);
    behaviorFields_.clear();
    behaviorToggles_.clear();
    bitButtonCount_=0;
    const auto add = [&](zengine::Behavior* behavior, std::string name, std::wstring label, bool priority, bool field, bool editable, BehaviorField::Style style=BehaviorField::Style::Normal,bool multiline=false,bool prefab=false,bool objectReference=false) {
        BehaviorField entry; entry.behavior=behavior; entry.name=std::move(name); entry.label=std::move(label); entry.priority=priority;
        entry.style=style;entry.multiline=multiline;entry.prefab=prefab;entry.objectReference=objectReference;
        const bool asButton = prefab || objectReference;
        if (field)
        {
            const auto id=FirstBehaviorField+behaviorFields_.size();
            entry.field.window=CreateWindowExW(0,asButton?L"BUTTON":L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|(asButton?BS_PUSHBUTTON:(multiline?(ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL):ES_AUTOHSCROLL)|((editable && editData_)?0:ES_READONLY)),
                0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
            if (!entry.field.window) throw std::runtime_error("Cannot create script field.");
            SendMessageW(entry.field.window,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            if(!asButton)SendMessageW(entry.field.window,EM_SETLIMITTEXT,multiline?65536:4096,0);
            SetWindowSubclass(entry.field.window,EditProcedure,1,reinterpret_cast<DWORD_PTR>(this));
            if(asButton)EnableWindow(entry.field.window,editData_ && (prefab ? editable : true));
        }
        behaviorFields_.push_back(std::move(entry));
        if (style == BehaviorField::Style::BehaviorHeader)
        {
            const auto id=FirstBehaviorToggle+static_cast<int>(behaviorToggles_.size());
            const auto title=collapsedBehaviors_.contains(behavior)?L"+":L"-";
            const auto toggle=CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
            if (!toggle) throw std::runtime_error("Cannot create behavior collapse control.");
            SendMessageW(toggle,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            SetWindowSubclass(toggle,EditProcedure,1,reinterpret_cast<DWORD_PTR>(this));
            editorStyle::Attach(toggle); // dark theme, matching the rest of the Inspector chrome
            behaviorToggles_.push_back({behavior,toggle});
        }
    };
    const auto addShape = [&](zengine::physics::Collider* collider) {
        BehaviorField entry;entry.behavior=collider;entry.name="shape";entry.label=L"Shape";entry.combo=true;
        const auto id=FirstBehaviorField+behaviorFields_.size();entry.field.window=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        if(!entry.field.window)throw std::runtime_error("Cannot create collider shape control.");SendMessageW(entry.field.window,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        for(const auto* value:{L"Box",L"Sphere",L"Capsule"})SendMessageW(entry.field.window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));behaviorFields_.push_back(std::move(entry));
    };
    // Generic drop-down for a ui:: control property (bool / anchor). `items` are
    // the LoadUiProperty value strings, in list order.
    const auto addUiCombo = [&](zengine::Behavior* behavior, const char* key, std::wstring label, std::vector<std::wstring> items) {
        BehaviorField entry; entry.behavior=behavior; entry.name=key; entry.label=std::move(label);
        entry.combo=true; entry.uiControl=true; entry.uiKind=zengine::ui::UiPropertyKind::Bool; entry.comboItems=std::move(items);
        const auto id=FirstBehaviorField+behaviorFields_.size();
        entry.field.window=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        if(!entry.field.window)throw std::runtime_error("Cannot create UI property combo.");
        SendMessageW(entry.field.window,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        for(const auto& item:entry.comboItems)SendMessageW(entry.field.window,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item.c_str()));
        EnableWindow(entry.field.window,editData_);
        behaviorFields_.push_back(std::move(entry));
    };
    // One edit-field row for a ui:: control property of the given kind.
    const auto addUiField = [&](zengine::Behavior* behavior, const zengine::ui::UiPropertyField& prop) {
        using K = zengine::ui::UiPropertyKind;
        const int axisCount = prop.kind==K::Vec2 ? 2 : prop.kind==K::Color ? 4 : 1;
        const bool multiline = prop.kind==K::Multiline;
        for (int axis=0; axis<axisCount; ++axis)
        {
            add(behavior, prop.key, Wide(prop.label), false, true, true, BehaviorField::Style::Normal, multiline);
            auto& e = behaviorFields_.back();
            e.uiControl=true; e.uiKind=prop.kind;
            if (axisCount>1) { e.axis=axis; e.axisCount=axisCount; }
        }
    };
    const auto addBitmask = [&](zengine::Behavior* behavior, std::string name, std::wstring label) {
        BehaviorField entry; entry.behavior=behavior; entry.name=std::move(name); entry.label=std::move(label); entry.bitmask=true;
        for (int bit=0; bit<CollisionBits; ++bit) {
            const auto id=FirstBehaviorBit + bitButtonCount_++;
            const auto button=CreateWindowExW(0,L"BUTTON",std::to_wstring(bit+1).c_str(),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX|BS_PUSHLIKE,
                0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
            if(!button)throw std::runtime_error("Cannot create collision bit control.");
            SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            EnableWindow(button,editData_);
            entry.bits.push_back(button);
        }
        behaviorFields_.push_back(std::move(entry));
    };
    if (object_) for (std::size_t i=0;i<object_->BehaviorCount();++i)
    {
        auto& behavior=object_->BehaviorAt(i);
        auto* script=dynamic_cast<zengine::ScriptBehavior*>(&behavior);
        auto* uiCtl=dynamic_cast<zengine::ui::UiControl*>(&behavior);
        const auto name=script ? std::filesystem::path(Wide(script->Asset())).stem().wstring() :
            uiCtl ? L"UI — "+Wide(uiCtl->TypeName()) :
            dynamic_cast<zengine::MeshRenderer*>(&behavior) ? L"Mesh Renderer" :dynamic_cast<zengine::audio::AudioSource*>(&behavior)?L"Audio Player":dynamic_cast<zengine::audio::AudioEffect*>(&behavior)?L"Audio Effect":dynamic_cast<zengine::Light*>(&behavior)?L"Light":dynamic_cast<zengine::physics::Collider*>(&behavior)?L"Collider":dynamic_cast<zengine::Camera*>(&behavior)?L"Camera":dynamic_cast<zengine::physics::RigidBody*>(&behavior)?L"Rigid Body":dynamic_cast<zengine::physics::KinematicBody*>(&behavior)?L"Kinematic Body":dynamic_cast<zengine::physics::StaticBody*>(&behavior)?L"Static Body":dynamic_cast<zengine::physics::Area*>(&behavior)?L"Area":L"Native Behavior";
        add(&behavior,{},name,false,false,false,BehaviorField::Style::BehaviorHeader);
        add(&behavior,{},L"Priority (higher runs first)",true,true,true);
        if(auto* meshRenderer=dynamic_cast<zengine::MeshRenderer*>(&behavior))
        {
            add(&behavior,"__material",L"Material (.material asset)",false,true,true);
            behaviorFields_.back().materialPath=true;
            if(resolveMaterial_ && !meshRenderer->Material().empty())
            {
                zengine::materials::Effective effective;
                try { effective=resolveMaterial_(meshRenderer->Material()); } catch(...) { effective.ok=false; effective.error="Cannot load material."; }
                using PT=zengine::shaders::ParamType;
                for(const auto& parameter:effective.parameters)
                {
                    const int axes = parameter.type==PT::Float4?4 : parameter.type==PT::Float3?3 : parameter.type==PT::Float2?2 : parameter.type==PT::Texture2D?1 : 1;
                    for(int a=0;a<axes;++a)
                    {
                        add(&behavior,parameter.name,Wide(parameter.name+(parameter.type==PT::Texture2D?" (texture)":"")),false,true,true);
                        auto& e=behaviorFields_.back();
                        e.materialParam=true; e.materialType=parameter.type;
                        if(parameter.type!=PT::Texture2D && axes>1){e.axis=a;e.axisCount=axes;}
                    }
                }
                if(!effective.ok && !effective.error.empty())
                    add(&behavior,{},L"Material: "+Wide(effective.error),false,false,false,BehaviorField::Style::ScriptLabel);
            }
        }
        if(dynamic_cast<zengine::Camera*>(&behavior))
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{{"fov",L"Field of view (degrees)"},{"near",L"Near plane"},{"far",L"Far plane"}})add(&behavior,key,label,false,true,true);
        if(dynamic_cast<zengine::audio::AudioSource*>(&behavior))
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{
                {"clip",L"Clip (.wav/.mp3/.ogg/.flac)"},{"spatial",L"3D positional (0 or 1)"},{"autoplay",L"Autoplay (0 or 1)"},
                {"loop",L"Loop (0 or 1)"},{"volume",L"Volume (0 - 1)"},{"pitch",L"Pitch (0.05 - 4)"},
                {"attenuation",L"Attenuation (none / linear / inverse)"},{"min_distance",L"Min distance (full volume)"},
                {"max_distance",L"Max distance (silent)"}})
                add(&behavior,key,label,false,true,true);
        if(dynamic_cast<zengine::audio::AudioEffect*>(&behavior))
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{
                {"effect",L"Effect (reverb)"},{"decay",L"Reverb decay (seconds)"},{"wet_mix",L"Wet mix (0 - 1)"},
                {"blend_distance",L"Boundary blend (world units)"}})
                add(&behavior,key,label,false,true,true);
        if(dynamic_cast<zengine::Light*>(&behavior)) {
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{
                {"light_type",L"Type (point / directional / spot)"},{"intensity",L"Intensity"},{"range",L"Range (point / spot)"},
                {"falloff",L"Falloff exponent"},{"spot_inner",L"Spot inner angle (deg)"},{"spot_outer",L"Spot outer angle (deg)"},
                {"static",L"Static - bakes into lightmaps (0 or 1)"}})
                add(&behavior,key,label,false,true,true);
            for(int axis=0;axis<3;++axis){add(&behavior,"light_color",L"Colour (RGB)",false,true,true);behaviorFields_.back().axis=axis;behaviorFields_.back().axisCount=3;}
        }
        if(auto* collider=dynamic_cast<zengine::physics::Collider*>(&behavior)){
            addShape(collider);
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{{"offset",L"Offset"},{"size",L"Size"}})
                for(int axis=0;axis<3;++axis){add(&behavior,key,label,false,true,true);behaviorFields_.back().axis=axis;}
        }
        if(auto* body=dynamic_cast<zengine::physics::Body*>(&behavior)) {
            addBitmask(&behavior,"layer",L"Collision layers");
            addBitmask(&behavior,"mask",L"Collision mask (collides with)");
            for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{{"friction",L"Friction"},{"bounciness",L"Bounciness (0 - 1)"}})add(&behavior,key,label,false,true,true);
            if(dynamic_cast<zengine::physics::RigidBody*>(body)){add(&behavior,"mass",L"Mass",false,true,true);add(&behavior,"gravity",L"Gravity scale",false,true,true);}
            if(dynamic_cast<zengine::physics::MovingBody*>(body))for(const auto& [key,label]:std::initializer_list<std::pair<const char*,const wchar_t*>>{{"velocity",L"Velocity"},{"angular_velocity",L"Angular velocity"},{"constant_force",L"Constant force"},{"constant_torque",L"Constant torque"}})for(int axis=0;axis<3;++axis){add(&behavior,key,label,false,true,true);behaviorFields_.back().axis=axis;}
        }
        if (script && scriptHost_)
        {
            const auto error=scriptHost_->Error(*script);
            if (!error.empty()) add(&behavior,{},L"Error: "+Wide(error),false,false,false);
            const auto addArrayButton = [&](const wchar_t* caption) {
                const auto id=FirstBehaviorBit + bitButtonCount_++;
                const auto button=CreateWindowExW(0,L"BUTTON",caption,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
                    0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
                if(!button)throw std::runtime_error("Cannot create array control.");
                SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
                EnableWindow(button,editData_ && !(scriptHost_&&scriptHost_->Playing()));
                editorStyle::Attach(button);
                behaviorFields_.back().bits.push_back(button);
            };
            for (const auto& field:scriptHost_->Fields(*script))
            {
                if(field.array) {
                    add(&behavior,field.name,Wide(field.name+" [array] — "+field.value),false,false,false,BehaviorField::Style::ScriptLabel);
                    behaviorFields_.back().arrayHeader=true; behaviorFields_.back().type="array";
                    addArrayButton(L"+ Add element");
                    continue;
                }
                if(field.arrayIndex>=0) {
                    const bool ref=field.reference, prefab=field.type=="prefab";
                    add(&behavior,field.name,Wide("["+std::to_string(field.arrayIndex)+"]  "+field.type),false,true,field.editable && !ref,
                        BehaviorField::Style::Normal,false,prefab,ref);
                    behaviorFields_.back().type=field.type; behaviorFields_.back().arrayIndex=field.arrayIndex;
                    addArrayButton(L"✕");
                    continue;
                }
                if((field.type=="Vector3" || field.type=="Vector2") && !field.name.empty()) {
                    const int count = field.type=="Vector2" ? 2 : 3;
                    for(int axis=0;axis<count;++axis) {
                        add(&behavior,field.name,Wide(field.name),false,true,field.editable);
                        behaviorFields_.back().axis=axis; behaviorFields_.back().axisCount=count;
                    }
                    continue;
                }
                add(&behavior,field.name,field.name.empty()?Wide(field.label):Wide(field.name+" ("+field.type+")"),false,!field.name.empty(),field.editable,
                    field.name.empty()?BehaviorField::Style::ScriptLabel:BehaviorField::Style::Normal,field.multiline,field.type=="prefab",field.reference);
                if(!field.name.empty()) behaviorFields_.back().type=field.type;
            }
        }
        if (uiCtl)
        {
            using K = zengine::ui::UiPropertyKind;
            for (const auto& prop : zengine::ui::UiControlSchema(*uiCtl))
            {
                if (prop.kind == K::Bool) addUiCombo(&behavior, prop.key, Wide(prop.label), {L"false", L"true"});
                else if (prop.kind == K::Anchor)
                {
                    std::vector<std::wstring> anchors;
                    for (int a=0; a<20; ++a) anchors.push_back(Wide(zengine::ui::AnchorName(static_cast<zengine::ui::Anchor>(a))));
                    addUiCombo(&behavior, prop.key, Wide(prop.label), std::move(anchors));
                    behaviorFields_.back().uiKind = K::Anchor;
                }
                else addUiField(&behavior, prop);
            }
        }
    }
    for (std::size_t i=0;i<behaviorFields_.size();++i)
    {
        if (behaviorFields_[i].bitmask) { RefreshCollisionBits(behaviorFields_[i]); continue; }
        auto control=behaviorFields_[i].field.window; if (!control) continue;
        const auto value=BehaviorValue(i);
        if(behaviorFields_[i].combo && behaviorFields_[i].uiControl)
        {
            int sel=0;
            for (int n=0;n<static_cast<int>(behaviorFields_[i].comboItems.size());++n)
                if (behaviorFields_[i].comboItems[n]==value) { sel=n; break; }
            SendMessageW(control,CB_SETCURSEL,sel,0);
        }
        else if(behaviorFields_[i].combo)SendMessageW(control,CB_SETCURSEL,dynamic_cast<zengine::physics::Collider*>(behaviorFields_[i].behavior)?static_cast<int>(dynamic_cast<zengine::physics::Collider*>(behaviorFields_[i].behavior)->Shape()):0,0);
        else if(behaviorFields_[i].objectReference) SetWindowTextW(control,(value.empty()||value==L"None")?L"None — click to pick":value.c_str());
        else SetWindowTextW(control,(behaviorFields_[i].prefab&&value.empty())?L"Choose prefab...":value.c_str());
        behaviorFields_[i].field.focusText=value;
    }
    updating_=false;
    EnableWindow(addBehaviorButton_, object_ != nullptr && editData_);
    for (const auto control:{meshEnabled_,chooseMesh_,cubeMesh_,clearMesh_}) EnableWindow(control,object_!=nullptr && editData_);
    const auto* mesh = object_ ? object_->GetBehavior<zengine::MeshRenderer>() : nullptr;
    SendMessageW(meshEnabled_, BM_SETCHECK, mesh && mesh->Enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    Layout();
}

HWND InspectorPanel::FieldWindowForProperty(const zengine::Behavior* behavior, const std::string& key, int axis) const
{
    for (const auto& entry : behaviorFields_)
        if (entry.behavior == behavior && entry.uiControl && entry.name == key && (entry.axis < 0 || entry.axis == axis))
            return entry.field.window;
    return nullptr;
}
int InspectorPanel::BehaviorHeight() const
{
    int height=0;
    for (const auto& entry:behaviorFields_) height+=RowHeight(entry);
    return height;
}
int InspectorPanel::RowHeight(const BehaviorField& entry) const
{
    if (entry.style != BehaviorField::Style::BehaviorHeader && IsBehaviorCollapsed(entry)) return 0;
    if(entry.axis>=0)return entry.axis==entry.axisCount-1?72:0;
    if(entry.bitmask)return 82;
    if(entry.arrayHeader)return 32;
    if(entry.arrayIndex>=0)return 48;
    return entry.style==BehaviorField::Style::BehaviorHeader ? behaviorHeaderHeight_ : entry.multiline?112:entry.field.window?52:24;
}
bool InspectorPanel::IsBehaviorCollapsed(const BehaviorField& entry) const
{
    return entry.behavior && entry.style != BehaviorField::Style::BehaviorHeader && collapsedBehaviors_.contains(entry.behavior);
}
std::wstring InspectorPanel::BehaviorValue(std::size_t index)
{
    const auto& entry=behaviorFields_.at(index);
    if (entry.materialPath)
    {
        auto* mr=dynamic_cast<zengine::MeshRenderer*>(entry.behavior);
        return mr ? Wide(mr->Material()) : std::wstring{};
    }
    if (entry.materialParam)
    {
        auto* mr=dynamic_cast<zengine::MeshRenderer*>(entry.behavior);
        if(!mr || !resolveMaterial_) return {};
        zengine::materials::Effective effective;
        try { effective=resolveMaterial_(mr->Material()); } catch(...) { return {}; }
        const auto* value=effective.Find(entry.name);
        if(!value) return {};
        if(value->type==zengine::shaders::ParamType::Texture2D) return Wide(value->texture);
        const int axis=entry.axis<0?0:entry.axis;
        std::wostringstream out;out<<std::setprecision(9)<<value->numbers[static_cast<std::size_t>(axis)];return out.str();
    }
    if (entry.uiControl)
    {
        auto* uiCtl=dynamic_cast<zengine::ui::UiControl*>(entry.behavior);
        if(!uiCtl)return {};
        std::string raw;
        for(const auto& [k,v]:zengine::ui::SaveUiControl(*uiCtl)) if(k==entry.name){raw=v;break;}
        if(entry.uiKind==zengine::ui::UiPropertyKind::Bool) return (raw=="1"||raw=="true")?L"true":L"false";
        if(entry.axis>=0){std::istringstream in(raw);std::string tok="0";for(int a=0;a<=entry.axis;++a)if(!(in>>tok))tok="0";return Wide(tok);}
        return Wide(raw);
    }
    if(entry.combo)return {};
    if (entry.priority) { std::wostringstream out; out<<std::setprecision(9)<<entry.behavior->Priority(); return out.str(); }
    if (auto* camera=dynamic_cast<zengine::Camera*>(entry.behavior)) {
        std::wostringstream out; out<<std::setprecision(9);
        if(entry.name=="fov")out<<camera->FieldOfView(); else if(entry.name=="near")out<<camera->NearPlane(); else if(entry.name=="far")out<<camera->FarPlane();
        return out.str();
    }
    if (auto* src=dynamic_cast<zengine::audio::AudioSource*>(entry.behavior)) {
        if(entry.name=="clip")return Wide(src->Clip());
        if(entry.name=="attenuation")return Wide(zengine::audio::AttenuationName(src->AttenuationModel()));
        std::wostringstream out; out<<std::setprecision(9);
        if(entry.name=="spatial")out<<(src->Spatial()?1:0);
        else if(entry.name=="autoplay")out<<(src->Autoplay()?1:0);
        else if(entry.name=="loop")out<<(src->Loop()?1:0);
        else if(entry.name=="volume")out<<src->Volume();
        else if(entry.name=="pitch")out<<src->Pitch();
        else if(entry.name=="min_distance")out<<src->MinDistance();
        else if(entry.name=="max_distance")out<<src->MaxDistance();
        return out.str();
    }
    if (auto* fx=dynamic_cast<zengine::audio::AudioEffect*>(entry.behavior)) {
        if(entry.name=="effect")return Wide(zengine::audio::EffectKindName(fx->Kind()));
        std::wostringstream out; out<<std::setprecision(9);
        if(entry.name=="decay")out<<fx->Decay();
        else if(entry.name=="wet_mix")out<<fx->WetMix();
        else if(entry.name=="blend_distance")out<<fx->BlendDistance();
        return out.str();
    }
    if (auto* lg=dynamic_cast<zengine::Light*>(entry.behavior)) {
        if(entry.name=="light_type")return Wide(zengine::LightTypeName(lg->LightType()));
        std::wostringstream out; out<<std::setprecision(9);
        if(entry.name=="light_color"){const auto c=lg->Color();out<<(entry.axis==0?c.x:entry.axis==1?c.y:c.z);}
        else if(entry.name=="intensity")out<<lg->Intensity();
        else if(entry.name=="range")out<<lg->Range();
        else if(entry.name=="falloff")out<<lg->Falloff();
        else if(entry.name=="spot_inner")out<<lg->SpotInner();
        else if(entry.name=="spot_outer")out<<lg->SpotOuter();
        else if(entry.name=="static")out<<(lg->Static()?1:0);
        return out.str();
    }
    if (auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior); script && scriptHost_)
        for (const auto& field:scriptHost_->Fields(*script)) if (field.name==entry.name
            && field.array==entry.arrayHeader && (entry.arrayIndex<0 ? field.arrayIndex<0 : field.arrayIndex==entry.arrayIndex)) {
            if(entry.axis<0){auto value=Wide(field.value);if(entry.multiline){std::wstring display;for(std::size_t i=0;i<value.size();++i){if(value[i]==L'\n' && (!i || value[i-1]!=L'\r'))display+=L'\r';display+=value[i];}return display;}return value;}
            auto values=Wide(field.value);std::replace(values.begin(),values.end(),L',',L' ');
            std::wistringstream input(values);double v=0;
            for(int axis=0;axis<=entry.axis;++axis)input>>v;
            std::wostringstream out;out<<std::setprecision(9)<<v;return out.str();
        }
    if(auto* body=dynamic_cast<zengine::physics::Body*>(entry.behavior)) {
        std::wostringstream out;out<<std::setprecision(9);
        if(entry.name=="layer")out<<body->Layer();else if(entry.name=="mask")out<<body->Mask();else if(entry.name=="friction")out<<body->Friction();else if(entry.name=="bounciness")out<<body->Bounciness();
        else if(auto* rigid=dynamic_cast<zengine::physics::RigidBody*>(body);rigid&&(entry.name=="mass"||entry.name=="gravity"))out<<(entry.name=="mass"?rigid->Mass():rigid->GravityScale());
        else if(auto* moving=dynamic_cast<zengine::physics::MovingBody*>(body)){const auto value=entry.name=="velocity"?moving->Velocity():entry.name=="angular_velocity"?moving->AngularVelocity():entry.name=="constant_force"?moving->ConstantForce():moving->ConstantTorque();out<<(entry.axis==0?value.x:entry.axis==1?value.y:value.z);}
        return out.str();
    }
    if(auto* collider=dynamic_cast<zengine::physics::Collider*>(entry.behavior);collider && entry.axis>=0) {
        const auto value=entry.name=="offset"?collider->Offset():collider->Size();
        std::wostringstream out;out<<std::setprecision(9)<<(entry.axis==0?value.x:entry.axis==1?value.y:value.z);return out.str();
    }
    return {};
}
void InspectorPanel::ChangeBehaviorField(std::size_t index)
{
    if (updating_ || !editData_) return;
    auto& entry=behaviorFields_.at(index);
    if (entry.materialPath)
    {
        if(auto* mr=dynamic_cast<zengine::MeshRenderer*>(entry.behavior))
        { mr->SetMaterial(Utf8(ReadText(entry.field.window))); entry.field.valid=true; if(changed_)changed_(); RefreshBehaviors(); }
        return;
    }
    if (entry.materialParam)
    {
        auto* mr=dynamic_cast<zengine::MeshRenderer*>(entry.behavior);
        if(!mr || !setMaterialValue_ || !resolveMaterial_) return;
        try
        {
            zengine::materials::Value value; value.name=entry.name; value.type=entry.materialType;
            if(entry.materialType==zengine::shaders::ParamType::Texture2D)
                value.texture=Utf8(ReadText(entry.field.window));
            else
            {
                const int axes=entry.axisCount>0&&entry.axis>=0?entry.axisCount:1;
                const auto current=resolveMaterial_(mr->Material()).Numbers(entry.name,{{0,0,0,0}});
                value.numbers=current;
                const int axis=entry.axis<0?0:entry.axis;
                float parsed; if(!ParseNumber(ReadText(entry.field.window),parsed)) throw std::invalid_argument("bad number");
                value.numbers[static_cast<std::size_t>(axis)]=parsed;
                (void)axes;
            }
            setMaterialValue_(mr->Material(),value);
            entry.field.valid=true; if(changed_)changed_();
        }
        catch(const std::exception&){ entry.field.valid=false; }
        InvalidateRect(entry.field.window,nullptr,FALSE);
        return;
    }
    if (entry.uiControl)
    {
        auto* uiCtl=dynamic_cast<zengine::ui::UiControl*>(entry.behavior);
        try
        {
            if(!uiCtl)throw std::invalid_argument("no control");
            using K=zengine::ui::UiPropertyKind;
            std::string out;
            if(entry.combo)
            {
                const auto sel=SendMessageW(entry.field.window,CB_GETCURSEL,0,0);
                if(sel<0||sel>=static_cast<int>(entry.comboItems.size()))throw std::invalid_argument("bad choice");
                out=Utf8(entry.comboItems[static_cast<std::size_t>(sel)]);
            }
            else
            {
                const auto text=ReadText(entry.field.window);
                if(entry.axis>=0)
                {
                    float v;if(!ParseNumber(text,v))throw std::invalid_argument("bad number");
                    const auto base=index-entry.axis;std::wstring combined;
                    for(int a=0;a<entry.axisCount;++a){if(a)combined+=L' ';combined+=(a==entry.axis?text:BehaviorValue(base+a));}
                    out=Utf8(combined);
                }
                else if(entry.uiKind==K::Float||entry.uiKind==K::Int)
                {
                    float v;if(!ParseNumber(text,v))throw std::invalid_argument("bad number");
                    out=Utf8(text);
                }
                else if(entry.multiline)
                {
                    std::string s=Utf8(text),n;for(std::size_t i=0;i<s.size();++i){if(s[i]=='\r'&&i+1<s.size()&&s[i+1]=='\n')continue;n+=s[i];}out=std::move(n);
                }
                else out=Utf8(text);
            }
            zengine::ui::LoadUiProperty(*uiCtl,entry.name,out);
            entry.field.valid=true;
            if(changed_)changed_();
        }
        catch(const std::exception&){entry.field.valid=false;}
        InvalidateRect(entry.field.window,nullptr,FALSE);
        return;
    }
    try
    {
        if(entry.combo){auto* collider=dynamic_cast<zengine::physics::Collider*>(entry.behavior);const auto selected=SendMessageW(entry.field.window,CB_GETCURSEL,0,0);if(!collider||selected<0||selected>2)throw std::invalid_argument("Invalid collider shape");collider->SetShape(static_cast<zengine::physics::ColliderShape>(selected));entry.field.valid=true;if(changed_)changed_();return;}
        const auto text=ReadText(entry.field.window);
        if (entry.priority)
        {
            wchar_t* end=nullptr; errno=0;
            float value=std::wcstof(text.c_str(),&end);
            if (end==text.c_str()) throw std::invalid_argument("Invalid priority");
            while (*end && std::iswspace(*end)) ++end;
            if (*end || errno==ERANGE || !std::isfinite(value)) throw std::invalid_argument("Invalid priority");
            entry.behavior->SetPriority(value);
        }
        else if (auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior); script && scriptHost_) {
            if(entry.arrayIndex>=0){scriptHost_->SetArrayElement(*script,entry.name,static_cast<std::size_t>(entry.arrayIndex),Utf8(text));}
            else if(entry.axis<0){auto value=Utf8(text);if(entry.multiline){std::string normalized;for(std::size_t i=0;i<value.size();++i){if(value[i]=='\r' && i+1<value.size() && value[i+1]=='\n')continue;normalized+=value[i];}value=std::move(normalized);}scriptHost_->SetField(*script,entry.name,value);}
            else {
                wchar_t* end=nullptr;errno=0;const double value=std::wcstod(text.c_str(),&end);
                if(end==text.c_str())throw std::invalid_argument("Invalid vector component");
                while(*end && std::iswspace(*end))++end;
                if(*end || errno==ERANGE || !std::isfinite(value))throw std::invalid_argument("Invalid vector component");
                std::wstring combined;
                for(int axis=0;axis<entry.axisCount;++axis) {
                    if(axis)combined+=L", ";combined+=axis==entry.axis?text:BehaviorValue(index-entry.axis+axis);
                }
                scriptHost_->SetField(*script,entry.name,Utf8(combined));
            }
        }
        else if(auto* collider=dynamic_cast<zengine::physics::Collider*>(entry.behavior);collider && entry.axis>=0) {
            float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid collider vector");
            const auto base=index-entry.axis;auto vector=entry.name=="offset"?collider->Offset():collider->Size();float* parts[]={&vector.x,&vector.y,&vector.z};
            for(int axis=0;axis<3;++axis)if(axis==entry.axis)*parts[axis]=value;else if(!ParseNumber(BehaviorValue(base+axis),*parts[axis]))throw std::invalid_argument("Invalid collider vector");
            if(entry.name=="offset")collider->SetOffset(vector);else collider->SetSize(vector);
        }
        else if(auto* camera=dynamic_cast<zengine::Camera*>(entry.behavior)) {
            float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid camera value");
            if(entry.name=="fov")camera->SetFieldOfView(value);else if(entry.name=="near")camera->SetNearPlane(value);else camera->SetFarPlane(value);
        }
        else if(auto* src=dynamic_cast<zengine::audio::AudioSource*>(entry.behavior)) {
            const auto raw=Utf8(text);
            if(entry.name=="clip")src->SetClip(raw);
            else if(entry.name=="attenuation"){zengine::audio::Attenuation a;if(!zengine::audio::ParseAttenuation(raw,a))throw std::invalid_argument("Attenuation must be none, linear or inverse");src->SetAttenuationModel(a);}
            else {
                float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid audio number");
                if(entry.name=="spatial")src->SetSpatial(value!=0);
                else if(entry.name=="autoplay")src->SetAutoplay(value!=0);
                else if(entry.name=="loop")src->SetLoop(value!=0);
                else if(entry.name=="volume")src->SetVolume(value);
                else if(entry.name=="pitch")src->SetPitch(value);
                else if(entry.name=="min_distance")src->SetMinDistance(value);
                else if(entry.name=="max_distance")src->SetMaxDistance(value);
            }
        }
        else if(auto* fx=dynamic_cast<zengine::audio::AudioEffect*>(entry.behavior)) {
            if(entry.name=="effect"){zengine::audio::EffectKind k;if(!zengine::audio::ParseEffectKind(Utf8(text),k))throw std::invalid_argument("Only 'reverb' is supported");fx->SetKind(k);}
            else {
                float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid audio effect number");
                if(entry.name=="decay")fx->SetDecay(value);
                else if(entry.name=="wet_mix")fx->SetWetMix(value);
                else if(entry.name=="blend_distance")fx->SetBlendDistance(value);
            }
        }
        else if(auto* lg=dynamic_cast<zengine::Light*>(entry.behavior)) {
            if(entry.name=="light_type"){zengine::Light::Type t;if(!zengine::ParseLightType(Utf8(text),t))throw std::invalid_argument("Type must be point, directional or spot");lg->SetLightType(t);}
            else {
                float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid light number");
                if(entry.name=="light_color"){auto c=lg->Color();(entry.axis==0?c.x:entry.axis==1?c.y:c.z)=value;lg->SetColor(c);}
                else if(entry.name=="intensity")lg->SetIntensity(value);
                else if(entry.name=="range")lg->SetRange(value);
                else if(entry.name=="falloff")lg->SetFalloff(value);
                else if(entry.name=="spot_inner")lg->SetSpotInner(value);
                else if(entry.name=="spot_outer")lg->SetSpotOuter(value);
                else if(entry.name=="static")lg->SetStatic(value!=0);
            }
        }
        else if(auto* body=dynamic_cast<zengine::physics::Body*>(entry.behavior)) {
            if(entry.name=="layer"||entry.name=="mask"){wchar_t* end=nullptr;errno=0;const auto value=std::wcstoull(text.c_str(),&end,0);while(end&&*end&&std::iswspace(*end))++end;if(!end||end==text.c_str()||*end||errno==ERANGE||value>0xffffffffull)throw std::invalid_argument("Invalid collision bits");if(entry.name=="layer")body->SetLayer(static_cast<std::uint32_t>(value));else body->SetMask(static_cast<std::uint32_t>(value));}
            else if(entry.axis<0){float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid physics number");if(entry.name=="friction")body->SetFriction(value);else if(entry.name=="bounciness")body->SetBounciness(value);else if(auto* rigid=dynamic_cast<zengine::physics::RigidBody*>(body);entry.name=="mass")rigid->SetMass(value);else if(rigid)rigid->SetGravityScale(value);}
            else {float value;if(!ParseNumber(text,value))throw std::invalid_argument("Invalid physics vector");const auto base=index-entry.axis;zengine::Vec3 vector{};float* parts[]={&vector.x,&vector.y,&vector.z};for(int axis=0;axis<3;++axis){if(axis==entry.axis)*parts[axis]=value;else if(!ParseNumber(BehaviorValue(base+axis),*parts[axis]))throw std::invalid_argument("Invalid physics vector");}auto* moving=dynamic_cast<zengine::physics::MovingBody*>(body);if(entry.name=="velocity")moving->SetVelocity(vector);else if(entry.name=="angular_velocity")moving->SetAngularVelocity(vector);else if(entry.name=="constant_force")moving->SetConstantForce(vector);else moving->SetConstantTorque(vector);}
        }
        entry.field.valid=true;
        if (changed_) changed_();
    }
    catch (const std::exception&) { entry.field.valid=false; }
    InvalidateRect(entry.field.window,nullptr,FALSE);
}
void InspectorPanel::RefreshCollisionBits(const BehaviorField& entry) const
{
    auto* body=dynamic_cast<zengine::physics::Body*>(entry.behavior);
    if(!body)return;
    const std::uint32_t value=entry.name=="layer"?body->Layer():body->Mask();
    for(int bit=0;bit<static_cast<int>(entry.bits.size());++bit)
        if(entry.bits[bit])SendMessageW(entry.bits[bit],BM_SETCHECK,((value>>bit)&1u)?BST_CHECKED:BST_UNCHECKED,0);
}
void InspectorPanel::ToggleCollisionBit(std::size_t fieldIndex,int bit)
{
    if(!editData_||fieldIndex>=behaviorFields_.size())return;
    auto& entry=behaviorFields_[fieldIndex];
    auto* body=dynamic_cast<zengine::physics::Body*>(entry.behavior);
    if(!body||bit<0||bit>=static_cast<int>(entry.bits.size()))return;
    const bool on=SendMessageW(entry.bits[bit],BM_GETCHECK,0,0)==BST_CHECKED;
    std::uint32_t value=entry.name=="layer"?body->Layer():body->Mask();
    if(on)value|=(1u<<bit); else value&=~(1u<<bit);
    if(entry.name=="layer")body->SetLayer(value); else body->SetMask(value);
    if(changed_)changed_();
}
void InspectorPanel::ShowAddArrayElementMenu(std::size_t fieldIndex)
{
    if(!editData_ || fieldIndex>=behaviorFields_.size() || !scriptHost_ || scriptHost_->Playing())return;
    auto& entry=behaviorFields_[fieldIndex];
    auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);
    if(!script)return;
    const auto& types=zengine::ScriptHost::ArrayElementTypes();
    HMENU menu=CreatePopupMenu();
    for(std::size_t i=0;i<types.size();++i)
    {
        if(i==7)AppendMenuW(menu,MF_SEPARATOR,0,nullptr); // value types | reference types
        AppendMenuW(menu,MF_STRING,static_cast<UINT_PTR>(i+1),Wide(types[i]).c_str());
    }
    RECT r{};if(!entry.bits.empty())GetWindowRect(entry.bits[0],&r);
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,r.left,r.bottom,0,window_,nullptr);
    DestroyMenu(menu);
    if(command>=1 && command<=static_cast<int>(types.size()))
    {
        try { scriptHost_->AddArrayElement(*script,entry.name,types[command-1]); RefreshBehaviors(); if(changed_)changed_(); }
        catch(const std::exception&) {}
    }
}
void InspectorPanel::RemoveArrayElementAt(std::size_t fieldIndex)
{
    if(!editData_ || fieldIndex>=behaviorFields_.size() || !scriptHost_ || scriptHost_->Playing())return;
    auto& entry=behaviorFields_[fieldIndex];
    auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);
    if(!script || entry.arrayIndex<0)return;
    try { scriptHost_->RemoveArrayElement(*script,entry.name,static_cast<std::size_t>(entry.arrayIndex)); RefreshBehaviors(); if(changed_)changed_(); }
    catch(const std::exception&) {}
}
void InspectorPanel::FinishBehaviorField(std::size_t index, bool cancel)
{
    auto& entry=behaviorFields_.at(index);
    if(entry.combo||entry.prefab||entry.objectReference||entry.bitmask)return;
    if (cancel) { updating_=true; SetWindowTextW(entry.field.window,entry.field.focusText.c_str()); updating_=false; ChangeBehaviorField(index); }
    const auto value=BehaviorValue(index);
    updating_=true; SetWindowTextW(entry.field.window,value.c_str()); updating_=false;
    entry.field.valid=true; entry.field.focusText=value;
}
void InspectorPanel::RefreshLiveValues()
{
    for (int i=2;i<static_cast<int>(fields_.size());++i)
        if (GetFocus()!=fields_[i].window && pressed_!=i) SetText(i,FieldValue(i));
    updating_=true;
    for (std::size_t i=0;i<behaviorFields_.size();++i)
    {
        if (behaviorFields_[i].bitmask) { RefreshCollisionBits(behaviorFields_[i]); continue; }
        if (const auto control=behaviorFields_[i].field.window; control && GetFocus()!=control && !behaviorFields_[i].combo)
        { const auto value=BehaviorValue(i);SetWindowTextW(control,(behaviorFields_[i].prefab&&value.empty())?L"Choose prefab...":value.c_str()); behaviorFields_[i].field.valid=true; }
    }
    updating_=false;
}

bool InspectorPanel::TransformFieldUsed(int index) const
{
    if (index < 2) return true;
    if (!object_ || !object_->Is2D()) return true;
    const int component = (index - 2) / 3, axis = (index - 2) % 3;
    // 2D: Position uses X/Y, Rotation uses the Z slot only, Scale uses X/Y.
    if (component == 1) return axis == 2;
    return axis != 2;
}
float InspectorPanel::Value(int index) const
{
    if (!object_) return 0;
    const int component = (index - 2) / 3, axis = (index - 2) % 3;
    if (object_->Is2D())
    {
        if (!TransformFieldUsed(index)) return 0;
        const auto& t = zengine::As2D(*object_).GetTransform();
        if (component == 0) return axis == 0 ? t.Position().x : t.Position().y;
        if (component == 1) return t.Rotation();
        return axis == 0 ? t.Scale().x : t.Scale().y;
    }
    const auto& transform = zengine::As3D(*object_).GetTransform();
    const auto& v = component == 0 ? transform.Position() : component == 1 ? transform.Rotation() : transform.Scale();
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}
void InspectorPanel::SetValue(int index, float value)
{
    if (!object_) return;
    const int component = (index - 2) / 3, axis = (index - 2) % 3;
    if (object_->Is2D())
    {
        if (!TransformFieldUsed(index)) return;
        auto& t = zengine::As2D(*object_).GetTransform();
        if (component == 0) { auto p = t.Position(); (axis == 0 ? p.x : p.y) = value; t.SetPosition(p); }
        else if (component == 1) t.SetRotation(value);
        else { auto s = t.Scale(); (axis == 0 ? s.x : s.y) = value; t.SetScale(s); }
        if (changed_) changed_();
        return;
    }
    auto& transform = zengine::As3D(*object_).GetTransform();
    auto v = component == 0 ? transform.Position() : component == 1 ? transform.Rotation() : transform.Scale();
    if (axis == 0) v.x = value; else if (axis == 1) v.y = value; else v.z = value;
    if (component == 0) transform.SetPosition(v);
    else if (component == 1) transform.SetRotation(v);
    else transform.SetScale(v);
    if (changed_) changed_();
}
std::wstring InspectorPanel::FieldValue(int index) const
{
    if (!object_) return {};
    if (index == 0) return Wide(object_->Name());
    if (index == 1)
    {
        std::string text;
        for (const auto& tag : object_->Tags()) { if (!text.empty()) text += ", "; text += tag; }
        return Wide(text);
    }
    if (!TransformFieldUsed(index)) return {};
    std::wostringstream text;
    text << std::setprecision(7) << Value(index);
    return text.str();
}
void InspectorPanel::SetText(int index, const std::wstring& value)
{
    updating_ = true;
    SetWindowTextW(fields_[index].window, value.c_str());
    fields_[index].valid = true;
    updating_ = false;
    InvalidateRect(fields_[index].window, nullptr, FALSE);
}
void InspectorPanel::RefreshFields()
{
    for (int index = 0; index < static_cast<int>(fields_.size()); ++index)
    {
        SetText(index, FieldValue(index));
        fields_[index].focusText = FieldValue(index);
        EnableWindow(fields_[index].window, object_ != nullptr && TransformFieldUsed(index) && (index<2?editData_:editTransform_));
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void InspectorPanel::ChangeField(int index)
{
    if (updating_ || !object_ || !(index<2?editData_:editTransform_)) return;
    const auto text = ReadText(fields_[index].window);
    try
    {
        if (index == 0) object_->SetName(Utf8(text));
        else if (index == 1)
        {
            std::vector<std::string> tags;
            std::istringstream stream(Utf8(text));
            std::string tag;
            while (std::getline(stream, tag, ',')) tags.push_back(tag);
            object_->SetTags(std::move(tags));
        }
        else
        {
            float value = 0;
            if (!ParseNumber(text, value)) throw std::invalid_argument("Invalid transform number");
            SetValue(index, value);
        }
        fields_[index].valid = true;
        if (changed_) changed_();
    }
    catch (const std::invalid_argument&) { fields_[index].valid = false; }
    InvalidateRect(fields_[index].window, nullptr, FALSE);
}
void InspectorPanel::FinishField(int index, bool cancel)
{
    if (cancel)
    {
        SetText(index, fields_[index].focusText);
        ChangeField(index);
    }
    SetText(index, FieldValue(index)); // Invalid/incomplete text reverts to the last valid value.
    fields_[index].focusText = FieldValue(index);
}
void InspectorPanel::EndScrub(bool cancel)
{
    if (pressed_ < 0) return;
    const int index = pressed_;
    pressed_ = -1;
    if (cancel && scrubbing_) SetValue(index, startValue_);
    scrubbing_ = false;
    SetText(index, FieldValue(index));
    if (GetCapture() == fields_[index].window) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
}

void InspectorPanel::Layout()
{
    RECT client{}; GetClientRect(window_, &client);
    const bool narrow = client.right < 250;
    const int behaviorHeight = BehaviorHeight();
    const bool hasMesh = object_ && object_->GetBehavior<zengine::MeshRenderer>();
    const int contentHeight = (narrow ? 488 : 438) + behaviorHeight + (hasMesh ? 122 : 0);
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMin = 0; scroll.nMax = contentHeight - 1; scroll.nPage = static_cast<UINT>(client.bottom);
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight - static_cast<int>(client.bottom)));
    scroll.nPos = scroll_; SetScrollInfo(window_, SB_VERT, &scroll, TRUE);
    GetClientRect(window_, &client);
    const int width = static_cast<int>(client.right);
    const int labelWidth = width >= 250 ? 68 : 12;
    const int column = std::max(24, (width - labelWidth - 16) / 3);
    HDWP positions=BeginDeferWindowPos(static_cast<int>(fields_.size()+behaviorFields_.size()+8)+bitButtonCount_);
    const auto place=[&](HWND control,int x,int y,int w,int h) {
        if(positions)positions=DeferWindowPos(positions,control,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        else SetWindowPos(control,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    };
    for (int index = 0; index < static_cast<int>(fields_.size()); ++index)
    {
        if (!fields_[index].window) continue;
        int x = 12, y = index == 0 ? 28 : 80, w = std::max(30, width - 24);
        if (index >= 2)
        {
            x = labelWidth + ((index - 2) % 3) * column;
            y = width >= 250 ? 168 + ((index - 2) / 3) * 37 : 184 + ((index - 2) / 3) * 54;
            w = column - 3;
        }
        place(fields_[index].window, x, y - scroll_, w, 24);
    }
    int y = (width >= 250 ? 329 : 378) - scroll_;
    for (auto& entry:behaviorFields_)
    {
        const int rowHeight=RowHeight(entry);
        const bool collapsed=IsBehaviorCollapsed(entry);
        if (entry.style == BehaviorField::Style::BehaviorHeader)
        {
            for (auto& toggle:behaviorToggles_)
                if (toggle.behavior == entry.behavior && toggle.window)
                {
                    SetWindowTextW(toggle.window,collapsedBehaviors_.contains(entry.behavior)?L"+":L"-");
                    place(toggle.window,width-28,y+5,20,std::max(18,rowHeight-10));
                    ShowWindow(toggle.window,SW_SHOW);
                    break;
                }
        }
        if (entry.arrayHeader && !entry.bits.empty()) {
            ShowWindow(entry.bits[0],collapsed?SW_HIDE:SW_SHOW);
            if (!collapsed) place(entry.bits[0],width-124,y+3,112,22);
        }
        else if (entry.arrayIndex>=0 && !entry.bits.empty()) {
            ShowWindow(entry.bits[0],collapsed?SW_HIDE:SW_SHOW);
            if (!collapsed) place(entry.bits[0],width-36,y+21,24,24);
        }
        else if (!entry.bits.empty()) {
            const int perRow=(CollisionBits+1)/2;
            const int span=std::max(perRow*18, width-24);
            const int cellW=std::max(16,(span-(perRow-1)*3)/perRow);
            for (int bit=0;bit<static_cast<int>(entry.bits.size());++bit) {
                ShowWindow(entry.bits[bit],collapsed?SW_HIDE:SW_SHOW);
                if (collapsed) continue;
                const int col=bit%perRow, rowIdx=bit/perRow;
                place(entry.bits[bit],12+col*(cellW+3),y+20+rowIdx*26,cellW,24);
            }
        }
        if (entry.field.window) {
            ShowWindow(entry.field.window,collapsed?SW_HIDE:SW_SHOW);
            if (collapsed) { y+=rowHeight; continue; }
            if(entry.axis>=0) {const int cell=std::max(30,(width-36)/entry.axisCount);place(entry.field.window,12+entry.axis*(cell+6),y+40,cell,24);}
            else if(entry.arrayIndex>=0) place(entry.field.window,12,y+21,std::max(30,width-52),24);
            // A COMBOBOX's height argument is the height of the DROPPED-DOWN control,
            // so it must be tall enough to show the list; the closed combo still
            // paints at one row and does not eat clicks below it.
            else if(entry.combo) place(entry.field.window,12,y+21,std::max(30,width-24),220);
            else place(entry.field.window,12,y+21,std::max(30,width-24),entry.multiline?84:24);
        }
        y+=rowHeight;
    }
    y+=5;
    for (HWND control : {meshEnabled_,chooseMesh_,cubeMesh_,clearMesh_}) ShowWindow(control,hasMesh ? SW_SHOW : SW_HIDE);
    if (hasMesh)
    {
        place(meshEnabled_,12,y,std::max(30,width-24),24);
        place(chooseMesh_,12,y+52,std::max(30,width-24),26);
        place(cubeMesh_,12,y+82,std::max(30,(width-30)/2),26);
        place(clearMesh_,15+(width-30)/2,y+82,std::max(30,(width-30)/2),26);
        y += 122;
    }
    place(addBehaviorButton_,12,y,std::max(30,width-24),28);
    if(positions)EndDeferWindowPos(positions);
    // Child edits are repositioned without drawing individually. Repaint the
    // complete clipped surface immediately so their old locations cannot
    // remain as afterimages while the scrollbar is moving.
    RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}
void InspectorPanel::Paint(HDC into)
{
    PAINTSTRUCT paint{}; const HDC dc = into ? into : BeginPaint(window_, &paint);
    RECT client{}; GetClientRect(window_, &client); FillRect(dc, &client, background_);
    SelectObject(dc, font_); SetTextColor(dc, Text); SetBkMode(dc, TRANSPARENT);
    const int width = static_cast<int>(client.right);
    const auto sectionPen=CreatePen(PS_SOLID,2,SectionBorder);const auto oldSectionPen=SelectObject(dc,sectionPen);const auto oldSectionBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc,4,109-scroll_,static_cast<int>(client.right)-4,(width>=250?293:342)-scroll_,9,9);
    SelectObject(dc,oldSectionBrush);SelectObject(dc,oldSectionPen);DeleteObject(sectionPen);
    const auto label = [&](const wchar_t* text, int x, int y, int width) {
        RECT r{x, y - scroll_, x + width, y + 20 - scroll_};
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    };
    label(L"Name", 12, 6, width - 24);
    label(L"Tags (comma-separated)", 12, 58, width - 24);
    label(L"Transform", 12, 117, width - 24);
    const int labelWidth = width >= 250 ? 68 : 12;
    const int column = std::max(24, (width - labelWidth - 16) / 3);
    for(int axis=0;axis<3;++axis){SetTextColor(dc,AxisColor(axis));label(axis==0?L"X":axis==1?L"Y":L"Z",labelWidth+column*axis,144,column);}SetTextColor(dc,Text);
    const wchar_t* names[]{L"Position", L"Rotation", L"Scale"};
    for (int row = 0; row < 3; ++row)
        label(names[row], 12, width >= 250 ? 170 + row * 37 : 164 + row * 54, width >= 250 ? 54 : width - 24);
    label(L"Rotation: degrees | Shift-drag: fine", 12, width >= 250 ? 275 : 324, width - 24);
    SelectObject(dc,behaviorFont_);
    label(L"Behaviors attached", 12, width >= 250 ? 305 : 354, width - 24);
    SelectObject(dc,font_);
    int rowY=width>=250?329:378;
    if(!behaviorFields_.empty()) {
        const auto sectionPen=CreatePen(PS_SOLID,2,SectionBorder);const auto oldSectionPen=SelectObject(dc,sectionPen);const auto oldSectionBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc,4,(width>=250?299:348)-scroll_,width-4,rowY+BehaviorHeight()+6-scroll_,9,9);
        SelectObject(dc,oldSectionBrush);SelectObject(dc,oldSectionPen);DeleteObject(sectionPen);
        const auto pen=CreatePen(PS_SOLID,1,BehaviorBorder);const auto oldPen=SelectObject(dc,pen);const auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        int top=rowY;auto* group=behaviorFields_.front().behavior;
        for(std::size_t i=0;i<behaviorFields_.size();++i) {
            const int bottom=rowY+RowHeight(behaviorFields_[i]);
            const bool end=i+1==behaviorFields_.size() || behaviorFields_[i+1].behavior!=group;
            if(end){RoundRect(dc,7,top-scroll_,width-7,bottom-scroll_,7,7);top=bottom;if(i+1<behaviorFields_.size())group=behaviorFields_[i+1].behavior;}
            rowY=bottom;
        }
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);rowY=width>=250?329:378;
    }
    for (const auto& entry:behaviorFields_)
    {
        if(IsBehaviorCollapsed(entry))continue;
        SelectObject(dc,entry.style==BehaviorField::Style::BehaviorHeader ? behaviorFont_ :
            entry.style==BehaviorField::Style::ScriptLabel ? labelFont_ : font_);
        const int left=12;
        RECT row{left,rowY-scroll_,entry.style==BehaviorField::Style::BehaviorHeader?width-34:width-12,rowY-scroll_+(entry.style==BehaviorField::Style::BehaviorHeader?RowHeight(entry):20)};
        if(entry.axis<=0)DrawTextW(dc,entry.label.c_str(),-1,&row,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if(entry.axis>=0) {
            const int cell=std::max(30,(width-36)/entry.axisCount);
            SetTextColor(dc,AxisColor(entry.axis));label(entry.axis==0?L"X":entry.axis==1?L"Y":L"Z",12+entry.axis*(cell+6),rowY+20,cell);SetTextColor(dc,Text);
        }
        rowY+=RowHeight(entry);
    }
    SelectObject(dc,font_);
    const auto* mesh = object_ ? object_->GetBehavior<zengine::MeshRenderer>() : nullptr;
    const int base = (width >= 250 ? 334 : 383) + BehaviorHeight();
    if (mesh)
    {
        const auto asset = mesh->Asset().empty() ? L"Model: None" : mesh->Asset() == zengine::MeshRenderer::CubeAsset ? L"Model: Built-in Cube" : L"Model: " + Wide(mesh->Asset());
        label(asset.c_str(),12,base+27,width-24);
    }
    label(scriptHost_ && scriptHost_->Playing() ? L"Play: variable edits are temporary" : L"Scripts run in Play mode", 12, base + (mesh ? 122 : 0) + 66, width-24);
    if (!into) EndPaint(window_, &paint);
}
LRESULT CALLBACK InspectorPanel::WindowProcedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto* panel = reinterpret_cast<InspectorPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        panel = static_cast<InspectorPanel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    try { return panel ? panel->HandleMessage(message, w, l) : DefWindowProcW(window, message, w, l); }
    catch (...) { return 0; } // No C++ exception can escape a native callback.
}
LRESULT InspectorPanel::HandleMessage(UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case WM_SIZE: Layout(); return 0;
    case WM_PAINT: Paint(); return 0;
    case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(w)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: ShowBehaviorMenu({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); return 0;
    case WM_COMMAND:
    {
        if (!editData_ && ((LOWORD(w)>=AddScriptButton && LOWORD(w)<=AddLightCommand) || (LOWORD(w)>=AddScriptSubFirst && LOWORD(w)<AddScriptSubFirst+400))) return 0;
        const int toggleIndex=LOWORD(w)-FirstBehaviorToggle;
        if (toggleIndex>=0 && toggleIndex<static_cast<int>(behaviorToggles_.size()) && HIWORD(w)==BN_CLICKED)
        {
            auto* behavior=behaviorToggles_[toggleIndex].behavior;
            if (!behavior) return 0;
            if (!collapsedBehaviors_.erase(behavior)) collapsedBehaviors_.insert(behavior);
            Layout();
            InvalidateRect(window_,nullptr,FALSE);
            return 0;
        }
        if (const int bitId=LOWORD(w)-FirstBehaviorBit; bitId>=0 && bitId<bitButtonCount_ && HIWORD(w)==BN_CLICKED && !updating_)
        {
            const auto target=reinterpret_cast<HWND>(l);
            for (std::size_t f=0;f<behaviorFields_.size();++f)
                for (int bit=0;bit<static_cast<int>(behaviorFields_[f].bits.size());++bit)
                    if (behaviorFields_[f].bits[bit]==target) {
                        if (behaviorFields_[f].arrayHeader) ShowAddArrayElementMenu(f);
                        else if (behaviorFields_[f].arrayIndex>=0) RemoveArrayElementAt(f);
                        else ToggleCollisionBit(f,bit);
                        return 0;
                    }
            return 0;
        }
        if(LOWORD(w)==RemoveBehaviorCommand)
        {
            auto* behavior=contextBehavior_;contextBehavior_=nullptr;
            if(!behavior || !editData_ || !object_ || (scriptHost_&&scriptHost_->Playing()))return 0;
            bool attached=false;for(std::size_t i=0;i<object_->BehaviorCount();++i)if(&object_->BehaviorAt(i)==behavior){attached=true;break;}
            if(!attached)return 0;
            if(auto* script=dynamic_cast<zengine::ScriptBehavior*>(behavior);script&&scriptHost_)scriptHost_->Forget(*script);
            collapsedBehaviors_.erase(behavior);
            if(object_->RemoveBehavior(*behavior)){RefreshBehaviors();if(changed_)changed_();}
            return 0;
        }
        const int dynamicIndex=LOWORD(w)-FirstBehaviorField;
        if (dynamicIndex>=0 && dynamicIndex<static_cast<int>(behaviorFields_.size()))
        {
            auto& entry=behaviorFields_[dynamicIndex];
            if(!updating_ && HIWORD(w)==BN_CLICKED && entry.prefab && choosePrefab_)
            {
                auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);
                if(const auto asset=choosePrefab_(Utf8(BehaviorValue(dynamicIndex)));asset && script){
                    try {
                        if(entry.arrayIndex>=0) scriptHost_->SetArrayElement(*script,entry.name,static_cast<std::size_t>(entry.arrayIndex),*asset);
                        else scriptHost_->SetField(*script,entry.name,*asset);
                        SetWindowTextW(entry.field.window,Wide(*asset).c_str());entry.field.valid=true;if(changed_)changed_();
                    } catch(const std::exception&) { entry.field.valid=false; InvalidateRect(entry.field.window,nullptr,FALSE); }
                }
                return 0;
            }
            if(!updating_ && HIWORD(w)==BN_CLICKED && entry.objectReference && pickObject_ && scriptHost_ && editData_)
            {
                auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);
                if(script)
                {
                    zengine::GameObjectId current=0;
                    if(entry.arrayIndex<0) { if(const auto refs=scriptHost_->AuthoredReferences(*script); refs.count(entry.name)) current=refs.at(entry.name); }
                    RECT anchor{}; GetWindowRect(entry.field.window,&anchor);
                    if(const auto picked=pickObject_(entry.type,current,anchor))
                    {
                        try {
                            if(entry.arrayIndex>=0) scriptHost_->SetArrayElementReference(*script,entry.name,static_cast<std::size_t>(entry.arrayIndex),*picked);
                            else scriptHost_->SetObjectReference(*script,entry.name,*picked);
                            entry.field.valid=true; RefreshBehaviors(); if(changed_)changed_();
                        }
                        catch(const std::exception&) { entry.field.valid=false; InvalidateRect(entry.field.window,nullptr,FALSE); }
                    }
                }
                return 0;
            }
            if (!updating_ && HIWORD(w)==EN_CHANGE) ChangeBehaviorField(dynamicIndex);
            if (!updating_ && HIWORD(w)==CBN_SELCHANGE) ChangeBehaviorField(dynamicIndex);
            if (!updating_ && HIWORD(w)==EN_SETFOCUS) behaviorFields_[dynamicIndex].field.focusText=BehaviorValue(dynamicIndex);
            if (!updating_ && HIWORD(w)==EN_KILLFOCUS) FinishBehaviorField(dynamicIndex,false);
            return 0;
        }
        if (LOWORD(w) == AddBehaviorButton && HIWORD(w) == BN_CLICKED && object_)
        {
            const auto menu = CreatePopupMenu();
            const bool is2D = object_->Is2D(); // 3D-only behaviors are disabled for UI / 2D objects
            const UINT only3D = is2D ? MF_GRAYED : 0;
            AppendMenuW(menu,MF_STRING | only3D | (object_->GetBehavior<zengine::MeshRenderer>() ? MF_GRAYED : 0),AddMeshCommand,L"Mesh Renderer");
            // "Add Script" opens a submenu of every .zsh in the project; the last item browses.
            const auto scripts = CreatePopupMenu();
            std::size_t listed = 0;
            if (scriptList_) for (const auto& path : scriptList_())
            {
                if (listed >= 400) break;
                const auto stem = std::filesystem::path(path).stem().wstring();
                AppendMenuW(scripts,MF_STRING,static_cast<UINT_PTR>(AddScriptSubFirst + listed++),(stem.empty()?path:stem).c_str());
            }
            if (listed) AppendMenuW(scripts,MF_SEPARATOR,0,nullptr);
            AppendMenuW(scripts,MF_STRING,AddScriptCommand,L"Browse…");
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(scripts),L"Add Script");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|only3D|(object_->GetBehavior<zengine::physics::Collider>()?MF_GRAYED:0),AddColliderCommand,L"Collider");
            const bool hasBody=object_->GetBehavior<zengine::physics::RigidBody>()||object_->GetBehavior<zengine::physics::KinematicBody>()||object_->GetBehavior<zengine::physics::StaticBody>()||object_->GetBehavior<zengine::physics::Area>();const UINT bodyFlags=MF_STRING|only3D|(hasBody?MF_GRAYED:0);
            AppendMenuW(menu,bodyFlags,AddRigidBodyCommand,L"Rigid Body");AppendMenuW(menu,bodyFlags,AddKinematicBodyCommand,L"Kinematic Body");AppendMenuW(menu,bodyFlags,AddStaticBodyCommand,L"Static Body");AppendMenuW(menu,bodyFlags,AddAreaCommand,L"Area");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|only3D|(object_->GetBehavior<zengine::Camera>()?MF_GRAYED:0),AddCameraCommand,L"Camera");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING|(object_->GetBehavior<zengine::audio::AudioSource>()?MF_GRAYED:0),AddAudioSourceCommand,L"Audio Player");
            AppendMenuW(menu,MF_STRING|only3D|((!object_->GetBehavior<zengine::physics::Area>()||object_->GetBehavior<zengine::audio::AudioEffect>())?MF_GRAYED:0),AddAudioEffectCommand,L"Audio Effect (needs an Area)");
            AppendMenuW(menu,MF_STRING|only3D|(object_->GetBehavior<zengine::Light>()?MF_GRAYED:0),AddLightCommand,L"Light");
            RECT button{}; GetWindowRect(addBehaviorButton_,&button);
            const auto command = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,button.left,button.bottom,0,window_,nullptr);
            DestroyMenu(menu);
            if (command) SendMessageW(window_,WM_COMMAND,command,0);
            return 0;
        }
        if (LOWORD(w) == AddScriptCommand) { if (object_ && addScript_) addScript_(); return 0; }
        if (const int scriptItem=LOWORD(w)-AddScriptSubFirst; scriptItem>=0 && scriptItem<400)
        {
            if (object_ && attachScript_ && scriptList_)
            { const auto list=scriptList_(); if (scriptItem<static_cast<int>(list.size())) attachScript_(list[scriptItem]); }
            return 0;
        }
        if(LOWORD(w)>=AddColliderCommand&&LOWORD(w)<=AddAreaCommand&&object_&&!object_->Is2D()){if(LOWORD(w)==AddColliderCommand)object_->AddBehavior<zengine::physics::Collider>();else if(LOWORD(w)==AddRigidBodyCommand)object_->AddBehavior<zengine::physics::RigidBody>();else if(LOWORD(w)==AddKinematicBodyCommand)object_->AddBehavior<zengine::physics::KinematicBody>();else if(LOWORD(w)==AddStaticBodyCommand)object_->AddBehavior<zengine::physics::StaticBody>();else object_->AddBehavior<zengine::physics::Area>();RefreshBehaviors();if(changed_)changed_();return 0;}
        if(LOWORD(w)==AddCameraCommand&&object_&&!object_->Is2D()&&!object_->GetBehavior<zengine::Camera>()){object_->AddBehavior<zengine::Camera>();RefreshBehaviors();if(changed_)changed_();return 0;}
        if(LOWORD(w)==AddAudioSourceCommand&&object_&&!object_->GetBehavior<zengine::audio::AudioSource>()){object_->AddBehavior<zengine::audio::AudioSource>();RefreshBehaviors();if(changed_)changed_();return 0;}
        if(LOWORD(w)==AddAudioEffectCommand&&object_&&!object_->Is2D()&&object_->GetBehavior<zengine::physics::Area>()&&!object_->GetBehavior<zengine::audio::AudioEffect>()){object_->AddBehavior<zengine::audio::AudioEffect>();RefreshBehaviors();if(changed_)changed_();return 0;}
        if(LOWORD(w)==AddLightCommand&&object_&&!object_->Is2D()&&!object_->GetBehavior<zengine::Light>()){object_->AddBehavior<zengine::Light>();RefreshBehaviors();if(changed_)changed_();return 0;}
        if (LOWORD(w) == AddMeshCommand || LOWORD(w) == ChooseMeshButton || LOWORD(w) == CubeMeshButton || LOWORD(w) == ClearMeshButton)
        {
            if (object_ && meshAction_) meshAction_(LOWORD(w) == AddMeshCommand ? MeshAction::Add : LOWORD(w) == ChooseMeshButton ? MeshAction::Choose : LOWORD(w) == CubeMeshButton ? MeshAction::Cube : MeshAction::Clear);
            return 0;
        }
        if (LOWORD(w) == MeshEnabled && HIWORD(w) == BN_CLICKED && object_)
        {
            if (auto* mesh = object_->GetBehavior<zengine::MeshRenderer>())
                mesh->SetEnabled(SendMessageW(meshEnabled_,BM_GETCHECK,0,0) == BST_CHECKED);
            if (changed_) changed_();
            return 0;
        }
        const int index = LOWORD(w) - NameField;
        if (index < 0 || index >= static_cast<int>(fields_.size())) break;
        if (HIWORD(w) == EN_CHANGE) ChangeField(index);
        if (HIWORD(w) == EN_SETFOCUS) fields_[index].focusText = FieldValue(index);
        if (HIWORD(w) == EN_KILLFOCUS && !updating_) FinishField(index, false);
        return 0;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC:
    {
        const int index = GetDlgCtrlID(reinterpret_cast<HWND>(l)) - NameField;
        const int dynamicIndex=GetDlgCtrlID(reinterpret_cast<HWND>(l))-FirstBehaviorField;
        const bool valid = dynamicIndex>=0 && dynamicIndex<static_cast<int>(behaviorFields_.size()) ? behaviorFields_[dynamicIndex].field.valid :
            index < 0 || index >= static_cast<int>(fields_.size()) || fields_[index].valid;
        int axis=-1;if(index>=2 && index<static_cast<int>(fields_.size()))axis=(index-2)%3;else if(dynamicIndex>=0&&dynamicIndex<static_cast<int>(behaviorFields_.size()))axis=behaviorFields_[dynamicIndex].axis;
        SetTextColor(reinterpret_cast<HDC>(w), axis>=0?AxisColor(axis):Text);
        SetBkColor(reinterpret_cast<HDC>(w), valid ? RGB(52, 54, 60) : RGB(92, 45, 45));
        return reinterpret_cast<LRESULT>(valid ? fieldBrush_ : invalidBrush_);
    }
    case WM_MOUSEWHEEL:
    {
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(w);
        const int steps=wheelRemainder_/WHEEL_DELTA;
        wheelRemainder_-=steps*WHEEL_DELTA;
        if(steps){scroll_-=steps*32;Layout();}
        return 0;
    }
    case WM_VSCROLL:
    {
        const int action = LOWORD(w);
        if (action == SB_LINEUP) scroll_ -= 20;
        if (action == SB_LINEDOWN) scroll_ += 20;
        if (action == SB_PAGEUP) scroll_ -= 120;
        if (action == SB_PAGEDOWN) scroll_ += 120;
        if (action == SB_THUMBTRACK) { SCROLLINFO info{sizeof(info), SIF_TRACKPOS}; GetScrollInfo(window_, SB_VERT, &info); scroll_ = info.nTrackPos; }
        Layout(); return 0;
    }
    }
    return DefWindowProcW(window_, message, w, l);
}

bool InspectorPanel::AssignPrefabAt(POINT point,const std::string& asset)
{
    auto target=WindowFromPoint(point);
    for(std::size_t i=0;i<behaviorFields_.size();++i)
    {
        auto& entry=behaviorFields_[i];
        if(!entry.prefab || !entry.field.window || (target!=entry.field.window && !IsChild(entry.field.window,target)))continue;
        if(!editData_ || !IsWindowEnabled(entry.field.window))return false;
        auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);if(!script||!scriptHost_)return false;
        scriptHost_->SetField(*script,entry.name,asset);SetWindowTextW(entry.field.window,Wide(asset).c_str());entry.field.valid=true;if(changed_)changed_();return true;
    }
    return false;
}
bool InspectorPanel::AssignAssetPathAt(POINT point,const std::string& asset)
{
    const auto target=WindowFromPoint(point);
    POINT client=point; ScreenToClient(window_,&client);
    RECT bounds{}; GetClientRect(window_,&bounds);
    const bool inPanel=PtInRect(&bounds,client);
    int rowTop=(bounds.right>=250?329:378)-scroll_; int hoveredRow=-1;
    for(std::size_t r=0;r<behaviorFields_.size();++r)
    { const int h=RowHeight(behaviorFields_[r]); if(inPanel&&h>0&&client.y>=rowTop&&client.y<rowTop+h) hoveredRow=static_cast<int>(r); rowTop+=h; }

    for(std::size_t i=0;i<behaviorFields_.size();++i)
    {
        auto& entry=behaviorFields_[i];
        if(!entry.uiControl || entry.uiKind!=zengine::ui::UiPropertyKind::Texture || !entry.field.window)continue;
        const bool overField=target==entry.field.window||IsChild(entry.field.window,target)||static_cast<int>(i)==hoveredRow;
        if(!overField)continue;
        if(!editData_ || !IsWindowEnabled(entry.field.window))return false;
        auto* control=dynamic_cast<zengine::ui::UiControl*>(entry.behavior);
        if(!control)return false;
        try { zengine::ui::LoadUiProperty(*control,entry.name,asset); }
        catch(const std::exception&){ return false; }
        SetWindowTextW(entry.field.window,Wide(asset).c_str());
        entry.field.valid=true;
        RefreshBehaviors();
        if(changed_)changed_();
        return true;
    }
    return false;
}
void InspectorPanel::ShowBehaviorMenu(POINT screenPoint)
{
    POINT point=screenPoint;
    if(point.x==static_cast<LONG>(-1) && point.y==static_cast<LONG>(-1))GetCursorPos(&point);
    POINT client=point;ScreenToClient(window_,&client);
    RECT bounds{};GetClientRect(window_,&bounds);const int width=static_cast<int>(bounds.right);
    int y=(width>=250?329:378)-scroll_;zengine::Behavior* target=nullptr;
    for(const auto& entry:behaviorFields_)
    {
        const int height=RowHeight(entry);
        if(height>0 && client.y>=y && client.y<y+height){target=entry.behavior;break;}
        y+=height;
    }
    if(!target)return;
    contextBehavior_=target;
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING|((!editData_||!object_|| (scriptHost_&&scriptHost_->Playing()))?MF_GRAYED:0),RemoveBehaviorCommand,L"Remove Behavior");
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window_,nullptr);DestroyMenu(menu);
    if(command==RemoveBehaviorCommand && editData_ && object_ && !(scriptHost_&&scriptHost_->Playing()))
    {
        // Defer destruction until the child control's WM_CONTEXTMENU callback
        // has unwound. Destroying that control synchronously crashes comctl32.
        if(!PostMessageW(window_,WM_COMMAND,MAKEWPARAM(RemoveBehaviorCommand,0),0))contextBehavior_=nullptr;
    }
    else contextBehavior_=nullptr;
}
bool InspectorPanel::AssignObjectReferenceAt(POINT point, zengine::GameObjectId target)
{
    const auto control=WindowFromPoint(point);
    // Geometric fallback: which behavior row is the drop point over? (WindowFromPoint
    // skips child windows whose top-level ancestor is hidden, e.g. in tests.)
    POINT client=point; ScreenToClient(window_,&client);
    RECT bounds{}; GetClientRect(window_,&bounds);
    const bool inPanel = PtInRect(&bounds,client);
    int rowTop=(bounds.right>=250?329:378)-scroll_; int hoveredRow=-1;
    for (std::size_t r=0;r<behaviorFields_.size();++r)
    { const int h=RowHeight(behaviorFields_[r]); if (inPanel && h>0 && client.y>=rowTop && client.y<rowTop+h) { hoveredRow=static_cast<int>(r); } rowTop+=h; }

    for (std::size_t i=0;i<behaviorFields_.size();++i)
    {
        auto& entry=behaviorFields_[i];
        auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior);
        const bool overRow = static_cast<int>(i)==hoveredRow;

        // Drop onto an array's "+ Add element" row appends a new object-reference slot.
        if (entry.arrayHeader && (overRow || (!entry.bits.empty() && (control==entry.bits[0] || IsChild(entry.bits[0],control)))))
        {
            if (!editData_ || !script || !scriptHost_ || scriptHost_->Playing()) return false;
            try { scriptHost_->SetArrayElementReference(*script,entry.name,static_cast<std::size_t>(-1),target); } // -1 clamps to append
            catch (const std::exception&) { return false; }
            RefreshBehaviors(); if (changed_) changed_(); return true;
        }

        const bool overField = entry.field.window && (control==entry.field.window || IsChild(entry.field.window,control) || overRow);
        if (!entry.objectReference || !overField) continue;
        if (!editData_ || !IsWindowEnabled(entry.field.window)) return false;
        if (!script || !scriptHost_) return false;

        if (entry.arrayIndex>=0)
        {
            try { scriptHost_->SetArrayElementReference(*script,entry.name,static_cast<std::size_t>(entry.arrayIndex),target); }
            catch (const std::exception&) { return false; }
            RefreshBehaviors(); if (changed_) changed_(); return true;
        }

        scriptHost_->SetObjectReference(*script,entry.name,target);
        const auto fields=scriptHost_->Fields(*script);
        for (const auto& field:fields) if (field.name==entry.name) { updating_=true;SetWindowTextW(entry.field.window,Wide(field.value).c_str());updating_=false;entry.field.focusText=Wide(field.value);break; }
        entry.field.valid=true;
        if (changed_) changed_();
        InvalidateRect(window_,nullptr,FALSE);
        return true;
    }
    return false;
}
LRESULT CALLBACK InspectorPanel::EditProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
{
    try { return reinterpret_cast<InspectorPanel*>(data)->HandleEdit(window, message, w, l); }
    catch (...) { return DefSubclassProc(window, message, w, l); }
}
LRESULT InspectorPanel::HandleEdit(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if(message==WM_CONTEXTMENU){SendMessageW(this->window_,message,w,l);return 0;}
    const int dynamicIndex=GetDlgCtrlID(window)-FirstBehaviorField;
    if (dynamicIndex>=0 && dynamicIndex<static_cast<int>(behaviorFields_.size()))
    {
        if(message==WM_CONTEXTMENU){SendMessageW(this->window_,message,w,l);return 0;}
        if(message==WM_MOUSEWHEEL){SendMessageW(this->window_,message,w,l);return 0;}
        // Let the character message insert the newline exactly once. Forwarding
        // both keydown and char makes RichEdit create two lines for one Enter.
        if(behaviorFields_[dynamicIndex].multiline && message==WM_KEYDOWN && w==VK_RETURN)return 0;
        if(behaviorFields_[dynamicIndex].multiline && message==WM_CHAR && w==VK_RETURN)return DefSubclassProc(window,message,w,l);
        if (message==WM_GETDLGCODE) return DLGC_WANTALLKEYS;
        if (message==WM_KEYDOWN && (w==VK_RETURN || w==VK_ESCAPE || w==VK_TAB))
        {
            FinishBehaviorField(dynamicIndex,w==VK_ESCAPE);
            HWND next=window_;
            if (w==VK_TAB)
            {
                const int step=(GetKeyState(VK_SHIFT)&0x8000)?-1:1;
                for (int i=dynamicIndex+step;i>=0 && i<static_cast<int>(behaviorFields_.size());i+=step)
                    if (behaviorFields_[i].field.window) { next=behaviorFields_[i].field.window; break; }
            }
            SetFocus(next); return 0;
        }
        if (message==WM_CHAR && (w==VK_RETURN || w==VK_ESCAPE || w==VK_TAB)) return 0;
        return DefSubclassProc(window,message,w,l);
    }
    const int index = GetDlgCtrlID(window) - NameField;
    if(index<0 || index>=static_cast<int>(fields_.size()))return DefSubclassProc(window,message,w,l);
    if(message==WM_MOUSEWHEEL){SendMessageW(this->window_,message,w,l);return 0;}
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB))
    {
        EndScrub(w == VK_ESCAPE);
        FinishField(index, w == VK_ESCAPE);
        if (w == VK_TAB)
        {
            const int next = (index + ((GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1)) % 11;
            SetFocus(fields_[next].window); SendMessageW(fields_[next].window, EM_SETSEL, 0, -1);
        }
        else SetFocus(window_);
        return 0;
    }
    if (message == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
    if (index >= 2 && object_ && editTransform_)
    {
        if (message == WM_LBUTTONDOWN)
        {
            fields_[index].focusText = FieldValue(index);
            SetFocus(window); pressed_ = index; scrubbing_ = false;
            startPoint_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(window, &startPoint_);
            startValue_ = Value(index);
            scrubStep_ = index >= 5 && index <= 7 ? 0.5f : 0.01f;
            if (GetKeyState(VK_SHIFT) & 0x8000) scrubStep_ *= 0.1f;
            SetCapture(window); return 0;
        }
        if (message == WM_MOUSEMOVE && pressed_ == index)
        {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(window, &point);
            const LONG delta = point.x - startPoint_.x;
            scrubbing_ = scrubbing_ || std::abs(delta) >= GetSystemMetrics(SM_CXDRAG);
            if (scrubbing_)
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                SetValue(index, std::clamp(startValue_ + static_cast<float>(delta) * scrubStep_, -1'000'000.0f, 1'000'000.0f));
                SetText(index, FieldValue(index));
            }
            return 0;
        }
        if (message == WM_LBUTTONUP && pressed_ == index)
        {
            const bool dragged = scrubbing_;
            EndScrub(false);
            if (dragged) fields_[index].focusText = FieldValue(index);
            else SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (message == WM_CAPTURECHANGED && pressed_ == index) { EndScrub(true); return 0; }
    }
    return DefSubclassProc(window, message, w, l);
}
