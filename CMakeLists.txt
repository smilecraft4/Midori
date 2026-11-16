cmake_minimum_required(VERSION 3.26)

project(Midori
    VERSION 0.2.5
    DESCRIPTION "Infinite rasterized painting app"
    LANGUAGES C CXX
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Midori targets Windows x64 with cl or clang-cl. No host fallback.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(MidoriShaders)
include(MidoriEmbed)

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

# The UI's text rasterizer: hinted glyphs, sharper than stb_truetype at small sizes. Only the TrueType/OpenType
# fonts the system has are read, so no optional codec.
set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BROTLI ON)
set(SKIP_INSTALL_ALL ON)
add_subdirectory(vendors/freetype EXCLUDE_FROM_ALL)

# Only the DX12 renderer backend is built: input comes from Midori's own events, not imgui_impl_win32.
add_library(imgui STATIC
    vendors/imgui/imgui.cpp
    vendors/imgui/imgui_demo.cpp
    vendors/imgui/imgui_draw.cpp
    vendors/imgui/imgui_tables.cpp
    vendors/imgui/imgui_widgets.cpp
    vendors/imgui/backends/imgui_impl_dx12.cpp
    vendors/imgui/misc/freetype/imgui_freetype.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendors/imgui/backends
)
# With FreeType enabled, every font atlas loads through it.
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_USE_WCHAR32
    IMGUI_ENABLE_FREETYPE
)
target_link_libraries(imgui PRIVATE d3d12 dxgi d3dcompiler freetype)

# Static: Windows Vista and later run its thread-local destructors without DllMain.
add_library(mdbx STATIC vendors/libmdbx/mdbx.c)
target_include_directories(mdbx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendors/libmdbx)
target_compile_definitions(mdbx PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(mdbx PRIVATE ntdll)

# Tiles are stored as QOI, in the canvas file and the cache. No stdio: only the in-memory calls are built.
add_library(qoi STATIC vendors/qoi/qoi.c)
target_include_directories(qoi SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendors/qoi)

set(MIDORI_SOURCES
    src/action.cpp
    src/app.cpp
    src/base.cpp
    src/canvas.cpp
    src/color.cpp
    src/curve.cpp
    src/history.cpp
    src/job.cpp
    src/layer.cpp
    src/mdsl.cpp
    src/preferences.cpp
    src/qoi_buffer.cpp
    src/stroke.cpp
    src/tag.cpp
    src/text.cpp
    src/tile.cpp
    src/tool.cpp
    src/ui.cpp
    src/viewport.cpp
)

set(MIDORI_SOURCES_WIN32
    src/win32/win32_main.cpp
    src/win32/win32_platform.cpp
    src/win32/win32_graphics.cpp
    src/win32/win32_shader.cpp
)

set(MIDORI_SHADERS_WIN32
    data/win32/shaders/atlas.hlsli
    data/win32/shaders/blend.hlsli
    data/win32/shaders/brush.hlsli
    data/win32/shaders/common.hlsli
    data/win32/shaders/tile.vs.hlsl
    data/win32/shaders/tile.ps.hlsl
    data/win32/shaders/brush_outline.hlsli
    data/win32/shaders/brush_outline.vs.hlsl
    data/win32/shaders/brush_outline.ps.hlsl
    data/win32/shaders/tile_blend.cs.hlsl
    data/win32/shaders/tile_mip.cs.hlsl
    data/win32/shaders/stroke.cs.hlsl
    data/win32/shaders/transform.cs.hlsl
    data/win32/shaders/history.cs.hlsl
)

set(MIDORI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/data/win32/icon.ico")
set(MIDORI_RESOURCE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/win32_resource.h")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/data/win32/midori.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/midori.rc
    @ONLY
)

# Everything but the platform layer and the renderer, which the app adds and the tests and benchmark stand in for. What
# every Midori target shares comes with it: includes, definitions, warnings.
add_library(midori_core STATIC ${MIDORI_SOURCES})
target_include_directories(midori_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_include_directories(midori_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Preferences and translations are JSON. Exceptions are off: it aborts where it would throw.
target_include_directories(midori_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendors/json)
target_compile_definitions(midori_core PUBLIC
    JSON_NOEXCEPTION
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
target_compile_options(midori_core PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:preprocessor>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:-Wno-missing-field-initializers>"
)
target_precompile_headers(midori_core PRIVATE
    <spdlog/spdlog.h>
    <expected>
    <string_view>
)
target_link_libraries(midori_core PUBLIC spdlog::spdlog Tracy::TracyClient imgui mdbx qoi)
# round.mdsl, the built-in brush, painted with when brushes/round.mdsl cannot be. The tests and benchmark read it too.
midori_target_embed(midori_core data/brushes/round.mdsl)

enable_language(RC)
add_executable(midori WIN32
    ${MIDORI_SOURCES_WIN32}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/midori.rc
    data/win32/midori.manifest
)
target_include_directories(midori PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(midori SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/wintab)
target_include_directories(midori SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendors/DirectX-Headers/include)
target_precompile_headers(midori PRIVATE <spdlog/spdlog.h> <expected> <string_view>)
target_link_libraries(midori PRIVATE midori_core d3d12 dxgi dxguid dcomp d3dcompiler comctl32 shell32 ole32 windowscodecs)

midori_target_shaders(midori ${MIDORI_SHADERS_WIN32})

# Every brush is compiled at runtime into the stroke shader, so its source and includes are in the exe.
midori_target_embed(midori
    data/win32/shaders/stroke.cs.hlsl
    data/win32/shaders/atlas.hlsli
    data/win32/shaders/blend.hlsli
)

# Headless, on the core library: the platform functions its files call come from tests/headless.cpp. The tests run with
# `ctest`; the benchmark prints its timings.
add_executable(midori_tests
    tests/headless.cpp
    tests/test_main.cpp
    tests/test_color.cpp
    tests/test_curve.cpp
    tests/test_fuzzy.cpp
    tests/test_keymap.cpp
    tests/test_mdsl.cpp
    tests/test_tile.cpp
    tests/test_viewport.cpp
)
target_link_libraries(midori_tests PRIVATE midori_core)

add_executable(midori_bench
    tests/headless.cpp
    bench/bench_main.cpp
)
target_link_libraries(midori_bench PRIVATE midori_core)

enable_testing()
add_test(NAME midori_tests COMMAND midori_tests)

set_target_properties(midori midori_tests midori_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

# The ASan runtime lives next to cl.exe, only on PATH in a VS developer prompt: ship it next to the exe. All three
# executables share one folder.
string(FIND "${CMAKE_CXX_FLAGS_DEBUG}" "/fsanitize=address" MIDORI_ASAN_FLAG)
if(NOT MIDORI_ASAN_FLAG EQUAL -1)
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH MIDORI_COMPILER_DIR)
    add_custom_command(TARGET midori POST_BUILD
        COMMAND "$<$<CONFIG:Debug>:${CMAKE_COMMAND};-E;copy_if_different;${MIDORI_COMPILER_DIR}/clang_rt.asan_dynamic-x86_64.dll;$<TARGET_FILE_DIR:midori>>"
        COMMAND_EXPAND_LISTS
    )
    add_dependencies(midori_tests midori)
    add_dependencies(midori_bench midori)
endif()

# Translations are read from lang/ next to the exe, and brushes from brushes/, round.mdsl there the one painted with at
# start.
add_custom_command(TARGET midori POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/lang $<TARGET_FILE_DIR:midori>/lang
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/brushes $<TARGET_FILE_DIR:midori>/brushes
)
