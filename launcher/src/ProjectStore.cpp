#include "ProjectStore.h"

#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winver.h>
#include <winhttp.h>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace zlauncher
{
namespace
{
constexpr std::size_t kConfigLimit = 1024 * 1024;

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count,
                        nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

fs::path LocalAppData()
{
    PWSTR location = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &location)))
        throw std::runtime_error("Cannot locate the local application data folder.");
    fs::path result(location);
    CoTaskMemFree(location);
    return result;
}

fs::path ModuleFilePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) throw std::runtime_error("Cannot determine the launcher location.");
        if (n < buffer.size()) { buffer.resize(n); break; }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer);
}

fs::path ModuleDirectory() { return ModuleFilePath().parent_path(); }

// Read a bounded text file. Returns nullopt when it does not exist.
std::optional<std::string> ReadTextFile(const fs::path& file, std::size_t limit)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    const auto size = fs::file_size(file, ec);
    if (ec || size > limit) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(size)) && size != 0) return std::nullopt;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3); // tolerate a UTF-8 BOM
    return text;
}

// Overwrite a small preferences-style file. These are replaceable state, so a
// straight rewrite (temp + atomic replace) is enough.
void WriteTextFileAtomic(const fs::path& target, const std::string& text)
{
    fs::create_directories(target.parent_path());
    fs::path temp = target;
    temp += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write to the launcher data folder.");
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) { out.close(); fs::remove(temp); throw std::runtime_error("Cannot write launcher data."); }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec)
    {
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
        if (ec) { fs::remove(temp); throw std::runtime_error("Cannot save launcher data."); }
    }
}

// Pull the quoted value that follows `name` in a ZENGINE_PROJECT config.
std::optional<std::wstring> ProjectConfigName(const std::string& text)
{
    std::istringstream in(text);
    std::string token;
    if (!(in >> token) || token != "ZENGINE_PROJECT") return std::nullopt;
    int version = 0;
    in >> version;
    while (in >> token)
    {
        if (token == "name")
        {
            std::string value;
            if (in >> std::quoted(value)) return Wide(value);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void ValidateProjectName(const std::wstring& name)
{
    const bool sane = !name.empty() && name.size() <= 80 && name != L"." && name != L".." &&
                      name.front() != L' ' && name.back() != L' ' && name.back() != L'.';
    if (!sane)
        throw std::runtime_error("Choose a project name (1-80 characters, no leading or trailing "
                                 "spaces and no trailing dot).");
    if (name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
        throw std::runtime_error("The project name contains characters that are not allowed in a folder name.");

    std::wstring base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return std::towupper(c); });
    const bool reserved = base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
                          (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
                           base[3] >= L'1' && base[3] <= L'9');
    if (reserved) throw std::runtime_error("That project name is a reserved Windows device name.");
}

// Find the single *.zproject directly inside a folder, if there is exactly one.
std::optional<fs::path> FindConfig(const fs::path& folder)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return std::nullopt;
    std::optional<fs::path> found;
    for (const auto& e : fs::directory_iterator(folder, ec))
    {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        if (_wcsicmp(e.path().extension().c_str(), L".zproject") != 0) continue;
        if (found) return std::nullopt; // ambiguous
        found = e.path();
    }
    return found;
}

void RewriteConfigName(const fs::path& config, const std::wstring& newName)
{
    auto text = ReadTextFile(config, kConfigLimit);
    if (!text) return;
    const auto pos = text->find("name");
    if (pos == std::string::npos) return;
    const auto open = text->find('"', pos);
    if (open == std::string::npos) return;
    auto close = open + 1;
    while (close < text->size() && (*text)[close] != '"')
        close += ((*text)[close] == '\\' && close + 1 < text->size()) ? 2 : 1;
    if (close >= text->size()) return;

    std::ostringstream quoted;
    quoted << std::quoted(Utf8(newName));
    text->replace(open, close - open + 1, quoted.str());
    WriteTextFileAtomic(config, *text);
}

std::string EncodeMinimalConfig(const std::wstring& name)
{
    std::ostringstream out;
    out << "ZENGINE_PROJECT 1\nname " << std::quoted(Utf8(name))
        << "\nassets \"Assets\"\nlast_scene \"\"\nscenes 0\nend\n";
    return out.str();
}

// --- per-project ".zlaunch" sidecar --------------------------------------------
// A tiny launcher-owned file kept next to the project's .zproject. It carries the
// display name, a version string, an icon reference and the timestamps the
// project row shows. The editor is free to overwrite name/version/icon later.
struct Sidecar
{
    std::wstring name;
    std::wstring version;
    std::wstring icon;
    std::wstring created;   // "YYYY-MM-DD HH:MM:SS"
    std::wstring launched;
};

std::wstring FormatFileTime(const FILETIME& ft)
{
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&ft, &utc)) return {};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t buffer[24];
    swprintf(buffer, 24, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay,
             local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

std::wstring NowStamp()
{
    FILETIME ft{};
    SYSTEMTIME st{};
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    return FormatFileTime(ft);
}

std::wstring CreationStamp(const fs::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
    return FormatFileTime(data.ftCreationTime);
}

std::wstring LastWriteStamp(const fs::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
    return FormatFileTime(data.ftLastWriteTime);
}

// Where the sidecar lives: "<folder>/<folder name>.zlaunch", or any single
// existing "*.zlaunch" already in the folder.
fs::path SidecarPath(const fs::path& folder)
{
    const fs::path preferred = folder / (folder.filename().wstring() + L".zlaunch");
    std::error_code ec;
    if (fs::exists(preferred, ec)) return preferred;
    fs::path found;
    if (fs::is_directory(folder, ec))
        for (const auto& e : fs::directory_iterator(folder, ec))
        {
            if (ec) break;
            if (e.is_regular_file() && _wcsicmp(e.path().extension().c_str(), L".zlaunch") == 0)
            {
                if (!found.empty()) return preferred; // ambiguous, fall back to the canonical name
                found = e.path();
            }
        }
    return found.empty() ? preferred : found;
}

std::optional<Sidecar> ReadSidecar(const fs::path& folder)
{
    auto text = ReadTextFile(SidecarPath(folder), 64 * 1024);
    if (!text) return std::nullopt;
    std::istringstream in(*text);
    std::string token;
    if (!(in >> token) || token != "ZLAUNCH") return std::nullopt;
    int version = 0;
    in >> version;
    Sidecar out;
    while (in >> token)
    {
        std::string value;
        if (!(in >> std::quoted(value))) break;
        if (token == "name") out.name = Wide(value);
        else if (token == "version") out.version = Wide(value);
        else if (token == "icon") out.icon = Wide(value);
        else if (token == "created") out.created = Wide(value);
        else if (token == "launched") out.launched = Wide(value);
    }
    return out;
}

void WriteSidecar(const fs::path& folder, const Sidecar& sidecar)
{
    std::ostringstream out;
    out << "ZLAUNCH 1\n"
        << "name " << std::quoted(Utf8(sidecar.name)) << '\n'
        << "version " << std::quoted(Utf8(sidecar.version)) << '\n'
        << "icon " << std::quoted(Utf8(sidecar.icon)) << '\n'
        << "created " << std::quoted(Utf8(sidecar.created)) << '\n'
        << "launched " << std::quoted(Utf8(sidecar.launched)) << '\n';
    WriteTextFileAtomic(SidecarPath(folder), out.str());
}

std::wstring GetClassesString(const wchar_t* subkey)
{
    const std::wstring full = std::wstring(L"Software\\Classes\\") + subkey;
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, full.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS)
        return {};
    return buffer;
}

