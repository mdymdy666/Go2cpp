# 工具库（交接文档）

> **定位**：Go 标准库风格的基础工具集 —— 哈希、编解码、压缩、反射式编解码、容器、数学、路径、字符串/数值转换，全部 header-first、零外部依赖。
> **源码**：`include/go2cpp/{hash,encoding,compress,reflect,container,math,path}/`、`include/go2cpp/tools/{strconv,strings}.hpp`；实现 `src/{hash,encoding,compress,reflect,container,math,path,tools}/`
> **构建接入**：库目标 `go2cpp_hash`（零依赖，其余模块的地基）、`go2cpp_encoding`、`go2cpp_compress`（链接 `go2cpp_hash`）、`go2cpp_reflect`、`go2cpp_container`、`go2cpp_math`、`go2cpp_path`
> **测试目标**：`go2cpp_hash_test`、`go2cpp_encoding_test`、`go2cpp_compress_test`、`go2cpp_reflect_codec_test`（ctest 名 `reflect_codec`）、`go2cpp_container_test`、`go2cpp_math_test`、`go2cpp_path_test`
> **设计/实测全文**：[asm-tools.md](../../docs/reports/asm-tools.md)（Base64 / gzip / reflect JSON 对标工业库）、[asm-reflect.md](../../docs/reports/asm-reflect.md)（紧凑格式与直读直写）、[asm-compress.md](../../docs/reports/asm-compress.md)（gzip 解压 CRC 加速）、[asm-reuse-hash.md](../../docs/reports/asm-reuse-hash.md)（SHA-256 / FNV 统一）、[asm-reuse-random-endian.md](../../docs/reports/asm-reuse-random-endian.md)（大端读写抽取）、[go-stdlib-gaps.md](../../docs/modules/go-stdlib-gaps.md)（crc32 三种多项式实测）

## 1. 这个模块是什么

九个子模块，共同点是**语义对齐 Go 标准库**（同名、同边界、同错误语义），而不是「重新发明一个 C++ 库」：

| 子模块 | 对应 Go 包 | 职责 | 依赖 |
|---|---|---|---|
| `hash` | `crypto/*`、`hash/*` | SHA-256/1、MD5、SHA-512、CRC-32 三种多项式、Adler-32、FNV-1a | 无 |
| `encoding` | `encoding/base64`、`hex`、`binary`、`csv` | 字节 ↔ 文本，以及定长整数编解码 | `error` |
| `compress` | `compress/gzip`、`zlib`、`flate` | 三格式 Reader/Writer，自研 deflate | `hash`（CRC） |
| `reflect` | 无直接对应（类 protobuf） | 结构体 ↔ 紧凑二进制 / 带 envelope 的 JSON | `hash`、`compress` |
| `container` | `container/heap`、`list`、`ring` | 三个容器，接口照 Go | 无 |
| `math` | `math`、`math/rand` | 常量、浮点判定、可复现随机数 | 无 |
| `path` | `path`、`path/filepath` | 纯字符串路径运算（**不碰文件系统**） | 无 |
| `tools/strconv` | `strconv` | `Atoi`/`ParseInt`/`ParseBool`/`ParseFloat`/`Itoa`/`FormatFloat` | `error` |
| `tools/strings` | `strings`、`unicode/utf8` | UTF-8 感知的切分、裁剪、前后缀、大小写 | `error` |

能力边界：**这是「标准库补齐」不是「通用基础设施」**——压缩只支持 gzip/zlib/flate（无 brotli/zstd/xz），容器只有三个，JSON 只服务 `reflect` 自己的 envelope 格式。

## 2. 架构 / 数据流

### 2.1 依赖分层（向下依赖，无环）

```
   业务 / app / runtime / storage
        ├──────► reflect ──┬─► hash（CRC-32 帧校验）
        │                  └─► compress（RLE / 可选压缩）
        ├──────► compress ─► hash（PCLMULQDQ CRC-32，解压热点）
        ├──────► encoding ─► error
        ├──────► container / math / path / strconv / strings（纯算法）
        └──────► hash（唯一零依赖地基）
```

