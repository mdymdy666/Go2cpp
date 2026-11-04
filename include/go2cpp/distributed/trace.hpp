#pragma once

/**
 * @file trace.hpp
 * @brief 分布式追踪上下文：`TraceId` / `SpanId` / `TraceContext`（header-only）。
 *
 * ## 模块定位
 *
 * P0 的目标是「一次跨节点调用能被串起来」，而不是完整的 OTel 实现。本文件
 * 只提供三件事：
 *
 * 1. **标识**：128 位 `TraceId`、64 位 `SpanId`，全 0 表示无效；
 * 2. **传播**：`TraceContext::ToHeader()` / `TryParseHeader()` 的文本格式
 *    （W3C traceparent 风格），以及 `InjectTrace` / `ExtractTrace` 把上下文
 *    写进/读出**请求头 map**，供任何带元数据的传输层复用；
 * 3. **当前上下文**：线程局部 `SetCurrentTraceContext` + RAII `TraceScope`，
 *    让 rpc 客户端在发请求时不需要每个调用点都显式传参。
 *
 * ## 文本格式
 *
 * ```text
 * 00-<trace-id 32hex>-<span-id 16hex>-<parent-span-id 16hex>-01
 * ```
 *
 * 与 W3C `traceparent` 的差异：多了第 4 段 parent-span-id（全 0 表示根
 * span）。这样下游既能知道「上游是谁」（parent），又能直接延续自己的
 * span，不需要额外 header。版本固定 `00`，标志固定 `01`（sampled）。
 *
 * ## 线程模型
 *
 * - 值类型全部可拷贝，比较/编解码是纯函数，任意线程安全；
 * - `CurrentTraceContext()` 是 `thread_local`：一个线程只持有一个当前
 *   span。跨线程传递时必须显式拷贝上下文（不要依赖 thread_local 继承）。
 *
 * ## 边界
 *
 * - 不做采样决策、不做导出、不做时钟/时长：这些是 OTel 集成（P2+）的事；
 * - 不校验 TraceId 的版本位（128 位全随机即可），只拒绝全 0。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>

namespace go2cpp::distributed {

namespace trace_detail {

/// @brief 十六进制字符转 0..15；非法返回 16。
inline int HexValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return 16;
}

/// @brief 把低 `digits` 个十六进制字符写进 `out` 的 `[begin, begin+digits)`。
inline void WriteHex(std::string& out, std::size_t begin, std::size_t digits,
                     std::uint64_t value) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t index = digits; index > 0; --index) {
        out[begin + index - 1] = kDigits[value & 0xFULL];
        value >>= 4;
    }
}

/// @brief 原子序号源（inline 函数内的 static 在整个进程内唯一）。
inline std::atomic<std::uint64_t>& Counter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

/// @brief 进程级随机种子（首次调用读一次熵源，之后只做纯计算）。
inline std::uint64_t Seed() noexcept {
    static const std::uint64_t seed = []() noexcept {
        std::uint64_t value = 0;
        try {
            std::random_device device;
            value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        } catch (...) {
            value = 0;
        }
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        value ^= static_cast<std::uint64_t>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) *
                 0xD6E8FEB86659FD93ULL;
        value ^= reinterpret_cast<std::uintptr_t>(&value) * 0x9E3779B97F4A7C15ULL;
        return value == 0 ? 0xA5A5A5A5A5A5A5A5ULL : value;
    }();
    return seed;
}

/// @brief SplitMix64 混合（无状态、雪崩好）。
inline std::uint64_t Mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

/// @brief 读取下一个 64 位伪随机值（序号 + 种子混合，无系统调用）。
inline std::uint64_t NextRandom() noexcept {
    const std::uint64_t sequence = Counter().fetch_add(1, std::memory_order_relaxed);
    return Mix(Seed() + sequence * 0x9E3779B97F4A7C15ULL);
}

} // namespace trace_detail

/**
 * @brief 128 位追踪 id（一次请求跨节点保持不变的关联键）。
 */
class TraceId final {
public:
    /// 十六进制文本长度：32 个字符。
    static constexpr std::size_t kTextLength = 32;

    constexpr TraceId() noexcept = default;

    /// @brief 由两个半区构造（反序列化/测试用）。
    static constexpr TraceId FromParts(std::uint64_t high, std::uint64_t low) noexcept {
        TraceId id;
        id.m_high = high;
        id.m_low = low;
        return id;
    }

