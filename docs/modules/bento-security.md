# bento-security 模块组：AEAD 加密、X25519 密钥交换、密钥库与握手

> **状态**：现行（三个模块均已交付；`crypto` 与 `x25519` 已在 `Link` 握手中启用）。
> **日期**：2026-10-28；**适用基线**：`afc135a`（`feat(bento): X25519 前向保密 + 弱网拥塞控制修复`）。
> **源码位置**：`include/go2cpp/bento/net/{crypto,x25519,keystore,link}.hpp`（334 / 128 / 218 / 572 行）、`src/bento/net/{crypto,x25519,keystore,link}.cpp`（963 / 287 / 565 / 2364 行）。
> **引用约定**：`xxx()` 均为基线源码中可检索的真实函数；常量写作 `kXxx`；`Link::` 前缀表示链路层入口，其余属 `go2cpp::bento` 命名空间。
> **测试目录**：`tests/bento/`，ctest 名 `bento_crypto`、`bento_secure_link`、`bento_secure_injection`、`bento_zero_rtt`、`bento_x25519`、`bento_x25519_handshake`、`bento_keystore`。
> **读者**：改动加密/握手代码、做安全评审或排查「握手失败」的人。

## 1. 模块清单

| 模块 | 命名空间 | 头文件 | 实现 | 测试文件 | 职责 |
|---|---|---|---|---|---|
| `crypto` | `go2cpp::bento` | `net/crypto.hpp` | `net/crypto.cpp` | `test_crypto.cpp`、`test_secure_link.cpp`、`test_secure_injection.cpp`、`test_zero_rtt.cpp` | ChaCha20-Poly1305 AEAD、HChaCha20 KDF、握手体编解码、加密帧封解、重放窗口、0-RTT 票据 |
| `x25519` | `go2cpp::bento` | `net/x25519.hpp` | `net/x25519.cpp` | `test_x25519.cpp`、`test_x25519_handshake.cpp` | RFC 7748 ECDH：私钥生成、公钥派生、共享密钥（低阶点拒绝） |
| `keystore` | `go2cpp::bento` | `net/keystore.hpp` | `net/keystore.cpp` | `test_keystore.cpp` | hex/base64 加载、长度校验、脱敏、常量时间比较、轮换保留窗口 |
| 握手（链路层） | `go2cpp::bento` | `net/link.hpp` | `net/link.cpp` | `test_secure_link.cpp`、`test_secure_injection.cpp`、`test_x25519_handshake.cpp` | PSK 握手状态机、HELLO/HELLO_ACK 认证字段、X25519 临时密钥接线、会话密钥安装 |

`keystore` 不依赖 `link`：`ApplyPreSharedKey(target, key)` 以头文件模板适配任意 `SetPreSharedKey(const std::uint8_t*, std::size_t)` 对象。

## 2. 威胁模型与信任边界

**保护什么**

- 线路窃听：握手体与全部数据帧都是 AEAD 密文（`kFlagEncrypted=0x10`），外层帧头作为 AAD 被认证，篡改任一字节都让 `AeadOpen` 失败。
- 无 PSK 的主动注入：伪造/截断/重放/明文注入包在 `OpenFrame`、`OpenHandshakeFrame` 或 `ParseFrame` 处被拒。
- 重放：每方向独立 64 位滑动窗口；只有认证成功的包才推进窗口。
- 密钥误用：`KeyStore` 只接受 16B/32B，拒绝把口令或配置片段当密钥；日志与错误只出现 `Masked()`。
- 静态 PSK 泄漏后的旧会话：X25519 临时密钥把共享密钥混入 PSK，会话密钥不再只由半长期密钥决定。

**不保护的**