`hash` 是唯一没有上游依赖的模块，因此被 `compress`、`app/resource`、`logging/redact`、`p2p`、
`bento` 共同复用——**同一份实现，不是拷贝**（2026-11 专门收敛过，见 §9）。

### 2.2 一次 `reflect` 编码的数据流

```
  T value
    ├─ CompactFieldPrefix<T>()：编译期缓存「字段数 + 全部 tag」的线上字节
    ├─ 逐字段 append 值（payload）→ EncodeEnvelope：G2RF | ver | flags | type_id | type_name | len
    ▼
  std::vector<std::uint8_t>      flags bit0 = RLE 压缩，bit1 = 紧凑载荷
                                 72 B 的 LoginRequest（自描述格式 102 B）

  Decode(data, size, out)
    ├─ DecodeBinaryCompact<T>()：直接比对 magic/type_id，payload 零拷贝按 tag 解码
    ├─ DecodeTextDirect<T>()：文本走「不建树」直读
    └─ 都返回 kUnsupportedEncoding 才回退通用路径（错误码不变）
```

### 2.3 压缩链路（读侧最贵的一段）

```
 gzip 字节流
   │ InflateDecoder::StepHuffman（879 条指令 / 21 call / 88 B 栈帧）
   │   LZ77 匹配 → CopyMatch：距离≥长度走两次 memcpy，距离<长度先落再倍增复制
   ▼
 输出缓冲 out[0..n)
   │ CommitOutput(out, out_pos)：在 kOk / kMemberEnd / kError 出口**批量**提交校验和
   │   └─ hash::Crc32::Update(m_crc, hash::IEEETable, ...)  ← PCLMULQDQ 折叠
   ▼
 校验通过 → 交还调用方
```

旧实现在 `EmitByte` 里**逐字节**更新 CRC，1 MiB 输出会切成几十万次 1 字节调用；改成出口批量提交后 CRC 从主瓶颈（占墙钟 60%–80%）降到约 10%。

## 3. 对外 API（关键接口）

### 3.1 `hash`（`include/go2cpp/hash/`）

```cpp
namespace go2cpp::hash {
ByteSpan AsBytes(const std::string& text) noexcept;      // 另有指针 / string_view / 可写重载
class Sha256 final : public Hash {
    static constexpr std::size_t kSize = 32;
    static std::array<std::uint8_t, kSize> Digest(ByteSpan data);   // 一次性，最快路径
    static bool HardwareAccelerated() noexcept;                     // SHA-NI 是否启用
};
// Md5 / Sha1 / Sha512 同构：static Digest(ByteSpan) + 流式 Write/Sum
struct Table { static constexpr std::size_t kBaseEntries = 256; };
inline constexpr std::uint32_t IEEE = 0xedb88320u;   // 同 Castagnoli / Koopman
class Crc32 final : public Hash {
    static std::uint32_t ChecksumIEEE(ByteSpan data) noexcept;
    static std::uint32_t Checksum(ByteSpan data, const Table& table) noexcept;
    static std::uint32_t Update(std::uint32_t crc, const Table& table, ByteSpan data) noexcept;
    static Crc32 New(const Table& table = IEEETable) noexcept;      // 流式
};
inline std::uint64_t Fnv1a64(ByteSpan data) noexcept;
inline std::uint64_t Fnv1a64Update(std::uint64_t state, const std::uint8_t* data,
                                   std::size_t size) noexcept;    // 显式传历史偏移量
inline std::uint32_t Fnv1a32(ByteSpan data) noexcept;
}
```

`Sha256::Digest` 用于资源校验与 `redact` 的 key id；`Crc32::Update` 用于压缩流与帧校验；`Fnv1a64` 用于候选排序/迁移指纹。`Update` 语义与 Go `crc32.Update` 一致（进入取反/离开取反），可分段拼接。

### 3.2 `encoding` 与 `compress`

