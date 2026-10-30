# ---------------------------------------------------------------------------
# Bento Transport Protocol v2（自研最简 UDP 可靠/不可靠混合协议）
#
# 设计参考 yojimbo 的 reliable 层（包序号/ACK 位图/PTO 重传/分片重组），
# 但代码为独立实现：16B 帧头（含 conn_id）、32 位 seq、累积 ACK + 32 位 SACK、
# 每通道独立不可靠 seq（DropStale）、dep_seq/BARRIER 因果原语。
# 线格式以 tests/bento/PROTOCOL.md 的 v2 定义为准（旧版 12 字节帧头已废弃）。
#
# 默认 OFF：不影响既有库、测试、安装面。
# 打开方式：
#   cmake -S . -B build-bento -DGO2CPP_WITH_BENTO=ON
# 测试由 GO2CPP_BENTO_TESTS 独立控制（默认 ON），不依赖
# GO2CPP_BUILD_TESTS，因此可以只构建 Bento 与它自己的测试目标
# （数量以 CMake 的 add_test 注册为准，当前 26+）：
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/x25519.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/congestion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/bandwidth.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/link.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/link_server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/peer_hub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/session_control.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/tcp_proxy.cpp
        # MOD-13/14：配对协议与会话表。
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/pairing.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/session_table.cpp
        # MOD-10/11/12 通用模块：通用帧编解码、心跳、重连退避。
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/heartbeat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/backoff.cpp)

    target_include_directories(go2cpp_bento
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento PUBLIC cxx_std_17)
    # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
    target_compile_options(go2cpp_bento PRIVATE -Wall -Wextra -Wpedantic -Werror
        ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento PUBLIC Threads::Threads)
    # 帧校验和复用 hash 模块（CRC-32/IEEE，slicing-by-8）。完整构建下该目标
    # 必然已存在；standalone 只构建 Bento 时把 hash 的两个源文件并入本目标，
    # 避免为两处 CRC 依赖引入整个模块构建。
    if (TARGET go2cpp_hash)
        target_link_libraries(go2cpp_bento PUBLIC go2cpp_hash)
    else()
        target_sources(go2cpp_bento PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/crc32.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/hash.cpp)
    endif()
    set_target_properties(go2cpp_bento PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento ALIAS go2cpp_bento)

    # MOD-26/28：通用命令协议 + 文件传输（JinShan 场景缺口）。
    # MOD-31：通用备份模块（store-only zip + 打包 + 上传）。
    # 依赖 go2cpp_os（os::proc 进程执行）与 go2cpp_hash（CRC-32/SHA-256），
    # 因此只在完整模块构建里创建；standalone 构建跳过并给出提示，不悄悄降级 API。
    if (TARGET go2cpp_os AND TARGET go2cpp_hash)
        add_library(go2cpp_bento_app STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/command.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/file_transfer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/zip.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/backup.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/fetch.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/file_browser.cpp)

        target_include_directories(go2cpp_bento_app
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_compile_features(go2cpp_bento_app PUBLIC cxx_std_17)
        target_compile_options(go2cpp_bento_app PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        target_link_libraries(go2cpp_bento_app
            PUBLIC go2cpp_bento go2cpp_os go2cpp_hash Threads::Threads)
        set_target_properties(go2cpp_bento_app PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_library(go2cpp::bento_app ALIAS go2cpp_bento_app)
    else()
        message(STATUS "Bento command/file transfer: OFF (go2cpp_os/go2cpp_hash not available)")
    endif()

    # 通用传输抽象（JinShan MOD-07/08/09）：ITransport + TCP/内存实现。
    # 独立目标；帧编解码由 go2cpp_bento 的 MOD-10 frame.cpp 提供。
    add_library(go2cpp_bento_transport STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/tcp_transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/memory_transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/udp_transport.cpp
        # MOD-15：通用字节中继（有界缓冲/慢客户端/半关闭/超时）。
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/relay.cpp)

    target_include_directories(go2cpp_bento_transport
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento_transport PUBLIC cxx_std_17)
    target_compile_options(go2cpp_bento_transport PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento_transport PUBLIC Threads::Threads)
    set_target_properties(go2cpp_bento_transport PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento_transport ALIAS go2cpp_bento_transport)

    # MOD-37：Linux 运行环境能力探测（/proc、权限、epoll、cgroup、网络接口、
    # 文件系统与 rlimit）。只依赖标准库与 POSIX，独立成目标，便于宿主在
    # 启动阶段先探测再决定降级策略。
    add_library(go2cpp_bento_capability STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/capability.cpp)

    target_include_directories(go2cpp_bento_capability
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento_capability PUBLIC cxx_std_17)
    target_compile_options(go2cpp_bento_capability PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
    set_target_properties(go2cpp_bento_capability PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento_capability ALIAS go2cpp_bento_capability)

    # MOD-39：通用会话编排（endpoint → transport → pairing → channel →
    # session_control → heartbeat → graceful close）。复用 net 的配对/帧/
    # 心跳/控制权原语，不复制协议实现。
    add_library(go2cpp_bento_session STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/app/session.cpp)

    target_include_directories(go2cpp_bento_session
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento_session PUBLIC cxx_std_17)
    target_compile_options(go2cpp_bento_session PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento_session
        PUBLIC go2cpp_bento go2cpp_bento_transport Threads::Threads)
    set_target_properties(go2cpp_bento_session PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento_session ALIAS go2cpp_bento_session)

    # 错误体系 + 反射（bento::error / bento::reflect）：只依赖标准库，
    # 与传输层解耦；后续 net 模块可以按需链接本目标复用统一错误/绑定实现。
    add_library(go2cpp_bento_error STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/error/error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/error/registry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/error/mapping.cpp)

    target_include_directories(go2cpp_bento_error
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento_error PUBLIC cxx_std_17)
    target_compile_options(go2cpp_bento_error PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento_error PUBLIC Threads::Threads)
    set_target_properties(go2cpp_bento_error PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento_error ALIAS go2cpp_bento_error)

    # MOD-05：通用密钥管理（多来源加载/脱敏/常量时间比较/轮换）。只依赖
    # error，独立成目标，不给 go2cpp_bento 增加新的链接依赖。
    add_library(go2cpp_bento_keystore STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/keystore.cpp)

    target_include_directories(go2cpp_bento_keystore
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento_keystore PUBLIC cxx_std_17)
    target_compile_options(go2cpp_bento_keystore PRIVATE
        -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
    target_link_libraries(go2cpp_bento_keystore
        PUBLIC go2cpp_bento_error Threads::Threads)
    set_target_properties(go2cpp_bento_keystore PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento_keystore ALIAS go2cpp_bento_keystore)

    # HTTP/SSE 服务端骨架：复用 base 的 http1 解析器、SSE 流写入器与
    # go2cpp_network 的 Socket。独立目标，不给 go2cpp_bento 增加网络依赖；
    # standalone 构建（没有 go2cpp_network）时跳过，API 不悄悄降级。
    if (TARGET go2cpp_network)
        add_library(go2cpp_bento_http STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/http_server.cpp)

        target_include_directories(go2cpp_bento_http
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_compile_features(go2cpp_bento_http PUBLIC cxx_std_17)
        # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
        target_compile_options(go2cpp_bento_http PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        target_link_libraries(go2cpp_bento_http
            PUBLIC go2cpp_bento go2cpp_network Threads::Threads)
        set_target_properties(go2cpp_bento_http PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_library(go2cpp::bento_http ALIAS go2cpp_bento_http)
    else()
        message(STATUS "Bento HTTP/SSE server: OFF (go2cpp_network not available)")
    endif()

    option(GO2CPP_BENTO_TESTS "Build Bento transport tests" ON)
    if (GO2CPP_BENTO_TESTS)
        enable_testing()

        # 错误/反射：纯本地测试，不依赖 go2cpp_bento（避免把传输层拖进构建）。
        add_executable(bento_test_error
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_error.cpp)
        add_executable(bento_test_reflect
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_reflect.cpp)
        foreach(bento_error_test bento_test_error bento_test_reflect)
            target_include_directories(${bento_error_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_link_libraries(${bento_error_test} PRIVATE go2cpp_bento_error)
            target_compile_options(${bento_error_test} PRIVATE
                -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        endforeach()
        add_test(NAME bento_error COMMAND bento_test_error)
        add_test(NAME bento_reflect COMMAND bento_test_reflect)
        set_tests_properties(bento_error bento_reflect PROPERTIES TIMEOUT 120)

        # MOD-05：密钥加载/脱敏/轮换 + PeerHub/Link PSK 集成（UDP 回环）。
        add_executable(bento_test_keystore
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_keystore.cpp)
        target_include_directories(bento_test_keystore PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
        target_link_libraries(bento_test_keystore PRIVATE
            go2cpp_bento_keystore go2cpp_bento)
        target_compile_options(bento_test_keystore PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        add_test(NAME bento_keystore COMMAND bento_test_keystore)
        set_tests_properties(bento_keystore PROPERTIES TIMEOUT 180)

        # HTTP/SSE 服务端骨架：回环端口 + 真实 socket 客户端，覆盖 GET/POST/
        # SSE 推流/404/优雅关闭/并发/异常隔离；无外部网络依赖。
        if (TARGET go2cpp_bento_http)
            add_executable(bento_test_http_server
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_http_server.cpp)
            target_include_directories(bento_test_http_server PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_link_libraries(bento_test_http_server PRIVATE go2cpp_bento_http)
            target_compile_options(bento_test_http_server PRIVATE
                -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
            add_test(NAME bento_http_server COMMAND bento_test_http_server)
            set_tests_properties(bento_http_server PROPERTIES TIMEOUT 120)
        endif()

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
        add_executable(bento_test_link_server
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_link_server.cpp)
        add_executable(bento_test_frame
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_frame.cpp)
        add_executable(bento_test_heartbeat
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_heartbeat.cpp)
        add_executable(bento_test_backoff
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_backoff.cpp)
        add_executable(bento_test_pairing
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_pairing.cpp)
        add_executable(bento_test_session_table
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_session_table.cpp)
        add_executable(bento_netem_probe
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/netem_probe.cpp)

        # MOD-37/39：环境能力探测与会话编排。两者都不需要外部端口：
        # 会话测试用 MemoryTransport 注入口，能力测试只读本机环境。
        add_executable(bento_test_capability
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_capability.cpp)
        add_executable(bento_test_session
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_session.cpp)
        foreach(bento_app_module_test bento_test_capability bento_test_session)
            target_include_directories(${bento_app_module_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_compile_options(${bento_app_module_test} PRIVATE
                -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        endforeach()
        target_link_libraries(bento_test_capability PRIVATE go2cpp_bento_capability)
        target_link_libraries(bento_test_session PRIVATE go2cpp_bento_session)

        # 传输抽象测试：内存/TCP 回环、半关闭、超时、帧粘包/半包/错误。
        add_executable(bento_test_transport
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_transport.cpp)
        target_include_directories(bento_test_transport PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
        target_link_libraries(bento_test_transport PRIVATE go2cpp_bento_transport go2cpp_bento)
        target_compile_options(bento_test_transport PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})

        # MOD-15：通用字节中继。真实 TCP 回环 + 内存编排传输，覆盖背压/
        # 慢客户端/半关闭/超时；不依赖外部端口。
        add_executable(bento_test_relay
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_relay.cpp)
        target_include_directories(bento_test_relay PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
        target_link_libraries(bento_test_relay PRIVATE go2cpp_bento_transport)
        target_compile_options(bento_test_relay PRIVATE
            -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})

        # UDP 数据报与 X25519 独立测试：数据报边界/截断/超时 + RFC 7748 向量。
        add_executable(bento_test_udp_transport
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_udp_transport.cpp)
        add_executable(bento_test_x25519
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_x25519.cpp)
        add_executable(bento_test_x25519_handshake
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_x25519_handshake.cpp)
        foreach(bento_primitive_test bento_test_udp_transport bento_test_x25519
                bento_test_x25519_handshake)
            target_include_directories(${bento_primitive_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_compile_options(${bento_primitive_test} PRIVATE
                -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
        endforeach()
        target_link_libraries(bento_test_udp_transport PRIVATE go2cpp_bento_transport)
        target_link_libraries(bento_test_x25519 PRIVATE go2cpp_bento)
        target_link_libraries(bento_test_x25519_handshake PRIVATE go2cpp_bento)

        # MOD-26/28：命令协议与文件传输（依赖完整模块构建里的 app 目标）。
        # MOD-31：zip/backup/upload 测试。
        if (TARGET go2cpp_bento_app)
            add_executable(bento_test_command
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_command.cpp)
            add_executable(bento_test_file_transfer
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_file_transfer.cpp)
            add_executable(bento_test_zip
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_zip.cpp)
            add_executable(bento_test_backup
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_backup.cpp)
            add_executable(bento_test_upload
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_upload.cpp)
            add_executable(bento_test_fetch
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_fetch.cpp)
            add_executable(bento_test_file_browser
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_file_browser.cpp)
            foreach(bento_app_test bento_test_command bento_test_file_transfer
                    bento_test_zip bento_test_backup bento_test_upload
                    bento_test_fetch bento_test_file_browser)
                target_include_directories(${bento_app_test} PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
                target_link_libraries(${bento_app_test} PRIVATE go2cpp_bento_app)
                target_compile_options(${bento_app_test} PRIVATE
                    -Wall -Wextra -Wpedantic -Werror ${BENTO_OPT_FLAGS})
            endforeach()
            add_test(NAME bento_command COMMAND bento_test_command)
            add_test(NAME bento_file_transfer COMMAND bento_test_file_transfer)
            add_test(NAME bento_zip COMMAND bento_test_zip)
            add_test(NAME bento_backup COMMAND bento_test_backup)
            add_test(NAME bento_upload COMMAND bento_test_upload)
            add_test(NAME bento_fetch COMMAND bento_test_fetch)
            add_test(NAME bento_file_browser COMMAND bento_test_file_browser)
            # 环境不满足时测试进程以 77 退出 -> CTest 记为 Skipped。
            set_tests_properties(bento_command bento_file_transfer bento_zip
                bento_backup bento_upload bento_fetch bento_file_browser
                PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
        else()
            message(STATUS "Bento command/file transfer tests: OFF")
        endif()

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
                bento_test_link_server
                bento_test_frame
                bento_test_heartbeat
                bento_test_backoff
                bento_test_pairing
                bento_test_session_table
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
        add_test(NAME bento_link_server COMMAND bento_test_link_server)
        # MOD-10/11/12：帧编解码、心跳、退避的纯本地测试（无网络依赖）。
        add_test(NAME bento_frame COMMAND bento_test_frame)
        add_test(NAME bento_heartbeat COMMAND bento_test_heartbeat)
        add_test(NAME bento_backoff COMMAND bento_test_backoff)
        add_test(NAME bento_pairing COMMAND bento_test_pairing)
        add_test(NAME bento_session_table COMMAND bento_test_session_table)
        add_test(NAME bento_transport COMMAND bento_test_transport)
        add_test(NAME bento_relay COMMAND bento_test_relay)
        add_test(NAME bento_capability COMMAND bento_test_capability)
        add_test(NAME bento_session COMMAND bento_test_session)
        set_tests_properties(bento_capability bento_session
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        add_test(NAME bento_udp_transport COMMAND bento_test_udp_transport)
        add_test(NAME bento_x25519 COMMAND bento_test_x25519)
        add_test(NAME bento_x25519_handshake COMMAND bento_test_x25519_handshake)
        # 用户态弱网代理：无 CAP_NET_ADMIN 时替代 tc netem 的可重复版本。
        # 20% 双向丢包 + 200ms 单向延迟，可靠流要求 100% 按序。
        # 60s 收尾窗口：20% 丢包下 PTO 最坏会退避到 2s，40s 偶发尾部未收完。
        # 不传 --port：探针默认 bind 0，由内核分配随机端口，避免并发
        # agent/测试抢同一固定端口（同一机器可同时跑多个实例）。
        add_test(NAME bento_weaknet_proxy
            COMMAND bento_netem_probe --messages 200 --payload 256
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
            bento_link_server
            bento_frame bento_heartbeat bento_backoff
            bento_pairing bento_session_table
            PROPERTIES TIMEOUT 90)
        set_tests_properties(bento_transport bento_relay PROPERTIES TIMEOUT 120)
        set_tests_properties(bento_udp_transport bento_x25519 bento_x25519_handshake
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        set_tests_properties(bento_relay PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(bento_weaknet_proxy PROPERTIES TIMEOUT 150)
        set_tests_properties(bento_netem PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()

    message(STATUS "Bento transport: ON (tests=${GO2CPP_BENTO_TESTS})")
else()
    message(STATUS "Bento transport: OFF (set GO2CPP_WITH_BENTO=ON to enable)")
endif()
