cmake_minimum_required(VERSION 3.20)

project(Midori)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Data
find_program(GLSLANG_VALIDATOR glslangValidator)

if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found! Install glslang or add to PATH.")
endif()

set(VULKAN_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/shaders/vulkan)
set(VULKAN_SHADER_BINARY_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data/shaders/vulkan)
file(MAKE_DIRECTORY ${VULKAN_SHADER_BINARY_DIR})
set(VULKAN_SHADERS
  ${VULKAN_SHADER_SOURCE_DIR}/layer.vert.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/layer.frag.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/tile.vert.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/tile.frag.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/merge.comp.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/paint.comp.glsl
  ${VULKAN_SHADER_SOURCE_DIR}/erase.comp.glsl
)

foreach(SHADER IN LISTS VULKAN_SHADERS)
  get_filename_component(SHADER_NAME ${SHADER} NAME_WLE)
  set(SPV_OUTPUT ${VULKAN_SHADER_BINARY_DIR}/${SHADER_NAME}.spv)

  add_custom_command(
    OUTPUT ${SPV_OUTPUT}
    COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SPV_OUTPUT}
    DEPENDS ${SHADER}
    COMMENT "Compiling ${SHADER_NAME} to SPV"
  )
  list(APPEND SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(midori_shaders ALL DEPENDS ${SHADERS})

# Core
add_subdirectory(deps/SDL)
add_subdirectory(deps/glm)
add_subdirectory(deps/tracy)
add_library(midori_lib
  "src/app.cpp"
  "src/canvas.cpp"
  "src/renderer.cpp"
  "src/memory.cpp"
  "src/history.cpp"
  "src/viewport.cpp"
  "src/waypoint.cpp"
  "src/layers.cpp"
)
target_compile_definitions(midori_lib PUBLIC $<$<CONFIG:Debug>:TRACY_ENABLE>)
target_link_libraries(midori_lib PUBLIC SDL3::SDL3 Tracy::TracyClient glm::glm)
target_include_directories(midori_lib PUBLIC "deps/imgui/")
target_include_directories(midori_lib PUBLIC "deps/nlohmann/")
target_include_directories(midori_lib PUBLIC "deps/qoi/")
target_include_directories(midori_lib PUBLIC "deps/uuid/")
target_sources(midori_lib PRIVATE
  "deps/imgui/imgui.cpp"
  "deps/imgui/imgui_draw.cpp"
  "deps/imgui/imgui_widgets.cpp"
  "deps/imgui/imgui_tables.cpp"
  "deps/imgui/backends/imgui_impl_sdl3.cpp"
  "deps/imgui/backends/imgui_impl_sdlgpu3.cpp"
)

# Executable
add_executable(midori "src/main.cpp")
target_link_libraries(midori PRIVATE midori_lib)
add_dependencies(midori midori_shaders)

# Installer
# TODO

# Tests
enable_testing()
add_subdirectory(deps/googletest)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_executable(midori_test
  "test/midori_ring_array_test.cpp"
  "test/midori_history_test.cpp"
)
target_link_libraries(midori_test PRIVATE midori_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(midori_test)

# Benchmark
add_subdirectory(deps/benchmark)
add_executable(midori_benchmark
  "benchmark/midori_benchmark.cpp"
)
target_link_libraries(midori_benchmark PRIVATE midori_lib benchmark::benchmark_main)
