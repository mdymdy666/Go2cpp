#pragma once

/**
 * @file format.hpp
 * @brief Go2Cpp 语义级 record/replay 的**稳定磁盘格式**（v1 → v2，向后兼容）。
 *
 * 设计目标（与 `docs/replay-service.md` 第 2 节一一对应）：
 *   - **可流式写**：记录逐条 append，不需要回填大小；结束写一条 `kEnd`。
 *   - **可读**：小端定长头 + 变长 payload，逐条 CRC32；任何语言都能 100 行解出。
 *   - **可诊断**：每条事件带 `seq`（全局线性化序号）、`gid`、`channel`、`ts_ns`。
 *   - **可演进**：v2 只在 v1 的 payload **尾部追加**字段，v1 解码器读 v2
 *     文件时会忽略追加部分（逐条 CRC 仍覆盖全文，因此不会误判损坏）。
 *
 * ## 文件布局
 *
 * ```
 * FileHeader (32 bytes)
 * Record*                 // 见 RecordHeader / EventType
 * Record(kEnd)            // 最后一条；payload 带总条数与整文件滚动 CRC
 * ```
 *
 * ## RecordHeader（40 bytes，小端）
 *
 * | 偏移 | 大小 | 字段 | 含义 |
 * |---|---|---|---|
 * | 0  | 4 | size        | 整条记录字节数（header + payload），>= 40 |
 * | 4  | 2 | type        | `EventType` |
 * | 6  | 2 | flags       | bit0=注入/丢失, bit1=失败, bit2=合成事件 |
 * | 8  | 8 | seq         | 全局递增序号；**线性化顺序**（持锁内分配） |
 * | 16 | 8 | ts_ns       | steady_clock 纳秒；仅同机比较 |
 * | 24 | 4 | gid         | 逻辑 fiber id（0 起） |
 * | 28 | 4 | channel     | 逻辑 channel id；`kNoChannel` 表示无 |
 * | 32 | 4 | aux         | 类型相关：send/recv/park/wake 存 `xfer_id` |
 * | 36 | 4 | crc32       | 覆盖 [0,36) + payload 的 IEEE CRC32 |
 *
 * ## v2 新增（全部是 payload 尾部的追加字段）
 *
 * - `vtime_ns`：**虚拟时钟**。录制端取 steady_clock 相对录制起点的纳秒；
 *   重放端只信这个值，不读真实时钟。
 * - `data`：`ValueKind::kBytes` 消息值的序列化字节（string/bytes/结构体）。
 * - `kSelect.cases`：每个 case 的 channel id 与类型；`chosen` 是决策结果。
 * - 新事件 `kTimer`（sleep/deadline/timeout）与 `kCtx`（cancel/deadline）。
 *
 * ## 为什么 seq 必须在锁内分配
 *
 * replay 的驱动顺序来自 `kPark/kWake` 的 seq 顺序。如果 seq 在锁外分配，
 * 两个 fiber 在临界区内完成的交接可能拿到与线性化点相反的顺序，重放就会
 * 走到记录中不存在的状态。`Recorder::NextSeq()` 的约定：调用方必须持有
 * 产生该事件的互斥锁（对 `SyncChannel` 就是 channel 的 `m_mutex` 或
 * `SelectGroup` 的组锁）。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace go2cpp::replay {

/// 文件魔数（8 字节，含结尾 NUL）。
inline constexpr char kMagic[8] = {'G', '2', 'C', 'R', 'P', 'L', 'Y', '\0'};
/// 写端版本：v4 在 v3 基础上新增资源事件（lock/unlock/pc/channel）。
/// 读端仍接受 v1~v4：旧文件缺少的字段按默认值处理。
inline constexpr std::uint16_t kFormatVersion = 4;
/// 读端接受的最小版本。v1 文件没有 v2 的追加字段，解码器按剩余字节判断。
inline constexpr std::uint16_t kMinFormatVersion = 1;
inline constexpr std::uint32_t kFileHeaderSize = 32;
inline constexpr std::uint32_t kRecordHeaderSize = 40;
inline constexpr std::uint32_t kMaxRecordSize = 1U << 20;  ///< 防御性上限 1 MiB
inline constexpr std::uint32_t kNoChannel = 0xFFFFFFFFU;

/**
 * @brief 事件类型（**稳定编号，不得复用**）。
 *
 * 语义事件（由 instrumented channel 在决定点写入）与调度器旁证事件
 * （由 `SchedulerObserver` 写入）共用一条事件流，用类型区分。
 */