- **端点被控**：PSK 与会话密钥在进程内存中，拿到内存即可解密；无 `mlock`/HSM。
- **PSK 分发**：如何把 32B PSK 安全送到两端不在本模块范围（`KeyStore` 只负责读入与保存形式）。
- **身份与证书**：无证书、无签名、无 PKI。双方共用一个 PSK，任何持 PSK 者可冒充任一端；X25519 公钥未认证，只靠 PSK 混合间接绑定。
- **降级到明文**：加密开启后不会退回明文，但允许对端无公钥时回退纯 PSK（`psk_fallbacks`），这是兼容旧对端的显式设计。
- 拒绝服务：窗口机制只防伪造高序号推动窗口，不防带宽耗尽；时间戳校验依赖双方墙钟大致同步。

## 3. API 参考

### 3.1 crypto：AEAD 与密钥派生

| 签名 | 语义 | 安全约束 |
|---|---|---|
| `void AeadSeal(const std::uint8_t key[kChaCha20KeyBytes], const std::uint8_t nonce[kChaCha20NonceBytes], const std::uint8_t* aad, std::size_t aad_bytes, const std::uint8_t* plaintext, std::size_t bytes, std::uint8_t* ciphertext, std::uint8_t tag[kAeadTagBytes]) noexcept` | RFC 8439 §2.8 加密，输出 16B tag | key 32B、nonce 12B；**同一密钥下 nonce 绝不重复**；允许原地加解密 |
| `bool AeadOpen(const std::uint8_t key[kChaCha20KeyBytes], const std::uint8_t nonce[kChaCha20NonceBytes], const std::uint8_t* aad, std::size_t aad_bytes, const std::uint8_t* ciphertext, std::size_t bytes, const std::uint8_t tag[kAeadTagBytes], std::uint8_t* plaintext) noexcept` | 先算 tag 再 `ConstantTimeEqual16` 比较，通过后才解密 | 返回 `false` 时**不保证** `plaintext` 内容；错误密钥与篡改不可区分 |
| `void MakeSecureNonce(SecureDirection direction, std::uint32_t seq, std::uint8_t nonce[kChaCha20NonceBytes]) noexcept` | nonce = 方向 u32 BE ‖ seq u64 BE | seq 形参 32 位，高 4 字节恒 0；方向标签另由 KDF 分离密钥 |
| `void DeriveHandshakeKey(const std::uint8_t psk[kPskBytes], SecureDirection direction, std::uint8_t out[kChaCha20KeyBytes]) noexcept` | `HChaCha20(psk, "BENTO-HS-C2S-001"/"BENTO-HS-S2C-001")` | 握手方向密钥独立，不可当会话密钥 |
| `void DeriveSessionKey(const std::uint8_t psk[kPskBytes], const std::uint8_t client_nonce[kHandshakeNonceBytes], const std::uint8_t server_nonce[kHandshakeNonceBytes], SecureDirection direction, std::uint8_t out[kChaCha20KeyBytes]) noexcept` | 纯 PSK：`HChaCha20` 依次绑定 client_nonce、server_nonce、方向标签 | 不混 X25519；仅兼容回退与 0-RTT 恢复 |
| `void DeriveSessionKeyWithX25519(const std::uint8_t psk[kPskBytes], const std::uint8_t shared[kX25519KeyBytes], const std::uint8_t client_nonce[kHandshakeNonceBytes], const std::uint8_t server_nonce[kHandshakeNonceBytes], SecureDirection direction, std::uint8_t out[kChaCha20KeyBytes]) noexcept` | `HChaCha20(psk,"BENTO-X25519-001")` 后把共享密钥**前后两半**分别混入，再走方向派生 | 两半都参与；`shared` 全零必须先判失败 |
| `void DerivePskFingerprint(const std::uint8_t psk[kPskBytes], std::uint8_t out[8]) noexcept` | `HChaCha20(psk,"BENTO-PSK-FP-01")` 取前 8B | 只用于把票据绑定到 PSK，不是认证凭据 |
| `void SecureRandomBytes(std::uint8_t* out, std::size_t bytes)` | `std::random_device` 填充 | 仅握手/测试低频路径，**不用于每包 nonce** |
| `void SecureZero(void* data, std::size_t bytes) noexcept`、`bool ConstantTimeEqual16(const std::uint8_t* a, const std::uint8_t* b) noexcept` | volatile 清零；16B 定时比较 | 临时密钥必须走 `SecureZero`，普通 `memset` 可能被优化掉 |

