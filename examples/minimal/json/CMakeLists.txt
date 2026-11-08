# minimal_json —— 证明 `go2cpp_json` 可以单独使用
#
# 用法：
#   cmake -S <go2cpp>/examples/minimal/json -B /tmp/mj -DGO2CPP_SOURCE_DIR=<go2cpp 根>
#   cmake --build /tmp/mj
#   /tmp/mj/minimal_json
#   ldd /tmp/mj/minimal_json | grep -c 'libgo2cpp_network\|libgo2cpp_fiber\|libgo2cpp_scheduler'   # 必须是 0
cmake_minimum_required(VERSION 3.16)
project(minimal_json CXX)

if(NOT DEFINED GO2CPP_SOURCE_DIR)
    message(FATAL_ERROR "请传 -DGO2CPP_SOURCE_DIR=<go2cpp 根目录>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只引入 go2cpp 的构建（复用它的 target 定义）
add_subdirectory(${GO2CPP_SOURCE_DIR} ${CMAKE_BINARY_DIR}/go2cpp)

add_executable(minimal_json main.cpp)

# ★ 唯一链接：go2cpp_json（它只依赖 go2cpp_error）
target_link_libraries(minimal_json PRIVATE go2cpp_json)
