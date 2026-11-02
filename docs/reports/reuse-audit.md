# Go2Cpp 功能重复 / 复用 只读审查报告

- 审查对象：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树，HEAD `2238633`，工作树已有未提交改动）
- 审查方式：只读。逐头文件/实现文件阅读 + 全仓 grep 统计（`include/` 与 `src/`，共 584 个文件）
- 审查日期：2026-11-02
- 边界：未修改任何源码；本报告写在 `/tmp/reuse_audit.md`

## 结论摘要

| 分类 | 组数 | 说明 |
|---|---|---|
| 真重复（同一功能，可合并） | 15 | 其中 6 组属于「复制粘贴级」：字节序读写、FNV-1a、文本解析、ASCII 工具、hex、SHA-256 |
| 部分重复（骨架相同、协议/语义不同） | 4 | 分帧器、picoquic 集成、定时索引、KV |
| 看似重复但不同（不要合并） | 14 | Channel 家族、Transport 家族、像素 SIMD/标量、转发头链等 |
| 薄封装（不算重复） | 8 类 | `go/*` 兼容层、`.h` 转发头、`config_codec → reflect` 等 |
| 公共设施有、但没人用 | 3 | `logging`（仅 timer 用）、`encoding/hex`（0 生产使用者）、`network/framing`（仅测试） |

最值得动手的三件事（按性价比排序）：

1. **抽公共的大端读写**：全仓 13 处手写 U16/U32/U64 大端读取，其中 7 处逐字节相同（`src/bento/net/*`），改一处即可消除整类漂移。
2. **合并随机数熵源**：`bento::SecureRandomBytes`（`std::random_device`）与 `p2p::SecureRandomBytes`（`getrandom`）同名同签名、安全强度不同，属安全隐患而非风格问题。
3. **让 compress 复用 `hash::Crc32` / `hash::Adler32`**：`deflate_reader.hpp` 与 `deflate_writer.hpp` 各写一套 CRC 表（16 表 / 8 表），而 `hash::crc32` 已有 PCLMULQDQ 加速（项目自测 1 KiB 块 3.4 → 18.8 GB/s）。

---

## 一、真重复（同一功能，可合并）

### T1 大端 U16/U32/U64 读写（13 处，最严重）

| # | 实现方式 | 位置 |
|---|---|---|
| T1.1 | 逐字节位移，大端 | `src/bento/net/protocol.cpp:18,23,30,35`（WriteU16 / WriteU32 / ReadU16 / ReadU32） |
| T1.2 | 同上，逐字节相同 | `src/bento/net/command.cpp:50,56,64,71,77,83` |
| T1.3 | 同上，逐字节相同 | `src/bento/net/file_transfer.cpp:47,53,61,68,74,80` |
| T1.4 | 同上，逐字节相同 | `src/bento/net/pairing.cpp:22,27,34,40,45,50` |
| T1.5 | 同上，逐字节相同 | `src/bento/net/peer_hub.cpp:125,132,137,142` |
| T1.6 | 同上，逐字节相同 | `src/bento/net/link.cpp:43,50,55,60` |
| T1.7 | 同上，逐字节相同 | `src/bento/net/frame.cpp:31,36,43,48` |
| T1.8 | 写 `std::vector` 的大端 | `src/bento/app/session.cpp:68,73` |
| T1.9 | 大端，供 Bento 桥 | `src/app/bento_bridge.cpp:24,29` |
| T1.10 | 大端 + `memcpy`/`bswap` 优化版 | `src/app/frame.cpp:23,40`（LoadBE32 / LoadBE16）、`:361,363`（GetU16 / GetU32） |
| T1.11 | 大端 | `src/p2p/relay_protocol.cpp:17,22` |
| T1.12 | 大端消息字 | `src/hash/sha1.cpp:30`、`src/hash/sha256.cpp:27` |
| T1.13 | 小端（语义不同，不能混用） | `include/go2cpp/debug/diagnostic_protocol.hpp:127,143,149`（PutU16/PutU32/GetU16/GetU32） |

公共设施其实已存在、却几乎无人使用：

- `include/go2cpp/network/endian.hpp:29-72`：`byteswap16/32/64`、`host_to_be16/32/64`、`be_to_host*`（constexpr、零分配）；
- `include/go2cpp/encoding/binary.hpp` + `src/encoding/binary.cpp:66-90`：`Load16/32/64`、`Store16/32/64`（带 `BinaryValue<T>` 错误返回；**生产使用者为 0**）。

判断：**真重复**。T1.1–T1.7 是复制粘贴；T1.8–T1.12 是同算法不同包装。

建议：在 `network/endian.hpp` 补 `LoadBE16/32/64` + `StoreBE*`（或新增零依赖 `go2cpp/base/endian_read.hpp`），bento 7 个文件、app 2 个、p2p 1 个、hash 2 个统一调用。`diagnostic_protocol` 的小端版本不并入（语义不同），必要时用 `LoadLE*` 命名区分。

风险：低。注意两点：(a) 现有实现「未对齐读」的处理不同（`app/frame.cpp` 用 memcpy + bswap，bento 是逐字节），统一后应固定为 UB-free 的 memcpy 版本；(b) 帧头解析在热路径，需确认内联后指令数不增。

### T2 FNV-1a 哈希（5 处）

