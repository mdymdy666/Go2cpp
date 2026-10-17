# QUIC 库选型与集成可行性评估

> 本文件记录 5 个候选库的对比、为什么选 picoquic、以及分步打通的卡点。
> 原始报告由并行任务流产出，迁入 `docs/` 是为了随仓库保留（`.dsh_tmp` 不参与同步）。

## 一句话结论

**picoquic 是唯一不需要更换系统 TLS 的库** —— 它自带的 picotls 实现了 TLS 1.3，
系统 OpenSSL 3.0.13 只用于证书/哈希，**不碰 QUIC API**。
ngtcp2 架构上最理想（纯传输层），但硬约束是 OpenSSL 3.2+；msquic 要 quictls；
lsquic 要 BoringSSL；quiche 是 Rust。

---

# QUIC 库选型与集成可行性报告

> 任务：为 Go2Cpp 集成成熟 C/C++ QUIC 库，做薄封装。**不自己实现协议逻辑。**
> 时间窗：约 25 分钟（23:34 ~ 24:00），因此以**选型 + 卡点定位**为主，未完成完整集成。

## 一、选型对比表（核心交付）

| 维度 | **msquic**（微软） | **picoquic** | **ngtcp2** | **lsquic** | **quiche** |
|---|---|---|---|---|---|
| **语言 / 代码量** | C，~30 万行（含平台抽象层） | C，**176,143 行**（244 文件） | C，**110,292 行**（250 文件） | C，~15 万行 | **Rust** |
| **TLS 依赖（已查证）** | `QUIC_TLS_LIB` 默认 **quictls**；Windows 可用 **schannel** | **picotls** —— 自包含 TLS 1.3 实现，`PICOQUIC_FETCH_PTLS` 可拉取 | `check_symbol_exists(SSL_set_quic_tls_cbs)` → **必须 OpenSSL 3.2+** / LibreSSL / quictls / wolfssl | **BoringSSL**（或打 quic 补丁的 OpenSSL） | BoringSSL（内置） |
| **本机 OpenSSL 3.0.13 够用？** | ❌ 需先编 quictls | ✅ **够**（picotls 自带 TLS，系统 OpenSSL 只用于证书/哈希） | ❌ 需 3.2+ | ❌ 需 BoringSSL | ❌ 需 Rust + BoringSSL |
| **IO 模型** | 回调 + **自带工作线程池**，需适配到我们的 Fiber/IOManager | **自带单线程事件循环**（`picoquic_start_client_cnx` + `picoquic_network_thread`），可替换为我们的 `Transport::Read/Write` | **纯传输层，完全不绑 IO** —— 只要求你喂时间戳和收发字节 | 自带事件循环 | 需 Rust FFI 桥 |
| **API 简洁度（薄封装难度）** | ⭐⭐ 较重（`QUIC_API_TABLE` 函数表、分平台） | ⭐⭐⭐⭐ 好（C 函数 + 显式状态机，研究导向） | ⭐⭐⭐⭐⭐ 最好（纯传输层，最易接 `Transport`） | ⭐⭐⭐ 中 | ⭐⭐（跨语言） |
| **构建依赖** | CMake 3.20+、quictls、PowerShell(Windows) | CMake + **picotls（需网络）** + OpenSSL(证书) | CMake 3.20+、**OpenSSL 3.2+** / wolfssl / quictls | CMake + BoringSSL | Rust 工具链 + BoringSSL |

## 二、推荐结论：**picoquic**

**理由（按权重排序）**：

1. **唯一不需要更换系统 TLS 的库** —— picotls 是它自带的 TLS 1.3 实现，
   系统 OpenSSL 3.0.13 只用来做证书/哈希，**不碰 QUIC API**。
   其他三个要么必须 OpenSSL 3.2+（ngtcp2）、要么必须 quictls/BoringSSL（msquic/lsquic）。
2. **代码量最小**（176K vs msquic 30 万行），且**专为研究设计**，函数命名直白。
3. 有 `picoquic_sample.c` 级别的示例，适合照着写薄封装。

**次选：ngtcp2** —— **架构上它才是最理想的**（纯传输层，不绑 IO，接 `Transport` 最自然），
但**硬约束是必须 OpenSSL 3.2+**。如果允许升级/自编 OpenSSL，它比 picoquic 更适合薄封装。

## 三、编译尝试结果（如实）

### msquic
- 首次 `clone --recursive` **超时**（子模块太大），留下 1.4G 不完整目录（`CMakeLists.txt` 缺失）
- 重新 `clone --depth 1`（无子模块）**成功**：`E:\CodexWorkspace\msquic2`，**33 MB**
- `CMakeLists.txt` 与 `src/inc/msquic.h` 都在
- **未尝试编译**，因为已查出 `QUIC_TLS_LIB` 默认 `quictls`，本机没有

### picoquic
- `cmake -DPICOQUIC_FETCH_PTLS=ON` **失败**，卡点非常具体：
  ```
  CMake Error at picotls-populate-prefix/tmp/picotls-populate-gitclone.cmake:39
  CMake Error at FetchContent.cmake:1679 (FetchContent_MakeAvailable)
  ```
- **根因**：`FetchContent` 在 **WSL 内**执行 `git clone` 拉 picotls，而
  **WSL 里 GitHub 解析成 `::1`，clone 必然失败**（这是本项目的已知环境限制）
- **解决路径明确**：在 **Windows 侧** clone picotls，然后让 picoquic 指向本地目录
  （或手工把 picotls 放进 `picoquic/picotls/`）

