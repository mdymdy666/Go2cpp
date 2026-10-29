# crypto / x25519 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/crypto.hpp`、`src/bento/net/crypto.cpp`、
  `include/go2cpp/bento/net/x25519.hpp`、`src/bento/net/x25519.cpp`、
  `tests/bento/test_crypto.cpp`、`tests/bento/test_x25519.cpp`、
  `tests/bento/test_x25519_handshake.cpp`
- 审计工具：`tests/bento/audit_crypto_stress.cpp`（独立程序，不注册 CTest，
  与既有 `audit_*_stress.cpp` 一致；工作树中已存在且未跟踪）
- 构建：`.build-bento-wsl`（Ninja，Release `-O3 -DNDEBUG`，
  `-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU / 19 GiB，GCC 13.3.0，内核 6.6.87.2
- 结论：**无 P0**。确认并保留 1 个 P1 修复（0-RTT 重放窗口，本次补测）；
  修复 4 个 P2（线格式文档、票据/指纹敏感清零、`OpenFrame` 失败残留明文、
  审计工具随机篡改抖动）；**记录 2 个 P1（均在 `link.cpp`，按边界未改）**
  与 4 个 P2 / 5 个 P3。百万级 Seal/Open、1000+ 随机、5 个场景、
  4 线程并发全部通过；ASan+UBSan、valgrind 无内存错误与泄漏。

## 1. 压测数据

单次运行、`taskset -c 2` 绑定单核；WSL2 计时噪声约 ±5–8%。所有 AEAD/帧项目
各 **1,000,000 次**，规模满足 ≥100 万次要求。1400B 为 AEAD 原语（协议单帧
载荷上限 1300B，帧级另测 1300B）。

### 1.1 AEAD 原语（ChaCha20-Poly1305，含 AAD=0）

| 明文 | Seal MB/s | Open MB/s | Seal ns/op | Open ns/op | CPU |
|---:|---:|---:|---:|---:|---:|
| 32B | 114.3 | 108.3 | 280.0 | 295.4 | 95.8% |
| 256B | 404.7 | 390.0 | 632.6 | 656.4 | 95.8–95.9% |
| 1400B | 499.7 | 494.9 | 2801.5 | 2828.9 | 97.2–98.4% |

未绑核同规模复跑（对照）：32B 123.4/121.3、256B 437.2/419.1、
1400B 518.1/487.6 MB/s，证明结果对调度不敏感、量级稳定。

### 1.2 帧级 SealFrame/OpenFrame（含 16B 外层头与 16B tag）

| 载荷 | Seal MB/s（payload / wire） | Open MB/s（payload / wire） | Seal / Open ns/op |
|---:|---:|---:|---:|
| 32B | 104.7 / 261.6 | 103.7 / 259.3 | 305.8 / 308.5 |
| 256B | 335.9 / 398.9 | 339.8 / 403.5 | 762.1 / 753.5 |
| 1300B | 494.6 / 512.9 | 476.6 / 494.2 | 2628.1 / 2727.8 |

### 1.3 其他原语

| 项目 | 结果 |
|---|---|
| ChaCha20 流加密（1 MiB 缓冲） | **830.4 MB/s**（1.26 ms/MiB） |
| `DeriveSessionKeyWithX25519` | 518.2 ns/op（1.93 Mops/s） |
| HChaCha20 单次 | 78.1 ns（12.80 Mops/s） |
| X25519 标量乘 | **306.8 µs/op**（20,000 次 6.14 s） |
| 重放窗口混合读写 | 3.2 ns/op（308 Mops/s；350 万接受 / 150 万拒绝） |

### 1.4 内存、并发与全量运行

- `audit_crypto_stress 1.0 --mode=all`：**313,976 checks / 0 failures**，
  25.4 s；RSS 4.2 MB，峰值 5.9 MB。
- 泄漏：2,000 轮 Seal/Open + X25519，RSS 前/后均为 4,228 KB，**delta 0 KB**。
- 并发：4 线程 × 20,000 次 Seal/Open + 每 500 轮 X25519，failures=0。
- ASan+UBSan：`audit 0.02 --mode=all` **6,852 checks / 0 failures**，
  同一进程内包含 5 个场景与并发，无任何 sanitizer 报告。
- valgrind：`bento_test_crypto` **0 errors / 0 leaks**（“All heap blocks were
  freed”）；仅吞吐断言在约 31 MB/s 下失败，见 P3-1。

### 1.5 改动前后 A/B（同一 audit 程序，HEAD `crypto.cpp` vs 当前）

| 项目 | HEAD | 当前 | 变化 |
|---|---:|---:|---:|
| ChaCha20 1 MiB | 708.9 MB/s | 830.4 MB/s | **+17.1%** |
| AEAD 1400B seal / open | 485.8 / 457.1 | 499.7 / 494.9 | +2.9% / +8.3% |
| AEAD 256B seal / open | 389.7 / 352.2 | 404.7 / 390.0 | +3.8% / +10.7% |
| AEAD 32B seal / open | 125.8 / 120.3 | 114.3 / 108.3 | **−9.1% / −10.0%** |
| KDF mix / X25519 | 500.9 ns / 286.9 µs | 518.2 ns / 306.8 µs | 运行噪声 |

结论：4 路并行只对大缓冲有效（1 MiB +17%），代码原注释“1.6~2 倍”不成立，
已改为实测值；32B 小包不用 4 路路径，约 9% 回退来自新增的敏感数据清零
（每 op 约 200B volatile 写）。这是可接受的安全成本，但需在报告中记录。

## 2. 随机测试（1000+）

| 项目 | 规模 | 结果 |
|---|---|---|
| AEAD 随机密钥/nonce/明文/AAD | 1,000 轮，明文 0–1024B，AAD 0–64B | 往返全通过；1B/多字节/截断/tag 四类篡改 `accepted=0`；错误密钥全部拒绝 |
| 帧级随机篡改 | 1,000 轮，载荷 1–512B | `accepted_tamper=0`，1,000 次全部拒绝；失败状态均落在 kAuthFailed/kMalformed/kReplay/kNotEncrypted |
| 票据随机篡改 | 500 轮 | 单 bit 翻转、错误 PSK、长度减 1 全部拒绝，`accepted=0` |
| 重放窗口随机 | 1,000 + 2,000 轮 | `InitializeBefore` 后旧序号 `stale accepted=0`；乱序接受与重复拒绝均符合预期 |
| 第二随机种子 | `--seed=0x12345678`，同规模 | 117,107 checks / 0 failures，四类 `accepted=0` |

认证失败时的明文泄露检查使用哨兵（0xA5/0x5A），逐字节确认调用方缓冲
**未被写入任何明文**；`AeadOpen` 先常量时间比较 tag，通过后才解密。

## 3. 复杂场景（5 个，全部通过）

| # | 场景 | 关键证据 |
|---|---|---|
| 1 | PSK + X25519 加密握手，双向数据 | 3 轮；双方 `x25519_handshakes=1`、`psk_fallbacks=0`；100+100 条消息双向完整；`auth_failures=0`、`replay_drops=0`；X25519 会话 `ExportSessionTicket()` 为空（不降级 0-RTT） |
| 2 | 篡改 / 重放（64 位滑动窗口） | 100 帧，顺序 1..50 后跳到 100，再乱序 99..60 全部接受；51 仍可接受（age 49），36 已滑出（age 64）拒绝；重放 100/50/60 均 kReplay；1B/多字节/截断/tag 四类篡改 kAuthFailed/kMalformed 且输出为空；伪造 seq=100000 认证失败后 `highest` 保持 42，seq=43 仍可接受（窗口未被 DoS 推高） |
| 3 | 前向保密 | 10 轮；仅持 PSK + 双方公钥 + 双方 nonce 无法打开会话（`leaked=0`）；把公钥当共享密钥、方向反转、全零共享密钥、公钥翻 1 bit 均派生不同密钥并认证失败 |
| 4 | 低阶点拒绝 | 7 个点（u=0/1/p-1/p/p+1 + 两个 order-8 大点）× 20 个随机私钥全部拒绝且输出清零；Link 层发送 u=1、p-1、order8 三种 HELLO：不回 ACK、`auth_failures>=3`、`x25519_handshakes=0`、`psk_fallbacks=0` |
| 5 | 旧端降级 | 3 轮，交替 36B 旧 HELLO 与 68B 全零公钥 HELLO；服务端均回 52B 旧布局 ACK、公钥全零、`psk_fallbacks=1`；随后用纯 PSK 派生密钥注入 STREAM，服务端正确收到载荷 |

## 4. 解耦检查

- 头文件自包含：仅 `#include "go2cpp/bento/net/crypto.hpp"` 或
  `x25519.hpp` 的独立 TU 在 `-std=c++17 -Wall -Wextra -Wpedantic -Werror`
  下编译通过。
