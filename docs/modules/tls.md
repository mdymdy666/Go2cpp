# 模块文档：tls（OpenSSL 客户端传输 / ALPN）

> **状态：现行**。基线 `HEAD 2ba2f4e`（2026-10-26 核对；`src/`、`include/` 相对 HEAD 无改动，本次只改文档）。
> 本文是 TLS 传输层的**唯一现行文档**：概述、API、设计、边界与测试集中在这里。
> 旧文档 `docs/network.md` 的 HTTP/2 + TLS 小节、`docs/module-audit-2026-10-20.md`
> 的 TLS 审计条目已并入本文；前者顶部已加合并标注，后者是历史审计快照，只作追溯。
> 其中「证书校验默认关闭（`verify_peer=false`）」的描述**已过时**，当前代码默认 `true`，
> 本文按代码更正（见 §5.1）。
> 引用约定：`file:line` 指当前工作树；引用历史审计结论时单独注明时间与前提。

---

## 1. 概述与职责

`TlsTransport` 是 HTTP/2 子模块的 **TLS 客户端 IO 实现**：把已建立的 TCP `Socket`
包成 OpenSSL `SSL` 会话，实现 `http2::Transport` 的 `Read` / `Write` / `Closed`，
并通过 ALPN 协商 `h2`。它**只做传输层**，不做 HTTP/2 协议、不做监听、不做证书管理
（`include/go2cpp/network/http2/tls_transport.hpp:3-9`、`src/network/http2/tls_transport.cpp:3`）。

| 做 | 不做 |
|---|---|
| TLS 1.2+ 客户端握手（`SSL_connect`） | 服务端 TLS（只有客户端，`tls_transport.hpp:49`） |
| SNI、证书链校验、SAN/CN 主机名校验 | 会话复用、0-RTT、会话票据管理（`tls_transport.hpp:49`） |
| ALPN 提议 `h2`，`http/1.1` 兜底（`tls_transport.cpp:28`） | 校验对端一定选中 `h2`——由调用方检查 `NegotiatedProtocol()` |
| 非阻塞 fd + `poll`，在 Fiber 中让出 worker | 拥有调度器、创建线程、处理 HTTP/2 帧 |
| `close_notify` 尽力发送、错误诊断文本 | 证书签发、CA 管理、OCSP/CRL 策略 |

**路径澄清**：任务书写的 `include/go2cpp/network/tls*.hpp`、`src/network/tls*.cpp`
不存在；实际文件在 `network/http2/` 下。根路径没有 `go2cpp/tls.hpp` 转发头，
使用方应直接包含 `go2cpp/network/http2/tls_transport.hpp`。

---

