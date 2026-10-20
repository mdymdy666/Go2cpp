include(GNUInstallDirs)
find_package(Threads REQUIRED)
set(GO2CPP_NATIVE_CONTEXT OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    set(GO2CPP_NATIVE_CONTEXT ON)
endif()
if (NOT GO2CPP_NATIVE_CONTEXT)
    find_package(Boost 1.70 REQUIRED COMPONENTS context)
endif()
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
if (GO2CPP_NATIVE_CONTEXT)
    target_sources(go2cpp_fiber PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context_x64.S)
    target_compile_definitions(go2cpp_fiber PRIVATE GO2CPP_USE_NATIVE_CONTEXT=1)
endif()
add_library(go2cpp_sync ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/sync.cpp)
add_library(go2cpp_future ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/future.cpp)
add_library(go2cpp_io
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io_backend.cpp)
add_library(go2cpp_log ${CMAKE_CURRENT_SOURCE_DIR}/src/logging/log.cpp)
# TLS（TlsTransport）依赖 OpenSSL。公共头 tls_transport.hpp 只前置声明
# SSL/SSL_CTX，OpenSSL 头不会泄漏给使用方，因此作为 PRIVATE 依赖接入。
find_package(OpenSSL REQUIRED)
add_library(go2cpp_network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/address.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/byte_array.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/fd_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/socket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/sse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http2/tls_transport.cpp)
add_library(go2cpp_config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/config.cpp
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
add_library(go2cpp_os
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp)
target_link_libraries(go2cpp_os PUBLIC go2cpp_config)

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
# **不自研 DEFLATE**：Huffman 编码 + LZ77 滑动窗口全部委托系统 zlib
# （RFC 1951/1950/1952），本模块只做流式 IO 适配、多成员 gzip 与错误翻译。
#
# find_package 而不是硬编码 `-lz` 或 `/usr/lib/libz.so`：库名、版本、是否
# 拆成 libz.so/libz.a 因发行版而异，导入目标 ZLIB::ZLIB 也自带 include 路径。
# 缺 zlib 时 configure **直接失败**——有意为之：若退化成「编译通过但不压缩」，
# 会静默产出坏数据，比构建失败难查得多。
find_package(ZLIB REQUIRED)
add_library(go2cpp_compress
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/compress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/deflater.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compress/inflater.cpp)
# go2cpp_error 是 PUBLIC 依赖：Reader/Writer 是头文件模板，模板实例化时会调用
# go2cpp::Is / streams::IoEof，使用方必须能链接到 error 模块。
target_link_libraries(go2cpp_compress PUBLIC go2cpp_error ZLIB::ZLIB)

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
target_link_libraries(go2cpp_config PUBLIC go2cpp_scheduler go2cpp_log go2cpp_hash)
if (GO2CPP_NATIVE_CONTEXT)
    # Linux x86_64 uses the internal context ABI; it no longer needs the
    # Boost.Context protected-stack implementation at link time.
else()
    target_link_libraries(go2cpp_fiber PUBLIC Boost::context)
    if (GO2CPP_HAVE_VALGRIND_HEADER)
        target_compile_definitions(go2cpp_fiber PRIVATE BOOST_USE_VALGRIND=1)
    endif()
endif()
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
endif()
target_link_libraries(go2cpp_network PUBLIC go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync)
# TlsTransport 的实现细节：OpenSSL 只出现在 .cpp 里。
target_link_libraries(go2cpp_network PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_hook PUBLIC go2cpp_io go2cpp_fiber
                          go2cpp_config ${CMAKE_DL_LIBS})
endif()
target_link_libraries(go2cpp_runtime INTERFACE
    go2cpp_error go2cpp_control_flow go2cpp_context go2cpp_channel go2cpp_scheduler
    go2cpp_fiber go2cpp_sync go2cpp_future go2cpp_io go2cpp_network go2cpp_log go2cpp_config
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