- `crypto.cpp` 依赖：标准库 `<algorithm> <cstring> <random>` +
  `crypto.hpp`、`protocol.hpp`、`x25519.hpp`；`x25519.cpp` 仅标准库
  `<cstring> <random>` + `x25519.hpp`。
- `g++ -MM` 只列出上述 3 个项目内头文件；`nm -C -u` 的未定义符号只有
  libc/libstdc++（`memmove`/`memset`/`random_device`/`operator new`）与
  `protocol.cpp` 的 `BuildFrame/ParseFrame/DecodeHeader/EncodeHeader`。
- 无 OpenSSL/libsodium/第三方密码库；无 hash 依赖——KDF 用 HChaCha20，
  不需要 SHA。`crypto.hpp` 依赖 `protocol.hpp` 仅为了帧头常量与
  `kFlagEncrypted`，不依赖 link/transport/logging，可单独测试。

## 5. 问题清单

### P0

无。未发现无需 PSK 即可恢复会话密钥、伪造认证帧或绕过低阶点检查的问题。

### P1-1（已修复，工作树既有改动；本次复核并补测）0-RTT 恢复后旧序号可重放

- 证据：`InitializeBefore(next_seq)` 原先只把 `m_bitmap` 置为 1，即只标记
  `highest=next_seq-1` 本身；窗口内 age 1..63 的旧会话序号仍通过
  `WouldAccept`。0-RTT 票据是纯 PSK 派生（`link.cpp:1706/1714` 调用），
  旧会话密钥在恢复后仍然有效，因此最多 63 个旧包可被重放。
