cmake_minimum_required(VERSION 3.21)

project(Midori
    VERSION 0.1.0
    DESCRIPTION "Infinite rasterized painting app"
    LANGUAGES CXX
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

# Tracy v0.14.1; the profiler GUI must be the same version. Configure with -DTRACY_ENABLE=OFF to
# compile it out. On demand: nothing is collected until a profiler connects, so an unconnected run
# costs almost nothing (but events from before the connection, allocations included, are missed).
# Localhost only: no listening socket on the network, and no firewall prompt.
set(TRACY_ENABLE ON CACHE BOOL "Enable the Tracy profiler client")
set(TRACY_ON_DEMAND ON)
set(TRACY_ONLY_LOCALHOST ON)
set(TRACY_STATIC ON)
add_subdirectory(vendors/tracy)

set(MIDORI_SOURCES
    src/app.cpp
    src/base.cpp
    src/color.cpp
)

set(MIDORI_SOURCES_WIN32
    src/win32/win32_base.cpp
    src/win32/win32_graphics.cpp
    src/win32/win32_input.cpp
    src/win32/win32_main.cpp
    src/win32/win32_platform.cpp
    src/win32/win32_wintab.cpp
)

# common.hlsli is listed so that editing it rebuilds every shader.
set(MIDORI_SHADERS_WIN32
    data/win32/shaders/common.hlsli
    data/win32/shaders/color.ps.hlsl
    data/win32/shaders/triangle.vs.hlsl
    data/win32/shaders/tile.vs.hlsl
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
    # Embedded by the linker: per-monitor DPI awareness and the UTF-8 code page.
    data/win32/midori.manifest
)

set_target_properties(midori PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

target_include_directories(midori PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# Wacom's Wintab headers only; Wintab32.dll is loaded at runtime, so there is nothing to link.
target_include_directories(midori SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/wintab)

# SPDLOG_ACTIVE_LEVEL is defined here rather than in log.hpp so the precompiled spdlog header is
# compiled with the same level as every source. The Windows defines make every translation unit see
# <windows.h> configured the same way.
target_compile_definitions(midori PRIVATE
    $<$<CONFIG:Debug>:MIDORI_DEBUG=1>
    MIDORI_PROFILE="$<CONFIG>"
    SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,SPDLOG_LEVEL_TRACE,SPDLOG_LEVEL_INFO>
    MIDORI_WINDOWS=1
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Warnings are errors. Exceptions are off for every target through the presets' CMAKE_CXX_FLAGS
# (no /EH flag, _HAS_EXCEPTIONS=0), as is AddressSanitizer in the msvc preset's Debug flags: both
# must match across all objects. clang's missing-field-initializers fires on Win32 SDK structs
# initialized with designated initializers, so it is off.
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

target_link_libraries(midori PRIVATE spdlog::spdlog Tracy::TracyClient d3d12 dxgi dxguid dcomp)

midori_target_shaders(midori ${MIDORI_SHADERS_WIN32})