LSTATUS SetClassesString(const wchar_t* subkey, const std::wstring& data)
{
    HKEY key{};
    const std::wstring full = std::wstring(L"Software\\Classes\\") + subkey;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, nullptr, 0, KEY_WRITE,
                                     nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                            static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status;
}

// --- editor update over GitHub releases ---------------------------------------
// HTTPS GET of a full URL into `out`. Follows redirects. `out` is capped.
bool HttpGet(const std::wstring& url, std::string& out, std::wstring& error)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    parts.lpszHostName = host;      parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path;       parts.dwUrlPathLength = _countof(path);
    parts.lpszExtraInfo = extra;    parts.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        error = L"Malformed download URL.";
        return false;
    }
    const std::wstring resource = std::wstring(path) + extra;

    HINTERNET session = WinHttpOpen(L"zLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"Could not start a network session."; return false; }

    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;

    bool ok = false;
    if (request)
    {
        const std::wstring headers = L"User-Agent: zLauncher\r\nAccept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

            out.clear();
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                out.append(chunk.data(), read);
                if (out.size() > 300u * 1024u * 1024u)
                {
                    error = L"The download is unexpectedly large; aborted.";
                    break;
                }
            }

            if (error.empty())
            {
                if (status == 404)
                    error = L"No editor releases have been published on GitHub yet.";
                else if (status < 200 || status >= 300)
                    error = L"GitHub returned HTTP " + std::to_wstring(status) + L".";
                else
                    ok = true;
            }
        }
        else
        {
            error = L"The network request failed (offline?).";
        }
    }
    else if (error.empty())
    {
        error = L"Could not connect to GitHub.";
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

std::string JsonString(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    auto k = json.find(needle);
    if (k == std::string::npos) return {};
    auto colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    auto q1 = json.find('"', colon);
    if (q1 == std::string::npos) return {};
    std::string out;
    for (auto i = q1 + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            const char n = json[++i];
            out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n;
        }
        else if (c == '"')
        {
            break;
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// Scan a GitHub /releases (or /releases/latest) payload for the highest-numbered
// asset named "<prefix>_<n>.zip". Returns its number and download URL.
bool HighestNumberedAsset(const std::string& json, const std::string& prefix, int& number,
                          std::string& url)
{
    number = -1;
    const std::string wanted = prefix + "_";
    const std::string nameKey = "\"name\"";
    for (std::size_t pos = 0; (pos = json.find(nameKey, pos)) != std::string::npos;)
    {
        pos += nameKey.size();
        const auto colon = json.find(':', pos);
        const auto q1 = colon == std::string::npos ? std::string::npos : json.find('"', colon);
        const auto q2 = q1 == std::string::npos ? std::string::npos : json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string value = json.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        if (value.rfind(wanted, 0) != 0 || value.size() < wanted.size() + 5 ||
            value.compare(value.size() - 4, 4, ".zip") != 0)
            continue;
        const std::string digits = value.substr(wanted.size(), value.size() - wanted.size() - 4);
        if (digits.empty() ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const int n = std::atoi(digits.c_str());

        const auto urlKey = json.find("\"browser_download_url\"", pos);
        if (urlKey == std::string::npos) continue;
        const auto u1 = json.find('"', json.find(':', urlKey));
        const auto u2 = u1 == std::string::npos ? std::string::npos : json.find('"', u1 + 1);
        if (u2 == std::string::npos) continue;

        if (n > number)
        {
            number = n;
            url = json.substr(u1 + 1, u2 - u1 - 1);
        }
    }
    return number >= 0;
}

std::string FirstZipAssetUrl(const std::string& json)
{
    const std::string needle = "\"browser_download_url\"";
    for (std::size_t pos = 0; (pos = json.find(needle, pos)) != std::string::npos;)
    {
        const auto colon = json.find(':', pos + needle.size());
        const auto q1 = colon == std::string::npos ? std::string::npos : json.find('"', colon);
        const auto q2 = q1 == std::string::npos ? std::string::npos : json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string url = json.substr(q1 + 1, q2 - q1 - 1);
        if (url.size() > 4 && _stricmp(url.c_str() + url.size() - 4, ".zip") == 0) return url;
        pos = q2 + 1;
    }
    return {};
}

bool RunAndWait(const std::wstring& commandLine, DWORD timeoutMs, DWORD& exitCode)
{
    std::wstring mutableCommand = commandLine;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}

// --- where the editor lives --------------------------------------------------
// Preferred: a flat "<Program Files>\zEngine\zEngine.exe". Writing there needs
// administrator rights, so when that copy fails we fall back to a per-user,
// versioned "<LocalAppData>\zEngine\Engine\Downloads\<n>\" and just point the
// launcher (and the shortcuts) at the newest one.
fs::path ProgramFilesEditorDirImpl()
{
    PWSTR dir = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &dir)) && dir)
        base = dir;
    if (dir) CoTaskMemFree(dir);
    if (base.empty()) base = L"C:\\Program Files";
    return base / L"zEngine";
}

fs::path EditorDownloadsRootImpl()
{
    return LocalAppData() / L"zEngine" / L"Engine" / L"Downloads";
}

// Kept for the public API; the "versions root" is now the per-user downloads dir.
fs::path EngineVersionsRootImpl() { return EditorDownloadsRootImpl(); }

// Parse the trailing run of digits from a "version.txt" ("engine_4" -> 4).
int NumberFromVersionFile(const fs::path& editorDir)
{
    auto text = ReadTextFile(editorDir / L"version.txt", 4096);
    if (!text) return -1;
    std::string s = *text;
    while (!s.empty() && (s.back() < '0' || s.back() > '9')) s.pop_back();
    std::size_t start = s.size();
    while (start > 0 && s[start - 1] >= '0' && s[start - 1] <= '9') --start;
    if (start == s.size()) return -1;
    return std::atoi(s.c_str() + start);
}

// The newest editor sitting under `versionsRoot`: each direct subfolder that
// contains a "zEngine.exe" is a candidate; a folder whose name is all digits is
// treated as a build number and the highest one wins, otherwise the most
// recently written exe wins. `outNumber` receives the winning build number, or
// -1 when the winner has a non-numeric folder name / nothing was found.
std::optional<fs::path> NewestEditorUnder(const fs::path& versionsRoot, int* outNumber)
{
    if (outNumber) *outNumber = -1;
    std::error_code ec;
    if (!fs::is_directory(versionsRoot, ec)) return std::nullopt;

    std::optional<fs::path> best;
    int bestNumber = -1;
    FILETIME bestTime{};
    for (const auto& e : fs::directory_iterator(versionsRoot, ec))
    {
        if (ec) break;
        if (!e.is_directory()) continue;

        fs::path exe = e.path() / L"zEngine.exe";
        if (!fs::is_regular_file(exe, ec))
        {
            exe.clear();
            for (const auto& candidate :
                 { e.path() / L"Release" / L"zEngine.exe", e.path() / L"bin" / L"zEngine.exe" })
                if (fs::is_regular_file(candidate, ec)) { exe = candidate; break; }
            if (exe.empty()) continue;
        }

        const std::wstring folderName = e.path().filename().wstring();
        int number = -1;
        if (!folderName.empty() &&
            std::all_of(folderName.begin(), folderName.end(),
                        [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            number = _wtoi(folderName.c_str());

        bool take = false;
        if (number >= 0)
        {
            take = number > bestNumber;
        }
        else if (bestNumber < 0)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data))
            {
                if (!best || CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0)
                {
                    take = true;
                    bestTime = data.ftLastWriteTime;
                }
            }
            else if (!best)
            {
                take = true;
            }
        }

        if (take)
        {
            best = exe;
            if (number >= 0) bestNumber = number;
        }
    }
    if (outNumber) *outNumber = bestNumber;
    return best;
}

// The scratch folder a download/unzip works in. Caller cleans it up.
fs::path EditorDownloadWorkDir() { return LocalAppData() / L"zLauncher" / L"editor-download"; }

// Download a release archive from `url` and unzip it with the system `tar` into
// the scratch work dir. On success `sourceDir` is the folder that holds the
// editor files (GitHub source archives that wrap everything in one top-level
// folder are unwrapped). Returns false + fills `message` on any failure.
bool DownloadAndUnzip(const std::wstring& url, fs::path& sourceDir, std::wstring& message)
{
    std::wstring error;
    std::string archiveBytes;
    if (!HttpGet(url, archiveBytes, error))
    {
        message = L"Could not download the release: " + error;
        return false;
    }
    if (archiveBytes.size() < 128)
    {
        message = L"The download returned no data (" + std::to_wstring(archiveBytes.size()) +
                  L" bytes) - the release asset may be missing.";
        return false;
    }

    std::error_code ec;
    const fs::path work = EditorDownloadWorkDir();
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    const fs::path zipPath = work / L"release.zip";
    {
        std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
        out.write(archiveBytes.data(), static_cast<std::streamsize>(archiveBytes.size()));
        if (!out)
        {
            fs::remove_all(work, ec);
            message = L"Could not write the downloaded archive to disk.";
            return false;
        }
    }

    const fs::path extractDir = work / L"extracted";
    fs::create_directories(extractDir, ec);
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring command = L"\"" + std::wstring(systemDir) + L"\\tar.exe\" -xf \"" +
                                 zipPath.wstring() + L"\" -C \"" + extractDir.wstring() + L"\"";
    DWORD exitCode = 0;
    if (!RunAndWait(command, 180000, exitCode))
    {
        fs::remove_all(work, ec);
        message = L"Could not unzip the release (tar exit " + std::to_wstring(exitCode) + L").";
        return false;
    }

    fs::path source = extractDir;
    {
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(extractDir, ec)) entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(entries.front(), ec)) source = entries.front();
    }

    // A release ought to contain the editor exe; a source zipball would not.
    if (!fs::is_regular_file(source / L"zEngine.exe", ec))
    {
        // tolerate one more nesting level (e.g. a "Release/" wrapper)
        for (const auto& e : fs::directory_iterator(source, ec))
            if (e.is_directory() && fs::is_regular_file(e.path() / L"zEngine.exe", ec))
            {
                source = e.path();
                break;
            }
    }

    sourceDir = source;
    return true;
}

