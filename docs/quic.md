# QUIC / HTTP3 模块（2026-10-20 以代码为准订正）

> ## 当前状态：picoquic + picohttp 薄封装，自实现已删除
>
> 本文档在 2026-10-20 的 HTTP/3 审计与修复后重写（审计项 D20）。
> 下面「历史」一节保留 2026-10-18 之前自实现时期的记录，**不代表当前代码**。
>
> **当前代码不是 QUIC 实现，而是成熟库集成：**
>
> | 文件 | 行数 | 职责 |
> |---|---:|---|
> | `include/go2cpp/network/quic/picoquic_connection.hpp` | 270 | 裸 QUIC 客户端接口（pimpl，不暴露 picoquic 类型） |
> | `src/network/quic/picoquic_connection.cpp` | 698 | 唯一 `#include <picoquic.h>` 的 TU：握手驱动、按流收发、关闭、事件翻译 |
> | `include/go2cpp/network/http3/client.hpp` | 466 | HTTP/3 客户端门面（Request/Response/Client/Options） |
> | `src/network/http3/client.cpp` | 1357 | 唯一 `#include <h3zero.h>` 的 TU：HEADERS/DATA、QPACK（picohttp）、控制流、GOAWAY |
> | `example/network/picoquic_hq_client.cpp` | 188 | hq-interop 端到端示例（UDP Transport 的生产参考实现） |
>
> 仓库内**没有** `src/network/quic/varint.cpp`、`packet.cpp`、`frame.cpp`，
> 也**没有** `tests/network/test_quic_*.cpp` —— 那些文件在 2026-10-18 已删除；
> 当前 `tests/` 下没有 QUIC/HTTP3 测试（见 §6「已知缺口」，审计 D19）。
> 如果看到旧文档/旧提交提到这些文件，均为历史记录。

---

## 1. 依赖与构建

### 1.1 依赖（本机路径）

- picoquic **1.1.53.0**：`/home/ubunturoot/build-deps/picoquic`
  - 头文件：`picoquic/picoquic.h`、`picohttp/h3zero.h`、`picohttp/h3zero_common.h`、`picohttp/demoserver.h`
  - 静态库：`build/libpicoquic-core.a`、`build/libpicoquic-log.a`、`build/libpicohttp-core.a`
- picotls：`/home/ubunturoot/build-deps/picotls`
  - `libpicotls-core.a`、`libpicotls-openssl.a`、`libpicotls-fusion.a`、`libpicotls-minicrypto.a`
- 系统 OpenSSL 3.0.13（只做证书解析/哈希；TLS 1.3 由 picotls 实现）

### 1.2 CMake

```bash
cmake -S . -B build-work -DGO2CPP_WITH_PICOQUIC=ON \
      -DPICOQUIC_ROOT=/home/ubunturoot/build-deps/picoquic \
      -DPICOTLS_ROOT=/home/ubunturoot/build-deps/picotls
```

`GO2CPP_WITH_PICOQUIC` **默认 OFF**（`cmake/Go2CppModules.cmake:312`）；
打开后生成 `go2cpp_quic_picoquic`、`go2cpp_http3_picoquic` 两个静态库与
`go2cpp_picoquic_hq_client` 示例。

**静态库链接顺序（关键）**：`libpicohttp-core.a` 必须排在
`libpicoquic-core.a` 之前，否则 `h3zero_*` 的符号解析会失败
（`cmake/Go2CppModules.cmake:344` 已有注释）。

### 1.3 独立构建（不经 CMake，用于快速验证）

```bash
PICO=/home/ubunturoot/build-deps/picoquic
PTLS=/home/ubunturoot/build-deps/picotls
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    -I$PICO/picoquic -I$PICO/picohttp -I$PTLS/include \
    -c src/network/quic/picoquic_connection.cpp -o /tmp/q.o
g++ ... 同样编译 src/network/http3/client.cpp -o /tmp/h3.o
g++ ... test.cpp /tmp/h3.o /tmp/q.o \
    $PICO/build/libpicohttp-core.a $PICO/build/libpicoquic-core.a \
    $PICO/build/libpicoquic-log.a \
    $PTLS/libpicotls-core.a $PTLS/libpicotls-openssl.a \
    $PTLS/libpicotls-fusion.a $PTLS/libpicotls-minicrypto.a \
    -lssl -lcrypto -lpthread
```

