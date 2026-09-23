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

add_library(nr_layer_win SHARED nr_layer_win.c nr_layer_win.def)
target_include_directories(nr_layer_win PRIVATE "${VULKAN_SDK}/Include")
target_link_libraries(nr_layer_win PRIVATE ws2_32)
# No libvulkan link: the layer resolves everything through the loader's
# GetProcAddr chain, like the Linux original.
set_target_properties(nr_layer_win PROPERTIES PREFIX "" OUTPUT_NAME "nr_layer_win")
