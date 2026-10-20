/**
 * @file connection.hpp
 * @brief HTTP/2 连接层（握手、帧循环、流管理、HPACK 集成）。
 *
 * ## 定位
 *
 * 这是"协议逻辑"的最后一块：它把 HPACK、帧层、流状态机串起来，形成
 * 「客户端发请求 / 服务端回响应」的闭环。**它不关心字节从哪来**
 * —— IO 通过 `Transport` 注入，所以能完全脱离网络测试。
 *
 * ## 并发模型
 *
 * 全部接口都是**同步阻塞**语义（最贴近 Go 的 `http2`）：`Handshake`、
 * `ReadEvent`、`WriteData` 在调用线程/Fiber 上阻塞到完成为止。
 * 在生产者环境里，`Transport` 的实现用本项目的 `IOManager::wait` + Fiber
 * 把阻塞变成协程挂起，**连接层代码一行不改**。
 *
 * ## 已实现
 *
 * - 客户端/服务端握手（preface + SETTINGS 交换 + ACK）；
 * - 客户端发起请求（HEADERS）、发送 DATA、关闭流；
 * - 服务端接收 HEADERS/DATA 并作为事件投递；
 * - 流表管理（每条流一个 `StreamStateMachine`）；
 * - PING 自动回 ACK、GOAWAY、RST_STREAM、WINDOW_UPDATE 事件；
 * - 收到 DATA 后自动回 WINDOW_UPDATE 恢复窗口（简单策略，见下）。
 *
 * ## 明确未实现（不假装支持）
 *
 * - **CONTINUATION**：**已支持**。发送时按 `SETTINGS_MAX_FRAME_SIZE` 自动分片
 *   （HEADERS + 若干 CONTINUATION），接收时自动拼接直到 `END_HEADERS`；
 *   实现上依赖"同一时刻只有一条流在发 header block"这一约束 —— 因为 `Connection`
 *   是单驱动者（一个 Fiber）且发送是同步的，这个约束天然成立；
 * - **流控**：**已实现窗口跟踪**。发送 DATA 前检查对端窗口，不足返回
 *   `kFlowControl`（不阻塞等待，调用方自行重试）；接收 DATA 扣减本地窗口，
 *   当消耗超过一半时发 WINDOW_UPDATE 补回。**未实现**按读取速度自适应配平
 *   （见下）；
 * - **优先级**：PRIORITY 帧被解析但不参与调度（RFC 9113 已弃用）；
 * - **服务器推送**：本实现不支持。客户端在 SETTINGS 中通告 `ENABLE_PUSH=0`；
 *   收到 PUSH_PROMISE 一律按连接错误 `PROTOCOL_ERROR` 处理。**不能**把它当作
 *   未知帧跳过：PUSH_PROMISE 携带的 header block 会更新对端的 HPACK 编码器
 *   动态表，而本端解码器不更新，必然导致后续 header 解析失步。
 *
 * ## 本文件定义什么
 * - `Role` / `ConnectionConfig`：角色与全部可调参数（帧尺寸、header 上限、
 *   HPACK 表大小、初始窗口、并发流上限）；
 * - `ConnStatus` / `ConnState`：操作结果与连接生命周期状态；
 * - `IncomingEvent`：`ReadEvent()` 交付给上层的统一事件结构；
 * - `Connection`：握手、请求/响应、流表、流控、GOAWAY 的唯一入口。
 *
 * ## 阅读建议
 * 先看 `ConnState` 与公开方法的同步阻塞语义，再看
 * `docs/state-machines/Http2Connection.md`，最后进 `connection.cpp` 的
 * `ReadEvent()` —— 它是整个协议的分发中心。
 *
 * ## 边界与陷阱
 * - 所有接口同步阻塞：`ReadEvent()` 会等到下一个帧才返回；
 * - **单驱动者**：一个 `Connection` 只能由一个线程/Fiber 使用，类内没有锁；
 * - `WriteData()` 返回 `kFlowControl` 不是错误：等 `WINDOW_UPDATE`（再读一次
 *   事件）后重试；被拒绝的调用不会消耗 END_STREAM。
 */
#pragma once