两个 TU 与示例在 `-Werror` 下零警告（2026-10-20 验证）。

---

## 2. 当前能力（以代码为准）

### 2.1 裸 QUIC：`net::quic::PicoquicConnection`

| 方法 | 语义 |
|---|---|
| `Connect(transport, config, error)` | 只创建 picoquic 上下文并发 ClientHello；握手在后续 `Poll` 推进 |
| `Poll(error)` / `PollFor(error, timeout_ms)` | 发完所有待发包 → 用 `Transport::ReadWithTimeout` 读一个 datagram → 喂 `picoquic_incoming_packet`；等待被 picoquic 下一个定时器（PTO/idle/ACK）与调用方预算截短 |
| `Close(code, error)` | 主动关闭并**立即**把 CONNECTION_CLOSE 写出去（幂等） |
| `WriteStream/ReadStream` | 按流收发；`ReadStream` 返回 0 只表示暂无数据 |
| `StreamFinished(id)` / `StreamErrorCode(id)` | FIN / RESET_STREAM 的状态查询（审计 D17） |
| `LastErrorCode()` | 关闭码（对端 app > 本地 app > 对端 transport > 本地 transport）或最近错误 |

事件处理（`Impl::StreamCallback`）覆盖：`ready`、`stream_data`、`stream_fin`、
`stream_reset`、`stop_sending`、`close`、`application_close`、`stateless_reset`
（审计 D09）。**回调内异常会被捕获**（D18），不会穿过 picoquic 的 C 调用栈。

### 2.2 HTTP/3：`net::http3::Client`

| 方法 | 语义 |
|---|---|
| `Connect(transport, sni[, Options], error)` | 创建连接；`Options` 控制超时、证书校验、`:authority`/SNI、响应体上限 |
| `Send(Request, error)` | 编码 HEADERS [+ DATA] 并排队；收到 GOAWAY 后拒绝 `stream_id >= GoAwayId()` 的新流 |
| `Receive(Response*, error)` | 按 FIFO 等待 FIN + HEADERS，取回 status/body/stream_id |
| `Poll(error)` / `Close(code, error)` | 单步驱动 / 主动关闭 |
| `Authority()` / `Sni()` | 实际使用的 `:authority` 与 TLS SNI（D15：两者可分离） |
| `GoAwayReceived()` / `GoAwayId()` / `ServerSettingsReceived()` | 服务端控制流状态（D16） |
| `LastStreamId()` | 最近分配的流号；未发送过返回 `kInvalidStreamId`（UINT64_MAX，D21） |

`Options` 字段：

```cpp
struct Options {
    std::uint32_t timeout_ms{5000};      // 握手/等待响应的总预算
    bool verify_peer{false};             // 生产必须 true；无 CA 时 fail-closed
    std::string cert_root_file_name;     // 为空则探测系统信任库
    std::string authority;               // 最高优先级，完整覆盖 :authority
    std::string host;                    // authority host；为空则从 sni 推断
    std::uint16_t port{0};               // 非默认端口必须显式给（如 14434）
    std::string sni;                     // 覆盖 TLS SNI（与 authority 分离）
    std::size_t max_response_bytes{64MiB}; // 0 = 不限制（不建议）
};
```

**服务端控制流（D16）**：
- 控制流（stream 3）的第一个 varint 是流类型 0x00，随后必须先是 SETTINGS；
- SETTINGS 用 picohttp 的 `h3zero_settings_components_decode` 解析；
- GOAWAY 被解析并记录；后续 GOAWAY 的流号只能更小（否则 H3_ID_ERROR）；
- 控制流上的 DATA/HEADERS/PUSH_PROMISE 是 H3_FRAME_UNEXPECTED，控制流关闭是
  H3_CLOSED_CRITICAL_STREAM；
