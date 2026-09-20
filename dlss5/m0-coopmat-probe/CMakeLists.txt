cmake_minimum_required(VERSION 3.20)
project(m0_coopmat_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK env var not set")
endif()
set(VULKAN_SDK "$ENV{VULKAN_SDK}")

# Shape of the cooperative-matrix tile. These MUST match the config the probe
# binary selects at runtime; build.ps1 re-runs cmake with the right values if
# the first guess mismatches (probe exits with code 2 in that case).
set(CFG_M 16        CACHE STRING "coopmat M")
set(CFG_N 16        CACHE STRING "coopmat N")
set(CFG_K 16        CACHE STRING "coopmat K")
set(CFG_SUBGROUP 32 CACHE STRING "subgroup size")
set(CFG_CF32 1      CACHE STRING "1 = f32 C/result, 0 = f16")

set(SPV ${CMAKE_CURRENT_BINARY_DIR}/probe.spv)

set(SHADER_DEFINES
    -DSUBGROUP_SIZE=${CFG_SUBGROUP}
    -DM=${CFG_M}
    -DN=${CFG_N}
    -DK=${CFG_K})
if(CFG_CF32)
    list(APPEND SHADER_DEFINES -DC_F32)
endif()

add_custom_command(
    OUTPUT ${SPV}
    COMMAND "${VULKAN_SDK}/Bin/glslangValidator.exe"
            -V --target-env vulkan1.3
            ${SHADER_DEFINES}
            ${CMAKE_CURRENT_SOURCE_DIR}/probe.comp
            -o ${SPV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/probe.comp
    COMMENT "Compiling probe.comp -> probe.spv (M=${CFG_M} N=${CFG_N} K=${CFG_K} SG=${CFG_SUBGROUP} CF32=${CFG_CF32})"
    VERBATIM)

add_custom_target(probe_shader ALL DEPENDS ${SPV})

add_executable(probe main.cpp)
add_dependencies(probe probe_shader)

target_include_directories(probe PRIVATE "${VULKAN_SDK}/Include")
target_link_directories(probe PRIVATE "${VULKAN_SDK}/Lib")
target_link_libraries(probe PRIVATE vulkan-1)

target_compile_definitions(probe PRIVATE
    CFG_M=${CFG_M}
    CFG_N=${CFG_N}
    CFG_K=${CFG_K}
    CFG_SUBGROUP=${CFG_SUBGROUP}
    CFG_CF32=${CFG_CF32})

add_custom_command(TARGET probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SPV} $<TARGET_FILE_DIR:probe>/probe.spv)