- 修复：`src/bento/net/crypto.cpp:642-655` 改为 `m_bitmap = ~0ULL`，把整个
  窗口标记为已消费；新序号 > highest 仍正常接受。`crypto.hpp:226-229` 的
  语义说明与实现一致。
- 验证：`test_crypto` 新增 age 0..63 全拒绝、跳号接收、32 位回绕附近初始化；
  `audit` random-replay 3,000 轮 `stale accepted=0`；`bento_zero_rtt` 通过。

### P1-2（记录，越界：`link.cpp`）32 位 wire seq 回绕后 (key, nonce) 重用

- 证据：`SealAndSend` 用 `seq = m_tx_wire_seq + 1; if (seq == 0) seq = 1;`
  （`link.cpp:1191-1199`），无 rekey/关闭；AEAD nonce 为
  `direction || seq`（`crypto.cpp:627-632`）。`ReplayWindow` 按有符号 32 位
  差值把回绕后的 1 当作新序号接受。
- 影响：2^32 帧后同一会话密钥重复使用同一 nonce。按 1400B/500 Mbps 估算
  约 **26.7 小时**（1000 fps 约 49.7 天）。攻击者录到首帧与回绕帧即可得
  两帧明文异或，并复用 Poly1305 一次性密钥；回绕瞬间可注入 1 个伪造帧。
