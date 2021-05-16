cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_COMPILER "clang++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2") 

aux_source_directory(. SF)
add_library(stest ${SF})

target_include_directories(stest PRIVATE ../)