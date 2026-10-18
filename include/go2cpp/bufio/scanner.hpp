/**
 * @file scanner.hpp
 * @brief `bufio::Scanner` —— 按分隔符切分输入流（对应 Go 的 `bufio.Scanner`）。
 *
 * @details **切分模型与 Go 完全一致**：`Scanner` 维护一块**自己的**
 *   缓冲区，反复把「尚未切分的窗口」交给 `SplitFunc`：
 *
 *   ```
 *   SplitFunc(data, atEOF) -> { advance, token, error }
 *   ```
 *
 *   - `advance`：从 `data` 头部消费多少字节（可以消费掉分隔符）；
 *   - `token`：当前切出的 token，**必须指向 `data` 内部**（零拷贝）；
 *   - `error`：非空即终止扫描，`Err()` 可读。
 *
 *   若 `advance == 0 && token 为空`，表示「数据还不够，继续读」；在
 *   `atEOF == true` 时这表示「没有更多 token」。
 *
 *   **为什么 token 是指针而不是拷贝**：Go 的 `Scanner.Bytes()` 直接返回
 *   缓冲切片；`Text()` 才拷贝。本实现保持同样分工，便于大文件逐行处理
 *   不产生每行一次分配。
 *
 *   ## 与 Go 对齐的语义
 *   - 默认按行（`ScanLines`），去掉行尾 `\n` 与 `\r`；
 *   - 缓冲从 `buffer_size`（默认 4096）开始，token 放不下时**翻倍增长**，
 *     上限 `max_token_size`（默认 64 KiB，Go 的 `MaxScanTokenSize`）；
 *     超限 → `ErrTooLong`；
 *   - 正常读到流末尾时 `Scan()` 返回 false，且 `Err()` 为**空**
 *     （Go 的 `Scanner.Err` 只在真实错误时非空）；
 *   - `Bytes()` 返回的视图在下一次 `Scan()` 后失效；`Text()` 返回拷贝。
 *
 *   ## 与 Go 的差异
 *   1. `ScanWords` 只按 **ASCII 空白**（空格、`\t`、`\n`、`\r`、`\v`、`\f`）
 *      切分。Go 用 `unicode.IsSpace` 走 UTF-8 解码；本项目尚未提供 rune
 *      解码依赖，按 ASCII 实现并在测试里固定行为。
 *   2. 自定义 `SplitFunc` 的 `advance` 越界、或 `token` 落到窗口外时，
 *      报 `ErrInvalidAdvance` 而**不是 panic**——C++ 侧不能让用户回调
 *      把进程带走。
 *
 * ## 模块定位
 *
 * `bufio` 的「切分层」，位于 `bufio::Reader`（或任意 `Read(char*, size_t)` 源）
 * 之上、具体协议解析之下。它解决的是「一次读多少」之外的另一个问题：
 * **一条记录到哪里结束**。上游只提供字节流，`Scanner` 用可替换的 `SplitFunc`
 * 把它变成逐行/逐词/逐字节的 token；下游是日志读取、配置解析、HTTP 头解析等
 * 按记录消费的模块。
 *
 * ## 本文件定义什么
 *
 * - `SplitResult`：`SplitFunc` 的三元返回（`advance` / `token` / `error`）；
 * - `SplitFunc`：切分函数类型；
 * - `ScanLines` / `ScanWords` / `ScanBytes`：三个预置切分函数（定义在
 *   `src/bufio/bufio.cpp`，因此使用方必须链接 `go2cpp_bufio`）；
 * - `Scanner<ReaderT>`：拥有窗口缓冲、负责扩容与终止判定的扫描器。
 *
 * ## 阅读建议
 *
 * 1. 先读文件头的 `SplitFunc` 契约：`advance == 0 && token 为空` 是「继续读」，
 *    `ErrFinalToken` 是「最后一个 token」；
 * 2. 再看 `Scanner::Scan` 的五个步骤与终止路径；
 * 3. 最后看 `ReadMore` 的窗口压缩/翻倍扩容——`ErrTooLong` 只可能从这里产生。
 *
 * ## 边界与陷阱
 *
 * `Bytes()` 的视图在下一次 `Scan()` 后失效；`Text()` 才拷贝。`Scan()` 返回
 * false 时**必须**看 `Err()`：正常 EOF 时它是空的，真实错误才非空。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/errors.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bufio {

/**
 * @brief `SplitFunc` 的返回值。
 * @details 与 Go 的 `(advance int, token []byte, err error)` 一一对应。
 *
 * ## 定位
 * 用户自定义 `SplitFunc` 与 `Scanner::Scan` 之间的唯一协议对象；
 * `ScanLines`/`ScanWords`/`ScanBytes` 与调用方传入的 lambda 都返回它。
 */
