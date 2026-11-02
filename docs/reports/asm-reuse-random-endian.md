> 来源：/tmp/reuse_p0p1.md，2026-11-02 归档

# 复用 P0 落地报告：安全随机统一 + 大端读写抽取

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树；HEAD `ee2c77a`，工作树已有并行任务的未提交改动）
- 独立构建目录：`/tmp/build-reuse`（未触碰 `build-review` / `build-channel-audit`）
- 日期：2026-11-02
- 依据：`docs/reports/reuse-audit.md` 的 T3（安全随机）与 T1（大端读写）
- 边界遵守：只改 `include/go2cpp/{bento,p2p,network,app,hash}/**`、`src/{bento,p2p,network,app,hash}/**`、`tests/**`；**未 commit**；未改 `cmake/`、`docs/`、`AI_COLLAB.md`

---

## 任务 1：安全随机统一（P0，安全隐患）

### 问题
`bento::SecureRandomBytes`（`std::random_device`）与 `p2p::SecureRandomBytes`（`getrandom`）
**同名同签名、安全强度不同**；另有 X25519 私钥、配对 token 两处直接 `random_device`。

### 落地（5 处 → 唯一实现 `go::crypto::rand`）

| # | 位置 | 改动 |
|---|---|---|
| 1 | `src/bento/net/crypto.cpp` `SecureRandomBytes` | 委托 `go::crypto::rand::Read`；短读抛 `std::runtime_error`（原来 `random_device` 失败也会抛） |
| 2 | `src/bento/net/x25519.cpp` `FillRandom`（X25519 私钥） | 同上，直接调 `go::crypto::rand::Read` |
| 3 | `src/bento/net/token_pairing.cpp` `RandomU64`（配对 token） | 删除 `thread_local random_device`，改调 `go::crypto::rand::Read` |
| 4 | `src/p2p/random.cpp` `SecureRandomBytes` | 删除自写的 `getrandom`/`/dev/urandom`/确定性填充三级链，改为 `go::crypto::rand::Read`；**保留每线程 1 KiB 池化**（只是批量取回，语义不变） |
| 5 | `include/go2cpp/go/crypto_rand.hpp` | 已是完整 CSPRNG（`getrandom(2)` → `/dev/urandom`，含 Windows 回退），**作为唯一实现不改** |

同时更新 `crypto.hpp` / `x25519.hpp` / `token_pairing.hpp` / `p2p/random.hpp` 的契约注释（熵源、`@throws`）。

**未合并（按审查报告保留）**：`bento/net/coap.cpp` 的 `m_rng_state`（非密码学用途）、
`network/websocket.cpp` 的 splitmix64 掩码密钥（热路径已缓存）。

### 语义差异（明确记录）
- bento 三条路径：系统 CSPRNG **整体**不可用时抛 `std::runtime_error`，**不**静默产出可预测密钥；
  与旧实现（`random_device` 可能抛）在错误面上兼容。
- p2p：`SecureRandomBytes` 头文件契约是「不抛异常」，因此池填充失败仍走确定性兜底
  （仅当 `getrandom` 与 `/dev/urandom` 同时不可用；正常路径不会进入），注释已写明。

### 验证（1e6 字节）
独立程序 `tests/bento/test_secure_random.cpp`：

| 指标 | bento（新） | p2p（新） | 旧 bento（random_device） | 旧 p2p（getrandom） |
|---|---|---|---|---|
| 字节频率 χ²（df=255，期望 255±22.6） | 247.2 | 225.4 | 284.0 | 235.8 |
| 16 字节块重复数（62500 块） | 0 | 0 | — | — |
| 不同字节占比 vs 新实现 | — | 99.61% | 99.62% | 99.61% |

- 两个新实现 1e6 字节均覆盖全部 256 个字节值；
- p2p 池化路径（4 字节/次 × 10 万次 = 40 万字节）χ² = 258.2（合格），跨池边界正常；
- 结论：新旧实现**输出不同但都合格**，且 bento / p2p 现在共享同一熵源。

---

## 任务 2：大端读写抽取（13 处）

### 公共实现
`include/go2cpp/network/endian.hpp` 新增 6 个 inline 函数（`go2cpp::net`，并 `using` 到 `go2cpp`）：

- `ReadBE16/32/64`：`std::memcpy` + `byteswap`，未对齐安全、无严格别名 UB；小端主机编译为 `bswap/movbe`（与原 `app/frame.cpp` 的优化版一致）；
- `WriteBE16/32/64`：逐字节写（与原 13 处实现逐字节一致，编译器会合并为单次 store）。

### 替换清单（保留原函数名，函数体只做委托）

| # | 文件 | 原函数 |
|---|---|---|
| 1 | `src/bento/net/protocol.cpp` | `WriteU16/32`、`ReadU16/32` |
| 2 | `src/bento/net/command.cpp` | `WriteU16/32/64`、`ReadU16/32/64` |
| 3 | `src/bento/net/file_transfer.cpp` | `WriteU16/32/64`、`ReadU16/32/64` |
| 4 | `src/bento/net/pairing.cpp` | `WriteU16/32/64`、`ReadU16/32/64` |
| 5 | `src/bento/net/peer_hub.cpp` | `WriteU32/64`、`ReadU32/64` |
| 6 | `src/bento/net/link.cpp` | `WriteU32/64`、`ReadU32/64` |
| 7 | `src/bento/net/frame.cpp` | `WriteU16/32`、`ReadU16/32` |
| 8 | `src/bento/app/session.cpp` | `WriteU16/32`（vector 追加版）、`ReadU16/32`（带长度校验） |
| 9 | `src/app/frame.cpp` | `LoadBE16/32/64`（保留 `GetU16/32/64` 包装） |
| 10 | `src/app/bento_bridge.cpp` | `WriteU16/32` |
| 11 | `src/p2p/relay_protocol.cpp` | `ReadU16/32`、`StoreU16/32` |
| 12 | `src/hash/sha1.cpp` | `LoadBE32` |
| 13 | `src/hash/sha256.cpp` | `LoadBE32` |
| +1 | `src/bento/net/channel.cpp` | `WriteU16BE/ReadU16BE`（审查清单 T1 漏列，属同一复制粘贴，一并消除） |

