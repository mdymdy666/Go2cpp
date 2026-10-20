/**
 * @file runtime.hpp
 * @brief 可迁移协程的最小运行时（v3：safepoint id + 句柄 + 签名）。
 *
 * ## v3 与 v2 的差别
 *
 * - 用户函数内的挂起点用 **safepoint id** 表达（`MIGRATION_SAFEPOINT`），
 *   函数布局变化不再影响恢复；
 * - 栈上**已登记**的显式可序列化堆对象转成句柄迁移（`MIGRATION_MIGRATE_OBJECT`）；
 * - 快照默认强制 HMAC-SHA256 签名；
 * - 符号注册支持 lambda（`MIGRATION_SYMBOL_FN`），并有冲突检测与版本哈希。
 *
 * ## 不依赖固定地址
 *
 * `FiberCtl` / `ChannelCtl` 是**全局对象**，用符号名注册；Fiber 栈用普通
 * `mmap` 分配，恢复进程映射到任意地址；快照中的代码/数据地址表达成
 * `{符号名, 偏移}`，栈内指针表达成 `{相对旧栈基址的偏移}`，恢复时逐个重定位。
 *
 * ## 对编译选项的要求
 *
 * - `-fno-omit-frame-pointer -fno-optimize-sibling-calls`：让 rbp 链完整，
 *   `ChannelRecvAt` 也能用 `__builtin_frame_address(0) + 8` 精确记录调用点；
 * - `-fno-stack-protector`：挂起帧里的 canary 来自生成进程，恢复进程的
 *   `__stack_chk_guard` 不同，必须在可迁移代码上关闭栈保护；
 * - `-rdynamic`：让 `dladdr`/`dlsym` 能解析主程序符号（兜底路径）。
 */
#pragma once

#include <cstdint>
#include <string>

namespace migration {

/// Fiber 生命周期状态（数值直接写进快照，禁止改序）。
enum class FiberState : std::uint32_t {
    kReady = 0,
    kRunning = 1,
    kParked = 2,
    kCompleted = 3,
    kFailed = 4,
};

/// 默认 Fiber 栈大小。
inline constexpr std::uint64_t kDefaultStackSize = 2ULL << 20;
/// 栈在快照中向上保留的余量（恢复后若继续向下增长，需要可写空间）。
inline constexpr std::uint64_t kStackReserve = 1ULL << 20;

/// Fiber 的持久控制块。全 POD，可逐字段序列化。
struct FiberCtl {
    std::uint64_t ctx;        ///< 挂起时的 rsp（保存区首地址）
    std::uint64_t caller;     ///< 上一次 resume 的调用者上下文（不迁移）
    std::uint64_t stack_base; ///< 栈低地址
    std::uint64_t stack_size; ///< 栈映射大小
    std::uint64_t top;        ///< 栈高地址
    std::uint64_t id;         ///< 调试编号
    std::uint32_t state;      ///< FiberState
    std::uint32_t reserved0;
    std::uint64_t fn;         ///< 主体函数地址（同一二进制/同符号可重定位）
    std::uint64_t channel;    ///< 正在等待的通道（0 表示无）
    std::uint64_t waiter;     ///< 栈上的等待节点地址（0 表示无）
    std::int64_t result;      ///< 主体收到的最后一个值
    std::uint64_t arg;        ///< 预留参数
    // ---- v3 safepoint 字段 ----
    std::uint64_t safepoint_slot;  ///< 最近一次挂起点的返回地址槽
    std::uint32_t safepoint_id;    ///< 最近一次挂起点的 id（0 = 无）
    std::uint32_t parked;          ///< ParkAt 的等待标志
};

/// 通道的持久控制块。演示用无缓冲通道。
struct ChannelCtl {
    std::uint64_t head;      ///< 等待队列头（Waiter*，指向 Fiber 栈）
    std::uint64_t tail;      ///< 等待队列尾
    std::uint64_t count;     ///< 缓冲区中的元素个数
    std::uint64_t capacity;  ///< 0 表示无缓冲
    std::uint32_t closed;    ///< 是否已关闭
    std::uint32_t reserved0;
    std::int64_t buffer[8];  ///< 有缓冲模式下的环形缓冲
};

/// 等待节点：**分配在 Fiber 栈上**，随栈快照迁移。
struct Waiter {
    std::uint64_t next;  ///< 下一个等待者
    std::uint64_t owner; ///< FiberCtl 地址
    std::int64_t value;  ///< 收到的值
    std::uint32_t ready; ///< 是否已被填充
    std::uint32_t reserved0;
};

/// 全局控制块：有名字的符号，恢复进程按名字解析。
extern FiberCtl g_fiber_ctl;
extern ChannelCtl g_channel_ctl;

/// demo 主体函数签名（可来自主程序或 dlopen 的 SO）。
using DemoBodyFn = void (*)(FiberCtl*);

/// 设置日志角色前缀（"A" = dump 进程，"B" = restore 进程）。
void SetProcessRole(const char* role);

/// 可选：从 SO 加载 demo 主体。未调用时使用可执行文件内置的实现。
/// @param path SO 路径；@param error 失败原因。
bool LoadDemoLibrary(const std::string& path, std::string* error);

/// 进程 A：分配栈、启动 Fiber 跑到挂起，然后把「栈 + 符号化上下文 + 等待
/// 状态」写到 path。
bool DumpDemoToFile(const std::string& path, std::string* error);

/// 进程 B：校验并加载快照，按符号重建栈/控制块，向通道发送两条消息，
/// 恢复 Fiber 直到完成。
bool RestoreDemoFromFile(const std::string& path, int first, int second,
                         std::string* error, std::string* trace);

/// 只做格式与一致性校验，不恢复执行。
bool VerifySnapshotFile(const std::string& path, std::string* error,
                        std::string* report);

// ---- 供 demo SO 调用的导出接口 ----
/// 在通道上接收一个值（可能挂起当前 Fiber）；不记录 safepoint。
std::int64_t ChannelRecv(ChannelCtl* channel, FiberCtl* self);

/// v3：在通道上接收一个值，并记录挂起点 id 与返回地址槽。
/// **必须由用户函数直接调用**（`MIGRATION_RECV` 宏已封装）；
/// 包一层 helper 会让 `__builtin_frame_address(0) + 8` 记录到 helper 的帧。
std::int64_t ChannelRecvAt(ChannelCtl* channel, FiberCtl* self,
                           std::uint32_t safepoint_id);

/// v3：在当前位置 park，直到 `ResumeParkedFiber` 唤醒。
void ParkAt(FiberCtl* self, std::uint32_t safepoint_id);

/// v3：唤醒一个 `ParkAt` 挂起的 Fiber，并投递一个值。
bool ResumeParkedFiber(FiberCtl* self, std::int64_t value);

/// 带角色前缀的日志。
void Log(const char* format, ...);

}  // namespace migration
