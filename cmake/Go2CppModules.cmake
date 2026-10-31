include(GNUInstallDirs)
find_package(Threads REQUIRED)
# 自研上下文后端（context_x64.S + FiberStack）只覆盖 Linux x86_64，
# 且不再有 Boost.Context 后备；其它平台在配置阶段直接拒绝，避免
# 生成一个运行时必然失败的目标。
if (NOT CMAKE_SYSTEM_NAME STREQUAL Linux OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
    CMAKE_SYSTEM_PROCESSOR MATCHES [=[aarch64|arm64|riscv|ppc|s390]=])
    message(FATAL_ERROR [=[Go2Cpp fiber backend requires Linux x86_64 (context_x64.S); other platforms are not supported after removing Boost.Context]=])
endif()
# 仍保留该变量供安装包配置使用（go2cpp_runtimeConfig.cmake.in 据此跳过
# find_dependency(Boost)）；后端已无条件为自研实现。
set(GO2CPP_NATIVE_CONTEXT ON)
include(CheckIncludeFileCXX)
check_include_file_cxx(valgrind/valgrind.h GO2CPP_HAVE_VALGRIND_HEADER)

option(BUILD_SHARED_LIBS "Build shared runtime modules" ON)
option(GO2CPP_BUILD_HOOK "Build the Linux syscall interposer" ON)
if (GO2CPP_BUILD_HOOK AND NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR "The hook requires shared runtime modules to keep one TLS/FD registry; use GO2CPP_BUILD_HOOK=OFF for a static runtime")
endif()

add_library(go2cpp_error ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/error.cpp)
add_library(go2cpp_control_flow ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/control_flow.cpp)
add_library(go2cpp_context ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context.cpp)
add_library(go2cpp_channel ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/channel.cpp)
add_library(go2cpp_scheduler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/parking_condition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/thread_policy.cpp)
add_library(go2cpp_fiber
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/fiber_local.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context_backend.cpp)
target_sources(go2cpp_fiber PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context_x64.S)
add_library(go2cpp_sync ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/sync.cpp)
add_library(go2cpp_future ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/future.cpp)

# ---------------------------------------------------------------------------
# 并发热路径编译选项（并发优化清单 #9）
#
# 下面 6 个库构成每次 park/wake/通道操作都要穿过的热路径。它们按共享库构建，
# GCC 因此认为同库内的全局符号可被运行时插替（ELF interposition），从而放弃
# 跨 TU 内联、函数克隆等过程间优化；同时库内对外部符号的调用都要经过 PLT
# 跳板。
#
# - -fno-semantic-interposition：允许编译器在「同库内」内联/克隆导出函数。
#   它不改变 ELF 语义（符号仍导出、仍可被动态链接器解析），只去掉 GCC 为支持
#   运行时插替而加的保守屏障；
# - -fno-plt：外部调用直接走 GOT 间接，省掉 PLT 跳板与一次额外跳转，也让
#   编译器对调用点有更好的调度空间。与顶层已启用的 -Wl,-z,now 兼容：重定位
#   仍在启动期一次性完成，不会回到懒绑定的 xsave 栈崩溃路径。
#
# 作用范围严格限制在这 6 个库目标：不改全局 CMAKE_CXX_FLAGS，也不碰
# go2cpp_hook —— 它的 LD_PRELOAD 拦截语义依赖 PLT，必须保持默认。
# ---------------------------------------------------------------------------
option(GO2CPP_CONCURRENCY_HOT_FLAGS
    "Enable -fno-semantic-interposition/-fno-plt on the concurrency hot libraries" ON)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND GO2CPP_CONCURRENCY_HOT_FLAGS)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-fno-semantic-interposition
        GO2CPP_HOT_HAVE_NO_SEMANTIC_INTERPOSITION)
    check_cxx_compiler_flag(-fno-plt GO2CPP_HOT_HAVE_NO_PLT)
    check_cxx_compiler_flag(-fno-stack-protector GO2CPP_HOT_HAVE_NO_STACK_PROTECTOR)

    set(GO2CPP_HOT_LIB_TARGETS
        go2cpp_context
        go2cpp_channel
        go2cpp_scheduler
        go2cpp_fiber
        go2cpp_sync
        go2cpp_future)
    foreach(hot_target IN LISTS GO2CPP_HOT_LIB_TARGETS)
        set(hot_target_flags)
        if (GO2CPP_HOT_HAVE_NO_SEMANTIC_INTERPOSITION)
            list(APPEND hot_target_flags -fno-semantic-interposition)
        endif()
        if (GO2CPP_HOT_HAVE_NO_PLT)
            list(APPEND hot_target_flags -fno-plt)
        endif()
        # 热路径函数每任务都带 canary（约 148 个函数、296 处访问），
        # 对已经过 ASan/TSan 与边界测试的并发原语去掉栈保护可省 2-5ns/任务。
        if (GO2CPP_HOT_HAVE_NO_STACK_PROTECTOR)
            list(APPEND hot_target_flags -fno-stack-protector)
        endif()
        if (hot_target_flags AND TARGET ${hot_target})
            target_compile_options(${hot_target} PRIVATE ${hot_target_flags})
        endif()
        # -Bsymbolic-functions：库内对该库导出函数的引用直接绑定到本地定义，
        # 不再经 GOT 间接（汇编审查实测 GOT 间接 476→138）。与
        # -fno-semantic-interposition 互补；不改符号可见性。
        if (TARGET ${hot_target})
            target_link_options(${hot_target} PRIVATE -Wl,-Bsymbolic-functions)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 网络 / Bento 热路径编译选项（汇编审查 Top-8：feed_impl 66/119 个调用走 @plt）