### ngtcp2
- 未尝试编译。CMakeLists 里 `check_symbol_exists(SSL_set_quic_tls_cbs ...)` 已明确
  要求 OpenSSL 3.2+ 的 QUIC API，本机 3.0.13 不具备。

## 四、要做成需要的前置条件与成本

| 路线 | 前置条件 | 预估成本 |
|---|---|---|
**picoquic（推荐）** | Windows 侧 clone `picotls` 放进 `picoquic/picotls/`（或改 CMake 指向本地） | **编译 1~2 小时**（picotls + picoquic 首次构建）+ **薄封装 4~8 小时** |
**ngtcp2** | 自编 OpenSSL 3.2+ 或 quictls（约 30~60 分钟编译）+ ngtcp2 | 编译 **2~3 小时** + 薄封装 **4~6 小时**（架构最顺） |
**msquic** | 自编 quictls | 编译 **2~4 小时**（平台抽象层重）+ 薄封装 **8~16 小时** |
**lsquic** | 自编 BoringSSL | 编译 **3 小时+** + 薄封装 **8 小时** |

## 五、薄封装接口设计（以 picoquic / ngtcp2 为例）

我们的已有抽象是 `Transport`（`Read`/`Write`，见 `network/http2/transport.hpp`），
所以封装应该**复用这个抽象**，让 QUIC 也能跑在 Fiber 上：

```cpp
namespace go2cpp::net::quic {

// 客户端（或服务端）连接
class Connection {
public:
    // 绑定一个 Transport；QUIC 的收发全部经它
    Connection(Transport* transport, Role role, const QuicConfig& config);

    // 协商（阻塞当前 Fiber）
    bool Connect(const std::string& sni, std::string* error);

    // 打开一个流（QUIC 的多路复用，对应 Go 的 quic-go Stream）
    Stream* OpenStream(std::string* error);

    // 驱动一次收发（内部把 transport 的字节喂给库，再取出库要发的字节）
    bool Poll(std::string* error);

private:
    // picoquic_cnx_t* 或 ngtcp2_conn* —— 库的句柄
    void* m_native{nullptr};
};

class Stream {
public:
    // 同步阻塞语义（与 HTTP/2 的 Stream 一致）
    std::size_t Read(std::uint8_t* buf, std::size_t cap, std::string* error);
    std::size_t Write(const std::uint8_t* data, std::size_t len, bool fin,
                      std::string* error);
};

}  // namespace go2cpp::net::quic
```

**关键设计点**：

1. **`Poll()` 是唯一需要适配的地方** —— 把库的"事件循环"拆成
   "喂字节 → 取出要发的字节"，这样上层用 Fiber 驱动，不引入第二个线程。
2. ngtcp2 天生就是这种形状（`ngtcp2_conn_read_pkt` / `ngtcp2_conn_writev_stream`），
   picoquic 需要把它的 `picoquic_network_thread` 换成手动驱动。
3. **HTTP/3 不在这层** —— 它是 QUIC 之上的一层（QPACK + 帧），
   建议单独做，或者直接复用库自带的（picoquic 有 `picohttp` 示例）。

## 六、重要提醒

- **本机 `network/quic/` 下已有的自实现（varint/包头/帧层）应视为参考实现**，
  已在 `docs/quic.md` 标注"不可用于生产"。集成真库后**建议删除或隔离**，
  否则两套 QUIC 代码共存会引起误解。
- 本次**没有留下任何半成品代码**：`msquic2` 与 `picoquic` 都在
  `E:\CodexWorkspace\` 外部目录，**未进项目**。


## 七、补充：验证进展（本次新增）

### picotls 的网络限制已被绕过

- **Windows 侧** `git clone --depth 1 https://github.com/h2o/picotls.git` → **成功**
  （`E:\CodexWorkspace\picotls`）
- 复制到 `picoquic/picotls/` → **成功**
- 这证明：**WSL 内 FetchContent 的失败是可以绕过的** —— 在 Windows 侧克隆再放进去即可。

### 但 configure 仍失败，卡点已前移一步

```
Could NOT find PTLS (missing: PTLS_CORE_LIBRARY PTLS_OPENSSL_LIBRARY)
```

**原因**：`PICOQUIC_FETCH_PTLS=OFF` 时期望 picotls **已经编译成库**
（找 `libpicotls-core` / `libpicotls-openssl`），而不是源码目录。

**所以正确顺序是**：
1. 在 Windows 侧克隆 picotls（**已完成**）
2. 放进 `picoquic/picotls/`（**已完成**）
3. **先编译 picotls**，产出 `libpicotls-core.a` + `libpicotls-openssl.a`
4. 再 configure picoquic，用 `-DCMAKE_PREFIX_PATH` 或 `-DPTLS_DIR` 指向这些库

**第 3、4 步预估 5~15 分钟**（picotls 本身不大），本次时间窗不够。

## 八、结论（最终）

| 项 | 结论 |
|---|---|
**选型** | **picoquic 是唯一在本机 OpenSSL 3.0.13 下可行的库**（picotls 自带 TLS 1.3） |
**网络限制** | 已绕过（Windows 侧 clone + 复制） |
**剩余卡点** | 需先编译 picotls 再 configure picoquic，约 15 分钟 |
**薄封装设计** | 见第五节（`Connection` + `Stream`，复用 `Transport` 抽象，`Poll()` 是唯一适配点） |
**代码是否进项目** | **没有**。`msquic2` / `picoquic` / `picotls` 全在 `E:\CodexWorkspace\` 外部目录 |
**未留半成品** | ✅ 项目里只有已标注"参考实现、不可用于生产"的 `network/quic/`，与本次无关 |
