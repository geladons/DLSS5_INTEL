cmake_minimum_required(VERSION 3.20)
project(m6_weights_loader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

add_executable(m6loader main.cpp)

target_include_directories(m6loader PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m6loader PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m6loader PRIVATE vulkan-1 dxgi)