#
# 与上面并发热库同一套理由：共享库内的外部调用默认经 PLT 跳板（一次额外
# 间接跳转 + GOT 载入，并阻止调用点的调度优化）。顶层已启用 -Wl,-z,now，
# 重定位在加载期一次性完成，-fno-plt 不会退回懒绑定，也不会重新引入
# xsave 栈溢出路径。
#
# 注意：
#   - 只作用于 go2cpp_network 与 go2cpp_bento*，不改全局 CMAKE_CXX_FLAGS；
#   - go2cpp_hook 必须保持默认（它的 LD_PRELOAD 拦截语义依赖 PLT）；
#   - bento 目标在本文件之后（Go2CppBento.cmake）才定义，因此用
#     cmake_language(DEFER) 把选项附加推迟到顶层 CMakeLists 处理结束时。
# ---------------------------------------------------------------------------
option(GO2CPP_NETWORK_HOT_FLAGS
    "Enable -fno-plt on the network/bento hot libraries" ON)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND GO2CPP_NETWORK_HOT_FLAGS)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-fno-plt GO2CPP_NETWORK_HAVE_NO_PLT)
    if (GO2CPP_NETWORK_HAVE_NO_PLT)
        function(go2cpp_apply_network_hot_flags)
            foreach(network_hot_target
                    go2cpp_network
                    go2cpp_bento
                    go2cpp_bento_app
                    go2cpp_bento_transport
                    go2cpp_bento_capability
                    go2cpp_bento_session
                    go2cpp_bento_omni
                    go2cpp_bento_error
                    go2cpp_bento_keystore
                    go2cpp_bento_http
                    go2cpp_bento_ws
                    go2cpp_bento_socks)
                if (TARGET ${network_hot_target})
                    target_compile_options(${network_hot_target} PRIVATE -fno-plt)
                endif()
            endforeach()
        endfunction()
        cmake_language(DEFER CALL go2cpp_apply_network_hot_flags)
        message(STATUS "network/bento hot flags: -fno-plt")
    endif()
