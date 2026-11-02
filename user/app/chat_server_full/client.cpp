/**
 * @file client.cpp
 * @brief ChatClient 的实现：连接、自动 PONG、TOKEN 捕获、stdin 发送。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "client.h"

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

#include "protocol.h"

namespace chat {
namespace {

constexpr std::uint8_t kChatChannel = 1;
constexpr std::uint32_t kClientPeerId = 1;

go2cpp::bento::PeerHubConfig MakeHubConfig() {
    go2cpp::bento::PeerHubConfig config;
    config.bind_address = "127.0.0.1";
    config.bind_port = 0;
    config.require_encryption = false;  // 与服务端的本机示例配置一致
    config.heartbeat_interval_ms = 500;
    config.peer_timeout_ms = 10000;
    config.handshake_timeout_ms = 5000;
    return config;
}

}  // namespace

ChatClient::ChatClient(Options options) : m_hub(MakeHubConfig()), m_options(std::move(options)) {}

ChatClient::~ChatClient() {
    m_running.store(false, std::memory_order_release);
    if (m_pump.joinable()) {
        m_pump.join();
    }
    (void)m_hub.RemovePeer(kClientPeerId);
    m_hub.Close();
}

bool ChatClient::Start(std::string* error) {
    if (!m_hub.Open(error)) {
        return false;
    }
    m_hub.on_stream = [this](std::uint32_t, std::uint8_t channel, const std::uint8_t* data,
                             std::size_t bytes) {
        if (channel == kChatChannel) {
            OnMessage(data, bytes);
        }
    };
    if (!m_hub.AddPeer(kClientPeerId, m_options.host, m_options.port, error)) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_hub.peer_connected(kClientPeerId) && std::chrono::steady_clock::now() < deadline) {
        m_hub.RunOnce(20);
    }
    if (!m_hub.peer_connected(kClientPeerId)) {
        if (error != nullptr) {
            *error = "握手超时（服务端未启动或端口不一致）";
        }
        return false;
    }
    if (!m_options.auto_join) {
        return true;
    }
    if (!m_options.token.empty()) {
        return Send("HELLO " + m_options.token);
    }
    if (!Send("JOIN " + m_options.name)) {
        if (error != nullptr) *error = "发送 JOIN 失败";
        return false;
    }
    // 短暂泵一轮，让欢迎语与 TOKEN 在 Run() 启动前到达。
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < settle) {
        m_hub.RunOnce(10);
    }
    return true;
}

void ChatClient::Run() {
    m_pump = std::thread([this] {
        while (m_running.load(std::memory_order_acquire)) {
            m_hub.RunOnce(20);
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "/quit") {
            break;
        }
        if (line.rfind("JOIN ", 0) == 0 && Trim(line.substr(5)) == m_options.name) {
            continue;  // 启动时已自动 JOIN
        }
        if (!line.empty() && !Send(line)) {
            std::cerr << "[client] 发送失败（连接已断开？）\n";
            break;
        }
    }
    m_running.store(false, std::memory_order_release);
    if (m_pump.joinable()) {
        m_pump.join();
    }
    (void)m_hub.RemovePeer(kClientPeerId);
    m_hub.Close();
}

std::string ChatClient::token() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_token;
}

bool ChatClient::Send(const std::string& text) {
    return m_hub.SendStream(kClientPeerId, kChatChannel, text.data(), text.size());
}

void ChatClient::SetToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_token = token;
}

void ChatClient::OnMessage(const std::uint8_t* data, std::size_t bytes) {
    const std::string text(reinterpret_cast<const char*>(data), bytes);
    if (text == "PING") {
        (void)Send("PONG");
        return;
    }
    if (text.rfind("TOKEN ", 0) == 0) {
        SetToken(Trim(text.substr(6)));
        std::cout << "[token] " << token() << std::endl;
    }
    std::cout << "[收到] " << text << std::endl;
}

}  // namespace chat
