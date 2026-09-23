cmake_minimum_required(VERSION 3.20)
project(m11d CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
    VK_USE_PLATFORM_WIN32_KHR)

# the shared chain module (shaders compile there; d5c_stage_shaders stages spv)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../chain ${CMAKE_CURRENT_BINARY_DIR}/chain)

add_executable(m11d main.cpp)
add_dependencies(m11d d5c_chain)
target_link_libraries(m11d PRIVATE d5c_chain ws2_32)

d5c_stage_shaders(m11d)