## 2. 文件布局与构建接线

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/network/http2/tls_transport.hpp` | `TlsTransport` 声明、`SSL_CTX`/`SSL` 前置声明 | 233 行 |
| `src/network/http2/tls_transport.cpp` | OpenSSL 集成、SIGPIPE 屏蔽、`WaitFor`、关闭与错误记录 | 505 行 |
| `include/go2cpp/network/http2/transport.hpp` | `Transport` 接口与 `MemoryTransport` | 215 行 |
| `tests/network/test_tls_alpn.cpp` | TLS 握手 + ALPN 实测程序 | 99 行 |
| `scripts/verify_tls_alpn.sh` | 启动 `openssl s_server` 并运行上述程序的脚本 | 见脚本 |

构建接线（`cmake/Go2CppModules.cmake`）：

- `find_package(OpenSSL REQUIRED)`：`:45`；
- `tls_transport.cpp` 编入 `go2cpp_network`：`:54`；
- `target_link_libraries(go2cpp_network PRIVATE OpenSSL::SSL OpenSSL::Crypto)`：`:326`。
  OpenSSL 只出现在 `.cpp`，公共头不泄漏 OpenSSL 类型（`:43-44`）。

**历史上 TLS 不在发布物**的审计结论已失效：`src/network/http2/tls_transport.cpp`
现在已在 `go2cpp_network` 的源文件列表中（`cmake/Go2CppModules.cmake:54`）。

---

## 3. API

### 3.1 构造与查询

| API | 语义 | 位置 |
|---|---|---|
| `static std::unique_ptr<TlsTransport> CreateClient(Socket::Ptr, const std::string& server_name, bool verify_peer = true, Duration timeout = 10s, const std::string& ca_file = {}, const std::string& ca_path = {})` | 建 `SSL_CTX`/`SSL`，设 SNI、校验、ALPN；失败返回 `nullptr` | 声明 `tls_transport.hpp:112-115`；实现 `tls_transport.cpp:123-197` |
| `bool Handshake(std::string* error = nullptr)` | 循环 `SSL_connect`，读 ALPN 结果；失败写入中文诊断 | 声明 `:132`；实现 `:361-417` |
| `void Cancel() noexcept` | 置取消标记（可跨线程），不可复位 | `:139` |
| `const std::string& NegotiatedProtocol() const noexcept` | ALPN 结果，如 `"h2"`；未协商为空 | `:142` |
| `std::string TlsVersion() const` | `SSL_get_version()`，诊断用 | 声明 `:145`；实现 `:419-425` |
| `int LastError() const noexcept` | 最近一次 `SSL_get_error()`（`SSL_ERROR_*` 数值） | 声明 `:161`；实现 `:302-304` |
| `std::string LastErrorString() const` | 可读错误：`SSL_ERROR_*` 名、`ERR` 队列、证书校验原因 | 声明 `:169`；实现 `:306-316` |
| `bool Closed() const noexcept` | `m_ssl==nullptr` / `m_eof` / 已发 `close_notify` / 已取消 | `:191-195` |

### 3.2 读写与关闭

| API | 语义 | 位置 |
|---|---|---|
| `std::size_t Read(std::uint8_t*, std::size_t)` | 最多读 `capacity` 字节；超时、EOF、错误都返回 `0`，细节看错误查询 | 声明 `:180`；实现 `:427-467` |
| `std::size_t Write(const std::uint8_t*, std::size_t)` | 循环写完 `length`；失败返回已写字节（小于 `length`） | 声明 `:190`；实现 `:469-503` |
| `void Close()` | 发送 `close_notify`，至多一次；析构自动调用 | 声明 `:154`；实现 `:318-359` |

**类不变量**（`tls_transport.hpp:78-84`）：`m_ssl == nullptr` 时 `Closed()` 恒真；
`m_fd` 与 `m_socket->fd()` 一致且被设为非阻塞；`m_eof` 置位后不再尝试 `SSL_read`/
`SSL_write`；只有 `LastError()`/`LastErrorString()` 允许跨线程查询，其余接口与
`Connection` 一样只由一个 Fiber 驱动。

---

## 4. 设计

### 4.1 `CreateClient`：安全默认与上下文初始化

步骤与对应代码：

1. **参数校验**（`tls_transport.cpp:126-130`）：`socket` 非空、`timeout.count() >= 0`，
   且拒绝 `Duration::max()`。后者旧实现会被 `static_cast<int>` 溢出成 `poll(-1)`，
   导致永久挂起；现在直接返回 `nullptr`。
2. **OpenSSL 初始化**（`:134-137`）：`OPENSSL_init_ssl()` 加载 SSL/CRYPTO 错误字符串，
   OpenSSL 3.x 幂等。
3. **建上下文并设最低版本**（`:138-142`）：`TLS_client_method()`，最低 TLS 1.2。
4. **证书校验**（`:143-161`）：`verify_peer=true`（默认）时 `SSL_VERIFY_PEER`；
   信任锚优先级为显式 `ca_file`/`ca_path` > 系统默认信任库
   （`SSL_CTX_set_default_verify_paths()`，尊重 `SSL_CERT_FILE`/`SSL_CERT_DIR`）。
   证书加载失败返回 `nullptr`，不会静默降级。
5. **建会话并设非阻塞**（`:163-172`）：`SSL_new`、`fcntl(O_NONBLOCK)`、`SSL_set_fd`。
   非阻塞是关键：阻塞 fd 会让 `SSL_*` 卡住整个 worker 线程（`tls_transport.hpp:13-18`）。
6. **SNI**（`:173-177`）：`server_name` 非空时设 `SSL_set_tlsext_host_name`。
7. **主机名校验**（`:180-192`）：仅当 `verify_peer && !server_name.empty()`；
   先 `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`，IP 字面量走
   `X509_VERIFY_PARAM_set1_ip_asc`，DNS 走 `SSL_set1_host`。
8. **ALPN**（`:193-195`）：`h2` 优先、`http/1.1` 兜底；设置失败返回 `nullptr`。

默认值：`verify_peer = true`（`tls_transport.hpp:94`、`:113`），`timeout = 10000ms`
（`:114`）。`verify_peer=false` 只用于自签名测试（`:95`、`tls_transport.cpp:158-160`）。

### 4.2 握手

`Handshake()`（`tls_transport.cpp:361-417`）是一个带总 deadline 的 `SSL_connect` 循环：

- 进入前先屏蔽 SIGPIPE（`:367`），对端在握手期关闭时不能杀进程；
- `SSL_connect` 返回 `1` 即完成；`WANT_READ`/`WANT_WRITE` 交给
  `WaitFor(true/false, deadline)`，超时与取消分别写入
  `"TLS 握手等待可读/可写超时"`、`"TLS 握手已取消"`（`:380-401`）；
- 其它错误调用 `RecordSslError()`，错误文本为 `"TLS 握手失败: " + LastErrorString()`
  （`:402-406`）；
- 成功后用 `SSL_get0_alpn_selected()` 记录结果（`:409-415`）。

注意：握手成功**不代表 ALPN 是 `h2`**。`Connection`/调用方必须检查
`NegotiatedProtocol() == "h2"`；服务端可能选中 `http/1.1` 或什么都不选。

### 4.3 等待、超时与取消

`WaitFor()`（`tls_transport.cpp:220-274`）把等待切成 **100ms** 分片（`:225`）：

- 每片先检查取消标记与 socket 有效性（`:227-232`）；
- 剩余时间按**本次操作的总 deadline** 计算，不会在每次 WANT 时重新获得完整超时
  （`:234-247`）；总 deadline 由 `Deadline()` 在每次 `Handshake`/`Read`/`Write`
  入口生成（`:213-218`、`:368`、`:432`、`:476`）；
- `poll()` 返回 `POLLERR|POLLHUP|POLLNVAL` 时返回 `true`，让 `SSL_*` 读取真实错误
  （`:265-268`）；
- `EINTR` 继续，其它 `poll` 失败返回 `false`（`:256-261`）；
- `Cancel()`（`tls_transport.hpp:139`）只是原子置位；等待中的 `poll` 最迟一个分片后
  返回，后续 `Read`/`Write` 返回 `0`、`Closed()` 变真（`:136-138`）。

项目 Hook 会拦截 `poll`，所以在 Fiber 中等待会 park 当前 G，而不是阻塞 worker；
在普通线程中则是普通的 100ms 轮询（`tls_transport.hpp:13-16`）。

### 4.4 `Read` / `Write`

`Read()`（`tls_transport.cpp:427-467`）：

- 入口条件：`m_ssl` 非空、buffer 非空、`capacity > 0`、未 EOF、未取消；
- `SSL_read` 的 `length` 是 `int`，`capacity > INT_MAX` 时按 `INT_MAX` 截断，避免隐式
  溢出（`:433-436`）；
- 正数直接返回；`WANT_READ`/`WANT_WRITE` 走 `WaitFor` 后重试；
- `SSL_ERROR_ZERO_RETURN` 表示收到对端 `close_notify`，置 `m_eof` 返回 `0`，
  **不覆盖** `LastError`（`:452-456`）；
- `SSL_ERROR_SYSCALL` 且 `ERR_peek_error()==0` 表示对端直接关 TCP（无 `close_notify`），
  记录错误、置 `m_eof` 返回 `0`（`:457-462`）；
- 其它错误记录、置 `m_eof`、返回 `0`（`:463-465`）。

`Write()`（`tls_transport.cpp:469-503`）循环 `SSL_write` 直到写完或失败：
`WANT_WRITE`/`WANT_READ` 等待后重试；等待失败（超时/取消）直接 `break`，返回**已写
字节数**，此时不保证置 `m_eof`；只有 `SSL_*` 真实错误才记录并置 `m_eof`、`break`
（`:489-501`）。因此调用方必须用「返回值是否等于 `length`」判断成功。

### 4.5 关闭语义

`Close()`（`tls_transport.cpp:318-359`）是**尽力而为的单向关闭**：

- `m_shutdown_sent` 用 `exchange` 保证 `SSL_shutdown` 至多执行一次（`:322-324`）；
- 关闭前屏蔽 SIGPIPE（`:330-332`），对端先关闭时不能杀进程；
- 预算 `min(m_timeout, 500ms)`（`:333-334`），最多 8 次尝试（`:335`）；
- `SSL_shutdown` 返回 `>=0` 即认为本端 `close_notify` 已交给 socket（`:338-343`）：
  `1` 是双向完成，`0` 是已发出、仍在等对端回包；两种都直接返回；
- `WANT_WRITE` 说明 `close_notify` 还没写进 socket，用 `WaitFor(false, deadline)` 重试；
  预算耗尽就返回（`:345-350`）；
- `WANT_READ` 只可能出现在等对端 `close_notify` 的阶段，此时本端通知已发出，
  直接置 `m_eof` 返回（`:351-354`）；
- 其它错误 `RecordSslError()` 后置 `m_eof` 返回（`:355-357`）。

析构函数先 `Close()`，再 `SSL_free` / `SSL_CTX_free`（`:199-211`）。因为不等对端
回包，`Close()` 不会把析构拖到对端超时；代价是本端无法确认对端是否也正常关闭。
`close_notify` 的意义是让对端区分「正常结束」与「TCP 被截断」，否则 HTTP/2 层可能把
半截响应当成完整响应（`:200-202`）。

### 4.6 SIGPIPE 屏蔽

`ScopedSigpipeBlock`（`tls_transport.cpp:44-83`）用线程级 `pthread_sigmask` 屏蔽
SIGPIPE，而不是全局 `signal()`：

- OpenSSL 的 socket BIO 用 `write()`，对端关闭后写会触发 SIGPIPE，默认动作是终止进程；
- 屏蔽后 `write` 返回 `EPIPE`，由 `SSL_get_error` 转成 `SSL_ERROR_SYSCALL`；
- 析构时不能只恢复掩码：写失败会让 SIGPIPE 处于「本线程 pending」，恢复掩码的瞬间
  会按默认动作终止进程。因此先用 `sigpending` 检查并用 `sigtimedwait` 消费 pending，
  再恢复旧掩码（`:62-74`）。

握手、应用数据写、关闭写 `close_notify` 三条路径都使用它（`:332`、`:367`、`:475`）。

### 4.7 错误模型

- `RecordSslError()`（`tls_transport.cpp:276-300`）记录 `SSL_get_error()` 数值，拼接
  `SSL_ERROR_*` 名称与 `ERR_get_error()` 队列；
- 只有开启了对端校验（`SSL_VERIFY_PEER`）时才附加 `SSL_get_verify_result()` 的原因，
  避免 `verify_peer=false` 时把残留的自签名结果误挂到无关 EOF 上（`:284-297`）；
- `LastError()` 读原子值（`:302-304`）；
- `LastErrorString()` 先读已记录文本；没有记录时返回实时 `ERR` 队列，队列为空才返回
  空串（`:306-316`）；
- **超时/取消不是 SSL 错误**：`WaitFor` 返回 `false` 时通常不写 `LastError`，所以
  `Read`/`Write` 返回 `0`/短写时不能只靠错误文本区分「超时」与「EOF」，还要看
  `Closed()`、`Cancel()` 与调用方自己的 deadline。

---

## 5. 边界、降级与已知限制

### 5.1 文档与代码的默认值冲突（重要）

`tls_transport.hpp:44` 仍写着「证书校验默认关闭（`verify_peer = false`），生产必须打开」，
`docs/network.md` 旧文也重复了同一说法；但当前代码的默认值是 `verify_peer = true`
（`tls_transport.hpp:94`、`:113`，实现 `tls_transport.cpp:143-157`），文件头
`tls_transport.hpp:22-29` 也明确解释了「审计发现不安全默认后改为 true，是 fail-closed」。
**以代码和本文为准**：默认校验证书链；依赖旧默认连自签名服务的调用方会握手失败，
正确做法是配置 `ca_file`/`ca_path`，而不是关闭校验。

### 5.2 其他边界

- **服务端 TLS**：没有。`TlsTransport` 只有 `CreateClient()`，没有 `SSL_accept` 路径
  （`tls_transport.hpp:30`、`:49`）。
- **会话复用 / 0-RTT**：没有；每次 `CreateClient()` 新建 `SSL_CTX`，没有会话缓存、
  ticket 或 early data 接口（`tls_transport.hpp:49`）。
- **空 `server_name`**：不设 SNI，也**不做主机名校验**；`verify_peer=true` 只校验证书链。
  需要虚拟主机或主机名绑定时必须传名字。
- **`verify_peer=false`**：只用于自签名/本地测试；它会同时关闭链校验和主机名校验
  （`:180` 的条件）。
- **`Read`/`Write` 的 0 语义**：0 可能是超时、EOF、取消或错误；`LastError` 不保证
  覆盖超时。`Read` 的契约来自 `Transport`「阻塞到至少 1 字节或关闭」，但 TLS 层在
  等待超时时也会返回 0，调用方必须自行处理重试/终止（`transport.hpp:65-71`）。
- **`ReadWithTimeout` 未覆盖**：`TlsTransport` 只覆盖 `Read`/`Write`/`Closed`；
  `Transport::ReadWithTimeout` 的默认实现忽略 `timeout_ms` 直接调 `Read`
  （`transport.hpp:86-95`）。因此带 deadline 的调用仍受 `CreateClient` 时固定的
  `m_timeout` 约束，而不是调用方传入的剩余预算。若上层需要更短 deadline，应把
  `CreateClient` 的 `timeout` 设成所需上限。
- **非线程安全**：一个 `TlsTransport` 只应由一个 Fiber/线程驱动；`Cancel()` 与错误
  查询是例外（`tls_transport.hpp:82-84`）。不要在另一个线程里同时 `Close()` 或读写。
- **会改写 socket 为非阻塞**：`fcntl(O_NONBLOCK)` 在 `CreateClient` 内执行且不恢复
  （`tls_transport.cpp:167-169`）；socket 所有权已转入，但若调用方在别处还持有同一
  fd，会观察到非阻塞语义。
- **`fcntl` 失败未检查**：`:169` 忽略返回值；若设置失败，SSL 可能按阻塞 fd 工作，
  破坏 Fiber 友好性。当前 Socket 默认已非阻塞，所以实际风险低，但这是一个隐含前提。
- **ALPN 不强制**：只提议、不验证；调用方必须检查 `NegotiatedProtocol()=="h2"`。
- **HTTP/2 over TLS 完整链路未验证**：现有测试只到 TLS + ALPN 协商，缺真正的 h2
  TLS 服务端（nginx/nghttpd）；`docs/network.md` 的同一结论仍然有效。
- **关闭是单向的**：不等对端 `close_notify`；对端是否正常关闭无法确认。
- **取消不可复位**：`Cancel()` 置位后 `Closed()` 恒真；需要新连接只能重建对象。
- **没有高层接线**：除 `tests/network/test_tls_alpn.cpp` 外，仓库没有
  `TlsTransport::CreateClient()` 的调用点；使用方需要自己创建传输并把
  `TlsTransport*` 注入 `http2::Connection`，没有现成的 `http2` 客户端封装。

### 5.3 历史审计结论对照

`docs/module-audit-2026-10-20.md` 对 TLS 的审计结论已修复，当前状态如下：

| 历史结论（2026-10-20） | 当前状态 | 证据 |
|---|---|---|
| TLS 不在发布物，`tls_transport.cpp` 不在构建 target | 已修复：编入 `go2cpp_network`，链接 OpenSSL | `cmake/Go2CppModules.cmake:54`、`:326`；审计原文 `docs/module-audit-2026-10-20.md:23` |
| 无主机名校验 | 已修复：`SSL_set1_host` / `set1_ip_asc` | `src/network/http2/tls_transport.cpp:180-192`；审计原文 `docs/module-audit-2026-10-20.md:45` |
| `Duration::max()` 导致 `poll(-1)` 永久挂起 | 已修复：`CreateClient` 直接拒绝 | `src/network/http2/tls_transport.cpp:126-130`；审计原文 `docs/module-audit-2026-10-20.md:45` |
| `verify_peer` 默认 `false` | 已改为 `true`（fail-closed 的 breaking change） | `include/go2cpp/network/http2/tls_transport.hpp:94`、`:113`；审计原文 `docs/module-audit-2026-10-20.md:204`、`:261` |

---

## 6. 测试与验证

### 6.1 现有测试

`tests/network/test_tls_alpn.cpp`（99 行）用 `openssl s_server -alpn h2 -www` 作为
独立对端，验证：

1. `CreateClient(socket, "localhost", false, 10s)` 能创建传输（`:47`）；
2. `Handshake()` 成功（`:54`）；
3. ALPN 结果非空且等于 `"h2"`（`:63-64`）；
4. 能在 TLS 上写 `GET / HTTP/1.0\r\n\r\n` 并读到响应（`:67-74`）；
5. 程序由单 worker 的 `Scheduler` 以 Fiber 方式运行（`:80-92`）。

脚本 `scripts/verify_tls_alpn.sh` 负责生成自签名证书、启动 `s_server`、运行测试
二进制。注意测试用 `verify_peer=false` 是为了接受自签名证书，**不是**默认值。

### 6.2 注册状态与运行方式

TLS 测试**不注册为 ctest**：它依赖外部 `openssl s_server`，属非 hermetic 测试，
只构建独立二进制，避免无 OpenSSL 的 CI 误报（`cmake/Go2CppTests.cmake:385-391`）。
运行方式：

```bash
# 先构建（构建目录按实际环境替换）
cmake -S . -B build -DGO2CPP_BUILD_TESTS=ON
cmake --build build --target go2cpp_tls_alpn_test
BUILD_DIR=build scripts/verify_tls_alpn.sh
```

### 6.3 覆盖缺口（如实标注）

- 未覆盖失败握手、证书过期/主机名不匹配、`Cancel()` 打断握手、`Close()` 在坏网络下
  的预算行为；这些路径只有代码级保证，没有自动化回归。
- 未覆盖 `Read`/`Write` 的超时、短写、`SSL_ERROR_SYSCALL` 分支。
- 没有 HTTP/2 over TLS 的端到端测试（缺 h2 TLS 服务端）。

---

## 7. 使用示例

```cpp
#include "go2cpp/network/http2/tls_transport.hpp"
#include "go2cpp/network/socket.hpp"