| # | 位置 | 实现 |
|---|---|---|
| T2.1 | `src/p2p/endpoint.cpp:24-29` | `HashBytes(std::size_t*, ...)`，FNV-1a，偏移 `1469598103934665603` |
| T2.2 | `src/p2p/candidate.cpp:73-78` | 与 T2.1 逐字节相同（同模块内复制） |
| T2.3 | `src/concurrency/fiber.cpp:3024-3031` | `MigrationFnv1a`，u64，偏移 `0xcbf29ce484222325` |
| T2.4 | `src/media/audio.cpp:98-104` | `Fnv1a64`，与 T2.3 同算法，注释自称「与 VideoFrame::Checksum 同一算法」 |
| T2.5 | `include/go2cpp/debug/diagnostic_protocol.hpp:120-126` | FNV-1a **32 位**（语义不同，不能与 64 位互换） |

判断：**真重复**（T2.1–T2.4）。附带隐患：T2.1/T2.2 用 `std::size_t` 做状态、却用 FNV-64 常数，在 32 位平台上会静默截断——合并到固定宽度 `uint64_t` 能顺手修掉这个潜在 bug。

建议：放 `go2cpp/hash/fnv.hpp`（`Fnv1a64` / `Fnv1a32`）；T2.5 可保留 32 位版本但调用公共实现。

风险：低。FNV 无状态、无算法差异（除宽度）。

### T3 安全随机字节（5 处熵源 + 3 处直接 random_device）

| # | 位置 | 熵源 | 备注 |
|---|---|---|---|
| T3.1 | `include/go2cpp/go/crypto_rand.hpp:155,865,895` | `getrandom(2)` → `/dev/urandom` → Windows `random_device` | 完整 CSPRNG，另带 BigInt/Prime |
| T3.2 | `src/p2p/random.cpp:22,39,60,92-135` | `getrandom` → `/dev/urandom` → 时间戳兜底 | 线程本地 1024B 池；函数名 `SecureRandomBytes` |
| T3.3 | `src/bento/net/crypto.cpp:395-409` | `std::random_device` | 函数名同样叫 `SecureRandomBytes`，**与 T3.2 同名同签名** |
| T3.4 | `include/go2cpp/bento/net/x25519.hpp:94` | `random_device` | 密钥生成 |
| T3.5 | `include/go2cpp/bento/net/token_pairing.hpp:105` | `random_device` | 配对 token |
| T3.6 | `src/network/websocket.cpp:73` | `random_device` 播种 + splitmix64 | 掩码密钥，热路径已缓存，可接受 |
| T3.7 | `src/bento/net/coap.cpp:559,598,848` | 自研 `m_rng_state`（时间/计数混合） | CoAP token，非密码学用途，可接受 |

判断：**真重复**（T3.1–T3.5 都是「密码学安全随机字节」）。T3.3 用 `random_device`、T3.2 用 `getrandom`，安全强度不同却同名，极易在 review 里被当成同一个东西。

建议：以 T3.1 为唯一实现（或抽 `go2cpp/os/random.hpp` 只暴露 `SecureRandomBytes`）；p2p 改为调用，把「线程本地池化」作为可选优化下沉；bento 三个调用点改 include。`coap` / `websocket` 的自研 RNG 保留（性能/协议语义不同）。

风险：中。bento 的既定约束是「无第三方依赖」，T3.1 是纯头文件 + 系统调用，满足约束；但需确认 bento 编译单元没有 `-D_WIN32` 分支问题。合并后必须复跑握手/密钥相关测试。

### T4 文本 → 数字/布尔解析（10 处）

| # | 位置 | 实现 |
|---|---|---|
| T4.1 | `include/go2cpp/tools/strconv.hpp:234,339,388,433` | 公共实现：ParseUint / ParseInt / ParseBool / ParseFloat（完整、带错误锚点） |
| T4.2 | `include/go2cpp/go/flag.hpp:181,196,212` | `ParseBoolText` / `ParseIntText` / `ParseFloatText`，内部 `strtoll` / `strtod` |
| T4.3 | `include/go2cpp/bento/reflect/caster.hpp:34,116,158` | `ParseInt` / `ParseDouble` / `ParseBool`（手写字符扫描） |
| T4.4 | `src/config/config.cpp:144` | `ParseBool`（1/0/yes/no/on/off） |
| T4.5 | `src/config/so_manifest.cpp:50,72` | `ParseU64` / `ParseBool` |
| T4.6 | `src/app/resource.cpp:76` | `ParseU64` |
| T4.7 | `src/bento/app/capability.cpp:169` | `ParseU64` |
| T4.8 | `src/bento/app/fetch.cpp:881` | `ParseU64Decimal` |
| T4.9 | `src/media/signaling.cpp:66,111` | `ParseUint` / `ParseU64` |
| T4.10 | `include/go2cpp/config/config_codec.hpp`（DecodeLegacyBool / DecodeLegacyDouble） | 旧格式兼容层，**有意保留**（薄封装，不计入重复） |

判断：**真重复**（T4.2–T4.9）。语义差异确实存在：是否接受前导 `+`、是否接受前后空白、溢出时报错还是饱和、bool 词表不同（Go 的 `t/T/TRUE` vs config 的 `1/0/yes/no/on/off`）。这些差异应显式参数化，而不是各写一份。

建议：`tools/strconv` 增加 `ParseBoolAny`（多词表）与 `ParseU64Clamped`，各模块调用；config 的旧词表保持兼容。`go/flag` 必须改用 `strconv::ParseInt`（与 Go 语义对齐，且有单测）。

风险：中。`flag` 的 `strtoll` 版本与 `strconv::ParseInt` 都要求完整消费，行为应一致，但需逐例对拍（尤其是 `0x` 前缀、前导零、`+`）。

### T5 ASCII 字符串工具（Trim / ToLower / StartsWith，14+ 处）

