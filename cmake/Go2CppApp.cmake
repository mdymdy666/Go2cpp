# ---------------------------------------------------------------------------
# App Framework（应用层流表框架）
#
# 设计参考 HTTP/2 RFC 7540 的流表 + 游戏资源分发：
#   - 9B 帧头（length/type/flags/R+stream_id），HEADERS/DATA/RST/GOAWAY/
#     PRIORITY/WINDOW_UPDATE/PING/SETTINGS；
#   - 流状态机、连接/流两级窗口、依赖树 + 权重 WFQ 调度；
#   - 资源分块 + SHA-256/CRC-32 校验 + 断点续传；
#   - KV 流（小表单帧 / 大表流式 / 每键一流）。
#
# 默认 OFF，打开方式：
#   cmake -S . -B build-app -DGO2CPP_WITH_APP=ON
# 测试由 GO2CPP_APP_TESTS 独立控制（默认 ON）：
#   cmake -DGO2CPP_WITH_APP=ON -DGO2CPP_APP_TESTS=OFF
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_APP "Build the application-layer stream framework (app/)" OFF)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

if (GO2CPP_WITH_APP)
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(APP_OPT_FLAGS -O2)
    else()
        set(APP_OPT_FLAGS)
    endif()

    set(GO2CPP_APP_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/kv.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/stream_table.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/kv_stream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/connection.cpp)

    add_library(go2cpp_app STATIC ${GO2CPP_APP_CORE_SOURCES})

    target_include_directories(go2cpp_app
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_app PUBLIC cxx_std_17)
    target_compile_options(go2cpp_app PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${APP_OPT_FLAGS})
    target_link_libraries(go2cpp_app PUBLIC Threads::Threads)
    set_target_properties(go2cpp_app PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::app ALIAS go2cpp_app)

    # 校验和复用仓库的 hash 模块；standalone 构建时把两个实现并入本目标。
    if (TARGET go2cpp_hash)
        target_link_libraries(go2cpp_app PUBLIC go2cpp_hash)
    else()
        target_sources(go2cpp_app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/crc32.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/sha256.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/hash.cpp)
    endif()

    # ---- 与 Bento 对接：20B 通用帧桥 + Link（UDP 可靠+不可靠）适配 ----
    # 只有 Bento 目标存在时才编译；核心库保持零 Bento 依赖。
    if (TARGET go2cpp_bento)
        target_sources(go2cpp_app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/bento_bridge.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/bento_link_transport.cpp)
        target_link_libraries(go2cpp_app PUBLIC go2cpp_bento)
        target_compile_definitions(go2cpp_app PUBLIC GO2CPP_APP_HAS_BENTO=1)
    else()
        message(STATUS "app: Bento bridge/Link transport OFF (go2cpp_bento not built)")
    endif()

    # ---- 与 HttpServer（TCP）对接：帧批隧道 ----
    if (TARGET go2cpp_bento_http AND TARGET go2cpp_network)
        target_sources(go2cpp_app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/http_tunnel.cpp)
        target_link_libraries(go2cpp_app PUBLIC go2cpp_bento_http)
        target_compile_definitions(go2cpp_app PUBLIC GO2CPP_APP_HAS_HTTP=1)
    else()
        message(STATUS "app: HTTP tunnel OFF (go2cpp_bento_http not built)")
    endif()
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
option(GO2CPP_APP_TESTS "Build app framework tests" ON)

if (GO2CPP_WITH_APP AND GO2CPP_APP_TESTS)
    enable_testing()
    set(GO2CPP_APP_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/app)

    set(GO2CPP_APP_TESTS_CORE
        app_test_frame
        app_test_stream_table
        app_test_resource
        app_test_kv
        app_test_connection
        app_test_load)

    foreach(app_test ${GO2CPP_APP_TESTS_CORE})
        add_executable(${app_test} ${GO2CPP_APP_TEST_DIR}/${app_test}.cpp)
        target_include_directories(${app_test} PRIVATE ${GO2CPP_APP_TEST_DIR})
        target_link_libraries(${app_test} PRIVATE go2cpp_app)
        target_compile_options(${app_test} PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${APP_OPT_FLAGS})
    endforeach()

    add_test(NAME app_frame COMMAND app_test_frame)
    add_test(NAME app_stream_table COMMAND app_test_stream_table)
    add_test(NAME app_resource COMMAND app_test_resource)
    add_test(NAME app_kv COMMAND app_test_kv)
    add_test(NAME app_connection COMMAND app_test_connection)
    add_test(NAME app_load COMMAND app_test_load)
    set_tests_properties(app_frame app_stream_table app_resource app_kv
        app_connection PROPERTIES TIMEOUT 180)
    # 1 GiB 资源 + 1000 并发流的负载用例：给足时间，不掩盖真实性能。
    set_tests_properties(app_load PROPERTIES TIMEOUT 900)

    # Bento 帧桥 + Link 真实 UDP 回环。
    if (TARGET go2cpp_bento)
        add_executable(app_test_bento
            ${GO2CPP_APP_TEST_DIR}/app_test_bento.cpp)
        target_include_directories(app_test_bento PRIVATE
            ${GO2CPP_APP_TEST_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
        target_link_libraries(app_test_bento PRIVATE go2cpp_app go2cpp_bento)
        target_compile_options(app_test_bento PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${APP_OPT_FLAGS})
        add_test(NAME app_bento COMMAND app_test_bento)
        set_tests_properties(app_bento PROPERTIES TIMEOUT 300)
    endif()

    # HttpServer（真实 TCP）帧隧道。
    if (TARGET go2cpp_bento_http)
        add_executable(app_test_http
            ${GO2CPP_APP_TEST_DIR}/app_test_http.cpp)
        target_include_directories(app_test_http PRIVATE ${GO2CPP_APP_TEST_DIR})
        target_link_libraries(app_test_http PRIVATE
            go2cpp_app go2cpp_bento_http go2cpp_network)
        target_compile_options(app_test_http PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${APP_OPT_FLAGS})
        add_test(NAME app_http COMMAND app_test_http)
        set_tests_properties(app_http PROPERTIES TIMEOUT 300)
    endif()
endif()