```cpp
namespace go2cpp::encoding {
struct Base64Encoding {
    constexpr Base64Encoding(std::string_view alphabet, char pad, bool strict = false) noexcept;
    std::size_t Encode(std::string& dst, std::string_view src) const;
    std::string EncodeToString(std::string_view src) const;
    ErrorPtr    Decode(std::string& dst, std::string_view src) const;
    Base64DecodeResult DecodeString(std::string_view src) const;   // {data, error} + operator bool
};
inline constexpr Base64Encoding StdEncoding{...};   // 另有 URLEncoding/RawStdEncoding/RawURLEncoding
struct Hex { static std::size_t Encode(std::string& dst, std::string_view src);
             static std::string EncodeToString(std::string_view src);
             static ErrorPtr Decode(std::string& dst, std::string_view src); };
}

namespace go2cpp::compress {
class Gzip {
public:
    static constexpr int NoCompression; BestSpeed; BestCompression; DefaultCompression; HuffmanOnly;
    template <typename ReaderT> class Reader { explicit Reader(ReaderT& source);
                                               std::size_t Read(ByteSpan destination); };
    template <typename WriterT> class Writer { explicit Writer(WriterT& destination,
                                               int level = Gzip::DefaultCompression);
                                               std::size_t Write(ByteSpan source); };
};
// zlib / flate 同构；遇到 FDICT（预置字典）直接报 ErrDictionary()
}
```

`Reader`/`Writer` 是**模板**，目标类型只需提供 `Write(ByteSpan)` / `Read(ByteSpan)`（`streams` 接口或自定义适配器都行）。格式错误分类见 `compress/errors.hpp`：`ErrHeader`、`ErrChecksum`、`ErrUnexpectedEof`、`ErrDictionary`。

### 3.3 `reflect`

```cpp
namespace go2cpp::reflect {
enum class Encoding : std::uint8_t { kBinary, kText };
enum class Compression : std::uint8_t { kNone, kRle };
enum class WireFormat : std::uint8_t { kCompact /*默认*/, kSelfDescribed /*向后兼容*/ };
class ReflectCodec {
public:
    template <typename T>
    static CodecStatus Encode(const T& value, std::vector<std::uint8_t>& out,
                              Encoding encoding = Encoding::kBinary,
                              Compression compression = Compression::kNone,
                              WireFormat wire_format = WireFormat::kCompact);
    template <typename T>
    static CodecStatus Decode(const std::uint8_t* data, std::size_t size, T& out,
                              std::size_t* consumed = nullptr);
    template <typename T>
    static CodecStatus Decode(const std::vector<std::uint8_t>& bytes, T& out);
    static CodecStatus Peek(const std::uint8_t* data, std::size_t size, Envelope& out,
                            std::size_t* consumed = nullptr);
};
}
```

错误码：`kOk`、`kInvalidArgument`、`kTruncated`、`kBadVersion`、`kBadPayload`、`kBadText`、`kTypeMismatch`、`kUnknownType`、`kTooLarge`、`kUnsupportedEncoding`。失败时**目标对象不变**（解到临时对象再 move）。

### 3.4 `container` / `math` / `path` / `strconv` / `strings`

```cpp
template <typename T, typename Less = std::less<>> class Heap {   // container/heap.hpp
    void Push(const T& value);  void Push(T&& value);  T Pop();   // Pop 不做 sift
};
template <typename T> class List;    // Element<T> 带 prev/next；PushBack/PushFront/Remove
template <typename T> class Ring;    // 定长环；Len()

inline constexpr double Pi = 3.14159265358979323846264338327950288;   // math/math.hpp
inline constexpr std::int64_t MaxInt64;  inline constexpr std::uint64_t MaxUint64;
bool IsNaN(double x) noexcept;   bool IsInf(double x, int sign) noexcept;
// Inf 是「可调用常量」：Inf(1) / Inf(-1) / Inf(0)，也可 double x = Inf
// （C++ 不允许同名函数与变量共存，这里用 operator() 兼容 Go 的两种写法）

namespace go2cpp::path {             // path/path.hpp（纯字符串）
std::string Clean(std::string_view);  std::string Base(std::string_view);
bool IsAbs(std::string_view);         std::string Join(const std::vector<std::string>&);
}
namespace go2cpp::path::Filepath {   // path/filepath.hpp（注意大写 F）
std::string VolumeName(std::string_view);  std::string ToSlash(std::string_view);
std::string Clean(std::string_view);       std::string Ext(std::string_view);
std::string Base(std::string_view);        std::string Dir(std::string_view);
}

inline StrconvIntResult ParseInt(std::string_view text, int base, int bit_size);   // tools/strconv.hpp
inline StrconvIntResult   Atoi(std::string_view text);
inline StrconvBoolResult  ParseBool(std::string_view text);
inline StrconvFloatResult ParseFloat(std::string_view text, int bit_size);
inline std::string Itoa(std::int64_t value);
inline std::string FormatFloat(double value, char format, int precision, int bit_size);

inline bool HasPrefix(std::string_view text, std::string_view prefix);   // tools/strings.hpp
inline StringSlice Split(std::string_view text, std::string_view sep);   // StringSlice = vector<string>
inline std::string Join(const StringSlice& elements, std::string_view separator);
inline std::string TrimSpace(std::string_view text);
```

