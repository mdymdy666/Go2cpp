# chat_server_full —— 多模块聊天服务（房间/私聊/心跳/重连/持久化）

在 `chat_server_simple` 的“广播 + 昵称”之上补齐聊天服务该有的能力：
房间、私聊、在线列表、应用层心跳、断线重连 token、消息持久化、INI 配置与
分级日志。服务端与测试客户端仍是同一个可执行文件。

## 功能

1. **会话管理**：`SessionTable` 维护三个不变量——在线昵称唯一、断线 token 在
   宽限期（默认 60 s）内可恢复、所有操作线程安全；
2. **房间**：`ROOM <name>` 切换，默认 `lobby`；房间内广播，其他房间收不到；
3. **私聊**：`DM <昵称> <文本>`，目标不在线返回 `ERROR`；
4. **在线列表**：`WHO` 返回当前在线昵称；
5. **心跳**：链路心跳 500 ms；应用层每 `app_ping_interval_ms` 发 `PING`，
   客户端自动 `PONG`；`app_session_timeout_ms` 内无活动则移出在线表并广播；
6. **重连**：`JOIN` 成功下发 `TOKEN <t>`；断线宽限内用 `HELLO <t>` 恢复到
   原昵称与房间；同一 token 的新连接会接管旧连接（UDP 没有 FIN，旧 peer 等
   链路层自然超时）；
7. **消息持久化**：`epoch_ms|room|from|text` 追加到文件；加入/恢复会话时回放
   该房间最近 `replay_lines` 条（`HIST` 前缀）；
8. **配置**：`server.ini` 的 `[chat]` / `[logging]`，命令行覆盖 host/port；
9. **日志**：debug/info/warn/error，stdout + 文件，记录 JOIN/DM/ROOM/超时/关闭；
10. **优雅关闭**：`SIGTERM` → 停心跳 Fiber → `LinkServer::Stop()`（触发断开
    回调）→ `Scheduler::shutdown()`。

## 协议

客户端 → 服务端（可靠通道 1，UTF-8，一行一条）：

| 命令 | 说明 |
| --- | --- |
| `JOIN <name>` | 首次加入；昵称 1..32 字节、不含空白 |
| `HELLO <token>` | 恢复会话（宽限期内有效） |
| `ROOM <room>` | 切换房间；房间名 1..24 字节 `[A-Za-z0-9_-]` |
| `SAY <text>` | 房间广播 |
| `DM <name> <text>` | 私聊 |
| `WHO` / `ROOMS` | 在线列表 / 房间列表 |
| `PING` / `PONG` | 应用层心跳 |
| `QUIT` | 主动离开（token 保留） |

服务端 → 客户端：`OK/ERROR/TOKEN/SYS/MSG/DM/WHO/ROOMS/HIST/PING/PONG`。
`MSG <room> <from> <text>`、`DM <from> <text>`、`HIST <room> <from> <text>`。

## 文件结构

```
chat_server_full/
├── main.cpp          # --server / --client 参数、信号、生命周期
├── config.h/.cpp     # INI → Config（先组装副本，全部成功才提交）
├── logger.h/.cpp     # 线程安全日志
├── protocol.h/.cpp   # 命令解析、昵称/房间校验、下行消息编码
├── session.h/.cpp    # 会话表：在线/昵称索引/离线 token/房间/超时
├── store.h/.cpp      # 消息持久化（追加 + 尾部回放）
├── app_server.h/.cpp # LinkServer 回调 → 命令分发 → 广播/私聊/持久化
├── client.h/.cpp    # 测试客户端：PeerHub + 自动 PONG + TOKEN 捕获
└── server.ini        # 示例配置
```

## 构建与运行

```sh
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_chat_server_full -j8

# 服务端（仓库根目录）
./.build-bento-wsl/example/app/go2cpp_chat_server_full --server
# 客户端
./.build-bento-wsl/example/app/go2cpp_chat_server_full --client alice
# 客户端输入：SAY 你好 / ROOM dev / SAY 房间消息 / DM bob 私聊 / WHO / ROOMS / PING / /quit
```

`--no-join` 只连接不 JOIN（测试“未加入”错误路径）；`--token <t>` 用
`HELLO` 恢复会话。默认配置见 `server.ini`，命令行 `--host/--port` 可覆盖。

## 测试

### 1) 基础多客户端

