#pragma once
#include "InputAssets.h"
#include <windows.h>
#include <functional>
class InputMapEditor final {
public:
    InputMapEditor(HWND owner,std::filesystem::path assets);
    ~InputMapEditor();
    void Show();
    bool ConfirmClose();
    bool Dirty() const { return dirty_; }
    HWND Window() const { return window_; }
    static constexpr int List=4100,Add=4101,Remove=4102,Save=4103,Reload=4104,Apply=4105,Name=4110,Type=4111,Binding0=4120,Controller=4130,Analog=4131,Deadzone=4132;
private:
    static LRESULT CALLBACK Procedure(HWND,UINT,WPARAM,LPARAM);
    void Layout(); void Populate(); void Select(int); void ApplyFields(); void Load(); void SaveFile(); void Title();
    void TypeControls();
    HWND window_=nullptr,list_=nullptr,status_=nullptr;
    std::filesystem::path assets_;
    zengine::input::Map map_;
    std::string loaded_;
    int selected_=-1;
    bool dirty_=false,loading_=false;
};
