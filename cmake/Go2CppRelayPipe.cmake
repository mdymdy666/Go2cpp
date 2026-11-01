# ---------------------------------------------------------------------------
# RelayPipe（JinShan REQ-002）的独立构建接入
#
# 背景：`cmake/Go2CppRelay.cmake` 同时承载 Hub/Lease/Heartbeat（REQ-003/005），
# 由同目录的另一条工作流维护。为避免多 agent 同时改一个 CMake 文件、以及
# 「Hub 源码尚在编辑时拖垮 RelayPipe 编译」，这里把 RelayPipe 做成**独立
# 目标** `go2cpp_relay_pipe`：
#
#   - 库：src/relay/relay_pipe.cpp（只依赖 go2cpp_error / go2cpp_context）
#   - 测试：tests/relay/test_relay_pipe.cpp（ctest 名 relay_pipe，标签 relay）
#   - 进入统一安装/导出集合与 go2cpp_runtime 聚合目标
#
# 合并建议：当 Hub/Lease 目标稳定后，把 relay_pipe.cpp 直接加进
# `go2cpp_relay` 即可；本文件的目标名与别名（go2cpp::relay_pipe）可以保留为
# 兼容入口，也可以删除。当前独立目标不改变 go2cpp_relay 的任何语义。
#
# 本文件由顶层 CMakeLists.txt 直接 include（由 cmake/Go2CppRelay.cmake 的
# 注释约定），必须在 Go2CppModules.cmake 之后、Go2CppInstall.cmake 之前。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_RELAY_PIPE "Build RelayPipe (bidirectional byte relay)" ON)

if (NOT GO2CPP_WITH_RELAY_PIPE)
    message(STATUS "relay pipe: OFF (GO2CPP_WITH_RELAY_PIPE=OFF)")
    return()
endif()

add_library(go2cpp_relay_pipe
    ${CMAKE_CURRENT_SOURCE_DIR}/src/relay/relay_pipe.cpp)
add_library(go2cpp::relay_pipe ALIAS go2cpp_relay_pipe)

target_include_directories(go2cpp_relay_pipe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(go2cpp_relay_pipe PUBLIC cxx_std_17)
target_link_libraries(go2cpp_relay_pipe
    PUBLIC go2cpp_error go2cpp_context Threads::Threads)
target_compile_options(go2cpp_relay_pipe PRIVATE -Wall -Wextra -Wpedantic)

set_target_properties(go2cpp_relay_pipe PROPERTIES
    EXPORT_NAME relay_pipe
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(go2cpp_relay_pipe PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# 统一安装/导出与 runtime 聚合；Go2CppInstall.cmake 在本文件之后 include。
list(FIND GO2CPP_MODULE_TARGETS go2cpp_relay_pipe go2cpp_relay_pipe_module_index)
if (go2cpp_relay_pipe_module_index EQUAL -1)
    list(APPEND GO2CPP_MODULE_TARGETS go2cpp_relay_pipe)
endif()
if (TARGET go2cpp_runtime)
    target_link_libraries(go2cpp_runtime INTERFACE go2cpp_relay_pipe)
endif()

if (GO2CPP_BUILD_TESTS)
    # 自带 main 的独立可执行文件：失败时能直接定位到慢 peer/超时/压测，
    # 不并入 go2cpp_tests 的巨型二进制。
    add_executable(go2cpp_relay_pipe_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay/test_relay_pipe.cpp)
    target_link_libraries(go2cpp_relay_pipe_test PRIVATE
        go2cpp_relay_pipe go2cpp_hash)
    target_include_directories(go2cpp_relay_pipe_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay)
    target_compile_options(go2cpp_relay_pipe_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME relay_pipe COMMAND go2cpp_relay_pipe_test)
    # 1e8 双向 + 40 轮随机 + 慢 peer/超时场景；慢机器/Sanitizer 留足余量。
    set_tests_properties(relay_pipe PROPERTIES LABELS relay TIMEOUT 900)
    message(STATUS "relay pipe: ON (go2cpp_relay_pipe, test=relay_pipe)")
endif()
