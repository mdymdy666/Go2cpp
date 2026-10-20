#pragma once

/**
 * @file debug_dump.hpp
 * @brief 统一调试 dump 的数据契约与格式化工具（只读，格式稳定）。
 *
 * ## 定位
 *
 * 本文件是「可迁移协程 / 重放服务」的**输出层契约**：所有组件的内省接口都
 * 返回纯数据快照，再由这里统一序列化为 **text（key=value）** 与 **JSON**
 * 两种格式。CLI / DebugServer / 重放服务只依赖这两种格式，不依赖组件内部
 * 类型布局。
 *
 * ## 格式（稳定，工具按此解析）
 *
 * text：
 * ```text
 * schema=go2cpp.dump.<component>/1
 * version=1
 * <key>=<value>            # 每行一个字段，值到行尾，转义 \n \r \\ \=
 * processor.0.id=0         # 数组用 <name>.<index>.<field>
 * processor.0.state=kRunning
 * ```
 *
 * json：一个 JSON 对象，字段名与 text 的 key 一致（嵌套结构按 `processor`
 * 数组表达），顶层必有 `"schema"` 与 `"version"`。
 *
 * ## 只读保证
 *
 * 本文件所有 `Dump*` 函数只读取调用时已经采集好的快照，或调用组件中
 * **明确定义为只读**的快照函数；不修改运行时状态。
 *
 * ## 边界
 *
 * - 快照由多次独立读取拼成，不是同一瞬间的全局原子视图（各组件输出会
 *   用 `consistent=per_call` 或等价字段如实标注）；
 * - 事件环只在 `GO2CPP_DEBUG_DUMP=ON` 构建中记录；OFF 构建返回
 *   `enabled=false`，不会假装有数据。
 */

#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_scheduler.hpp"

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/core/timer.hpp"
#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::debug {

/// 组件命名的稳定常量（CLI / 服务端共用；不要改名；改动视为破坏协议）。
inline constexpr const char* kComponentScheduler = "scheduler";
inline constexpr const char* kComponentFiber = "fiber";
inline constexpr const char* kComponentChannel = "channel";
inline constexpr const char* kComponentParkingCondition = "parking_condition";
inline constexpr const char* kComponentMutex = "mutex";
inline constexpr const char* kComponentIO = "io";
inline constexpr const char* kComponentEvents = "events";
inline constexpr const char* kComponentRuntime = "runtime";
inline constexpr const char* kComponentTimer = "timer";

/// 把文本转义为单行值：反斜杠、换行、回车、`=` 与首尾空白可见。
std::string DumpEscape(std::string_view value);

/// JSON 字符串转义（不含首尾引号）。
std::string JsonEscape(std::string_view value);

/// 一次 dump 的双格式结果。text 不含 `[component ...]` 头（由聚合器添加）。
struct DumpPayload {
    std::string text;
    std::string json;
    bool available{true};   ///< false 表示组件当前不可用（如未注册/已销毁）。
    std::string reason;     ///< available=false 时的稳定原因码。

    static DumpPayload Unavailable(std::string reason_code);
};

/**
 * @brief text dump 构建器：保证 `key=value` 每行一个字段、键序由调用方固定。
 *
 * @note 本类**不排序**字段（排序会让新增字段的位置随值变化，不利于
 *       CLI 做稳定 diff）；调用方按 schema 文档顺序添加。
 */
class TextDump final {
public:
    TextDump() = default;

    /// 写 `schema=` 与 `version=` 两个固定首行。
    TextDump& Head(std::string_view schema, std::uint32_t version = 1) {
        Key("schema", schema);
        Key("version", static_cast<std::uint64_t>(version));
        return *this;
    }

    TextDump& Key(std::string_view key, std::string_view value) {
        m_out.append(key.data(), key.size());
        m_out.push_back('=');
        m_out += DumpEscape(value);
        m_out.push_back('\n');
        return *this;
    }
    TextDump& Key(std::string_view key, const std::string& value) {
        return Key(key, std::string_view(value));
    }
    TextDump& Key(std::string_view key, const char* value) {
        return Key(key, std::string_view(value ? value : ""));
    }
    TextDump& Key(std::string_view key, bool value) {
        return Key(key, value ? std::string_view("true")
                              : std::string_view("false"));
    }
    TextDump& Key(std::string_view key, std::uint64_t value) {
        return Key(key, std::string(std::to_string(value)));
    }
    TextDump& Key(std::string_view key, std::uint32_t value) {
        return Key(key, static_cast<std::uint64_t>(value));
    }
    TextDump& Key(std::string_view key, std::int64_t value) {
        return Key(key, std::to_string(value));
    }
    TextDump& Key(std::string_view key, int value) {
        return Key(key, static_cast<std::int64_t>(value));
    }
    /// 十六进制输出（指针/位掩码用），固定 `0x` 前缀、小写、无前导零。
    TextDump& KeyHex(std::string_view key, std::uint64_t value);
    /// 原文追加（只用于已经格式化好的多行片段，如事件列表）。
    TextDump& Raw(std::string_view text) {
        m_out.append(text.data(), text.size());
        return *this;
    }