struct SplitResult {
    /// 从窗口头部消费的字节数；0 表示「需要更多数据」。
    std::size_t advance{0};
    /**
     * @brief 切出的 token；**必须**指向传入窗口内部。
     * @details 用 `data != nullptr` 判定「本轮有没有 token」——这样
     *          「空 token」（例如连续两个 `\n` 之间的空行）才是可表达的；
     *          `advance == 0` 的空 token 会被当成「数据还不够」。
     *          默认构造的 `ByteSpan`（`data == nullptr`）表示没有 token。
     */
    ByteSpan token{};
    /// 非空即终止扫描。
    ErrorPtr error;
};

/// 切分函数类型（Go 的 `bufio.SplitFunc`）。
using SplitFunc = std::function<SplitResult(ByteSpan data, bool at_eof)>;

/// 按行切分：去掉行尾 `\n`，若存在 `\r` 也去掉（Go 的 `ScanLines`）。
SplitResult ScanLines(ByteSpan data, bool at_eof);

/// 按 ASCII 空白切分，跳过前导空白（Go 的 `ScanWords` 的 ASCII 版）。
SplitResult ScanWords(ByteSpan data, bool at_eof);

/// 每个字节一个 token（Go 的 `ScanBytes`）。
SplitResult ScanBytes(ByteSpan data, bool at_eof);

/**
 * @brief 把底层 Reader 切成 token 的扫描器。
 * @tparam ReaderT 底层 Reader（契约同 `bufio::Reader`：`Read(char*, size_t)`）。
 *
 * ## 定位
 * `bufio` 的切分层：上游是任意 `Read(char*, size_t)` 源（通常是
 * `bufio::Reader`），下游是所有「按行/按词/按字节」消费的协议解析。
 * `Scanner` 自己拥有窗口缓冲，因此不依赖 `bufio::Reader` 的 `ReadLine`/
 * `ReadSlice`，可以独立决定扩容与超长判定。
 *
 * ## 生命周期与线程模型
 * 构造时按引用绑定底层 Reader 并分配初始缓冲；不可拷贝/赋值。谁创建谁销毁，
 * 底层对象必须比它活得久。所有方法假定同一线程串行调用，内部**不加锁**；
 * `SplitFunc` 也在调用线程上同步执行，不要在里面做阻塞等待。
 *
 * ## 状态机
 * `Idle -> Token/Finished/Failed` 等转化见 `docs/state-machines/Scanner.md`。
 *
 * ## 不变量
 * - `0 <= m_start <= m_end <= m_buffer.size()`；
 * - `m_max_token_size >= m_buffer.size() >= m_initial_buffer_size`；
 * - `m_done` 为真后 `m_token` 为空、`m_start == m_end == 0`，`Scan()` 直接 false；
 * - `m_done && !m_err` 表示正常 EOF；`m_done && m_err` 表示失败终止；
 * - `m_token` 非空时，其 `[data, data + size)` 必定落在 `m_buffer` 内。
 *
 * @warning 只引用底层 Reader，不拥有它。
 */