注意：**本模块没有 HKDF**。派生是 HChaCha20 链（RFC 7539 §2.3 排列），域分离靠 16B ASCII 标签，不是 RFC 5869 的 `HKDF-Extract/Expand`。

### 3.2 crypto：ReplayWindow

| 签名 | 语义 | 安全约束 |
|---|---|---|
| `static constexpr std::uint32_t ReplayWindow::kWindow = 64` | 窗口位宽 | 乱序容忍上限 64 个序号 |
| `void Reset() noexcept` / `void InitializeBefore(std::uint32_t next_seq) noexcept` | 清空；或设为「下一个期望 seq 为 next_seq」（`<=1` 等价 `Reset()`） | `InitializeBefore` 只用于 0-RTT：旧会话已确认的序号必须视为已消费 |
| `bool WouldAccept(std::uint32_t seq) const noexcept` | 只查询不更新 | `seq==0` 恒 `false` |
| `bool Accept(std::uint32_t seq) noexcept` | 接受并推进窗口，`false` 表示重放/过旧 | 调用前必须已完成 AEAD 认证 |
| `std::uint32_t highest() const noexcept` / `bool initialized() const noexcept` | 观测最高序号与初始化状态 | 无副作用 |

### 3.3 x25519

| 签名 | 语义 | 安全约束 |
|---|---|---|
| `inline constexpr std::size_t kX25519KeyBytes = 32`、`kX25519BasePoint` | 密钥长度与基点 u=9 | 固定 32B，无变长接口 |
| `void X25519GeneratePrivateKey(std::uint8_t private_key[kX25519KeyBytes])` | 生成随机私钥（不 clamp，clamp 在标量乘法内部） | `std::random_device` 可能抛异常，勿放在握手失败路径里 |
| `bool X25519PublicKeyFromPrivate(const std::uint8_t private_key[kX25519KeyBytes], std::uint8_t public_key[kX25519KeyBytes]) noexcept` | 以 u=9 做标量乘法得公钥 | 参数为空返回 `false`；不改私钥 |
| `bool X25519ComputeSharedSecret(const std::uint8_t private_key[kX25519KeyBytes], const std::uint8_t peer_public_key[kX25519KeyBytes], std::uint8_t shared_secret[kX25519KeyBytes]) noexcept` | 计算 ECDH 共享密钥 | 参数为空或结果为**全零（低阶点）**返回 `false` 并清零输出；调用方必须中止握手 |

实现是 TweetNaCl `crypto_scalarmult` 的 C++ 移植（public domain）：16×16-bit limb、Montgomery ladder、常数时间条件交换；对端 u 坐标最高位按 RFC 7748 忽略。

### 3.4 keystore