    /**
     * @brief 生成新的追踪 id（128 位，无锁、无系统调用）。
     * @return 新的 id；同一进程内连续调用不相等。
     */
    static TraceId Generate() noexcept {
        const std::uint64_t high = trace_detail::NextRandom();
        const std::uint64_t low = trace_detail::Mix(high ^ trace_detail::NextRandom());
        return FromParts(high, low);
    }

    /// @brief 是否非全 0。
    constexpr bool IsValid() const noexcept { return m_high != 0 || m_low != 0; }
    constexpr std::uint64_t high() const noexcept { return m_high; }
    constexpr std::uint64_t low() const noexcept { return m_low; }

    /// @brief 32 个小写十六进制字符；无效 id 输出全 0。
    std::string ToString() const {
        std::string text(kTextLength, '0');
        trace_detail::WriteHex(text, 0, 16, m_high);
        trace_detail::WriteHex(text, 16, 16, m_low);
        return text;
    }

    /**
     * @brief 解析 32 个十六进制字符。
     * @param text [in] 待解析文本（借用；不保留）。
     * @return 解析结果；长度或字符非法时返回无效 id（全 0）。
     */
    static TraceId Parse(std::string_view text) noexcept {
        TraceId id;
        if (TryParse(text, &id)) {
            return id;
        }
        return TraceId{};
    }

    /**
     * @brief `Parse` 的出参版本。
     * @param text [in] 待解析文本（长度必须恰为 32）。
     * @param out [out] 成功写入；失败时保持原值，可为 nullptr。
     * @return 成功 true；`out == nullptr` 或文本非法返回 false。
     */
    static bool TryParse(std::string_view text, TraceId* out) noexcept {
        if (out == nullptr || text.size() != kTextLength) {
            return false;
        }
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
            const int value = trace_detail::HexValue(text[index]);
            if (value >= 16) {
                return false;
            }
            if (index < 16) {
                high = (high << 4) | static_cast<std::uint64_t>(value);
            } else {
                low = (low << 4) | static_cast<std::uint64_t>(value);
            }
        }
        *out = FromParts(high, low);
        return true;
    }

    friend constexpr bool operator==(const TraceId& lhs, const TraceId& rhs) noexcept {
        return lhs.m_high == rhs.m_high && lhs.m_low == rhs.m_low;
    }
    friend constexpr bool operator!=(const TraceId& lhs, const TraceId& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(const TraceId& lhs, const TraceId& rhs) noexcept {
        return lhs.m_high < rhs.m_high ||
               (lhs.m_high == rhs.m_high && lhs.m_low < rhs.m_low);
    }

private:
    std::uint64_t m_high{0};  ///< 高 64 位。
    std::uint64_t m_low{0};   ///< 低 64 位。
};

/**
 * @brief 64 位 span id（一次调用内的节点；0 表示「无父 span」）。
 */
class SpanId final {
public:
    /// 十六进制文本长度：16 个字符。
    static constexpr std::size_t kTextLength = 16;

    constexpr SpanId() noexcept = default;
    explicit constexpr SpanId(std::uint64_t value) noexcept : m_value(value) {}

    /// @brief 生成新的 span id（64 位，无锁、无系统调用）。
    static SpanId Generate() noexcept {
        std::uint64_t value = trace_detail::NextRandom();
        // 全 0 保留给「无父 span」，极小概率命中时换一个非 0 值。
        if (value == 0) {
            value = 1;
        }
        return SpanId(value);
    }

    /// @brief 是否非 0。
    constexpr bool IsValid() const noexcept { return m_value != 0; }
    constexpr std::uint64_t value() const noexcept { return m_value; }

    /// @brief 16 个小写十六进制字符；无效 id 输出全 0。
    std::string ToString() const {
        std::string text(kTextLength, '0');
        trace_detail::WriteHex(text, 0, kTextLength, m_value);
        return text;
    }

    /**
     * @brief 解析 16 个十六进制字符。
     * @param text [in] 待解析文本（长度必须恰为 16）。
     * @return 解析结果；非法文本返回无效 id（0）。
     */
    static SpanId Parse(std::string_view text) noexcept {
        if (text.size() != kTextLength) {
            return SpanId{};
        }
        std::uint64_t value = 0;
        for (const char character : text) {
            const int digit = trace_detail::HexValue(character);
            if (digit >= 16) {
                return SpanId{};
            }
            value = (value << 4) | static_cast<std::uint64_t>(digit);
        }
        return SpanId(value);
    }