- QPACK encoder(7)/decoder(11) 流被**消费并丢弃**（维持流控），不建立动态表；
- 服务端 push 流（`stream_id & 0x3 == 2`）与双向流（`& 0x3 == 1`）视为
  H3_STREAM_CREATION_ERROR。

### 2.3 已知能力边界（设计上不做，不是待补的桩）

- 请求方法只有 GET / POST（picohttp 只提供这两个构造器）；
- 响应头只解出 `content-type`（picohttp 解析器的限制）；
- 不使用 QPACK 动态表：只用静态表 + literal，压缩率较低；服务端 encoder 指令
  只被消费，不解析；
- 没有 0-RTT、服务端 push（收到即协议错误）、连接迁移/多路径；
- 客户端单向流（2/6/10）由 `h3zero_protocol_init` 创建，本封装不直接管理。

---

## 3. 关键契约与陷阱

1. **`Transport` 必须是 datagram 语义**：一次 `Read` 返回一个完整 UDP 报文。
2. **`Transport::ReadWithTimeout` 是实现方义务**：基类默认退化为阻塞 `Read`，
   不覆盖它时超时仍可能被一次长阻塞击穿（审计 D02）。示例
   `example/network/picoquic_hq_client.cpp` 用 `poll()` 给出生产参考实现。
3. **回调线程模型**：picoquic 没有第二线程；回调在调用 `Poll`/`Send`/`Receive`
   的线程上**同步**执行。旧注释「回调在 picoquic 自己的线程执行」是错的（D20）。
4. **异常边界（D18）**：两个 C 回调都整体包在 `try/catch(...)` 里；捕获后置
   `closed`、错误码 0x0102（HTTP/3 INTERNAL_ERROR），并 `picoquic_close`。
   错误字符串留到 C++ 调用栈再拼（catch 内只做不抛操作）。
5. **关闭语义**：`Close()` 立即发 CONNECTION_CLOSE；析构复用同一路径
   （best-effort，吞掉 Transport 异常）。不关闭会耗尽 picoquicdemo 的 256 个
   连接槽（审计 D01）。
6. **`:authority` 与 SNI**：`Connect(transport, "host:port")` 会拆成
   SNI=host、authority=host:port；`Options::host/port/sni/authority` 可分别覆盖。
   authority 拒绝 CR/LF/空白等注入字符（D15）。
7. **响应体上限**：`max_response_bytes` 超限即关连接并报错（D08）；流控窗口
   也只按实际需要/上限打开，不按对端声明长度照单全收。
8. **`LastStreamId()` 哨兵**：未发送过请求时返回 `kInvalidStreamId`（不是 0，
   因为 0 是合法流号，D21）。
9. **超时预算**：`Send`/`Receive` 把剩余预算传给 `PollFor`，不再只检查两次
   `Poll` 之间（D02）；`PollFor(-1)` 才是无界等待。

---

## 4. 测试与验收（2026-10-20）

- 独立构建：两个 TU + 示例，`-Wall -Wextra -Wpedantic -Werror` **零警告**；
- 本地验收：`/tmp/fix_http3/acceptance.sh` **29/29 PASS**（25 项原有 +
  D15 authority/SNI、D16 SETTINGS/GOAWAY、D09 application_close/stream_reset、
  D18 回调异常）；
- 30 轮端到端（GET + 5s 超时 + 10 次建连 + 关闭码）：**30/30，0 崩溃**
  （`/tmp/fix_http3/round30.log`）；
- 300 次建连 × 3 种模式（pending 析构 / 收响应 / 显式 Close）全部 300/300；
- `ctest`：见修复报告 `fix_http3_final.md` 的实测数字。

### 4.1 服务端（本地验证）

