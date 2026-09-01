cmake_minimum_required(VERSION 3.24)

project(zEngine VERSION 0.1.0 LANGUAGES C CXX)
# Keep standalone games self-contained: no MSVC redistributable installation.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT zEngine)
include(CTest)
add_subdirectory(scripting)
add_library(zEngineInput STATIC src/input/InputMap.cpp)
target_compile_features(zEngineInput PUBLIC cxx_std_20)
target_include_directories(zEngineInput PUBLIC src)
add_library(zEngineInputTools STATIC src/input/InputAssets.cpp src/input/InputMapEditor.cpp)
target_link_libraries(zEngineInputTools PUBLIC zEngineInput PRIVATE comctl32)
target_compile_definitions(zEngineInputTools PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(zEngineCore STATIC src/core/GameObject.cpp src/core/BehaviorLifecycle.cpp)
add_library(zEngineGizmos STATIC src/TransformGizmo.cpp)
target_link_libraries(zEngineGizmos PUBLIC zEngineCore)
target_compile_features(zEngineCore PUBLIC cxx_std_20)
target_include_directories(zEngineCore PUBLIC src)

# Jolt is an implementation detail of zEnginePhysics. Keep the public engine API
# small and portable, and target SSE2 so games still run on older/low-spec PCs.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/JoltPhysics/Build EXCLUDE_FROM_ALL)
add_library(zEnginePhysics STATIC src/physics/PhysicsBehavior.cpp src/physics/PhysicsWorld.cpp)
target_compile_features(zEnginePhysics PUBLIC cxx_std_20)
target_include_directories(zEnginePhysics PUBLIC src)
target_link_libraries(zEnginePhysics PUBLIC zEngineCore PRIVATE Jolt)

add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC third_party/ufbx)

add_library(zEngineAssets STATIC src/FbxImporter.cpp src/FbxImporter.h src/ModelData.h)
target_compile_features(zEngineAssets PUBLIC cxx_std_20)
target_include_directories(zEngineAssets PUBLIC src)
target_link_libraries(zEngineAssets PRIVATE ufbx)