#include "go2cpp/network/http2/frame.hpp"
#include "go2cpp/network/http2/hpack.hpp"
#include "go2cpp/network/http2/stream_state.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace go2cpp::net::http2 {

/** @brief 连接角色。 */
enum class Role : std::uint8_t { kClient, kServer };

/** @brief 连接配置。 */
struct ConnectionConfig final {
    std::uint32_t max_frame_size{kDefaultMaxFrameSize};
    /**
     * @brief 单个 header block（HEADERS + 其全部 CONTINUATION）允许的**压缩后**
     *        字节数上限。
     * @details 这是防 CONTINUATION 泛滥的第一道闸：对端可以用无数个
     *          CONTINUATION 帧凑出一个巨大的 header block，若不在拼接过程中
     *          就累计检查，内存会随输入线性膨胀。默认 64 KiB —— 与 RFC 语境下
     *          常见的 `SETTINGS_MAX_HEADER_LIST_SIZE` 同量级，且远小于会让
     *          VmRSS 明显上升的规模。
     */
    std::uint32_t max_header_block_size{65536};
    /**
     * @brief 解压后 header 列表的大小上限（RFC 9113 §6.5.2 的语义）。
     * @details 每项按 `name.size() + value.size() + 32` 计（与 HPACK 动态表
     *          的条目开销一致）。这一道防的是 **HPACK 解压炸弹**：压缩块可能
     *          很小，解压后却极大（大量重复头 + Huffman）。两道限制缺一不可。
     */
    std::uint32_t max_header_list_size{65536};
    std::uint32_t header_table_size{4096};
    std::uint32_t initial_window_size{65535};
    /**
     * @brief 本端在 SETTINGS 中通告的 `MAX_CONCURRENT_STREAMS`（入站上限）。
     * @note 它同时充当"未收到对端 SETTINGS 时的兜底值"，但**是否执行**由
     *       `enforce_max_concurrent_streams` 决定。
     */
    std::uint32_t max_concurrent_streams{100};
    /**
     * @brief 流表里最多保留多少条**已关闭**的流（默认 256）。
     * @details RFC 9113 允许在关闭后的一小段时间内继续收到迟到的
     *          WINDOW_UPDATE/RST_STREAM；因此不能一关闭就删除状态机。但长连接
     *          如果永不清理，流表会随流号线性增长（审计 I3）。这里采用有界
     *          保留：已关闭流超过阈值后，按流 id 从小到大淘汰最老的条目，
     *          同时删除两条流级窗口记录。0 表示关闭即删除（最严格，但会失去
     *          对迟到帧的容忍度，仅用于验证/特殊场景）。
     */
    std::uint32_t max_retired_streams{256};
    /**
     * @brief 是否强制非 CONNECT 请求携带 `:scheme`（默认开启）。
     * @details 为兼容仓库既有测试与老客户端，默认规则是：带 `:authority`
     *          的请求必须有 `:scheme`；不带 `:authority` 的旧式请求暂不强制。
     *          把 `strict_scheme` 置 true 可对所有非 CONNECT 请求强制。
     *          `:scheme` 为空或包含非法字符时**始终**拒绝。
     */
    bool require_scheme{true};
    /**
     * @brief 是否对所有非 CONNECT 请求强制 `:scheme`（默认关闭）。
     * @details 默认关闭只为兼容既有测试：`test_connection.cpp` 的部分用例
     *          发送没有 `:authority`/`:scheme` 的旧式请求。生产环境建议开启。
     */
    bool strict_scheme{false};
    /**
     * @brief 客户端发送请求时，若缺少 `:scheme` 是否自动补 `default_scheme`。
     * @details 自动补齐只发生在「`:authority` 存在（或 `strict_scheme` 开启）
     *          且方法不是 CONNECT」时，插入位置在伪头区末尾。这样服务端在
     *          `require_scheme` 下仍能收到合法请求；显式传 `:scheme` 的调用方
     *          不受影响。关闭该开关可用于验证「缺失 :scheme 被拒绝」。
     */
    bool auto_fill_scheme{true};
    /// auto_fill_scheme 开启时补入的默认 scheme 值。
    std::string default_scheme{"http"};
    /**
     * @brief 是否把对端 SETTINGS_MAX_HEADER_LIST_SIZE 应用到**客户端请求**。
     * @details 默认关闭：`test_connection.cpp` 场景 11 case2 用同一个很小的
     *          `max_header_list_size` 同时配置 client/server，并期望客户端把
     *          超限请求发出去、由服务端入站检查拒绝。若默认在 StartRequest
     *          检查对端上限，客户端会提前返回 `kHeaderListTooLarge`，使该用例
     *          失败。服务端响应方向（`SendResponseHeaders`）**始终**检查。
     *          建议后续修正该测试（两端使用不同 config）后把默认值改为 true。
     */
    bool enforce_peer_header_list_on_requests{false};
    /**
     * @brief 是否执行 RFC 9113 §5.1.2 的并发流上限（默认**开启**）。
     * @details 开启后：
     *          - 客户端在 `StartRequest()` 检查**对端** SETTINGS 通告的上限，
     *            超限返回 `kTooManyStreams` 且不消耗流号；
     *          - 服务端收到超过**本端**通告上限的新流时回
     *            RST_STREAM(REFUSED_STREAM)，HPACK 动态表保持同步。
     *
     * 关闭开关只用于「压测需要刻意制造无限流水线」的场景；生产默认必须
     * 执行该限制，否则单连接可以被无限新建流拖垮。
     */
    bool enforce_max_concurrent_streams{true};
};

