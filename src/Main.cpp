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

        MSG message{};
        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (editor.TranslateShortcut(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else
            {
                editor.Render();
                if (IsIconic(window)) Sleep(20);
            }
        }
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        ShowFatalError(error.what());
        return EXIT_FAILURE;
    }
}
