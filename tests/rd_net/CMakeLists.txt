# rd_net 验收测试（REQ-006 socket / REQ-007 event_loop / REQ-008 protocol /
# REQ-009 reliability）。Windows（REQ-010）与 POSIX 共用同一批源文件：
# 平台差异在 test_socket.cpp / test_event_loop.cpp 的 `_WIN32` 分支内，
# 测试断言两平台完全一致。
#
# 用法（在根工程里）：
#   add_subdirectory(tests/rd_net)
#   cmake --build build --target rd_net_test_socket
# 前提：根工程已定义 go2cpp_network / go2cpp_io / go2cpp_context / go2cpp_error
# 四个库目标（与 build_and_run_*.sh 里的 -lgo2cpp_* 对应）。

set(RD_NET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(RD_NET_INCLUDE "${RD_NET_ROOT}/include")
set(RD_NET_SOCKET_SRC "${RD_NET_ROOT}/src/rd_net/socket.cpp")
set(RD_NET_LOOP_SRC "${RD_NET_ROOT}/src/rd_net/event_loop.cpp")
set(RD_NET_PROTOCOL_SRC "${RD_NET_ROOT}/src/rd_net/protocol.cpp")
set(RD_NET_LIBS go2cpp_network go2cpp_io go2cpp_context go2cpp_error)

function(rd_net_configure target)
    target_include_directories(${target} PRIVATE "${RD_NET_INCLUDE}")
    target_link_libraries(${target} PRIVATE ${RD_NET_LIBS})
    if(WIN32)
        # REQ-010：Winsock + psapi（GetProcessMemoryInfo 用于 RSS 断言）。
        target_link_libraries(${target} PRIVATE ws2_32 psapi)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX)
        endif()
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${target} PRIVATE Threads::Threads)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_executable(rd_net_test_socket test_socket.cpp "${RD_NET_SOCKET_SRC}")
rd_net_configure(rd_net_test_socket)
add_test(NAME rd_net_socket COMMAND rd_net_test_socket)

add_executable(rd_net_test_event_loop test_event_loop.cpp "${RD_NET_SOCKET_SRC}"
                                                        "${RD_NET_LOOP_SRC}")
rd_net_configure(rd_net_test_event_loop)
add_test(NAME rd_net_event_loop COMMAND rd_net_test_event_loop)

add_executable(rd_net_test_protocol test_protocol.cpp "${RD_NET_SOCKET_SRC}"
                                                      "${RD_NET_LOOP_SRC}"
                                                      "${RD_NET_PROTOCOL_SRC}")
rd_net_configure(rd_net_test_protocol)
add_test(NAME rd_net_protocol COMMAND rd_net_test_protocol)

# reliability 是纯头文件测试，不需要链接任何库（对应 build_and_run_reliability.sh）。
add_executable(rd_net_test_reliability test_reliability.cpp)
target_include_directories(rd_net_test_reliability PRIVATE "${RD_NET_INCLUDE}")
if(MSVC)
    target_compile_options(rd_net_test_reliability PRIVATE /W4 /WX)
else()
    target_compile_options(rd_net_test_reliability PRIVATE -Wall -Wextra -Wpedantic
                                                          -Werror)
endif()
add_test(NAME rd_net_reliability COMMAND rd_net_test_reliability)