endif()
# ---------------------------------------------------------------------------
# TLS 访问模型：整 TU 的 initial-exec（TLS 内联落地）
#
# 变量级 tls_model("initial-exec") 只覆盖变量本身的数据访问；非平凡
# thread_local 的初始化守卫（__tls_guard）是编译器生成的独立 TLS 符号，
# 仍按 TU 默认的 local-dynamic 寻址，所以同库仍有 `call __tls_get_addr`
# （scheduler 基线 78 条）。只有整个 TU 加 -ftls-model=initial-exec 才能把
# 数据访问和 guard 一起内联；影子编译/链接实测 10 个目标 261 -> 0。
#
# 范围：并发 6 库 + io/network/bufio/p2p。这些目标已核对过 TU 依赖闭包，
# 不含 tools/replay 的 default-visibility inline thread_local
# （replay_source.hpp / gid.hpp / ucx.hpp）——那类符号在 -shared 下会生成
# R_X86_64_TPOFF32，GNU ld 直接拒绝链接。
#
# 独立开关 GO2CPP_TLS_INITIAL_EXEC：默认 ON；需要把这些库真正 dlopen 的
# 场景显式关掉，整体回到 general-dynamic。原因：initial-exec 会让 .so 带上
# DF_STATIC_TLS，dlopen 时从 glibc 静态 TLS surplus 分配（实测约 1.6KB；
# scheduler.so 单库 960B，本组 10 库合计 2KB+），叠加后必报
# "cannot allocate memory in static TLS block"。走 DT_NEEDED 的常规
# 进程启动没有这个约束。
#
# 附加时机：io/network/bufio 在本块之后定义，p2p 更是到 Go2CppP2P.cmake 才
# 定义，所以用 cmake_language(DEFER) 推迟到顶层目录处理结束时再附加。
# 与 GO2CPP_CONCURRENCY_HOT_FLAGS 正交：后者管 -fno-semantic-interposition /
# -fno-plt，本开关只管 TLS 模型，两者可独立关闭。
# ---------------------------------------------------------------------------
option(GO2CPP_TLS_INITIAL_EXEC
    "Use -ftls-model=initial-exec on the concurrency/io/network/bufio/p2p libraries (turn OFF when these libraries must be dlopen()ed)" ON)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND GO2CPP_TLS_INITIAL_EXEC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-ftls-model=initial-exec GO2CPP_HOT_HAVE_TLS_INITIAL_EXEC)
    if (GO2CPP_HOT_HAVE_TLS_INITIAL_EXEC)
        function(go2cpp_apply_tls_initial_exec)
            foreach(tls_ie_target
                    go2cpp_context
                    go2cpp_channel
                    go2cpp_scheduler
                    go2cpp_fiber
                    go2cpp_sync
                    go2cpp_future
                    go2cpp_io
                    go2cpp_network
                    go2cpp_bufio
                    go2cpp_p2p)
                if (TARGET ${tls_ie_target})
                    target_compile_options(${tls_ie_target} PRIVATE -ftls-model=initial-exec)
                endif()
            endforeach()
        endfunction()
        cmake_language(DEFER CALL go2cpp_apply_tls_initial_exec)
        message(STATUS "TLS: -ftls-model=initial-exec on concurrency/io/network/bufio/p2p")
    endif()
endif()
add_library(go2cpp_io
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io_backend.cpp)
add_library(go2cpp_log ${CMAKE_CURRENT_SOURCE_DIR}/src/logging/log.cpp)
# TLS（TlsTransport）可选：GO2CPP_WITH_TLS=ON 时才 find_package(OpenSSL)
# 并链接；OFF 时 tls_transport.cpp 只编译 stub，不引入 libssl/libcrypto。
# Bento 的 PSK 加密是自研 ChaCha20/X25519，不依赖 OpenSSL；只有 http2/tls
# 模块需要它，因此默认值保持 ON 以兼容既有行为，但可以整体关掉。
option(GO2CPP_WITH_TLS "Build the OpenSSL-backed TLS transport" ON)
if (GO2CPP_WITH_TLS)
    find_package(OpenSSL REQUIRED)
