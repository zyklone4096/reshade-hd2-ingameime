cmake_minimum_required(VERSION 4.1)
project(reshade_hd2_ingameime)

set(CMAKE_CXX_STANDARD 20)

add_library(reshade_hd2_ingameime SHARED library.cpp input.cpp)
target_include_directories(reshade_hd2_ingameime PRIVATE reshade/include)
target_link_libraries(reshade_hd2_ingameime PRIVATE imm32)

set_target_properties(reshade_hd2_ingameime PROPERTIES
    SUFFIX ".addon64")
