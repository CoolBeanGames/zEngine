#include "InspectorPanel.h"
#include "core/ScriptBehavior.h"
#include "core/MeshRenderer.h"
#include <windowsx.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr wchar_t ClassName[] = L"zEngineInspector";
    constexpr COLORREF Background = RGB(40, 42, 47), Text = RGB(214, 216, 221);
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
    if (background_) DeleteObject(background_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    if (invalidBrush_) DeleteObject(invalidBrush_);
    if (instance_) UnregisterClassW(ClassName, instance_);
}

void InspectorPanel::Create(HWND parent, HINSTANCE instance, HFONT font, std::function<void()> changed)
{
    instance_ = instance; font_ = font; changed_ = std::move(changed);
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
    addScriptButton_ = CreateWindowExW(0, L"BUTTON", L"+ Add Script", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(AddScriptButton)), instance, nullptr);
    if (!addScriptButton_) throw std::runtime_error("Cannot create Add Script button.");
    SendMessageW(addScriptButton_, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
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
    Bind(nullptr);
    Layout();
}

void InspectorPanel::Bind(zengine::GameObject* object)
{
    EndScrub(true);
    for (int index = 0; index < static_cast<int>(fields_.size()); ++index)
        if (GetFocus() == fields_[index].window) { FinishField(index, false); SetFocus(window_); }
    for (std::size_t index=0;index<behaviorFields_.size();++index)
        if (behaviorFields_[index].field.window && GetFocus()==behaviorFields_[index].field.window)
        { FinishBehaviorField(index,false); SetFocus(window_); }
    object_ = object;
    RefreshFields();
    RefreshBehaviors();
}
void InspectorPanel::RefreshBehaviors()
{
    // Rebuild only at structural changes/save/session boundaries, never on each tick.
    updating_=true;
    for (auto& entry:behaviorFields_) if (entry.field.window) DestroyWindow(entry.field.window);
    behaviorFields_.clear();
    const auto add = [&](zengine::Behavior* behavior, std::string name, std::wstring label, bool priority, bool field, bool editable) {
        BehaviorField entry; entry.behavior=behavior; entry.name=std::move(name); entry.label=std::move(label); entry.priority=priority;
        if (field)
        {
            const auto id=FirstBehaviorField+behaviorFields_.size();
            entry.field.window=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|(editable?0:ES_READONLY),
                0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
            if (!entry.field.window) throw std::runtime_error("Cannot create script field.");
            SendMessageW(entry.field.window,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            SendMessageW(entry.field.window,EM_SETLIMITTEXT,4096,0);
            SetWindowSubclass(entry.field.window,EditProcedure,1,reinterpret_cast<DWORD_PTR>(this));
        }
        behaviorFields_.push_back(std::move(entry));
    };
    if (object_) for (std::size_t i=0;i<object_->BehaviorCount();++i)
    {
        auto& behavior=object_->BehaviorAt(i);
        auto* script=dynamic_cast<zengine::ScriptBehavior*>(&behavior);
        add(&behavior,{},script?Wide(script->Asset()):L"Mesh Renderer",false,false,false);
        add(&behavior,{},L"Priority (higher runs first)",true,true,true);
        if (script && scriptHost_)
        {
            const auto error=scriptHost_->Error(*script);
            if (!error.empty()) add(&behavior,{},L"Error: "+Wide(error),false,false,false);
            for (const auto& field:scriptHost_->Fields(*script))
                add(&behavior,field.name,field.name.empty()?Wide(field.label):Wide(field.name+" ("+field.type+")"),false,!field.name.empty(),field.editable);
        }
    }
    for (std::size_t i=0;i<behaviorFields_.size();++i) if (auto control=behaviorFields_[i].field.window)
    {
        const auto value=BehaviorValue(i);
        SetWindowTextW(control,value.c_str()); behaviorFields_[i].field.focusText=value;
    }
    updating_=false;
    EnableWindow(addScriptButton_, object_ != nullptr);
    EnableWindow(addBehaviorButton_, object_ != nullptr);
    const auto* mesh = object_ ? object_->GetBehavior<zengine::MeshRenderer>() : nullptr;
    SendMessageW(meshEnabled_, BM_SETCHECK, mesh && mesh->Enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    Layout();
}

int InspectorPanel::BehaviorHeight() const
{
    int height=0;
    for (const auto& entry:behaviorFields_) height+=entry.field.window?52:24;
    return height;
}
std::wstring InspectorPanel::BehaviorValue(std::size_t index)
{
    const auto& entry=behaviorFields_.at(index);
    if (entry.priority) { std::wostringstream out; out<<std::setprecision(9)<<entry.behavior->Priority(); return out.str(); }
    if (auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior); script && scriptHost_)
        for (const auto& field:scriptHost_->Fields(*script)) if (field.name==entry.name) return Wide(field.value);
    return {};
}
void InspectorPanel::ChangeBehaviorField(std::size_t index)
{
    if (updating_) return;
    auto& entry=behaviorFields_.at(index);
    try
    {
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
        else if (auto* script=dynamic_cast<zengine::ScriptBehavior*>(entry.behavior); script && scriptHost_)
            scriptHost_->SetField(*script,entry.name,Utf8(text));
        entry.field.valid=true;
    }
    catch (const std::exception&) { entry.field.valid=false; }
    InvalidateRect(entry.field.window,nullptr,FALSE);
}
void InspectorPanel::FinishBehaviorField(std::size_t index, bool cancel)
{
    auto& entry=behaviorFields_.at(index);
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
        if (const auto control=behaviorFields_[i].field.window; control && GetFocus()!=control)
        { SetWindowTextW(control,BehaviorValue(i).c_str()); behaviorFields_[i].field.valid=true; }
    updating_=false;
}

float InspectorPanel::Value(int index) const
{
    if (!object_) return 0;
    const auto& transform = object_->GetTransform();
    const int component = (index - 2) / 3;
    const auto& v = component == 0 ? transform.Position() : component == 1 ? transform.Rotation() : transform.Scale();
    const int axis = (index - 2) % 3;
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}
void InspectorPanel::SetValue(int index, float value)
{
    if (!object_) return;
    auto& transform = object_->GetTransform();
    const int component = (index - 2) / 3, axis = (index - 2) % 3;
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
        EnableWindow(fields_[index].window, object_ != nullptr);
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void InspectorPanel::ChangeField(int index)
{
    if (updating_ || !object_) return;
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
    const int contentHeight = (narrow ? 480 : 430) + behaviorHeight + (hasMesh ? 122 : 0);
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMin = 0; scroll.nMax = contentHeight - 1; scroll.nPage = static_cast<UINT>(client.bottom);
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight - static_cast<int>(client.bottom)));
    scroll.nPos = scroll_; SetScrollInfo(window_, SB_VERT, &scroll, TRUE);
    GetClientRect(window_, &client);
    const int width = static_cast<int>(client.right);
    const int labelWidth = width >= 250 ? 68 : 12;
    const int column = std::max(24, (width - labelWidth - 16) / 3);
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
        MoveWindow(fields_[index].window, x, y - scroll_, w, 24, TRUE);
    }
    int y = (width >= 250 ? 321 : 370) - scroll_;
    for (auto& entry:behaviorFields_)
    {
        if (entry.field.window) MoveWindow(entry.field.window,12,y+21,std::max(30,width-24),24,TRUE);
        y+=entry.field.window?52:24;
    }
    y+=5;
    for (HWND control : {meshEnabled_,chooseMesh_,cubeMesh_,clearMesh_}) ShowWindow(control,hasMesh ? SW_SHOW : SW_HIDE);
    if (hasMesh)
    {
        MoveWindow(meshEnabled_,12,y,std::max(30,width-24),24,TRUE);
        MoveWindow(chooseMesh_,12,y+52,std::max(30,width-24),26,TRUE);
        MoveWindow(cubeMesh_,12,y+82,std::max(30,(width-30)/2),26,TRUE);
        MoveWindow(clearMesh_,15+(width-30)/2,y+82,std::max(30,(width-30)/2),26,TRUE);
        y += 122;
    }
    MoveWindow(addBehaviorButton_,12,y,std::max(30,width-24),28,TRUE);
    MoveWindow(addScriptButton_,12,y+34,std::max(30,width-24),28,TRUE);
    InvalidateRect(window_, nullptr, FALSE);
}
void InspectorPanel::Paint()
{
    PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window_, &paint);
    RECT client{}; GetClientRect(window_, &client); FillRect(dc, &client, background_);
    SelectObject(dc, font_); SetTextColor(dc, Text); SetBkMode(dc, TRANSPARENT);
    const auto label = [&](const wchar_t* text, int x, int y, int width) {
        RECT r{x, y - scroll_, x + width, y + 20 - scroll_};
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    };
    const int width = static_cast<int>(client.right);
    label(L"Name", 12, 6, width - 24);
    label(L"Tags (comma-separated)", 12, 58, width - 24);
    label(L"Transform", 12, 117, width - 24);
    const int labelWidth = width >= 250 ? 68 : 12;
    const int column = std::max(24, (width - labelWidth - 16) / 3);
    label(L"X", labelWidth, 144, column); label(L"Y", labelWidth + column, 144, column);
    label(L"Z", labelWidth + column * 2, 144, column);
    const wchar_t* names[]{L"Position", L"Rotation", L"Scale"};
    for (int row = 0; row < 3; ++row)
        label(names[row], 12, width >= 250 ? 170 + row * 37 : 164 + row * 54, width >= 250 ? 54 : width - 24);
    label(L"Rotation: degrees | Shift-drag: fine", 12, width >= 250 ? 275 : 324, width - 24);
    label(object_ && object_->BehaviorCount() ? L"Behaviors attached" : L"No behaviors attached", 12, width >= 250 ? 297 : 346, width - 24);
    int rowY=width>=250?321:370;
    for (const auto& entry:behaviorFields_)
    { label(entry.label.c_str(),12,rowY,width-24); rowY+=entry.field.window?52:24; }
    const auto* mesh = object_ ? object_->GetBehavior<zengine::MeshRenderer>() : nullptr;
    const int base = (width >= 250 ? 326 : 375) + BehaviorHeight();
    if (mesh)
    {
        const auto asset = mesh->Asset().empty() ? L"Model: None" : mesh->Asset() == zengine::MeshRenderer::CubeAsset ? L"Model: Built-in Cube" : L"Model: " + Wide(mesh->Asset());
        label(asset.c_str(),12,base+27,width-24);
    }
    label(scriptHost_ && scriptHost_->Playing() ? L"Play: variable edits are temporary" : L"Scripts run in Play mode", 12, base + (mesh ? 122 : 0) + 66, width-24);
    EndPaint(window_, &paint);
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
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
    {
        const int dynamicIndex=LOWORD(w)-FirstBehaviorField;
        if (dynamicIndex>=0 && dynamicIndex<static_cast<int>(behaviorFields_.size()))
        {
            if (!updating_ && HIWORD(w)==EN_CHANGE) ChangeBehaviorField(dynamicIndex);
            if (!updating_ && HIWORD(w)==EN_SETFOCUS) behaviorFields_[dynamicIndex].field.focusText=BehaviorValue(dynamicIndex);
            if (!updating_ && HIWORD(w)==EN_KILLFOCUS) FinishBehaviorField(dynamicIndex,false);
            return 0;
        }
        if (LOWORD(w) == AddBehaviorButton && HIWORD(w) == BN_CLICKED && object_)
        {
            const auto menu = CreatePopupMenu();
            AppendMenuW(menu,MF_STRING | (object_->GetBehavior<zengine::MeshRenderer>() ? MF_GRAYED : 0),AddMeshCommand,L"Mesh Renderer");
            AppendMenuW(menu,MF_STRING,AddScriptCommand,L"Script...");
            RECT button{}; GetWindowRect(addBehaviorButton_,&button);
            const auto command = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,button.left,button.bottom,0,window_,nullptr);
            DestroyMenu(menu);
            if (command) SendMessageW(window_,WM_COMMAND,command,0);
            return 0;
        }
        if (LOWORD(w) == AddScriptCommand) { if (object_ && addScript_) addScript_(); return 0; }
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
        if (LOWORD(w) == AddScriptButton && HIWORD(w) == BN_CLICKED)
        { if (object_ && addScript_) addScript_(); return 0; }
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
        SetTextColor(reinterpret_cast<HDC>(w), Text);
        SetBkColor(reinterpret_cast<HDC>(w), valid ? RGB(52, 54, 60) : RGB(92, 45, 45));
        return reinterpret_cast<LRESULT>(valid ? fieldBrush_ : invalidBrush_);
    }
    case WM_MOUSEWHEEL: scroll_ -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 37; Layout(); return 0;
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
LRESULT CALLBACK InspectorPanel::EditProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
{
    try { return reinterpret_cast<InspectorPanel*>(data)->HandleEdit(window, message, w, l); }
    catch (...) { return DefSubclassProc(window, message, w, l); }
}
LRESULT InspectorPanel::HandleEdit(HWND window, UINT message, WPARAM w, LPARAM l)
{
    const int dynamicIndex=GetDlgCtrlID(window)-FirstBehaviorField;
    if (dynamicIndex>=0 && dynamicIndex<static_cast<int>(behaviorFields_.size()))
    {
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
    if (index >= 2 && object_)
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
