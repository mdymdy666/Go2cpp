# ---------------------------------------------------------------------------
# Bento Transport Protocol v0.1（自研最简 UDP 可靠/不可靠混合协议）
#
# 设计参考 yojimbo 的 reliable 层（包序号/ACK 位图/PTO 重传/分片重组），
# 但代码为独立实现：12B 帧头、32 位 seq、累积 ACK + 32 位 SACK、
# 每通道独立不可靠 seq（DropStale）、dep_seq/BARRIER 因果原语。
#
# 默认 OFF：不影响既有库、测试、安装面。
# 打开方式：
#   cmake -S . -B build-bento -DGO2CPP_WITH_BENTO=ON
# 测试由 GO2CPP_BENTO_TESTS 独立控制（默认 ON），不依赖
# GO2CPP_BUILD_TESTS，因此可以只构建 Bento 与它的 3+1 个测试目标：
#   cmake -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_BENTO "Build the Bento minimal UDP transport" OFF)

# standalone 模式下本文件先于 Go2CppModules.cmake 被 include，需要自己解析
# Threads；完整模式下重复 find_package 是幂等的。
include(GNUInstallDirs)
find_package(Threads REQUIRED)

if (GO2CPP_WITH_BENTO)
    # 默认构建类型为空（-O0）。Bento 的加密性能验收要求 >=50MB/s，
    # ChaCha20/Poly1305 在 -O0 下达不到；只给本模块与其测试加 -O2，
    # 不改变项目其他目标的默认优化级别。
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(BENTO_OPT_FLAGS -O2)
    else()
        set(BENTO_OPT_FLAGS)
    endif()

    add_library(go2cpp_bento STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/relation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/crypto.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/congestion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/bandwidth.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/link.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/peer_hub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/session_control.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/tcp_proxy.cpp)

    target_include_directories(go2cpp_bento
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento PUBLIC cxx_std_17)
    # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
    target_compile_options(go2cpp_bento PRIVATE -Wall -Wextra -Wpedantic -Werror
        ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento PUBLIC Threads::Threads)
    set_target_properties(go2cpp_bento PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento ALIAS go2cpp_bento)

    option(GO2CPP_BENTO_TESTS "Build Bento transport tests" ON)
    if (GO2CPP_BENTO_TESTS)
        enable_testing()

        add_executable(bento_test_link_loopback
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_link_loopback.cpp)
        add_executable(bento_test_unreliable_dropstale
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_unreliable_dropstale.cpp)
        add_executable(bento_test_video_fragment
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_video_fragment.cpp)
        add_executable(bento_test_rtt_pto
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_rtt_pto.cpp)
        add_executable(bento_test_fast_retransmit
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_fast_retransmit.cpp)
        add_executable(bento_test_stream_fragment
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_stream_fragment.cpp)
        add_executable(bento_test_crypto
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_crypto.cpp)
        add_executable(bento_test_secure_link
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_secure_link.cpp)
        add_executable(bento_test_secure_injection
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_secure_injection.cpp)
        add_executable(bento_test_congestion
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_congestion.cpp)
        add_executable(bento_test_flow_control
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_flow_control.cpp)
        add_executable(bento_test_bandwidth
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_bandwidth.cpp)
        add_executable(bento_test_stats_json
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_stats_json.cpp)
        add_executable(bento_test_ipv6
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_ipv6.cpp)
        add_executable(bento_test_concurrent_send
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_concurrent_send.cpp)
        add_executable(bento_test_conn_migration
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_conn_migration.cpp)
        add_executable(bento_test_fec
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_fec.cpp)
        add_executable(bento_test_pmtud
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_pmtud.cpp)
        add_executable(bento_test_zero_rtt
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_zero_rtt.cpp)
        add_executable(bento_test_peer_hub
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_peer_hub.cpp)
        add_executable(bento_test_tcp_proxy
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_tcp_proxy.cpp)
        add_executable(bento_test_session_control
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_session_control.cpp)
        add_executable(bento_test_peer_hub_secure
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_peer_hub_secure.cpp)
        add_executable(bento_test_log_hook
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_log_hook.cpp)
        add_executable(bento_netem_probe
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/netem_probe.cpp)

        foreach(bento_test_target
                bento_test_link_loopback
                bento_test_unreliable_dropstale
                bento_test_video_fragment
                bento_test_rtt_pto
                bento_test_fast_retransmit
                bento_test_stream_fragment
                bento_test_crypto
                bento_test_secure_link
                bento_test_secure_injection
                bento_test_congestion
                bento_test_flow_control
                bento_test_bandwidth
                bento_test_stats_json
                bento_test_ipv6
                bento_test_concurrent_send
                bento_test_conn_migration
                bento_test_fec
                bento_test_pmtud
                bento_test_zero_rtt
                bento_test_peer_hub
                bento_test_tcp_proxy
                bento_test_session_control
                bento_test_peer_hub_secure
                bento_test_log_hook
                bento_netem_probe)
            target_include_directories(${bento_test_target} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_link_libraries(${bento_test_target} PRIVATE go2cpp_bento)
            target_compile_options(${bento_test_target} PRIVATE
                -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        endforeach()

        add_test(NAME bento_link_loopback COMMAND bento_test_link_loopback)
        add_test(NAME bento_unreliable_dropstale COMMAND bento_test_unreliable_dropstale)
        add_test(NAME bento_video_fragment COMMAND bento_test_video_fragment)
        add_test(NAME bento_rtt_pto COMMAND bento_test_rtt_pto)
        add_test(NAME bento_fast_retransmit COMMAND bento_test_fast_retransmit)
        add_test(NAME bento_stream_fragment COMMAND bento_test_stream_fragment)
        add_test(NAME bento_crypto COMMAND bento_test_crypto)
        add_test(NAME bento_secure_link COMMAND bento_test_secure_link)
        add_test(NAME bento_secure_injection COMMAND bento_test_secure_injection)
        add_test(NAME bento_congestion COMMAND bento_test_congestion)
        add_test(NAME bento_flow_control COMMAND bento_test_flow_control)
        add_test(NAME bento_bandwidth COMMAND bento_test_bandwidth)
        add_test(NAME bento_stats_json COMMAND bento_test_stats_json)
        add_test(NAME bento_ipv6 COMMAND bento_test_ipv6)
        add_test(NAME bento_concurrent_send COMMAND bento_test_concurrent_send)
        add_test(NAME bento_conn_migration COMMAND bento_test_conn_migration)
        add_test(NAME bento_fec COMMAND bento_test_fec)
        add_test(NAME bento_pmtud COMMAND bento_test_pmtud)
        add_test(NAME bento_zero_rtt COMMAND bento_test_zero_rtt)
        add_test(NAME bento_peer_hub COMMAND bento_test_peer_hub)
        add_test(NAME bento_tcp_proxy COMMAND bento_test_tcp_proxy)
        add_test(NAME bento_session_control COMMAND bento_test_session_control)
        add_test(NAME bento_peer_hub_secure COMMAND bento_test_peer_hub_secure)
        add_test(NAME bento_log_hook COMMAND bento_test_log_hook)
        # 用户态弱网代理：无 CAP_NET_ADMIN 时替代 tc netem 的可重复版本。
        # 20% 双向丢包 + 200ms 单向延迟，可靠流要求 100% 按序。
        # 60s 收尾窗口：20% 丢包下 PTO 最坏会退避到 2s，40s 偶发尾部未收完。
        add_test(NAME bento_weaknet_proxy
            COMMAND bento_netem_probe --port 34568 --messages 200 --payload 256
                    --timeout-ms 60000 --loss 20 --delay-ms 200)
        # netem 需要 root/CAP_NET_ADMIN；脚本在无权限时 exit 77 -> SKIP。
        add_test(NAME bento_netem
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_netem.sh
                    $<TARGET_FILE:bento_netem_probe>)
        set_tests_properties(
            bento_link_loopback bento_unreliable_dropstale bento_video_fragment
            bento_rtt_pto bento_fast_retransmit bento_stream_fragment
            bento_crypto bento_secure_link bento_secure_injection bento_congestion
            bento_flow_control bento_bandwidth bento_stats_json bento_ipv6
            bento_concurrent_send bento_conn_migration bento_fec bento_pmtud
            bento_zero_rtt bento_peer_hub bento_tcp_proxy
            bento_session_control bento_peer_hub_secure bento_log_hook
            PROPERTIES TIMEOUT 90)
        set_tests_properties(bento_weaknet_proxy PROPERTIES TIMEOUT 150)
        set_tests_properties(bento_netem PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()

    message(STATUS "Bento transport: ON (tests=${GO2CPP_BENTO_TESTS})")
else()
    message(STATUS "Bento transport: OFF (set GO2CPP_WITH_BENTO=ON to enable)")
endif()