| 签名 | 语义 | 安全约束 |
|---|---|---|
| `KeyStore::Key::FromBytes(const std::uint8_t* data, std::size_t bytes)`；`bool valid()`、`size()`、`data()` | 从字节构造；有效性、长度、明文指针 | 长度非 16/32 或 `data` 为空返回空 Key；`data()` 暴露明文，禁止写日志 |
| `std::string ToHex()` / `std::string Masked()` | 全文 hex；保留前 4 后 4 字符其余 `*` | `ToHex()` 敏感；`Masked()` 长度同 hex，空 Key 为 `"<empty>"`，可安全记录 |
| `error::ErrorPtr LoadFromString(std::string_view text)` | hex 或 base64（可带 `hex:`/`base64:` 前缀），忽略空白 | 失败返回错误且**保持已加载密钥不变** |
| `error::ErrorPtr LoadFromFile(const std::string& path)` / `LoadFromEnv(const std::string& variable)` | 读文件 / 读环境变量 | 文件权限宽于 0600 只追加 warning 仍加载；变量未设置/为空/编码非法返回错误 |
| `const Key& Current()`、`const Key& Previous()`、`bool HasUsablePrevious()` | 当前、上一代（仅保留窗口内）、上一代是否可用 | `Previous()` 窗口外返回空 Key |
| `bool Accepts(const Key& candidate) const noexcept` | 候选是否匹配当前或窗口内上一代 | 走 `ConstantTimeEquals`，不按内容提前返回 |
| `bool Rotate(const Key& new_key)`；`void SetRotationRetentionMs(std::uint64_t)`、`rotation_retention_ms()` | 新密钥就位、旧密钥进保留窗口；调整/读取窗口（默认 300000ms） | 新 Key 无效返回 `false` 且不改状态；窗口按 `steady_clock` 计时，不受系统时间影响 |
| `static bool ConstantTimeEquals(const Key& a, const Key& b) noexcept`（另有字节串重载） | 常量时间比较，两者均无效视为相等 | 内容比较不短路，总执行 `max(a_bytes,b_bytes)` 轮；长度不等返回 `false` |
| `template <typename Target> bool ApplyPreSharedKey(Target& target, const KeyStore::Key& key)` | 把 Key 交给 `SetPreSharedKey` | 16B Key 对 `Link`/`PeerHub` 返回 `false`（它们要求 32B） |

`KeyStore` **非线程安全**，是配置期对象：加载/轮换与读取必须由调用方串行化。

### 3.5 握手链路 API（`Link`，只列真实存在者）

| 签名 | 语义 | 安全约束 |
|---|---|---|
| `bool Link::SetPreSharedKey(const std::uint8_t* key, std::size_t bytes)` | 设置 32B PSK，启用认证加密 | 长度非 32B 返回 `false` 并保持原模式；须在 `Open`/`Connect` 前调用 |
| `bool secure() const`、`bool session_established() const`、`const SecureStats& secure_stats() const` | 加密模式、会话密钥就绪、加解密计数（含 `x25519_handshakes`、`psk_fallbacks`、`replay_drops`） | 只读观测 |
| `std::vector<std::uint8_t> ExportSessionTicket()` / `bool Resume(const std::vector<std::uint8_t>& ticket)` | 导出 0-RTT 票据 / 用票据恢复并直接进入 `kConnected` | **仅纯 PSK 会话**可用，X25519 会话返回空/`false`；成功后用 `ReplayWindow::InitializeBefore` 恢复序号 |
| `LinkConfig::pre_shared_key`、`enable_x25519`（默认 `true`）、`handshake_replay_window_ms`（默认 30000）、`session_ticket_lifetime_ms`（默认 60000） | 握手配置 | `enable_x25519=false` 时 HELLO/ACK 退回旧线格式 |
| `bool SealFrame(const std::uint8_t key[kChaCha20KeyBytes], SecureDirection direction, std::uint32_t seq, const std::uint8_t* inner, std::size_t bytes, std::vector<std::uint8_t>* out)` / `bool OpenFrame(const std::uint8_t key[kChaCha20KeyBytes], SecureDirection direction, ReplayWindow* replay, const std::uint8_t* packet, std::size_t bytes, std::vector<std::uint8_t>* inner, OpenStatus* status)` | 完整内层帧 ↔ 加密外层帧；握手帧另有 `SealHandshakeFrame`/`OpenHandshakeFrame` | `seq` 非 0；外层 16B 头作 AAD，`conn_id` 明文可见并参与认证；`replay` 非空时只有认证成功才 `Accept` |
| 私有实现：`SendSecureHello()`、`SendSecureHelloAck()`、`HandleSecureHello()`、`HandleSecureHelloAck()`、`GenerateX25519KeyPair()`、`DeriveX25519SessionKeys()`、`ClearX25519Ephemeral()` | 握手收发与 X25519 接线点 | 私有成员，仅用于说明行为，不属于公开契约 |

