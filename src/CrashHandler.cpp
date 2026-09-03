#include "CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace zengine::crash
{
    namespace
    {
        std::string g_appName = "zEngine";
        std::string g_buildId;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
        std::terminate_handler g_previousTerminate = nullptr;

        constexpr std::size_t kBreadcrumbs = 24;
        std::array<std::string, kBreadcrumbs> g_trail;
        std::atomic<std::size_t> g_trailNext{0};
        std::mutex g_trailMutex;

        std::string BaseDirectory()
        {
            PWSTR local = nullptr;
            std::wstring dir;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            {
                dir.assign(local);
                CoTaskMemFree(local);
            }
            if (dir.empty())
            {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(MAX_PATH, temp);
                dir = temp;
            }
            dir += L"\\zEngine\\crashes";
            // Narrow (UTF-8) for the public API; the path is ASCII in practice.
            const int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string out(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), n, nullptr, nullptr);
            return out;
        }

        std::wstring Widen(const std::string& s)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        }

        void EnsureDirectory(const std::wstring& dir)
        {
            std::wstring partial;
            for (wchar_t c : dir)
            {
                if (c == L'\\' || c == L'/')
                {
                    if (!partial.empty() && partial.back() != L':') CreateDirectoryW(partial.c_str(), nullptr);
                    partial += L'\\';
                }
                else partial += c;
            }
            CreateDirectoryW(partial.c_str(), nullptr);
        }

        std::string Timestamp()
        {
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            return buffer;
        }

        std::string ModuleAt(void* address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(address), &module) || !module)
                return "?";
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            const auto offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
            char out[MAX_PATH];
            std::snprintf(out, sizeof(out), "%ls+0x%llx", name, static_cast<unsigned long long>(offset));
            return out;
        }

        std::string WriteReport(const char* kind, const std::string& detail, EXCEPTION_POINTERS* seh)
        {
            const auto dirNarrow = BaseDirectory();
            const auto dir = Widen(dirNarrow);
            EnsureDirectory(dir);
            const auto stamp = Timestamp();
            const auto stem = dirNarrow + "\\" + g_appName + "-" + stamp;
            const auto txtPath = stem + ".txt";

            if (FILE* f = nullptr; _wfopen_s(&f, Widen(txtPath).c_str(), L"wb") == 0 && f)
            {
                std::fprintf(f, "zEngine crash report\n====================\n");
                std::fprintf(f, "app:      %s\n", g_appName.c_str());
                std::fprintf(f, "build:    %s\n", g_buildId.c_str());
                std::fprintf(f, "when:     %s (local)\n", stamp.c_str());
                std::fprintf(f, "kind:     %s\n", kind);
                std::fprintf(f, "thread:   %lu\n", GetCurrentThreadId());
                if (!detail.empty()) std::fprintf(f, "detail:   %s\n", detail.c_str());
                if (seh && seh->ExceptionRecord)
                {
                    const auto* rec = seh->ExceptionRecord;
                    std::fprintf(f, "code:     0x%08lx\n", rec->ExceptionCode);
                    std::fprintf(f, "at:       %s\n", ModuleAt(rec->ExceptionAddress).c_str());
                    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
                        std::fprintf(f, "access:   %s address 0x%llx\n",
                                     rec->ExceptionInformation[0] ? "write" : "read",
                                     static_cast<unsigned long long>(rec->ExceptionInformation[1]));
                }
                std::fprintf(f, "\nrecent activity (newest last):\n");
                {
                    std::lock_guard<std::mutex> lock(g_trailMutex);
                    const std::size_t next = g_trailNext.load();
                    for (std::size_t i = 0; i < kBreadcrumbs; ++i)
                    {
                        const auto& note = g_trail[(next + i) % kBreadcrumbs];
                        if (!note.empty()) std::fprintf(f, "  - %s\n", note.c_str());
                    }
                }
                std::fclose(f);
            }

            // Best-effort minidump (small: header + thread stacks).
            if (seh)
            {
                const auto dmpPath = Widen(stem + ".dmp");
                HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    MINIDUMP_EXCEPTION_INFORMATION info{};
                    info.ThreadId = GetCurrentThreadId();
                    info.ExceptionPointers = seh;
                    info.ClientPointers = FALSE;
                    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MiniDumpNormal, seh ? &info : nullptr, nullptr, nullptr);
                    CloseHandle(file);
                }
            }

            // Leave a marker so the next launch can point the user at it.
            if (FILE* f = nullptr; _wfopen_s(&f, Widen(dirNarrow + "\\last-crash.txt").c_str(), L"wb") == 0 && f)
            {
                std::fwrite(txtPath.data(), 1, txtPath.size(), f);
                std::fclose(f);
            }
            return txtPath;
        }

        LONG WINAPI SehFilter(EXCEPTION_POINTERS* pointers)
        {
            const auto path = WriteReport("unhandled exception (SEH)", {}, pointers);
            const auto message = Widen("zEngine has crashed.\n\nA crash report was written to:\n" + path);
            MessageBoxW(nullptr, message.c_str(), L"zEngine", MB_OK | MB_ICONERROR);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        void TerminateHandler()
        {
            std::string detail = "std::terminate";
            if (auto current = std::current_exception())
            {
                try { std::rethrow_exception(current); }
                catch (const std::exception& e) { detail = std::string("uncaught std::exception: ") + e.what(); }
                catch (...) { detail = "uncaught non-standard exception"; }
            }
            const auto path = WriteReport("std::terminate", detail, nullptr);
            const auto message = Widen("zEngine has crashed.\n\n" + detail + "\n\nReport: " + path);
            MessageBoxW(nullptr, message.c_str(), L"zEngine", MB_OK | MB_ICONERROR);
            if (g_previousTerminate) g_previousTerminate();
            _exit(3);
        }
    }

    void Install(std::string_view appName, std::string_view buildId)
    {
        if (!appName.empty()) g_appName.assign(appName);
        g_buildId.assign(buildId);
        g_previousFilter = SetUnhandledExceptionFilter(SehFilter);
        g_previousTerminate = std::set_terminate(TerminateHandler);
        Breadcrumb("crash handler installed");
    }

    std::string ReportHandledFatal(std::string_view detail)
    {
        return WriteReport("handled fatal error", std::string(detail), nullptr);
    }

    void Breadcrumb(std::string_view note)
    {
        std::lock_guard<std::mutex> lock(g_trailMutex);
        const std::size_t slot = g_trailNext.fetch_add(1) % kBreadcrumbs;
        g_trail[slot].assign(note);
    }

    std::string TakePreviousCrashReport()
    {
        const auto marker = Widen(BaseDirectory() + "\\last-crash.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, marker.c_str(), L"rb") != 0 || !f) return {};
        char buffer[1024]{};
        const auto read = std::fread(buffer, 1, sizeof(buffer) - 1, f);
        std::fclose(f);
        _wremove(marker.c_str());
        std::string path(buffer, read);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        if (FILE* check = nullptr; _wfopen_s(&check, Widen(path).c_str(), L"rb") == 0 && check) { std::fclose(check); return path; }
        return {};
    }

    std::string ReportDirectory() { return BaseDirectory(); }
}