template <typename ReaderT>
class Scanner {
public:
    /**
     * @brief 用默认缓冲（4096）与默认上限（64 KiB）构造扫描器。
     * @param source 底层 Reader。
     */
    explicit Scanner(ReaderT& source) : Scanner(source, kDefaultBufferSize) {}

    /**
     * @brief 指定初始缓冲尺寸构造扫描器。
     * @param source 底层 Reader。
     * @param buffer_size 初始缓冲；小于 16 提升到 16。
     * @param max_token_size 单 token 上限；小于初始缓冲时提升到初始缓冲。
     */
    explicit Scanner(ReaderT& source, std::size_t buffer_size,
                     std::size_t max_token_size = kMaxScanTokenSize)
        : m_source(&source),
          m_buffer(detail::ClampBufferSize(buffer_size)),
          m_initial_buffer_size(detail::ClampBufferSize(buffer_size)),
          m_max_token_size(std::max(detail::ClampBufferSize(buffer_size),
                                    max_token_size)),
          m_split(ScanLines) {}

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    /**
     * @brief 推进到下一个 token。
     * @return 切到 token 返回 true；流结束或有错误返回 false。
     * @note 返回 false 后：`Err()` 为空表示正常结束，否则是终止原因。
     *
     * ## 步骤
     * step1. 已终止（`m_done`）直接 false；清空上一轮 token 视图；
     * step2. 只要窗口有数据或已有错误，就以 `atEOF = (m_err != nullptr)` 调
     *        `SplitFunc`；
     * step3. 处理返回值：`ErrFinalToken` 交付 token 后正常终止；其它错误终止；
     *        `advance`/`token` 越界或 `advance` 未覆盖 token → `ErrInvalidAdvance`；
     * step4. 有合法 token 且能推进时记录并返回 true；只有 `advance` 则继续切；
     * step5. 窗口切不出且已 EOF → 正常结束；否则 `ReadMore()`，失败则终止。
     */
    bool Scan() {
        // step1: 终止后不再工作；每次 Scan 先从「无 token」开始
        if (m_done) {
            return false;
        }
        m_token = ByteSpan();
        while (true) {
            // step2: 窗口非空或已带错误时，交给切分函数；错误态也要再切一次 atEOF
            if (m_end > m_start || m_err) {
                const ByteSpan window(m_buffer.data() + m_start, m_end - m_start);
                const SplitResult result = m_split(window, m_err != nullptr);
                // step3: 先处理错误/终止信号，再校验 advance 与 token 的边界
                if (result.error) {
                    if (go2cpp::Is(result.error, ErrFinalToken())) {
                        // Go 1.20+：SplitFunc 用 ErrFinalToken 声明「最后一个
                        // token」。本次仍交付 token，下一次 Scan 返回 false 且
                        // Err() 为空（哨兵本身不是错误）。
                        if (result.token.data() != nullptr) {
                            const std::uint8_t* const window_begin = window.data();
                            const std::uint8_t* const window_end =
                                window_begin + window.size();
                            if (result.token.data() < window_begin ||
                                result.token.data() + result.token.size() >
                                    window_end) {
                                SetErr(ErrInvalidAdvance());
                                Shutdown();
                                return false;
                            }
                            m_token = result.token;
                        }
                        // Go 的 readErr() 在把 atEOF=true 交给 SplitFunc 时
                        // 已经把 EOF 消费掉；否则 Err() 会错误地返回 EOF。
                        if (go2cpp::Is(m_err, streams::IoEof())) {
                            m_err = {};
                        }
                        Shutdown();
                        return true;
                    }
                    SetErr(result.error);
                    Shutdown();
                    return false;
                }
                // step4: advance 不能越过窗口；后面还会校验 token 与覆盖关系
                if (result.advance > window.size()) {
                    SetErr(ErrInvalidAdvance());
                    Shutdown();
                    return false;
                }
                // 判定「有 token」用指针而不是长度：空行是合法 token
                // （Go 里 token != nil 与 len(token) == 0 是两个概念）。
                if (result.token.data() != nullptr) {
                    const std::uint8_t* const window_begin = window.data();
                    const std::uint8_t* const window_end = window_begin + window.size();
                    if (result.token.data() < window_begin ||
                        result.token.data() + result.token.size() > window_end) {
                        SetErr(ErrInvalidAdvance());
                        Shutdown();
                        return false;
                    }
                    // advance 必须覆盖 token，否则同一个 token 会被无限重复交付。
                    const std::size_t token_offset =
                        static_cast<std::size_t>(result.token.data() - window_begin);
                    if (result.advance < token_offset + result.token.size()) {
                        SetErr(ErrInvalidAdvance());
                        Shutdown();
                        return false;
                    }
                    // 空 token 且 advance == 0 无法推进，按「需要更多数据」处理，
                    // 否则 Scan 会永远返回同一个空 token。
                    if (!(result.token.empty() && result.advance == 0)) {
                        // token 指向缓冲区内部：直接把视图交给调用方（零拷贝）。
                        m_token = result.token;
                        m_start += result.advance;
                        return true;
                    }
                }
                if (result.advance > 0) {
                    m_start += result.advance;
                    continue;
                }
            }
            // step5: 窗口耗尽：已 EOF 就正常结束，否则补数据后再切
            if (m_err) {
                // 已经到流末尾且窗口里再也切不出 token：正常结束。
                Shutdown();
                if (go2cpp::Is(m_err, streams::IoEof())) {
                    m_err = {};
                }
                return false;
            }
            if (!ReadMore()) {
                // 缓冲已到上限仍切不出 token：ErrTooLong（或空闲读超限）。
                Shutdown();
                return false;
            }
        }
    }