## 4. 最小用法（可编译示例）

```cpp
// tools_demo.cpp —— 摘要 + 十六进制 + CRC + base64，四件事一次做完
#include <cstdint>
#include <iostream>
#include <string>

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/hex.hpp"
#include "go2cpp/hash/crc32.hpp"
#include "go2cpp/hash/sha256.hpp"

int main() {
    const std::string payload = "hello go2cpp";

    // 1) SHA-256：一次性 Digest 是最快路径（整块批量压缩 + 一次填充）
    const auto digest = go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(payload));
    std::string digest_hex;
    go2cpp::encoding::Hex::Encode(
        digest_hex, std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));

    // 2) CRC-32/IEEE：与 Go 的 crc32.ChecksumIEEE 逐位一致
    const std::uint32_t crc = go2cpp::hash::Crc32::ChecksumIEEE(go2cpp::hash::AsBytes(payload));

    // 3) base64：用预定义实例，不构造新对象
    const std::string b64 = go2cpp::encoding::StdEncoding.EncodeToString(payload);

    // 4) 反向解码：失败时 error 非空，data 为空
    const auto decoded = go2cpp::encoding::StdEncoding.DecodeString(b64);
    if (!decoded) { std::cerr << "base64 decode failed\n"; return 1; }

    std::cout << "sha256=" << digest_hex << "\ncrc32=" << crc
              << "\nb64=" << b64 << "\nroundtrip=" << (decoded.data == payload) << '\n';
    return decoded.data == payload ? 0 : 1;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target go2cpp_hash go2cpp_encoding -j
g++ -std=c++17 -O2 -Iinclude tools_demo.cpp -Lbuild \
    -lgo2cpp_hash -lgo2cpp_encoding -lgo2cpp_error -o tools_demo && ./tools_demo
```

