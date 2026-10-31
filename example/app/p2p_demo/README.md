# p2p_demo —— 三个进程的 UDP 打洞演示

用 **一个 STUN echo 服务器 + 两个独立 peer 进程**，演示
`go2cpp::p2p::P2PChannel` 的完整打洞流程：

```
        ┌──────────────────────────┐
        │  go2cpp_p2p_demo stun    │  127.0.0.1:34780
        │  （STUN Binding echo）   │  返回 srflx = 服务器看到的源地址
        └────────────▲─────────────┘
                     │ STUN Binding
        ┌────────────┴─────────────┐        ┌──────────────────────────┐
        │  peer a  127.0.0.1:随机   │  UDP   │  peer b  127.0.0.2:随机  │
        │  只交换 srflx 候选        │◄──────►│  只交换 srflx 候选        │
        └────────────▲─────────────┘ 打洞   └────────────▲─────────────┘
                     │                                   │
              写/读 candidates-a.txt              写/读 candidates-b.txt
                     └──────── 文件信令目录 ─────────────┘
```

## 为什么这样能模拟 NAT

- Linux 的 `127.0.0.0/8` 整段都在 `lo` 上，`127.0.0.2` 可直接 bind，不需要
  额外网卡或 root。两个 peer 分别绑 `127.0.0.1` 与 `127.0.0.2`，就像两台
  不同 NAT 后的主机；
- 两个 peer 都设置 `gather_host_candidates = false`，只把 STUN 返回的
  **srflx（服务器反射地址）**写进信令——这正是 NAT 后主机的真实处境：
  本机地址不可达，对端只拿到公网映射；
- 双方读到对方候选后几乎同时 `Connect()`，`P2PChannel` 并行发送 STUN
  Binding 检查（`check_interval = 50 ms` 重发），任一侧收到对端的 Binding
  Request 或匹配事务 ID 的 Success Response 即锁定地址，打洞完成。

## 演示点

1. **候选收集**：`GatherCandidates()` 得到 host / srflx / relay 候选，
   按 RFC 8445 的优先级排序（host > prflx > srflx > relay）；
2. **信令解耦**：`P2PConfig::signaling` 只是接口；本例用文件邮箱手动交换
   候选（`SerializeCandidates` / `AddRemoteCandidate`），生产可把同样的
   文本换成 IM 私聊 / WebSocket / Redis；
3. **打洞与连通检查**：`Connect()` 内部发 STUN Binding，成功后
   `state() == kConnected`、`IsRelayed() == false`；
4. **数据面**：`Send()` / `Recv()` 收发 3 轮请求/响应，统计包数、字节、
   连通检查次数、序列缺口与 RTT；
5. **优雅关闭**：`Close()` 释放通道；STUN 服务器收到 SIGTERM 后停止并
   打印处理请求数。

## 构建

```sh
# 仓库根目录
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_p2p_demo -j8
```

`go2cpp_p2p_demo` 只在 `GO2CPP_WITH_P2P=ON`（默认 ON）时注册。

## 运行

### 方式一：一键脚本（推荐自检）

```sh
bash example/app/p2p_demo/run_demo.sh
```

脚本会起 STUN、先后起 a/b、等待完成、打印两份日志，最后清理临时目录与
子进程；成功时输出 `P2P 演示成功` 且退出码 0。

### 方式二：三个终端手动跑

```sh
# 终端 1：STUN echo
./.build-bento-wsl/example/app/go2cpp_p2p_demo stun --port 34780

# 终端 2：peer a（绑 127.0.0.1）
./.build-bento-wsl/example/app/go2cpp_p2p_demo peer --role a \
    --stun 127.0.0.1:34780 --signal-dir /tmp/go2cpp_p2p_demo/sig

# 终端 3：peer b（绑 127.0.0.2）
./.build-bento-wsl/example/app/go2cpp_p2p_demo peer --role b \
    --stun 127.0.0.1:34780 --signal-dir /tmp/go2cpp_p2p_demo/sig
```

两个 peer 的 `--signal-dir` 必须相同；目录不存在会自动创建。

## 预期输出（2026-10-31 实测）

```text
[peer a] 绑定 127.0.0.1:0，STUN=127.0.0.1:34780，收集候选...
[peer a] 收集到 1 个候选：
  srflx 127.0.0.1:52359       base=127.0.0.1:52359       priority=1694498815
[peer a] 候选已写入 .../sig/candidates-a.txt，等待对端候选...
[peer a] 收到对端 1 个候选，开始打洞...
[peer a] 连接成功（0 ms）：state=connected local=127.0.0.1:52359 peer=127.0.0.2:34036 relayed=0
[peer a] -> hello-0-from-a
...
[peer a] <- ack-2
[peer a] 统计：发包=3 收包=3 业务字节=42/15 连通检查=1/2 RTT=0.000ms 平均=0.000ms 序列缺口=0
P2P_OK role=a relayed=0 local=127.0.0.1:52359 peer=127.0.0.2:34036
```

peer b 对称：`local=127.0.0.2:34036 peer=127.0.0.1:52359`。STUN 最后打印：

```text
[stun] 已停止：处理 Binding 请求 2 个，响应 2 个
```

> RTT 显示 `0.000 ms` 是**本机回环**的真实结果：keepalive 的往返在 1 ms
> 时间戳精度内完成。跨机器运行时该值会变成正常的几毫秒。

## 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `stun --port` | `34780` | STUN echo 监听端口 |
| `peer --role` | `a` | `a` / `b`，决定默认 bind 地址与收发角色 |
| `peer --bind` | a=`127.0.0.1:0`、b=`127.0.0.2:0` | 本地绑定地址；端口 0 由内核分配 |
| `peer --stun` | `127.0.0.1:34780` | STUN 服务器地址 |
| `peer --signal-dir` | `/tmp/go2cpp_p2p_demo` | 候选文件的交换目录 |
| `peer --timeout-ms` | `8000` | 等待候选与 `Connect()` 的总超时 |
| `peer --host-candidates` | 关 | 打开后同时收集 host 候选，可对比局域网直连 |

## 边界与取舍

- 只演示 **UDP 打洞 + 直连**：`enable_relay = false`，没有 TURN 中继降级；
- 文件信令不是线程安全的分布式队列，只适合本机演示；跨机器请实现
  `Signaling`（接口只有 `Send` / `Recv` 两个方法）；
- 数据面没有重传/拥塞控制/FEC，单包上限 1200 字节；可靠传输由上层负责；
- STUN 服务器不做认证与 MESSAGE-INTEGRITY，只回答地址发现；
- 不支持 IPv6 场景的 `::1` 双栈演示（`Endpoint` 支持，但本例默认 IPv4）。
