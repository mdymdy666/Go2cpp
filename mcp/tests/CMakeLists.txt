add_executable(test_json_rpc test_json_rpc.cpp)
target_link_libraries(test_json_rpc PRIVATE mcp_core)
target_compile_options(test_json_rpc PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME json_rpc COMMAND test_json_rpc)

add_executable(test_registry test_registry.cpp)
target_link_libraries(test_registry PRIVATE mcp_core)
target_compile_options(test_registry PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME registry COMMAND test_registry)

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE mcp_core)
target_compile_options(test_tools PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME tools COMMAND test_tools)

add_executable(test_vector test_vector.cpp)
target_link_libraries(test_vector PRIVATE mcp_core)
target_compile_options(test_vector PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vector COMMAND test_vector)

add_executable(test_validation test_validation.cpp)
target_link_libraries(test_validation PRIVATE mcp_core)
target_compile_options(test_validation PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME validation COMMAND test_validation)

add_executable(test_retrieval test_retrieval.cpp)
target_link_libraries(test_retrieval PRIVATE mcp_core)
target_compile_options(test_retrieval PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME retrieval COMMAND test_retrieval)

# 进程执行适配层（go2cpp::os::Run）：启动/输出/超时/退出码/并发/泄漏。
add_executable(test_process test_process.cpp)
target_include_directories(test_process PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_process PRIVATE mcp_core)
target_compile_options(test_process PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME process COMMAND test_process)
set_tests_properties(process PROPERTIES TIMEOUT 120)

# ROS 2 工具组：用假 ros2 脚本覆盖成功/超时/错误退出码/命令缺失/白名单。
add_executable(test_robot_control test_robot_control.cpp)
target_link_libraries(test_robot_control PRIVATE mcp_core)
target_compile_options(test_robot_control PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME robot_control COMMAND test_robot_control)
set_tests_properties(robot_control PROPERTIES TIMEOUT 120)


