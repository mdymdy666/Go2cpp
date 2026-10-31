# chat_server_simple —— 单文件聊天服务（LinkServer + 内置客户端）

一个 `main.cpp` 同时提供**服务端**和**测试客户端**：服务端用
`bento::LinkServer`（UDP 可靠通道）收 N 个 peer；客户端用同仓库的
`bento::PeerHub` 连接，两者线格式互通，不需要额外的聊天客户端。

## 功能

- 多客户端广播：任意 peer 发 `SAY`，所有已连接 peer 都收到 `昵称: 文本`；
- `JOIN <name>` 登记昵称；`WHO` 查看在线列表；`/quit` 客户端退出；
- 连接/断开时自动广播系统消息；
- 消息上限 1024 字节，昵称上限 32 字节；
- 服务端收到 `SIGINT/SIGTERM` 后 `LinkServer::Stop()`，触发断开回调并退出。

## 协议（可靠通道 1，UTF-8 文本）

| 方向 | 消息 | 说明 |
| --- | --- | --- |
| C→S | `JOIN <name>` | 登记昵称，之后才能发 SAY |
| C→S | `SAY <text>` / 裸文本 | 广播到所有在线 peer |
| C→S | `WHO` | 单播在线列表 |
| C→S | `/quit` | 客户端退出（本地处理） |
| S→C | `* text` | 系统消息/公告 |
| S→C | `<name>: <text>` | 聊天消息 |
| S→C | `* 在线: ...` | WHO 响应 |

## 构建与运行

```sh
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_chat_server_simple -j8

# 终端 1：服务端（默认 127.0.0.1:9100）
./.build-bento-wsl/example/app/go2cpp_chat_server_simple --server --port 9100

# 终端 2/3：客户端（启动时自动 JOIN alice）
./.build-bento-wsl/example/app/go2cpp_chat_server_simple --client alice --port 9100
# 输入：SAY 你好 / WHO / /quit
```

服务端与客户端都是同一可执行文件，用 `--server` / `--client <name>` 切换。

## 测试（脚本化，无需交互）

```sh
cd /path/to/Go2cpp
BIN=./.build-bento-wsl/example/app/go2cpp_chat_server_simple
$BIN --server --port 19100 > /tmp/chat_simple_server.log 2>&1 &
SPID=$!
sleep 0.5

{ printf 'SAY hello, I am alice\n'; sleep 3; } | \
  $BIN --client alice --port 19100 > /tmp/chat_simple_alice.log 2>&1 &
APID=$!
sleep 0.8
{ printf 'SAY hi alice\nWHO\n'; sleep 3; } | \
  $BIN --client bob --port 19100 > /tmp/chat_simple_bob.log 2>&1 &
BPID=$!

wait "$APID"; wait "$BPID"
kill -TERM "$SPID"; wait "$SPID"; echo "server_exit=$?"

grep 'bob: hi alice' /tmp/chat_simple_alice.log   # alice 应收到 bob 的广播
grep '在线: alice bob' /tmp/chat_simple_bob.log    # bob 的 WHO 响应
```

实测（2026-10-31）：两个客户端互收广播，`WHO` 返回 `在线: alice bob`；
客户端 EOF 后自动 `RemovePeer`，服务端退出码 0。

## 结构

```
chat_server_simple/
└── main.cpp   # ChatServer（LinkServer 回调 + 广播）+ ChatClient（PeerHub）
```

## 边界与取舍

- UDP 明文（`require_encryption = false`），只用于本机示例；生产必须配置 PSK/TLS；
- 单房间、无持久化、无心跳应用层协议、无重连；断线后昵称立即被回收；
- 同名昵称允许重复（没有唯一性校验）；
- 应用层消息没有分帧，一行一条；超长消息被服务端拒绝；
- 客户端 `--client` 启动时自动 JOIN，stdin 再写同名 JOIN 会被忽略。

下一步：看 `chat_server_full` 如何用会话表、房间、私聊、心跳、重连 token
和持久化把这些缺口补齐。