// Copy every file under `source` into `destDir` (created if needed), overwriting.
// Returns false + `message` when any file could not be written (e.g. Program
// Files without elevation, or the editor is running).
bool CopyTreeInto(const fs::path& source, const fs::path& destDir, std::wstring& message)
{
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec)
    {
        message = L"Could not create " + destDir.wstring() + L".";
        return false;
    }

    int copied = 0;
    int failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(source, ec))
    {
        const fs::path relative = fs::relative(e.path(), source, ec);
        if (relative.empty()) continue;
        const fs::path destination = destDir / relative;
        std::error_code cec;
        if (e.is_directory())
        {
            fs::create_directories(destination, cec);
            continue;
        }
        fs::create_directories(destination.parent_path(), cec);
        fs::copy_file(e.path(), destination, fs::copy_options::overwrite_existing, cec);
        if (cec) ++failed;
        else ++copied;
    }

    if (copied == 0)
    {
        message = L"Nothing could be written under " + destDir.wstring() + L".";
        return false;
    }
    if (failed > 0)
    {
        message = std::to_wstring(failed) + L" file(s) could not be written under " +
                  destDir.wstring() + L".";
        return false;
    }
    return true;
}

// Create/overwrite a .lnk shortcut at `linkFile` pointing at `target`.
bool WriteShortcut(const fs::path& target, const fs::path& linkFile, const std::wstring& description)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;

    link->SetPath(target.c_str());
    link->SetWorkingDirectory(target.parent_path().c_str());
    link->SetIconLocation(target.c_str(), 0);
    if (!description.empty()) link->SetDescription(description.c_str());

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
    {
        std::error_code ec;
        fs::create_directories(linkFile.parent_path(), ec);
        ok = SUCCEEDED(file->Save(linkFile.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// (Re)create the Start-menu and Desktop shortcuts to the newest installed editor.
void RefreshEditorShortcuts(const fs::path& editorExe)
{
    std::error_code ec;
    if (editorExe.empty() || !fs::is_regular_file(editorExe, ec)) return;

    auto place = [&](REFKNOWNFOLDERID id) {
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &dir)) && dir)
            WriteShortcut(editorExe, fs::path(dir) / L"zEngine Editor.lnk", L"zEngine editor");
        if (dir) CoTaskMemFree(dir);
    };
    place(FOLDERID_Programs);
    place(FOLDERID_Desktop);
}

