cmake_minimum_required(VERSION 3.24)

# zLauncher is the standalone entry point to zEngine. It is intentionally a
# separate solution: it never links the engine and never builds from the engine
# tree. Configure it on its own, e.g.
#   cmake -S launcher -B builds/launcher
#   cmake --build builds/launcher --config Release
project(zLauncher VERSION 0.1.0 LANGUAGES CXX)

# Match the engine: static CRT so the launcher is a single self-contained exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The build number is baked into the exe. A launcher release is published as
# "launcher_<number>.zip"; the running launcher offers to self-update when it
# sees a release asset whose number is higher than its own. Bump this when
# cutting a release. The build date is kept for display only.
set(ZLAUNCHER_BUILD_NUMBER "6" CACHE STRING "Monotonic launcher release number")
string(TIMESTAMP ZLAUNCHER_BUILD_DATE "%Y-%m-%d" UTC)

add_executable(zLauncher WIN32
    src/Main.cpp
    src/LauncherWindow.cpp
    src/ProjectStore.cpp
    src/NewProjectDialog.cpp
    src/LauncherWindow.h
    src/ProjectStore.h
    src/NewProjectDialog.h
    src/Style.h
    src/Resource.h
    src/zLauncher.rc
)

target_compile_features(zLauncher PRIVATE cxx_std_20)
target_compile_definitions(zLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    ZLAUNCHER_VERSION="${PROJECT_VERSION}" ZLAUNCHER_BUILD_DATE="${ZLAUNCHER_BUILD_DATE}"
    ZLAUNCHER_BUILD_NUMBER=${ZLAUNCHER_BUILD_NUMBER})
target_link_libraries(zLauncher PRIVATE comctl32 shell32 ole32 shlwapi advapi32 version winhttp)

# Tiny helper that swaps the launcher exe while the launcher is closed, then
# relaunches it. The launcher spawns this and exits when a newer build is found.
add_executable(zLauncherUpdate WIN32 updater/Main.cpp src/zLauncher.rc)
target_compile_features(zLauncherUpdate PRIVATE cxx_std_20)
target_compile_definitions(zLauncherUpdate PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(zLauncherUpdate PRIVATE shell32 winhttp)
if(MSVC)
    target_compile_options(zLauncherUpdate PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(zLauncher PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:zLauncher>"
    VS_DEBUGGER_COMMAND "$<TARGET_FILE:zLauncher>")

if(MSVC)
    target_compile_options(zLauncher PRIVATE /W4 /permissive- /EHsc /utf-8)
    # Comctl32 v6 for themed common controls and window subclassing.
    target_link_options(zLauncher PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()