enum class EventType : std::uint16_t {
    kMeta = 1,    ///< 文件元信息：gid 数、channel 数、值类型、demo 名
    kSpawn = 2,   ///< gid 对应的 fiber 开始运行
    kExit = 3,    ///< gid 结束；payload 带该 fiber 的收发计数
    kSend = 10,   ///< 一次 channel 发送的**最终结果**
    kRecv = 11,   ///< 一次 channel 接收的**最终结果**
    kSelect = 12, ///< 一次 select 的最终结果（含选中的 case 下标）
    kClose = 13,  ///< 一次 close
    kTimer = 14,  ///< 虚拟时钟事件：sleep / deadline set / timeout（v2）
    kCtx = 15,    ///< Context 事件：cancel / deadline set / deadline fired（v2）
    kPark = 20,   ///< 一次挂起（进入等待）；重放的驱动点
    kWake = 21,   ///< 一次唤醒（离开等待）；重放的驱动点
    kSched = 30,  ///< 调度器旁证事件（observer：started/suspended/completed）
    kNote = 40,   ///< 自由文本标注（注入、watchdog、诊断）
    // v4：资源事件（与 channel/select/timer 同一条 seq 流）。
    // 录制端在锁/条件变量/channel 的慢路径写；回放端只读重演，
    // 不重新竞争，因此序列与 owner 完全由记录决定。
    kResMutexLock = 50,    ///< 互斥锁加锁（含无竞争快路径）
    kResMutexUnlock = 51,  ///< 互斥锁解锁（target=被唤醒者）
    kResPCWait = 52,       ///< ParkingCondition 等待
    kResPCNotify = 53,     ///< ParkingCondition 唤醒
    kResChannelSend = 54,  ///< channel 发送（含立即完成/阻塞）
    kResChannelRecv = 55,  ///< channel 接收（含立即完成/阻塞）
    kEnd = 99,    ///< 文件结束；payload 带总条数与整文件 CRC
};

/**
 * @brief 消息值的编码方式（`kMeta.value_kind`）。
 *
 * 一个文件内可以混用：`kI64` 事件用 `Event::value`，`kBytes` 事件用
 * `Event::data`（`value` 存该 bytes 的 FNV-1a 64，便于旧工具看摘要）。
 */
enum class ValueKind : std::uint32_t {
    kI64 = 0,   ///< 整型（v1 唯一取值）
    kBytes = 1, ///< 序列化字节（string / bytes / 用户 Serialize 钩子）
    kMixed = 2, ///< 同一文件里两种都有
};

/// select 的 case 类型（`kSelect` payload 的 case 表）。
enum class SelectCaseKind : std::uint8_t {
    kRecv = 0,    ///< case v = <-ch
    kSend = 1,    ///< case ch <- v
    kDefault = 2, ///< default:
    kTimeout = 3, ///< <-time.After(...)
    kContext = 4, ///< <-ctx.Done()
};

/// `kTimer` 的 kind（稳定编号，只追加）。
enum class TimerKind : std::uint8_t {
    kSleep = 0,            ///< 一次 sleep（重放时按记录瞬间推进虚拟时钟）
    kDeadlineSet = 1,      ///< 设置 deadline
    kDeadlineExceeded = 2, ///< deadline 到期
    kTimeoutSelected = 3,  ///< select 的 timeout case 被选中
    // v3：TimerService 回放（一次定时器的完整生命周期）
    kServiceArm = 4,        ///< 注册（Schedule/ScheduleEvery）
    kServiceFire = 5,       ///< 回调执行（fired_at_ns / callback_result）
    kServiceCancel = 6,     ///< 取消（取消后不再触发）
    kServiceReschedule = 7, ///< 周期重排（下一次 deadline）
};

/// `kCtx` 的 kind。
enum class ContextEventKind : std::uint8_t {
    kCancelled = 0,        ///< 显式 Cancel
    kDeadlineSet = 1,      ///< 设置 deadline
    kDeadlineExceeded = 2, ///< deadline 到期
};

/// 事件类型的稳定文本名（CLI/日志用；未知值返回 "unknown"）。
const char* EventTypeName(EventType type) noexcept;
/// 消息值编码方式的名字。
const char* ValueKindName(ValueKind kind) noexcept;
/// select case 类型的名字。
const char* SelectCaseKindName(SelectCaseKind kind) noexcept;
/// timer 事件类型的名字。
const char* TimerKindName(TimerKind kind) noexcept;
/// context 事件类型的名字。
const char* ContextEventKindName(ContextEventKind kind) noexcept;

/**
 * @brief channel 操作的最终结果。
 *
 * 只记录**最终**结果；"曾经阻塞"由 `blocked` 标志与 `kPark` 事件表达。
 */