公共实现：`include/go2cpp/tools/strings.hpp:184,207,234,419`（`EqualFold` / `HasPrefix` / `TrimSpace` / `ToLower` 等）。

各模块自写：

| 函数 | 位置 |
|---|---|
| `ToUpperAscii` | `src/network/http3/client.cpp:77` |
| `ToLowerAscii` | `src/network/http3/client.cpp:84`、`src/bento/net/command.cpp:107`、`src/bento/app/fetch.cpp:84`、`src/storage/db.cpp:158`（ToUpperAscii） |
| `ToLower` | `src/config/config_codec.cpp:45`、`src/media/signaling.cpp:54` |
| `Trim` | `src/bento/app/backup.cpp:74`、`src/config/config.cpp:50`、`src/config/so_manifest.cpp:43`、`src/debug/debug_server.cpp:68`、`src/debug/diagnostic_agent.cpp:97` |
| `TrimView` | `src/bento/app/capability.cpp:134`、`src/config/config_codec.cpp:35` |
| `TrimAscii` | `src/bento/app/fetch.cpp:98` |
| `StartsWithIgnoreCase` | `src/bento/net/keystore.cpp:62`、`src/bento/app/backup.cpp:102`、`src/bento/app/fetch.cpp:113` |
| `StartsWith` | `src/media/signaling.cpp:62` |
| `SplitTokens` | `src/debug/debug_server.cpp:118` |

判断：**真重复**，但要注意 `strings.hpp` 是 **UTF-8 rune 语义**（`TrimSpace` 处理 U+0085/U+00A0 等），各模块的 `ToLowerAscii` / `TrimAscii` 是纯字节快路径。直接替换会把热路径改成 rune 解码，属性能退化。

建议：在 `tools/strings.hpp` 增加语义明确的 `AsciiToLower` / `AsciiTrimSpace` / `AsciiHasPrefixIgnoreCase`，各模块改调用；需要 UTF-8 语义的地方（config 值、日志）继续用 `strings::`。这样既统一又不牺牲热路径。

风险：低-中。要防止「把 ASCII 函数换成 UTF-8 函数」这种看似等价、实则改行为的替换。

### T6 hex / base64 编解码（hex 15 处、base64 2 套）

- 公共实现：`include/go2cpp/encoding/hex.hpp`、`include/go2cpp/encoding/base64.hpp` + `src/encoding/hex.cpp` / `src/encoding/base64.cpp`
- **生产使用者：0**（只出现在 `encoding/encoding.hpp` 聚合头里）

各模块自写 hex 表 / 编解码：

| 位置 | 用途 |
|---|---|
| `src/bento/net/keystore.cpp:99,155,357,370` | hex 解码 + 输出（**密钥加载路径**） |
| `src/bento/net/keystore.cpp:125,185` | base64 解码（约 100 行，**密钥加载路径**） |
| `src/bento/net/file_transfer.cpp:320` | SHA-256 转 hex 文本 |
| `src/bento/net/token_pairing.cpp:41` | token hex |
| `src/bento/error/mapping.cpp:33` | 错误码 hex |
| `src/bento/app/capability.cpp:58` | 能力串 |
| `src/bento/app/backup.cpp:450,560` | 清单/摘要 hex |
| `src/bento/app/fetch.cpp:147,1791` | ETag / 摘要 hex |
| `src/config/config.cpp:1196`、`src/config/so_manifest.cpp:161` | 配置诊断 hex |
| `src/app/resource.cpp:51` | 资源校验 hex |
| `src/debug/diagnostic_agent.cpp:239` | 事件 hex |
| `src/reflect/json.cpp:24` | JSON 字符串 `\u00XX` 转义 |
| `src/media/signaling.cpp:237` | 用 hex 字符集做 token 字母表（**不是 hex 编码**，保留） |
| `src/p2p/candidate.cpp:81` | foundation hex |
| `include/go2cpp/logging/redact.hpp:206` | key_id hex |
| `include/go2cpp/tools/fmt.hpp:157` | `%x` 格式化（**属格式化，保留**） |

判断：**真重复**（hex 输出 15 处、base64 解码 2 套）。`keystore` 自己写 base64 解码尤其值得合并——密钥解析是最需要单点审计的地方。

建议：`encoding::hex` 增加 `AppendHex(std::string&, ByteSpan)` 与 `DecodeHex(ByteSpan, out)`；`keystore` 改调用 `encoding::base64`（其 `Strict()` 模式已有）。保留 `fmt.hpp` 的 `%x` 与 `signaling` 的字符集常量。

风险：低。注意 `encoding::base64` 的错误语义是「返回 ErrorPtr + 位置」，而 `keystore` 需要严格校验 + 拒绝多余填充；合并时把严格模式做成参数。

### T7 SHA-256（3 份实现）

| # | 位置 | 说明 |
|---|---|---|
| T7.1 | `include/go2cpp/hash/sha256.hpp` + `src/hash/sha256.cpp` | 通用软件实现，Go `hash.Hash` 接口 |
| T7.2 | `include/go2cpp/logging/redact.hpp:34,86-181` | 内嵌约 90 行最小实现 + 64 个轮常量；注释理由「logging 只依赖 C++17 标准库」 |
| T7.3 | `include/go2cpp/app/sha256_ni.hpp:36-` | SHA-NI 硬件加速 + 回退到 T7.1；注释理由「不改 src/hash/ 边界」 |