**帧开销以实现为准**：`kFrameHeaderBytes = 16`（protocol.hpp，v0.2 起含 4B `conn_id`），`kSecureOverheadBytes = 16 + 16 = 32`。`crypto.hpp` 顶部注释与 `PROTOCOL.md` §9.2 的「12B 外层头 / 28B 开销」是历史值，与 `SealFrame`/`OpenFrame` 实现不符。

## 4. 设计：握手流程、密钥派生与前向保密

下文出现的 `RandomNonZeroU32()`、`IsZeroX25519Key()` 是 `link.cpp` 匿名命名空间的**内部函数**（非公开 API）。

1. **客户端发起**：`Connect()` 内 `ResetSecureSession()` → `SecureRandomBytes(m_client_nonce, 16)` → `m_handshake_tx_seq = RandomNonZeroU32()` → `GenerateX25519KeyPair()`（`X25519GeneratePrivateKey` + `X25519PublicKeyFromPrivate`）。生成失败时公钥保持全零，对端按纯 PSK 回退。
2. **HELLO 封装**：`EncodeHelloBody` 写 magic `"BENT"`、version=1、session、16B nonce、8B 墙钟时间戳，末尾追加 32B 临时公钥（旧布局 36B / 新布局 68B）；`SealHandshakeFrame` 用 `DeriveHandshakeKey(psk, kClientToServer)` 做 AEAD，密文缓存进 `m_cached_hello`，重传复用同一密文（幂等且不重用 `(key, nonce)`）。
3. **监听方验包**：`HandleSecureHello` → `OpenHandshakeFrame`（失败按 `kAuthFailed` 计 `auth_failures`，否则 `malformed_frames`）→ `DecodeHelloBody` 校验 session → `TimestampAcceptable`（默认 ±30s）→ 相同 client nonce 的重复 HELLO 幂等重发缓存 ACK → `IsKnownClientNonce` 命中则按重放拒绝（最多记 8 个）。
4. **监听方 X25519**：若 `enable_x25519 && !IsZeroX25519Key(hello.x25519_public)`，本地生成临时密钥并 `X25519ComputeSharedSecret`；任一失败**不发送 ACK**、计 `auth_failures` 并返回（低阶点失败后绝不降级）。成功则 `DeriveSessionKeyWithX25519` 派生 tx/rx，`SecureZero` 共享密钥与临时私钥，计 `x25519_handshakes`。
5. **回退分支**：对端公钥全零（旧版本或显式关闭）走 `DeriveSessionKey`，计 `psk_fallbacks`，线格式同时退到旧长度。
6. **HELLO_ACK**：`HelloAckBody` 回显 client nonce、填 server nonce 与监听方临时公钥，用 `DeriveHandshakeKey(psk, kServerToClient)` 封装；监听方派生完成后即清零私钥，不进入会话状态。
7. **客户端收尾**：`HandleSecureHelloAck` 校验 session 与 client nonce 一致（不一致按 `auth_failures` 拒绝）、时间戳、握手窗口 `WouldAccept`/`Accept`；公钥非零则 `DeriveX25519SessionKeys`（`X25519ComputeSharedSecret` + `DeriveSessionKeyWithX25519` + `ClearX25519Ephemeral`），失败直接返回、**不降级**；公钥零则 `DeriveSessionKey` 并计 `psk_fallbacks`。
8. **数据面**：此后 `SealAndSend` 每帧 `SealFrame`（seq 从 1 递增，发送失败也推进），接收端 `OpenFrame` 解密后再走原有 `ParseFrame`/通道逻辑，可靠性语义不变。