    friend constexpr bool operator==(const SpanId& lhs, const SpanId& rhs) noexcept {
        return lhs.m_value == rhs.m_value;
    }
    friend constexpr bool operator!=(const SpanId& lhs, const SpanId& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(const SpanId& lhs, const SpanId& rhs) noexcept {
        return lhs.m_value < rhs.m_value;
    }

private:
    std::uint64_t m_value{0};  ///< 0 表示无效/无父。
};

/**
 * @brief 一次调用的追踪上下文：当前 span + 父 span + 整条链路 id。
 *
 * @details 跨节点传播时只传这三个值；下游用 `ChildSpan()` 生成自己的
 *          span（trace 不变、parent 变成上游 span），从而形成父子链。
 */
struct TraceContext {
    TraceId trace;    ///< 整条链路 id（跨节点不变）。
    SpanId span;      ///< 当前节点的 span id。
    SpanId parent;    ///< 上游 span id；无效表示根 span。

    /// @brief 是否可用于传播：trace 与 span 都必须有效。
    bool IsValid() const noexcept { return trace.IsValid() && span.IsValid(); }

    /// @brief 创建根上下文（新的 trace + 新的 span，无父）。
    static TraceContext CreateRoot() noexcept {
        TraceContext context;
        context.trace = TraceId::Generate();
        context.span = SpanId::Generate();
        return context;
    }

    /**
     * @brief 派生下游 span。
     * @return 新上下文：`trace` 不变、`parent` 为当前 `span`、`span` 为新值。
     *         当前上下文无效时返回无效上下文（不抛异常）。
     */
    TraceContext ChildSpan() const noexcept {
        if (!IsValid()) {
            return TraceContext{};
        }
        TraceContext child;
        child.trace = trace;
        child.parent = span;
        child.span = SpanId::Generate();
        return child;
    }

    /**
     * @brief 编码为 `00-<trace>-<span>-<parent>-01` 文本。
     * @return 定长 32+1+16+1+16+1+16+1+2 = 72 字节；无效上下文返回空串。
     */
    std::string ToHeader() const {
        if (!IsValid()) {
            return {};
        }
        std::string header;
        header.reserve(72);
        header.append("00-");
        header.append(trace.ToString());
        header.push_back('-');
        header.append(span.ToString());
        header.push_back('-');
        header.append(parent.ToString());
        header.append("-01");
        return header;
    }

    /**
     * @brief 解析 `ToHeader()` 的文本。
     * @param text [in] 待解析文本（借用；不保留）。
     * @return 解析结果；段数/长度/版本/标志/十六进制任一非法都返回无效上下文。
     */
    static TraceContext ParseHeader(std::string_view text) noexcept {
        TraceContext context;
        if (TryParseHeader(text, &context)) {
            return context;
        }
        return TraceContext{};
    }

