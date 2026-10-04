cmake_minimum_required(VERSION 3.20)
project(ddarec CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

add_executable(ddarec ddarec.cpp)
target_link_libraries(ddarec PRIVATE dxgi d3d11)
