cmake_minimum_required(VERSION 3.16)
project(ambilight_native LANGUAGES CXX)

add_executable(ambilight_native
    src/main.cpp
)

target_compile_features(ambilight_native PRIVATE cxx_std_17)

target_link_libraries(ambilight_native PRIVATE
    d3d11
    d3dcompiler
    dxgi
    user32
    winmm
)

if(MSVC)
    target_compile_options(ambilight_native PRIVATE /W4 /EHsc)
    target_compile_options(ambilight_native PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(ambilight_native PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ambilight_native PRIVATE $<$<CONFIG:Release>:-O3>)
endif()
