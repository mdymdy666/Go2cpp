# 一键全能力构建（人肉开关）：同时打开 DEBUG_DUMP / DIAGNOSTIC / REPLAY /
# MIGRATION_API。适合「一条命令产出五能力测试所需的全部二进制」：
#   cmake -S . -B build-enhanced -DGO2CPP_BUILD_ENHANCED=ON -DGO2CPP_WITH_CLI=ON
#   cmake --build build-enhanced -j
# 默认 OFF：各能力开关仍可单独使用，默认构建保持零开销（热路径宏被消除）。
option(GO2CPP_BUILD_ENHANCED
       "One-switch preset: DEBUG_DUMP + DIAGNOSTIC + REPLAY + MIGRATION_API" OFF)
if (GO2CPP_BUILD_ENHANCED)
    set(GO2CPP_DEBUG_DUMP ON CACHE BOOL "preset by GO2CPP_BUILD_ENHANCED" FORCE)
    set(GO2CPP_DIAGNOSTIC ON CACHE BOOL "preset by GO2CPP_BUILD_ENHANCED" FORCE)
    set(GO2CPP_REPLAY ON CACHE BOOL "preset by GO2CPP_BUILD_ENHANCED" FORCE)
    set(GO2CPP_MIGRATION_API ON CACHE BOOL "preset by GO2CPP_BUILD_ENHANCED" FORCE)
    # GO2CPP_WITH_CLI 在顶层 CMakeLists 定义（本文件先被 include）；
    # 用户显式 -DGO2CPP_WITH_CLI=... 时尊重用户选择，否则一并打开。
    if (NOT DEFINED GO2CPP_WITH_CLI)
        set(GO2CPP_WITH_CLI ON CACHE BOOL "preset by GO2CPP_BUILD_ENHANCED" FORCE)
    endif()
    message(STATUS "GO2CPP_BUILD_ENHANCED=ON: DEBUG_DUMP/DIAGNOSTIC/REPLAY/MIGRATION_API all ON")
endif()

option(GO2CPP_BUILD_TESTS "Build Go2Cpp runtime tests" ON)
option(GO2CPP_BUILD_EXAMPLES "Build Go2Cpp runtime examples" ON)
option(GO2CPP_ENABLE_TSAN "Build with ThreadSanitizer when supported" OFF)
option(GO2CPP_BUILD_BENCHMARKS "Build standalone performance benchmarks" OFF)
# 调试内省总开关。OFF（默认）时：
#   - 事件环记录点被宏完全消除，热路径零开销；
#   - DebugServer 不监听 socket（Start() 返回 false）；
#   - 组件只读 dump 函数仍可调用（它们只在被调用时工作）。
# ON 时记录 park/wake/runqueue 事件并启用 Unix socket 服务，用于重放与迁移。
option(GO2CPP_DEBUG_DUMP "Build debug dump event ring and Unix socket server" OFF)

# SocketDebug（最小实现）：per-fd 计数 + 256 条事件环 + 关闭归档。
# 默认 OFF：宏只在 go2cpp_network 内部定义，socket_debug.hpp 里默认为 0，
#   socket.cpp 的 GO2CPP_SOCKET_DEBUG_RECORD(...) 走 if constexpr(false) 被
#   整块丢弃，热路径零开销，也不产生任何 SocketDebug 符号。
# ON 时只给 go2cpp_network 与专项测试目标注入宏：其他模块不 include 该头，
#   因此不会出现「同一个类两种定义」的 ODR 风险，也不会触发全量重编译。
option(GO2CPP_DEBUG_SOCKET
       "Build per-socket debug counters and 256-event rings" OFF)
if (GO2CPP_DEBUG_SOCKET)
    message(STATUS "SocketDebug: ON (per-fd counters + 256-event ring)")
endif()

