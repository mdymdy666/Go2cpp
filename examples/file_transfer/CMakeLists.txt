# ---------------------------------------------------------------------------
# file_transfer：资源传输示例（独立构建，不改主构建文件）
#
# 先构建主工程（app 框架必须已存在）：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_APP=ON
#   cmake --build .build-bento-wsl -j
#
# 再构建本目录：
#   cmake -S examples/file_transfer -B .build-file-transfer -G Ninja
#   cmake --build .build-file-transfer -j
#   ./.build-file-transfer/file_transfer_demo --suite
#
# 依赖：.build-bento-wsl 里的 libgo2cpp_app.a 与 libgo2cpp_hash.so。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(go2cpp_file_transfer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(GO2CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "go2cpp 源码根目录")
set(GO2CPP_BUILD_DIR "${GO2CPP_ROOT}/.build-bento-wsl" CACHE PATH "已构建的 go2cpp 构建目录")

if(NOT EXISTS "${GO2CPP_BUILD_DIR}/libgo2cpp_app.a")
    message(FATAL_ERROR
        "缺少 ${GO2CPP_BUILD_DIR}/libgo2cpp_app.a；先构建主工程（-DGO2CPP_WITH_APP=ON）")
endif()
if(NOT EXISTS "${GO2CPP_BUILD_DIR}/libgo2cpp_hash.so")
    message(FATAL_ERROR "缺少 ${GO2CPP_BUILD_DIR}/libgo2cpp_hash.so")
endif()

find_package(Threads REQUIRED)

add_executable(file_transfer_demo file_transfer_demo.cpp)
target_include_directories(file_transfer_demo PRIVATE "${GO2CPP_ROOT}/include")
target_compile_options(file_transfer_demo PRIVATE -Wall -Wextra -Wpedantic -Werror -O3)
target_link_libraries(file_transfer_demo PRIVATE
    "${GO2CPP_BUILD_DIR}/libgo2cpp_app.a"
    "${GO2CPP_BUILD_DIR}/libgo2cpp_hash.so"
    Threads::Threads)
target_link_options(file_transfer_demo PRIVATE "-Wl,-rpath,${GO2CPP_BUILD_DIR}")

enable_testing()
add_test(NAME file_transfer_suite COMMAND file_transfer_demo --suite)