判断：**真重复**（3 份）。T7.2/T7.3 都有明确理由，且 T7.3 会回退到 T7.1、测试对拍。真正的问题不是「有 3 份」，而是「加速没有下沉」：`bento/net/file_transfer.cpp`、`bento/app/backup.cpp`、`bento/app/fetch.cpp` 三处大文件校验用的是**软件版**（0.42 GB/s），而 app 层 SHA-NI（2.0 GB/s）只服务 app 资源校验。

建议：
1. 把 SHA-NI 压缩函数下沉到 `hash/sha256.cpp`（`hash/crc32` 已有 PCLMUL 先例），用 target attribute + 运行时 `__builtin_cpu_supports("sha")` 分派；`app/sha256_ni.hpp` 退化为薄转发或删除。
2. `logging/redact` 的内嵌实现可以保留（避免 logging 依赖 hash 的编译耦合），但注释应写明「刻意分叉 + 定期对拍」（已有测试）。

风险：中。SHA-NI 需要 `-msha`/target attribute 与 cpuid 检测；合并后必须跑 NIST 向量 + 非 x86 回退测试。

### T8 CRC32 / Adler32（compress 自写两套 + hash 公共设施闲置）

| # | 位置 | 实现 |
|---|---|---|
| T8.1 | `include/go2cpp/hash/crc32.hpp` + `src/hash/crc32.cpp` | 表驱动 + PCLMULQDQ / Castagnoli 硬件路径，流式 `Update` |
| T8.2 | `include/go2cpp/hash/adler32.hpp` | 公共 Adler-32 |
| T8.3 | `src/compress/deflate_reader.hpp:1041-1105` | 自写 slice-by-16 CRC32 + 16 张表 + `UpdateAdlerBlock:1105` |
| T8.4 | `src/compress/deflate_writer.hpp:754,774,797` | 自写 CRC32（8 张表）+ `UpdateAdler` |
| T8.5 | `include/go2cpp/debug/diagnostic_protocol.hpp:120` | FNV-1a 32 校验（**不是 CRC**，算法不同，保留） |

复用良好的对照：`src/frame/frame_codec.cpp:88-93`、`src/bento/net/frame.cpp:207-216`、`src/bento/app/zip.cpp:443,1192` 都调用 `hash::Crc32`。

判断：**真重复**（T8.3/T8.4 vs T8.1/T8.2）。

建议：compress 改为 `hash::Crc32::Update` / `hash::Adler32`。`hash::Crc32::Update` 是流式接口，支持任意切分；硬件路径对小片段有分派开销，需在 compress 的「每次 1~64 KiB」粒度上实测，若不划算再保留小片段表路径（但表也应由 `hash::Crc32` 提供，避免 24 张表常量重复）。

风险：中。compress 是热路径（gzip 压缩/解压），必须前后对比吞吐。

### T9 错误体系（bento/error 是 go2cpp/error 的子集）

| # | 位置 | 规模 |
|---|---|---|
| T9.1 | `include/go2cpp/error/error.hpp`（1280 行）+ `code.hpp` + `registry.hpp` + `compat.hpp` | 超集：类型链、16 个语义码、Registry、销毁工作表 |
| T9.2 | `include/go2cpp/bento/error/error.hpp`（329 行）+ `registry.hpp` + `mapping.hpp` | 子集：同样的 `Error` / `ErrorPtr` / `StringError` / `WrappedError` / `JoinError` / `NewError` / `Wrap` / `Join` / `IsWrappedError` / `UnwrapAll` / `ErrorMessage` / `As` |
| T9.3 | `include/go2cpp/bento/error/bridge.hpp:35` | 已有 T9.1 ↔ T9.2 的桥（有损映射） |
| T9.4 | `include/go2cpp/rpc/status.hpp:36` | 第三套 gRPC 16 码（注释明确「刻意不复用」） |
| T9.5 | `include/go2cpp/core/status.hpp:10` | 7 态 `StatusCode`（调度器生命周期） |
| T9.6 | 各模块自有码 | `frame::ErrorCode`、`app::frame::ErrorCode`、`app::bento_bridge::Status`、`os::monitor::Status`、`reflect::CodecStatus`、`storage::db::Code`、`distributed::Status` |

判断：**真重复**（T9.1 vs T9.2）。`go2cpp/error/error.hpp` 自己在注释里写「零外部依赖：只依赖标准库，可被任意模块（含 Bento）单独 include」——T9.2 存在的理由已经消失。T9.4/T9.5/T9.6 是**领域码**，不应合并（wire 语义/模块边界不同）。

建议：bento 全模块改为 `go2cpp::error`，`bento/error/bridge.hpp` 只保留「bento 私有码 → go2cpp 码」映射；`bento/error/error.cpp` 可删。`rpc/status` 保持独立（文档理由成立）。

风险：中。迁移面大（`bento/net/keystore.hpp`、`bento/app/*`、tests/bento），两套 `ErrorCode` 的枚举值/名字不同，需逐值对齐并保留 bridge 的兼容转换。

### T10 JSON（两套解析/序列化）

| # | 位置 | 说明 |
|---|---|---|
| T10.1 | `include/go2cpp/reflect/json.hpp` + `src/reflect/json.cpp`（739 行） | 自研：`JsonValue` / `JsonWriter` / `JsonParser`，严格 JSON、限深 64 / 成员 4096 / 字符串 16 MiB |
| T10.2 | `include/go2cpp/tools/json.hpp`（319 行） | nlohmann/json v3.11.3 薄封装 + 迭代式深度栅栏 `ParseJson` |
| T10.3 | `include/go2cpp/config/config_codec.hpp` | 委托 T10.1（**良好复用**） |