using namespace std::chrono_literals;

auto socket = go2cpp::net::Socket::CreateTCP(AF_INET);
if (!socket || !socket->connect(address, 5000ms)) return;

// 生产：verify_peer 用默认 true；这里显式写 true 便于审阅。
auto tls = go2cpp::net::http2::TlsTransport::CreateClient(
    socket, "example.com", /*verify_peer=*/true, 10s);
if (!tls) return;

std::string error;
if (!tls->Handshake(&error)) {
    // error 带 SSL_ERROR_*、ERR 队列与证书校验原因
    return;
}
if (tls->NegotiatedProtocol() != "h2") return;  // ALPN 不是 h2 时不要继续 HTTP/2
// 之后把 tls.get() 作为 http2::Transport* 注入 Connection
```

---

## 8. 相关文档

- `docs/network.md`：网络层总览（TLS 小节已合并到本文）。
- `include/go2cpp/network/http2/transport.hpp`：`Transport` 三方法契约与超时语义。
- `docs/modules/io.md`：`poll` 在 Fiber 中如何 park/wake。
- `docs/quic.md`：QUIC 使用 picotls 的另一条 TLS 1.3 路径，与本模块无关。
- `docs/module-audit-2026-10-20.md`：历史审计（TLS 条目已修复，结论不再代表现状）。