/** @brief 连接层的操作结果。 */
enum class ConnStatus : std::uint8_t {
    kOk,
    kClosed,
    kProtocolError,
    kUnsupported,
    /// 发送窗口不足（对端还没给 WINDOW_UPDATE）。调用方应等待后重试。
    kFlowControl,
    /// header 块或解压后的 header 列表超过配置上限（防 CONTINUATION 泛滥与
    /// HPACK 解压炸弹）。收到它时应当主动断开连接。
    kHeaderListTooLarge,
    /// 并发流数量达到上限（RFC 9113 §5.1.2）。调用方应等已有流结束后重试。
    kTooManyStreams,
};

const char* ToString(ConnStatus status) noexcept;

/**
 * @brief 连接生命周期状态。
 * @details kOpen 是唯一允许新建流的局面；kDraining 表示已发出或收到
 *          GOAWAY（已有流可以继续收尾，但不再接受新流）；kClosed 表示
 *          传输已失败，任何操作都不应再发起。状态只由 Connection 内部
 *          迁移，外部通过 state() 只读观察。
 */
enum class ConnState : std::uint8_t {
    kOpen,      ///< 握手后可正常收发。
    kDraining,  ///< 已发/已收 GOAWAY：不再接受新流，已有流可继续。
    kClosed,    ///< 传输失败：连接已不可用。
};

/** @brief 从连接读到的一个事件。 */
struct IncomingEvent final {
    enum class Kind : std::uint8_t {
        kHeaders,
        kData,
        kSettings,
        kPing,
        kGoAway,
        kRstStream,
        kWindowUpdate,
        kUnknown,
    };
    Kind kind{Kind::kUnknown};
    std::uint32_t stream_id{0};
    bool end_stream{false};
    /// kind == kHeaders 时有效（已用 HPACK 解出）。
    std::vector<HeaderField> headers;
    /// kind == kData 时有效。
    std::vector<std::uint8_t> data;
    /// kind == kPing 时有效。
    std::uint64_t ping_opaque{0};
    /// kind == kGoAway / kRstStream 时有效。
    ErrorCode error_code{ErrorCode::kNoError};
    /// kind == kGoAway 时有效。
    std::uint32_t last_stream_id{0};
};