// Which GitHub repo to pull the editor/launcher from. Overridable via
// <launcher data>/repo.txt ("owner/repo").
void ResolveRepo(std::string& owner, std::string& repo)
{
    owner = "CoolBeanGames";
    repo = "zEngine";
    if (auto text = ReadTextFile(LocalAppData() / L"zLauncher" / L"repo.txt", 4096))
    {
        std::string line = *text;
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (const auto end = line.find_first_of(" \t\r\n"); end != std::string::npos) line.erase(end);
        if (const auto slash = line.find('/'); slash != std::string::npos && slash + 1 < line.size())
        {
            owner = line.substr(0, slash);
            repo = line.substr(slash + 1);
        }
    }
}

// Per-user association: double-clicking a ".zlaunch" runs `zLauncher.exe "<file>"`.
// Idempotent, and a no-op once it already points at this exe.
void RegisterFileAssociation()
{
    const std::wstring command = L"\"" + ModuleFilePath().wstring() + L"\" \"%1\"";
    if (GetClassesString(L"zLauncher.Project\\shell\\open\\command") == command &&
        GetClassesString(L".zlaunch") == L"zLauncher.Project")
        return;

    const std::wstring exe = L"\"" + ModuleFilePath().wstring() + L"\"";
    SetClassesString(L".zlaunch", L"zLauncher.Project");
    SetClassesString(L"zLauncher.Project", L"zEngine Project");
    SetClassesString(L"zLauncher.Project\\DefaultIcon", exe + L",0");
    SetClassesString(L"zLauncher.Project\\shell\\open\\command", command);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
} // namespace

fs::path ProjectStore::RegistryFile()
{
    return LocalAppData() / L"zLauncher" / L"projects.txt";
}

std::optional<fs::path> ProjectStore::LocateEditor()
{
    const fs::path here = ModuleDirectory();

    // An explicit override wins: <launcher data>/editor_path.txt with one path.
    if (auto text = ReadTextFile(LocalAppData() / L"zLauncher" / L"editor_path.txt", 32 * 1024))
    {
        std::wstring line = Wide(*text);
        line.erase(0, line.find_first_not_of(L" \t\r\n"));
        const auto end = line.find_last_not_of(L" \t\r\n");
        if (end != std::wstring::npos) line.erase(end + 1);
        std::error_code ec;
        if (!line.empty() && fs::is_regular_file(fs::path(line), ec)) return fs::path(line);
    }

    // The installed editor: a flat "<Program Files>\zEngine\zEngine.exe" first,
    // then the newest "<n>" folder under the per-user downloads dir.
    {
        std::error_code ec;
        const fs::path pf = ProgramFilesEditorDirImpl() / L"zEngine.exe";
        if (fs::is_regular_file(pf, ec)) return pf;
    }
    if (auto installed = NewestEditorUnder(EditorDownloadsRootImpl(), nullptr)) return installed;

    const fs::path candidates[] = {
        here / L"zEngine.exe",
        here / L"Release" / L"zEngine.exe",
        here / L"Debug" / L"zEngine.exe",
        here.parent_path() / L"zEngine.exe",
    };
    std::error_code ec;
    for (const auto& c : candidates)
        if (fs::is_regular_file(c, ec)) return c;

    // Development fallback: walk up looking for the engine's build outputs.
    for (fs::path dir = here; !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
    {
        const fs::path dev[] = {
            dir / L"builds" / L"release" / L"Release" / L"zEngine.exe",
            dir / L"builds" / L"debug" / L"Debug" / L"zEngine.exe",
        };
        for (const auto& c : dev)
            if (fs::is_regular_file(c, ec)) return c;
    }
    return std::nullopt;
}

std::wstring ProjectStore::EditorVersion()
{
    auto editor = LocateEditor();
    if (!editor) return {};

    // A plain "version.txt" next to the editor wins (the engine build can drop
    // one there; it is also the natural place for a future updater to record it).
    if (auto text = ReadTextFile(editor->parent_path() / L"version.txt", 4096))
    {
        std::wstring line = Wide(*text);
        const auto cut = line.find_first_of(L"\r\n");
        if (cut != std::wstring::npos) line.erase(cut);
        while (!line.empty() && (line.front() == L' ' || line.front() == L'v' || line.front() == L'V'))
            line.erase(line.begin());
        while (!line.empty() && line.back() == L' ') line.pop_back();
        if (!line.empty()) return line;
    }

    // Next, the exe's own version resource, if it was built with one.
    DWORD handle = 0;
    if (const DWORD size = GetFileVersionInfoSizeW(editor->c_str(), &handle))
    {
        std::vector<BYTE> buffer(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoLen = 0;
        if (GetFileVersionInfoW(editor->c_str(), handle, size, buffer.data()) &&
            VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoLen) && info &&
            infoLen >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xFEEF04BDu)
        {
            wchar_t out[64];
            swprintf(out, 64, L"%u.%u.%u", HIWORD(info->dwProductVersionMS),
                     LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS));
            return out;
        }
    }

    // Development fallback: an editor run straight out of the engine's build tree
    // still records its version in the adjacent CMakeCache.txt.
    for (fs::path dir = editor->parent_path(); !dir.empty() && dir != dir.root_path();
         dir = dir.parent_path())
    {
        auto cache = ReadTextFile(dir / L"CMakeCache.txt", 4 * 1024 * 1024);
        if (!cache) continue;
        const auto key = cache->find("CMAKE_PROJECT_VERSION:");
        if (key == std::string::npos) continue;
        const auto eq = cache->find('=', key);
        if (eq == std::string::npos) continue;
        auto end = cache->find_first_of("\r\n", eq);
        std::string value = cache->substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        if (!value.empty()) return Wide(value);
    }
    return {};
}

