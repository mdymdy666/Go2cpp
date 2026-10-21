cmake_minimum_required(VERSION 3.16)

project(go2cpp_persistence_tools LANGUAGES CXX)

# 独立工程：不接入顶层 CMakeLists，只有显式构建本目录时才生效。
# 依赖 tools/replay 的 format.cpp（记录格式与 CRC 的唯一真源），
# 但**不**链接 Recorder，避免把运行时/回放依赖带进原型。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PERSISTENCE_BUILD_TESTS "Build persistence tests" ON)

set(REPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../replay"
    CACHE PATH "tools/replay 目录（提供 format.cpp 与头文件）")

find_package(Threads REQUIRED)

add_library(go2cpp_persistence STATIC
    src/encoder.cpp
    src/writer.cpp
    src/reader.cpp
    "${REPLAY_ROOT}/src/format.cpp"
)
target_include_directories(go2cpp_persistence
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${REPLAY_ROOT}/include"
)
target_compile_options(go2cpp_persistence PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(go2cpp_persistence PUBLIC Threads::Threads)

add_executable(persist_bench bench/persist_bench.cpp)
target_link_libraries(persist_bench PRIVATE go2cpp_persistence)
target_compile_options(persist_bench PRIVATE -Wall -Wextra -Wpedantic)

# 只做 io_uring / O_DIRECT 能力探测，不参与压测。
add_executable(platform_probe tests/platform_probe.cpp)

if(PERSISTENCE_BUILD_TESTS)
    enable_testing()
    add_executable(persistence_tests tests/persistence_tests.cpp)
    target_link_libraries(persistence_tests PRIVATE go2cpp_persistence)
    target_compile_options(persistence_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_persistence_tests COMMAND persistence_tests)
endif()