```bash
cd /home/ubunturoot/build-deps/picoquic
./build/picoquicdemo -p 14434 -c certs/cert.pem -k certs/key.pem -w /tmp/quic_www -a h3
./build/picoquicdemo -p 14433 -c certs/cert.pem -k certs/key.pem -w /tmp/quic_www -a hq-interop
./build/picoquicdemo -p 14435 -c /tmp/fix_http3/certs/srv.pem -k /tmp/fix_http3/certs/srv.key -w /tmp/quic_www -a h3
```

D16/D09 的 GOAWAY、application_close、stream_reset 由 `/tmp/fix_http3/h3_test_server`
（最小 picoquic 服务端，不属于仓库）注入。

---

## 5. 历史：自实现时期（2026-10-18 之前，已删除）

> 以下描述的是**已删除**的代码，仅作教训记录。当前代码不要按这里理解。

自实现只覆盖 QUIC 的**地基**：varint（RFC 9000 §16）、包头（§17）、帧层（§19），
约 1702 行。它**不能建立连接、不能传输数据**，缺 TLS 1.3、Initial 密钥派生、
Header Protection、丢包检测/PTO、拥塞控制、流状态机、QPACK。

删除原因：它既不能用于生产，又要占 1702 行维护，还容易让后来者误以为项目支持
QUIC —— 这个误导风险比代码本身更糟。picoquic 薄封装已经端到端验证，借用成熟
协议实现，只保留 `Transport` 与 Fiber 的粘合。

历史教训：

1. 「能解析字节」和「能用」是两个数量级的差距；varint 30 行，能建连的 QUIC 是
   数万行 + 多年迭代。
2. 选型硬约束要先查：本机 OpenSSL 3.0.13 不支持 QUIC API，直接排除
   ngtcp2/lsquic/msquic(默认)，只剩 picoquic（自带 picotls）。
3. 使用者的要求是「找一个库，不要自己实现」；预算不够时应报告做不了，
   而不是自作主张改方向。

> **历史测试数据**（已失效，不要引用）：自实现时期曾有 varint 20 万次往返、
> 包头 2 万次往返/10 万次变异、帧层 10 场景与「94/94 ctest」等记录；对应源文件
> 与测试文件均已删除，当前仓库**没有**这些测试。旧文档里的
> `tests/network/test_quic_packet.cpp`、`tests/network/test_quic_frame.cpp` 不存在。

### 5.1 库集成评估（已完成，旧评估文档已过时）

`docs/quic-lib-evaluation.md` 与 `docs/quic-go-reading-guide.md` 写于集成之前，
其中「picoquic 集成被 picotls 阻断、代码未进项目」「本项目连 HTTP/2 都没有」等
结论**已过时**：picoquic 1.1.53 + picotls 已在本机编译并接入 CMake，
HTTP/2 也已有完整实现。这两份文档不在本次修复的可改范围（需决策是否重写）。

---

## 6. 已知缺口 / 需决策

| 缺口 | 说明 | 状态 |
|---|---|---|
| 仓库内无 HTTP/3/QUIC ctest | 审计 D19：`GO2CPP_WITH_PICOQUIC` 默认 OFF，且无 `tests/network/test_http3_*` | **需决策**：`cmake/`、`tests/` 不在本次修复边界；验收测试在 `/tmp/fix_http3`（不进仓库） |
| QPACK 动态表 | 只消费 encoder/decoder 流，不解析插入指令 | 设计边界（`h3zero` 客户端同样不用动态表） |
| 响应头全集 | picohttp 解析器只保留 `:status`/`content-type`/`range` 等 | 设计边界 |
| 服务端 push / 0-RTT / 迁移 | 未实现 | 设计边界 |
| 两份旧评估文档 | `quic-lib-evaluation.md`、`quic-go-reading-guide.md` 与现状矛盾 | **需决策**：本次可改文件不含它们 |

---

## 7. 参考

- RFC 9000（QUIC transport）、RFC 9001（TLS）、RFC 9114（HTTP/3）、RFC 9204（QPACK）
- picoquic: <https://github.com/private-octopus/picoquic>
- picotls: <https://github.com/h2o/picotls>