ProjectEntry ProjectStore::Resolve(const fs::path& folder)
{
    ProjectEntry entry;
    std::error_code ec;
    entry.folder = fs::weakly_canonical(folder, ec);
    if (entry.folder.empty()) entry.folder = folder;

    if (auto config = FindConfig(entry.folder))
    {
        entry.configFile = *config;
        entry.valid = true;
        if (auto text = ReadTextFile(*config, kConfigLimit))
            if (auto name = ProjectConfigName(*text))
                entry.name = *name;
    }
    if (entry.name.empty()) entry.name = entry.folder.filename().wstring();
    if (entry.name.empty()) entry.name = entry.folder.wstring();

    // Row metadata. Missing pieces stay empty and are not shown.
    if (auto sidecar = ReadSidecar(entry.folder))
    {
        entry.version = sidecar->version;
        entry.created = sidecar->created;
        entry.launched = sidecar->launched;
    }
    if (entry.created.empty())
        entry.created = CreationStamp(!entry.configFile.empty() ? entry.configFile : entry.folder);

    const fs::path current = entry.folder / L"builds" / L"current";
    std::error_code bec;
    if (fs::is_directory(current, bec)) entry.built = LastWriteStamp(current);

    return entry;
}

void ProjectStore::MarkLaunched(std::size_t index)
{
    if (index >= projects_.size()) return;
    ProjectEntry& entry = projects_[index];

    Sidecar sidecar;
    if (auto existing = ReadSidecar(entry.folder)) sidecar = *existing;
    if (sidecar.name.empty()) sidecar.name = entry.name;
    if (sidecar.created.empty())
    {
        sidecar.created = CreationStamp(!entry.configFile.empty() ? entry.configFile : entry.folder);
        if (sidecar.created.empty()) sidecar.created = NowStamp();
    }
    sidecar.launched = NowStamp();

    try
    {
        WriteSidecar(entry.folder, sidecar);
    }
    catch (const std::exception&)
    {
        // A read-only project folder should not break launching.
        return;
    }
    entry.launched = sidecar.launched;
    entry.created = sidecar.created;
    if (entry.version.empty()) entry.version = sidecar.version;
}

void ProjectStore::Load()
{
    projects_.clear();
    auto text = ReadTextFile(RegistryFile(), kConfigLimit);
    if (!text) return;

    std::istringstream in(*text);
    std::string token;
    if (!(in >> token) || token != "ZLAUNCHER") return;
    int version = 0;
    in >> version;

    std::vector<std::wstring> seen;
    while (in >> token)
    {
        if (token != "project") continue;
        std::string quoted;
        if (!(in >> std::quoted(quoted))) break;
        fs::path folder = Wide(quoted);
        if (folder.empty()) continue;

        std::wstring key = folder.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);

        projects_.push_back(Resolve(folder));
    }
}

void ProjectStore::Save() const
{
    std::ostringstream out;
    out << "ZLAUNCHER 1\n";
    for (const auto& p : projects_)
        out << "project " << std::quoted(Utf8(p.folder.wstring())) << '\n';
    WriteTextFileAtomic(RegistryFile(), out.str());
}

void ProjectStore::AddExisting(const fs::path& folder)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        throw std::runtime_error("That folder does not exist.");
    if (!FindConfig(folder))
        throw std::runtime_error("That folder does not contain a single .zproject file.");

    auto entry = Resolve(folder);
    std::wstring key = entry.folder.wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
    for (const auto& p : projects_)
    {
        std::wstring other = p.folder.wstring();
        std::transform(other.begin(), other.end(), other.begin(), [](wchar_t c) { return std::towlower(c); });
        if (other == key) return; // already listed
    }
    projects_.push_back(std::move(entry));
    Save();
}

void ProjectStore::Remove(std::size_t index)
{
    if (index >= projects_.size()) return;
    projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(index));
    Save();
}

ProjectEntry ProjectStore::Create(const fs::path& parent, const std::wstring& name,
                                  const ProjectTemplate* tmpl)
{
    ValidateProjectName(name);

    std::error_code ec;
    if (!parent.is_absolute() || !fs::is_directory(parent, ec))
        throw std::runtime_error("Choose an existing folder to create the project in.");

    const fs::path root = fs::weakly_canonical(parent, ec) / name;
    if (fs::exists(root, ec))
        throw std::runtime_error("A folder with that name already exists in that location.");

    if (!fs::create_directories(root, ec) || ec)
        throw std::runtime_error("Could not create the project folder.");

    try
    {
        Sidecar sidecar;
        if (tmpl)
        {
            if (!fs::is_directory(tmpl->folder, ec))
                throw std::runtime_error("The selected template folder is missing.");
            fs::copy(tmpl->folder, root,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                         fs::copy_options::skip_symlinks,
                     ec);
            if (ec) throw std::runtime_error("Could not copy the template into the new project.");

            fs::create_directories(root / L"Assets", ec);
            auto config = FindConfig(root);
            if (!config)
                throw std::runtime_error("The template does not contain a .zproject file.");
            fs::path desired = root / (name + L".zproject");
            if (_wcsicmp(config->c_str(), desired.c_str()) != 0)
            {
                fs::rename(*config, desired, ec);
                if (ec) desired = *config; // keep the template's name if rename fails
            }
            RewriteConfigName(desired, name);

            // Keep any version/icon the template carried, but re-issue the
            // sidecar under this project's canonical "<name>.zlaunch" filename.
            if (auto templateSidecar = ReadSidecar(root))
            {
                sidecar.version = templateSidecar->version;
                sidecar.icon = templateSidecar->icon;
            }
            for (const auto& e : fs::directory_iterator(root, ec))
                if (e.is_regular_file() && _wcsicmp(e.path().extension().c_str(), L".zlaunch") == 0)
                    fs::remove(e.path(), ec);
        }
        else
        {
            fs::create_directories(root / L"Assets", ec);
            WriteTextFileAtomic(root / (name + L".zproject"), EncodeMinimalConfig(name));
        }

        // Every project folder gets a ".zlaunch" file: version / name / icon plus
        // the timestamps the launcher shows. Double-clicking it opens the project.
        sidecar.name = name;
        sidecar.created = NowStamp();
        sidecar.launched.clear();
        WriteSidecar(root, sidecar);
    }
    catch (...)
    {
        // Leave partial files in place (they may be recoverable) but do not
        // register a project we failed to finish.
        throw;
    }

    auto entry = Resolve(root);
    if (!entry.valid)
        throw std::runtime_error("The new project was created but its config is not readable.");

    projects_.push_back(entry);
    Save();
    return entry;
}

