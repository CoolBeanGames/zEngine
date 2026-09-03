#include "EditorShell.h"
#include "CrashHandler.h"

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
    zengine::crash::Install("zEngine", __DATE__ " " __TIME__);
    if (const auto previous = zengine::crash::TakePreviousCrashReport(); !previous.empty())
        ShowFatalError("zEngine did not shut down cleanly last time.\nA crash report from that run is at:\n" + previous);
    try
    {
        zengine::crash::Breadcrumb("editor starting up");
        ComApartment apartment;
        EditorShell editor(instance);
        const HWND window = editor.Create(showCommand);
        editor.InitializeRenderer();
        editor.InitializeStartup();
        zengine::crash::Breadcrumb("editor entering the main loop");

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
        const auto report = zengine::crash::ReportHandledFatal(std::string("std::exception escaped wWinMain: ") + error.what());
        ShowFatalError(std::string(error.what()) + "\n\nCrash report:\n" + report);
        return EXIT_FAILURE;
    }
}