**前向保密的实际边界**：X25519 已接线（`afc135a`）。`m_x25519_private` 只在握手窗口内存在，`ClearX25519Ephemeral()` 用 `SecureZero` 清零；会话密钥混入了**双方**临时公钥算出的共享密钥，因此只掌握静态 PSK 的旁观者无法解出该会话（`test_x25519_handshake.cpp` 的 `forward secrecy` 用例验证「仅公钥 + PSK 无法解密」）。`docs/modules/bento-modules.md` 第 51 行仍写「X25519 集成到 crypto HELLO …… 后续可选」，该记录对应更早提交（`046d39f`），已被 `afc135a` 的代码推翻；本文以代码为准。

**0-RTT 的取舍**：票据固定 92B（`kSessionTicketBytes`），只含 `conn_id`/session/issued/expires/双方 nonce/两个方向的 `client_next_wire_seq`/`server_next_wire_seq`，**不含密钥**；MAC 由 PSK 派生（`HChaCha20(psk,"BENTO-TICKET-01")`，再以票据偏移 8 起的 16B 派生一次性 Poly1305 密钥），拿不到 PSK 就伪造不了。代价是恢复出的会话是**纯 PSK**、没有 X25519 前向保密，因此 `ExportSessionTicket`/`Resume` 对 X25519 会话直接返回空/`false`，宁可放弃 0-RTT 也不静默降级。

## 5. 重放防护

- 每方向一个 `ReplayWindow`（`m_data_rx_window` 与 `m_handshake_rx_window` 相互独立），窗口 64 位：`m_highest` 是收到过的最大 `seq`，`m_bitmap` 的 bit i 表示 `highest - i` 已收到。
- 判定用**有符号 32 位回绕差值**：`delta = static_cast<std::int32_t>(seq - highest)`；`delta > 0` 接受；否则 `age = -delta`，`age >= kWindow` 拒绝，再看 bitmap 对应位。
- 接受更高序号时 `bitmap = shift >= 64 ? 1 : ((bitmap << shift) | 1)`、`highest = seq`；接受窗口内旧序号时置位。`seq == 0` 恒拒绝（`SealFrame` 也不允许 0）。
- 乱序接受范围：`[highest-63, highest]` 内未收到的序号，以及任何更大的序号；更旧的一律拒绝。
- 序号回绕：比较按有符号差值，2^32 回绕后仍能区分新旧包；但窗口只覆盖 64 个序号，当 `highest` 接近 `0xFFFFFFFF` 时，回绕前的低序号旧包与回绕后的新包无法区分（32 位序号空间的固有歧义，单会话需发满 2^32 帧才触及）。
- 拒绝路径：`OpenFrame` 返回 `false` 并写 `OpenStatus`——`kMalformed`（长度/标志/版本/`seq=0` 非法）、`kNotEncrypted`（未置 `kFlagEncrypted`）、`kReplay`（窗口拒绝）、`kAuthFailed`（Poly1305 不匹配）。窗口只在 `AeadOpen` 成功后才 `Accept`，伪造高序号无法推高窗口（防 DoS）。
- 握手层另有非窗口防护：时间戳窗口（`handshake_replay_window_ms`，0 表示不校验）、最近 8 个 client nonce 缓存、每次连接随机 `m_handshake_tx_seq`。

## 6. 边界与已知限制

