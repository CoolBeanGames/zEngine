#pragma once

#include "MaterialAssets.h"

#include <windows.h>
#include <filesystem>
#include <vector>

// ZE-102: a standalone editor for a ".material" asset - its shader reference and
// the concrete values pinned to each of the shader's parameters. Opened by
// double-clicking a .material in the Media Library. Mirrors InputMapEditor.
class MaterialEditor final
{
public:
    MaterialEditor(HWND owner, std::filesystem::path assetsRoot, std::filesystem::path file);
    ~MaterialEditor();

    void Show();
    bool ConfirmClose();
    bool Dirty() const { return dirty_; }
    HWND Window() const { return window_; }
    const std::filesystem::path& File() const { return file_; }

    static constexpr int MaxRows = 16;
    static constexpr int Shader = 4400, Save = 4401, Reload = 4402;
    static constexpr int Label0 = 4410;            // Label0 .. Label0+MaxRows-1
    static constexpr int Field0 = 4430;            // Field0 .. Field0 + MaxRows*4 - 1

private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void Populate();          // rebuild the visible rows from the effective params
    void ReadFields();        // pull the edit fields back into params_
    void LoadFile();
    void SaveFile();
    void Title();
    zengine::materials::Effective Resolve() const;

    std::filesystem::path assetsRoot_, file_;
    zengine::materials::MaterialDoc doc_, loaded_;
    std::vector<zengine::materials::Value> params_;
    HWND window_ = nullptr, status_ = nullptr;
    bool dirty_ = false, loading_ = false;
};