endif()
add_library(go2cpp_network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/address.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/byte_array.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/fd_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/socket.cpp
    # SocketDebug 最小实现：OFF 时该 TU 为空（只有 include + #if），
    # 接入点也被宏消除，因此默认构建的产物与之前一致。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/socket_debug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/sse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/tls_transport.cpp
    # HTTP/2 协议核心：此前只在测试目标里编译，导出库缺这些符号，生产
    # 使用需自行加源文件（见 docs/modules/http2.md §6）。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/huffman.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/hpack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/stream_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/connection.cpp)
add_library(go2cpp_config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/config_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/so_manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/so_reloader.cpp)

# math 模块：Go math 最小子集。绝大多数函数委托 libm；Max/Min 与 Modf 的
# 特殊值语义按 Go 文档自实现（详见 include/go2cpp/math/math.hpp）。
add_library(go2cpp_math ${CMAKE_CURRENT_SOURCE_DIR}/src/math/math.cpp)
# math/rand 模块：xoshiro256** + splitmix64 播种；全局函数用 std::mutex 保护，
# 本地 Rand 实例不加锁（对齐 Go 的顶层函数 / *rand.Rand 划分）。
add_library(go2cpp_rand ${CMAKE_CURRENT_SOURCE_DIR}/src/math/rand.cpp)

# os 模块：文件/目录/环境变量/参数。走 syscall 绕过 Hook（见 os/file.hpp 的边界论证），
# 依赖 config（src/os/config.cpp 用 Config::SaveToFile 导出 app.ini）。
# os 模块：文件/目录/环境变量/参数 + 多进程/IPC（R1-R31）。走裸 syscall
# 绕过 Hook（见 os/proc.hpp 的边界论证），依赖 config 与 io（IOManager 异步适配）。
add_library(go2cpp_os
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/raw_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/io_capability.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/proc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/shm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/mq.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/pty.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/supervisor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/pool.cpp)
target_link_libraries(go2cpp_os PUBLIC go2cpp_config PRIVATE go2cpp_io)

# path 模块：Go `path`（POSIX 纯字符串）+ `path/filepath`（平台相关）。
# 不依赖 os 模块 —— 边界清晰：os 不做路径规范化，path 不做文件读写。
# path.cpp 零外部依赖；filepath.cpp 只用 libc 的 lstat/readlink/getcwd/opendir。
add_library(go2cpp_path
    ${CMAKE_CURRENT_SOURCE_DIR}/src/path/path.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/path/filepath.cpp)

# container 模块：Go `container/heap`、`container/list`、`container/ring`。
# 三个容器都是 header-only 模板；container.cpp 只做头文件自包含检查，
# 同时提供真实 library target，以复用项目按模块安装/导出的机制。
add_library(go2cpp_container
    ${CMAKE_CURRENT_SOURCE_DIR}/src/container/container.cpp)

# compress 模块：Go `compress/flate`、`compress/zlib`、`compress/gzip` 的薄封装。
# **自研 DEFLATE**：LZ77 滑动窗口 + 固定 Huffman 编码在 src/compress/
# deflate_writer.hpp，解码器（stored/fixed/dynamic Huffman）在
# deflate_reader.hpp。RFC 1950/1952 的头尾与 adler32/crc32 也由本模块实现，
# 因此不再需要 find_package(ZLIB)，也不链接 libz。
add_library(go2cpp_compress
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/compress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/deflater.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/inflater.cpp)
# go2cpp_error 是 PUBLIC 依赖：Reader/Writer 是头文件模板，模板实例化时会调用
# go2cpp::Is / streams::IoEof，使用方必须能链接到 error 模块。
target_link_libraries(go2cpp_compress PUBLIC go2cpp_error)

# encoding 模块：Go `encoding/{base64,hex,binary,csv}` 的 C++ 对应实现。
# 纯 CPU、无 IO / 调度依赖，只链接 error（错误身份锚点）。
add_library(go2cpp_encoding
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/base64.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/hex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/binary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/csv.cpp)
target_link_libraries(go2cpp_encoding PUBLIC go2cpp_error)
set_target_properties(go2cpp_encoding PROPERTIES EXPORT_NAME encoding)
add_library(go2cpp::encoding ALIAS go2cpp_encoding)

# hash 模块：Go hash/crc32、hash/adler32 与 crypto/md5|sha1|sha256 的零依赖实现。
# CRC32 = 基础 256 表 + slicing-by-8；Adler32 = 5552 分块；MD5/SHA-1/SHA-256 按
# RFC 1321 / FIPS 180-4 自实现。统一 Hash 接口对齐 Go hash.Hash 的 Sum/Write 语义。
# 无 error 依赖：本模块不返回 go2cpp::Error，只返回摘要字节。
add_library(go2cpp_hash
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/adler32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/crc32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/hash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/md5.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/sha1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/sha256.cpp)
set_target_properties(go2cpp_hash PROPERTIES EXPORT_NAME hash)
add_library(go2cpp::hash ALIAS go2cpp_hash)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GO2CPP_BUILD_HOOK)
    add_library(go2cpp_hook SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/network/hook.cpp)