- 建议：tx seq 到高水位（如 0xFFFFFF00）强制重握手/关闭，或协议升级为
  64 位 seq。按任务边界未改 `link.cpp`。

### P1-3（记录，越界：`link.cpp`）握手密钥静态 + 32 位随机 seq，生日碰撞导致 nonce 重用

- 证据：握手密钥仅由 `DeriveHandshakeKey(psk, direction)` 派生
  （`crypto.cpp:556-562`），nonce = `direction || seq`（`crypto.cpp:627`），
  而 HELLO/HELLO_ACK 的 seq 是 32 位随机数（`link.cpp:639/2308`，
  `RandomNonZeroU32` 见 `link.cpp:64-72`）。
- 影响：同一 PSK 部署下约 **7.7 万次握手即有 50% 概率发生 seq 碰撞**；
  碰撞使两次不同 HELLO 共用 (key, nonce)：握手明文异或泄露，Poly1305
  一次性密钥复用可伪造 HELLO（如全零 X25519 公钥触发 PSK 回退），造成
  会话重置/绑定 DoS；不泄露 PSK 或会话密钥。
- 建议：把连接级随机盐放进明文外层头并混入握手密钥（需协议版本升级），
  或使用持久化 32 位计数器 + epoch，或缩短 PSK 轮换周期。crypto 内部
  单独改会破坏旧端互通，故按边界记录。

### P2-1（已修复）`crypto.hpp` 线格式文档与实际不符

- 证据：文档写“外层 12B 头 / ver=1 / seq 偏移 4”，实际
  `kFrameHeaderBytes=16`、`kProtocolVersion=2`（`protocol.hpp:67/70`），
  外层头含 4B conn_id、seq 在偏移 8、密文从 16 开始。
- 修复：`crypto.hpp:26-41` 更正为 16B 头、ver=2、conn_id、偏移 8/12/16。

### P2-2（已修复）敏感中间值清零不完整

- 证据：`TicketMac` 的 `ticket_key`/`one_time_key`、`DerivePskFingerprint`
  的 `digest`、一次性 `Poly1305` 的 ctx 与部分块未清零；而 `AeadSeal/Open`
  与握手密钥已在工作树改动中清零。
- 修复：`crypto.cpp` 在 `TicketMac`、`DerivePskFingerprint`、`Poly1305`
  返回前调用 `SecureZero`。不影响输出（KAT/随机测试复跑通过）。

### P2-3（已修复）`OpenFrame` 早退路径残留上一次明文

- 证据：`inner->assign()` 只在通过长度检查后执行；重放/畸形等早退路径
  不清空调用方缓冲，复用 vector 的调用方可能读到上一包明文。
- 修复：`crypto.cpp:913-925` 入口先 `inner->clear()`；`crypto.hpp:311`
  注明“失败时不残留旧明文”。认证失败路径仍填零，语义更强。

### P2-4（记录）`OpenFrame` 未校验内外层 type/conn_id 一致

- 证据：外层 type/conn_id 参与 AAD，内层在密文中，但两者可以不同；
  只有持有密钥的一端能构造。可能导致按外层 conn_id 路由、按内层头解析
  的语义错位。
- 建议：解密后校验 `outer.type == inner.header.type` 且
  `outer.conn_id == inner.header.conn_id`；改动可能影响测试夹具，先记录。

### P2-5（记录）底层 AEAD/流加密 API 的空指针前置条件不一致

- `AeadSeal` 返回 void、`AeadOpen` 返回 bool、`ChaCha20Xor`/`Poly1305`
  在 `bytes>0` 且缓冲为空时会解引用；头文件未统一说明。内部调用点均
  合法，但作为可复用原语建议补文档或统一防御性返回（`AeadSeal` 无法
  报错，静默返回更危险，故只记录）。

