/**
 * @file splice_pipe.hpp
 * @brief Linux `splice(2)` 零拷贝双向转发：socket ⇄ socket，不经过用户态缓冲。
 *
 * @details 与 `RelayPipe` 的语义等价（双向、背压、写 deadline、统计），区别只在
 *          数据面：`RelayPipe` 每次 `Read` 把内核缓冲拷进用户态再 `Write` 拷回
 *          内核；本模块用「socket -> pipe -> socket」两次 `splice`，数据始终留在
 *          内核页里，用户态只搬长度。
 *
 * ```
 *   a_fd --splice--> [pipe] --splice--> b_fd
 *   a_fd <--splice-- [pipe] <--splice-- b_fd
 * ```
 *
 * ## 适用条件
 *
 * 两端都必须是 `splice` 支持的 fd（socket / pipe）。任何一端是用户态流（如
 * tunnel 的 `MuxStream`）都无法使用——此时调用方必须回退 `RelayPipe`，
 * `Stream::NativeFd()` 返回 -1 就是回退信号。
 *
 * ## 背压与超时
 *
 * 没有用户态队列：源端 `splice` 读满管道后返回 `EAGAIN`，线程停在 `poll` 上，
 * 读速度天然不快于写速度（背压）。目的端 `poll(POLLOUT)` 超过
 * `write_timeout` 判定为慢 peer：本方向失败、`timeouts + 1`、`shutdown` 目的端，
 * 另一方向继续（与 `RelayPipe` 的隔离语义一致）。
 *
 * ## 非 Linux
 *
 * `Supported()` 返回 false，`Run()` 立即失败并给出原因；调用方应回退
 * `RelayPipe`。所有 `splice` 符号都在 `#if defined(__linux__)` 内。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace go2cpp {
namespace relay {

/// `SplicePipe` 的可调参数。
struct SpliceConfig {
    /// 管道容量目标（`F_SETPIPE_SZ`）；默认 1 MiB。设置失败时沿用内核默认。
    std::size_t pipe_bytes = 1U * 1024U * 1024U;
    /// 单次 `splice` 搬运上限；默认 256 KiB。
    std::size_t splice_chunk = 256U * 1024U;
    /// 单次目的端等待上限；超时即判定慢 peer。默认 30 s。
    std::chrono::milliseconds write_timeout{30000};
};

/// 运行期累计统计；`Stats()` 返回某一时刻快照。
struct SpliceStats {
    /// a -> b 已推入目的端的字节数。
    std::uint64_t a_to_b = 0;
    /// b -> a 已推入目的端的字节数。
    std::uint64_t b_to_a = 0;
    /// deadline 事件数；每方向最多记一次。
    std::uint64_t timeouts = 0;
};

/**
 * @brief 双向 `splice` 转发器。
 *
 * ## 不变量
 * - 数据不进入用户态：全程只有 `splice` 与 `poll`；
 * - 每个方向独立失败、独立统计；EOF 时只 `shutdown(目的端, SHUT_WR)`；
 * - `Run()` 至多成功进入一次，返回后所有线程已回收、管道已关闭。
 */
class SplicePipe final {
public:
    /**
     * @brief 构造并归一化配置。
     * @param a_fd [in] A 端 fd（不拥有；必须存活到 `Run` 返回）。
     * @param b_fd [in] B 端 fd（不拥有；同上）。
     * @param cfg  [in] 参数；零值按 `SpliceConfig` 注释归一化。
     */
    SplicePipe(int a_fd, int b_fd, SpliceConfig cfg = {});

    /// 析构；调用方须保证 `Run()` 已返回。
    ~SplicePipe();

    SplicePipe(const SplicePipe&) = delete;
    SplicePipe& operator=(const SplicePipe&) = delete;
    SplicePipe(SplicePipe&&) = delete;
    SplicePipe& operator=(SplicePipe&&) = delete;

    /**
     * @brief 双向转发直到两端 EOF、任一方向失败或 `ctx` 取消。
     *
     * @param ctx [in] 取消上下文；空指针等价于 `Context::Background()`。
     * @param err [out] 首个失败原因；成功时为 null。可为 nullptr。
     * @return 两个方向都干净 EOF 时 true；否则 false。
     *
     * @note 阻塞调用，返回时两个方向线程均已回收。
     */
    bool Run(const ContextPtr& ctx, ErrorPtr* err);

    /// 便捷重载：永不取消的上下文。
    bool Run(ErrorPtr* err) { return Run(ContextPtr(), err); }

    /// 统计快照（原子读，运行期可调）。
    SpliceStats Stats() const;

    /// 归一化后的配置。
    const SpliceConfig& Config() const noexcept;

    /// 当前平台是否支持（编译期 + 运行期探测）。
    static bool Supported() noexcept;

    /// 不支持时的原因；支持时返回空串。
    static std::string UnsupportedReason();

private:
    struct State;
    std::shared_ptr<State> m_state;
};

/**
 * @brief 读环境变量 `GO2CPP_TUNNEL_SPLICE`（1/true/on/yes，大小写不敏感）。
 *
 * @return 开关状态；默认 false。仅在 `SplicePipe::Supported()` 为真时才有意义。
 */
bool SpliceEnabledFromEnv();

}  // namespace relay
}  // namespace go2cpp