    /**
     * @brief `ParseHeader` 的出参版本，便于区分「无 trace」与「解析失败」。
     *
     * @param text [in] 待解析文本；长度必须恰为 72，版本 `00`、标志 `01`；
     *        借用，调用返回后不保留。
     * @param out [out] 成功时写入解析结果；失败时**保持原值不变**，可为 nullptr。
     *
     * @return
     * - true：`out` 已写入且 `IsValid() == true`；调用方可继续 `ChildSpan()`。
     * - false：长度/版本/标志/分隔符/十六进制任一非法；调用方**不要**使用
     *   `out`（它仍是旧值）。
     *
     * ## 定位
     * 跨节点追踪的**唯一反序列化入口**。如果这里宽松（例如接受错位字符），
     * 下游会把不同链路的 span 拼到同一棵树上，排障时的因果链就是错的。
     *
     * ## 步骤
     * step1. 长度必须恰为 72（`00-` + 32 + `-` + 16 + `-` + 16 + `-01`）；
     * step2. 固定位置校验版本/标志/4 个分隔符，避免用 split 带来的容错；
     * step3. 解析 trace 并拒绝全 0；span 必须非 0，parent 允许全 0（根 span）；
     * step4. 用「原文 == 规范化文本」识别非法十六进制（Parse 对非法返回 0）；
     * step5. 全部通过才写 `out`，保证失败时出参不被污染。
     *
     * ## 并发/失败语义
     * - 调用约束：任意线程，纯函数，不共享可变状态。
     * - 失败语义：`noexcept`，不抛异常；失败时 `out` 保持原值。
     * - 不变量：`ParseHeader(ctx.ToHeader()) == ctx` 对任意有效 ctx 成立。
     */
    static bool TryParseHeader(std::string_view text, TraceContext* out) noexcept {
        if (out == nullptr || text.size() != 72) {
            return false;
        }
        if (text[0] != '0' || text[1] != '0' || text[2] != '-' || text[35] != '-' ||
            text[52] != '-' || text[69] != '-' || text[70] != '0' || text[71] != '1') {
            return false;
        }
        TraceId trace;
        if (!TraceId::TryParse(text.substr(3, 32), &trace) || !trace.IsValid()) {
            return false;
        }
        // SpanId::Parse 对非法十六进制返回 0；用「原文 == 规范化文本」区分
        // 「合法的全 0 parent」与「含非法字符」。span 不允许全 0，parent 允许。
        const std::string_view span_text = text.substr(36, 16);
        const std::string_view parent_text = text.substr(53, 16);
        const SpanId span = SpanId::Parse(span_text);
        const SpanId parent = SpanId::Parse(parent_text);
        if (!span.IsValid() || span.ToString() != span_text || parent.ToString() != parent_text) {
            return false;
        }
        TraceContext context;
        context.trace = trace;
        context.span = span;
        context.parent = parent;
        *out = context;
        return true;
    }
};

/// @brief 追踪上下文的请求头键（HTTP 风格 map，rpc/传输层可复用）。
inline constexpr std::string_view kTraceHeader = "g2c-traceparent";

/**
 * @brief 把追踪上下文写入请求头 map。
 * @param headers [in,out] 目标头 map（键 `kTraceHeader`）；已有值会被覆盖。
 * @param context [in] 上下文；无效时**不写入**并删除已有键。
 */
inline void InjectTrace(std::map<std::string, std::string>& headers,
                        const TraceContext& context) {
    const std::string header = context.ToHeader();
    if (header.empty()) {
        headers.erase(std::string(kTraceHeader));
        return;
    }
    headers[std::string(kTraceHeader)] = header;
}

/**
 * @brief 从请求头 map 读取追踪上下文。
 * @param headers [in] 请求头 map（借用；不保留）。
 * @param out [out] 成功写入；失败时保持原值，可为 nullptr。
 * @return 成功 true；没有该键、值为空或格式非法返回 false。
 */
inline bool ExtractTrace(const std::map<std::string, std::string>& headers,
                         TraceContext* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    const auto it = headers.find(std::string(kTraceHeader));
    if (it == headers.end() || it->second.empty()) {
        return false;
    }
    return TraceContext::TryParseHeader(it->second, out);
}

/**
 * @brief 本线程当前上下文的唯一存储（所有读写都经过它）。
 * @return 线程局部对象的引用；同一线程内所有调用共享同一实例。
 */
inline TraceContext& CurrentTraceContextStorage() noexcept {
    static thread_local TraceContext current;
    return current;
}

/**
 * @brief 返回本线程当前的追踪上下文（默认无效；只读）。
 * @return 当前上下文引用；线程退出前有效，调用方不得跨线程持有。
 */
inline const TraceContext& CurrentTraceContext() noexcept {
    return CurrentTraceContextStorage();
}

/**
 * @brief 设置本线程当前追踪上下文（覆盖旧值）。
 * @param context [in] 新上下文；无效值会清空当前上下文。
 */
inline void SetCurrentTraceContext(const TraceContext& context) noexcept {
    CurrentTraceContextStorage() = context;
}

/**
 * @brief 清空本线程当前追踪上下文。
 */
inline void ClearCurrentTraceContext() noexcept {
    SetCurrentTraceContext(TraceContext{});
}

/**
 * @brief 作用域内设置当前追踪上下文，析构时恢复旧值（RAII，不可拷贝）。
 *
 * @details 用于「客户端发起一次调用」：进入作用域设置父上下文，调用返回
 *          后自动恢复，避免污染调用方的上下文。析构函数是 `noexcept`。
 */
class TraceScope final {
public:
    /// @brief 设置当前上下文并记录旧值。
    explicit TraceScope(const TraceContext& context) noexcept : m_previous(CurrentTraceContext()) {
        SetCurrentTraceContext(context);
    }

    ~TraceScope() { SetCurrentTraceContext(m_previous); }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
    TraceScope(TraceScope&&) = delete;
    TraceScope& operator=(TraceScope&&) = delete;

private:
    TraceContext m_previous;  ///< 进入作用域前的上下文，析构时恢复。
};

} // namespace go2cpp::distributed
