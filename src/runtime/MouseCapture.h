#pragma once
// ZE-84: apply a script-requested mouse capture mode (Input.set_mouse_mode) to the
// OS cursor and fill the MouseFrame. Shared by the standalone player and the
// editor's Play preview.
//   0 visible   - normal cursor, clamped -1..1 viewport position
//   1 hidden    - cursor invisible, otherwise normal
//   2 confined  - cursor clipped to the window/viewport
//   3 confined+hidden
//   4 captured  - cursor re-centred every frame; x/y become an unbounded delta
//                 accumulator so Mouse.delta stays correct (for FPS look).

#include "zscript/Script.h"

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace zengine::game {

struct MouseCaptureState {
    double accumulatedX = 0, accumulatedY = 0; // captured-mode running position (normalised)
    bool cursorHidden = false;
    bool clipping = false;

    // Call once when leaving Play / losing focus so the OS cursor is restored.
    void Release() {
        if (cursorHidden) { while (ShowCursor(TRUE) < 0) {} cursorHidden = false; }
        if (clipping) { ClipCursor(nullptr); clipping = false; }
        accumulatedX = accumulatedY = 0;
    }
};

// `client` = the region scripts see as the viewport (client px). `cursor` is the
// cursor position relative to that region's top-left. `hasFocus` gates capture
// so the editor / a background player never steals the pointer.
inline void ApplyMouseMode(int mode, HWND window, const RECT& client, POINT cursor,
                           bool hasFocus, MouseCaptureState& state, script::MouseFrame& mouse) {
    const long w = client.right - client.left, h = client.bottom - client.top;
    const bool wantHidden = hasFocus && (mode == 1 || mode == 3 || mode == 4);
    const bool wantClip   = hasFocus && (mode == 2 || mode == 3 || mode == 4);

    if (wantHidden && !state.cursorHidden) { while (ShowCursor(FALSE) >= 0) {} state.cursorHidden = true; }
    else if (!wantHidden && state.cursorHidden) { while (ShowCursor(TRUE) < 0) {} state.cursorHidden = false; }

    if (wantClip) {
        RECT screen = client;
        POINT tl{ client.left, client.top }, br{ client.right, client.bottom };
        ClientToScreen(window, &tl); ClientToScreen(window, &br);
        screen = { tl.x, tl.y, br.x, br.y };
        ClipCursor(&screen); state.clipping = true;
    } else if (state.clipping) { ClipCursor(nullptr); state.clipping = false; }

    if (mode == 4 && hasFocus && w > 0 && h > 0) {
        const POINT centre{ static_cast<long>(w / 2), static_cast<long>(h / 2) };
        state.accumulatedX += (cursor.x - centre.x) / (w * 0.5);
        state.accumulatedY += (centre.y - cursor.y) / (h * 0.5);
        POINT centreScreen{ client.left + centre.x, client.top + centre.y };
        ClientToScreen(window, &centreScreen);
        SetCursorPos(centreScreen.x, centreScreen.y);
        mouse.x = state.accumulatedX;
        mouse.y = state.accumulatedY;
        mouse.inside = true;
        return;
    }

    state.accumulatedX = state.accumulatedY = 0;
    if (w > 0 && h > 0) {
        mouse.inside = cursor.x >= 0 && cursor.y >= 0 && cursor.x < w && cursor.y < h;
        mouse.x = std::clamp(cursor.x / static_cast<double>(w) * 2.0 - 1.0, -1.0, 1.0);
        mouse.y = std::clamp(1.0 - cursor.y / static_cast<double>(h) * 2.0, -1.0, 1.0);
    }
}

}
