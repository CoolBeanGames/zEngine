#include "EditorShell.h"

#include <windows.h>
#include <objbase.h>

#include <exception>
#include <cstdlib>
#include <string>
#include <stdexcept>

namespace
{
    struct ComApartment
    {
        ComApartment()
        {
            if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
                throw std::runtime_error("Could not initialize Windows image services.");
        }
        ~ComApartment() { CoUninitialize(); }
    };
    void ShowFatalError(const std::string& message)
    {
        MessageBoxA(nullptr, message.c_str(), "zEngine error", MB_OK | MB_ICONERROR);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        ComApartment apartment;
        EditorShell editor(instance);
        const HWND window = editor.Create(showCommand);
        editor.InitializeRenderer();
        editor.InitializeStartup();

        MSG message{};
        while (message.message != WM_QUIT)
        {
            // A continuous mouse drag can keep the queue nonempty forever. Drain a
            // bounded batch, then render so gizmo/camera interaction stays at frame rate.
            for (unsigned handled=0;handled<64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);++handled)
            {
                if (message.message==WM_QUIT) break;
                if (editor.TranslateShortcut(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if(message.message==WM_QUIT)break;
            editor.Render();
            if (IsIconic(window)) Sleep(20);
        }
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        ShowFatalError(error.what());
        return EXIT_FAILURE;
    }
}
