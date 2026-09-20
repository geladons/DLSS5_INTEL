cmake_minimum_required(VERSION 3.20)
project(m7_graph_proto CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shaders. publish.glsl is #included (verbatim reference copy), so pass the
# shader dir as include path. Same pattern as m3-neural-passthrough.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADERS round_test gemm cosine softmax elementwise)
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
add_custom_target(m7_shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(m7proto main.cpp)
add_dependencies(m7proto m7_shaders)

target_include_directories(m7proto PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(m7proto PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(m7proto PRIVATE vulkan-1 dxgi)

foreach(S ${SHADERS})
    add_custom_command(TARGET m7proto POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${S}.spv $<TARGET_FILE_DIR:m7proto>/${S}.spv)
endforeach()