# ---------------------------------------------------------------------------
# 插件主动通知：假插件 .so + 单测
# ---------------------------------------------------------------------------
# 假插件单独编译成 MODULE 库，放在 test_plugins/ 专用目录：e2e 用 --plugins 指向
# 该目录时只会加载它，不会混入参考项目的 8 个插件。
add_library(fake_notify_plugin MODULE fake_plugin/notify_plugin.cpp)
target_include_directories(fake_notify_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
)
target_compile_options(fake_notify_plugin PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(fake_notify_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fake_notify_plugin"
)

set(FAKE_PLUGIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_plugins)
add_custom_command(TARGET fake_notify_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FAKE_PLUGIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:fake_notify_plugin> ${FAKE_PLUGIN_DIR}
)

# 正常转发 / 非法帧拒绝 / 工作线程 / 无客户端丢弃 / 卸载后不回调 / 并发投递。
add_executable(test_plugin_notify test_plugin_notify.cpp)
target_include_directories(test_plugin_notify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_plugin_notify PRIVATE mcp_core)
target_compile_options(test_plugin_notify PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(test_plugin_notify fake_notify_plugin)
add_test(NAME plugin_notify COMMAND test_plugin_notify ${FAKE_PLUGIN_DIR})

# 监控工具（离线）：MockMonitorClient 注入，验证四个工具与参数透传，不启动
# Manager、不走网络。
add_executable(test_monitor_client test_monitor_client.cpp)
target_link_libraries(test_monitor_client PRIVATE mcp_core)
target_compile_options(test_monitor_client PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_client COMMAND test_monitor_client)
set_tests_properties(monitor_client PROPERTIES TIMEOUT 120)

# 监控工具（真实 HTTP）：注册 / schema 校验 / Manager 未启动 / 真实 Manager。
add_executable(test_monitor_tools test_monitor_tools.cpp)
target_link_libraries(test_monitor_tools PRIVATE mcp_core go2cpp_os_monitor)
target_compile_options(test_monitor_tools PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_tools COMMAND test_monitor_tools)
set_tests_properties(monitor_tools PROPERTIES TIMEOUT 120)

# e2e 辅助进程：真实 Manager，端口由内核分配并通过 stdout 报告（PORT=<n>）。
add_executable(fake_monitor_manager fake_monitor_manager.cpp)
target_link_libraries(fake_monitor_manager PRIVATE go2cpp_os_monitor)
target_compile_options(fake_monitor_manager PRIVATE -Wall -Wextra -Wpedantic)

# 边界：Manager 未启动 / HTTP 500/404 / 超时 / 畸形响应 / 空客户端。
# 用测试内的假 HTTP 服务端制造故障，不依赖真实 Manager 的异常路径。
add_executable(test_monitor_boundaries test_monitor_boundaries.cpp)
target_link_libraries(test_monitor_boundaries PRIVATE mcp_core go2cpp_os_monitor)
target_compile_options(test_monitor_boundaries PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_boundaries COMMAND test_monitor_boundaries)
set_tests_properties(monitor_boundaries PROPERTIES TIMEOUT 120)

# 容量淘汰：真实 Manager + 真实 HTTP push，覆盖 256 默认容量、1/0 极端容量、
# 非法 push 拒绝与 4×100 并发推送。
add_executable(test_monitor_capacity test_monitor_capacity.cpp)
target_link_libraries(test_monitor_capacity PRIVATE mcp_core go2cpp_os_monitor)
target_compile_options(test_monitor_capacity PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_capacity COMMAND test_monitor_capacity)
set_tests_properties(monitor_capacity PROPERTIES TIMEOUT 120)

# 并发：100 并发 MCP 工具调用（Mock 客户端），验证响应隔离、混合工具与
# 并发注册/注销干扰。
add_executable(test_monitor_concurrent_tools test_monitor_concurrent_tools.cpp)
target_link_libraries(test_monitor_concurrent_tools PRIVATE mcp_core)
target_compile_options(test_monitor_concurrent_tools PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_concurrent_tools COMMAND test_monitor_concurrent_tools)
set_tests_properties(monitor_concurrent_tools PROPERTIES TIMEOUT 120)

# 参数边界：limit 0/1/256/257、timeout 0/100/60000/60001 矩阵 + 1000 组随机
# fuzz + handler 侧 clamp。
add_executable(test_monitor_args_fuzz test_monitor_args_fuzz.cpp)
target_link_libraries(test_monitor_args_fuzz PRIVATE mcp_core)
target_compile_options(test_monitor_args_fuzz PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME monitor_args_fuzz COMMAND test_monitor_args_fuzz)
set_tests_properties(monitor_args_fuzz PROPERTIES TIMEOUT 120)

# e2e：真实 Manager + 真实 mcp_server（stdio），验证四个工具与失败诊断。
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME monitor_e2e
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_monitor_e2e.py
                     $<TARGET_FILE:mcp_server> $<TARGET_FILE:fake_monitor_manager>)
    set_tests_properties(monitor_e2e PROPERTIES TIMEOUT 120)

    # HTTP 故障 e2e：真实 mcp_server（stdio）+ 假故障 HTTP 源。
    # 覆盖 500/404/超时/截断/非 JSON/垃圾字节/连接拒绝，并断言进程存活。
    add_test(NAME monitor_http_faults
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_monitor_http_faults.py
                     $<TARGET_FILE:mcp_server>)
    set_tests_properties(monitor_http_faults PROPERTIES TIMEOUT 120)

    # 文档一致性：README/docs 声明的 ctest 数、工具数与实际 ctest -N /
    # tools/list 一致，测试地图逐项覆盖全部 ctest。
    add_test(NAME docs_consistency
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_docs_consistency.py
                     $<TARGET_FILE:mcp_server> ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
    set_tests_properties(docs_consistency PROPERTIES TIMEOUT 120)
else()
    message(STATUS "未找到 Python3，跳过 monitor_e2e")
endif()
