cmake_minimum_required(VERSION 3.20)
project(d5c_chain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
    VK_USE_PLATFORM_WIN32_KHR)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shader SOURCES live with m8b-live (single source of truth); this module
# compiles its own .spv copies for its consumers (m11d, later m8b-live).
# Two families with DIFFERENT publish.glsl numerics - kept in separate
# include roots so #include "publish.glsl" resolves per family.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../m8b-live/shaders)
set(M8_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../m8b-live/shaders/m8)

# transport/front-end shaders needed by ChainEngine in absolute mode
set(SHADERS decode features compose encode)
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

set(M8_SHADERS gemm gemm_rn1 cosine cosine_win softmax elementwise
               partition transpose_we gather_residual merge featpack headpack
               pool2 upmerge)
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

add_custom_target(d5c_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_library(d5c_chain STATIC
    vk_context.cpp
    weights.cpp
    arena.cpp
    recorder.cpp
    recorder_blocks.cpp
    engine.cpp)
add_dependencies(d5c_chain d5c_shaders)

target_include_directories(d5c_chain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} "${VULKAN_SDK}/Include")
target_link_directories(d5c_chain PUBLIC "${VULKAN_SDK}/Lib")
target_link_libraries(d5c_chain PUBLIC vulkan-1)

# d5c_stage_shaders(<target>): copy the 18 spv next to the consumer exe.
# The spv dir travels via a global property: CMAKE_CURRENT_BINARY_DIR inside a
# function resolves in the CALLER's scope, and the chain lists live in the
# chain/ directory scope.
set_property(GLOBAL PROPERTY D5C_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}")
function(d5c_stage_shaders TGT)
    get_property(SPV_DIR GLOBAL PROPERTY D5C_SPV_DIR)
    set(SHADERS decode features compose encode)
    set(M8_SHADERS gemm gemm_rn1 cosine cosine_win softmax elementwise
                   partition transpose_we gather_residual merge featpack headpack
                   pool2 upmerge)
    foreach(S ${SHADERS} ${M8_SHADERS})
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${SPV_DIR}/${S}.spv $<TARGET_FILE_DIR:${TGT}>/${S}.spv)
    endforeach()
endfunction()