### P2-6（记录）0-RTT 票据一次性 Poly1305 密钥的输入过窄

- `TicketMac` 的 HChaCha20 输入是 `conn_id|session|issued_ms`（16B）；
  同一毫秒内两次导出且 seq 变化时会重用一次性密钥（仅 PSK 持有者可
  制造，影响有限）。票据也未绑定方向/角色，存在反射语义。
- 建议：把完整票据体（或 expires+双 nonce+双 seq）摘要后作为
  HChaCha20 输入，并在明文里加角色位。

### P2-7（记录）HELLO 重放缓存仅 8 条 + 30s 时间窗

- 证据：`link.hpp:561`（`kMaxRememberedClientNonces=8`）、
  `link.cpp:2085`；重放的旧 HELLO 在 8 次更新握手后会被移出缓存，
  时间戳仍在 30s 窗口内时会被当作新会话处理。X25519 路径因临时私钥
  已清零而失败；纯 PSK 回退路径可被重置/绑定，形成 DoS。
- 建议：扩大缓存（如 64/256）或按 session_id 维护高水位。

### P2-8（已修复，审计工具）随机篡改抖动与零迭代

- 多字节 XOR 篡改可能互相抵消，原断言会在约 4–5% 的运行中把“未真正
  篡改”误判为漏检；`scale=0` 时压测循环空转、`inf ns/op`。
- 修复：`audit_crypto_stress.cpp` 对“密文与 tag 均未变化”的轮次跳过；
  所有迭代/轮数改为 `std::max(1, ...)`，`scale<=0/NaN` 回落 1.0。

### P3（记录）

1. `test_crypto` 的 `seal/open_mbps >= 50` 硬阈值在 valgrind 下必然失败
   （约 31 MB/s），ASan 下通过（63.1 MB/s），原生 531+ MB/s；属环境假阴性，
   建议 valgrind 只跑 audit 小规模，或按 instrumentation 放宽阈值。
2. `ReplayWindow` 用 `static_cast<int32_t>(seq - highest)` 实现回绕比较，
   依赖二进制补码（GCC/Clang 均满足，已注释）。
3. `ChaCha20Xor` 的 `counter + offset/64` 超过 2^32 块会回绕；当前帧上限
   1500B，不可达，但 RFC 8439 的 256 GiB 上限未显式断言。
4. `SecureRandomBytes`/`FillRandom` 每次构造 `std::random_device`（握手
   低频路径，开销可接受）；可改为 `thread_local` 复用。
5. `X25519GeneratePrivateKey` 在熵源不可用时可能抛异常（头文件已说明）。

## 6. 测试复查

| 测试 | checks | 覆盖 | SKIP |
|---|---:|---|---|
| `test_crypto` | **5,482** | RFC 8439 ChaCha20/Poly1305/AEAD 向量、HChaCha20 KAT、KDF 域分离、窗口/初始化/回绕、12 种长度、原地、1000 轮随机篡改、200 轮帧篡改、参数矩阵、票据、并发、吞吐 | 无 |
| `test_x25519` | **2,591** | RFC 7748 向量（含 1000 轮迭代）、clamp、最高位忽略、低阶点、200 随机密钥对、mod p 等价、极值私钥、64 个唯一私钥、4 线程 | 无 |
| `test_x25519_handshake` | 通过 | HELLO 新旧布局、KDF 分离、前向保密、低阶点、Link 握手、0-RTT 不导出 X25519 票据 | 无 |
| `audit_crypto_stress` | 313,976（全量） | 百万级压测、随机、5 场景、并发、泄漏 | 不适用（独立工具） |

- 断言有效性：否定用例断言具体 `OpenStatus`（而非仅 `false`）；随机篡改
  断言 `accepted==0`；认证失败用哨兵逐字节确认未写明文；窗口测试同时断言
  `highest` 与 `WouldAccept`。未发现“恒真断言”或吞掉失败的分支。
