cmake_minimum_required(VERSION 3.20)

project(Midori)

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Dependencies

# Fetch SDL3
include(FetchContent)
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.26
)
FetchContent_MakeAvailable(SDL3)

# Fetch glm
include(FetchContent)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.2
)
FetchContent_MakeAvailable(glm)

# Fetch Dear ImGui (docking branch for docking/multi-viewport support)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.4-docking
)

FetchContent_MakeAvailable(imgui)

# Fetch Tracy
include(FetchContent)
FetchContent_Declare(
  tracy
  GIT_REPOSITORY https://github.com/wolfpld/tracy.git
  GIT_TAG v0.12.2
)
FetchContent_MakeAvailable(tracy)

# # Compiling shaders
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

# # Library
add_library(midori_lib
  "src/app.cpp"
  "src/canvas.cpp"
  "src/renderer.cpp"
)
target_include_directories(midori_lib PUBLIC include)
target_compile_features(midori_lib PUBLIC cxx_std_23)
target_compile_definitions(midori_lib PUBLIC TRACY_ENABLE)
target_link_libraries(midori_lib PUBLIC SDL3::SDL3 Tracy::TracyClient glm::glm)
target_include_directories(midori_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_sources(midori_lib PRIVATE
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

# # Application
add_executable(midori src/main.cpp)
target_link_libraries(midori PRIVATE midori_lib)
add_dependencies(midori midori_shaders)

# # Tests
enable_testing()
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(midori_test
  "test/midori_test.cpp"
)
target_link_libraries(midori_test PRIVATE midori_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(midori_test)

# # Benchmarks

# Fetch Google Benchmark
include(FetchContent)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# Add benchmark executable
add_executable(midori_benchmark
  "benchmark/midori_benchmark.cpp"
)
target_link_libraries(midori_benchmark PRIVATE midori_lib benchmark::benchmark_main)
