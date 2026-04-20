cmake_minimum_required(VERSION 3.20)

project(Midori VERSION 0.1.0)

include(GNUInstallDirs)


set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    set(TRACY_ENABLE ON)
else()
    set(TRACY_ENABLE OFF)
endif()

# Dependencies
add_subdirectory(deps/SDL EXCLUDE_FROM_ALL)
add_subdirectory(deps/glm EXCLUDE_FROM_ALL)
add_subdirectory(deps/tracy EXCLUDE_FROM_ALL)
add_subdirectory(deps/freetype EXCLUDE_FROM_ALL)
add_subdirectory(deps/harfbuzz EXCLUDE_FROM_ALL)
add_subdirectory(deps/EASTL EXCLUDE_FROM_ALL)



# Executable

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h")  


if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(midori WIN32 "src/main.cpp" "data/resource.rc")
    add_subdirectory(data/shaders/dxil)
    add_subdirectory(data/shaders/spirv)
    add_dependencies(midori midori_shaders_dxil midori_shaders_spirv)
    target_compile_definitions(midori PRIVATE MIDORI_WINDOWS)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(midori "src/main.cpp")
    add_subdirectory(data/shaders/spirv)
    add_dependencies(midori midori_shaders_spirv)
    target_compile_definitions(midori PRIVATE MIDORI_LINUX)
endif()

target_sources(midori PRIVATE
  "src/app.cpp"
  "src/canvas.cpp"
  "src/renderer.cpp"
  "src/memory.cpp"
  "src/viewport.cpp"
  "src/commands.cpp"
  "src/states.cpp"
  "src/ui.cpp"
)

target_link_libraries(midori PRIVATE 
    SDL3::SDL3 
    Tracy::TracyClient 
    glm::glm 
    freetype 
    harfbuzz 
    EASTL
)
target_include_directories(midori PRIVATE 
    "deps/imgui/" 
    "deps/nlohmann/" 
    "deps/qoi/" 
    "deps/utf8/"
    "${CMAKE_BINARY_DIR}/generated"
)
target_sources(midori PRIVATE
  "deps/imgui/imgui.cpp"
  "deps/imgui/imgui_draw.cpp"
  "deps/imgui/imgui_widgets.cpp"
  "deps/imgui/imgui_tables.cpp"
  "deps/imgui/backends/imgui_impl_sdl3.cpp"
  "deps/imgui/backends/imgui_impl_sdlgpu3.cpp"
)

# Install
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(TARGETS midori RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES data/midori.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME midori.png)
endif()
