# MidoriShaders.cmake
#
# Compiles HLSL offline to DXIL with dxc and embeds the bytecode in the target, so shaders are never
# compiled at runtime and the app ships no shader compiler.
#
#   midori_target_shaders(<target> <file>...)
#
# Files are relative to the current source dir, like target_sources. Shaders are named
# <name>.<stage>.hlsl with <stage> one of vs, ps, cs; the entry point is VSMain / PSMain / CSMain and
# the profile is <stage>_6_0. Each shader becomes one header holding one byte array, both named after
# the file with '.' replaced by '_' (so <name> must be a valid C identifier):
#
#   data/win32/shaders/tile.vs.hlsl      -> #include "midori/shaders/tile_vs.h"      -> tile_vs
#   data/win32/shaders/brush_dab.cs.hlsl -> #include "midori/shaders/brush_dab_cs.h" -> brush_dab_cs
#
# The header is dxc's own /Fh output, `const unsigned char tile_vs[] = {...};`; take the size with
# sizeof(tile_vs).
#
# The Windows SDK's dxc is preferred: dxil.dll sits next to it and signs the DXIL, which D3D12 requires
# outside of developer mode. dxc's -MD lists dependencies instead of compiling, so pass shared .hlsli
# files in the same call: every shader of the call is rebuilt when one of them changes.
#
# Headers are generated per configuration. Debug embeds debug info and skips optimisation so PIX and
# RenderDoc can show source; other configurations are optimised and stripped of reflection and debug
# data. Root signatures declared in HLSL are kept in every configuration.

function(midori_target_shaders target)
    if(NOT MIDORI_DXC)
        get_filename_component(kits_root
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
        file(GLOB dxc_dirs LIST_DIRECTORIES true "${kits_root}/bin/10.*/x64")
        list(SORT dxc_dirs COMPARE NATURAL ORDER DESCENDING)
    endif()
    # HINTS are searched before PATH, so a Vulkan SDK dxc (no dxil.dll, unsigned output) is not picked.
    find_program(MIDORI_DXC dxc HINTS ${dxc_dirs} REQUIRED)

    set(shaders "")
    set(includes "")
    foreach(input IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH input BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE path)
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "midori_target_shaders: '${input}' not found (looked for '${path}').")
        endif()
        cmake_path(GET path EXTENSION LAST_ONLY extension)
        if(extension STREQUAL ".hlsli")
            list(APPEND includes "${path}")
        elseif(extension STREQUAL ".hlsl")
            list(APPEND shaders "${path}")
        else()
            message(FATAL_ERROR "midori_target_shaders: '${input}' is neither .hlsl nor .hlsli.")
        endif()
    endforeach()

    set(root "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
    set(flags "$<IF:$<CONFIG:Debug>,-Zi;-Qembed_debug;-Od,-O3;-Qstrip_reflect;-Qstrip_debug>")

    set(stage_extensions .vs .ps .cs)
    set(headers "")
    foreach(shader IN LISTS shaders)
        # tile.vs.hlsl -> stem tile.vs, stage vs, name tile_vs
        cmake_path(GET shader STEM LAST_ONLY stem)
        cmake_path(GET stem EXTENSION LAST_ONLY stage_extension)
        if(NOT stage_extension IN_LIST stage_extensions)
            message(FATAL_ERROR "midori_target_shaders: '${shader}' must be named <name>.<vs|ps|cs>.hlsl.")
        endif()
        string(SUBSTRING "${stage_extension}" 1 -1 stage)
        string(TOUPPER "${stage}" stage_upper)
        string(REPLACE "." "_" name "${stem}")

        set(header "${root}/midori/shaders/${name}.h")
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/midori/shaders"
            COMMAND "${MIDORI_DXC}" -nologo -WX -T "${stage}_6_0" -E "${stage_upper}Main" "${flags}"
                    -Vn "${name}" -Fh "${header}" "${shader}"
            DEPENDS "${shader}" ${includes}
            COMMENT "Compiling shader ${stem}.hlsl"
            VERBATIM
            COMMAND_EXPAND_LISTS
        )
        list(APPEND headers "${header}")
    endforeach()

    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PRIVATE "${root}")
endfunction()