    const std::string& str() const noexcept { return m_out; }
    std::string Take() { return std::move(m_out); }

private:
    std::string m_out;
};

/**
 * @brief 一次 dump 的一致性探针：基于事件环的**全局发布序号**。
 *
 * 语义：
 * - `supported == false`（默认 OFF 构建）：事件环不记录，序号恒为 0，
 *   消费方**不能**据此判断是否有并发变化；
 * - `supported == true` 且 `seq_start == seq_end`：采集期间没有任何事件发布，
 *   dump 内容在「事件可见性」意义上是一致的；
 * - `seq_start != seq_end`：采集期间有并发事件，dump 是多时刻拼成的快照
 *   （仍可能可用，但消费方不应把它当作同一瞬间的全局视图）。
 *
 * 该标记是**提示**而不是事务：事件环只覆盖被插桩的调度路径，未插桩的状态
 * 变化（例如纯用户态写 Channel 缓冲）不会递增 seq。
 */
struct ConsistencyStamp {
    std::uint64_t seq_start{0}; ///< 采集开始前的事件环发布序号
    std::uint64_t seq_end{0};   ///< 采集结束后的事件环发布序号
    bool supported{false};      ///< 本构建是否记录事件（决定前两项是否可信）

    bool Changed() const noexcept { return seq_start != seq_end; }
};

/// 开始一次一致性探测（读取 `EventRing::PublishSeq()`，O(1)）。
ConsistencyStamp BeginConsistency() noexcept;
/// 结束探测（再次读取发布序号）；可与 BeginConsistency 成对使用。
void EndConsistency(ConsistencyStamp& stamp) noexcept;
/// 把一致性字段追加到 text dump：`seq_at_start/seq_at_end/changed_during_dump/event_seq_supported`。
void AppendConsistency(TextDump& text, const ConsistencyStamp& stamp);
/// 把一致性字段追加到 JSON 片段（返回值以 `,` 开头，可直接拼接）。
std::string JsonConsistency(const ConsistencyStamp& stamp);

/// GMP 状态名（稳定字符串；未知值输出 `kUnknown`，不猜测）。
const char* GStateNameStable(GState state) noexcept;
const char* MStateNameStable(MState state) noexcept;
const char* PStateNameStable(PState state) noexcept;
const char* FiberStateNameStable(FiberState state) noexcept;
const char* SuspendReasonNameStable(SuspendReason reason) noexcept;
/// ParkingCondition 节点状态：0=kWaiting 1=kNotified 2=kTimedOut 3=kCancelled。
const char* WaitStateNameStable(int state) noexcept;

// ---------------------------------------------------------------------------
// 各组件 dump（只读）
// ---------------------------------------------------------------------------

/// scheduler：M/P/G 状态、每 P runqueue 长度、全局 runqueue、metrics。
DumpPayload DumpScheduler(Scheduler& scheduler,
                          const DumpOptions& options = {});

/// fiber：状态/父 Fiber/嵌套深度/栈范围与 committed。
DumpPayload DumpFiber(const Fiber& fiber);

/// channel：缓冲长度/容量、发送/接收等待者、select 注册数。
DumpPayload DumpChannel(const ChannelDebugSnapshot& snapshot);

/// ParkingCondition：等待者数量与每个节点状态。
DumpPayload DumpParkingCondition(const core::ParkingCondition& condition);

/// sync::Mutex：owner（近似）、等待者、状态字四位。
DumpPayload DumpMutex(const sync::Mutex& mutex);

/// sync::ConditionVariable：等待者数量。
DumpPayload DumpConditionVariable(const sync::ConditionVariable& condition);

/// io::IOManager：FdSlot 数量、等待者、epoll 注册数、事件环 wake 统计。
DumpPayload DumpIOManager(io::IOManager& manager);

/// timer：活动数、堆顶 deadline、按来源分类、fire/cancel/reschedule 统计、
/// 后台线程状态；带 seq_at_start/seq_at_end 一致性标记。
DumpPayload DumpTimer(const core::TimerService& service);

/// 事件环（scheduler park/wake/runqueue 序列）。
DumpPayload DumpEvents(std::size_t max = 0);

/// 运行时信息：pid、构建开关、线程数、事件环线程数。
DumpPayload DumpRuntimeInfo();

/// 把 payload 写成文件（原子替换：先写 `.tmp` 再 rename）。
bool WriteDumpFile(const std::string& path, const DumpPayload& payload,
                   bool json, std::string* error);

}  // namespace go2cpp::debug
