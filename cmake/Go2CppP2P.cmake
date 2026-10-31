# ---------------------------------------------------------------------------
# P2P 公网穿透模块（STUN / ICE 简化 / UDP 打洞 / 简化 TURN / TCP 同时打开）
#
# 零外部依赖：只使用 libc、pthread 与 Linux socket API；不链接
# go2cpp_runtime，因此既能被主运行时使用，也能被独立工具/测试直接链接，
# 且不受 hook 的 syscall 拦截影响。
#
# 默认 ON：模块体量小、无外部依赖；关闭方式 -DGO2CPP_WITH_P2P=OFF。
# 测试由独立开关 GO2CPP_P2P_TESTS 控制（默认 ON），不依赖
# GO2CPP_BUILD_TESTS，因此可以只构建 P2P 与其测试：
#   cmake -S . -B build-p2p -DGO2CPP_WITH_P2P=ON -DGO2CPP_P2P_TESTS=ON
#
# 文档：docs/modules/p2p-nat-traversal.md
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_P2P "Build the P2P NAT traversal module" ON)
option(GO2CPP_P2P_TESTS "Build P2P tests (STUN/ICE/hole punching/relay)" ON)

if (GO2CPP_WITH_P2P)
    add_library(go2cpp_p2p
        # 基础类型与传输抽象
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/endpoint.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/random.cpp
        # STUN（RFC 5389 子集 + RFC 3489 NAT 类型检测 + 自写 echo 服务器）
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/stun_message.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/stun_client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/stun_server.cpp
        # ICE 候选与信令
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/candidate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/signaling.cpp
        # 简化 TURN（自定义中继协议 + Allocation/Send/Data 数据面）
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/relay_protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/turn_server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/turn_client.cpp
        # 通道（打洞 + ICE 检查 + 中继降级 + 数据面）与 TCP 同时打开
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/p2p_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p/tcp_punch.cpp)

    target_include_directories(go2cpp_p2p
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            # relay_protocol.hpp 是内部头，不对外导出。
            ${CMAKE_CURRENT_SOURCE_DIR}/src/p2p)
    target_compile_features(go2cpp_p2p PUBLIC cxx_std_17)
    # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
    target_compile_options(go2cpp_p2p PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(go2cpp_p2p PUBLIC Threads::Threads)
    set_target_properties(go2cpp_p2p PROPERTIES
        EXPORT_NAME p2p
        POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::p2p ALIAS go2cpp_p2p)
    # 纳入统一的安装/导出集合（Go2CppInstall.cmake 只导出该列表）。
    list(APPEND GO2CPP_MODULE_TARGETS go2cpp_p2p)

    if (GO2CPP_P2P_TESTS)
        enable_testing()
        add_executable(p2p_test_stun
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p/test_stun.cpp)
        add_executable(p2p_test_p2p
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p/test_p2p.cpp)
        add_executable(p2p_test_tcp_punch
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p/test_tcp_punch.cpp)
        add_executable(p2p_test_p2p_interprocess
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p/test_p2p_interprocess.cpp)
        foreach(p2p_test IN ITEMS
                p2p_test_stun p2p_test_p2p p2p_test_tcp_punch
                p2p_test_p2p_interprocess)
            target_link_libraries(${p2p_test} PRIVATE go2cpp_p2p)
            target_include_directories(${p2p_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p)
            target_compile_options(${p2p_test} PRIVATE
                -O2 -Wall -Wextra -Wpedantic)
            add_test(NAME ${p2p_test} COMMAND ${p2p_test})
            # 测试含 3~5s 的连接超时与压测；慢机器留足余量。
            set_tests_properties(${p2p_test} PROPERTIES TIMEOUT 300)
        endforeach()

        # 基准只构建、不注册 ctest：数据随机器负载变化，且中继吞吐需要
        # 限速发送（避免打满 rmem_max 后被内核丢包），不适合当回归。
        add_executable(p2p_bench
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/p2p/bench_p2p.cpp)
        target_link_libraries(p2p_bench PRIVATE go2cpp_p2p)
        target_compile_options(p2p_bench PRIVATE -O2 -Wall -Wextra -Wpedantic)
    endif()

    message(STATUS "P2P NAT traversal: ON (tests=${GO2CPP_P2P_TESTS})")
else()
    message(STATUS "P2P NAT traversal: OFF")
endif()