endif()

# The public umbrella target keeps one stable link name while the module
# targets remain independently inspectable and replaceable by embedders.
add_library(go2cpp_runtime INTERFACE)
add_library(go2cpp::runtime ALIAS go2cpp_runtime)

set_target_properties(go2cpp_error PROPERTIES EXPORT_NAME error)
set_target_properties(go2cpp_control_flow PROPERTIES EXPORT_NAME control_flow)
set_target_properties(go2cpp_context PROPERTIES EXPORT_NAME context)
set_target_properties(go2cpp_channel PROPERTIES EXPORT_NAME channel)
set_target_properties(go2cpp_scheduler PROPERTIES EXPORT_NAME scheduler)
set_target_properties(go2cpp_fiber PROPERTIES EXPORT_NAME fiber)
set_target_properties(go2cpp_sync PROPERTIES EXPORT_NAME sync)
set_target_properties(go2cpp_future PROPERTIES EXPORT_NAME future)
set_target_properties(go2cpp_io PROPERTIES EXPORT_NAME io)
set_target_properties(go2cpp_network PROPERTIES EXPORT_NAME network)
set_target_properties(go2cpp_log PROPERTIES EXPORT_NAME log)
set_target_properties(go2cpp_config PROPERTIES EXPORT_NAME config)
set_target_properties(go2cpp_math PROPERTIES EXPORT_NAME math)
set_target_properties(go2cpp_rand PROPERTIES EXPORT_NAME rand)
set_target_properties(go2cpp_path PROPERTIES EXPORT_NAME path)
set_target_properties(go2cpp_container PROPERTIES EXPORT_NAME container)
set_target_properties(go2cpp_compress PROPERTIES EXPORT_NAME compress)
if (TARGET go2cpp_hook)
    set_target_properties(go2cpp_hook PROPERTIES EXPORT_NAME hook)
endif()
set_target_properties(go2cpp_runtime PROPERTIES EXPORT_NAME runtime)

# Keep the source-tree and installed package spellings identical. The aliases
# are not a second library or ABI; they only make add_subdirectory consumers
# use the same names as find_package consumers.
add_library(go2cpp::error ALIAS go2cpp_error)
add_library(go2cpp::control_flow ALIAS go2cpp_control_flow)
add_library(go2cpp::context ALIAS go2cpp_context)
add_library(go2cpp::channel ALIAS go2cpp_channel)
add_library(go2cpp::scheduler ALIAS go2cpp_scheduler)
add_library(go2cpp::fiber ALIAS go2cpp_fiber)
add_library(go2cpp::sync ALIAS go2cpp_sync)
add_library(go2cpp::future ALIAS go2cpp_future)
add_library(go2cpp::io ALIAS go2cpp_io)
add_library(go2cpp::network ALIAS go2cpp_network)
add_library(go2cpp::log ALIAS go2cpp_log)
add_library(go2cpp::config ALIAS go2cpp_config)
add_library(go2cpp::os ALIAS go2cpp_os)
add_library(go2cpp::math ALIAS go2cpp_math)
add_library(go2cpp::rand ALIAS go2cpp_rand)
add_library(go2cpp::path ALIAS go2cpp_path)
add_library(go2cpp::container ALIAS go2cpp_container)
add_library(go2cpp::compress ALIAS go2cpp_compress)
if (TARGET go2cpp_hook)
    add_library(go2cpp::hook ALIAS go2cpp_hook)
endif()

# metrics 模块：Counter / Gauge / Histogram + Prometheus 文本导出。
# 并发安全：Counter/Gauge 用 atomic，Histogram 每桶一个 atomic。
# 无外部依赖；Render() 输出符合 Prometheus 文本格式（已用 prometheus_client 验证）。
add_library(go2cpp_metrics src/metrics/metrics.cpp)
set_target_properties(go2cpp_metrics PROPERTIES EXPORT_NAME metrics)
add_library(go2cpp::metrics ALIAS go2cpp_metrics)