- SKIP 逃逸：`test_util.hpp` 提供 `Skip()`/返回码 77，但三份 crypto/x25519
  测试**均未使用**；本次也未新增任何 SKIP。
- 本次补测：0-RTT 窗口全 64 位旧序号、跳号与回绕、HChaCha20 已知答案
  （draft-irtf-cfrg-xchacha-03 §2.2.1）、`ConstantTimeEqual16`、PSK 指纹、
  握手帧 null/0 参数、认证失败不推进窗口；并发测试改为每线程独立 seq，
  避免测试自身构成 (key, nonce) 重用。
- 剩余缺口（记录）：① 没有 Link 层“0-RTT 恢复后重放旧数据帧”的集成用例；
  ② 32 位 seq 回绕无法在链路层测（需 4×2^32 帧）；③ 未测外层/内层
  type、conn_id 不一致；④ 未测 `AeadSeal/Open` 的空指针 + 非零长度；
  ⑤ `SecureRandomBytes` 只有唯一性测试，无统计性测试。

## 7. 验证命令与结果

```bash
# 构建（零警告，-Werror）
cmake --build .build-bento-wsl -j 8

# 全量 bento 回归
cd .build-bento-wsl && ctest -R bento --output-on-failure
# 48/48：47 passed + bento_netem Skipped（无 CAP_NET_ADMIN，按设计）

# 百万级审计（默认 seed；含 5 场景/并发/泄漏）
./.build-bento-wsl/audit_crypto_stress 1.0 --mode=all
# [SUMMARY] checks=313976 failures=0 elapsed=25.399s rss=4228KB peak=5888KB

# 单核压测（报告表格数据）
taskset -c 2 ./.build-bento-wsl/audit_crypto_stress 1.0 --mode=bench

# 第二随机种子
./.build-bento-wsl/audit_crypto_stress 1.0 --mode=random --seed=0x12345678
# 1000/1000/500/1000 轮，accepted=0，117107 checks，0 failures

# ASan + UBSan
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Iinclude -Itests/bento \
  tests/bento/test_crypto.cpp src/bento/net/{crypto,protocol,x25519}.cpp -o /tmp/t_asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/t_asan
# [PASS] test_crypto (5482 checks)

# valgrind
valgrind --leak-check=full --errors-for-leak-kinds=definite \
  .build-bento-wsl/bento_test_crypto
# 0 errors；0 leaks；仅 50MB/s 吞吐阈值在 valgrind 下失败（见 P3-1）

# 头文件自包含 + 依赖
g++ -std=c++17 -Iinclude -Wall -Wextra -Wpedantic -Werror -fsyntax-only /tmp/self_crypto.cpp
g++ -std=c++17 -Iinclude -MM src/bento/net/crypto.cpp
```

## 8. 改动文件

- `include/go2cpp/bento/net/crypto.hpp`：线格式文档更正（P2-1）、
  `OpenFrame` 输出缓冲语义说明（P2-3）。
- `src/bento/net/crypto.cpp`：保留 4 路 ChaCha20 与 32 位 `random_device`
  改动；`InitializeBefore` 整窗标记（P1-1）；票据/指纹/Poly1305 清零
  （P2-2）；`OpenFrame` 入口清空（P2-3）；4 路并行注释改为实测值。
- `src/bento/net/x25519.cpp`：验证既有 `Zeroize` 改动（无新增修改）。
- `tests/bento/test_crypto.cpp`：+~500 行（0-RTT/回绕/KAT/随机/边界/并发）。
- `tests/bento/test_x25519.cpp`：验证既有 146 行补测（无新增修改）。
- `tests/bento/audit_crypto_stress.cpp`：独立审计程序（1,467 行，
  未跟踪，不注册 CTest）；修复随机篡改抖动与零迭代（P2-8）。
- 未 commit；未改 `link.cpp`、`protocol.hpp` 及 base 模块。
