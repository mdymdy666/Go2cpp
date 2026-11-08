# minimal_platform —— 证明 `go2cpp_platform` 零依赖
cmake_minimum_required(VERSION 3.16)
project(minimal_platform CXX)

if(NOT DEFINED GO2CPP_SOURCE_DIR)
    message(FATAL_ERROR "请传 -DGO2CPP_SOURCE_DIR=<go2cpp 根目录>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${GO2CPP_SOURCE_DIR} ${CMAKE_BINARY_DIR}/go2cpp)

add_executable(minimal_platform main.cpp)
# ★ 唯一链接：go2cpp_platform（零依赖）
target_link_libraries(minimal_platform PRIVATE go2cpp_platform)
