# MidoriEmbed.cmake
#
# Embeds text files in the target, for what the app needs at runtime without shipping it as files.
#
#   midori_target_embed(<target> <file>...)
#
# Files are relative to the current source dir, like target_sources. Each becomes one header holding one
# `constexpr std::string_view`, both named after the file with '.' replaced by '_':
#
#   data/brushes/round.mdsl            -> #include "midori/embed/round_mdsl.h"     -> round_mdsl
#   data/win32/shaders/stroke.cs.hlsl  -> #include "midori/embed/stroke_cs_hlsl.h" -> stroke_cs_hlsl
#
# The header is written by this file run as a script, so nothing is compiled for it. MSVC limits each piece of a string
# literal to about 16 KB, so the text is cut at line ends into pieces it accepts.

if(CMAKE_SCRIPT_MODE_FILE)
    file(READ "${EMBED_INPUT}" text)
    string(FIND "${text}" ")midori_embed\"" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "${EMBED_INPUT} holds )midori_embed\", which would end the embedded text.")
    endif()
    set(pieces "")
    string(LENGTH "${text}" length)
    while(length GREATER 12000)
        string(SUBSTRING "${text}" 0 12000 head)
        string(FIND "${head}" "\n" cut REVERSE)
        if(cut EQUAL -1)
            set(cut 11999)
        endif()
        math(EXPR cut "${cut} + 1")
        string(SUBSTRING "${text}" 0 ${cut} piece)
        string(APPEND pieces "R\"midori_embed(${piece})midori_embed\"\n    ")
        string(SUBSTRING "${text}" ${cut} -1 text)
        string(LENGTH "${text}" length)
    endwhile()
    string(APPEND pieces "R\"midori_embed(${text})midori_embed\"")
    file(WRITE "${EMBED_OUTPUT}"
        "#pragma once\n\n#include <string_view>\n\n// Generated from ${EMBED_INPUT}.\n"
        "constexpr std::string_view ${EMBED_NAME} =\n    ${pieces};\n")
    return()
endif()

set(MIDORI_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(midori_target_embed target)
    set(root "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(headers "")
    foreach(input IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH input BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE path)
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "midori_target_embed: '${input}' not found (looked for '${path}').")
        endif()
        cmake_path(GET path FILENAME file)
        string(REPLACE "." "_" name "${file}")
        set(header "${root}/midori/embed/${name}.h")
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" "-DEMBED_INPUT=${path}" "-DEMBED_OUTPUT=${header}" "-DEMBED_NAME=${name}"
                    -P "${MIDORI_EMBED_SCRIPT}"
            DEPENDS "${path}" "${MIDORI_EMBED_SCRIPT}"
            COMMENT "Embedding ${file}"
            VERBATIM
        )
        list(APPEND headers "${header}")
    endforeach()
    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PRIVATE "${root}")
endfunction()
