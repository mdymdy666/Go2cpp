/**
 * @file main.cpp
 * @brief chat_server 简单版：单文件 LinkServer + 内置客户端，多客户端广播。
 * @details 服务端使用 bento::LinkServer（UDP 可靠通道），客户端使用同协议的
 *          bento::PeerHub；两者可互连，因此一个可执行文件就能自测：
 *
 *            ./go2cpp_chat_server_simple                       # 默认服务端 :9100
 *            ./go2cpp_chat_server_simple --server --port 9100
 *            ./go2cpp_chat_server_simple --client alice        # 另开终端
 *
 *          应用协议（UTF-8 文本，可靠通道 1）：
 *            JOIN <name>     登记昵称（不登记则显示 peer-<id>）
 *            SAY <text>      发消息；裸文本等价于 SAY
 *            WHO             单播在线列表
 *            /quit           客户端退出
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/bento/net/link_server.hpp"
#include "go2cpp/bento/net/peer_hub.hpp"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using go2cpp::bento::LinkPeerId;
using go2cpp::bento::LinkReliability;
using go2cpp::bento::LinkServer;
using go2cpp::bento::LinkServerConfig;
using go2cpp::bento::PeerHub;
using go2cpp::bento::PeerHubConfig;

constexpr std::uint8_t kChatChannel = 1;
constexpr std::size_t kMaxTextBytes = 1024;
constexpr std::uint32_t kClientPeerId = 1;

/// @brief 去掉首尾空白；消息来自网络，必须防住全空白输入。
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// @brief 把 peer 名与广播封装在一起；回调都在接收线程执行。
class ChatServer final {
   public:
    bool Start(const std::string& host, std::uint16_t port, std::string* error) {
        LinkServerConfig config;
        config.hub.bind_address = host;
        config.hub.bind_port = port;
        config.hub.max_peers = 32;
        config.hub.heartbeat_interval_ms = 500;
        config.hub.peer_timeout_ms = 10000;
        config.hub.require_encryption = false;  // 本机示例走明文，生产必须开启
        config.poll_timeout_ms = 5;
        if (!m_server.Listen(host, port, config, error)) {
            return false;
        }
        m_server.OnPeerConnected([this](LinkPeerId peer) {
            SendText(peer, "* 欢迎加入，发送 JOIN <name> 登记昵称");
        });
        m_server.OnPeerDisconnected(
            [this](LinkPeerId peer, const char*) { Announce(peer, "离开"); });
        m_server.OnStream([this](LinkPeerId peer, std::uint8_t channel,
                                 const std::uint8_t* data, std::size_t bytes) {
            if (channel == kChatChannel) {
                Handle(peer, std::string(reinterpret_cast<const char*>(data), bytes));
            }
        });
        return m_server.Start();
    }

    void Stop() { m_server.Stop(); }
    std::uint16_t port() const { return m_server.local_port(); }

   private:
    void SendText(LinkPeerId peer, const std::string& text) {
        (void)m_server.Send(peer, kChatChannel, text.data(), text.size());
    }

    void Announce(LinkPeerId peer, const char* action) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_names.find(peer);
            name = (found == m_names.end()) ? ("peer-" + std::to_string(peer)) : found->second;
            m_names.erase(peer);  // 离开后不再保留，重新加入必须重新 JOIN
        }
        const std::string message = "* " + name + " " + action;
        m_server.Broadcast(kChatChannel, message.data(), message.size());
    }

    void Handle(LinkPeerId peer, std::string text) {
        text = Trim(std::move(text));
        if (text.empty()) {
            return;
        }
        if (text.size() > kMaxTextBytes) {
            SendText(peer, "* 消息过长（上限 1024 字节）");
            return;
        }
        if (text.rfind("JOIN ", 0) == 0) {
            Join(peer, Trim(text.substr(5)));
            return;
        }
        if (text == "WHO") {
            SendText(peer, Who());
            return;
        }
        if (text.rfind("SAY ", 0) == 0) {
            text = text.substr(4);
        }
        Chat(peer, text);
    }

    void Join(LinkPeerId peer, const std::string& name) {
        if (name.empty() || name.size() > 32) {
            SendText(peer, "* 昵称长度必须是 1..32 字节");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_names[peer] = name;
        }
        const std::string message = "* " + name + " 加入聊天室";
        m_server.Broadcast(kChatChannel, message.data(), message.size());
        SendText(peer, "* 已登记为 " + name);
    }

    void Chat(LinkPeerId peer, const std::string& text) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_names.find(peer);
            name = (found == m_names.end()) ? ("peer-" + std::to_string(peer)) : found->second;
        }
        const std::string line = name + ": " + text;
        m_server.Broadcast(kChatChannel, line.data(), line.size());
    }

    std::string Who() {
        std::string list = "* 在线:";
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_names) {
            list += " " + entry.second;
        }
        return list;
    }

    LinkServer m_server;
    mutable std::mutex m_mutex;                 ///< 保护 m_names（回调线程写，主线程不读）。
    std::map<LinkPeerId, std::string> m_names;  ///< peer -> 昵称。
};

/// @brief 交互客户端：stdin 发送，后台线程 RunOnce 接收并打印。
class ChatClient final {
   public:
    explicit ChatClient(PeerHubConfig config) : m_hub(std::move(config)) {}

    bool Start(const std::string& host, std::uint16_t port, const std::string& name,
               std::string* error) {
        m_name = name;
        if (!m_hub.Open(error)) {
            return false;
        }
        m_hub.on_stream = [](std::uint32_t, std::uint8_t, const std::uint8_t* data,
                             std::size_t bytes) {
            std::cout << "[收到] " << std::string(reinterpret_cast<const char*>(data), bytes)
                      << std::endl;
        };
        if (!m_hub.AddPeer(kClientPeerId, host, port, error)) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_hub.peer_connected(kClientPeerId) &&
               std::chrono::steady_clock::now() < deadline) {
            m_hub.RunOnce(20);
        }
        if (!m_hub.peer_connected(kClientPeerId)) {
            if (error != nullptr) {
                *error = "握手超时（服务端未启动或端口不一致）";
            }
            return false;
        }
        return Send("JOIN " + name);
    }

    void Run() {
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
            // 启动时已自动 JOIN；脚本里再写一次同名 JOIN 时忽略，避免重复公告。
            if (line.rfind("JOIN ", 0) == 0 && Trim(line.substr(5)) == m_name) {
                continue;
            }
            if (!line.empty() && !Send(line)) {
                std::cerr << "[client] 发送失败（连接已断开？）\n";
                break;
            }
        }
        m_running.store(false, std::memory_order_release);
        m_pump.join();
        (void)m_hub.RemovePeer(kClientPeerId);
        m_hub.Close();
    }

   private:
    bool Send(const std::string& text) {
        return m_hub.SendStream(kClientPeerId, kChatChannel, text.data(), text.size());
    }

    PeerHub m_hub;
    std::string m_name;
    std::thread m_pump;
    std::atomic<bool> m_running{true};
};

}  // namespace

int main(int argc, char** argv) {
    bool client_mode = false;
    std::string name = "guest";
    std::string host = "127.0.0.1";
    std::uint16_t port = 9100;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--client" && index + 1 < argc) {
            client_mode = true;
            name = argv[++index];
        } else if (argument == "--server") {
            client_mode = false;
        } else if (argument == "--port" && index + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++index]));
        } else if (argument == "--host" && index + 1 < argc) {
            host = argv[++index];
        } else {
            std::cerr << "用法: " << argv[0]
                      << " [--server|--client <name>] [--host 127.0.0.1] [--port 9100]\n";
            return 2;
        }
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "pthread_sigmask 失败\n";
        return 1;
    }

    if (client_mode) {
        PeerHubConfig config;
        config.bind_address = "127.0.0.1";
        config.bind_port = 0;
        config.require_encryption = false;
        config.heartbeat_interval_ms = 500;
        config.peer_timeout_ms = 10000;
        ChatClient client(config);
        std::string error;
        if (!client.Start(host, port, name, &error)) {
            std::cerr << "[chat_simple][client] 连接失败: " << error << "\n";
            return 1;
        }
        std::cout << "[chat_simple][client] 已连接，昵称 " << name
                  << "（输入 /quit 退出）\n";
        client.Run();
        return 0;
    }

    ChatServer server;
    std::string error;
    if (!server.Start(host, port, &error)) {
        std::cerr << "[chat_simple][server] 启动失败: " << error << "\n";
        return 1;
    }
    std::cout << "[chat_simple][server] 监听 udp://" << host << ":" << server.port() << "\n";
    int signal_number = 0;
    if (sigwait(&signals, &signal_number) != 0) {
        std::cerr << "[chat_simple][server] sigwait 失败\n";
    }
    std::cout << "[chat_simple][server] 收到信号 " << signal_number << "，关闭\n";
    server.Stop();
    std::cout << "[chat_simple][server] 已退出\n";
    return 0;
}
