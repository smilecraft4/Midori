# MidoriShaders.cmake
#
# Compiles HLSL offline with fxc and embeds the bytecode in the target, so core shaders are never
# compiled at runtime and the app does not ship or load d3dcompiler_47.dll.
#
#   midori_target_shaders(<target> <file>...)
#
# Files are relative to the current source dir, like target_sources. Shaders are named
# <name>.<stage>.hlsl with <stage> one of vs, ps, cs; the entry point is VSMain / PSMain / CSMain and
# the profile is <stage>_5_0. Each shader becomes one header holding one byte array:
#
#   data/win32/shaders/tile.vs.hlsl      -> #include "midori/shaders/tile_vs.h"      -> kTileVS
#   data/win32/shaders/brush_dab.cs.hlsl -> #include "midori/shaders/brush_dab_cs.h" -> kBrushDabCS
#
# The header is fxc's own /Fh output, `const BYTE kTileVS[] = {...};`, so include <windows.h> before
# it (the D3D11 backend already does) and take the size with sizeof(kTileVS).
#
# fxc does not report #include dependencies, so pass shared .hlsli files in the same call: every
# shader of the call is rebuilt when one of them changes.
#
# Headers are generated per configuration. Debug keeps debug info and skips optimisation so PIX and
# RenderDoc can show source; other configurations are optimised and stripped of reflection data.

function(midori_target_shaders target)
    if(NOT MIDORI_FXC)
        get_filename_component(kits_root
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
        file(GLOB fxc_dirs LIST_DIRECTORIES true "${kits_root}/bin/10.*/x64")
        list(SORT fxc_dirs COMPARE NATURAL ORDER DESCENDING)
    endif()
    find_program(MIDORI_FXC fxc HINTS ${fxc_dirs} REQUIRED)

    set(shaders "")
    set(includes "")
    foreach(input IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH input BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE path)
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "midori_target_shaders: '${input}' not found (looked for '${path}').")
        endif()
        if(path MATCHES "\\.hlsli$")
            list(APPEND includes "${path}")
        elseif(path MATCHES "\\.hlsl$")
            list(APPEND shaders "${path}")
        else()
            message(FATAL_ERROR "midori_target_shaders: '${input}' is neither .hlsl nor .hlsli.")
        endif()
    endforeach()

    set(root "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
    set(flags "$<IF:$<CONFIG:Debug>,/Zi;/Od,/O3;/Qstrip_reflect>")

    set(headers "")
    foreach(shader IN LISTS shaders)
        get_filename_component(stem "${shader}" NAME_WLE)
        if(NOT stem MATCHES "^([A-Za-z0-9_]+)\\.(vs|ps|cs)$")
            message(FATAL_ERROR "midori_target_shaders: '${shader}' must be named <name>.<vs|ps|cs>.hlsl.")
        endif()
        set(name "${CMAKE_MATCH_1}")
        set(stage "${CMAKE_MATCH_2}")
        string(TOUPPER "${stage}" stage_upper)

        # brush_dab -> BrushDab
        string(REPLACE "_" ";" words "${name}")
        set(symbol "k")
        foreach(word IN LISTS words)
            string(SUBSTRING "${word}" 0 1 head)
            string(SUBSTRING "${word}" 1 -1 tail)
            string(TOUPPER "${head}" head)
            string(APPEND symbol "${head}${tail}")
        endforeach()
        string(APPEND symbol "${stage_upper}")

        set(header "${root}/midori/shaders/${name}_${stage}.h")
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/midori/shaders"
            COMMAND "${MIDORI_FXC}" /nologo /WX /T "${stage}_5_0" /E "${stage_upper}Main" "${flags}"
                    /Vn "${symbol}" /Fh "${header}" "${shader}"
            DEPENDS "${shader}" ${includes}
            COMMENT "Compiling shader ${name}.${stage}.hlsl"
            VERBATIM
            COMMAND_EXPAND_LISTS
        )
        list(APPEND headers "${header}")
    endforeach()

    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PRIVATE "${root}")
endfunction()