    /// 当前 token 的拷贝（Go 的 `Scanner.Text`）。
    std::string Text() const {
        if (m_token.empty()) {
            return std::string{};
        }
        return std::string(reinterpret_cast<const char*>(m_token.data()),
                           m_token.size());
    }

    /// 当前 token 的零拷贝视图（Go 的 `Scanner.Bytes`）；下次 `Scan` 后失效。
    ByteSpan Bytes() const noexcept { return m_token; }

    /// 终止原因；正常结束为空。
    const ErrorPtr& Err() const noexcept { return m_err; }

    /**
     * @brief 对应 Go `Scanner.Buffer` 的上限部分：设置单 token 最大字节数。
     * @param max_token_size 新上限；小于当前缓冲尺寸时提升到缓冲尺寸。
     * @note Go 的 `Buffer(buf, max)` 还允许调用方提供缓冲区复用；本实现的缓冲由
     *       Scanner 独占（`std::vector`），因此只保留 `max` 语义，不复用外部内存
     *       （见文件头「已知未支持项」）。
     */
    void Buffer(std::size_t max_token_size) noexcept {
        m_max_token_size = std::max(m_buffer.size(), max_token_size);
    }

    /// 当前单 token 上限。
    std::size_t MaxTokenSize() const noexcept { return m_max_token_size; }

    /// 替换切分函数（默认 `ScanLines`）。
    void SetSplit(SplitFunc split) { m_split = std::move(split); }

    /// 当前切分函数。
    const SplitFunc& Split() const noexcept { return m_split; }

    /// 初始缓冲尺寸（不含后续增长）。
    std::size_t BufferSize() const noexcept {
        return m_initial_buffer_size;
    }

private:
    /// 记录第一个错误（EOF 可以被真实错误覆盖，与 Go 的 `setErr` 一致）。
    void SetErr(const ErrorPtr& error) {
        if (!m_err || go2cpp::Is(m_err, streams::IoEof())) {
            m_err = error;
        }
    }

    /// 终止后清空窗口：保留尺寸，丢弃内容。
    void Shutdown() {
        m_start = 0;
        m_end = 0;
        m_done = true;
    }

