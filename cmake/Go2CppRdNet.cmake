# ---------------------------------------------------------------------------
# rd_net：JinShan REQ-006/007/008/009 的跨平台网络基础层（rd::net）
#
# 定位：UDP/TCP 四态 Socket、单线程事件循环、协议注入、可靠骨架。
# 平台：
#   - Linux/POSIX：socket.cpp 复用 go2cpp::net::Socket（依赖 go2cpp_network），
#     事件循环用 poll(2) + eventfd(2)；
#   - Windows：socket.cpp / event_loop.cpp 的 `#if defined(_WIN32)` 分支用
#     Winsock（WSAEventSelect + WaitForMultipleObjectsEx），只依赖 ws2_32，
#     不依赖 go2cpp_network（后者是 POSIX 实现）。
#
# 测试不在本文件注册：tests/rd_net/*.cpp 依赖 dirent/poll/fork，是 POSIX
# 专用用例，Windows 上不可编译；Windows 侧的验证入口是 CI 的
# `cmake --build --target go2cpp_rd_net`（见 .github/workflows/ci.yml）。
# ---------------------------------------------------------------------------

option(GO2CPP_WITH_RD_NET
    "Build go2cpp_rd_net (JinShan rd::net sockets + event loop)" ON)

if (NOT GO2CPP_WITH_RD_NET)
    message(STATUS "rd_net: OFF (GO2CPP_WITH_RD_NET=OFF)")
    return()
endif()

include(GNUInstallDirs)

set(GO2CPP_RD_NET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GO2CPP_RD_NET_SOURCES
    ${GO2CPP_RD_NET_ROOT}/src/rd_net/socket.cpp
    ${GO2CPP_RD_NET_ROOT}/src/rd_net/event_loop.cpp
    ${GO2CPP_RD_NET_ROOT}/src/rd_net/protocol.cpp
)

add_library(go2cpp_rd_net STATIC ${GO2CPP_RD_NET_SOURCES})
add_library(go2cpp::rd_net ALIAS go2cpp_rd_net)

target_include_directories(go2cpp_rd_net
    PUBLIC
        $<BUILD_INTERFACE:${GO2CPP_RD_NET_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(go2cpp_rd_net PUBLIC cxx_std_17)
set_target_properties(go2cpp_rd_net PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    # /W4 + /WX：Windows 分支是新增代码，先把警告挡在 CI，避免「先绿后烂」。
    # /permissive- 关闭 MSVC 的宽松模式；/Zc:__cplusplus 让 __cplusplus 正确。
    target_compile_options(go2cpp_rd_net PRIVATE
        /W4 /WX /permissive- /Zc:__cplusplus)
    # _WIN32_WINNT=0x0601（Windows 7）：inet_pton / getnameinfo 需要 >= 0x0600；
    # WIN32_LEAN_AND_MEAN 去掉 windows.h 里的旧 Winsock1/MFC 依赖；
    # NOMINMAX 阻止 <windows.h> 的 min/max 宏破坏 <algorithm>。
    target_compile_definitions(go2cpp_rd_net PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(go2cpp_rd_net PUBLIC ws2_32)
else()
    target_compile_options(go2cpp_rd_net PRIVATE -Wall -Wextra -Wpedantic)
    if (TARGET go2cpp_network)
        target_link_libraries(go2cpp_rd_net PUBLIC go2cpp_network)
    else()
        message(WARNING
            "rd_net: go2cpp_network 目标不存在，POSIX 分支链接会失败")
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(go2cpp_rd_net PUBLIC Threads::Threads)
endif()