# 事件环（EventRing）：scheduler/io 在 GO2CPP_DEBUG_DUMP=ON 时**直接调用**
# EventRing::Record，所以它的实现不能放在 go2cpp_debug 里 —— 否则
# scheduler/io 必须反向链接 go2cpp_debug，与 go2cpp_debug -> scheduler 构成
# 目标依赖环，只链接 scheduler 的测试目标会得到 undefined reference。
# 拆成零依赖小库后：scheduler/io -> debug_event -> libc，debug -> debug_event。
add_library(go2cpp_debug_event
    ${CMAKE_CURRENT_SOURCE_DIR}/src/debug/debug_event_ring.cpp)
set_target_properties(go2cpp_debug_event PROPERTIES EXPORT_NAME debug_event)
add_library(go2cpp::debug_event ALIAS go2cpp_debug_event)

# debug 模块：统一只读内省接口（scheduler/fiber/channel/parking_condition/
# sync/io）+ Unix socket DebugServer。
#
# 依赖方向：debug -> {scheduler,fiber,sync,io,debug_event}；被观测模块只依赖
# 零依赖的 debug_event（且仅在 GO2CPP_DEBUG_DUMP=ON 时建立链接）。
add_library(go2cpp_debug
    ${CMAKE_CURRENT_SOURCE_DIR}/src/debug/debug_dump.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/debug/debug_server.cpp)
# Doctor（在线诊断）：默认 OFF 时不编译该源文件，go2cpp_debug 中不存在
# DiagnosticAgent 符号；打开后由同一个 target 承载（不新增对外 target，
# 避免 install(EXPORT) 的 ABI 面扩大）。
if (GO2CPP_DIAGNOSTIC)
    target_sources(go2cpp_debug PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/debug/diagnostic_agent.cpp)
endif()
target_link_libraries(go2cpp_debug PUBLIC
    go2cpp_scheduler go2cpp_fiber go2cpp_sync go2cpp_io go2cpp_debug_event)
set_target_properties(go2cpp_debug PROPERTIES EXPORT_NAME debug)
add_library(go2cpp::debug ALIAS go2cpp_debug)

set(GO2CPP_MODULE_TARGETS
    go2cpp_error
    go2cpp_control_flow
    go2cpp_context
    go2cpp_channel
    go2cpp_scheduler
    go2cpp_fiber
    go2cpp_sync
    go2cpp_future
    go2cpp_io
    go2cpp_network
    go2cpp_log
    go2cpp_config
    # go2cpp_config 的公共头（config_codec.hpp）与模板实例化依赖 Reflect；
    # 它必须随 config 一起安装/导出，否则 install(EXPORT) 会因
    # 「go2cpp_config 依赖不在导出集的 go2cpp_reflect」而失败。
    go2cpp_reflect
    go2cpp_os
    go2cpp_math
    go2cpp_rand
    go2cpp_path
    go2cpp_container
    go2cpp_compress
    go2cpp_encoding
    go2cpp_hash
    go2cpp_metrics
    go2cpp_debug_event
    go2cpp_debug
)
if (TARGET go2cpp_hook)
    list(APPEND GO2CPP_MODULE_TARGETS go2cpp_hook)
endif()

foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
    target_include_directories(${module_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${module_target} PUBLIC cxx_std_17)
    set_target_properties(${module_target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${module_target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_options(${module_target} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${module_target} PUBLIC Threads::Threads)
endforeach()

# 显式链接 libm：std::sqrt/exp/log 等符号在部分工具链/链接方式下不会由
# libstdc++ 传递；Go math 模块依赖它们，不能靠隐式依赖碰运气。
if (UNIX)
    target_link_libraries(go2cpp_math PUBLIC m)
endif()

target_include_directories(go2cpp_runtime INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(go2cpp_context PUBLIC go2cpp_error go2cpp_scheduler)
target_link_libraries(go2cpp_channel PUBLIC go2cpp_context go2cpp_error)
target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_fiber go2cpp_log)
target_link_libraries(go2cpp_config PUBLIC go2cpp_scheduler go2cpp_log go2cpp_hash go2cpp_reflect)
target_link_libraries(go2cpp_sync PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_future PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_io PUBLIC go2cpp_context go2cpp_scheduler)
# GO2CPP_DEBUG_DUMP=ON 时 io.cpp 直接记录 EventRing 事件，必须能解析符号。
# OFF 时宏展开为 ((void)0)，不建立这条额外链接，保持原依赖图。
if (GO2CPP_DEBUG_DUMP)
    target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_debug_event)
    target_link_libraries(go2cpp_io PUBLIC go2cpp_debug_event)
    # sync.cpp 的 Mutex 慢路径与 channel.hpp（模板）的阻塞路径直接记录事件；
    # OFF 构建宏为空展开，不建立这两条链接。
    target_link_libraries(go2cpp_sync PUBLIC go2cpp_debug_event)
    target_link_libraries(go2cpp_channel PUBLIC go2cpp_debug_event)
    # fiber.cpp 在 park（Fiber::Suspend）时记录资源事件并维护 Fiber 链样本；
    # OFF 构建宏空展开，不建立这条链接。
    target_link_libraries(go2cpp_fiber PUBLIC go2cpp_debug_event)
endif()
target_link_libraries(go2cpp_network PUBLIC go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync)
# SocketDebug 的编译期宏只注入 network 自身：socket_debug.cpp 与 socket.cpp
# 同属这个 target，宏定义一致；不 include socket_debug.hpp 的其他模块不受
# 影响（也不会因为一个调试开关触发全量重编译）。
if (GO2CPP_DEBUG_SOCKET)
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_DEBUG_SOCKET=1)
endif()
# HOOK=OFF 时不存在 go2cpp_hook 目标。Socket::accept 对
# hook::mark_system_nonblocking 的调用必须随宏一起消失，否则共享库会留下
# 未解析符号，只链接 network/io 的程序直接链接失败；HOOK=ON 时把 hook 作为
# 私有依赖接进来，保证符号在加载期可解析。
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_network PRIVATE go2cpp_hook)
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_NETWORK_HOOK_ENABLED=1)
else()
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_NETWORK_HOOK_ENABLED=0)
endif()
# TlsTransport 的实现细节：OpenSSL 只出现在 .cpp 里，且只在 TLS=ON 时链接。
if (GO2CPP_WITH_TLS)
    target_link_libraries(go2cpp_network PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_WITH_TLS=1)
else()
    # 关闭时不链接 OpenSSL；tls_transport.cpp 编译成 stub，入口返回 nullptr/
    # 空结果，测试二进制仍可链接，但不会悄悄降级成明文连接。
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_WITH_TLS=0)
    message(STATUS "TLS transport: OFF (TlsTransport stubs; OpenSSL not linked)")
endif()
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_hook PUBLIC go2cpp_io go2cpp_fiber
                          go2cpp_config ${CMAKE_DL_LIBS})
endif()
target_link_libraries(go2cpp_runtime INTERFACE
    go2cpp_error go2cpp_control_flow go2cpp_context go2cpp_channel go2cpp_scheduler
    go2cpp_fiber go2cpp_sync go2cpp_future go2cpp_io go2cpp_network go2cpp_log go2cpp_config go2cpp_os
    go2cpp_math go2cpp_rand go2cpp_path go2cpp_container go2cpp_compress go2cpp_encoding go2cpp_hash
    Threads::Threads
    go2cpp_metrics go2cpp_debug
)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_runtime INTERFACE go2cpp_hook)
    # An interposer provides symbols through libc calls, not explicit API
    # references. Keep the DSO even when the executable never names a hook.
    target_link_options(go2cpp_runtime INTERFACE "LINKER:--no-as-needed")
endif()

if (GO2CPP_ENABLE_TSAN)
    foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
        target_compile_options(${module_target} PUBLIC
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${module_target} PUBLIC -fsanitize=thread)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 可选的 picoquic 薄封装
#
# 为什么默认 OFF：picoquic + picotls 是外部依赖（合计约 240 万字节静态库），
# 且头文件路径因机器而异。强制依赖会让每个开发者都要先装它们。
#
# 打开方式：
#   cmake -S . -B build -DGO2CPP_WITH_PICOQUIC=ON \
#         -DPICOQUIC_ROOT=/path/to/picoquic \
#         -DPICOTLS_ROOT=/path/to/picotls
#
# 关着的时候 `picoquic_connection.cpp` 不参与编译；头文件仍在但**不提供实现**
# （使用者会得到链接期 undefined reference —— 这是正确的，不会静默用到空实现）。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_PICOQUIC "Build the picoquic-based QUIC transport" OFF)
set(PICOQUIC_ROOT "" CACHE PATH "picoquic source root (contains picoquic/picoquic.h)")
set(PICOTLS_ROOT "" CACHE PATH "picotls root (contains include/picotls/ and libpicotls-*.a)")

