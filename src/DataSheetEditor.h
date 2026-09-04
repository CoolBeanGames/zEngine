#pragma once
#include "DataSheetAssets.h"

#include <windows.h>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// ZE-92: a standalone pop-up spreadsheet editor for a ".zsheet" data sheet.
// Rows are data-object instances (keyed by name), columns are the struct's
// fields. Opened by double-clicking a .zsheet in the Media Library. The column
// list is resolved by EditorShell from the compiled struct type.
class DataSheetEditor final
{
public:
    // `columns` is {field name, canonical type}, in the struct's declared order.
    DataSheetEditor(HWND owner, std::filesystem::path assetsRoot, std::filesystem::path file,
                    std::vector<std::pair<std::string, std::string>> columns);
    ~DataSheetEditor();

    void Show();
    bool ConfirmClose();
    bool Dirty() const { return dirty_; }
    HWND Window() const { return window_; }
    const std::filesystem::path& File() const { return file_; }

    static constexpr int AddRow = 4600, DeleteRow = 4601, Save = 4602, Reload = 4603;
    static constexpr int FirstCell = 4700; // key + cell edit controls, row-major

private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Rebuild();       // recreate the grid controls from doc_
    void ReadGrid();      // pull edits back into doc_
    void LoadFile();
    void SaveFile();
    void Layout();
    void Title();
    int Stride() const { return static_cast<int>(columns_.size()) + 1; } // key + one per column

    std::filesystem::path assetsRoot_, file_;
    std::vector<std::pair<std::string, std::string>> columns_;
    zengine::datasheet::SheetDoc doc_, loaded_;
    std::vector<HWND> cells_;   // Stride() per row: [0]=key, [1..]=cells
    std::vector<HWND> headers_; // column header labels
    HWND window_ = nullptr, status_ = nullptr;
    bool dirty_ = false, loading_ = false;
};