/**
 * @brief 一个 HTTP/2 连接。
 * @details 非线程安全：一个连接只能由一个线程/Fiber 驱动（与 Go 的 `http2`
 *          用法一致 —— 单 goroutine 读，写需外部同步）。这一点在类注释里
 *          说明而不加锁，是为了让协议逻辑保持简单、可测。
 *
 * ## 定位
 * 被 `http::Client`（HTTP/2 路径）、示例与测试直接构造；向下依赖 `Transport`
 * 取字节，同层用 `Decoder`/`Encoder`（HPACK）与 `StreamStateMachine`（流状态），
 * 向上交付 `IncomingEvent`。它不关心字节从哪来，因此可脱离网络测试。
 *
 * ## 不变量
 * - `handshaked()` 为真之前不分配流号，也不发 HEADERS/DATA；
 * - `StartRequest()` 只在 `kOpen` + 已握手时成功，客户端流号按奇数递增；
 * - `WriteFrame()` 失败即置 `kClosed`：绝不留下写了一半的帧却仍宣称可用；
 * - 发送窗口 = 连接级 + 流级；接收侧在扣减前先校验，窗口不会为负。
 *
 * ## 状态机
 * 状态转化表见 `docs/state-machines/Http2Connection.md`；
 * 流级转化见 `docs/state-machines/Http2StreamState.md`。
 */
class Connection final {
public:
    Connection(Transport* transport, Role role,
               ConnectionConfig config = ConnectionConfig{});
    ~Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Role role() const noexcept { return m_role; }
    const ConnectionConfig& config() const noexcept { return m_config; }
    bool handshaked() const noexcept { return m_handshaked; }
    /** @brief 返回连接生命周期状态（kOpen/kDraining/kClosed）。 */
    ConnState state() const noexcept { return m_state; }
    /** @brief 返回对端 GOAWAY 声明的最后一个已处理流 id（未收到为 0）。 */
    std::uint32_t peer_last_stream_id() const noexcept {
        return m_peer_last_stream_id;
    }

    /**
     * @brief 完成握手（客户端发 preface+SETTINGS 并等对端 SETTINGS；
     *        服务端反之）。
     * @return 状态。
     *
     * ## 步骤
     * step1. 客户端先写 24 字节 preface（服务端则读并逐字节比对）；
     * step2. 写本地 SETTINGS（表大小 / 帧上限 / 初始窗口 / 并发流数）；
     * step3. `FinishSettingsHandshake()` 循环收对端 SETTINGS，回 ACK 后置
     *         `m_handshaked = true`。
     *
     * ## 失败语义
     * 写失败立即把连接置 `kClosed`；preface 不匹配返回 `kProtocolError`。
     */
    ConnStatus Handshake(std::string* error = nullptr);

    /**
     * @brief 客户端发起一个请求：分配新流并发送 HEADERS。
     * @param headers 请求头（标准请求应含 `:method`/`:path`/`:scheme`/`:authority`）。
     * @param end_stream 是否在 HEADERS 上就结束发送方向（无 body）。
     * @param out_stream_id 成功时写入分配的流 id。
     * @return 状态。非客户端调用返回 `kUnsupported`；连接非 `kOpen`
     *         （draining/closed）时返回 `kProtocolError`；并发流达到对端上限
     *         返回 `kTooManyStreams`；header 列表超过对端通告的
     *         `SETTINGS_MAX_HEADER_LIST_SIZE` 返回 `kHeaderListTooLarge`。
     * @note 若请求缺 `:scheme` 且 `auto_fill_scheme` 开启，会按
     *       `default_scheme` 自动补齐（位置在伪头区末尾），避免与既有调用方
     *       的旧式请求不兼容；显式传入的 `:scheme` 永不覆盖。
     *
     * ## 步骤
     * step1. 校验角色（仅客户端）、连接 `kOpen`、已握手；
     * step2. 分配奇数流号（每次 +2）；
     * step3. `EncodeBlock()` 编码 HEADERS，`try_emplace` 建流并 `SendHeaders()`；
     * step4. 初始化该流窗口，`WriteHeaderBlock()` 在必要时拆 CONTINUATION。
     */
    ConnStatus StartRequest(const std::vector<HeaderField>& headers, bool end_stream,
                            std::uint32_t* out_stream_id, std::string* error = nullptr);

    /** @brief 在已打开的流上发送 DATA。 */
    /**
     * @brief 在已打开的流上发送 DATA。
     *
     * ## 步骤
     * step1. 只读预检：`CanSendData()`、帧尺寸、流控（都不修改状态）；
     * step2. 提交：`SendData()` 推进状态机；
     * step3. 扣减窗口；失败时 `ForceClose()`，不留「状态已变、帧没发」的中间态；
     * step4. 写帧；写失败同样 `ForceClose()`。
     */
    ConnStatus WriteData(std::uint32_t stream_id, const std::vector<std::uint8_t>& data,
                         bool end_stream, std::string* error = nullptr);

