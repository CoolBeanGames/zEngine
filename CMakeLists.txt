cmake_minimum_required(VERSION 3.24)

project(zEngine VERSION 0.1.0 LANGUAGES C CXX)
include(CTest)
add_subdirectory(scripting)

add_library(zEngineCore STATIC src/core/GameObject.cpp src/core/BehaviorLifecycle.cpp)
target_compile_features(zEngineCore PUBLIC cxx_std_20)
target_include_directories(zEngineCore PUBLIC src)

add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC third_party/ufbx)

add_library(zEngineAssets STATIC src/FbxImporter.cpp src/FbxImporter.h src/ModelData.h)
target_compile_features(zEngineAssets PUBLIC cxx_std_20)
target_include_directories(zEngineAssets PUBLIC src)
target_link_libraries(zEngineAssets PRIVATE ufbx)

add_library(zEngineScriptTools STATIC src/ScriptAssets.cpp src/ScriptEditor.cpp)
target_compile_features(zEngineScriptTools PUBLIC cxx_std_20)
target_include_directories(zEngineScriptTools PUBLIC src)
target_compile_definitions(zEngineScriptTools PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(zEngineScriptTools PRIVATE comctl32 ole32)
target_link_libraries(zEngineScriptTools PRIVATE zEngineScripting)
target_compile_definitions(zEngineScriptTools PRIVATE ZENGINE_SCRIPT_COMPILER)
add_library(zEngineScriptHost STATIC src/ScriptHost.cpp)
target_link_libraries(zEngineScriptHost PUBLIC zEngineCore zEngineScripting)
target_compile_features(zEngineScriptHost PUBLIC cxx_std_20)

add_executable(zEngine WIN32
    src/Main.cpp
    src/EditorShell.cpp
    src/EditorShell.h
    src/InspectorPanel.cpp
    src/InspectorPanel.h
    src/Renderer.cpp
    src/Renderer.h
)

target_compile_features(zEngine PRIVATE cxx_std_20)
target_compile_definitions(zEngine PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(zEngine PRIVATE
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    ole32
    shell32
    zEngineAssets
    zEngineCore
    comctl32
    comdlg32
    zEngineScriptTools
    zEngineScriptHost
)

if(MSVC)
    target_compile_options(zEngine PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(zEngineCore PRIVATE /W4 /permissive- /EHsc)
endif()

add_custom_command(TARGET zEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zEngine>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorCube.hlsl"
            "$<TARGET_FILE_DIR:zEngine>/shaders/ColorCube.hlsl"
    COMMENT "Copying shaders"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(zEngineScriptHostTests tests/ScriptHostTests.cpp)
    target_link_libraries(zEngineScriptHostTests PRIVATE zEngineScriptHost)
    add_test(NAME ScriptHostIntegration COMMAND zEngineScriptHostTests)
    set_tests_properties(ScriptHostIntegration PROPERTIES TIMEOUT 30)
    add_executable(zEngineLifecycleTests tests/LifecycleTests.cpp)
    target_link_libraries(zEngineLifecycleTests PRIVATE zEngineCore)
    add_test(NAME BehaviorLifecycle COMMAND zEngineLifecycleTests)
    set_tests_properties(BehaviorLifecycle PROPERTIES TIMEOUT 30)
    add_executable(zEngineScriptEditorTests tests/ScriptEditorTests.cpp)
    target_link_libraries(zEngineScriptEditorTests PRIVATE zEngineScriptTools zEngineCore)
    target_compile_definitions(zEngineScriptEditorTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME ScriptAssetsAndEditor COMMAND zEngineScriptEditorTests)
    set_tests_properties(ScriptAssetsAndEditor PROPERTIES TIMEOUT 60)
    add_executable(zEngineCoreTests tests/CoreTests.cpp)
    target_link_libraries(zEngineCoreTests PRIVATE zEngineCore)
    add_test(NAME GameObjectCore COMMAND zEngineCoreTests)
    add_executable(zEngineAssetTests tests/AssetTests.cpp src/Renderer.cpp src/EditorShell.cpp src/InspectorPanel.cpp)
    target_compile_features(zEngineAssetTests PRIVATE cxx_std_20)
    target_compile_definitions(zEngineAssetTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(zEngineAssetTests PRIVATE zEngineAssets zEngineCore zEngineScriptTools zEngineScriptHost d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 comctl32 comdlg32)
    add_dependencies(zEngineAssetTests zEngine)
    add_test(NAME FbxImport COMMAND zEngineAssetTests)
    add_test(NAME RendererAlbedo COMMAND zEngineAssetTests --gpu)
    add_test(NAME EditorDragDrop COMMAND zEngineAssetTests --editor)
    add_test(NAME GameObjectInspector COMMAND zEngineAssetTests --objects)
    add_test(NAME MeshRendererBehavior COMMAND zEngineAssetTests --meshes)
    add_test(NAME ScriptEditorIntegration COMMAND zEngineAssetTests --scripts)
    set_tests_properties(ScriptEditorIntegration PROPERTIES TIMEOUT 60)
    set_tests_properties(GameObjectCore FbxImport RendererAlbedo EditorDragDrop GameObjectInspector MeshRendererBehavior PROPERTIES TIMEOUT 60)
endif()
