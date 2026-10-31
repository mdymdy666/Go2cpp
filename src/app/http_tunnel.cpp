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

std::vector<std::uint8_t> HttpTunnelServer::HandleRequest(
    const std::vector<std::uint8_t>& body, std::uint32_t max_steps) {
    ++m_handled_requests;
    if (m_conn->closed()) {
        return m_transport->TakeOutput();
    }
    m_transport->Feed(body);
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
    return m_transport->TakeOutput();
}

}  // namespace go2cpp::app