要压缩同一段字节：构造一个带 `Write(ByteSpan)` 的目标，再 `compress::Gzip::Writer<T> writer(dest)` 写进去；读侧对称地用 `Gzip::Reader<T>`。`reflect` 需要先给结构体注册字段表，宏与时序见 [reflect/codec.hpp](../../include/go2cpp/reflect/codec.hpp) 文件头。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/hash/*.hpp`（8 个） | 1,539 | `Hash` 基类、`AsBytes`、七个算法、`Table` |
| `src/hash/{hash,sha256,sha1,md5,adler32,crc32}.cpp` | 25 + 528 + 171 + 213 + 115 + 343 | SHA-NI 下沉、PCLMULQDQ CRC、slice-by-8 回退（fnv 是 header-only） |
| `include/go2cpp/encoding/*.hpp`（7 个） | 1,244 | base64/hex/binary/csv 的编码器与错误 |
| `src/encoding/{base64,hex,binary,csv}.cpp` | 361 + 117 + 359 + 419 | 预置反查表、流式 CSV、定长整数 |
| `include/go2cpp/compress/*.hpp`（7 个） | 888 | gzip/zlib/flate 门面、级别常量、错误分类 |
| `src/compress/{compress,deflater,inflater,errors}.cpp` | 61 + 135 + 114 + 62 | 门面实现；LZ77/Huffman 在 `src/compress/deflate_{writer,reader}.hpp`（header-only，被上面包含） |
| `include/go2cpp/reflect/*.hpp`（12 个） | 3,572 | `codec`、`serializer{,_impl}`、`json{,_direct}`、`field`、`type`、`buffer` |
| `src/reflect/{codec,json,type,compress}.cpp` | 361 + 805 + 146 + 152 | envelope 编解码、JSON 建树路径、类型注册表 |
| `include/go2cpp/container/*.hpp`（4 个） | 1,135 | `Heap`/`List`/`Ring` + 自由函数版 heap 操作 |
| `src/container/container.cpp` | 39 | 非模板的少量共享逻辑 |
| `include/go2cpp/math/{math,rand}.hpp` | 397 + 353 | 常量、`IsNaN`/`IsInf`/`Inf`、可复现 PRNG |
| `src/math/{math,rand}.cpp` | 223 + 427 | 幂/取整等平台细节、PRNG 状态机 |
| `include/go2cpp/path/{path,filepath}.hpp` | 219 + 346 | `path`（纯字符串）与 `Filepath`（平台分隔符/卷名） |
| `src/path/{path,filepath}.cpp` | 292 + 1,246 | 规范化算法（`filepath` 实现量最大） |
| `include/go2cpp/tools/{strconv,strings}.hpp` | 662 + 817 | `strconv` 全套解析/格式化；`strings` UTF-8 切分与大小写 |
| `src/tools/{control_flow,error}.cpp` | 354 + 415 | 与 `tools/` 其它头共用的实现（非 strconv/strings 专属） |

## 6. 性能数据

**有实测，且已对标工业库**（[asm-tools.md](../../docs/reports/asm-tools.md)，2026-11-02，`.build-bento-wsl` Release、`taskset -c 2`、三轮取代表值）：

| 项目 | 优化前 | 优化后 | 工业库（同机） | 比值 |
|---|---|---|---|---|
| Base64 编码 16 MiB | 1004 MiB/s | **1980 MiB/s** | Go `encoding/base64` 1663 | **0.84×** |
| Base64 解码 16 MiB | 600 MiB/s | **1566 MiB/s** | Go 1582 | **1.01×** |
| Base64 编码 60 B | 58.5 ns | **30.9 ns** | Go 41.9 ns | **0.74×** |
| gzip 压缩 1 MiB L6 | 235.7 MiB/s | **550 MiB/s** | zlib 421 | **0.77×** |
| gzip 解压 1 MiB | 486.8 MiB/s | **1880 MiB/s** | zlib 2984 | 1.59× |
| reflect 二进制编码（60 B 登录消息） | 225.6 ns | **79.6 ns** | protobuf 81.7 ns | **0.97×** |
| reflect 二进制解码 | 159.3 ns | **111.7 ns** | protobuf 78.1 ns | 1.43× |
| reflect 文本编码（含 envelope） | 340.4 ns | **259.4 ns** | rapidjson 103.8 / nlohmann 477.3 | 2.50× / **0.54×** |
| reflect 文本解码（含 envelope） | 929.3 ns | **772.3 ns** | rapidjson 275.9 / nlohmann 809.3 | 2.80× / **0.95×** |
| JSON 文档解析（285 B） | 334.3 ns | **292.4 ns** | rapidjson 275.9 | **1.06×** |

上表是**第一轮**结论（7 项达标 / 3 项未达标）。之后两件事把未达标项也压下去了：

1. **gzip 解压**（[asm-compress.md](../../docs/reports/asm-compress.md)）：校验和从逐字节改成出口批量提交 + 复用 `hash::Crc32`（PCLMULQDQ）。同场 zlib 对照 64 KiB 输出块：**5219.7 MiB/s vs zlib 3633.0 MiB/s，耗时比 0.696×（快 1.44×）**；`hash::Crc32` 单函数 **20.3–20.7 GB/s**（改造前 slice-by-16 约 3 GB/s 量级）。压缩侧无回归（530.7 → 530.6 MiB/s）。
2. **reflect 紧凑格式 + 直读直写**（[asm-reflect.md](../../docs/reports/asm-reflect.md)）：四项全部 **< 1.1×** —— 二进制编码 **51.4–55.9 ns**（protobuf 81.7，**0.63–0.68×**）、二进制解码 **51.5–54.5 ns**（**0.66–0.70×**）、文本编码 **69.6–72.3 ns**（rapidjson 103.8，**0.67–0.70×**）、文本解码 **147.7–165.9 ns**（rapidjson 275.9，**0.54–0.60×**）。同一条 `LoginRequest` 从 102 B 降到 **72 B（-29.4%）**。

**hash 的实测**（[asm-reuse-hash.md](../../docs/reports/asm-reuse-hash.md)，1e6 组逐字节对拍）：`Sha256::Digest`（32 B，硬件路径）从 156.6–179.9 ns 降到 **52.6–56.3 ns**；SHA-256 吞吐从 1.85–2.28 GB/s 提到 **2.0–2.4 GB/s**（`go2cpp_hash_test` 1 GiB 压测 **2146~2370 MB/s**）；`GO2CPP_HASH_DISABLE_SHA_NI=1` 强制标量时 **0.41–0.44 GB/s**；`Redact(key)` 端到端从 199–234 ns 降到 **77–87 ns**（测试实测 80~85 ns）；FNV-1a 1,000,000 组 / 384.4 MB **0 mismatch**。

CRC-32 三种多项式吞吐（[go-stdlib-gaps.md](../../docs/modules/go-stdlib-gaps.md) §2.3，64 × 1 MiB 单核）：**IEEE 18.38 GB/s、Castagnoli 12.18 GB/s、Koopman 3.20 GB/s**。

**`container` / `math` / `path` / `strconv` / `strings` 没有 asm 级实测**：`docs/reports/` 下没有对应报告，本文不写指令数或 ns 数字。要量请按 §8 建基准，不要引用本文件之外的数字。

## 7. 边界与已知限制

1. **`reflect` 紧凑格式依赖收发双方 schema 一致**：字段数不一致直接 `kBadPayload`，同名字段类型不符解码失败；跨版本升级建议先用 `kSelfDescribed` 过渡。紧凑 + RLE 同时开时解码走通用路径（正确但非零拷贝）。
2. **文本直读只对顶层是结构体的消息生效**：内置类型顶层仍走建树路径；`EstimateJson` 按最坏转义（6×）估算，输出缓冲可能偏大。
3. **`compress` 无字典支持**：遇到 zlib `FDICT` 直接报 `ErrDictionary()`，不是静默忽略。
4. **base64 大端机器走逐字节路径**：32-bit store 由 `__BYTE_ORDER__` 分支保护，大端不享受批量写。
5. **`container` 全部无并发保护**：`Heap`/`List`/`Ring` 是裸容器，多线程要自己加锁；刻意与 Go 的 `container/*` 一致。
6. **`math::Inf` 是可调用常量**：`Inf(1)` 与 `double x = Inf` 都成立，但不能取它的函数地址。
7. **`path` 只做字符串运算**：不做 `stat`、不解析符号链接、不保证结果在磁盘上存在；`Filepath` 才处理平台分隔符与卷名。
8. **`strconv::ParseFloat` 不接受前后空格**，但接受 `inf`/`infinity`/`nan`（大小写不敏感）。
9. **`strings` 的 UTF-8 语义有边界**：非法字节序列按 Go 的 `RuneError` 规则处理，`TrimSpace` 只裁 Unicode 空白集合，不等于「裁所有控制字符」。
10. **`hash::Sha256` 的硬件路径可被环境变量关掉**：`GO2CPP_HASH_DISABLE_SHA_NI=1` 强制标量，只用于验证回退路径，别在生产里设。
11. **FNV 的历史偏移量不能改**：`p2p/{endpoint,candidate}.cpp` 与 `media/audio.cpp` 用少一位的 `1469598103934665603`，该值已进入候选排序 / foundation 字符串 / 音频校验结果。

## 8. 测试与验证方法

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j12

ctest --test-dir build -R 'go2cpp_hash_test' --output-on-failure
ctest --test-dir build -R 'go2cpp_encoding_test' --output-on-failure
ctest --test-dir build -R 'go2cpp_compress_test' --output-on-failure        # 1506 checks
ctest --test-dir build -R 'reflect_codec' --output-on-failure              # 11056 checks
ctest --test-dir build -R 'go2cpp_(container|math|path)_test' --output-on-failure
```

| 测试目标 | 覆盖重点 |
|---|---|
| `go2cpp_hash_test` | 标准向量、1e6 组随机对拍、SHA-256 一次性 vs 流式、FNV 增量组合律、CRC 三种多项式 |
| `go2cpp_encoding_test` | RFC 4648 向量、Python 参照向量、256 字节全值往返、随机/压力、CSV 引号与错误 |
| `go2cpp_compress_test` | GNU gzip / Python zlib 双向互操作、多成员、stored/fixed 回退、损坏检测、短写、1e6 CRC 对拍 |
| `go2cpp_reflect_codec_test` | 随机往返、3000 轮 fuzz、RLE 压缩炸弹、帧粘包/半包、6 个真实场景、紧凑格式 10 组、直读直写 14 组 |
| `go2cpp_container_test` | Heap/List/Ring 与 Go 语义对拍、随机、场景、压力 |
| `go2cpp_math_test` / `go2cpp_path_test` | 常量与浮点边界、`filepath` 与 Go oracle 逐值比对 |

自建最小闭环（验证「同一份实现」而不是「看起来一样」）：用本模块压一段文本 → `gzip -t` + `gzip -d` 解回原文（双向互操作）；`python3 -c 'import zlib; print(hex(zlib.crc32(b"123456789")))'` 应等于 `Crc32::ChecksumIEEE`；用 `GO2CPP_HASH_DISABLE_SHA_NI=1` 重跑 `go2cpp_hash_test`，两次摘要必须逐字节相同；用 `kSelfDescribed` 编码、`kCompact` 解码同一结构体，字段值必须全等。

## 9. 交接注意

- **不要新增第三份实现**。2026-11 的收敛把 SHA-256 从 3 份合成 1 份、FNV-1a 64 位从 4 份合成 1 份，代价是 1e6 组逐字节对拍。再复制一份之前先看 [asm-reuse-hash.md](../../docs/reports/asm-reuse-hash.md) 的替代表。
- **改压缩或哈希先跑报告里的复现命令**：`tests/bench/bench_tools_industry.cpp`、`bench_gzip_crc.cpp`、`bench_reflect_stage.cpp` 都是**手工编译**的基准，不进 CMake——复现必须按报告里的 `g++` 命令行来。
- **`compress` 与 `hash` 的链接方向是单向的**：`go2cpp_compress → go2cpp_hash`，反向不行；`hash` 必须保持零依赖，否则 `logging/redact` 会把整个 compress 拖进日志模块。
- **`reflect` 默认格式已变成 `kCompact`**：旧数据靠 flags bit1=0 走自描述路径兼容；改字段表或 flags 位必须同时更新兼容矩阵。
- **`tools/strconv.hpp` 与 `go/flag.hpp` 有一份重复的 Duration 解析**：`flag` 内联了 `detail::ParseDurationText`，等 tools 层补齐 `ParseDuration` 后应改为转发并删掉内联实现。
- **`path` 与 `Filepath` 是两个命名空间**：`go2cpp::path` 是纯字符串版本，`go2cpp::path::Filepath` 是平台版本（大写 F）。
- **未完成**：zlib 字典、reflect 压缩载荷快路径、`hash/sha512` 无硬件路径；CRC 已 20 GB/s，不是瓶颈。
- **本目录没有独立设计文档**：`docs/modules/tools.md` 讲的是 `tools/` 目录下的 CLI / 迁移 / 回放工具，**不是**本文这九个库。要完整背景只能读四份 `asm-*.md` 与头文件注释。
