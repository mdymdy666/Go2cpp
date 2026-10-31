# cache 模块（header-only 策略缓存）：LRU / LRU-K / 2Q / W-TinyLFU /
# GDSF / ARC / CLOCK，统一 `go2cpp::cache::Cache<K, V>` 接口。
#
# 设计边界：模块只有头文件，不引入新的二进制库；`go2cpp_cache` 是
# INTERFACE target，给测试/示例提供 include 路径与 C++17 要求。头文件随
# 顶层 `install(DIRECTORY include/)` 一起安装，不单独登记 EXPORT，避免
# 改动既有模块的导出清单。
add_library(go2cpp_cache INTERFACE)
add_library(go2cpp::cache ALIAS go2cpp_cache)
target_include_directories(go2cpp_cache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(go2cpp_cache INTERFACE cxx_std_17)
target_link_libraries(go2cpp_cache INTERFACE Threads::Threads)

if (GO2CPP_BUILD_TESTS)
    # ---- 正确性测试：单元 / 随机对拍 / 5 个真实场景 / 并发压测 ----
    # 纯模板实现，不链接 runtime，单独可执行便于定位失败算法。
    add_executable(go2cpp_cache_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/cache_test_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_headers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_lru.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_lru_k.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_two_queue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_tiny_lfu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_gdsf.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_arc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_clock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_interface.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_random.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_scenarios.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/test_stress.cpp)
    target_link_libraries(go2cpp_cache_test PRIVATE go2cpp_cache)
    target_compile_options(go2cpp_cache_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_cache_test COMMAND go2cpp_cache_test)
    set_tests_properties(go2cpp_cache_test PROPERTIES TIMEOUT 900)

    # ---- 多场景基准：hotspot / scan / weighted / cdn / mixed ----
    # 默认 `--quick` 供 ctest 快速回归；完整数据用 `--full` 手动跑。
    add_executable(go2cpp_cache_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cache/cache_bench.cpp)
    target_link_libraries(go2cpp_cache_bench PRIVATE go2cpp_cache)
    target_compile_options(go2cpp_cache_bench PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_cache_bench COMMAND go2cpp_cache_bench --quick)
    set_tests_properties(go2cpp_cache_bench PROPERTIES TIMEOUT 600)
endif()
