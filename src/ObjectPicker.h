#pragma once

#include "AssetLibrary.h"
#include <windows.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A universal, in-app picker that replaces the OS file dialogs. The caller builds a
// flat list of candidate Items (assets and/or scene objects, already narrowed to what
// makes sense in this context); the picker then adds live search + per-type-group
// filter chips on top of that list. It comes in two shapes that share one code path:
//   - Window():   a modal centred dialog with icons, search, chips and OK/Clear/Cancel.
//   - Dropdown():  the same list as a frameless popup under a control; a click picks.
class ObjectPicker final
{
public:
    // Stable control ids so tests can drive the modal without a real mouse.
    static constexpr int SearchField = 4300, ResultList = 4301, ClearButton = 4302,
                         OkButton = 4303, CancelButton = 4304, FirstFilterChip = 4310;

    struct Item
    {
        std::wstring label;                                 // primary text
        std::wstring detail;                                // dim secondary text (path / owner)
        std::wstring group;                                 // filter category, e.g. L"Prefab", L"RigidBody"
        std::wstring value;                                 // returned verbatim when chosen
        assetLibrary::Kind icon = assetLibrary::Kind::File; // small code-drawn glyph
    };
    struct Request
    {
        std::wstring title = L"Select";
        std::vector<Item> items;
        std::wstring current;      // pre-select the item whose value == current
        bool allowClear = false;   // offer a "None" result (Choice::cleared)
        std::wstring emptyText = L"Nothing of this type exists yet.";
    };
    struct Choice
    {
        bool picked = false;   // the user chose an item; value holds it
        bool cleared = false;  // the user chose "None"
        std::wstring value;
        static Choice Cancel() { return {}; }
        static Choice Pick(std::wstring v) { return {true, false, std::move(v)}; }
        static Choice Cleared() { return {false, true, {}}; }
    };

    static Choice Window(HWND owner, const Request& request);
    static Choice Dropdown(HWND owner, RECT anchorScreen, const Request& request);

    // Test hook: when set, Window()/Dropdown() skip the UI and return responder(request).
    // Used by call-site integration tests; the picker's own UI has a dedicated test.
    static void SetTestResponder(std::function<Choice(const Request&)> responder);
};