`tools/json.hpp:48-62` 的 TODO 注释仍写着「Reflect 未提供 JSON 后端，暂不替换 nlohmann」——但 T10.1 已经交付，注释已过时。

判断：**真重复**（两个 JSON 解析器 + 两个序列化器）。

建议：先做「调用面盘点」：`Json` / `ParseJson` 的使用者（example、tests/debug）依赖 nlohmann 的完整 API，短期无法替换；`reflect::Json` 被 config / so_manifest 使用。可行路径是**明确分工并写进文档**：`reflect::json` 是「运行时协议/配置的内部 JSON」，`tools/json` 是「边界层 + 深度栅栏 + 外部工具兼容」，并把过时 TODO 更新为「已评估，保留双实现的原因」。若追求单一实现，应把 `reflect::JsonValue` 换成 `Json` 别名并保留深度栅栏——但会破坏 reflect 的「零第三方依赖」定位，需项目决策。

风险：高（不要贸然合并）。两套对数字的保真度不同（reflect 保留数字原文，nlohmann 会做 double 往返）、NaN/Inf 策略不同、错误类型不同。

### T11 分帧/半包状态机（4 套）

| # | 位置 | 线格式 | 使用者 |
|---|---|---|---|
| T11.1 | `include/go2cpp/frame/frame_codec.hpp` + `.cpp` | 12B 头、小端、magic、CRC32 | rpc（`client.cpp` / `server.cpp`）、`rpc/protocol.hpp` |
| T11.2 | `include/go2cpp/app/frame.hpp` + `src/app/frame.cpp` | 9B HTTP/2 风格、大端、无 CRC | app 框架 |
| T11.3 | `include/go2cpp/bento/net/frame.hpp` + `.cpp` | 20B、大端、magic、CRC32 | bento（`bento/app/session.cpp`） |
| T11.4 | `include/go2cpp/network/framing.hpp` | 分隔符 / 固定长度 / 4B 前缀 | **仅 `runtime.hpp` 聚合头与 `tests/network`** |

四者都实现了「缓冲 → 判长度 → 切帧 → 失败清空」的同一套骨架。

判断：**部分真重复**。线上格式不同（不能统一成一种帧），但「流式缓冲 + 半包判定 + 上限检查」的骨架重复了 4 次。

建议：抽一个 `framing::StreamBuffer`（`Append` / `Peek` / `Consume` + 最大帧上限），三个 FrameDecoder 复用它；T11.4 要么复用同一骨架提供「无 magic」的通用 Framer，要么标注为示例并从 `runtime.hpp` 移除（它现在没有生产使用者）。

风险：中。三个解码器的错误策略不同（T11.2/T11.3 遇非法帧清空缓冲；T11.1 返回错误但保留缓冲），抽公共件时要把「失败策略」做成参数，否则会改变现有语义。

### T12 picoquic 集成（客户端复用，服务端另写）

| # | 位置 | 内容 |
|---|---|---|
| T12.1 | `include/go2cpp/network/quic/picoquic_connection.hpp` + `.cpp`（698 行） | 客户端薄封装：prepare_next_packet / incoming_packet |
| T12.2 | `include/go2cpp/rpc/transport_picoquic.hpp` + `.cpp`（1397 行） | 客户端复用 T12.1；**服务端自己写** picoquic_create / incoming_packet_ex / prepare_next_packet 循环 |

判断：**部分真重复**（picoquic 的「收包 → 喂协议 → 取包 → 发包」泵在两边各一份）。注释已解释「T12.1 只有客户端入口」，所以这是**能力缺口**而非无意识复制。

建议：在 `network/quic/` 增加 `PicoquicServer`（多连接、单线程驱动），rpc 的服务端泵改为调用它；这样「picoquic 只在一处被直接 include」的边界才真正成立。若短期不做，至少在 T12.2 注明「临时实现，待 T12.1 补齐服务端后收敛」。

风险：中-高。rpc 服务端用一把 `ServerCore::mutex` 串行化多连接，且依赖「回调里不 delete cnx」等细节；抽象时必须保持线程模型与回调约定，否则会引入 QUIC 层竞态。

### T13 KV（两套实现，设计承认，但使用面与注释相反）

| # | 位置 | 规模 | 使用面 |
|---|---|---|---|
| T13.1 | `include/go2cpp/storage/kv.hpp` + `src/storage/kv.cpp`（1202 行）自研 LSM | 活跃：`memtable.cpp`、`sstable.cpp`、`wal.cpp`、`seglog.cpp`、`memcache.cpp`、`watch.cpp`、`queue.cpp` 全部依赖 | 主实现 |
| T13.2 | `include/go2cpp/storage/leveldb_kv.hpp` + `src/storage/leveldb_kv.cpp`（603 行） | 仅 `tests/storage/test_leveldb_kv.cpp`；CMake `GO2CPP_WITH_LEVELDB=OFF` 默认关闭 | 可选对照 |

判断：**真重复**（两个持久化 KV），但**设计上承认**（`leveldb_kv.hpp:2-8` 明确写「自研保留作对照组」）。问题在于注释说 LevelDB 解决「1e8 级 RSS 不可控」，而生产代码没有任何一处切换到它——文档与事实不符。

建议：不要删（对照与压测有价值），但要 (a) 在模块文档里明确「生产默认 = 自研 KV；LevelDB = opt-in 对照」；(b) 若长期保留，抽一个最小 `IStore`（Get/Put/Delete/NewIterator/Write）接口，两个实现都实现它，避免第三个消费者在两种类型之间二选一。

