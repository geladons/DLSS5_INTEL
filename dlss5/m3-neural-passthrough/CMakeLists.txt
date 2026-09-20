cmake_minimum_required(VERSION 3.20)
project(m3_neural_passthrough CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same platform definitions as M2: d3d11_4.h chain-include + win32 Vulkan types.
add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
    VK_USE_PLATFORM_WIN32_KHR)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shaders. publish.glsl is #included by features.comp/standin.comp (ported from
# the reference src/gpu/publish.glsl), so pass the source dir as include path.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADERS decode letterbox rescale features standin compose encode)
set(SPV_OUTPUTS "")
foreach(S ${SHADERS})
    set(SPV ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv)
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND "${VULKAN_SDK}/Bin/glslangValidator.exe"
                -V --target-env vulkan1.3
                -I${SHADER_DIR}
                ${SHADER_DIR}/${S}.comp
                -o ${SPV}
        DEPENDS ${SHADER_DIR}/${S}.comp ${SHADER_DIR}/publish.glsl
        COMMENT "Compiling ${S}.comp -> ${S}.spv"
        VERBATIM)
    list(APPEND SPV_OUTPUTS ${SPV})
endforeach()
add_custom_target(m3_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(m3neural main.cpp)
add_dependencies(m3neural m3_shaders)

target_include_directories(m3neural PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m3neural PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m3neural PRIVATE d3d11 dxgi vulkan-1)

foreach(S ${SHADERS})
    add_custom_command(TARGET m3neural POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv $<TARGET_FILE_DIR:m3neural>/${S}.spv)
endforeach()
