#pragma once
// ZE-127: last-resort crash diagnostics for the editor and the standalone player.
// Installs an unhandled-SEH filter and a std::terminate handler that write a text
// report (and a best-effort minidump) under %LOCALAPPDATA%\zEngine\crashes, plus a
// small breadcrumb trail so the report says what the app was doing.

#include <string>
#include <string_view>

namespace zengine::crash
{
    // Call once, as the very first thing in wWinMain. `appName` tags the files
    // ("zEngine" / the game name). `buildId` is free-form (e.g. __DATE__ " " __TIME__).
    void Install(std::string_view appName, std::string_view buildId);

    // Record a short "what just happened" note (kept in a small ring buffer).
    void Breadcrumb(std::string_view note);

    // Write a crash report for a fatal error that WAS caught (e.g. a std::exception
    // that escaped to wWinMain). Returns the report path. Also arms the "previous
    // crash" marker so the next launch surfaces it.
    std::string ReportHandledFatal(std::string_view detail);

    // Full path of the most recent crash report from a PREVIOUS run, or "" if none.
    // Clears the pending marker so it is reported only once.
    std::string TakePreviousCrashReport();

    // Directory the reports are written to (created on demand).
    std::string ReportDirectory();
}
