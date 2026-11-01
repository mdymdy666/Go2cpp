# frame：通用帧编解码（FrameCodec）

> 状态：现行（2026-11-01，REQ-001 交付）。
> 源码：`include/go2cpp/frame/frame_codec.hpp` + `src/frame/frame_codec.cpp`；
> 测试：`tests/frame/test_frame_codec.cpp`；构建：`cmake/Go2CppFrame.cmake`。
> 目标消费者：JinShan 的 RDRL / RDCH 两条控制面协议（同一套机制、两份 `FrameSpec`）。

## 1. 定位

`go2cpp::frame::FrameCodec` 只做一件事：**把字节流切成一条条完整帧**。

- 定界：`magic + version + length`，半包自动续接、粘包一次全出；
- 上限：帧头声明的 `length` 超过 `max_payload` 立即报错，不预分配、不截断；
- 校验：`checksum` 可插拔（空 = 不校验，默认 CRC-32/IEEE，可换任意函数）；
- **不解析载荷语义**：`type` / `flags` / `payload` 原样搬运。

它**不做**的事（都属于调用方语义）：类型取值校验、错误码表、半帧超时计时、
加密、重传、连接管理、多路复用。

## 2. 目录与构建

```text
include/go2cpp/frame/frame_codec.hpp   # FrameSpec / Frame / FrameCodec / Error
src/frame/frame_codec.cpp              # 编解码实现（复用 hash::Crc32）
tests/frame/test_frame_codec.cpp       # 验收测试（单文件，自包含）
cmake/Go2CppFrame.cmake                # go2cpp_frame（STATIC）+ go2cpp_frame_codec_test
docs/modules/frame-codec.md            # 本文
```

| 目标 | 类型 | 说明 |
|---|---|---|
| `go2cpp_frame`（别名 `go2cpp::frame`） | STATIC | 只依赖 `go2cpp_hash`（CRC-32）与 C++17 |
| `go2cpp_frame_codec_test` | EXECUTABLE | ctest 名 `go2cpp_frame_codec_test` |

开关：`-DGO2CPP_WITH_FRAME=OFF` 关闭（默认 ON，只新增目标，不改既有目标）；
测试随 `GO2CPP_BUILD_TESTS` 构建。

```bash
cmake -S . -B .build-bento-wsl
cmake --build .build-bento-wsl --target go2cpp_frame_codec_test -j8
cd .build-bento-wsl && ctest -R go2cpp_frame_codec_test --output-on-failure
# 直接跑（打印帧率 / 峰值内存）：
./.build-bento-wsl/go2cpp_frame_codec_test
```

## 3. 线上格式

```text
 偏移  长度          字段        说明
 0     4             magic       4 字节魔数，逐字节相等（"RDRL"/"RDCH"/自定义）
 4     1             version     必须等于 FrameSpec::version，不协商降级
 5     1             type        上层消息类型（本层不解释，0x00–0xFF 均可）
 6     2             flags       上层标志位，小端（本层不解释）
 8     4             length      载荷字节数，小端，≤ max_payload
 12    header_len-12 reserved    扩展区（仅 header_len > 12）：编码写 0，解码跳过，
                                参与校验和计算
 header_len          length      payload
 ...                4           checksum（仅当 checksum 非空，小端）
```

- 多字节整数**一律小端**（与 RDRL/RDCH 一致）；
- 校验和覆盖 **帧头前 `header_len` 字节 + payload**，**不含校验和自身**；
  把 `length` 纳入校验，篡改长度会被校验和发现，而不是悄悄改变帧边界；
- 校验和放**帧尾**（不是帧头），这样 12 字节帧头与 RDRL/RDCH 现有布局逐字节一致；
- 帧头 12 字节是固定前缀，`header_len > 12` 只是给别的协议预留扩展区。

单帧字节数：

| checksum | 整帧长度 |
|---|---|
| 非空（默认） | `header_len + length + 4` |
| 空（不校验） | `header_len + length` |

## 4. API

