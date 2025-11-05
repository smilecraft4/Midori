cmake_minimum_required(VERSION 3.20)

project(Midori)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add SDL3
# Add Tracy

# Compile Shaders

# Compile MidoriLib

# Compile Midori

add_executable(midori)
add_subdirectory(src)

# Add test

# Add benchmarks