`diagnostic_protocol.hpp` 的小端版本语义不同，**未并入**（按报告建议）。

### 验证：逐字节一致
独立程序 `tests/network/test_endian_be.cpp`，基准是「改动前的逐字节实现」逐行副本：

- 固定边界值（含 0、`UINT*_MAX`、`0x80..`、`0xff..`）三档位宽；
- **1e6 字节随机数据的所有窗口**：`ReadBE16/32/64` 与旧实现逐个相等；
- **10 万组随机值**写入结果 `memcmp` 逐字节相等；
- 窗口遍历天然覆盖未对齐地址（offset 1/3/5…）。

结果：`checks=3700051 failures=0`。

---

## 构建与测试

### 构建
```
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-reuse -G Ninja \
  -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BENTO_TESTS=ON \
  -DGO2CPP_WITH_APP=ON -DGO2CPP_APP_TESTS=ON \
  -DGO2CPP_WITH_P2P=ON -DGO2CPP_P2P_TESTS=ON \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/build-reuse --target <39 个测试目标与 5 个库> -j 12
```
- 首个全量目标集：224 步，0 错误；
- 修改 `endian.hpp` 后增量重建：105 步，0 错误；
- bento / p2p / app 目标使用项目自带 `-Wall -Wextra -Wpedantic -Werror`，零警告。

### ctest（串行，最终一次）
```
ctest -R '^(app_(frame|bento|connection|stream_table|resource|kv|load)|bento_(frame|command|file_transfer|pairing|link_loopback|crypto|x25519|x25519_handshake|token_pairing|secure_link|peer_hub|peer_hub_secure|session|session_control|session_table|video_fragment|stream_fragment|transport|udp_transport|zero_rtt|pmtud|heartbeat|fec_codec|default_encryption|secure_injection|keystore)|p2p_test_.*|go2cpp_hash_test|go2cpp_frame_codec_test)$'
```
**39/39 通过，0 失败**（含 `go2cpp_hash_test`、`go2cpp_frame_codec_test`、`bento_frame`、`bento_command`、
`bento_file_transfer`、`bento_video_fragment`、`app_frame`、`app_bento`、`p2p_test_stun/p2p/tcp_punch/interprocess` 等）。

### 一个已知 flaky（与本改动无关）
`bento_peer_hub` 的 FEC 断言 `with.retransmits < without.retransmits` 在 **`ctest -j4` 并行跑 39 个测试**
时偶发失败（一次 off=18/on=31），单独运行 5/5 通过、`ctest` 串行 3/3 通过、最终串行全量 39/39 通过。
失败原因是丢包代理下的重传计数对系统负载/时序敏感（`LossProxy` 用固定种子，与随机熵源无关），
本次改动不改变任何字节语义（已用 370 万次对拍证明）。

### 独立验证程序（不在 ctest 中）
```
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
    tests/network/test_endian_be.cpp -o /tmp/test_endian_be && /tmp/test_endian_be
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
    tests/bento/test_secure_random.cpp -o /tmp/test_secure_random \
    -Wl,-rpath,/tmp/build-reuse -L/tmp/build-reuse \
    /tmp/build-reuse/libgo2cpp_bento.a /tmp/build-reuse/libgo2cpp_p2p.so \
    /tmp/build-reuse/libgo2cpp_hash.so -lpthread && /tmp/test_secure_random
```
两个新增测试源文件已放进 `tests/`，但**未注册 ctest**（`cmake/` 不在本次改动边界）；
后续把二者加入 `cmake/Go2CppBento.cmake` / `Go2CppTests.cmake` 即可变成常规回归。

---

## 遗留与需要注意的点

1. **`src/hash/sha256.cpp` 与并行的「SHA-NI 下沉」任务共用**：本次只加了 `#include` 与
   `LoadBE32` 委托两处小改，若对方后续重写该文件需保留这两处（否则 endian 对拍仍成立，
   只是该文件的重复实现会回来）。
2. 工作树 `git status` 里还有其它并行任务的改动（`logging/`、`reflect/`、`compress/deflate_reader.hpp`、
   `bento/net/keystore.cpp`、`p2p/{endpoint,candidate}.cpp` 等），**不是本次改动**。
3. 未纳入本次的同类实现（边界/语义原因，供后续复用任务参考）：
   - `include/go2cpp/debug/diagnostic_protocol.hpp`：小端（语义不同，应按报告加 `LoadLE*`）；
   - `src/network/http2/frame.cpp`、`src/distributed/transport.cpp`、`src/media/{audio,pipeline,codec}.cpp`、
     `src/p2p/stun_message.cpp`、`src/bento/net/crypto.cpp` 的 LE 存取：不在本次 13 处清单；
   - `src/hash/{md5,sha1,sha256,adler32}.cpp` 的**摘要输出**大端写（属于输出格式，非读取路径）。
4. 未 commit（按任务要求）。
