cmake_minimum_required(VERSION 3.20)
project(m8_full_chain CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shaders. publish.glsl is #included (verbatim reference copy), so pass the
# shader dir as include path. gemm is compiled twice: the default 16x32 tile and
# a GEMM_RN=1 16x16 variant for the head GEMM (N=16). Same pattern as m7.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADERS round_test gemm gemm_rn1 cosine cosine_win softmax elementwise
            partition transpose_we gather_residual merge)
set(SPV_OUTPUTS "")
foreach(S ${SHADERS})
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
                -I${SHADER_DIR}
                ${SHADER_DIR}/${SRCNAME}.comp
                -o ${SPV}
        DEPENDS ${SHADER_DIR}/${SRCNAME}.comp ${SHADER_DIR}/publish.glsl
        COMMENT "Compiling ${SRCNAME}.comp -> ${S}.spv"
        VERBATIM)
    list(APPEND SPV_OUTPUTS ${SPV})
endforeach()
add_custom_target(m8_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(m8proto main.cpp)
add_dependencies(m8proto m8_shaders)

target_include_directories(m8proto PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m8proto PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m8proto PRIVATE vulkan-1 dxgi)

foreach(S ${SHADERS})
    add_custom_command(TARGET m8proto POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv $<TARGET_FILE_DIR:m8proto>/${S}.spv)
endforeach()
