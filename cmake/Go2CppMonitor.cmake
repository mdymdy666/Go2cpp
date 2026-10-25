# go2cpp::os::monitor —— 主机监控模块（/proc 采集器 + eBPF 网络统计 + HTTP 推送）。
#
# 单独成一个库而不是塞进 go2cpp_os：监控依赖网络（HTTP 推送）与 libbpf，
# 而 go2cpp_os 的进程/IPC 用例不需要这些依赖，分开后不装 libbpf 的机器仍可
# 构建 go2cpp_os。
#
# eBPF 的可用性在 configure 阶段探测：
#   - clang 必须支持 -target bpf（用 --print-targets 判断，失败则关闭）；
#   - libbpf / libelf / libz 必须可链接；
# 任一缺失时仍编译用户态代码，只是 NetEbpfMonitor 永远返回 kUnsupported，
# 不影响四个 /proc 采集器。

option(GO2CPP_BUILD_OS_MONITOR_TESTS "Build go2cpp::os::monitor tests" ON)
option(GO2CPP_ENABLE_EBPF "Compile and embed the eBPF programs" ON)

set(GO2CPP_MONITOR_EBPF_AVAILABLE OFF)
if (GO2CPP_ENABLE_EBPF)
    find_program(GO2CPP_CLANG_EXECUTABLE NAMES clang)
    find_library(GO2CPP_LIBBPF_LIBRARY NAMES bpf)
    find_library(GO2CPP_LIBELF_LIBRARY NAMES elf)
    find_library(GO2CPP_LIBZ_LIBRARY NAMES z)
    if (GO2CPP_CLANG_EXECUTABLE AND GO2CPP_LIBBPF_LIBRARY AND
        GO2CPP_LIBELF_LIBRARY AND GO2CPP_LIBZ_LIBRARY AND
        CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        execute_process(
            COMMAND ${GO2CPP_CLANG_EXECUTABLE} -print-targets
            OUTPUT_VARIABLE GO2CPP_CLANG_TARGETS
            ERROR_QUIET)
        if (GO2CPP_CLANG_TARGETS MATCHES "bpf")
            set(GO2CPP_MONITOR_EBPF_AVAILABLE ON)
        endif()
    endif()
endif()

message(STATUS "go2cpp::os::monitor eBPF support: ${GO2CPP_MONITOR_EBPF_AVAILABLE}")

add_library(go2cpp_os_monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/monitor.cpp
    # 内部接口与可替换实现：SnapshotSink / SnapshotSource / SnapshotStore。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/interfaces.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/cpu_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/mem_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/disk_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/net_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/net_ebpf_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/aggregator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/pusher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/manager.cpp
    # 内核 tracepoint 监控：syscall / sched / futex + 共享的 eBPF 加载工具。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/kernel_ebpf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/syscall_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/sched_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/futex_monitor.cpp
    # TCP 重传/RTT（tracepoint）与文件 IO 延迟（kprobe）。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/tcp_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/monitor/fileio_monitor.cpp)

target_include_directories(go2cpp_os_monitor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        # nlohmann/json.hpp 位于 go2cpp/third_party，monitor.hpp 经
        # go2cpp/tools/json.hpp 间接包含它。
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
target_compile_features(go2cpp_os_monitor PUBLIC cxx_std_17)
target_compile_options(go2cpp_os_monitor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(go2cpp_os_monitor
    PUBLIC go2cpp_network go2cpp_scheduler go2cpp_channel
    PRIVATE Threads::Threads)
set_target_properties(go2cpp_os_monitor PROPERTIES
    EXPORT_NAME os_monitor
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)
add_library(go2cpp::os_monitor ALIAS go2cpp_os_monitor)

# 纳入统一的安装/导出集合：Go2CppInstall.cmake 只导出 GO2CPP_MODULE_TARGETS
# 里的目标，漏掉这一行会让 install(EXPORT ...) 因「依赖目标不在导出集」失败。
list(APPEND GO2CPP_MODULE_TARGETS go2cpp_os_monitor)

if (GO2CPP_MONITOR_EBPF_AVAILABLE)
    # 多架构头（asm/types.h 等）在 Debian/Ubuntu 上不在 /usr/include 根下。
    if (CMAKE_LIBRARY_ARCHITECTURE)
        set(GO2CPP_BPF_ARCH_INCLUDE /usr/include/${CMAKE_LIBRARY_ARCHITECTURE})
    else()
        set(GO2CPP_BPF_ARCH_INCLUDE /usr/include)
    endif()

    # 编译一个 .bpf.c，并把 ELF 以 <symbol>_elf 符号嵌入 go2cpp_os_monitor。
    # 每个监控器一个独立 ELF：某个 tracepoint 在内核缺失时只影响对应监控器，
    # 不会因为一个程序加载失败拖垮另外两个。
    function(go2cpp_embed_bpf name source symbol)
        set(object ${CMAKE_CURRENT_BINARY_DIR}/${name}.bpf.o)
        set(embedded ${CMAKE_CURRENT_BINARY_DIR}/${name}_elf.cpp)
        add_custom_command(
            OUTPUT ${object}
            COMMAND ${GO2CPP_CLANG_EXECUTABLE}
                    -O2 -g -target bpf -D__TARGET_ARCH_x86
                    -I${GO2CPP_BPF_ARCH_INCLUDE}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/bpf
                    -Wall -Werror
                    -c ${source} -o ${object}
            DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/bpf/monitor_hist.h
            COMMENT "Compiling eBPF program ${name}"
            VERBATIM)
        add_custom_command(
            OUTPUT ${embedded}
            COMMAND ${CMAKE_COMMAND}
                    -DBPF_OBJ=${object}
                    -DBPF_OUT=${embedded}
                    -DBPF_SYMBOL=${symbol}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bpf.cmake
            DEPENDS ${object} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bpf.cmake
            COMMENT "Embedding eBPF ELF into ${name}_elf.cpp"
            VERBATIM)
        target_sources(go2cpp_os_monitor PRIVATE ${embedded})
    endfunction()

    go2cpp_embed_bpf(net_stats
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/net_stats.bpf.c
                     go2cpp_net_stats)
    go2cpp_embed_bpf(syscall_latency
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/syscall_latency.bpf.c
                     go2cpp_syscall_latency)
    go2cpp_embed_bpf(sched_latency
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/sched_latency.bpf.c
                     go2cpp_sched_latency)
    go2cpp_embed_bpf(futex_latency
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/futex_latency.bpf.c
                     go2cpp_futex_latency)
    go2cpp_embed_bpf(tcp_monitor
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/tcp_monitor.bpf.c
                     go2cpp_tcp_monitor)
    go2cpp_embed_bpf(fileio_monitor
                     ${CMAKE_CURRENT_SOURCE_DIR}/bpf/fileio_monitor.bpf.c
                     go2cpp_fileio_monitor)

    target_compile_definitions(go2cpp_os_monitor PRIVATE GO2CPP_HAVE_EBPF=1)
    target_link_libraries(go2cpp_os_monitor
        PRIVATE ${GO2CPP_LIBBPF_LIBRARY} ${GO2CPP_LIBELF_LIBRARY}
                ${GO2CPP_LIBZ_LIBRARY})
endif()

# 接入总运行时入口，使 `go2cpp::runtime` 的使用者自动获得监控模块。
if (TARGET go2cpp_runtime)
    target_link_libraries(go2cpp_runtime INTERFACE go2cpp_os_monitor)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if (GO2CPP_BUILD_OS_MONITOR_TESTS)
    enable_testing()
    set(GO2CPP_OS_MONITOR_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/os/monitor)

    function(go2cpp_os_monitor_test name source timeout_seconds)
        add_executable(${name} ${GO2CPP_OS_MONITOR_TEST_DIR}/${source})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
            ${GO2CPP_OS_MONITOR_TEST_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(${name} PRIVATE go2cpp_os_monitor go2cpp_network)
        # network 的 socket.cpp 引用 hook::mark_system_nonblocking；共享库
        # 构建下测试可执行文件显式链接 hook，避免未定义符号（与
        # go2cpp_runtime 的 INTERFACE 链接保持一致的可见性）。
        if (TARGET go2cpp_hook)
            target_link_libraries(${name} PRIVATE go2cpp_hook)
            # network 的未定义符号由 hook 提供；--as-needed（默认）会因
            # 测试自身不引用 hook 符号而丢弃 DSO，显式关闭。
            target_link_options(${name} PRIVATE "LINKER:--no-as-needed")
        endif()
        target_compile_options(${name} PRIVATE -O2 -Wall -Wextra -Wpedantic)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT ${timeout_seconds})
    endfunction()

    go2cpp_os_monitor_test(go2cpp_os_monitor_collectors
                           test_monitor_collectors.cpp 120)
    # 接口化：MockSink 下的 Aggregator、注入 SnapshotStore 的 Manager、
    # 文件/HTTP sink 与 Pusher-as-Sink 多态。
    go2cpp_os_monitor_test(go2cpp_os_monitor_interfaces
                           test_monitor_interfaces.cpp 120)
    go2cpp_os_monitor_test(go2cpp_os_monitor_ebpf test_monitor_ebpf.cpp 120)
    go2cpp_os_monitor_test(go2cpp_os_monitor_e2e test_monitor_e2e.cpp 180)
    go2cpp_os_monitor_test(go2cpp_os_monitor_stress
                           test_monitor_stress.cpp 180)
    # 三个内核 tracepoint 监控器（syscall/sched/futex）的降级与真实数据用例。
    go2cpp_os_monitor_test(go2cpp_os_kernel_monitor test_kernel_monitor.cpp 240)
    # TCP 重传/RTT 与文件 IO 延迟的降级契约与真实数据用例。
    go2cpp_os_monitor_test(go2cpp_os_tcp_io_monitor test_tcp_io_monitor.cpp 240)

    # 非 root 降级契约（root 下返回 77 → ctest SKIP）：9 类指标里的 eBPF 网络
    # 与五个内核 tracepoint 监控器必须 available=false + reason 指向 root，
    # 同时四个 /proc 采集器不受影响。
    go2cpp_os_monitor_test(go2cpp_os_monitor_ebpf_nonroot
                           test_monitor_ebpf_nonroot.cpp 120)
    set_tests_properties(go2cpp_os_monitor_ebpf_nonroot PROPERTIES
        SKIP_RETURN_CODE 77)

    # 采集频率开销：1/5/20Hz 的单次成本换算占比 + 真实定时器跑 3 秒的
    # 进程 CPU 占比（目标 <5%）。
    go2cpp_os_monitor_test(go2cpp_os_monitor_overhead
                           test_monitor_overhead.cpp 180)

    # root 专用：通过 sudo 重跑同一可执行文件的 --require-ebpf 模式。
    # 无法提权时返回 77，ctest 记为 SKIP 而不是失败。
    add_test(NAME go2cpp_os_monitor_ebpf_root
             COMMAND bash -c
                 "echo root | sudo -S $<TARGET_FILE:go2cpp_os_monitor_ebpf> --require-ebpf")
    set_tests_properties(go2cpp_os_monitor_ebpf_root PROPERTIES
        TIMEOUT 120
        SKIP_RETURN_CODE 77)

    add_test(NAME go2cpp_os_kernel_monitor_root
             COMMAND bash -c
                 "echo root | sudo -S $<TARGET_FILE:go2cpp_os_kernel_monitor> --require-ebpf")
    set_tests_properties(go2cpp_os_kernel_monitor_root PROPERTIES
        TIMEOUT 240
        SKIP_RETURN_CODE 77)

    add_test(NAME go2cpp_os_tcp_io_monitor_root
             COMMAND bash -c
                 "echo root | sudo -S $<TARGET_FILE:go2cpp_os_tcp_io_monitor> --require-ebpf")
    set_tests_properties(go2cpp_os_tcp_io_monitor_root PROPERTIES
        TIMEOUT 240
        SKIP_RETURN_CODE 77)
endif()