风险：低（不动实现），中（抽接口会改调用面）。

### T14 反射与字段绑定（两套）

| # | 位置 | 机制 | 使用面 |
|---|---|---|---|
| T14.1 | `include/go2cpp/reflect/field.hpp` + `type.hpp` + `serializer*` + `GO2CPP_REFLECT_FIELD` 宏 | 结构体字段表 + 二进制/JSON 序列化 | `config/so_manifest.hpp:273`、`config_codec`、testing |
| T14.2 | `include/go2cpp/bento/reflect/{reflect,bind,caster}.hpp` | `TypeTraits<T>` + `FieldList<T>` + `Bind`（无宏、覆盖 4 种标量） | **仅 `tests/bento/*`** |

判断：**真重复**（都是「字段名 ↔ 值」的反射绑定）。T14.2 在生产代码里没有调用方，且只覆盖 int/double/bool/string，能力是 T14.1 的子集。

建议：删除 T14.2（或移到 `tests/` 作为「无宏反射」样本），bento 的 CSV/配置绑定改用 T14.1；若坚持保留「无宏、零侵入」版本，应在文件头写明「仅测试用，生产请用 go2cpp/reflect」。

风险：低（无生产使用者），但要先确认 `bento/app/*` 没有间接依赖（目前已确认没有）。

### T15 定时/超时索引（6 套机制）

| # | 位置 | 机制 | 语义 |
|---|---|---|---|
| T15.1 | `include/go2cpp/concurrency/core/timer.hpp` + `src/concurrency/timer.cpp` | `TimerService`：后台线程 + 条件变量 + 按到期时间排序的 multimap | 通用定时器 |
| T15.2 | `include/go2cpp/cache/detail/time_wheel.hpp` | 256 桶时间轮 + 锦标赛树 | LRU-K 的「最小 reference time」索引 |
| T15.3 | `src/network/io.cpp:648-991` | `std::multimap<time_point, TimerRecord>` | IO 等待超时 |
| T15.4 | `src/bento/net/session_table.cpp:73-105` | 有序 `(deadline, key)` 集合 | 会话租约过期 |
| T15.5 | `include/go2cpp/relay/lease.hpp` + `src/relay/lease.cpp` | **每对象一个后台线程** | 租约/心跳 |
| T15.6 | `include/go2cpp/bento/net/heartbeat.hpp` | 无后台线程，`Poll()` 驱动 | 单线程事件循环里的心跳 |

判断：**部分真重复**。T15.1–T15.4 都是「按时间取最小/到期集合」，但数据规模、并发模型、内存布局完全不同（详见第五节）。

建议：**不要合并**。只做一件事：在 `docs` 里写一张「定时设施选型表」（什么时候用哪个），避免新模块再发明第七套。

### T16 公共日志设施未被复用（不是重复实现，是缺口）

| # | 位置 | 现象 |
|---|---|---|
| T16.1 | `include/go2cpp/logging/log.hpp`（874 行）+ `src/logging/log.cpp`（1095 行） | 唯一生产使用者是 `src/concurrency/timer.cpp:36` |
| T16.2 | `src/config/config.cpp:816,837,843,847` | 热加载失败直接 `std::cerr`，绕过日志等级/轮转/脱敏 |
| T16.3 | `src/concurrency/scheduler.cpp:105,115,136,141,172`、`src/concurrency/sync.cpp:2230` | 诊断路径 `fprintf(stderr)`（崩溃前输出，可接受但未统一） |
| T16.4 | `src/debug/debug_event_ring.cpp:649`、`src/debug/diagnostic_agent.cpp:2632,3016` | 调试工具自身输出（边界内） |
| T16.5 | `src/os/proc.cpp:261,269`、`src/os/supervisor.cpp:352` | 子进程 stderr 转发（语义是转发，**不是日志**，保留） |

判断：T16.2 是「该走日志但没走」；T16.3–T16.5 情有可原。

建议：`config` 的热加载失败改 `GO2CPP_LOG_ERROR`（注意 config 可能早于 Logger 初始化，需要惰性/降级策略）；其余保持，并在 logging 文档里列出「允许直接写 stderr 的三种场景」（崩溃诊断、子进程转发、日志后端自身）。

风险：低。

---

## 二、看似重复但不同（不要合并）

