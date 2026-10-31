#pragma once

/**
 * @file stun_server.hpp
 * @brief 可独立部署的 STUN echo 服务器（RFC 5389 Binding + RFC 5780 扩展子集）。
 * @details 服务器存在的意义有两个：
 *
 *          1. **生产**：在没有现成 STUN 服务的局域网/私有云里直接启动一个，
 *             供 P2PChannel 收集 srflx 候选；
 *          2. **测试**：本模块的 NAT 类型检测依赖 CHANGE-REQUEST 与双 IP
 *             地址，公网 STUN 服务通常不保证这两个能力。自写服务器可以在
 *             本机用 `127.0.0.1` + `127.0.0.2`（Linux 整个 127/8 都在 lo 上）
 *             完整验证四类 NAT 判定，不依赖外网。
 *
 *          服务器同时打开最多 4 个 UDP socket：
 *
 *          | 编号 | 地址                         | 用途                     |
 *          |------|------------------------------|--------------------------|
 *          | 0    | primary IP : primary port    | 普通请求                 |
 *          | 1    | primary IP : alternate port  | CHANGE-REQUEST(port)     |
 *          | 2    | alternate IP : primary port  | CHANGE-REQUEST(ip)       |
 *          | 3    | alternate IP : alternate port| CHANGE-REQUEST(ip+port)  |
 *
 *          当未配置 alternate 时只开 socket 0；当 alternate 与 primary 同 IP
 *          时只开 0/1，且对「换 IP」的请求**不响应**（而不是用主 socket 回，
 *          否则 NAT 检测会误判为 Full Cone）。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/p2p/endpoint.hpp"

namespace go2cpp::p2p {

struct StunServerConfig {
    /// 主监听地址；端口 0 表示由内核分配。
    Endpoint primary = Endpoint::Loopback(3478);
    /**
     * @brief 备用地址；无效表示不提供 CHANGE-REQUEST 能力。
     * @note 要支持 RFC 3489 的 Full Cone 判定，备用地址必须是**另一个 IP**
     *       （例如 primary=127.0.0.1:P1、alternate=127.0.0.2:P2）。
     */
    Endpoint alternate;
    /// SOFTWARE 属性值。
    std::string software = "go2cpp-stun/1.0";
    /// 是否处理 CHANGE-REQUEST；关闭时一律从主 socket 响应。
    bool support_change_request = true;
    /// 接收缓冲区大小。
    std::size_t receive_buffer_bytes = 1U << 20U;
};

/**
 * @brief 单线程 poll 循环的 STUN 服务器。
 *
 * @details 线程在 `Start()` 时创建、`Stop()` 时 join；`Stop()` 幂等。
 *          服务器不校验 MESSAGE-INTEGRITY，也不做认证——它只回答地址发现
 *          请求，不承载任何用户数据。
 */
class StunServer {
public:
    explicit StunServer(StunServerConfig config = {});
    ~StunServer();

    StunServer(const StunServer&) = delete;
    StunServer& operator=(const StunServer&) = delete;

    /**
     * @brief 绑定所有 socket 并启动接收线程。
     * @param error 失败时写入原因（可为空）。
     * @return 是否启动成功；重复调用已启动的服务器返回 true。
     */
    bool Start(std::string* error = nullptr);

    /// 停止接收线程并关闭全部 socket；可重复调用。
    void Stop();

    bool started() const noexcept;

    /// 主监听端点（端口 0 已被解析为内核分配的实际端口）。
    Endpoint primary_endpoint() const;

    /// 备用端点；未配置时返回无效端点。
    Endpoint alternate_endpoint() const;

    /// 已处理的合法 Binding 请求数。
    std::uint64_t requests_served() const noexcept;

    /// 已发送的响应数（含错误响应；CHANGE-REQUEST 无法满足时不发送）。
    std::uint64_t responses_sent() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace go2cpp::p2p
