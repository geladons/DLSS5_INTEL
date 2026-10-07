cmake_minimum_required(VERSION 3.20)
project(m11_layer C)

# M11: Windows port of the dlss-nr-on-intel Vulkan present layer.
# Output: nr_layer_win.dll next to the manifest VkLayer_dlssnr_win.json.
# Compiled as C (not C++): the Vulkan header prototypes then carry C linkage
# and the .def file yields the clean export names the loader looks up
# (dllexport on the definitions is a hard error in MSVC C mode - C2375).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK "C:/VulkanSDK/1.4.357.0")
else()
    set(VULKAN_SDK "$ENV{VULKAN_SDK}")
endif()

# GPU residual-compose shaders: glslc -> SPIR-V -> embedded C header
# (shaders/_embed_spv.py). The DLL is loaded from arbitrary game dirs, so
# the SPIR-V must travel inside the binary - no side .spv files.
set(GW_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
add_custom_command(
    OUTPUT ${GW_SHADER_DIR}/nr_gpuwarp_spv.h
    COMMAND ${CMAKE_COMMAND} -E env "VULKAN_SDK=${VULKAN_SDK}"
            python ${GW_SHADER_DIR}/_embed_spv.py
    DEPENDS ${GW_SHADER_DIR}/gpuwarp_ds.comp
            ${GW_SHADER_DIR}/gpuwarp_match.comp
            ${GW_SHADER_DIR}/gpuwarp_compose.comp
            ${GW_SHADER_DIR}/_embed_spv.py
    WORKING_DIRECTORY ${GW_SHADER_DIR}
    COMMENT "glslc + embed gpuwarp SPIR-V (nr_gpuwarp_spv.h)")
add_custom_target(gpuwarp_spv DEPENDS ${GW_SHADER_DIR}/nr_gpuwarp_spv.h)

add_library(nr_layer_win SHARED nr_layer_win.c nr_framegen.c nr_degrade.c
            nr_util.c nr_async.c nr_residual.c nr_warp.c nr_gpuwarp.c
            daemon_client.c gdi_guard.c nr_ui.c nr_layer_win.def)
add_dependencies(nr_layer_win gpuwarp_spv)
target_include_directories(nr_layer_win PRIVATE "${VULKAN_SDK}/Include" "${CMAKE_CURRENT_SOURCE_DIR}/../chain")
target_link_libraries(nr_layer_win PRIVATE ws2_32)
# No libvulkan link: the layer resolves everything through the loader's
# GetProcAddr chain, like the Linux original.
set_target_properties(nr_layer_win PROPERTIES PREFIX "" OUTPUT_NAME "nr_layer_win")
