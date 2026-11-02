# ---------------------------------------------------------------------------
# RPC 框架（include/go2cpp/rpc/，Go 风格 服务注册/调用/流式）
#
# 目标：
#   - 协议：frame::FrameCodec（magic/version/CRC + 半包重组）+ Reflect 序列化；
#   - 服务注册：ServiceRegistry（完整方法名 -> 类型擦除的方法绑定）；
#   - 调用：Client::Call（同步）/ AsyncCall（异步+取消）/ ClientStream（流式）；
#   - 传输：TCP（POSIX，阻塞 + poll），WebSocket 适配器见 websocket_transport.hpp。
#
# 默认 ON：只新增一个静态库与若干测试目标，不改变既有目标。需要关闭时：
#   -DGO2CPP_WITH_RPC=OFF
# 测试由 GO2CPP_BUILD_TESTS + GO2CPP_RPC_TESTS 双开关控制（后者默认 ON）。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_RPC "Build the RPC framework (go2cpp::rpc)" ON)

if (NOT GO2CPP_WITH_RPC)
    return()
endif()

if (NOT TARGET go2cpp_frame OR NOT TARGET go2cpp_reflect)
    message(FATAL_ERROR
        "GO2CPP_WITH_RPC 需要 go2cpp_frame 与 go2cpp_reflect 目标（分别由 "
        "cmake/Go2CppFrame.cmake 与 cmake/Go2CppReflect.cmake 定义）。")
endif()

find_package(Threads REQUIRED)

add_library(go2cpp_rpc STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/status.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/transport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/client.cpp)
add_library(go2cpp::rpc ALIAS go2cpp_rpc)
target_include_directories(go2cpp_rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(go2cpp_rpc PUBLIC cxx_std_17)
# 新增目标按零警告维护：模块内全部代码都在本次变更范围内。
target_compile_options(go2cpp_rpc PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(go2cpp_rpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(go2cpp_rpc PUBLIC go2cpp_frame go2cpp_reflect Threads::Threads)

option(GO2CPP_RPC_TESTS "Build RPC framework tests" ON)

if (GO2CPP_BUILD_TESTS AND GO2CPP_RPC_TESTS)
    enable_testing()
    set(GO2CPP_RPC_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc)

    # 测试目标 -> 源文件（三者共用 tests/rpc/rpc_test_support.hpp）。
    add_executable(go2cpp_rpc_protocol_test
        ${GO2CPP_RPC_TEST_DIR}/test_rpc_protocol.cpp)
    add_executable(go2cpp_rpc_test
        ${GO2CPP_RPC_TEST_DIR}/test_rpc.cpp)
    add_executable(go2cpp_rpc_stream_test
        ${GO2CPP_RPC_TEST_DIR}/test_rpc_stream.cpp)
    foreach(rpc_test go2cpp_rpc_protocol_test go2cpp_rpc_test go2cpp_rpc_stream_test)
        target_include_directories(${rpc_test} PRIVATE ${GO2CPP_RPC_TEST_DIR})
        target_link_libraries(${rpc_test} PRIVATE go2cpp_rpc)
        target_compile_options(${rpc_test} PRIVATE -O2 -Wall -Wextra -Wpedantic -Werror)
    endforeach()

    add_test(NAME go2cpp_rpc_protocol_test COMMAND go2cpp_rpc_protocol_test)
    add_test(NAME go2cpp_rpc_test COMMAND go2cpp_rpc_test)
    add_test(NAME go2cpp_rpc_stream_test COMMAND go2cpp_rpc_stream_test)
    set_tests_properties(go2cpp_rpc_protocol_test PROPERTIES TIMEOUT 600)
    # 端到端含 1e6 次同步调用 + 1e5 次并发调用：给足时间，不用缩小规模掩盖性能。
    set_tests_properties(go2cpp_rpc_test PROPERTIES TIMEOUT 1800)
    set_tests_properties(go2cpp_rpc_stream_test PROPERTIES TIMEOUT 900)

    # 性能基准（延迟分布/协议层 1e7/网络与序列化下限/前-后对比）。
    add_executable(go2cpp_rpc_bench ${GO2CPP_RPC_TEST_DIR}/bench_rpc.cpp)
    target_include_directories(go2cpp_rpc_bench PRIVATE ${GO2CPP_RPC_TEST_DIR})
    target_link_libraries(go2cpp_rpc_bench PRIVATE go2cpp_rpc)
    target_compile_options(go2cpp_rpc_bench PRIVATE -O3 -Wall -Wextra -Wpedantic -Werror)

    # WebSocket 适配器端到端（只在 network 目标存在时构建；不拉入额外默认依赖）。
    if (TARGET go2cpp_network)
        add_executable(go2cpp_rpc_websocket_test
            ${GO2CPP_RPC_TEST_DIR}/test_rpc_websocket.cpp)
        target_include_directories(go2cpp_rpc_websocket_test PRIVATE ${GO2CPP_RPC_TEST_DIR})
        target_link_libraries(go2cpp_rpc_websocket_test PRIVATE
            go2cpp_rpc go2cpp_network)
        target_compile_options(go2cpp_rpc_websocket_test PRIVATE
            -O2 -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME go2cpp_rpc_websocket_test COMMAND go2cpp_rpc_websocket_test)
        set_tests_properties(go2cpp_rpc_websocket_test PROPERTIES TIMEOUT 300)
    endif()
endif()

# ---------------------------------------------------------------------------
# RPC over QUIC 风格传输（UDP + Bento 可靠层，可选）
#
# 独立静态库：go2cpp_rpc 只依赖 frame/reflect，不把 Bento（UDP/加密/拥塞）
# 拖进默认链接面。只有 GO2CPP_WITH_BENTO=ON 时才构建。
#
# 目标：
#   - go2cpp_rpc_quic：QuicClient / QuicListener / QuicStream（rpc::Transport）；
#   - go2cpp_rpc_quic_test：端到端/多流复用/0-RTT/丢包/性能（ctest）。
# ---------------------------------------------------------------------------
if (TARGET go2cpp_bento)
    add_library(go2cpp_rpc_quic STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc/transport_quic.cpp)
    add_library(go2cpp::rpc_quic ALIAS go2cpp_rpc_quic)
    target_include_directories(go2cpp_rpc_quic
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_rpc_quic PUBLIC cxx_std_17)
    target_compile_options(go2cpp_rpc_quic PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(go2cpp_rpc_quic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(go2cpp_rpc_quic
        PUBLIC go2cpp_rpc go2cpp_bento Threads::Threads)

    if (GO2CPP_BUILD_TESTS AND GO2CPP_RPC_TESTS)
        add_executable(go2cpp_rpc_quic_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc/test_rpc_quic.cpp)
        target_include_directories(go2cpp_rpc_quic_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc)
        target_link_libraries(go2cpp_rpc_quic_test PRIVATE go2cpp_rpc_quic)
        target_compile_options(go2cpp_rpc_quic_test PRIVATE
            -O2 -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME go2cpp_rpc_quic_test COMMAND go2cpp_rpc_quic_test)
        set_tests_properties(go2cpp_rpc_quic_test PROPERTIES TIMEOUT 1800)
    endif()
endif()