    /**
     * @brief 压缩窗口并读入更多数据；必要时扩容。
     * @return false 表示「无法再前进」——`ErrTooLong` 已置入 `Err()`。
     *
     * ## 步骤
     * step1. 把 `[m_start, m_end)` 的未切分数据搬到缓冲头部，游标归零；
     * step2. 窗口占满缓冲时翻倍扩容，夹到 `m_max_token_size`；无法再扩 →
     *        `ErrTooLong()`；
     * step3. 读入剩余空间；有数据先入窗口，错误记住、留待本轮以 atEOF 再切；
     * step4. 0 字节无错误读累计到 100 次 → `IoErrNoProgress()`。
     */
    bool ReadMore() {
        // step1: 压缩窗口，丢弃已切分的前缀
        if (m_start > 0) {
            const std::size_t remaining = m_end - m_start;
            if (remaining > 0) {
                std::memmove(m_buffer.data(), m_buffer.data() + m_start, remaining);
            }
            m_start = 0;
            m_end = remaining;
        }
        // step2: 窗口占满，按翻倍策略扩容；到达上限则报 ErrTooLong
        if (m_end == m_buffer.size()) {
            if (m_buffer.size() >= m_max_token_size) {
                SetErr(ErrTooLong());
                return false;
            }
            const std::size_t doubled = m_buffer.size() * 2;
            const std::size_t next =
                doubled < m_max_token_size ? doubled : m_max_token_size;
            if (next <= m_buffer.size()) {
                SetErr(ErrTooLong());
                return false;
            }
            m_buffer.resize(next);
        }
        const std::size_t room = m_buffer.size() - m_end;
        if (room == 0) {
            SetErr(ErrTooLong());
            return false;
        }
        // step3: 一次底层读；数据先入窗口，错误延后交付
        const streams::ReadResult result = m_source->Read(
            reinterpret_cast<char*>(m_buffer.data() + m_end), room);
        if (result.count > 0) {
            m_end += std::min(result.count, room);
            m_empty_reads = 0;
            // 数据与错误同时返回时，错误先记住；本轮循环会以 atEOF=true 再切一次。
            if (result.error) {
                m_err = result.error;
            }
            return true;
        }
        if (result.error) {
            m_err = result.error;
            return true;
        }
        // step4: 连续无进展读达到上限，判定底层违约
        if (++m_empty_reads >= kMaxConsecutiveEmptyReads) {
            m_err = streams::IoErrNoProgress();
        }
        return true;
    }

    ReaderT* m_source;                    ///< 底层 Reader（不拥有）；仅 ReadMore() 调用其 Read()
    std::vector<std::uint8_t> m_buffer;   ///< Scanner 独占的窗口缓冲；ReadMore() 压缩/扩容，Scan() 在 [m_start, m_end) 上切分
    std::size_t m_initial_buffer_size{0}; ///< 构造时的初始尺寸；只被 BufferSize() 读取，扩容后不变
    std::size_t m_max_token_size{kMaxScanTokenSize}; ///< 单 token 上限；构造/Buffer() 设置，ReadMore() 据此决定扩容上界
    std::size_t m_start{0};  ///< 窗口起点（尚未切分的位置）；Scan() 推进、ReadMore() 压缩时归零
    std::size_t m_end{0};    ///< 窗口终点（缓冲区已读到的位置）；ReadMore()/Scan() 推进
    std::size_t m_empty_reads{0}; ///< 连续「0 字节无错误」读次数；ReadMore() 累加、读到数据清零，达 100 置 IoErrNoProgress
    bool m_done{false};      ///< 终止标记；Shutdown() 置真，Scan() 首先检查；置真后不再读底层
    ByteSpan m_token{};      ///< 当前 token 视图（指向 m_buffer）；Scan() 写、Text()/Bytes() 读，下次 Scan 清空
    ErrorPtr m_err;          ///< 终止原因（正常 EOF 时被清空）；SetErr()/ReadMore()/Scan() 写，Err() 读
    SplitFunc m_split;       ///< 切分函数；SetSplit() 写、Scan() 调用；默认 ScanLines
};

}  // namespace go2cpp::bufio