```sh
BIN=./.build-bento-wsl/example/app/go2cpp_chat_server_full
rm -f /tmp/go2cpp_chat_history.log
$BIN --server --port 19200 > /tmp/chat_full_server.log 2>&1 &
SPID=$!
sleep 0.8
{ printf 'SAY hello from alice\nDM nobody hi\nWHO\n'; sleep 12; } | \
  $BIN --client alice --port 19200 > /tmp/chat_full_alice.log 2>&1 &
sleep 0.5
{ printf 'SAY hi from bob\nDM alice private-hello\nROOM dev\nSAY dev-only\nWHO\nROOMS\n'; sleep 8; } | \
  $BIN --client bob --port 19200 > /tmp/chat_full_bob.log 2>&1 &
sleep 0.8
{ printf 'WHO\nROOMS\nSAY carol-here\n'; sleep 4; } | \
  $BIN --client carol --port 19200 > /tmp/chat_full_carol.log 2>&1 &
```

期望：

- alice 收到 `MSG lobby bob hi from bob`、`DM bob private-hello`，**不**收到 `dev-only`；
- bob 收到 `OK 已进入房间 dev` 与 `ROOMS dev lobby`；
- carol 加入时收到 `HIST lobby ...` 历史回放；
- alice 的 `DM nobody` 收到 `ERROR 用户不在线: nobody`。

### 2) 错误路径

```sh
# 重复昵称
{ sleep 1; } | $BIN --client alice --port 19200 | grep 'ERROR 昵称已被占用'
# 未 JOIN 就发消息
{ printf 'SAY before-join\n'; sleep 1; } | $BIN --client raw --no-join --port 19200 | grep 'ERROR 尚未加入'
# 未知命令 / 超长消息
{ printf 'FOO bar\n'; head -c 1500 /dev/zero | tr '\0' 'x' | sed 's/^/SAY /'; printf '\n'; sleep 2; } | \
  $BIN --client erin --port 19200 | grep -E 'ERROR (未知命令|消息超过)'
```

### 3) 重连（token）

```sh
# alice 退出后从日志提取 token
TOKEN=$(grep '^\[token\]' /tmp/chat_full_alice.log | head -1 | awk '{print $2}')
{ printf 'WHO\nSAY back after reconnect\n'; sleep 5; } | \
  $BIN --client alice --port 19200 --token "$TOKEN" > /tmp/chat_full_alice2.log 2>&1
grep 'OK 会话已恢复: alice @lobby' /tmp/chat_full_alice2.log
```

### 4) 持久化与关闭

```sh
grep -E '\|lobby\|alice\|hello from alice|\|@dm:bob->alice\|bob\|private-hello' \
  /tmp/go2cpp_chat_history.log
kill -TERM "$SPID"; wait "$SPID"; echo "server_exit=$?"   # 期望 0
grep '已关闭，最终在线' /tmp/chat_full_server.log
```

实测（2026-10-31，WSL，9 个 peer：alice/bob/carol/dup/raw/erin/g1/g2/g3）：

- 房间隔离：alice 在 lobby 未收到 bob 发到 dev 的 `dev-only`（通过）；
- 私聊：alice 收到 `DM bob private-hello`；离线目标返回
  `ERROR 用户不在线: nobody`（通过）；
- 在线列表/房间列表：`WHO`/`ROOMS dev lobby`（通过）；
- 历史回放：carol/frank/alice2 都收到 `HIST ...`（通过）；
- 心跳：g1/g2/carol 被移出在线表并广播“心跳超时/断线”（通过）；
- 重连：`HELLO tok-1` 恢复 `alice @lobby`，frank 收到“重新连接”与后续
  `MSG lobby alice back after reconnect`（通过）；
- 错误：重复昵称、未加入、未知命令、超长消息全部返回 `ERROR`（通过）；
- 并发：g1/g2/g3 同时发送，三条消息都进入持久化文件（通过）；
- 关闭：服务端退出码 0，日志显示 `已关闭，最终在线 0，离线 token 8`（通过）。

> 有一个时序敏感的断言曾被移除：bob 加入后立即 `ROOM dev`，因此收不到 carol
> 稍后发到 lobby 的实时消息。这是“房间隔离”的正确行为，不是丢消息——bob 在
> dev 房间内仍收到了自己的 `dev-only`。

## 边界与取舍

- UDP 明文、无鉴权：昵称/房间只是逻辑隔离，生产需要 PSK 或 TLS + 身份认证；
- token 存内存，服务端重启后全部失效；宽限期用系统时钟；
- 文本协议没有转义：昵称禁止空白与 `|`，消息中的换行/`|` 持久化时替换为空格；
- 房间只做广播范围，不做成员上限/权限/禁言；
- 持久化是“追加文件 + 启动时不加载”，回放只读最近 `replay_lines` 条；
- 应用层心跳依赖客户端主动 `PONG`；恶意客户端不回复会在超时后被移出会话，
  但 UDP 链路本身由 LinkServer 的 peer timeout 回收。