if (GO2CPP_WITH_PICOQUIC)
    if (NOT PICOQUIC_ROOT OR NOT PICOTLS_ROOT)
        message(FATAL_ERROR
            "GO2CPP_WITH_PICOQUIC=ON 需要同时设置 PICOQUIC_ROOT 与 PICOTLS_ROOT")
    endif()

    add_library(go2cpp_quic_picoquic STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/quic/picoquic_connection.cpp)
    target_include_directories(go2cpp_quic_picoquic
        PRIVATE
        ${PICOQUIC_ROOT}/picoquic
        ${PICOTLS_ROOT}/include)
    target_link_libraries(go2cpp_quic_picoquic
        PUBLIC go2cpp_network
        PRIVATE
        ${PICOQUIC_ROOT}/build/libpicoquic-core.a
        ${PICOQUIC_ROOT}/build/libpicoquic-log.a
        ${PICOTLS_ROOT}/libpicotls-core.a
        ${PICOTLS_ROOT}/libpicotls-openssl.a
        ${PICOTLS_ROOT}/libpicotls-minicrypto.a
        ${PICOTLS_ROOT}/libpicotls-fusion.a
        ssl crypto pthread)

    add_executable(go2cpp_picoquic_hq_client
        ${CMAKE_CURRENT_SOURCE_DIR}/example/network/picoquic_hq_client.cpp)
    target_link_libraries(go2cpp_picoquic_hq_client PRIVATE go2cpp_quic_picoquic)

    # HTTP/3 客户端：QPACK 与 h3 帧全部走 picohttp（h3zero），不自研。
    # libpicohttp-core.a 必须排在 libpicoquic-core.a 之前（静态库符号解析顺序）。
    add_library(go2cpp_http3_picoquic STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http3/client.cpp)
    target_include_directories(go2cpp_http3_picoquic
        PRIVATE
        ${PICOQUIC_ROOT}/picoquic
        ${PICOQUIC_ROOT}/picohttp
        ${PICOTLS_ROOT}/include)
    target_compile_options(go2cpp_http3_picoquic PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(go2cpp_http3_picoquic
        PUBLIC go2cpp_quic_picoquic
        PRIVATE
        ${PICOQUIC_ROOT}/build/libpicohttp-core.a
        ${PICOQUIC_ROOT}/build/libpicoquic-core.a
        ${PICOQUIC_ROOT}/build/libpicoquic-log.a
        ${PICOTLS_ROOT}/libpicotls-core.a
        ${PICOTLS_ROOT}/libpicotls-openssl.a
        ${PICOTLS_ROOT}/libpicotls-minicrypto.a
        ${PICOTLS_ROOT}/libpicotls-fusion.a
        ssl crypto pthread)

    message(STATUS "picoquic QUIC transport: ON (picoquic=${PICOQUIC_ROOT})")
else()
    message(STATUS "picoquic QUIC transport: OFF (set GO2CPP_WITH_PICOQUIC=ON to enable)")
endif()

# bufio 模块：带缓冲的 IO（Reader/Writer/Scanner，语义对齐 Go 的 bufio 包）。
add_library(go2cpp_bufio src/bufio/bufio.cpp)

target_link_libraries(go2cpp_bufio PUBLIC go2cpp_error)
set_target_properties(go2cpp_bufio PROPERTIES EXPORT_NAME bufio)
add_library(go2cpp::bufio ALIAS go2cpp_bufio)
list(APPEND GO2CPP_MODULE_TARGETS go2cpp_bufio)

# 迁移格式层与 demo 只在实验开关打开时进入构建；它们不进入
# GO2CPP_MODULE_TARGETS，因此默认安装/导出与 ON 构建的安装面都不变。
if (GO2CPP_MIGRATION_API)
    include(${PROJECT_SOURCE_DIR}/cmake/Go2CppMigration.cmake)
endif()
