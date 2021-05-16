cmake_minimum_required(VERSION 3.0)

project(smalloc)

set(CMAKE_CXX_COMPILER "clang++")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2") 

add_subdirectory(test)

aux_source_directory(. SF)
add_executable(${PROJECT_NAME} ${SF})

target_include_directories(${PROJECT_NAME} PRIVATE ./ ./test)
target_link_libraries(${PROJECT_NAME} pthread stest)