```cpp
namespace go2cpp::frame {

struct Error {                      // 值类型：错误码 + 文本
    ErrorCode code = ErrorCode::kNone;
    std::string message;
    bool Ok() const noexcept;
    void Clear() noexcept;
};

struct Frame {                      // 一条完整帧
    std::uint8_t type = 0;
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> payload;
};

struct FrameSpec {
    std::array<std::uint8_t, 4> magic{{0, 0, 0, 0}};   // 必须显式设置
    std::uint8_t version = 1;
    int header_len = 12;            // [12, 4096]
    int max_payload = 65536;        // >= 0
    std::function<std::uint32_t(const std::uint8_t*, std::size_t)> checksum =
        &Crc32IeeeFrameChecksum;    // 默认 CRC-32/IEEE；置空 = 不校验

    bool HasChecksum() const noexcept;
    bool Valid() const noexcept;
    std::size_t MaxFrameBytes() const noexcept;   // 单帧最大字节数（有界缓冲的依据）
};

constexpr std::array<std::uint8_t, 4> MakeMagic(const char (&text)[5]) noexcept;
FrameSpec MakeFrameSpec(std::array<std::uint8_t, 4> magic, std::uint8_t version = 1,
                        int max_payload = 65536) noexcept;

class FrameCodec {
public:
    explicit FrameCodec(FrameSpec spec);

    bool Encode(const Frame& frame, std::vector<std::uint8_t>* out, Error* err) const;
    bool Feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>* frames,
              std::size_t* consumed, Error* err);
    void Reset() noexcept;

    const FrameSpec& Spec() const noexcept;
    bool SpecValid() const noexcept;
    std::size_t BufferedBytes() const noexcept;   // 未成帧尾部字节数（诊断）
};
}  // namespace go2cpp::frame
```

### 4.1 语义约定

| 项 | 约定 |
|---|---|
| `Encode` | 自动填 magic/version/length/校验和；`out` **先清空**，失败时保持为空（不留半帧）；载荷超限返回 `kPayloadTooLarge`，**不截断** |
| `Feed` | 追加语义：解出的帧**追加**到 `*frames`（不清理调用方已有内容） |
| `consumed` | 成功时 **恒等于 `len`**（字节已被编解码器接管，未成帧尾部留在内部缓冲）；失败时是出错前已处理的字节数，出错帧的帧头**不计入** |
| 半包 | 正常路径：返回 `true`、`*frames` 为空、`*consumed == len`；剩余字节留内部缓冲 |
| 粘包 | 一次 `Feed` 解出全部完整帧，顺序不变 |
| 出错 | 返回 `false`，内部缓冲**立即清空**（帧边界已不可信，调用方应断开连接）；出错前已解出的完整帧仍留在 `*frames` 里 |
| `Reset` | 丢弃半包并释放缓冲（重连 / 换协议）。不影响 spec 与已交付的 Frame |
| `err` 指针 | 可传 `nullptr`；进入 `Encode`/`Feed` 时先 `Clear()`，成功时保持空 |
| 线程模型 | 一个实例对应一条字节流，`Feed` 不可并发；`Encode` 只读 spec，可并发（自定义 checksum 需无共享状态） |

### 4.2 错误码

| 码 | 触发条件 | 调用方动作 |
|---|---|---|
| `kNone` | 成功 | — |
| `kInvalidArgument` | 出参为空 / `data == nullptr && len != 0` | 修代码（连接状态不变） |
| `kInvalidSpec` | `header_len ∉ [12,4096]` 或 `max_payload < 0` | 修配置（连接状态不变） |
| `kPayloadTooLarge` | `Encode` 的载荷 > `max_payload` | 拒绝发送，不截断 |
| `kLengthExceeded` | 解码时帧头 `length > max_payload` | 断开连接（协议违规） |
| `kBadMagic` | 前 4 字节不匹配 | 断开连接（可能是端口被别的协议占用） |
| `kBadVersion` | 版本字节不匹配 | 断开连接（跨版本不互通） |
| `kChecksumMismatch` | 校验和不匹配 | 断开连接（重传由上层决定） |
| `kOutOfMemory` | 缓冲分配失败 | 断开连接（解码路径不抛 `bad_alloc`） |

