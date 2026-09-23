# m12-dxgi test app: windowed D3D12 gradient presenter.
cmake_minimum_required(VERSION 3.20)
project(m12_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(m12_test main.cpp)
target_link_libraries(m12_test PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(m12_test PRIVATE NOMINMAX)
