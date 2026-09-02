// zLauncherUpdate - the "mini app" the launcher spawns to update itself.
//
//   zLauncherUpdate.exe --pid <n> --url <zipUrl> --dir <launcherDir> --exe <launcherExe>
//
// It waits for the launcher to exit, downloads the launcher zip, extracts it
// over <launcherDir>, relaunches <launcherExe>, and exits.

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace
{
std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* flag)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], flag) == 0) return argv[i + 1];
    return {};
}

void Fail(const std::wstring& text)
{
    MessageBoxW(nullptr, text.c_str(), L"zLauncher Update", MB_OK | MB_ICONWARNING);
}

bool HttpDownload(const std::wstring& url, const fs::path& target, std::wstring& error)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    parts.lpszHostName = host;   parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path;    parts.dwUrlPathLength = _countof(path);
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        error = L"Malformed update URL.";
        return false;
    }
    const std::wstring resource = std::wstring(path) + extra;

    HINTERNET session = WinHttpOpen(L"zLauncherUpdate", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;

    bool ok = false;
    if (request)
    {
        const wchar_t* headers = L"User-Agent: zLauncherUpdate\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status >= 200 && status < 300)
            {
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                std::string chunk;
                for (;;)
                {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                    chunk.resize(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                    out.write(chunk.data(), read);
                }
                ok = out.good();
                if (!ok) error = L"Could not write the downloaded file.";
            }
            else
            {
                error = L"Download failed (HTTP " + std::to_wstring(status) + L").";
            }
        }
        else
        {
            error = L"The download request failed.";
        }
    }
    else
    {
        error = L"Could not connect for the download.";
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool ExtractZip(const fs::path& zip, const fs::path& destination)
{
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(systemDir) + L"\\tar.exe\" -xf \"" + zip.wstring() +
                           L"\" -C \"" + destination.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    const std::wstring pidText = ArgValue(argc, argv, L"--pid");
    const std::wstring url = ArgValue(argc, argv, L"--url");
    const fs::path launcherDir = ArgValue(argc, argv, L"--dir");
    const fs::path launcherExe = ArgValue(argc, argv, L"--exe");
    LocalFree(argv);

    if (url.empty() || launcherDir.empty() || launcherExe.empty())
    {
        Fail(L"The updater was started without the information it needs.");
        return 1;
    }

    // Wait for the launcher to close so its files are not locked.
    if (!pidText.empty())
    {
        const DWORD pid = static_cast<DWORD>(_wtoi(pidText.c_str()));
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid))
        {
            WaitForSingleObject(process, 20000);
            CloseHandle(process);
        }
    }
    Sleep(400);

    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / L"zLauncherUpdate";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path zip = work / L"launcher.zip";
    const fs::path extract = work / L"payload";
    fs::create_directories(extract, ec);

    std::wstring error;
    if (!HttpDownload(url, zip, error))
    {
        Fail(L"Could not download the launcher update.\n" + error);
        ShellExecuteW(nullptr, L"open", launcherExe.c_str(), nullptr, launcherDir.c_str(), SW_SHOWNORMAL);
        return 1;
    }
    if (!ExtractZip(zip, extract))
    {
        Fail(L"Could not unzip the launcher update.");
        ShellExecuteW(nullptr, L"open", launcherExe.c_str(), nullptr, launcherDir.c_str(), SW_SHOWNORMAL);
        return 1;
    }

    // GitHub archives wrap everything in one folder.
    fs::path source = extract;
    {
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(extract, ec)) entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(entries.front(), ec)) source = entries.front();
    }

    int failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(source, ec))
    {
        const fs::path relative = fs::relative(e.path(), source, ec);
        if (relative.empty()) continue;
        const fs::path destination = launcherDir / relative;
        std::error_code cec;
        if (e.is_directory())
        {
            fs::create_directories(destination, cec);
            continue;
        }
        fs::create_directories(destination.parent_path(), cec);
        // The running updater exe cannot overwrite itself; stage it for next start.
        if (_wcsicmp(destination.filename().c_str(), L"zLauncherUpdate.exe") == 0)
        {
            fs::copy_file(e.path(), destination.wstring() + L".new",
                          fs::copy_options::overwrite_existing, cec);
            continue;
        }
        fs::copy_file(e.path(), destination, fs::copy_options::overwrite_existing, cec);
        if (cec) ++failed;
    }

    fs::remove_all(work, ec);

    if (failed > 0)
        Fail(std::to_wstring(failed) + L" launcher file(s) could not be replaced.");

    ShellExecuteW(nullptr, L"open", launcherExe.c_str(), nullptr, launcherDir.c_str(), SW_SHOWNORMAL);
    return failed == 0 ? 0 : 1;
}