enum class Decision : std::uint8_t {
    kOk = 0,           ///< 成功（含一次交接）
    kClosed = 1,       ///< 读到关闭 / 发往已关闭
    kAlreadyClosed = 2,///< 重复 close
    kWouldBlock = 3,   ///< 非阻塞路径未就绪
    kCancelled = 4,    ///< 被 Context 取消（v2 启用）
    kTimedOut = 5,     ///< 超时（v2 启用）
    kStuck = 6,        ///< 记录端 watchdog 判定卡死时补写的"未完成"
};

/// 挂起原因（`kPark/kWake` 的 payload）。
enum class ParkReason : std::uint8_t {
    kOther = 0,
    kSend = 1,
    kRecv = 2,
    kSelect = 3,
    kClose = 4,
    kTimeout = 5, ///< 超时唤醒（v2）
    kContext = 6, ///< Context 取消唤醒（v2）
};

/// 记录标志位。
inline constexpr std::uint16_t kFlagInjected = 1U << 0;  ///< 该事件是注入/丢失的
inline constexpr std::uint16_t kFlagFailed = 1U << 1;    ///< 操作失败
inline constexpr std::uint16_t kFlagSynthetic = 1U << 2; ///< 收尾时补写

/// `kMeta.ext_flags` 的位。
inline constexpr std::uint32_t kMetaHasValueCodec = 1U << 0;  ///< 支持 bytes 值
inline constexpr std::uint32_t kMetaVirtualClock = 1U << 1;   ///< 使用虚拟时钟

/// 时钟模式（`kMeta.clock_mode`）。
inline constexpr std::uint32_t kClockReal = 0;
inline constexpr std::uint32_t kClockVirtual = 1;

/// 一个 select case 的记录（仅诊断与重放校验，不参与决策）。
struct SelectCaseRecord {
    std::uint32_t channel{kNoChannel};
    SelectCaseKind kind{SelectCaseKind::kRecv};
};

/**
 * @brief 解析后的内存事件（**不是**磁盘布局，不要 memcpy 到磁盘）。
 *
 * 所有字段对每种类型都存在，未使用的保持默认值；这样 CLI 与重放引擎不必
 * 为每种类型维护 union 的析构/拷贝逻辑。
 */
struct Event {
    EventType type{EventType::kNote};
    std::uint16_t flags{0};
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    std::uint64_t vtime_ns{0};  ///< 虚拟时钟（v2；v1 文件为 0）
    std::uint32_t gid{0};
    std::uint32_t channel{kNoChannel};
    std::uint32_t xfer_id{0};   ///< 一次交接的唯一 id；send/recv 两侧相同
    std::uint32_t op_index{0};  ///< 该 gid 的第几次 channel 操作（0 起）
    std::int64_t value{0};      ///< 整型消息值；bytes 值为其 FNV-1a 摘要
    std::string data;           ///< bytes 消息值（v2；i64 时为空）
    Decision decision{Decision::kOk};
    std::uint8_t blocked{0};    ///< 0=立即完成, 1=经 park 后完成, 2=未完成
    ParkReason reason{ParkReason::kOther};
    std::uint16_t chosen{0};    ///< select 选中的 case 下标
    std::uint32_t sched_type{0};///< kSched 的原始枚举 / kEnd 的 file_crc
    std::uint32_t machine{0};
    std::uint32_t processor{0};
    std::uint64_t aux64{0};
    std::uint8_t timer_kind{0}; ///< TimerKind
    std::uint8_t ctx_kind{0};   ///< ContextEventKind
    std::uint32_t ctx_id{0};    ///< kCtx 的 context id
    std::uint64_t timer_ns{0};  ///< kTimer 的时长/绝对 deadline（虚拟纳秒）
    std::uint32_t ext_flags{0}; ///< kMeta 的扩展位
    std::uint32_t clock_mode{0};///< kMeta 的时钟模式
    std::vector<SelectCaseRecord> cases;  ///< kSelect 的 case 表
    std::string note;
    // ---- v3：kTimer（TimerService）的追加字段 ----
    std::uint32_t timer_id{0};       ///< 定时器标识（录制端分配，1 起）
    std::uint32_t timer_source{0};   ///< TimerSource（0=unknown/1=context/2=pc/3=channel/4=timed_mutex/5=scheduler/6=sleep/7=user）
    std::uint64_t deadline_ns{0};    ///< 绝对的虚拟截止时间（相对录制起点）
    std::uint64_t fired_at_ns{0};    ///< 回调开始执行时的虚拟时间
    std::int64_t callback_result{0}; ///< 回调返回值（重放时校验一致）
    // ---- v4：资源事件字段（其它类型保持 0）----
    std::uint64_t resource_id{0};    ///< 资源标识（录制端分配，0=未知）
    std::uint64_t actor_gid{0};      ///< 执行操作的逻辑 fiber（与 gid 一致）
    std::uint64_t target_gid{0};     ///< 被唤醒/交接的 gid（0=无）
    std::uint32_t resource_result{0};///< ResourceResult 数值（0=ok）
    std::uint64_t address{0};        ///< 资源对象地址（诊断定位用）
    std::uint32_t waiters{0};        ///< 操作后的等待者数（近似）
};

