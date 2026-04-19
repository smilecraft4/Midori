
find_program(DXC_EXECUTABLE dxc)

if(NOT DXC_EXECUTABLE)
  message(FATAL_ERROR "dxc not found! Install dxc or add to PATH.")
endif()

set(DXIL_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/data/shaders/dxil)
set(DXIL_SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/generated/shaders/dxil)
file(MAKE_DIRECTORY ${DXIL_SHADER_BINARY_DIR})

set(DXIL_SHADER_FILES
  "${DXIL_SHADER_SOURCE_DIR}/layer.vs.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/layer.ps.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/tile.vs.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/tile.ps.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/merge.cs.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/paint.cs.hlsl"
  "${DXIL_SHADER_SOURCE_DIR}/erase.cs.hlsl"
)

foreach(DXIL_SHADER IN LISTS DXIL_SHADER_FILES)
  get_filename_component(SHADER_NAME ${DXIL_SHADER} NAME_WLE)
  get_filename_component(SHADER_TYPE_EXT ${SHADER_NAME} EXT)
  string(SUBSTRING ${SHADER_TYPE_EXT} 1 -1 SHADER_TYPE)
  string(REPLACE "." "_" SHADER_VAR_NAME ${SHADER_NAME})
  set(DXIL_OUTPUT ${DXIL_SHADER_BINARY_DIR}/${SHADER_NAME})

  add_custom_command(
    OUTPUT ${DXIL_OUTPUT}.h
    COMMAND ${DXC_EXECUTABLE} /nologo /Emain /T${SHADER_TYPE}_6_0 /Zi /Fo ${DXIL_OUTPUT}.cso /Fd ${DXIL_OUTPUT}.pdb /Fh ${DXIL_OUTPUT}.h /Vn ${SHADER_VAR_NAME} ${DXIL_SHADER}
    DEPENDS ${DXIL_SHADER}
    COMMENT "Compiling ${SHADER_NAME} to DXIL"
  )
  list(APPEND DXIL_SHADERS ${DXIL_OUTPUT}.h)
endforeach()

add_custom_target(midori_shaders_dxil ALL DEPENDS ${DXIL_SHADERS})
