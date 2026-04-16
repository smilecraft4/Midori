cmake_minimum_required(VERSION 3.20)

project(Midori)


set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
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
add_subdirectory(deps/EASTL)

add_library(midori_lib
  "src/app.cpp"
  "src/canvas.cpp"
  "src/renderer.cpp"
  "src/memory.cpp"
  "src/viewport.cpp"
  "src/commands.cpp"
  "src/ui.cpp"
)

target_link_libraries(midori_lib PUBLIC SDL3::SDL3 Tracy::TracyClient glm::glm freetype harfbuzz EASTL)
target_include_directories(midori_lib PUBLIC "deps/imgui/")
target_include_directories(midori_lib PUBLIC "deps/nlohmann/")
target_include_directories(midori_lib PUBLIC "deps/qoi/")
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

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")  
    add_executable(midori WIN32 "src/main.cpp" "data/resource.rc")
    add_subdirectory(data/shaders/dxil)
    add_dependencies(midori midori_shaders_dxil)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(midori "src/main.cpp")
    add_subdirectory(data/shaders/spirv)
    add_dependencies(midori midori_shaders_spirv)
endif()

target_link_libraries(midori PRIVATE midori_lib)
