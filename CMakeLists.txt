cmake_minimum_required(VERSION 3.24)

project(zEngine VERSION 0.1.0 LANGUAGES CXX)

add_executable(zEngine WIN32
    src/Main.cpp
    src/EditorShell.cpp
    src/EditorShell.h
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
)

if(MSVC)
    target_compile_options(zEngine PRIVATE /W4 /permissive- /EHsc)
endif()

add_custom_command(TARGET zEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zEngine>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorCube.hlsl"
            "$<TARGET_FILE_DIR:zEngine>/shaders/ColorCube.hlsl"
    COMMENT "Copying shaders"
)