# 回放资源事件录制（GO2CPP_REPLAY=ON）。
#
# 只启用「资源注册表 + 资源事件转发」，不启用事件环/在线诊断：
#   - sync/channel/parking_condition 的 GO2CPP_DEBUG_RESOURCE_EVENT 宏展开；
#   - 事件转发给 tools/replay 的 Recorder 安装的 sink（与 channel/
#     select/timer 共用同一条 G2CRPLY v4 seq 流）；
#   - 不写环形缓冲、不启动 DebugServer。
# 与 GO2CPP_DEBUG_DUMP 可同时打开（两个数据源都工作，便于对照
# --view history 与回放结果）。默认 OFF：热路径零开销。
option(GO2CPP_REPLAY "Build replay recording hooks for resource events" OFF)
if (GO2CPP_REPLAY)
    add_compile_definitions(GO2CPP_REPLAY=1)
endif()

# 事件环两段容量（编译期常量，仅 GO2CPP_DEBUG_DUMP=ON 时生效）：
#   - 普通区：park / runqueue push-pop 等高频事件，写满后覆盖最旧；
#   - 关键区：wake / enqueue 拒绝 / claim 失败等关键事件，不被普通事件覆盖。
# 改这两个值需要重新 configure + 全量编译（宏会进入所有 TU）。
set(GO2CPP_DEBUG_EVENT_CAPACITY 256 CACHE STRING
    "Per-thread normal event ring capacity (GO2CPP_DEBUG_DUMP=ON)")
set(GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY 64 CACHE STRING
    "Per-thread critical event ring capacity (GO2CPP_DEBUG_DUMP=ON)")
if (GO2CPP_DEBUG_DUMP)
    add_compile_definitions(GO2CPP_DEBUG_DUMP=1)
    add_compile_definitions(
        GO2CPP_DEBUG_EVENT_CAPACITY=${GO2CPP_DEBUG_EVENT_CAPACITY})
    add_compile_definitions(
        GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY=${GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY})
endif()

# 跨进程 Fiber/Channel 迁移 API（实验性）。默认 OFF：
#   - 不编译迁移实现、不链接 tools/migration 的格式层；
#   - Fiber 的迁移等待路径不存在，Channel<T> 没有额外虚表，
#     类型布局与热路径与不定义该宏时完全一致。
# 打开方式：cmake -DGO2CPP_MIGRATION_API=ON
option(GO2CPP_MIGRATION_API
       "Build the experimental cross-process Fiber/Channel migration API" OFF)
if (GO2CPP_MIGRATION_API)
    add_compile_definitions(GO2CPP_MIGRATION_API=1)
endif()

# 在线诊断代理（Doctor）。默认 OFF（零开销）：
#   - 不编译 src/debug/diagnostic_agent.cpp，go2cpp_debug 里没有 Doctor 符号；
#   - 不创建线程、不创建 socket、不注册任何回调；
#   - DebugServer / 事件环的既有行为不受影响。
# ON 时启用「单线程命令平面 + 受控唤醒 + 等待图」，并给 tests/debug 增加
# 诊断测试（测试仍并入 go2cpp_tests，不新增 ctest 用例数）。
option(GO2CPP_DIAGNOSTIC "Build the online diagnostic agent (Doctor)" OFF)
# QUIC stream 承载：依赖 GO2CPP_WITH_PICOQUIC=ON。默认 OFF，只保留协议与
# transport 接口；打开后走 picoquic 的最小 bidi stream 适配（见
# docs/diagnostic-channel.md 的「落地范围」）。
option(GO2CPP_DIAGNOSTIC_QUIC
       "Enable the QUIC stream transport for Doctor (requires picoquic)" OFF)
if (GO2CPP_DIAGNOSTIC)
    add_compile_definitions(GO2CPP_DIAGNOSTIC=1)
endif()
if (GO2CPP_DIAGNOSTIC_QUIC AND NOT GO2CPP_DIAGNOSTIC)
    message(FATAL_ERROR "GO2CPP_DIAGNOSTIC_QUIC=ON 需要同时设置 GO2CPP_DIAGNOSTIC=ON")
endif()
if (GO2CPP_DIAGNOSTIC_QUIC AND NOT GO2CPP_WITH_PICOQUIC)
    message(FATAL_ERROR "GO2CPP_DIAGNOSTIC_QUIC=ON 需要 GO2CPP_WITH_PICOQUIC=ON")
endif()
if (GO2CPP_DIAGNOSTIC_QUIC)
    add_compile_definitions(GO2CPP_DIAGNOSTIC_QUIC=1)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
