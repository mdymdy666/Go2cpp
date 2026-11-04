# ---------------------------------------------------------------------------
# 公网穿透模块（include/go2cpp/tunnel/，Go2Cpp 的正式能力）
#
# 定位：一条命令把本地端口发布成公网 HTTP URL，不依赖 Tailscale / cloudflared
# 等外部程序。服务端 `tunnel-server` 跑在公网 VPS，客户端 `tunnel-client` 跑在
# NAT 后的本地机器；两端都是本仓库构建的原生可执行文件。
#
# 复用面（不重复造轮子）：
#   - go2cpp_frame       → 12 字节帧头 + 半包/粘包重组的 FrameCodec
#   - go2cpp_relay_pipe  → 每条流的双向中继 + 有界队列背压 + 写 deadline
#   - go2cpp_error       → 与项目其它模块一致的错误值
#
# 默认 ON；仅 POSIX（服务端/客户端直接用 poll(2)），Windows 上自动跳过。
#   -DGO2CPP_WITH_TUNNEL=OFF 关闭。
#
# 安装策略：与 cmake/Go2CppFrame.cmake 一致——本文件在 Go2CppInstall.cmake 之后
# include（因为要依赖 frame 目标），因此**不加入** GO2CPP_MODULE_TARGETS /
# go2cpp_runtime 聚合目标（否则会破坏 install(EXPORT) 的闭包）。库与两个可执行
# 由本文件自行 install；头文件随 `install(DIRECTORY include/)` 一并安装。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_TUNNEL "Build the public tunnel module (tunnel-server/client)" ON)

if (NOT GO2CPP_WITH_TUNNEL)
    message(STATUS "tunnel: OFF (GO2CPP_WITH_TUNNEL=OFF)")
    return()
endif()

if (WIN32)
    message(STATUS "tunnel: skipped on Windows (POSIX sockets only)")
    return()
endif()

if (NOT TARGET go2cpp_frame OR NOT TARGET go2cpp_relay_pipe)
    message(FATAL_ERROR
        "GO2CPP_WITH_TUNNEL 需要 go2cpp_frame 与 go2cpp_relay_pipe 目标；"
        "请确认 GO2CPP_WITH_FRAME / GO2CPP_WITH_RELAY_PIPE 未被关闭。")
endif()

# 复用（不重写）：Bento 的 PSK/AEAD（crypto.cpp + x25519.cpp）、Bento 的
# 指数退避（backoff.cpp）、metrics 的进程级注册表（metrics.cpp）。这三个模块
# 都各自归属更大的库（go2cpp_bento 默认 OFF、go2cpp_metrics 反向依赖
# go2cpp_network），而 tunnel 是**独立守护进程**，因此把这几份零依赖实现
# 直接编入本目标；tunnel 的可执行不链接 go2cpp_bento / go2cpp_network。
add_library(go2cpp_tunnel STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/net.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/mux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/x25519.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/backoff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/metrics.cpp)
add_library(go2cpp::tunnel ALIAS go2cpp_tunnel)

target_include_directories(go2cpp_tunnel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(go2cpp_tunnel PUBLIC cxx_std_17)
target_compile_options(go2cpp_tunnel PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(go2cpp_tunnel
    PUBLIC go2cpp_frame go2cpp_relay_pipe go2cpp_hash go2cpp_error go2cpp_log
           Threads::Threads)

set_target_properties(go2cpp_tunnel PROPERTIES
    EXPORT_NAME tunnel
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(go2cpp_tunnel PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# 两个命令行入口：输出名与文档/脚本里的一条命令一致（tunnel-server / -client）。
add_executable(tunnel_server ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/server_main.cpp)
set_target_properties(tunnel_server PROPERTIES OUTPUT_NAME tunnel-server)
target_link_libraries(tunnel_server PRIVATE go2cpp_tunnel)
target_compile_options(tunnel_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(tunnel_client ${CMAKE_CURRENT_SOURCE_DIR}/src/tunnel/client_main.cpp)
set_target_properties(tunnel_client PROPERTIES OUTPUT_NAME tunnel-client)
target_link_libraries(tunnel_client PRIVATE go2cpp_tunnel)
target_compile_options(tunnel_client PRIVATE -Wall -Wextra -Wpedantic)

# 自行安装（不进导出集，见文件头说明）。
if (NOT WIN32)
    install(TARGETS go2cpp_tunnel tunnel_server tunnel_client
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (GO2CPP_BUILD_TESTS)
    add_executable(go2cpp_tunnel_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel/test_tunnel.cpp)
    target_link_libraries(go2cpp_tunnel_test PRIVATE go2cpp_tunnel)
    target_include_directories(go2cpp_tunnel_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel)
    target_compile_options(go2cpp_tunnel_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME tunnel COMMAND go2cpp_tunnel_test)
    set_tests_properties(tunnel PROPERTIES LABELS tunnel TIMEOUT 900)
    message(STATUS "tunnel: ON (go2cpp_tunnel, tunnel-server/client, test=tunnel)")
endif()
