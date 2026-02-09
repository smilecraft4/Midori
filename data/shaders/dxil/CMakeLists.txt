
find_program(DXC_EXECUTABLE dxc)

if(NOT DXC_EXECUTABLE)
  message(FATAL_ERROR "dxc not found! Install glslang or add to PATH.")
endif()

set(DXIL_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/data/shaders/dxil)
set(DXIL_SHADER_BINARY_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/shaders/dxil)
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
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/layer.vs.hlsl" PROPERTIES ShaderType "vs")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/layer.ps.hlsl" PROPERTIES ShaderType "ps")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/tile.vs.hlsl" PROPERTIES ShaderType "vs")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/tile.ps.hlsl" PROPERTIES ShaderType "ps")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/merge.cs.hlsl" PROPERTIES ShaderType "cs")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/paint.cs.hlsl" PROPERTIES ShaderType "cs")
set_source_files_properties("${DXIL_SHADER_SOURCE_DIR}/erase.cs.hlsl" PROPERTIES ShaderType "cs")

foreach(DXIL_SHADER IN LISTS DXIL_SHADER_FILES)
  get_source_file_property(shadertype ${DXIL_SHADER} ShaderType)
  get_filename_component(SHADER_NAME ${DXIL_SHADER} NAME_WLE)
  set(DXIL_OUTPUT ${DXIL_SHADER_BINARY_DIR}/${SHADER_NAME})

  message("${DXIL_SHADER} ${shadertype}")
                    
  add_custom_command(
    OUTPUT ${DXIL_OUTPUT}.cso
    COMMAND dxc.exe /nologo /Emain /T${shadertype}_6_0 /Zi /Fo ${DXIL_OUTPUT}.cso /Fd ${DXIL_OUTPUT}.pdb ${DXIL_SHADER}
    DEPENDS ${DXIL_SHADER}
    COMMENT "Compiling ${SHADER_NAME} to DXIL"
  )
  list(APPEND DXIL_SHADERS ${DXIL_OUTPUT}.cso)
endforeach()

add_custom_target(midori_shaders_dxil ALL DEPENDS ${DXIL_SHADERS})