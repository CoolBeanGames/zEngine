#include "Renderer.h"

#include <windows.h>

#include <exception>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
    constexpr wchar_t WindowClassName[] = L"zEngineWindow";
    std::unique_ptr<Renderer> renderer;

    void ShowFatalError(const std::string& message)
    {
        MessageBoxA(nullptr, message.c_str(), "zEngine error", MB_OK | MB_ICONERROR);
    }

    LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_SIZE:
            if (renderer)
            {
                renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = WindowClassName;
        if (!RegisterClassExW(&windowClass))
        {
            throw std::runtime_error("Could not register the window class.");
        }

        RECT clientArea{0, 0, 1280, 720};
        AdjustWindowRect(&clientArea, WS_OVERLAPPEDWINDOW, FALSE);
        const HWND window = CreateWindowExW(
            0, WindowClassName, L"zEngine - Direct3D 11 Color Cube", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            clientArea.right - clientArea.left, clientArea.bottom - clientArea.top,
            nullptr, nullptr, instance, nullptr);
        if (!window)
        {
            throw std::runtime_error("Could not create the application window.");
        }

        renderer = std::make_unique<Renderer>();
        renderer->Initialize(window, 1280, 720);
        ShowWindow(window, showCommand);

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
                renderer->Render();
            }
        }
        renderer.reset();
        UnregisterClassW(WindowClassName, instance);
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        renderer.reset();
        ShowFatalError(error.what());
        return EXIT_FAILURE;
    }
}