std::vector<ProjectTemplate> ProjectStore::Templates() const
{
    std::vector<ProjectTemplate> result;
    auto editor = LocateEditor();
    if (!editor) return result;

    const fs::path dir = editor->parent_path() / L"templates";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return result;

    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (!e.is_directory()) continue;
        if (!FindConfig(e.path())) continue;
        result.push_back({ e.path().filename().wstring(), e.path() });
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return result;
}

bool ProjectStore::CopyToTemplates(const ProjectEntry& entry, std::wstring& message) const
{
    if (!entry.valid || entry.configFile.empty())
    {
        message = L"This project has no readable .zproject file.";
        return false;
    }
    auto editor = LocateEditor();
    if (!editor)
    {
        message = L"Could not find the editor, so its templates folder is unknown.";
        return false;
    }

    std::error_code ec;
    const fs::path templatesDir = editor->parent_path() / L"templates";
    fs::create_directories(templatesDir, ec);
    const fs::path dest = templatesDir / entry.name;
    if (fs::exists(dest, ec))
    {
        message = L"A template named \"" + entry.name + L"\" already exists in " +
                  templatesDir.wstring() + L".";
        return false;
    }
    if (!fs::create_directories(dest, ec) || ec)
    {
        message = L"Could not create " + dest.wstring() +
                  L" (the editor's templates folder may be read-only).";
        return false;
    }

    int files = 0;
    int failed = 0;
    const auto options = fs::directory_options::skip_permission_denied;
    auto it = fs::recursive_directory_iterator(entry.folder, options, ec);
    for (; it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), entry.folder, ec);
        if (rel.empty()) continue;

        // A template is just source + assets: skip build outputs and VCS data.
        const std::wstring top = rel.begin()->wstring();
        std::error_code sec;
        if (it->is_directory(sec) &&
            (_wcsicmp(top.c_str(), L"builds") == 0 || _wcsicmp(top.c_str(), L".git") == 0))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (_wcsicmp(top.c_str(), L"builds") == 0 || _wcsicmp(top.c_str(), L".git") == 0) continue;

        std::error_code cec;
        if (it->is_directory(cec))
        {
            fs::create_directories(dest / rel, cec);
            continue;
        }
        fs::create_directories((dest / rel).parent_path(), cec);
        fs::copy_file(it->path(), dest / rel, fs::copy_options::overwrite_existing, cec);
        if (cec) ++failed;
        else ++files;
    }

    if (files == 0)
    {
        fs::remove_all(dest, ec);
        message = L"Nothing was copied from the project folder.";
        return false;
    }
    if (failed > 0)
    {
        message = L"Template \"" + entry.name + L"\" created, but " + std::to_wstring(failed) +
                  L" file(s) could not be copied.";
        return false;
    }
    message = L"Created template \"" + entry.name + L"\" (" + std::to_wstring(files) + L" files) in " +
              templatesDir.wstring() + L".";
    return true;
}

bool ProjectStore::LaunchEntry(const ProjectEntry& entry, std::wstring& error)
{
    if (!entry.valid || entry.configFile.empty())
    {
        error = L"This project has no readable .zproject file.";
        return false;
    }
    auto editor = LocateEditor();
    if (!editor)
    {
        error = L"Could not find zEngine.exe. Put the launcher next to the editor, or create "
                L"%LOCALAPPDATA%\\zLauncher\\editor_path.txt containing its full path.";
        return false;
    }

    // The editor restores its most recently opened project on startup. Writing
    // that state here is the launcher's whole side of "open this project".
    std::error_code ec;
    const fs::path configAbs = fs::absolute(entry.configFile, ec);
    std::ostringstream state;
    state << "ZENGINE_EDITOR 1\nproject " << std::quoted(Utf8(configAbs.wstring())) << '\n';
    try
    {
        WriteTextFileAtomic(LocalAppData() / L"zEngine" / L"editor.state", state.str());
    }
    catch (const std::exception& e)
    {
        error = Wide(e.what());
        return false;
    }

    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", editor->c_str(), nullptr,
                      editor->parent_path().c_str(), SW_SHOWNORMAL));
    if (result <= 32)
    {
        error = L"Could not start the editor at " + editor->wstring() + L".";
        return false;
    }
    return true;
}

bool ProjectStore::LaunchEditor(const ProjectEntry& entry, std::wstring& error) const
{
    return LaunchEntry(entry, error);
}

bool ProjectStore::LaunchProjectAt(const fs::path& folder, std::wstring& error)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
    {
        error = L"That project folder no longer exists.";
        return false;
    }
    return LaunchEntry(Resolve(folder), error);
}

#ifndef ZLAUNCHER_BUILD_NUMBER
#define ZLAUNCHER_BUILD_NUMBER 0
#endif
#ifndef ZLAUNCHER_BUILD_DATE
#define ZLAUNCHER_BUILD_DATE ""
#endif

int ProjectStore::LauncherBuildNumber() { return ZLAUNCHER_BUILD_NUMBER; }

std::wstring ProjectStore::LauncherBuildDate()
{
    return Wide(std::string(ZLAUNCHER_BUILD_DATE));
}

namespace
{
// If a previous update staged a new helper (it cannot overwrite itself while
// running), put it in place now.
void SwapStagedUpdater()
{
    std::error_code ec;
    const fs::path updater = ModuleDirectory() / L"zLauncherUpdate.exe";
    const fs::path staged = fs::path(updater.wstring() + L".new");
    if (!fs::exists(staged, ec)) return;
    fs::remove(updater, ec);
    fs::rename(staged, updater, ec);
}
}