`ErrorCodeName(code)` 返回不带空格的短名（`"bad-magic"` 等），可直接做日志/指标标签。

## 5. 与 RDRL / RDCH 对接

RDRL 帧（JinShan `docs/05-detailed/protocol.md` §3.1）就是本文 §3 在
`header_len = 12`、`checksum = CRC-32/IEEE` 下的特例：

| RDRL 字段 | 本模块 |
|---|---|
| `magic` = `52 44 52 4C`（"RDRL"） | `FrameSpec::magic`（`MakeMagic("RDRL")`） |
| `version` = 1 | `FrameSpec::version` |
| `type` = 1–9 | `Frame::type`（本层不校验取值） |
| `flags`（发送置 0、接收忽略未知位） | `Frame::flags`（本层原样搬运） |
| `length` ≤ 65536 | `FrameSpec::max_payload = 65536` |
| `crc32` 覆盖帧头 12B + payload | `FrameSpec::checksum`（默认 CRC-32/IEEE，帧尾 4B） |

```cpp
// 控制面：RDRL（64 KiB 上限，CRC32 必开）
go2cpp::frame::FrameCodec rdrl(
    go2cpp::frame::MakeFrameSpec(go2cpp::frame::MakeMagic("RDRL"), 1, 65536));

// 应用面：RDCH（4 MiB 上限；实际控制消息 ≤ 48 KiB 是上层约束）
go2cpp::frame::FrameCodec rdch(
    go2cpp::frame::MakeFrameSpec(go2cpp::frame::MakeMagic("RDCH"), 1, 4 * 1024 * 1024));

// 每条连接一个实例；recv 到多少喂多少：
std::vector<go2cpp::frame::Frame> frames;
std::size_t consumed = 0;
go2cpp::frame::Error err;
if (!rdrl.Feed(buffer, n, &frames, &consumed, &err)) {
    // err.code ∈ {bad-magic,bad-version,length-exceeded,checksum-mismatch}
    // → 记日志（只记 code/长度，不记 payload 与 key）后关闭连接
}
```

对接时要由 JinShan 自己补的部分：

1. **类型/错误码语义**：`type 1..9`、`RESPONSE.status`、`ERROR.code` 都是上层表；
2. **半帧 30 s 超时**（RDRL §3.3 / `ERR-PROTO-005`）：本模块不持有时钟，调用方在
   `BufferedBytes() > 0` 时用单调时钟（如 REQ-005 的 `Lease`/`Heartbeat`）计时；
3. **粘包缓冲上限**：本模块已按 `MaxFrameBytes()` 有界（`12 + 65536 + 4 = 65552`），
   JinShan 无需再实现一次上限检查；
4. **加密/隧道**：RDRL 当前是明文 TCP + 密钥配对，本模块不碰加密；
5. **每连接一实例**：不要把同一实例跨连接复用，半包状态会串流。

### 5.1 与 `bento::net::Frame` 的关系

仓库里已有 `bento::net::Frame`（`include/go2cpp/bento/net/frame.hpp`，20 字节 `BENT` 头 +
`FrameFormat{magic,version}`）。两者定位不同，**不要混用**：

| | `bento::net::Frame` | `go2cpp::frame::FrameCodec` |
|---|---|---|
| 帧头 | 固定 20B（`BENT`/u32 magic + u16 version + reserved） | 12B 起，魔数/版本/上限可配 |
| 校验 | 固定 CRC-32（头前缀 + 载荷） | 可插拔（空 / CRC32 / 自定义） |
| 错误 | `FrameStatus` 枚举 + 清空缓冲 | `Error{code,message}` + 清空缓冲 |
| 用途 | Bento Link 内部传输层 | RDRL/RDCH 等外部协议 |

Bento 传输层继续用 `bento::net::Frame`；**JinShan 的 RDRL/RDCH 用本模块**。

## 6. 内存与性能

- **有界**：内部缓冲上限 = `FrameSpec::MaxFrameBytes()`；帧头一旦声明
  `length > max_payload` 立即报错，不会按声明值预分配（1e6 帧压测 peak buffered = 134 B）；