| # | 组 | 为什么不是重复 | 合并的后果 |
|---|---|---|---|
| S1 | `concurrency/channel.hpp`（4305 行）vs `concurrency/fast_channel.hpp`（1593 行） | Channel 是 Go 语义全功能（select / Context / Fiber park）；FastChannel 是实验性 SPSC/MPMC 无锁 ring，**只被 tests 使用**，且复用 channel 的 `WaitNodeBase` | 合并会把实验数据面代码拖进生产路径，或反过来丢掉 select 语义。**建议**：把 fast_channel 头注释「实验版（/tmp 专用，不入库）」改成与现状一致（它已在仓库、已被 5 个测试引用），或移到 `tests/concurrency/` |
| S2 | `concurrency/channel.hpp` vs `os/channel.hpp` vs `bento/net/channel.hpp` | 三者分别是 Go channel、进程间字节通道（IPC + 帧格式）、可靠传输通道（STREAM/DATAGRAM/VIDEO + 窗口/ACK） | 语义层级完全不同，合并会制造巨型抽象 |
| S3 | `media/pixel.cpp` vs `media/pixel_kernels.hpp` | `pixel_kernels.hpp` 只是 `pixel.cpp` 的内部声明，用于让测试**逐字节对拍**标量与 SSE2/SSSE3 内核 | 不存在两套像素转换；合并（删 SIMD 或删标量）会失去对拍基线 |
| S4 | `sync.hpp` vs `concurrency/sync.hpp` | 后者是纯转发头，注释明确说「此前携带过时副本，已改为转发」 | 已是正确处理，不算重复；只需注意转发链两跳 |
| S5 | `scheduler.hpp` → `scheduler/scheduler.hpp` → `concurrency/scheduler/scheduler.hpp`；`context` / `fiber` / `fiber_local` 同构 | 纯转发链，保持旧 include 路径可用 | 合并会破坏兼容路径；可选优化是把两跳压成一跳 |
| S6 | 6 套 Transport：`app/transport.hpp:60`、`rpc/transport.hpp:77`、`bento/net/transport.hpp:146`、`p2p/transport.hpp:40`、`media/transport.hpp`（UdpVideoSender/Receiver）、`distributed`（CoordinatorClient） | 错误模型（bool / Status / ErrorPtr / Result）、同步/异步、字节流 vs 数据报均不同 | 合并会产生「适配器爆炸」，且每层都要重新翻译错误码 |
| S7 | `relay::Lease/Heartbeat` vs `core::TimerService` vs `bento::Heartbeat` vs `distributed::LeaseSession` | Lease/Heartbeat 是「每对象一线程 + 单调时钟 + 回调最多一次」；TimerService 是中心化多定时器；bento Heartbeat 是 Poll 驱动的无线程状态机 | 合并会改变线程模型与回调时序（Lease 的文档明确说十万级定时器才该换中心定时器） |
| S8 | `container/list.hpp`（Go list，拥有节点、每次插入分配）vs `cache/detail/intrusive.hpp`（侵入式、零分配） | 所有权模型与性能目标相反 | 用 list 替换 cache 链表会让缓存热路径重新出现 malloc；用 intrusive 替换 list 会改变调用方的生命周期契约 |
| S9 | `storage/memtable.hpp`（开放寻址哈希 + Arena）vs `cache/detail/bucket_map.hpp`（链式侵入哈希） | 一个面向「写缓冲、顺序刷盘、无删除墓碑」，一个面向「稳态插一条淘汰一条、零分配」 | 合并会同时牺牲两者的热路径（memtable 探测次数 vs cache 插入分配） |
| S10 | `src/network/io.cpp:341` 的 `WaitNode` vs `sync::WaitNode` | 源码注释已列对照表：等的是 fd 就绪 vs 锁/通知，唤醒源与队列完全不同 | 合并会导致「一方漏唤醒，另一方永远等」（原文结论） |
| S11 | `cache::TimestampWheel` vs `io` 的 multimap vs `TimerService` | 键的语义不同（逻辑 tick / 绝对时间点 / 定时器 id），并发模型不同 | 见 S7/S10；时间轮的优势是「命中路径不付 O(log n)」，换 multimap 会退化 |
| S12 | `concurrency/channel.hpp:1198` RingBuffer vs `os/shm.hpp:161` RingBuffer / `SharedRingBuffer` vs `debug/debug_event_ring.hpp:535` EventRing | 进程内 channel 缓冲 / 跨进程共享内存环 / 每线程诊断事件环 | 内存模型（同进程 vs 跨进程原子）完全不同 |
| S13 | `frame/frame_codec` vs `app/frame` vs `bento/net/frame` | 线上格式不同（12B 小端 / 9B HTTP2 大端 / 20B 大端） | 见 T11：只应合并「缓冲骨架」，不能合并「格式」 |
| S14 | `go2cpp/reflect` vs `bento/reflect` | 见 T14：能力子集 + 无生产使用者。**这一组其实应合并**（列在此处是因为 bento 可能刻意保持「零宏、独立」） | 若 bento 是刻意独立，则应把 T14.2 移到 tests 并标注；否则合并到 T14.1 |

---

## 三、薄封装（不算重复）

| # | 封装 | 被封装者 | 说明 |
|---|---|---|---|
| W1 | `include/go2cpp/go/crc32.hpp` | `hash/crc32.hpp` | 纯别名 + 常量，不重新实现 CRC；文件头明确说明 |
| W2 | `include/go2cpp/*.h`、`include/go2cpp/network/*.h` | 对应的 `.hpp` | 2~3 行 `#include` 转发 |
| W3 | `go/error.go.hpp`、`go/sync.go.hpp`、`go/time.go.hpp`、`go/io.go.hpp`、`go/net.go.hpp` | `tools/error.hpp`、`sync.hpp`、`tools/time.hpp`、`io.hpp`、`net` | Go 书写习惯的零成本包装，明确「不重新实现」 |
| W4 | `config/config_codec.hpp` | `reflect/*` | 配置文本编解码全部委托 Reflect（含类型/范围校验） |
| W5 | `rpc/transport_picoquic.hpp`（客户端路径） | `net::quic::PicoquicConnection` | 包装为 `rpc::Transport`，并加 `call_id → stream` 多路复用 |
| W6 | `frame/frame_codec.cpp:88-93` | `hash::Crc32::ChecksumIEEE` | 一行适配，复用正确 |
| W7 | `bento/net/frame.cpp:207-216`、`bento/app/zip.cpp:443,1192` | `hash::Crc32` / `hash::Crc32::Update` | 复用正确 |
| W8 | `go/flag.hpp` 的 `Parse*` | 本应是 W 类，但实际自写 `strtoll/strtod` | **归入 T4 真重复**，不是薄封装 |

---

## 四、风险清单（看似可合并、实则不要动）

