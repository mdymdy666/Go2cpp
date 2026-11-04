# 基座大改审查记录（2026-11-04）

> 背景：今天对基座做了 10+ 处改动（Parser/Socket/Channel/go()/栈/TLS/tunnel/bufio/RWMutex/HttpServer/超时语义），
> 用户要求「好好审查一遍，把问题都整出来」。
> 方法：按**改动的交互**拆 6 路独立审查（不是按文件），每路：读 diff + 构造边界测试 + 前后对比。
> 结论：**P0×1（已修）/ P1×8（已修 6）/ P2×8（已报）/ P3×5**。

## 一、总览

| 审查范围 | P0 | P1 | P2 | P3 | 结论 |
|---|---|---|---|---|---|
| Parser | 0 | 2（已修） | 4（报告） | 0 | framing 逻辑基本正确，边界与契约有欠账 |
| Socket / Address | 0 | 0 | 2（已修） | 3 | BlockingRegion 嵌套安全、max 语义未变 |
| Channel / go() / RWMutex | **1**（已修） | 3 | 0 | 0 | RWMutex 修复正确且无性能退化 |
| TLS / tunnel / bufio | 0 | 2（已修） | 1 | 1 | TLS/bufio 独立验证无 bug；tunnel 两个真 bug |
| 构建 / ABI | 0 | 2（已修） | 1 | 0 | 7 个类布局变化，必须全量重建 |
| 测试期望 | 0 | 0 | 4 覆盖缺口 + 1 契约 | 0 | 期望基本一致，缺 304/consumed 断言 |

## 二、已修（本轮，共 6 个 commit）

| commit | 级别 | 内容 |
|---|---|---|
| `a5bf9db` | P1 | Parser：`m_finished` 后 feed 静默返回旧 `Complete`（新字节永久丢失）→ 改 Error；完成时清零 framing 快照 |
| `078a3d4` | **P0** | `os_signal.hpp:436` 忽略 `[[nodiscard]]` → `test_go_signal` **编译失败**（-Werror）→ `(void)` |
| `7fe300c` | P1×2 | tunnel：raw 握手 `FrameReader::Next` 8 KiB 预读**吞掉服务端 HTTP 头**（随机 bad magic）→ `NextExact`；detached 线程计数 UAF |
| `3d69f45` | P2 | Socket：负数 timeout 未兑现 EINVAL → 三处入口校验 |
| `3aa2e34` | P2 | `transport.cpp` 丢弃 `set_*_timeout` 返回值 → `io_timeout=0` 被拒后**静默变无限阻塞** |
| `8aa60f5` | P1×2 | CMake：`if (NOT TARGET app_gateway)` **恒真**（TARGET 不认 test 名）→ 哨兵 + include_guard；`test_realtime_room`/`tests/resilience` 未进 CTest → 注册 |

## 三、P2/P3 待修（已记录，非阻塞）

| 级别 | 位置 | 问题 | 建议 |
|---|---|---|---|
| P2 | `tunnel/server.cpp:1152` | `max_streams_per_tunnel` 对 raw 流失效（raw 不建 MuxStream，`streams_active` 不增长） | Entry 增 `atomic raw_active` 或租约 |
| P2 | `Parser` 快路径 `:476` | 换一个 `Response` 对象续 feed → body 对但 header 0 条、status 默认 200（假成功） | 文档写明须用同一对象 |
| P2 | `Parser.take_remaining()` | 隐式清 `m_response_to_head`（Client:862 已手动补） | 文档或改成显式 |
| P3 | `MuxSession::SendPayload` | 无锁读 `m_conn` 与加锁写的 `DetachConn` 构成数据竞争（当前无实际并发） | 加锁或用 atomic |
| P3 | 全仓约 10 处 | 忽略 `Send/TryRecv/SendOrThrow` 返回值（这些 target 无 `-Werror`） | 逐个 `(void)` 或判状态 |
| P3 | `go()` 返回 nullptr | 全仓 53 处 `auto x = go(...)` 无一判空 | 加 `if (!t)` 或文档 |

## 四、关键结论（无 bug 项，值得记录）

### RWMutex 丢唤醒修复（`c5b888f`）
- **窗口**：`Unlock` 先 `fetch_sub(release)` 开门，再 `load(m_waiting_readers)` 决定通知；读者登记恰好落在两者之间 → 唤醒被跳过。
- **修法**：`fetch_add` 后 acquire 重查 `state+pending`，成立则回滚计数 `continue`。
- **验证**：50 轮 × 100 线程 × 10 万 ops，0 挂起，终态 `readers=0/write_locked=0/pending=0`；性能无退化（8t 纯读 54–56 vs 54.9–57.0 Mops）。

### BlockingRegion 嵌套（Socket + hook）
- 内层 `enter_blocking` 因 M 已 `kBlocking` 返回 false，析构 no-op；`blocking_workers` **1 进 1 出**；`t_blocking_machine_id` 只由外层清。
- `Duration::max()` 的 100 ms 分片**不改变无限阻塞语义**（`timeout != max` 才判超时）。

### TLS Poly1305 IFMA 16 路
- 615 个长度（0..600 全量 + 边界 + 1 MB，随机 key/msg）：scalar vs avx512ifma **逐字节一致**。
- 篡改：33 B 全 264 bit + 1 MB 采样 2048 bit，**全部拒绝**。
- 唯一瑕疵：注释称每 lane 6 个部分积，实际 18 个（上界 < 2^59，不溢出）。

### bufio 内联
- `available < p.size()` 分支与原 `min` 逻辑逐行等价；586 次短读 + 边界 + EOF 全部正确。

## 五、ABI 风险（今日 7 个类布局变化）

| 类 | 变化 |
|---|---|
| `http::Parser` | 删 3 加 7（**事故源**，monitor SIGSEGV） |
| `http::Server` | 插 `m_options` + 构造签名 |
| `app::gateway::Config`/`Stats` | 插 `request_timeout` / +2 字段 |
| `bento::WsServerConfig` | 插 `websocket_prefetch_bytes`；`ReadHandshake` 签名 |
| `distributed::CoordinatorOptions` | 插 3 快照字段 |
| `tunnel::TunnelServerConfig`/`Entry` | +`raw_bind_timeout_ms` / +`raw_waiters` |
| `SchedulerConfig::fiber_stack_size` | 默认 2048→128 KiB（值语义） |

**结论**：任何依赖这些头文件的旧二进制都必须重建。依赖方（`nm` 实测）：`go2cpp_tests` 13、`mcp_server` 5、`app_test_gateway` 5、`app_test_http` 6。

## 六、事故复盘（今天两次误判）

| 误判 | 真相 | 教训 |
|---|---|---|
| 「13 个 SEGFAULT 是回归」 | 旧 exe + 新 so（ABI 不匹配） | 测试结论必须注明构建目录与 exe/so 是否同批 |
| 「Parser 未 reset 会吞 body」 | 旧库（12:44 `.so`）的假象；当前 HEAD 返回 Error | 拿 ABI 不一致的产物当证据 = 假证据 |

## 七、后续建议

1. `build.sh` 加「头文件新于产物则拒绝构建」的 stamp 校验；
2. CI 强制 clean rebuild；
3. 全仓 `[[nodiscard]]` 忽略点清理（约 10 处）；
4. `go()` 空句柄的判空规范。
