/**
 * @file app_server.cpp
 * @brief ChatServer 的实现：命令分发、房间广播、私聊、心跳与持久化。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "app_server.h"

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/link_server.hpp"
#include "logger.h"

namespace chat {
namespace {

using go2cpp::bento::LinkReliability;
using go2cpp::bento::LinkSendResult;

constexpr std::uint8_t kChatChannel = 1;
constexpr std::size_t kReplayScan = 200;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

const char* SendResultName(LinkSendResult result) {
    return go2cpp::bento::LinkSendResultName(result);
}

}  // namespace

ChatServer::ChatServer(Config config)
    : m_config(std::move(config)),
      m_sessions(m_config.reconnect_grace_ms),
      m_store(m_config.history_file),
      m_wake(go2cpp::MakeChannel<int>(1)) {}

ChatServer::~ChatServer() { Stop(); }

bool ChatServer::Start(std::string* error) {
    go2cpp::bento::LinkServerConfig config;
    config.hub.bind_address = m_config.host;
    config.hub.bind_port = m_config.port;
    config.hub.max_peers = m_config.max_peers;
    config.hub.heartbeat_interval_ms = m_config.heartbeat_ms;
    config.hub.peer_timeout_ms = m_config.peer_timeout_ms;
    config.hub.require_encryption = false;  // 本机示例走明文，生产必须开启
    config.poll_timeout_ms = 2;
    if (!m_server.Listen(m_config.host, m_config.port, config, error)) {
        return false;
    }
    m_server.OnPeerConnected([this](PeerId peer) { OnConnected(peer); });
    m_server.OnPeerDisconnected(
        [this](PeerId peer, const char* reason) { OnDisconnected(peer, reason); });
    m_server.OnStream([this](PeerId peer, std::uint8_t channel, const std::uint8_t* data,
                             std::size_t bytes) { OnMessage(peer, channel, data, bytes); });
    if (!m_server.Start()) {
        if (error != nullptr) *error = "LinkServer::Start 失败";
        return false;
    }
    m_running.store(true, std::memory_order_release);
    m_scheduler.start();
    m_pinger = m_scheduler.spawn([this] { PingLoop(); });
    LogInfo("chat_server_full 监听 udp://" + m_config.host + ":" +
            std::to_string(m_server.local_port()));
    LogInfo("会话宽限 " + std::to_string(m_config.reconnect_grace_ms) + "ms，历史文件 " +
            m_store.path());
    return true;
}

void ChatServer::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        m_server.Stop();
        return;
    }
    (void)m_wake->TrySend(1);
    if (m_pinger) {
        (void)m_pinger->wait_for(std::chrono::seconds(2));
        m_pinger.reset();
    }
    m_server.Stop();
    m_scheduler.shutdown();
    LogInfo("已关闭，最终在线 " + std::to_string(m_sessions.online_count()) + "，离线 token " +
            std::to_string(m_sessions.offline_count()));
    ShutdownLogger();
}

std::uint16_t ChatServer::port() const { return m_server.local_port(); }
std::size_t ChatServer::online_count() const { return m_sessions.online_count(); }

void ChatServer::OnConnected(PeerId peer) {
    LogInfo("peer " + std::to_string(peer) + " 已连接");
    SendTo(peer, FormatSystem("欢迎，请发送 JOIN <昵称> 或 HELLO <token> 恢复会话"));
}

void ChatServer::OnDisconnected(PeerId peer, const char* reason) {
    const auto session = m_sessions.Leave(peer, NowMs());
    if (!session) {
        LogInfo("peer " + std::to_string(peer) + " 断开（未加入会话），reason=" + reason);
        return;
    }
    const std::string notice =
        "* " + session->name + " 断线（" + std::to_string(m_config.reconnect_grace_ms / 1000) +
        " 秒内可 HELLO " + session->token + " 恢复）";
    BroadcastRoom(session->room, FormatSystem(notice));
    LogInfo("peer " + std::to_string(peer) + " 会话 " + session->name + " 断开，reason=" + reason);
}

void ChatServer::OnMessage(PeerId peer, std::uint8_t channel, const std::uint8_t* data,
                           std::size_t bytes) {
    if (channel != kChatChannel) {
        return;
    }
    if (bytes > m_config.max_message_bytes) {
        SendTo(peer, FormatError("消息超过 " + std::to_string(m_config.max_message_bytes) +
                                 " 字节"));
        return;
    }
    const std::string text(reinterpret_cast<const char*>(data), bytes);
    Command command;
    if (!ParseCommand(text, &command)) {
        SendTo(peer, FormatError("无法解析命令（空行或非法 verb）"));
        return;
    }
    m_sessions.Touch(peer, NowMs());
    Handle(peer, command);
}

void ChatServer::Handle(PeerId peer, const Command& command) {
    if (command.verb == "PING") {
        SendTo(peer, "PONG");
        return;
    }
    if (command.verb == "PONG") {
        return;
    }
    if (command.verb == "HELLO") {
        HandleHello(peer, command.rest);
        return;
    }
    if (command.verb == "JOIN") {
        HandleJoin(peer, command.rest);
        return;
    }
    if (!m_sessions.Get(peer)) {
        SendTo(peer, FormatError("尚未加入，请先 JOIN <昵称>"));
        return;
    }
    if (command.verb == "ROOM") {
        HandleRoom(peer, command.rest);
    } else if (command.verb == "SAY") {
        HandleSay(peer, command.rest);
    } else if (command.verb == "DM") {
        HandleDirect(peer, command.rest);
    } else if (command.verb == "WHO") {
        HandleWho(peer);
    } else if (command.verb == "ROOMS") {
        HandleRooms(peer);
    } else if (command.verb == "QUIT") {
        HandleQuit(peer);
    } else {
        SendTo(peer, FormatError("未知命令: " + command.verb));
    }
}

void ChatServer::HandleJoin(PeerId peer, const std::string& arguments) {
    std::string name;
    if (!SplitFirst(arguments, &name, nullptr) || !IsSafeName(name)) {
        SendTo(peer, FormatError("昵称必须是 1..32 字节且不含空白"));
        return;
    }
    const auto result = m_sessions.Join(peer, name, "", NowMs());
    if (!result.ok) {
        SendTo(peer, FormatError(result.error));
        return;
    }
    SendTo(peer, FormatToken(result.token));
    SendTo(peer, FormatOk("已加入 " + result.session.room + "，昵称 " + result.session.name));
    BroadcastRoom(result.session.room,
                  FormatSystem("* " + result.session.name + " 加入了 " + result.session.room));
    Replay(peer, result.session.room);
    LogInfo("peer " + std::to_string(peer) + " JOIN " + result.session.name + "，在线 " +
            std::to_string(m_sessions.online_count()));
}

void ChatServer::HandleHello(PeerId peer, const std::string& arguments) {
    std::string token;
    if (!SplitFirst(arguments, &token, nullptr)) {
        SendTo(peer, FormatError("用法: HELLO <token>"));
        return;
    }
    const auto session = m_sessions.Resume(token, peer, NowMs());
    if (!session) {
        SendTo(peer, FormatError("token 无效或已过期，请 JOIN <昵称>"));
        return;
    }
    SendTo(peer, FormatToken(session->token));
    SendTo(peer, FormatOk("会话已恢复: " + session->name + " @" + session->room));
    BroadcastRoom(session->room, FormatSystem("* " + session->name + " 重新连接"));
    Replay(peer, session->room);
    LogInfo("peer " + std::to_string(peer) + " HELLO 恢复 " + session->name);
}

void ChatServer::HandleRoom(PeerId peer, const std::string& arguments) {
    std::string room;
    if (!SplitFirst(arguments, &room, nullptr) || !IsSafeRoom(room)) {
        SendTo(peer, FormatError("房间名必须是 1..24 字节的字母/数字/_-"));
        return;
    }
    const auto before = m_sessions.Get(peer);
    if (!before) {
        return;
    }
    const std::string previous = before->room;
    if (previous == room) {
        SendTo(peer, FormatOk("已经在房间 " + room));
        return;
    }
    const auto after = m_sessions.SetRoom(peer, room, NowMs());
    if (!after) {
        return;
    }
    BroadcastRoom(previous, FormatSystem("* " + after->name + " 离开了 " + previous));
    BroadcastRoom(room, FormatSystem("* " + after->name + " 进入了 " + room));
    SendTo(peer, FormatOk("已进入房间 " + room));
    Replay(peer, room);
    LogInfo("peer " + std::to_string(peer) + " ROOM " + previous + " -> " + room);
}

void ChatServer::HandleSay(PeerId peer, const std::string& arguments) {
    const auto session = m_sessions.Get(peer);
    if (!session) {
        return;
    }
    const std::string text = Trim(arguments);
    if (text.empty()) {
        SendTo(peer, FormatError("用法: SAY <文本>"));
        return;
    }
    BroadcastRoom(session->room, FormatRoomMessage(session->room, session->name, text));
    Record(session->room, session->name, text);
}

void ChatServer::HandleDirect(PeerId peer, const std::string& arguments) {
    const auto session = m_sessions.Get(peer);
    if (!session) {
        return;
    }
    std::string target;
    std::string text;
    if (!SplitFirst(arguments, &target, &text) || text.empty()) {
        SendTo(peer, FormatError("用法: DM <昵称> <文本>"));
        return;
    }
    const auto destination = m_sessions.FindByName(target);
    if (!destination) {
        SendTo(peer, FormatError("用户不在线: " + target));
        return;
    }
    SendTo(destination->peer, FormatDirectMessage(session->name, text));
    SendTo(peer, FormatOk("私聊已送达 " + target));
    Record("@dm:" + session->name + "->" + target, session->name, text);
    LogInfo("DM " + session->name + " -> " + target + " (" + std::to_string(text.size()) + "B)");
}

void ChatServer::HandleWho(PeerId peer) {
    std::vector<std::string> names;
    for (const auto& session : m_sessions.Online()) {
        names.push_back(session.name);
    }
    SendTo(peer, FormatOnline(names));
}

void ChatServer::HandleRooms(PeerId peer) { SendTo(peer, FormatRooms(m_sessions.Rooms())); }

void ChatServer::HandleQuit(PeerId peer) {
    const auto session = m_sessions.Leave(peer, NowMs());
    if (!session) {
        return;
    }
    BroadcastRoom(session->room, FormatSystem("* " + session->name + " 离开了聊天室"));
    SendTo(peer, FormatOk("已退出（token " + session->token + " 保留 " +
                          std::to_string(m_config.reconnect_grace_ms / 1000) + " 秒）"));
}

void ChatServer::SendTo(PeerId peer, const std::string& text) {
    const auto result =
        m_server.Send(peer, kChatChannel, text.data(), text.size(), LinkReliability::kReliable);
    if (result != LinkSendResult::kOk) {
        LogDebug("发送失败 peer=" + std::to_string(peer) + " text=" + text + " result=" +
                 SendResultName(result));
    }
}

void ChatServer::BroadcastRoom(const std::string& room, const std::string& text) {
    for (const auto& session : m_sessions.InRoom(room)) {
        SendTo(session.peer, text);
    }
}

void ChatServer::Record(const std::string& room, const std::string& from,
                        const std::string& text) {
    StoredMessage message;
    message.epoch_ms = NowMs();
    message.room = room;
    message.from = from;
    message.text = text;
    std::string error;
    if (!m_store.Append(message, &error)) {
        LogWarn(error);
    }
}

void ChatServer::Replay(PeerId peer, const std::string& room) {
    if (m_config.replay_lines == 0) {
        return;
    }
    std::vector<StoredMessage> matches;
    for (const auto& message : m_store.Recent(kReplayScan)) {
        if (message.room == room) {
            matches.push_back(message);
        }
    }
    const std::size_t begin = matches.size() > m_config.replay_lines
                                  ? matches.size() - m_config.replay_lines
                                  : 0;
    for (std::size_t index = begin; index < matches.size(); ++index) {
        const auto& message = matches[index];
        SendTo(peer, FormatHistory(message.room, message.from, message.text));
    }
}

void ChatServer::PingLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        const auto woken = m_wake->RecvFor(std::chrono::milliseconds(m_config.app_ping_interval_ms));
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (woken.Ok()) {
            continue;
        }
        const std::uint64_t now = NowMs();
        for (const auto& session : m_sessions.Online()) {
            SendTo(session.peer, "PING");
        }
        for (const auto& expired : m_sessions.ExpireIdle(now, m_config.app_session_timeout_ms)) {
            BroadcastRoom(expired.room,
                          FormatSystem("* " + expired.name + " 心跳超时，已移出在线列表"));
            LogWarn("会话心跳超时: " + expired.name + "（token 保留可恢复）");
        }
        const std::size_t purged = m_sessions.PurgeOffline(now);
        if (purged != 0) {
            LogInfo("清理过期 token " + std::to_string(purged) + " 个");
        }
    }
}

}  // namespace chat
