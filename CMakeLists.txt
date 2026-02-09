cmake_minimum_required(VERSION 3.20)

project(Midori)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Data

set(TRACY_ENABLE ON)

# Core
add_subdirectory(deps/SDL)
add_subdirectory(deps/glm)
add_subdirectory(deps/tracy)
add_subdirectory(deps/freetype)
add_subdirectory(deps/harfbuzz)

add_library(midori_lib
  "src/app.cpp"
  "src/canvas.cpp"
  "src/renderer.cpp"
  "src/memory.cpp"
  "src/history.cpp"
  "src/viewport.cpp"
  "src/waypoint.cpp"
  "src/layers.cpp"
  "src/ui.cpp"
)
target_compile_options(midori_lib PRIVATE /W4 /WX)
# target_compile_definitions(midori_lib PUBLIC $<$<CONFIG:Debug>:TRACY_ENABLE>)
# target_compile_definitions(midori_lib PRIVATE TRACY_ENABLE)
target_link_libraries(midori_lib PUBLIC SDL3::SDL3 Tracy::TracyClient glm::glm freetype harfbuzz)
target_include_directories(midori_lib PUBLIC "deps/imgui/")
target_include_directories(midori_lib PUBLIC "deps/nlohmann/")
target_include_directories(midori_lib PUBLIC "deps/qoi/")
target_include_directories(midori_lib PUBLIC "deps/uuid/")
target_include_directories(midori_lib PUBLIC "deps/utf8/")
target_sources(midori_lib PRIVATE
  "deps/imgui/imgui.cpp"
  "deps/imgui/imgui_draw.cpp"
  "deps/imgui/imgui_widgets.cpp"
  "deps/imgui/imgui_tables.cpp"
  "deps/imgui/backends/imgui_impl_sdl3.cpp"
  "deps/imgui/backends/imgui_impl_sdlgpu3.cpp"
)

# Executable
add_executable(midori WIN32 "src/main.cpp")
target_link_libraries(midori PRIVATE midori_lib)
target_compile_options(midori PRIVATE /W4 /WX)

add_subdirectory(data/shaders/dxil)
add_subdirectory(data/shaders/spirv)
add_dependencies(midori midori_shaders_spirv midori_shaders_dxil)

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