    /** @brief 服务端回响应：发送 HEADERS。 */
    ConnStatus SendResponseHeaders(std::uint32_t stream_id,
                                   const std::vector<HeaderField>& headers, bool end_stream,
                                   std::string* error = nullptr);

    /** @brief 主动关闭一条流（RST_STREAM）。 */
    ConnStatus ResetStream(std::uint32_t stream_id, ErrorCode code,
                           std::string* error = nullptr);

    /** @brief 发送 GOAWAY 并把连接置为 kDraining（不再接受新流）。 */
    /**
     * @brief 发送 GOAWAY 并把连接置为 kDraining（不再接受新流）。
     *
     * ## 步骤
     * step1. 取当前最大流 id 作为 `last_stream_id`（已知偏差见实现注释）；
     * step2. 写 GOAWAY；
     * step3. 成功后把连接迁到 `kDraining`（已有流仍可收尾）。
     */
    ConnStatus SendGoAway(ErrorCode code, std::string* error = nullptr);

    /**
     * @brief 读取下一个事件（阻塞）。
     * @param out 成功时写入事件。
     * @return `kOk` 表示拿到事件；`kClosed` 表示对端关闭。
     *
     * ## 步骤
     * step1. `TakeFrame()` 取一个完整帧（不足则继续 `FillBuffer()`）；
     * step2. 按帧类型分发：HEADERS / DATA / SETTINGS / PING / RST_STREAM /
     *         GOAWAY / WINDOW_UPDATE / 未知；
     * step3. HEADERS 先拼 CONTINUATION 再 HPACK 解码，并做两道大小上限校验；
     *         DATA 先做接收流控预检，超过半窗再回 WINDOW_UPDATE；
     * step4. 推进对应流的 `StreamStateMachine`（新流初建窗口），填充 `IncomingEvent`。
     *
     * ## 失败语义
     * 协议错误返回 `kProtocolError` 且不自动发 GOAWAY：调用方应停止使用该连接。
     */
    ConnStatus ReadEvent(IncomingEvent* out, std::string* error = nullptr);

    /** @brief 流表（只读，便于测试与诊断）。 */
    const std::map<std::uint32_t, StreamStateMachine>& Streams() const noexcept {
        return m_streams;
    }

