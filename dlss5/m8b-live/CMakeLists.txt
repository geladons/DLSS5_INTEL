cmake_minimum_required(VERSION 3.20)
project(m8b_live CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same platform definitions as M2/M3/M4: d3d11_4.h chain-include + win32 Vulkan types.
add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
    VK_USE_PLATFORM_WIN32_KHR)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shaders, two families with DIFFERENT publish.glsl numerics — kept in
# separate include roots so #include "publish.glsl" resolves per family:
#   shaders/    m4 front-end (half_round via packHalf2x16, M3/M4-validated)
#   shaders/m8/ m8 chain (half_round via manual bit manipulation, M8a-validated
#               on Arc — packHalf2x16/unpackHalf2x16 is folded to a no-op by
#               this driver and every rounding point would silently vanish)
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(M8_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders/m8)

# m4 transport/front-end shaders (standin removed — replaced by the real chain)
set(SHADERS decode letterbox rescale features compose encode blit fbcancel)
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

# m8 chain kernels + m8b bridge shaders. gemm is compiled twice: the default
# 16x32 tile and a GEMM_RN=1 16x16 variant for the head GEMM (N=16), same as m7/m8.
set(M8_SHADERS gemm gemm_rn1 cosine cosine_win softmax elementwise
               partition transpose_we gather_residual merge featpack headpack)
foreach(S ${M8_SHADERS})
    set(SPV ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv)
    if(S STREQUAL "gemm_rn1")
        set(DEFS -DGEMM_RN=1)
        set(SRCNAME gemm)
    else()
        set(DEFS "")
        set(SRCNAME ${S})
    endif()
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND "${VULKAN_SDK}/Bin/glslangValidator.exe"
                -V --target-env vulkan1.3 ${DEFS}
                -I${M8_SHADER_DIR}
                ${M8_SHADER_DIR}/${SRCNAME}.comp
                -o ${SPV}
        DEPENDS ${M8_SHADER_DIR}/${SRCNAME}.comp ${M8_SHADER_DIR}/publish.glsl
        COMMENT "Compiling m8/${SRCNAME}.comp -> ${S}.spv"
        VERBATIM)
    list(APPEND SPV_OUTPUTS ${SPV})
endforeach()

add_custom_target(m8b_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(m8blive main.cpp)
add_dependencies(m8blive m8b_shaders)

target_include_directories(m8blive PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m8blive PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m8blive PRIVATE d3d11 dxgi vulkan-1)

foreach(S ${SHADERS} ${M8_SHADERS})
    add_custom_command(TARGET m8blive POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv $<TARGET_FILE_DIR:m8blive>/${S}.spv)
endforeach()
