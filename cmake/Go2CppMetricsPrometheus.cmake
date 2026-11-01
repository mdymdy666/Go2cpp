# Prometheus 文本导出 + HTTP /metrics 端点（新增模块，不改动现有 metrics 源）。
#
# 依赖方向：go2cpp_metrics -> go2cpp_network。
# `endpoint.cpp` 需要 `http::Headers::set` 的实现（src/network/http.cpp），
# 因此 go2cpp_metrics 对 go2cpp_network 是 PUBLIC 依赖；network 不反向依赖
# metrics，目标依赖图无环。
#
# 不新增库目标：新增的两个 .cpp 直接挂到已存在的 go2cpp_metrics 上，
# install(EXPORT) 的模块清单保持不变。

target_sources(go2cpp_metrics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/prometheus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/endpoint.cpp)
target_link_libraries(go2cpp_metrics PUBLIC go2cpp_network)

if (GO2CPP_BUILD_TESTS)
    # ---- 格式 / 分位数 / 并发测试：只依赖 go2cpp_metrics ----
    add_executable(go2cpp_metrics_prometheus_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics/test_prometheus.cpp)
    target_link_libraries(go2cpp_metrics_prometheus_test PRIVATE go2cpp_metrics)
    target_compile_options(go2cpp_metrics_prometheus_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_metrics_prometheus_test
             COMMAND go2cpp_metrics_prometheus_test)
    set_tests_properties(go2cpp_metrics_prometheus_test PROPERTIES TIMEOUT 300)

    # ---- 1e6 指标导出性能基准 ----
    # 不注册进 ctest 的常规路径：全量 ctest 会跑几十个目标，基准默认 1e6
    # 系列需要几秒；需要时手动执行 ./go2cpp_metrics_prometheus_bench。
    add_executable(go2cpp_metrics_prometheus_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics/bench_prometheus.cpp)
    target_link_libraries(go2cpp_metrics_prometheus_bench PRIVATE go2cpp_metrics)
    target_compile_options(go2cpp_metrics_prometheus_bench PRIVATE
        -Wall -Wextra -Wpedantic)

    # ---- HTTP /metrics 端到端测试：需要真实 HttpServer（go2cpp_bento_http） ----
    if (GO2CPP_WITH_BENTO AND TARGET go2cpp_bento_http)
        add_executable(go2cpp_metrics_endpoint_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics/test_prometheus_endpoint.cpp)
        target_link_libraries(go2cpp_metrics_endpoint_test PRIVATE
            go2cpp_metrics go2cpp_bento_http)
        target_compile_options(go2cpp_metrics_endpoint_test PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME go2cpp_metrics_endpoint_test
                 COMMAND go2cpp_metrics_endpoint_test)
        set_tests_properties(go2cpp_metrics_endpoint_test PROPERTIES TIMEOUT 120)
    else()
        message(STATUS
            "go2cpp_metrics_endpoint_test skipped: GO2CPP_WITH_BENTO=OFF")
    endif()
endif()
