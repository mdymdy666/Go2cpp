/**
 * @file http_tunnel.cpp
 * @brief HTTP 批式隧道实现。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/app/http_tunnel.hpp"

#include <algorithm>
#include <utility>

namespace go2cpp::app {

// ---------------------------------------------------------------------------
// 客户端传输
// ---------------------------------------------------------------------------

HttpTunnelClientTransport::HttpTunnelClientTransport(HttpExchange exchange)
    : m_exchange(std::move(exchange)) {}

bool HttpTunnelClientTransport::WriteAll(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return !m_closed;
    }
    if (data == nullptr || m_closed || !m_exchange) {
        return false;
    }
    m_out.insert(m_out.end(), data, data + size);
    return true;
}

int HttpTunnelClientTransport::Read(std::uint8_t* data, std::size_t capacity,
                                    std::uint32_t /*timeout_ms*/) {
    if (data == nullptr && capacity != 0) {
        return -1;
    }
    if (pending_in() == 0) {
        if (m_closed) {
            return -1;
        }
        if (m_out.empty()) {
            return 0;  // 没有待发数据，也就没有可交换的内容。
        }
        std::vector<std::uint8_t> response;
        if (!m_exchange(m_out, &response)) {
            m_closed = true;
            return -1;
        }
        m_out.clear();
        m_in = std::move(response);
        m_in_offset = 0;
        ++m_exchanges;
        if (m_in.empty()) {
            return 0;
        }
    }
    const std::size_t available = m_in.size() - m_in_offset;
    const std::size_t take = std::min(available, capacity);
    std::copy(m_in.begin() + static_cast<std::ptrdiff_t>(m_in_offset),
              m_in.begin() + static_cast<std::ptrdiff_t>(m_in_offset + take), data);
    m_in_offset += take;
    if (m_in_offset == m_in.size()) {
        m_in.clear();
        m_in_offset = 0;
    }
    return static_cast<int>(take);
}

void HttpTunnelClientTransport::Close() {
    m_closed = true;
    m_out.clear();
    m_in.clear();
    m_in_offset = 0;
}

bool HttpTunnelClientTransport::closed() const { return m_closed; }

std::string HttpTunnelClientTransport::Describe() const { return "http-tunnel-client"; }

// ---------------------------------------------------------------------------
// 服务端批式传输
// ---------------------------------------------------------------------------

class HttpTunnelServer::BatchTransport final : public ITransport {
   public:
    bool WriteAll(const std::uint8_t* data, std::size_t size) override {
        if (m_closed) {
            return false;
        }
        if (size != 0) {
            if (data == nullptr) {
                return false;
            }
            m_out.insert(m_out.end(), data, data + size);
        }
        return true;
    }

    int Read(std::uint8_t* data, std::size_t capacity, std::uint32_t /*timeout*/) override {
        if (m_offset >= m_in.size()) {
            return m_closed ? -1 : 0;
        }
        const std::size_t take = std::min(capacity, m_in.size() - m_offset);
        std::copy(m_in.begin() + static_cast<std::ptrdiff_t>(m_offset),
                  m_in.begin() + static_cast<std::ptrdiff_t>(m_offset + take), data);
        m_offset += take;
        return static_cast<int>(take);
    }

    void Close() override { m_closed = true; }
    bool closed() const override { return m_closed; }

    void Feed(const std::vector<std::uint8_t>& body) {
        m_in = body;
        m_offset = 0;
    }

    bool InputEmpty() const { return m_offset >= m_in.size(); }

    void ClearOutput() { m_out.clear(); }

    std::vector<std::uint8_t> TakeOutput() {
        std::vector<std::uint8_t> out = std::move(m_out);
        m_out.clear();
        return out;
    }

    std::size_t output_size() const { return m_out.size(); }

   private:
    std::vector<std::uint8_t> m_in;   ///< 本次请求 body。
    std::size_t m_offset = 0;         ///< 已读偏移。
    std::vector<std::uint8_t> m_out;  ///< 本次响应 body。
    bool m_closed = false;            ///< 是否关闭。
};

