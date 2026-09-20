cmake_minimum_required(VERSION 3.20)
project(m2_dxgi_vulkan_interop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ID3D11Device5 / ID3D11Fence / ID3D11DeviceContext4 live in d3d11_4.h, which
# d3d11.h only includes when NTDDI_VERSION >= NTDDI_WIN10_RS2; the win32 Vulkan
# external-memory/semaphore types require VK_USE_PLATFORM_WIN32_KHR.
add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
    VK_USE_PLATFORM_WIN32_KHR)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

set(SPV ${CMAKE_CURRENT_BINARY_DIR}/passthrough.spv)
add_custom_command(
    OUTPUT ${SPV}
    COMMAND "${VULKAN_SDK}/Bin/glslangValidator.exe"
            -V --target-env vulkan1.3
            ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.comp
            -o ${SPV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/passthrough.comp
    COMMENT "Compiling passthrough.comp -> passthrough.spv"
    VERBATIM)
add_custom_target(m2_shader ALL DEPENDS ${SPV})

add_executable(m2interop main.cpp)
add_dependencies(m2interop m2_shader)

target_include_directories(m2interop PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m2interop PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m2interop PRIVATE d3d11 dxgi vulkan-1)

add_custom_command(TARGET m2interop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SPV} $<TARGET_FILE_DIR:m2interop>/passthrough.spv)