bool ProjectStore::LauncherUpdate(std::wstring& message, bool& started) const
{
    started = false;
    SwapStagedUpdater();

    std::string owner = "CoolBeanGames";
    std::string repo = "zEngine";
    if (auto text = ReadTextFile(LocalAppData() / L"zLauncher" / L"repo.txt", 4096))
    {
        std::string line = *text;
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (const auto end = line.find_first_of(" \t\r\n"); end != std::string::npos) line.erase(end);
        if (const auto slash = line.find('/'); slash != std::string::npos && slash + 1 < line.size())
        {
            owner = line.substr(0, slash);
            repo = line.substr(slash + 1);
        }
    }

    std::string json;
    std::wstring error;
    if (!HttpGet(L"https://api.github.com/repos/" + Wide(owner) + L"/" + Wide(repo) + L"/releases",
                 json, error))
    {
        message = error;
        return false;
    }

    int number = -1;
    std::string url;
    if (!HighestNumberedAsset(json, "launcher", number, url) || number <= LauncherBuildNumber())
    {
        message = L"The launcher is up to date (build " + std::to_wstring(LauncherBuildNumber()) + L").";
        return true; // no error, just nothing to do
    }

    const fs::path updater = ModuleDirectory() / L"zLauncherUpdate.exe";
    std::error_code ec;
    if (!fs::is_regular_file(updater, ec))
    {
        message = L"A launcher update (build " + std::to_wstring(number) +
                  L") is available, but the updater helper zLauncherUpdate.exe is missing.";
        return false;
    }

    const std::wstring command =
        L"\"" + updater.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --url \"" + Wide(url) + L"\" --dir \"" + ModuleDirectory().wstring() + L"\" --exe \"" +
        ModuleFilePath().wstring() + L"\"";
    std::wstring mutableCommand = command;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi))
    {
        message = L"Could not start the launcher updater.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    started = true;
    message = L"Updating the launcher to build " + std::to_wstring(number) + L"…";
    return true;
}

void ProjectStore::RunFirstTimeSetup()
{
    // The association points at whichever exe is running now, so keep it current
    // on every start (cheap and idempotent); the one-time work is guarded below.
    RegisterFileAssociation();
    SwapStagedUpdater();

    const fs::path marker = LocalAppData() / L"zLauncher" / L"setup.done";
    std::error_code ec;
    if (fs::exists(marker, ec)) return;

    // Back-fill a sidecar into every already-registered project that lacks one.
    for (auto& entry : projects_)
    {
        if (ReadSidecar(entry.folder)) continue;
        Sidecar sidecar;
        sidecar.name = entry.name;
        sidecar.created = entry.created.empty() ? NowStamp() : entry.created;
        try
        {
            WriteSidecar(entry.folder, sidecar);
            if (auto written = ReadSidecar(entry.folder))
            {
                entry.version = written->version;
                entry.created = written->created;
                entry.launched = written->launched;
            }
        }
        catch (const std::exception&)
        {
        }
    }

    try
    {
        WriteTextFileAtomic(marker, "zLauncher first-run setup complete\n");
    }
    catch (const std::exception&)
    {
    }
}

std::optional<fs::path> ProjectStore::PlayableExe(const ProjectEntry& entry)
{
    const fs::path dir = entry.folder / L"builds" / L"current";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;

    fs::path preferred = dir / (entry.name + L".exe");
    if (fs::is_regular_file(preferred, ec)) return preferred;

    fs::path first;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (e.is_regular_file() && _wcsicmp(e.path().extension().c_str(), L".exe") == 0)
        {
            if (first.empty()) first = e.path();
        }
    }
    if (!first.empty()) return first;
    return std::nullopt;
}

bool ProjectStore::PlayProject(const ProjectEntry& entry, std::wstring& error)
{
    auto exe = PlayableExe(entry);
    if (!exe)
    {
        error = L"No build to run. Expected an executable in " +
                (entry.folder / L"builds" / L"current").wstring() + L".";
        return false;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", exe->c_str(), nullptr, exe->parent_path().c_str(),
                      SW_SHOWNORMAL));
    if (result <= 32)
    {
        error = L"Could not start " + exe->wstring() + L".";
        return false;
    }
    return true;
}

fs::path ProjectStore::EngineVersionsRoot() { return EngineVersionsRootImpl(); }

// Which editor we manage, and its build number. Prefers the flat Program Files
// install, then the newest per-user download.
std::optional<fs::path> ProjectStore::InstalledEditorDir(int* outNumber)
{
    if (outNumber) *outNumber = -1;
    std::error_code ec;
    const fs::path pf = ProgramFilesEditorDirImpl();
    if (fs::is_regular_file(pf / L"zEngine.exe", ec))
    {
        if (outNumber) *outNumber = NumberFromVersionFile(pf);
        return pf;
    }
    if (auto newest = NewestEditorUnder(EditorDownloadsRootImpl(), outNumber))
        return newest->parent_path();
    return std::nullopt;
}

bool ProjectStore::EnsureEditorInstalled(std::wstring& message, bool& changed, bool allowDownload) const
{
    changed = false;
    int installed = -1;
    auto currentDir = InstalledEditorDir(&installed);

    std::string owner;
    std::string repo;
    ResolveRepo(owner, repo);

    std::string json;
    std::wstring error;
    const bool online = HttpGet(
        L"https://api.github.com/repos/" + Wide(owner) + L"/" + Wide(repo) + L"/releases", json, error);

    int remote = -1;
    std::string url;
    if (online)
        HighestNumberedAsset(json, "engine", remote, url);

    // Download the release and install it: try the flat Program Files location
    // first, and fall back to a per-user "Downloads\<n>" folder when that copy is
    // refused (no elevation). Returns false + `message` on real failure.
    auto install = [&](int number) -> bool {
        fs::path source;
        if (!DownloadAndUnzip(Wide(url), source, message)) return false;

        const fs::path pf = ProgramFilesEditorDirImpl();
        std::wstring pfError;
        fs::path dest;
        if (CopyTreeInto(source, pf, pfError))
        {
            dest = pf;
        }
        else
        {
            const fs::path perUser = EditorDownloadsRootImpl() / std::to_wstring(number);
            std::error_code ec;
            fs::remove_all(perUser, ec);
            if (!CopyTreeInto(source, perUser, message))
            {
                std::error_code cec;
                fs::remove_all(EditorDownloadWorkDir(), cec);
                return false;
            }
            dest = perUser;
        }

        std::error_code cec;
        fs::remove_all(EditorDownloadWorkDir(), cec);

        try { WriteTextFileAtomic(dest / L"version.txt", "engine_" + std::to_string(number)); }
        catch (const std::exception&) {}
        RefreshEditorShortcuts(dest / L"zEngine.exe");
        changed = true;
        message = L"Installed the zEngine editor (build " + std::to_wstring(number) + L") to " +
                  dest.wstring() + L".";
        return true;
    };

    // Already have an editor: keep it current.
    if (currentDir)
    {
        if (online && !url.empty() && remote > installed)
        {
            if (!allowDownload)
            {
                RefreshEditorShortcuts(*currentDir / L"zEngine.exe");
                message = L"A newer editor (build " + std::to_wstring(remote) +
                          L") is available - use Update to install it.";
                return true;
            }
            if (!install(remote)) return false;
            message = L"Editor updated to build " + std::to_wstring(remote) + L".";
            return true;
        }

        RefreshEditorShortcuts(*currentDir / L"zEngine.exe");
        if (!online)
            message = L"Editor update check skipped: " + error;
        else if (installed >= 0)
            message = L"The editor is up to date (build " + std::to_wstring(installed) + L").";
        else
            message = L"The editor is up to date.";
        return true;
    }

    // Nothing installed yet.
    if (!allowDownload)
    {
        message = L"No zEngine editor is installed.";
        return false;
    }
    if (!online)
    {
        message = L"Cannot reach GitHub to download the editor: " + error;
        return false;
    }
    if (url.empty())
    {
        message = online ? L"No \"engine_<n>.zip\" asset was found on the latest GitHub releases."
                         : L"No downloadable editor release was found on GitHub.";
        return false;
    }

    return install(remote > 0 ? remote : 1);
}