// ---------------------------------------------------------------------------
// 服务端会话
// ---------------------------------------------------------------------------

HttpTunnelServer::HttpTunnelServer(ConnectionConfig config) {
    auto transport = std::make_unique<BatchTransport>();
    m_transport = transport.get();
    m_conn = std::make_unique<Connection>(std::move(transport), Role::kServer,
                                          std::move(config));
    std::string error;
    // 服务端 preface + SETTINGS 先进入输出缓冲，随第一次响应发出。
    m_conn->Start(&error);
}

HttpTunnelServer::~HttpTunnelServer() = default;

void HttpTunnelServer::SetResourceSinkFactory(
    std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)> factory) {
    m_conn->SetResourceSinkFactory(std::move(factory));
}

void HttpTunnelServer::SetEventHandler(std::function<void(const Event&)> handler) {
    m_conn->SetEventHandler(std::move(handler));
}

/**
 * @brief 处理一次 HTTP 批量隧道请求：把请求体喂进内存传输并推进协议栈，返回待写出的响应字节。
 * @param body [in] 请求体（对端上行字节）——借用，仅本次调用有效；可为空（表示只取输出）。
 * @param max_steps [in] 单次调用最多推进的协议步数——步；0 表示不推进，仅返回现有输出。
 * @return 待写出的响应字节——调用方**必须**原样作为 HTTP 响应体发回；空表示本次没有可写数据。
 * ## 定位
 * 服务端隧道唯一驱动器：把无状态的 HTTP 请求/响应映射到有状态的连接协议上。
 * 写错会让一次请求里永不返回（协议栈空转），或让病态输入把 worker 内存撑爆。
 * ## 步骤
 * step1. 计请求数；连接已关闭时直接取走并返回残留输出；
 * step2. 把请求体 Feed 进内存传输，作为协议栈的上行输入；
 * step3. 最多循环 max_steps 次 Step(0)，每次检查是否有进展（输入消费/帧产出）；
 * step4. 无进展、输入耗尽且输出未增长时提前退出，避免空转；
 * step5. 输出超过 16MB 时提前截断本轮，防止病态输入造成内存膨胀；
 * step6. TakeOutput 取走全部待发字节返回。
 * ## 并发/失败语义
 * - 调用约束：每个 HttpTunnelServer 只能被单线程串行调用（内部 m_conn/m_transport 无锁）。
 * - 原子性：一次调用内完成「喂输入 → 推进 → 取输出」，中间状态不对外可见。
 * - 失败语义：不抛异常；Step 返回负值视为连接不可用，直接返回已产生的输出。
 * - 不变量：返回的字节全部来自本连接输出缓冲；调用后缓冲已清空。
 */
std::vector<std::uint8_t> HttpTunnelServer::HandleRequest(
    const std::vector<std::uint8_t>& body, std::uint32_t max_steps) {
    // step1: 先计一次请求；连接已关闭时不再喂输入，只把残留输出交给调用方写回。
    ++m_handled_requests;
    if (m_conn->closed()) {
        return m_transport->TakeOutput();
    }
    // step2: Feed 只做入队；真正的协议推进在下面的 Step(0) 循环里。
    m_transport->Feed(body);
    // step3: 有预算才推进；每轮用 before/size 判断是否真的产生了进展。
    for (std::uint32_t i = 0; i < max_steps; ++i) {
        const std::size_t before = m_transport->output_size();
        const int frames = m_conn->Step(0);
        if (frames < 0) {
            break;
        }
        if (m_transport->InputEmpty() && frames == 0 &&
            m_transport->output_size() == before) {
            break;
        }
        if (m_transport->InputEmpty() && m_transport->output_size() > 16u * 1024u * 1024u) {
            break;  // 单次响应上限，防止病态输入让 worker 内存膨胀。
        }
    }
    // step6: 一次性取走本轮产生的全部输出，输出缓冲随之清空。
    return m_transport->TakeOutput();
}

}  // namespace go2cpp::app
