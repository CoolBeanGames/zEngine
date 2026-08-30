#include "EditorShell.h"

#include <windows.h>

#include <exception>
#include <cstdlib>
#include <string>

namespace
{
    void ShowFatalError(const std::string& message)
    {
        MessageBoxA(nullptr, message.c_str(), "zEngine error", MB_OK | MB_ICONERROR);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        EditorShell editor(instance);
        editor.Create(showCommand);
        editor.InitializeRenderer();

        MSG message{};
        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            else
            {
                editor.Render();
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
