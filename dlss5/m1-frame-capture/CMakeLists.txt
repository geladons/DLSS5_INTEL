cmake_minimum_required(VERSION 3.20)
project(m1_frame_capture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

add_executable(m1dda main.cpp)
target_link_libraries(m1dda PRIVATE dxgi d3d11)