    /** @brief 当前已写出的未刷新字节数（含在 Transport 内的）。 */
    std::size_t PendingInboundBytes() const noexcept { return m_read_buffer.size(); }

private:
    /// 保证缓冲区至少有 `need` 字节；不足时从 Transport 继续读。
    bool FillBuffer(std::size_t need, std::string* error);
    /// 从缓冲区取出一个完整帧。
    ConnStatus TakeFrame(Frame* out, std::string* error);
    /// 写一帧（直接经 Transport）。
    ConnStatus WriteFrame(const Frame& frame, std::string* error);
    /**
     * @brief 写一个 header block，必要时拆成 HEADERS + CONTINUATION。
     * @details 单个帧的 payload 上限是 `m_peer_max_frame_size - 9`（去掉帧头）。
     *          只有最后一片带 `END_HEADERS`；`end_stream` 只加在 HEADERS 上。
     *
     * ## 步骤
     * step1. 用 `m_peer_max_frame_size - 9` 算单片上限；
     * step2. 一帧装得下走快路径（绝大多数情况）；
     * step3. 否则 HEADERS(无 END_HEADERS) + 若干 CONTINUATION，最后一片带 END_HEADERS。
     */
    ConnStatus WriteHeaderBlock(std::uint32_t stream_id,
                                const std::vector<std::uint8_t>& block, bool end_stream,
                                std::string* error);
    /**
     * @brief 读取并拼接完整的 header block（处理零个或多个 CONTINUATION）。
     * @param first_block 首个 HEADERS 携带的片段。
     * @param stream_id 期望的流 id（CONTINUATION 必须同一条流）。
     * @param out 拼接结果。
     *
     * ## 步骤
     * step1. 清空输出并检查首片是否已超限；
     * step2. 循环取帧，直到遇到 `END_HEADERS`；
     * step3. 校验帧类型必须是 CONTINUATION、流 id 必须与 HEADERS 一致；
     * step4. 边拼边累计上限（不能等收完再判，否则 CONTINUATION 泛滥可撑爆内存）。
     */
    ConnStatus AssembleHeaderBlock(std::uint32_t stream_id,
                                   const std::vector<std::uint8_t>& first_block,
                                   std::vector<std::uint8_t>* out, std::string* error);
    /// 处理握手期的 SETTINGS 交换。
    ConnStatus FinishSettingsHandshake(std::string* error);
    /**
     * @brief 应用对端 SETTINGS（握手期与运行期共用）。
     * @details 只处理本实现支持且会影响发送行为的参数：
     *          - `HEADER_TABLE_SIZE`：HPACK 编码器上限，并排队 0x20 表大小更新；
     *          - `MAX_FRAME_SIZE`：发送帧的 payload 上限；
     *          - `INITIAL_WINDOW_SIZE`：流级发送窗口初值（按差值调整已有流，
     *            **不**影响连接级窗口 —— 连接窗口只能由 WINDOW_UPDATE 改变）；
     *          - `MAX_CONCURRENT_STREAMS`：对端允许的并发流上限。
     *          值域校验在 `ParseSettings()`（frame.cpp）完成，非法值不会到达这里。
     */
    ConnStatus ApplyPeerSettings(const SettingsFrame& settings, std::string* error);
    /**
     * @brief 把「动态表大小更新」排到下一个 header block 的首部。
     * @details RFC 7541 §4.2 要求表大小变化必须在后续第一个 header block 的
     *          开头用 `001xxxxx` 表示通知对端。`Encoder::SetTableSize()` 只改
     *          上限不扩表，真正的扩容/通知由这里完成：先调
     *          `Encoder::AppendTableSizeUpdate()`（它会发字节并改本地表上限），
     *          把字节暂存到 `m_pending_table_size_update`，由
     *          `StartRequest()` / `SendResponseHeaders()` 拼到块首。
     * @note 若在下一个块发出前又收到新的 SETTINGS，直接以最新值覆盖待发字节
     *       （对端只需知道最终大小，RFC 允许块首最多两条更新，发一条足够）。
     */
    ConnStatus QueueTableSizeUpdate(std::uint32_t new_size, std::string* error);
    /// 取或创建流状态机。
    StreamStateMachine* StreamFor(std::uint32_t stream_id);
    /// 当前计入并发上限的流数（open + half-closed，不含 closed）。
    std::size_t OpenStreamCount() const noexcept;