add_library(zEngineScriptTools STATIC src/ScriptAssets.cpp src/ScriptEditor.cpp src/ScriptCompletion.cpp)
target_compile_features(zEngineScriptTools PUBLIC cxx_std_20)
target_include_directories(zEngineScriptTools PUBLIC src)
target_compile_definitions(zEngineScriptTools PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(zEngineScriptTools PRIVATE comctl32 ole32)
target_link_libraries(zEngineScriptTools PRIVATE zEngineScripting)
target_compile_definitions(zEngineScriptTools PRIVATE ZENGINE_SCRIPT_COMPILER)
add_library(zEngineScriptHost STATIC src/ScriptHost.cpp)
target_link_libraries(zEngineScriptHost PUBLIC zEngineCore zEngineScripting zEnginePhysics)
target_compile_features(zEngineScriptHost PUBLIC cxx_std_20)
add_library(zEngineScenes STATIC src/Scene.cpp)
target_link_libraries(zEngineScenes PUBLIC zEngineCore zEngineScriptHost zEnginePhysics)
add_library(zEngineSceneAssets STATIC src/SceneAssets.cpp)
target_link_libraries(zEngineSceneAssets PUBLIC zEngineScenes)
target_compile_definitions(zEngineSceneAssets PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(zEnginePrefabs STATIC src/Prefab.cpp src/PrefabAssets.cpp)
target_link_libraries(zEnginePrefabs PUBLIC zEngineSceneAssets)
target_compile_definitions(zEnginePrefabs PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(zEngineProjects STATIC src/Project.cpp)
target_compile_features(zEngineProjects PUBLIC cxx_std_20)
target_include_directories(zEngineProjects PUBLIC src)
target_compile_definitions(zEngineProjects PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(zEngineProjects PRIVATE shell32 ole32)

add_library(zEngineGameRuntime STATIC src/runtime/GameSession.cpp src/runtime/GamePackage.cpp)
target_link_libraries(zEngineGameRuntime PUBLIC zEngineProjects zEnginePrefabs zEngineScriptTools zEngineInputTools zEngineAssets)
target_compile_definitions(zEngineGameRuntime PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_library(zEngineRenderer STATIC src/Renderer.cpp)
target_link_libraries(zEngineRenderer PUBLIC zEngineAssets zEngineCore zEngineGizmos d3d11 dxgi d3dcompiler windowscodecs ole32)
target_compile_definitions(zEngineRenderer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(zPlayer WIN32 src/runtime/PlayerMain.cpp)
target_compile_definitions(zPlayer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(zPlayer PRIVATE zEngineGameRuntime zEngineRenderer)
add_custom_command(TARGET zPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zPlayer>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorCube.hlsl" "$<TARGET_FILE_DIR:zPlayer>/shaders/ColorCube.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ufbx/LICENSE" "$<TARGET_FILE_DIR:zPlayer>/ufbx-LICENSE.txt")
add_custom_command(TARGET zPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/LICENSE" "$<TARGET_FILE_DIR:zPlayer>/Jolt-LICENSE.txt")

add_executable(zEngine WIN32
    src/Main.cpp
    src/EditorShell.cpp
    src/EditorProjects.cpp
    src/EditorGizmos.cpp
    src/EditorPrefabs.cpp
    src/EditorInput.cpp
    src/EditorChrome.cpp
    src/EditorFolders.cpp
    src/EditorBuild.cpp
    src/EditorHierarchy.cpp
    src/EditorShell.h
    src/InspectorPanel.cpp
    src/InspectorPanel.h
    src/ProjectDialog.cpp
    src/Renderer.h
)

target_compile_features(zEngine PRIVATE cxx_std_20)
set_target_properties(zEngine PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:zEngine>" VS_DEBUGGER_COMMAND "$<TARGET_FILE:zEngine>")
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
    zEngineGizmos
    comctl32
    comdlg32
    zEngineScriptTools
    zEngineScriptHost
    zEngineSceneAssets
    zEngineProjects
    zEnginePrefabs
    zEngineInputTools
    zEngineGameRuntime
    zEngineRenderer
)
add_dependencies(zEngine zPlayer)

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
    add_executable(zEnginePhysicsTests tests/PhysicsTests.cpp)
    target_link_libraries(zEnginePhysicsTests PRIVATE zEnginePhysics zEngineScenes)
    add_test(NAME PhysicsWorld COMMAND zEnginePhysicsTests)
    set_tests_properties(PhysicsWorld PROPERTIES TIMEOUT 60)
    add_executable(zEngineStyleTests tests/StyleTests.cpp)
    target_include_directories(zEngineStyleTests PRIVATE src)
    target_compile_features(zEngineStyleTests PRIVATE cxx_std_20)
    target_compile_definitions(zEngineStyleTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(zEngineStyleTests PRIVATE comctl32)
    add_test(NAME EditorButtonStyle COMMAND zEngineStyleTests)
    set_tests_properties(EditorButtonStyle PROPERTIES TIMEOUT 30)
    add_executable(zEngineInputTests tests/InputTests.cpp)
    target_link_libraries(zEngineInputTests PRIVATE zEngineInputTools zEngineScriptHost)
    target_compile_definitions(zEngineInputTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME InputMap COMMAND zEngineInputTests)
    set_tests_properties(InputMap PROPERTIES TIMEOUT 30)
    add_executable(zEngineSceneTests tests/SceneTests.cpp)
    target_link_libraries(zEngineSceneTests PRIVATE zEngineSceneAssets)
    target_compile_definitions(zEngineSceneTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME SceneAssets COMMAND zEngineSceneTests)
    set_tests_properties(SceneAssets PROPERTIES TIMEOUT 30)
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
    add_executable(zEngineAssetTests tests/AssetTests.cpp tests/ProjectTests.cpp tests/GizmoTests.cpp src/EditorShell.cpp src/EditorProjects.cpp src/EditorGizmos.cpp src/InspectorPanel.cpp src/ProjectDialog.cpp)
    target_link_libraries(zEngineAssetTests PRIVATE zEngineRenderer zEngineGameRuntime)
    target_sources(zEngineAssetTests PRIVATE src/EditorPrefabs.cpp tests/PrefabTests.cpp)
    target_sources(zEngineAssetTests PRIVATE src/EditorInput.cpp)
    target_sources(zEngineAssetTests PRIVATE src/EditorChrome.cpp)
    target_sources(zEngineAssetTests PRIVATE src/EditorFolders.cpp)
    target_sources(zEngineAssetTests PRIVATE src/EditorBuild.cpp)
    target_sources(zEngineAssetTests PRIVATE src/EditorHierarchy.cpp)
    target_link_libraries(zEngineAssetTests PRIVATE zEngineInputTools)
    target_link_libraries(zEngineAssetTests PRIVATE zEngineGizmos zEnginePrefabs)
    target_compile_features(zEngineAssetTests PRIVATE cxx_std_20)
    target_compile_definitions(zEngineAssetTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(zEngineAssetTests PRIVATE zEngineAssets zEngineCore zEngineScriptTools zEngineScriptHost zEngineSceneAssets zEngineProjects d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 comctl32 comdlg32)
    add_dependencies(zEngineAssetTests zEngine)
    add_test(NAME FbxImport COMMAND zEngineAssetTests)
    add_test(NAME RendererAlbedo COMMAND zEngineAssetTests --gpu)
    add_test(NAME EditorDragDrop COMMAND zEngineAssetTests --editor)
    add_test(NAME GameObjectInspector COMMAND zEngineAssetTests --objects)
    add_test(NAME MeshRendererBehavior COMMAND zEngineAssetTests --meshes)
    add_test(NAME ScriptEditorIntegration COMMAND zEngineAssetTests --scripts)
    add_test(NAME SceneEditorIntegration COMMAND zEngineAssetTests --scenes)
    add_test(NAME ProjectEditorIntegration COMMAND zEngineAssetTests --projects)
    add_test(NAME TransformGizmos COMMAND zEngineAssetTests --gizmos)
    add_test(NAME PrefabWorkflow COMMAND zEngineAssetTests --prefabs)
    add_test(NAME AssetFolders COMMAND zEngineAssetTests --folders)
    add_test(NAME GameObjectHierarchy COMMAND zEngineAssetTests --hierarchy)
    add_test(NAME MultilineTextInspector COMMAND zEngineAssetTests --text)
    set_tests_properties(MultilineTextInspector PROPERTIES TIMEOUT 45)
    set_tests_properties(GameObjectHierarchy PROPERTIES TIMEOUT 45)
    add_test(NAME StandaloneGameBuild COMMAND zEngineAssetTests --build)
    add_test(NAME StandaloneEditorBuild COMMAND zEngineAssetTests --editor-build)
    set_tests_properties(StandaloneEditorBuild PROPERTIES TIMEOUT 60)
    set_tests_properties(StandaloneGameBuild PROPERTIES TIMEOUT 120)
    set_tests_properties(AssetFolders PROPERTIES TIMEOUT 45)
    set_tests_properties(PrefabWorkflow PROPERTIES TIMEOUT 45)
    set_tests_properties(TransformGizmos PROPERTIES TIMEOUT 45)
    set_tests_properties(ProjectEditorIntegration PROPERTIES TIMEOUT 60)
    add_test(NAME ProjectSceneRecovery COMMAND zEngineAssetTests --project-recovery)
    add_test(NAME ProjectCreationDialog COMMAND zEngineAssetTests --project-dialog)
    add_test(NAME ProjectMissingRecovery COMMAND zEngineAssetTests --project-missing)
    set_tests_properties(ProjectSceneRecovery ProjectCreationDialog ProjectMissingRecovery PROPERTIES TIMEOUT 30)
    set_tests_properties(SceneEditorIntegration PROPERTIES TIMEOUT 60)
    set_tests_properties(ScriptEditorIntegration PROPERTIES TIMEOUT 60)
    set_tests_properties(GameObjectCore FbxImport RendererAlbedo EditorDragDrop GameObjectInspector MeshRendererBehavior PROPERTIES TIMEOUT 60)
endif()
