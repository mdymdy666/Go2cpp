# 应用示例（example/app）：web_server / chat_server 各有 simple 与 full 两版。
#
# 这些示例是「可运行的服务」而不是自检程序：它们监听端口、等待信号，
# 因此不注册 ctest（否则 ctest 会挂住）。运行方式见各自的 README。
#
# 依赖说明：
#   go2cpp_runtime     —— 基础运行时（Scheduler/Channel/network/http）
#   go2cpp_bento_http  —— bento::HttpServer（HTTP/SSE 服务端骨架）
#   go2cpp_bento       —— bento::LinkServer / PeerHub（UDP 多 peer 链路）
# 后两个目标只在 -DGO2CPP_WITH_BENTO=ON 且依赖模块齐全时存在；缺失时
# 明确打印跳过原因，不静默把服务示例编译成空壳。

set(GO2CPP_APP_WARNINGS -Wall -Wextra -Wpedantic)

function(go2cpp_app_target target)
    target_link_libraries(${target} PRIVATE ${ARGN})
    target_compile_options(${target} PRIVATE ${GO2CPP_APP_WARNINGS})
endfunction()

if (TARGET go2cpp_bento_http)
    # 简单版：单文件，适合第一次看「HttpServer + Scheduler + Channel」。
    add_executable(go2cpp_web_server_simple
        web_server_simple/main.cpp)
    go2cpp_app_target(go2cpp_web_server_simple go2cpp_runtime go2cpp_bento_http)

    # 全量版：按职责拆模块（配置/日志/路由/中间件/静态文件/API/SSE/组装）。
    add_executable(go2cpp_web_server_full
        web_server_full/main.cpp
        web_server_full/config.cpp
        web_server_full/logger.cpp
        web_server_full/router.cpp
        web_server_full/middleware.cpp
        web_server_full/static_files.cpp
        web_server_full/api.cpp
        web_server_full/sse_hub.cpp
        web_server_full/app_server.cpp)
    target_include_directories(go2cpp_web_server_full PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/web_server_full)
    go2cpp_app_target(go2cpp_web_server_full go2cpp_runtime go2cpp_bento_http)
else()
    message(STATUS "example/app: web_server 示例跳过（go2cpp_bento_http 未构建）")
endif()

if (TARGET go2cpp_bento)
    # 简单版：单文件，同时带 --server / --client 两种角色，便于本机互测。
    add_executable(go2cpp_chat_server_simple
        chat_server_simple/main.cpp)
    go2cpp_app_target(go2cpp_chat_server_simple go2cpp_runtime go2cpp_bento)

    # 全量版：协议/会话/房间/持久化/配置/日志/服务端/客户端分模块。
    add_executable(go2cpp_chat_server_full
        chat_server_full/main.cpp
        chat_server_full/config.cpp
        chat_server_full/logger.cpp
        chat_server_full/protocol.cpp
        chat_server_full/session.cpp
        chat_server_full/store.cpp
        chat_server_full/app_server.cpp
        chat_server_full/client.cpp)
    target_include_directories(go2cpp_chat_server_full PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/chat_server_full)
    go2cpp_app_target(go2cpp_chat_server_full go2cpp_runtime go2cpp_bento)
else()
    message(STATUS "example/app: chat_server 示例跳过（go2cpp_bento 未构建）")
endif()