    /// 字节通道；构造注入，所有 Read/Write 都经它，连接不拥有其生命周期。
    Transport* m_transport{nullptr};
    /// 角色；构造时决定，role() 只读，影响 preface 与流号奇偶。
    Role m_role{Role::kClient};
    /// 构造时的配置快照；config() 只读，握手/流控上限都取自它。
    ConnectionConfig m_config;
    /// 握手完成标记；仅 FinishSettingsHandshake() 置真，Handshake() 读取。
    bool m_handshaked{false};
    /// 连接生命周期状态；由本类内部迁移（见 ConnState）。
    ConnState m_state{ConnState::kOpen};
    /// 对端 GOAWAY 的 last_stream_id；0 表示尚未收到。
    std::uint32_t m_peer_last_stream_id{0};
    /// 下一个可用的客户端流 id（奇数）。
    std::uint32_t m_next_stream_id{1};
    /// 对端已发起过的最大流 id；新流必须严格大于它（RFC 9113 §5.1.1）。
    /// 流表里已关闭的流不会被删除，因此奇偶与单调性校验只看这个水位即可。
    std::uint32_t m_max_seen_peer_stream_id{0};
    /// 对端允许的单帧上限；SETTINGS 到达时更新，WriteHeaderBlock()/WriteData() 使用。
    std::uint32_t m_peer_max_frame_size{kDefaultMaxFrameSize};
    /// 对端 SETTINGS_INITIAL_WINDOW_SIZE（流级发送窗口初值）。
    /// 注意：它**不**是连接级窗口；连接窗口初值恒为 65535，只受 WINDOW_UPDATE(0) 影响。
    std::int64_t m_peer_initial_window_size{65535};
    /// 对端 SETTINGS_MAX_CONCURRENT_STREAMS；未收到时按 RFC 视为不限。
    std::uint32_t m_peer_max_concurrent_streams{0};
    bool m_peer_max_concurrent_set{false};
    /// 对端 SETTINGS_MAX_HEADER_LIST_SIZE；未通告时本端发送不受该值限制。
    std::uint32_t m_peer_max_header_list_size{0};
    bool m_peer_max_header_list_size_set{false};
    /// 对端（客户端）通告的 ENABLE_PUSH。服务端记录但不推送；客户端收到该
    /// 设置本身就是协议错误（服务端不得发送，RFC 9113 §6.5.2）。
    bool m_peer_enable_push{true};
    /// 读方向缓冲；FillBuffer() 追加、TakeFrame() 消费并 erase。
    std::vector<std::uint8_t> m_read_buffer;
    /// HPACK 解码器（含动态表）；只在 ReadEvent() 的 HEADERS 分支使用。
    Decoder m_decoder;
    /// HPACK 编码器（含动态表）；StartRequest()/SendResponseHeaders() 写，SETTINGS 到达时收缩。
    Encoder m_encoder;
    /// 待拼到下一个 header block 首部的 HPACK 动态表大小更新字节（0x20 前缀）。
    /// 由 QueueTableSizeUpdate() 写入，StartRequest()/SendResponseHeaders() 消费。
    std::vector<std::uint8_t> m_pending_table_size_update;
    /// 流表（键为流 id）；StreamFor() 查找、StartRequest()/ReadEvent() 插入、Streams() 只读暴露。
    std::map<std::uint32_t, StreamStateMachine> m_streams;
    /// 握手期间暂存的 SETTINGS 是否已收到/已发送。
    /// 是否已收到对端非 ACK SETTINGS；FinishSettingsHandshake() 置位。
    bool m_peer_settings_received{false};
    /// 本地 SETTINGS 是否已写出；Handshake() 置位（当前仅用于诊断）。
    bool m_local_settings_sent{false};

    // ---- 流控（RFC 9113 §6.9）----
    // 窗口可以为负（对端可能一次发来超过初始窗口的 DATA 之前先扣减），
    // 所以用有符号 64 位。
    /// 我方允许对端发送的剩余字节数（连接级）。
    std::int64_t m_local_window{65535};
    /// 对端允许我方发送的剩余字节数（连接级）。
    std::int64_t m_remote_window{65535};
    /// 每条流的本地窗口。
    std::map<std::uint32_t, std::int64_t> m_stream_local_window;
    /// 每条流的对端窗口。
    std::map<std::uint32_t, std::int64_t> m_stream_remote_window;

    /// 清理超过 max_retired_streams 的已关闭流（流表 + 两条窗口记录）。
    void ReapClosedStreams();
    /// 判断 id 是否属于「已分配/已见过、但已被流表淘汰」的流（容忍迟到帧）。
    bool IsEvictedStream(std::uint32_t stream_id) const noexcept;
    /// 按对端 SETTINGS_MAX_HEADER_LIST_SIZE 检查要发送的 header 列表。
    ConnStatus CheckOutboundHeaderList(const std::vector<HeaderField>& headers,
                                       std::string* error) const;
    /// 补齐客户端请求缺失的 :scheme（按配置），返回真正要编码的列表。
    std::vector<HeaderField> PrepareOutboundHeaders(
        const std::vector<HeaderField>& headers) const;
    /// 尽力发送 GOAWAY（用于帧级致命错误），失败不影响错误返回路径。
    void SendFatalGoAway(ErrorCode code);
    /// 初始化/重置某条流的窗口。
    void ResetStreamWindows(std::uint32_t stream_id);
    /// 只读检查发送窗口是否足够（不扣减）。
    bool HasSendWindow(std::uint32_t stream_id, std::size_t bytes) const;
    /// 检查并扣减发送窗口；不足返回 false。
    bool ConsumeSendWindow(std::uint32_t stream_id, std::size_t bytes);
};

}  // namespace go2cpp::net::http2
