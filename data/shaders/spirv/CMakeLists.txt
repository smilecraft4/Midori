
find_program(GLSLANG_VALIDATOR glslangValidator)

if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found! Install glslang or add to PATH.")
endif()

set(SPIRV_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/data/shaders/spirv)
set(SPIRV_SHADER_BINARY_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/shaders/spirv)
file(MAKE_DIRECTORY ${SPIRV_SHADER_BINARY_DIR})

set(SPIRV_SHADER_FILES
  "${SPIRV_SHADER_SOURCE_DIR}/layer.vert.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/layer.frag.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/tile.vert.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/tile.frag.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/merge.comp.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/paint.comp.glsl"
  "${SPIRV_SHADER_SOURCE_DIR}/erase.comp.glsl"
)

foreach(SPIRV_SHADER IN LISTS SPIRV_SHADER_FILES)
  get_filename_component(SHADER_NAME ${SPIRV_SHADER} NAME_WLE)
  set(SPIRV_OUTPUT ${SPIRV_SHADER_BINARY_DIR}/${SHADER_NAME}.spv)

  add_custom_command(
    OUTPUT ${SPIRV_OUTPUT}
    COMMAND ${GLSLANG_VALIDATOR} -V ${SPIRV_SHADER} -o ${SPIRV_OUTPUT}
    DEPENDS ${SPIRV_SHADER}
    COMMENT "Compiling ${SHADER_NAME} to SPIRV"
  )
  list(APPEND SPIRV_SHADERS ${SPIRV_OUTPUT})
endforeach()

add_custom_target(midori_shaders_spirv ALL DEPENDS ${SPIRV_SHADERS})