- **无证书、无身份认证**：PSK 是唯一身份凭据；X25519 公钥未经签名，只靠 PSK 混合间接绑定（改公钥 → 密钥不一致 → 解密失败，属检测而非身份证明）。
- **PSK 分发与保管不在范围内**；`KeyStore` 不做 `mlock`、不做内存加密，`Key` 拷贝是值语义（明文随拷贝扩散）。
- **密钥生命周期**：`m_tx_key`/`m_rx_key` 在 `ResetSecureSession()` 中用 `std::memset` 清零（非 `SecureZero` 的 volatile 语义）；`m_psk` 在该函数中**不清零**，只被下一次 `SetPreSharedKey` 覆盖。临时 X25519 私钥与共享密钥走 `SecureZero`。0-RTT 票据默认 60s 过期，keystore 轮换窗口默认 300s。
- **算法/长度上限**：AEAD 单帧密文受 `kMaxProbeFrameBytes = 1500` 限制，`kSecureOverheadBytes = 32`；key/nonce/tag 固定 32/12/16B，无变体参数。
- **常量时间范围**：tag 比较、`ConstantTimeEquals`、Montgomery ladder 条件交换是常数时间；`KeyStore` 的长度分支与 hex/base64 解码不是（长度不视为秘密）。
- **公钥有效性**：只检查共享密钥全零（RFC 7748 建议的最低限度），不做子群成员/扭曲点检查；更强防护需上层静态密钥或证书，本模块不提供。
- **客户端临时密钥生成失败会回退纯 PSK**（公钥全零）：连接仍加密但**没有前向保密**，只能通过 `psk_fallbacks` 计数观测；监听方遇到低阶点则拒绝握手、不发 ACK。
- **未实现**：HKDF、Ed25519/ECDSA 签名、公钥证书绑定、密钥协商的双向身份认证、轮换期间的会话迁移、`mlock`/HSM 集成。

## 7. 测试数据

断言数取自基线工作树 `grep -c 'BENTO_CHECK' tests/bento/<file>` 的**宏出现次数**；循环内断言运行时重复执行，实际断言数大于该值。`docs/modules/bento-modules.md` 记的「keystore 20 用例/2719 断言」「crypto+x25519 1160+」是构建期运行断言口径，与本表不可直接相加。

| 测试文件 | 断言宏 | 覆盖点 |
|---|---|---|
| `test_crypto.cpp` | 76 | RFC 8439 §2.4.2 ChaCha20、RFC 7539 §2.5.2 Poly1305、RFC 8439 §2.8.2 AEAD 向量；篡改/截断/重放/错误密钥拒绝；窗口边界与乱序；吞吐 ≥50MB/s |
| `test_secure_link.cpp` | 51 | PSK 握手、双向数据加密、错误 PSK 失败、明文/加密混合模式、`secure_stats()` 计数 |
| `test_secure_injection.cpp` | 17 | 故障注入：篡改/截断/重放/明文注入均被拒，同时可靠流仍 100% 按序送达 |
| `test_zero_rtt.cpp` | 48 | 票据恢复后首个包即加密、对端无需 HELLO；过期/篡改/错误 PSK 票据被拒 |
| `test_x25519.cpp` | 47 | RFC 7748 §6.1 Alice/Bob 与 §5.2 标量向量（含 1 次/1000 次迭代）、双向一致性、clamp、最高位忽略、低阶点 0/1/p-1/p/p+1 拒绝、空指针与确定性 |
| `test_x25519_handshake.cpp` | 179 | KDF 双方一致且共享密钥两半都参与；仅公钥+PSK 无法解密；新旧对端回退；篡改 ACK 公钥致解密失败；低阶点拒绝且不回 ACK；256 轮随机一致性 |
| `test_keystore.cpp` | 200 | hex/base64 加载与非法输入拒绝、失败不破坏已加载密钥、`Masked()` 前 4 后 4、常量时间比较、轮换窗口、日志/错误不含明文、500 组随机输入 |

运行：`ctest -R '^bento_'`（在构建目录执行，覆盖全部 `bento_*` 目标）。本文档编写期间**未运行** ctest，因此不声称「已验证通过」。

## 8. 示例

示例①：`KeyStore` 加载 + 脱敏 + 常量时间比较 + 轮换（链接 `go2cpp_bento_keystore` 与 `go2cpp_bento`）。

