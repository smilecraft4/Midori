cmake_minimum_required(VERSION 3.21)

project(Midori
    VERSION 0.1.0
    DESCRIPTION "Infinite rasterized painting app"
    LANGUAGES C CXX
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Midori targets Windows x64 with cl or clang-cl. No host fallback.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(MidoriShaders)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MIDORI_COMPANY "smilecraft4")
set(MIDORI_COPYRIGHT "Copyright (C) 2025")
set(MIDORI_EXECUTABLE "midori.exe")
string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" MIDORI_COMPILER)
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" MIDORI_ARCHITECTURE)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/midori/version.h
    @ONLY
)

set(SPDLOG_SYSTEM_INCLUDES ON)
set(SPDLOG_NO_EXCEPTIONS ON)
add_subdirectory(vendors/spdlog)

set(TRACY_ENABLE ON)
set(TRACY_ON_DEMAND ON)
set(TRACY_ONLY_LOCALHOST ON)
set(TRACY_STATIC ON)
add_subdirectory(vendors/tracy)

# Only the DX12 renderer backend is built: input comes from Midori's own events, not imgui_impl_win32.
add_library(imgui STATIC
    vendors/imgui/imgui.cpp
    vendors/imgui/imgui_demo.cpp
    vendors/imgui/imgui_draw.cpp
    vendors/imgui/imgui_tables.cpp
    vendors/imgui/imgui_widgets.cpp
    vendors/imgui/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui/backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_USE_WCHAR32
)
target_link_libraries(imgui PRIVATE d3d12 dxgi d3dcompiler)

# Static: Windows Vista and later run its thread-local destructors without DllMain.
add_library(mdbx STATIC vendors/libmdbx/mdbx.c)
target_include_directories(mdbx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendors/libmdbx)
target_compile_definitions(mdbx PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(mdbx PRIVATE ntdll)

set(MIDORI_SOURCES
    src/app.cpp
    src/base.cpp
    src/canvas.cpp
    src/color.cpp
    src/job.cpp
    src/stroke.cpp
    src/tile.cpp
    src/ui.cpp
    src/viewport.cpp
)

set(MIDORI_SOURCES_WIN32
    src/win32/win32_main.cpp
    src/win32/win32_platform.cpp
    src/win32/win32_graphics.cpp
)

set(MIDORI_SHADERS_WIN32
    data/win32/shaders/atlas.hlsli
    data/win32/shaders/blend.hlsli
    data/win32/shaders/common.hlsli
    data/win32/shaders/tile_ops.hlsli
    data/win32/shaders/tile.vs.hlsl
    data/win32/shaders/tile.ps.hlsl
    data/win32/shaders/tile_blend.cs.hlsl
    data/win32/shaders/tile_mip.cs.hlsl
    data/win32/shaders/stroke.cs.hlsl
)

set(MIDORI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/data/win32/icon.ico")
set(MIDORI_RESOURCE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/win32_resource.h")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/data/win32/midori.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/midori.rc
    @ONLY
)

enable_language(RC)
add_executable(midori WIN32
    ${MIDORI_SOURCES}
    ${MIDORI_SOURCES_WIN32}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/midori.rc
    data/win32/midori.manifest
)

set_target_properties(midori PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

# The ASan runtime lives next to cl.exe, only on PATH in a VS developer prompt: ship it next to the exe.
string(FIND "${CMAKE_CXX_FLAGS_DEBUG}" "/fsanitize=address" MIDORI_ASAN_FLAG)
if(NOT MIDORI_ASAN_FLAG EQUAL -1)
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH MIDORI_COMPILER_DIR)
    add_custom_command(TARGET midori POST_BUILD
        COMMAND "$<$<CONFIG:Debug>:${CMAKE_COMMAND};-E;copy_if_different;${MIDORI_COMPILER_DIR}/clang_rt.asan_dynamic-x86_64.dll;$<TARGET_FILE_DIR:midori>>"
        COMMAND_EXPAND_LISTS
    )
endif()

target_include_directories(midori PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_include_directories(midori SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/wintab)
target_include_directories(midori SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/DirectX-Headers/include)

target_compile_definitions(midori PRIVATE
    $<$<CONFIG:Debug>:MIDORI_DEBUG=1>
    MIDORI_PROFILE="$<CONFIG>"
    SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,SPDLOG_LEVEL_TRACE,SPDLOG_LEVEL_INFO>
    MIDORI_WINDOWS=1
)

target_compile_definitions(midori PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(midori PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:preprocessor>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:-Wno-missing-field-initializers>"
)

target_precompile_headers(midori PRIVATE
    <spdlog/spdlog.h>
    <expected>
    <string_view>
)

target_link_libraries(midori PRIVATE spdlog::spdlog Tracy::TracyClient imgui mdbx d3d12 dxgi dxguid dcomp comdlg32 comctl32 shell32)

midori_target_shaders(midori ${MIDORI_SHADERS_WIN32})
