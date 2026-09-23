# m12-dxgi: DXGI proxy that ships DX12 present frames to the m11d daemon.
# Build via build.cmd (VS generator with NMake fallback, per AGENTS.md).
cmake_minimum_required(VERSION 3.20)
project(m12_dxgi CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(m12_dxgi SHARED
    dllmain.cpp
    m12_log.cpp
    m12_client.cpp
    m12_dx12.cpp
    m12_hook.cpp
    m12_exports.cpp
    m12_dxgi.def
)

# dxguid.lib carries the COM IIDs as static data; no DXGI/D3D12 import libs
# are linked on purpose (importing dxgi.dll from a dll named dxgi would
# recurse at load time - every COM entry point is reached via vtables).
target_link_libraries(m12_dxgi PRIVATE dxguid kernel32 user32 ws2_32)

target_compile_definitions(m12_dxgi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
