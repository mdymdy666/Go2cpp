# ---------------------------------------------------------------------------
# 运维能力：日志轮转（include/go2cpp/logging/rotate.hpp）+ 配置热更新
# （include/go2cpp/config/watch.hpp）。
#
# 两个模块都是 header-only，不新增库目标，也不改既有模块的编译/链接面：
#   - rotate.hpp 组合 log.hpp 的 LogSink 与 compress::Gzip；
#   - watch.hpp 只依赖 C++17 标准库 + POSIX inotify（Linux 外自动轮询）。
#
# 测试按「独立可执行 + 自带 main」注册，便于单独运行定位失败场景：
#   - go2cpp_log_rotate_test：1e6 条/轮转 10 次的压测 + 8 个场景；
#   - go2cpp_config_watch_test：1e4 次变更 + 无锁读并发 + 轮询回退。
#
# 默认 ON；`-DGO2CPP_WITH_OPS_LOG_CONFIG=OFF` 可整体关闭。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_OPS_LOG_CONFIG
       "Build ops log-rotation / config-watch headers and tests" ON)

if (NOT GO2CPP_WITH_OPS_LOG_CONFIG)
    return()
endif()

if (NOT GO2CPP_BUILD_TESTS)
    return()
endif()

if (NOT TARGET go2cpp_log OR NOT TARGET go2cpp_compress)
    message(FATAL_ERROR
        "go2cpp_log_rotate_test 需要 go2cpp_log 与 go2cpp_compress 目标"
        "（由 cmake/Go2CppModules.cmake 定义）；请在全量构建中使用。")
endif()

# ---- 日志轮转：1e6 条 / 轮转 10 次 / 无丢失无乱序 ----
# 链接真实 log 与 compress 目标，确保 LogSink 适配器与自研 gzip 是生产路径，
# 而不是测试里另写的桩实现。
add_executable(go2cpp_log_rotate_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/logging/test_log_rotate.cpp)
target_link_libraries(go2cpp_log_rotate_test PRIVATE
    go2cpp_log go2cpp_compress)
target_compile_options(go2cpp_log_rotate_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -Werror)
add_test(NAME go2cpp_log_rotate_test COMMAND go2cpp_log_rotate_test)
set_tests_properties(go2cpp_log_rotate_test PROPERTIES TIMEOUT 600)

# ---- 配置热更新：1e4 次变更 / 无锁读 / 原子替换 / 轮询回退 ----
# header-only：不链接 runtime，避免把测试与调度器初始化绑定。
add_executable(go2cpp_config_watch_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/test_config_watch.cpp)
target_include_directories(go2cpp_config_watch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(go2cpp_config_watch_test PRIVATE Threads::Threads)
target_compile_options(go2cpp_config_watch_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -Werror)
add_test(NAME go2cpp_config_watch_test COMMAND go2cpp_config_watch_test)
set_tests_properties(go2cpp_config_watch_test PROPERTIES TIMEOUT 300)