bool ProjectStore::BuildProject(const ProjectEntry&, std::wstring& error) const
{
    error = L"Building a project without opening the editor is not available yet. "
            L"Open the project and use File ▸ Build Standalone Game…";
    return false;
}

bool ProjectStore::UpdateEditor(std::wstring& message) const
{
    auto editor = LocateEditor();
    if (!editor)
    {
        message = L"Could not find the editor to update. Set %LOCALAPPDATA%\\zLauncher\\editor_path.txt.";
        return false;
    }
    const fs::path editorDir = editor->parent_path();

    // Do not overwrite a development build tree.
    for (fs::path p = editorDir; !p.empty() && p != p.root_path(); p = p.parent_path())
        if (_wcsicmp(p.filename().c_str(), L"builds") == 0 || _wcsicmp(p.filename().c_str(), L"build") == 0)
        {
            message = L"The editor is running from a build tree (" + editorDir.wstring() +
                      L"); refusing to overwrite it. Point the launcher at an installed editor first.";
            return false;
        }

    // Which repo. Overridable via <launcher data>/repo.txt ("owner/repo").
    std::string owner = "CoolBeanGames";
    std::string repo = "zEngine";
    if (auto text = ReadTextFile(LocalAppData() / L"zLauncher" / L"repo.txt", 4096))
    {
        std::string line = *text;
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (const auto end = line.find_first_of(" \t\r\n"); end != std::string::npos) line.erase(end);
        if (const auto slash = line.find('/'); slash != std::string::npos && slash + 1 < line.size())
        {
            owner = line.substr(0, slash);
            repo = line.substr(slash + 1);
        }
    }

    std::string json;
    std::wstring error;
    if (!HttpGet(L"https://api.github.com/repos/" + Wide(owner) + L"/" + Wide(repo) + L"/releases/latest",
                 json, error))
    {
        message = error;
        return false;
    }

    std::wstring tag = Wide(JsonString(json, "tag_name"));

    // Prefer the "engine_<n>.zip" release asset (n becomes the recorded version),
    // then any .zip asset, then the source zipball.
    std::string archiveUrl;
    int engineNumber = -1;
    if (HighestNumberedAsset(json, "engine", engineNumber, archiveUrl))
        tag = L"engine_" + std::to_wstring(engineNumber);
    if (archiveUrl.empty()) archiveUrl = FirstZipAssetUrl(json);
    if (archiveUrl.empty()) archiveUrl = JsonString(json, "zipball_url");
    if (archiveUrl.empty())
    {
        message = L"The latest release has no downloadable archive.";
        return false;
    }

    std::string archiveBytes;
    if (!HttpGet(Wide(archiveUrl), archiveBytes, error))
    {
        message = L"Could not download the release: " + error;
        return false;
    }

    std::error_code ec;
    const fs::path work = LocalAppData() / L"zLauncher" / L"update";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path zipPath = work / L"release.zip";
    {
        std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
        out.write(archiveBytes.data(), static_cast<std::streamsize>(archiveBytes.size()));
        if (!out)
        {
            message = L"Could not write the downloaded archive to disk.";
            return false;
        }
    }

    const fs::path extractDir = work / L"extracted";
    fs::create_directories(extractDir, ec);
    wchar_t systemDir[MAX_PATH]{};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring command = L"\"" + std::wstring(systemDir) + L"\\tar.exe\" -xf \"" +
                                 zipPath.wstring() + L"\" -C \"" + extractDir.wstring() + L"\"";
    DWORD exitCode = 0;
    if (!RunAndWait(command, 180000, exitCode))
    {
        message = L"Could not unzip the release (tar exit " + std::to_wstring(exitCode) + L").";
        return false;
    }

    // GitHub source archives wrap everything in a single top-level folder.
    fs::path source = extractDir;
    {
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(extractDir, ec)) entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(entries.front(), ec)) source = entries.front();
    }

    int replaced = 0;
    int failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(source, ec))
    {
        const fs::path relative = fs::relative(e.path(), source, ec);
        if (relative.empty()) continue;
        const fs::path destination = editorDir / relative;
        std::error_code cec;
        if (e.is_directory())
        {
            fs::create_directories(destination, cec);
            continue;
        }
        fs::create_directories(destination.parent_path(), cec);
        fs::copy_file(e.path(), destination, fs::copy_options::overwrite_existing, cec);
        if (cec) ++failed;
        else ++replaced;
    }

    if (!tag.empty())
    {
        try { WriteTextFileAtomic(editorDir / L"version.txt", Utf8(tag)); }
        catch (const std::exception&) {}
    }
    fs::remove_all(work, ec);

    const std::wstring what = tag.empty() ? L"the latest release" : tag;
    if (replaced == 0)
    {
        message = L"Update failed: no files could be replaced (is the editor open?).";
        return false;
    }
    if (failed > 0)
    {
        message = L"Updated to " + what + L", but " + std::to_wstring(failed) +
                  L" file(s) could not be replaced. Close the editor and update again.";
        return false;
    }
    message = L"Editor updated to " + what + L" (" + std::to_wstring(replaced) + L" files).";
    return true;
}
}