```cpp
#include <array>
#include <cstdint>
#include <cstdio>
#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/bento/net/keystore.hpp"

int main() {
    using go2cpp::bento::KeyStore;
    KeyStore store;  // 生产用 LoadFromFile/LoadFromEnv；此处内联 32B PSK 便于试跑
    if (auto e = store.LoadFromString(
            "hex:000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")) {
        std::printf("load failed: %s\n", go2cpp::bento::error::ErrorMessage(e).c_str());
        return 1;
    }
    const KeyStore::Key& cur = store.Current();
    std::printf("loaded=%d bytes=%zu masked=%s\n", static_cast<int>(store.loaded()), cur.size(),
                cur.Masked().c_str());
    std::array<std::uint8_t, 32> fresh{};
    fresh.fill(0xA5);
    const KeyStore::Key next = KeyStore::Key::FromBytes(fresh.data(), fresh.size());
    std::printf("accepts_current=%d ct_equal_new=%d\n", static_cast<int>(store.Accepts(cur)),
                static_cast<int>(KeyStore::ConstantTimeEquals(cur, next)));
    if (!store.Rotate(next)) {
        return 2;
    }
    std::printf("rotated: accepts_new=%d previous_usable=%d\n",
                static_cast<int>(store.Accepts(next)),
                static_cast<int>(store.HasUsablePrevious()));
    return 0;
}
```

示例②：AEAD Seal/Open + tag 篡改 + `ReplayWindow` 拒绝重放（链接 `go2cpp_bento`）。

```cpp
#include <array>
#include <cstdint>
#include <cstdio>
#include "go2cpp/bento/net/crypto.hpp"

int main() {
    using namespace go2cpp::bento;
    std::array<std::uint8_t, kChaCha20KeyBytes> key{};
    SecureRandomBytes(key.data(), key.size());
    std::array<std::uint8_t, kChaCha20NonceBytes> nonce{};
    MakeSecureNonce(SecureDirection::kClientToServer, 1, nonce.data());

    const std::uint8_t aad[2] = {0xBE, 0x01};
    const std::uint8_t msg[] = "bento-aead-demo";
    std::array<std::uint8_t, sizeof(msg)> ct{}, out{};
    std::array<std::uint8_t, kAeadTagBytes> tag{};
    AeadSeal(key.data(), nonce.data(), aad, sizeof(aad), msg, sizeof(msg), ct.data(), tag.data());

    const bool opened = AeadOpen(key.data(), nonce.data(), aad, sizeof(aad), ct.data(), ct.size(),
                                 tag.data(), out.data());
    std::printf("open=%d text=%s\n", static_cast<int>(opened),
                reinterpret_cast<const char*>(out.data()));

    tag[0] ^= 0x01;  // 篡改 tag：必须失败
    const bool tampered = AeadOpen(key.data(), nonce.data(), aad, sizeof(aad), ct.data(),
                                   ct.size(), tag.data(), out.data());
    std::printf("tampered_open=%d\n", static_cast<int>(tampered));

    // 三次 Accept 分开求值：C++ 不保证 printf 实参的求值顺序。
    ReplayWindow window;
    const bool first = window.Accept(1);
    const bool replay = window.Accept(1);
    const bool newer = window.Accept(2);
    std::printf("first=%d replay=%d next=%d\n", static_cast<int>(first),
                static_cast<int>(replay), static_cast<int>(newer));
    return 0;
}
```

## 9. 相关文档

- [bento-session.md](./bento-session.md)：配对与 PSK 握手在应用层的接线。
- [bento-transport.md](./bento-transport.md)：加密帧如何进入 UDP 链路与通道层。
- [bento-foundation.md](./bento-foundation.md)：`keystore` 错误码、mapping 集成与边界补充（本文不重复）。
- [bento-scenarios.md](./bento-scenarios.md)：弱网/故障注入等场景下的加密行为。
- [PROTOCOL.md](../../tests/bento/PROTOCOL.md)：§9.2 加密帧格式、§9.3 PSK 握手与密钥派生。