1. **Channel vs FastChannel**：`fast_channel.hpp` 的 header 已声明「实验版」，但它在 `include/` 里且被 5 个测试引用。**不要**把它并入 `channel.hpp`（select 语义与无锁数据面冲突），只做定位澄清。
2. **`pixel.cpp` 的 SIMD/标量双路径**：不是重复，是「逐位等价」的对拍基线。删除任何一条都会让 SIMD 正确性失去证据。
3. **`io::WaitNode` vs `sync::WaitNode`**：源码已给出「合并会漏唤醒」的结论，保持现状。
4. **6 套 Transport**：错误模型不同，抽公共基类会引入大量转发与类型擦除；若一定要统一，先统一「错误码」而不是「接口」。
5. **时间轮 / multimap / TimerService**：各自针对「命中路径」「等待路径」「中心调度」优化，合并必然牺牲其中一条热路径。
6. **`memtable` 开放寻址 vs `bucket_map` 链式**：分别服务于「写多、顺序刷盘」与「稳态淘汰、零分配」，属不同数据结构选型。
7. **`storage/kv` vs `leveldb_kv`**：删自研会丢掉零依赖构建与对照压测；删 LevelDB 会丢掉 RSS 有界的可选后端。保留双实现，补统一接口或文档。
8. **JSON 双实现**：`reflect::json` 与 nlohmann 对数字/NaN/错误的行为不同，直接替换会导致配置/调试输出漂移；先盘调用面再决策。
9. **`bento/error` 合并到 `go2cpp/error`**：虽然 T9 判为真重复，但两套 `ErrorCode` 的枚举值与 `VisitSet`/`As` 的细节不同，迁移要逐值对拍，不能机械替换。

---

## 五、建议优先级

| 优先级 | 动作 | 预期收益 | 风险 |
|---|---|---|---|
| P0 | 抽公共大端读写（T1），先替换 bento 7 个文件 | 消除 7 份复制粘贴；后续协议层只改一处 | 低 |
| P0 | 合并 `SecureRandomBytes`（T3），bento 改用 getrandom 实现 | 消除安全强度不一致 | 中 |
| P1 | compress 复用 `hash::Crc32` / `Adler32`（T8），带吞吐对比 | 减少 24 张表常量 + 获得 PCLMUL 加速 | 中 |
| P1 | `go/flag`、config、app、bento 的 Parse* 改用 `tools/strconv`（T4） | 消除 8 份解析器与溢出行为差异 | 中 |
| P1 | 抽 `AsciiToLower` / `AsciiTrimSpace` / `AsciiHasPrefix`（T5） | 消除 14 处手写，且不改变热路径语义 | 低 |
| P2 | keystore 改用 `encoding/base64` + `encoding/hex`（T6） | 密钥路径单点审计 | 低 |
| P2 | bento 改用 `go2cpp::error`（T9），删 `bento/error/error.cpp` | 去掉一套 329 行错误体系 | 中 |
| P2 | `bento/reflect` 删除或移到 tests（T14） | 去掉一套无生产使用者的反射 | 低 |
| P3 | 抽 `framing::StreamBuffer`（T11） | 三个解码器共享半包骨架 | 中 |
| P3 | `network/quic` 补服务端封装（T12） | picoquic 单点集成 | 中-高 |
| P3 | 更新 `tools/json.hpp` 的过时 TODO、`hash/hash.hpp` 的「无生产调用方」、`leveldb_kv.hpp` 的「RSS 有界」表述（T10/T7/T13） | 文档与事实一致 | 无 |
| P3 | `config` 热加载失败改走日志（T16） | 统一可观测性 | 低 |

---

## 六、审查方法与局限

1. **方法**：`grep -rn` 统计函数名/常数/类型名（`WriteU32`、`ReadU16`、`SecureRandomBytes`、`FNV`、`crc32`、`Trim`、`ParseU64`、`0123456789abcdef`、`SharedRingBuffer` 等），再逐一打开文件确认实现是否真的相同；共阅读 40+ 个头/实现文件的关键段。
2. **局限**：
   - 只覆盖 `include/` 与 `src/`，未审查 `tests/`、`example*/` 里的重复（它们可能是刻意的对照实现）；
   - 未运行构建/测试，性能判断基于文件内已有的实测注释（如 CRC 18.8 GB/s、SHA-256 0.42 GB/s），未独立复测；
   - 「是否重复」按「同一功能 + 同一语义」判定；语义不同但名字相同的（如 `Status`、`Transport`、`Channel`）归入「看似重复」，不作为合并建议；
   - 工作树有未提交改动（其他并行工作），行号以审查时刻为准，合并前需重新确认。

## 附录：正面案例（已良好复用，可作为改造模板）

- `cache/` 模块：7 个淘汰策略全部复用 `detail::Link` / `IntrusiveList` / `BucketMap` / `NodePool` / `TimestampWheel`，各策略只写策略逻辑（185~459 行），无重复的链表/哈希/池代码。
- `storage/memcache.cpp:410-411`：直接复用 `cache::detail::TimestampWheel` 与 `cache::detail::NodePool`（跨模块复用 detail，虽有层次争议，但避免了第三套时间轮/节点池）。
- `frame/frame_codec.cpp:88`、`bento/net/frame.cpp:207`、`bento/app/zip.cpp:443`：三条独立路径都收敛到 `hash::Crc32`。
- `config/config_codec.hpp`：配置编解码全部委托 Reflect，自己只做 INI 适配与旧格式兼容。
- `go/crc32.hpp`：用 `using` 别名包装 `hash::Crc32`，不复制实现。
