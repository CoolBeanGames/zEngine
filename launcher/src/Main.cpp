#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <string>

#include "LauncherWindow.h"
#include "ProjectStore.h"

namespace
{
struct ComApartment
{
    bool ok = false;
    ComApartment() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComApartment() { if (ok) CoUninitialize(); }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    ComApartment com;

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&controls);

    // Double-clicked a ".zlaunch" file: open that project in the editor and exit.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::filesystem::path openFile;
        if (argv && argc >= 2)
        {
            std::filesystem::path candidate(argv[1]);
            if (_wcsicmp(candidate.extension().c_str(), L".zlaunch") == 0)
                openFile = candidate;
        }
        if (argv) LocalFree(argv);

        if (!openFile.empty())
        {
            const std::filesystem::path folder = openFile.parent_path();
            zlauncher::ProjectStore store;
            store.Load();
            try { store.AddExisting(folder); } catch (...) {}

            std::wstring error;
            if (!zlauncher::ProjectStore::LaunchProjectAt(folder, error))
            {
                MessageBoxW(nullptr, error.c_str(), L"zEngine Launcher", MB_OK | MB_ICONERROR);
                return EXIT_FAILURE;
            }
            return 0;
        }
    }

    try
    {
        zlauncher::LauncherWindow window(instance);
        window.Create(showCommand);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (window.PreTranslate(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "zEngine Launcher", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}