- **拷贝次数**：常用路径（整帧都在本次输入里）只拷贝一次 payload（`Frame::payload` 本就持有副本），
  不复制整帧；`Encode` 直接写进调用方的 `out`；
- **实测**（WSL2 / GCC 13 / Release `-O3` / 单线程 / 混合载荷 0–119 B）：
  1e6 帧 encode→随机切分 Feed→逐帧字节校验 **0.218 s ≈ 4.59 Mframes/s**，
  载荷合计 59.5 MB，进程 VmHWM 8.58 MB；
- 压测代码在 `tests/frame/test_frame_codec.cpp`（`TestStressMillionFrames`），
  ASan/UBSan/LSan 下同一用例通过（无泄漏、无越界，见 §7）。

## 7. 测试

`tests/frame/test_frame_codec.cpp`（自包含，无外部测试框架依赖）：

| 用例 | 覆盖点 |
|---|---|
| 黄金向量 | RDRL REGISTER / HEARTBEAT、RDCH HELLO 的线上字节与 zlib CRC 逐字节一致 |
| spec 矩阵 | RDRL(64 KiB) / RDCH(4 MiB, version=3) 各 23 种 type 的 encode→Feed→decode，字段与字节双重一致 |
| 半包 | 100 B 帧按 1 B 喂 100 次，逐字节断言「未收满不交付」 |
| 粘包 | 3 帧连发一次 Feed 返回 3 帧、`consumed` 精确；3 帧 + 半帧尾部 |
| 畸形 | magic/version/length/checksum 四类错误码可区分；错误前完整帧仍交付；错误后缓冲清空 |
| 校验和 | nil（不校验，篡改不报错）/ 自定义 FNV（每帧恰好回调一次） |
| 上限与配置 | `max_payload` 边界、非法 spec、`header_len=16` 扩展区（写 0 且参与校验） |
| 参数 | 空指针、`err` 预置后清空、`frames` 追加语义、`out` 复用 |
| 随机 | 5000 帧随机切分重组，逐帧一致 |
| fuzz | 200 轮垃圾字节 + 300 轮单字节变异，不崩、缓冲有界、错误可恢复 |
| 真实场景 ×5 | RDRL 握手（随机 TCP 分段）/ RDCH 48 KiB（黄金 CRC `0xdf0d4fdb`，黄金向量由 Python zlib 独立算出）/ 双协议共存 / 错误后重连 / 恶意 length 头内存放大防护 |
| 压测 | 1e6 帧，逐帧校验，无泄漏（ASan/LSan 复核） |

运行：

```bash
cd .build-bento-wsl && ctest -R go2cpp_frame_codec_test --output-on-failure
# ASan/UBSan/LSan 复核（同一份源码，独立编译）：
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude tests/frame/test_frame_codec.cpp src/frame/frame_codec.cpp \
    -L.build-bento-wsl -lgo2cpp_hash -o /tmp/frame_test_asan
LD_LIBRARY_PATH=.build-bento-wsl /tmp/frame_test_asan
```

## 8. 限制与已知边界

1. `FrameCodec` **不保证安全**：明文、无认证，CRC 只检错不防篡改（要防篡改用 HMAC/SHA-256）；
2. **不重传、不排序**：有序可靠字节流（TCP）之上的定界器；UDP 需要上层加序号/重传；
3. **没有超时**：半帧 30 s 之类的计时由调用方做（本模块只暴露 `BufferedBytes()`）；
4. 载荷上界是 `int`，`max_payload` 传负数即 `kInvalidSpec`；单帧上限即 `MaxFrameBytes()`；
5. `header_len > 12` 的扩展区**语义未定义**：编码写 0、解码跳过、参与校验——需要自定义
   字段的协议请在 `FrameSpec` 之外自己约定，或改用 `Encode` 后手工改写（会破坏校验和，不推荐）；
6. `Feed` 一次调用会把 `*frames` 追加到调用方容器，**不预留容量**：批量解码时应由调用方
   `frames.reserve(n)` 控制内存。