/**
 * @brief 文件头（32 字节 = magic 8 + version 2 + header_size 2 + flags 4 +
 *        created_unix_ns 8 + producer_id 8）。
 *
 * `producer_id` 不参与语义，只用于把同一台机器上多次录制区分开。
 */
struct FileHeader {
    std::uint16_t version{kFormatVersion};
    std::uint16_t header_size{static_cast<std::uint16_t>(kFileHeaderSize)};
    std::uint32_t flags{1};  ///< bit0: 逐条 CRC32 已启用
    std::uint64_t created_unix_ns{0};
    std::uint64_t producer_id{0};
};

/// IEEE CRC32（多项式 0xEDB88320，初值/异或 0xFFFFFFFF）。
std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t seed = 0) noexcept;

/// FNV-1a 64（bytes 值的摘要，用于旧工具/JSON 展示）。
std::uint64_t Fnv1a64(const void* data, std::size_t size,
                      std::uint64_t basis = 1469598103934665603ULL) noexcept;

/**
 * @brief 小端字节写入器（追加到 `std::vector<std::uint8_t>`）。
 *
 * 所有多字节字段显式按小端编码，因此格式与宿主端序无关。
 */
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : m_out(out) {}

    void U8(std::uint8_t value) { m_out.push_back(value); }
    void U16(std::uint16_t value) {
        m_out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        m_out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }
    void U32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            m_out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void U64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            m_out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
    void Str(const std::string& value) {
        U32(static_cast<std::uint32_t>(value.size()));
        m_out.insert(m_out.end(), value.begin(), value.end());
    }

private:
    std::vector<std::uint8_t>& m_out;
};

/**
 * @brief 小端字节读取器；越界后 `ok()` 变为 false，后续读取返回 0。
 *
 * 刻意不做异常：格式损坏是**预期输入**（截断文件、错文件），调用方应据此
 * 返回结构化错误而不是崩溃。
 */
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    bool ok() const noexcept { return m_ok; }
    std::size_t remaining() const noexcept { return m_size - m_pos; }

    std::uint8_t U8() {
        if (!Require(1)) return 0;
        return m_data[m_pos++];
    }
    std::uint16_t U16() {
        if (!Require(2)) return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(
            m_data[m_pos] | (static_cast<std::uint16_t>(m_data[m_pos + 1]) << 8U));
        m_pos += 2;
        return v;
    }
    std::uint32_t U32() {
        if (!Require(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(m_data[m_pos + static_cast<std::size_t>(i)])
                 << (8U * static_cast<unsigned>(i));
        }
        m_pos += 4;
        return v;
    }
    std::uint64_t U64() {
        if (!Require(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(m_data[m_pos + static_cast<std::size_t>(i)])
                 << (8U * static_cast<unsigned>(i));
        }
        m_pos += 8;
        return v;
    }
    std::int64_t I64() { return static_cast<std::int64_t>(U64()); }
    std::string Str() {
        const std::uint32_t size = U32();
        if (!m_ok || size > remaining()) {
            m_ok = false;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(m_data + m_pos), size);
        m_pos += size;
        return out;
    }
    void Skip(std::size_t size) { (void)Require(size); }

private:
    bool Require(std::size_t size) noexcept {
        if (!m_ok || size > m_size - m_pos) {
            m_ok = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos{0};
    bool m_ok{true};
};

/// 把一条事件编码为完整记录（header + payload + CRC），追加到 `out`。
void EncodeEvent(const Event& event, std::vector<std::uint8_t>& out);

/**
 * @brief 从内存缓冲解出一条记录。
 * @param data 缓冲区起点
 * @param size 缓冲区可用字节数
 * @param event 输出事件
 * @param consumed 输出本条记录占用的字节数
 * @param error 失败原因（成功时清空）
 * @return 成功解析完整一条记录返回 true
 */
bool DecodeEvent(const std::uint8_t* data, std::size_t size, Event* event,
                 std::size_t* consumed, std::string* error);

/// 文件头的编码/解码（解码接受 v1 与 v2）。
void EncodeFileHeader(const FileHeader& header, std::vector<std::uint8_t>& out);
bool DecodeFileHeader(const std::uint8_t* data, std::size_t size, FileHeader* header,
                      std::string* error);

}  // namespace go2cpp::replay
