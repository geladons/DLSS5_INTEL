cmake_minimum_required(VERSION 3.20)
project(m4_present_simple CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same platform definitions as M2/M3: d3d11_4.h chain-include + win32 Vulkan types.
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
set(SHADERS decode letterbox rescale features standin compose encode blit)
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
add_custom_target(m4s_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(m4simple main.cpp)
add_dependencies(m4simple m4s_shaders)

target_include_directories(m4simple PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m4simple PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m4simple PRIVATE d3d11 dxgi vulkan-1)

foreach(S ${SHADERS})
    add_custom_command(TARGET m4simple POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv $<TARGET_FILE_DIR:m4simple>/${S}.spv)
endforeach()
