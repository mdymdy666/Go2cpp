# bento-foundation 模块组：错误码、反射与 UDP 传输

> **状态**：现行（2026-10-28）；基线 `afc135a`（`git -C /UserData/CodexWorkSpace/Go2cpp rev-parse --short HEAD` 实测）。
> **范围**：`bento::error`、`bento::reflect`、`net::UdpTransport` 与 `KeyStore` 的 error 接缝。
> **源码**：8 个头文件共 1660 行（`include/go2cpp/bento/{error,reflect,net}`），3 个实现共 2150 行（`src/bento/{error,net}`）；`reflect` 为 header-only。
> **引用约定**：签名逐字取自头文件；不写行号，按符号名检索；结论尽量标注来源文件。
> **测试**：`tests/bento/test_{error,reflect,udp_transport,ipv6,keystore}.cpp`，入口 `ctest -R '^bento_'`。
> **边界**：keystore 的加载/轮换细节见 [bento-security.md](bento-security.md)，分层说明见第 8 节。

## 1. 模块清单

| 模块 | 命名空间 | 头文件 | 实现 | 测试 | 职责 |
|---|---|---|---|---|---|
| error | `go2cpp::bento::error` | `error/error.hpp`、`error/mapping.hpp`、`error/registry.hpp` | `src/bento/error/{error,mapping,registry}.cpp` | `test_error.cpp`（40 用例）、`test_error_support.hpp` | 不可变错误图、错误码与运行期码表、HTTP/日志/JSON 映射 |
| reflect | `go2cpp::bento::reflect`（`caster` 子命名空间） | `reflect/reflect.hpp`、`reflect/bind.hpp`、`reflect/caster.hpp` | 无（header-only） | `test_reflect.cpp`（18 用例） | 标量类型描述、严格文本转换、结构体字段绑定 |
| udp_transport | `go2cpp::bento::net` | `net/udp_transport.hpp` | `src/bento/net/udp_transport.cpp` | `test_udp_transport.cpp`（15 用例）、`test_ipv6.cpp`（2 用例，间接） | `ITransport` 的数据报实现：无连接收发 + `Connect` 默认对端 |
| keystore（补充） | `go2cpp::bento`（**不是** `bento::net`） | `net/keystore.hpp` | `src/bento/net/keystore.cpp` | `test_keystore.cpp`（20 用例） | 本文只写其 `ErrorPtr` 返回与错误码映射；完整 API 见 [bento-security.md](bento-security.md) |

## 2. 定位

- **error 是唯一零业务依赖的地基**：`error.hpp` 只 include 标准库，不引用任何 Bento 业务头（其 `@details` 明示），因此可被工具层、`net` 与外部调用方共享。错误以 `ErrorPtr = std::shared_ptr<const Error>` 传递，节点构造后不可变。
- **reflect 服务于「配置/序列化绑定」而非运行时对象内省**：它刻意不引入 RTTI 或宏生成的结构体描述表，字段列表由使用方在 `FieldList<T>` 显式声明，编译期即可完成类型检查（`reflect.hpp` @details）。
- **udp_transport 补齐 `ITransport` 的无连接语义**：`ITransport` 只抽象「已连接或可连接的可靠字节流」；UDP 在此之外增加 `Bind`/`SendTo`/`RecvFrom`，并把 `Connect` 降级为「设置默认对端」（`udp_transport.hpp` @details）。
- **依赖方向**：`app::session` 通过 `std::function<std::unique_ptr<net::ITransport>()>` 注入传输（`app/session.hpp:268`），`UdpTransport` 是其中一个实现；`net` 目录内只有 `keystore.hpp` 直接 include `error/error.hpp`（实测 `grep -rln 'bento/error' include/go2cpp/bento/net/` 只命中它）。`net::Link` 自带 UDP socket、不引用 `ITransport`，两条 UDP 代码路径并存。
- **两套「失败」语义并存**：传输层用 `net::Code` + `Result::system_error`（`transport.hpp`），不抛异常也不构造 `ErrorPtr`；`error` 体系面向需要错误图、跨层包装与 JSON 诊断的调用方，二者目前没有自动转换函数。

## 3. API 参考

### 3.1 error

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `enum class ErrorCode : std::int32_t`；`std::string_view ErrorCodeName(ErrorCode code) noexcept` | `kOk=0` … `kDeadlineExceeded=14`，`kWrappedError=15`；码名未知/私有码返回 `"UNKNOWN"` | 0–14 对齐 gRPC；业务私有码从 `>=1000` 起 `static_cast` |
| `ErrorPtr NewError(std::string message, ErrorCode code = ErrorCode::kUnknown)`；`ErrorPtr Wrap(ErrorPtr cause, std::string message)`；`ErrorPtr Join(std::vector<ErrorPtr>)`（另有 `initializer_list` 重载） | 创建叶子 / 单原因包装 / 多原因聚合；`Wrap` 的 `cause` 为空、`Join` 全部为空时返回空指针 | `Wrap` 参数顺序是「原因在前」；`Join` 过滤空指针 |
| `bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept`；`bool Has(const ErrorPtr& error, ErrorCode code) noexcept`；`template <typename T> std::shared_ptr<const T> As(const ErrorPtr& error) noexcept` | 图查询：身份匹配 / 码匹配 / 动态类型匹配；`Is` 的 `target` 为空时返回 `!error` | 均环安全；`T` 必须派生自 `Error` |
| `WalkResult WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept`；`std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error, std::size_t level = 1)`；`std::string ErrorMessage(const ErrorPtr&)`；`bool IsWrappedError(const ErrorPtr&) noexcept` | 前序 DFS（访问器返回 `kContinue`/`kSkipChildren`/`kStop`）；取直接原因或全部后代（`level == 0`）；错误文本；是否非空包装节点 | 每个对象最多访问一次；`UnwrapAll`/`ErrorMessage` 空指针分别返回空/空串 |
| `ErrorCodeRegistry::Instance()`；`Register(std::string_view)`；`Name(std::uint32_t)`；`Size()`；`Contains(std::string_view)`；`Lookup(std::string_view)`；`codes::kProtocol` … `codes::kConstraint` | 进程级码表：空名返回 0、同名幂等；未注册 `Name` 返回 `"<unknown>"`、`Lookup` 返回 0；10 个内置码常量 | `Name()` 视图指向内部 `deque`，后续注册不失效；内置码数值由注册表首次分配，**不要硬编码** |
| `ErrorTypeRegistry::Instance()`；`Register(std::string_view, std::string_view, std::uint8_t, ErrorFactory)`；`Register<T>()`；`Create(std::string_view)`；`Has`；`ParentName`；`Depth`；`Size`；`LastError`；`LastErrorDetail`；`ClearLastError` | 类型名 → 工厂/父类型/深度；`Create` 未注册或工厂异常返回空 | 深度上限 `kMaxErrorDepth = 3`；超限返回 false 并写 `LastError`（`kDepthLimitErrorCode = 15`） |
| `BENTO_ERROR_CLASS(TypeNameValue, ParentNameValue, DepthValue)`；`Error::Message()`、`Code()`、`TypeName()`、`ParentTypeName()`、`InheritanceDepth()`、`IsWrapping()`、`UnwrapAll(level)`、`Is(target)` | 生成 `TypeName`/`ParentTypeName`/`InheritanceDepth` 三个 override；其余节点接口 | `Message()`/`Code()` 仍需手写；`Message()` 假定无环，查询接口才环安全 |

**mapping**：任务描述中的 `MapError` 在代码中不存在，真实映射入口是下列三个函数（`mapping.hpp`）。

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `struct MappingPolicy { HttpStatusPolicy http_status; LogLevelPolicy log_level; JsonPolicy json; }`；`MappingPolicy MakeDefaultMappingPolicy()`；`const MappingPolicy& DefaultMappingPolicy()` | 可注入策略集合；某项为空回退默认；进程级默认策略为函数内静态 | 三个 `std::function` 可分别替换 |
| `int DefaultHttpStatus(ErrorCode code) noexcept`；`LogLevel DefaultLogLevel(ErrorCode code) noexcept` | 默认 HTTP / 日志等级映射；未知/私有码分别 500、`kError` | `kCancelled→499`/`kDebug`；`kResourceExhausted→429`；`kDeadlineExceeded→504` |
| `std::string DefaultJson(const ErrorPtr& error)`；`std::string MapJson(const ErrorPtr&, const MappingPolicy& = DefaultMappingPolicy())` | 错误图 JSON；空指针返回 `"null"`；`MapJson` 按策略回退 | 深度上限 64；环/共享子图重复节点为 `null` |
| `int MapHttpStatus(ErrorCode, const MappingPolicy& = DefaultMappingPolicy())`；`int MapHttpStatus(const ErrorPtr&, ...)`；`LogLevel MapLogLevel(ErrorCode, ...)`；`LogLevel MapLogLevel(const ErrorPtr&, ...)` | 错误码/错误 → HTTP 状态或日志等级；空错误视为 `kOk` | 策略项为空时回退；序列化本身不抛异常（字符串分配失败除外） |

### 3.2 reflect

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `template <typename T> struct TypeTraits`；`enum class FieldKind : std::uint8_t`；`struct TypeDescriptor { const char* name; std::size_t size; bool is_numeric; FieldKind kind; }`；`constexpr TypeDescriptor Describe<T>() noexcept`；`NameOf<T>()`；`SizeOf<T>()` | `kName`/`kNumeric`/`kKind`/`Caster`；`kString`/`kInt`/`kDouble`/`kBool`；类型描述、类型名、`sizeof`（`const T`/`T&` 先归一化） | 已特化 `int`、`double`、`bool`、`std::string`；未特化得到 `"unknown"`、`Caster = void` |
| `caster::StringToInt`、`StringToDouble`、`StringToBool`、`StringToString` | 严格整串解析：拒绝空白、尾随垃圾、`inf/nan/0x`、溢出 | 不读 locale，不调用 `strtod` |
| `template <typename T> struct FieldBinder { const char* name; std::function<bool(T&, std::string_view)> apply; }`；`template <typename T> FieldBinder<T> BindField(const char* name, Field T::*member)` | 单字段绑定器；成员指针 → 绑定器，字段类型自动推导 | 不能绑定 `const` 字段（`static_assert`） |
| `template <typename T> struct FieldList`；`bool Bind(T&, const Map&, const BindOptions&, BindError*)` 等 5 个重载 | 字段列表特化点（未特化无法 `Bind`）；在副本上应用全部字段，全部成功才提交，失败对象不变 | `Bind(const Map&)` 返回 `std::optional<T>`；需用户显式特化 `FieldList` |
| `struct BindOptions { bool reject_unknown; bool require_all; }`；`struct BindError { BindErrorKind kind; std::string field; std::string value; }` | 未知键/缺失字段策略；失败详情 | 默认都 false；`kNone`/`kUnknownField`/`kWrongType`/`kMissingField` |
| `#define BENTO_BIND_FIELD(Type, field)` | 展开为 `BindField(#field, &Type::field)` | 必须在 `FieldList<Type>::Fields()` 内使用 |

### 3.3 udp_transport

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `Result Bind(const Endpoint& local, bool reuse_address = false)` | 绑定本地地址；`host` 空为通配，端口 0 由内核分配 | 已打开返回 `kError + EALREADY` |
| `Result Connect(const Endpoint& endpoint, std::chrono::milliseconds timeout) override` | 只设置默认对端（UDP connect），不握手 | 成功≠对端在线；重复调用 `EALREADY`；跨地址族 `EAFNOSUPPORT` |
| `Result Read(std::uint8_t* buffer, std::size_t capacity, std::chrono::milliseconds timeout) override` | 从已连接对端读一个数据报 | 未连接 `kError + ENOTCONN`；`capacity == 0` 返回 `kOk + 0`；截断**静默** |
| `Result Write(const std::uint8_t* data, std::size_t size, std::chrono::milliseconds timeout) override` | 向已连接对端发一个数据报 | 原子发送，成功 `bytes == size`；未连接 `EDESTADDRREQ`；过大 `EMSGSIZE` |
| `Result SendTo(const Endpoint& peer, const std::uint8_t* data, std::size_t size, std::chrono::milliseconds timeout)`；`Result RecvFrom(std::uint8_t* buffer, std::size_t capacity, std::chrono::milliseconds timeout, Endpoint* peer, RecvInfo* info = nullptr)`；`struct RecvInfo { std::size_t bytes; std::size_t datagram_bytes; bool truncated; }` | 向任意对端发送（无需 `Connect`）；收一个数据报并返回来源与实际/原始长度；`peer`/`info` 可为 nullptr | 允许零长度数据报；基于 `recvmsg(MSG_TRUNC)`；`truncated` 为 true 时返回码仍是 `kOk`；失败时 `RecvInfo` 不保证被修改 |
| `Result ShutdownWrite() override`；`Result Close() override`；`Result LocalEndpoint(Endpoint*) const`；`Result PeerEndpoint(Endpoint*) const`；`bool is_open() const noexcept`；`bool is_connected() const noexcept` | 只置本地「不再发送」标志；幂等关闭 socket；`getsockname`/`getpeername` 的数字形式；fd 有效 / 已 `Connect` | UDP 无 FIN，对端收不到通知；关闭后所有操作 `kClosed`；未连接 `ENOTCONN`；参数空 `kError` |

**与 `ITransport` 的语义差异**：`Read` 一次只返回一个数据报（不是任意字节流）、`Write` 不会部分写、`Connect` 不建立连接、`ShutdownWrite` 不产生对端可见事件、`Read` 的截断不可恢复；需要来源地址或截断信息时必须用 `RecvFrom`（`udp_transport.hpp` @details）。

头文件中**没有** `SetReadTimeout` 成员：超时按每次调用的 `std::chrono::milliseconds timeout` 传入（整次调用的 deadline，`0` 表示非阻塞尝试一次），不保存在对象状态里；这是与「对象级读超时」API 的关键差异。

### 3.4 keystore 与 error 的接缝

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `error::ErrorPtr LoadFromString(std::string_view)`；`LoadFromFile(const std::string&)`；`LoadFromEnv(const std::string&)` | 成功返回 `nullptr`；失败返回 `ErrorPtr`，已加载密钥不变 | 完整加载语义见 [bento-security.md](bento-security.md) |
| `static Key FromBytes(const std::uint8_t* data, std::size_t bytes)`；`static bool ConstantTimeEquals(const Key&, const Key&) noexcept`；`(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t)` | 仅接受 16/32 字节（否则空 Key）；常量时间比较，两者均无效视为相等 | `kKeyBytes128 = 16`、`kKeyBytes256 = 32`、`kKeyMaxBytes = 32`；长度不同返回 false（长度非秘密） |
| `struct Config { std::uint64_t rotation_retention_ms = 5 * 60 * 1000; bool check_file_permissions = true; std::size_t max_input_bytes = 4096; }` | 轮换窗口、权限检查、单次输入上限 | 权限宽松只告警不拒绝 |
| `template <typename Target> bool ApplyPreSharedKey(Target&, const KeyStore::Key&)` | 把 Key 应用到任意有 `SetPreSharedKey(data, size)` 的对象 | 16B Key 对 `PeerHub`/`Link` 返回 false（它们要 32B） |

**errno → `ErrorCode` 映射（`keystore.cpp` 实测）**：`EACCES`/`EPERM → kPermissionDenied`；`ENOENT`/`ENOTDIR → kNotFound`；其余打开失败 `→ kUnavailable`。非 errno 路径：编码/长度/空输入 `→ kInvalidArgument`；超过 `max_input_bytes → kResourceExhausted`；读中文件变化 `→ kDataLoss`；Key 构造失败 `→ kInternal`。错误消息包含系统错误文本与路径/环境变量名，但不包含密钥明文或输入原文。

## 4. 设计要点

1. **错误码分段与稳定 ABI**：`ErrorCode` 是固定枚举（0–15），其数值进入 JSON 的 `"code"` 字段，不能重排；业务私有码用 `static_cast<ErrorCode>(>=1000)`。`ErrorCodeRegistry` 另行分配运行期稳定整数（从 1 递增、0 永久保留给 `kOk`），同名幂等，`Name()` 视图不受后续注册影响；因此内置码的**具体数值**不是 ABI，只保证进程内稳定。
2. **错误上下文不丢**：`Wrap` 逐层保留 message，`WrappedError::Message()` 把连续包装展平成 `"a: b: c"`，`JoinError` 用换行聚合，`UnwrapAll(0)` 可取全部后代。注意 `Error` 本身**没有** errno 字段：errno 由 `net::Result::system_error` 保留，`keystore` 则把 errno 文本拼进 `Message()` 并映射成 `ErrorCode`，不要把 errno 当作 `Error` 的通用属性。
3. **mapping 的方向是「bento → 外部表示」**：`MapHttpStatus`/`MapLogLevel`/`MapJson` 只读 `ErrorCode` 与错误图，不读 errno、不写日志、不依赖 HTTP 库；反方向的「errno → `net::Code`」分类由各传输实现内部的 `MapIoError` 完成（`tcp_transport.cpp`、`udp_transport.cpp` 各自一份），两套映射互不复用。
4. **反射的取舍是零 RTTI + 非侵入注册**：`TypeTraits` 只有四个特化，字段表由 `FieldList<T>` 手写；未支持类型在 `Bind` 的 `static_assert` 处编译失败，而不是运行时静默跳过。`caster` 手写严格解析且不读 locale，避免配置解析受部署环境影响。
5. **UDP 的 `Connect` 只是设默认对端**：`::connect` 对 UDP 不产生握手报文，只让内核绑定默认地址并开始投递 ICMP 错误；因此「Connect 成功」不代表对端在线，`ECONNREFUSED` 往往在下一次 `Read`/`Write` 才出现。这是与 `TcpTransport` 最容易被误用的差异。

## 5. 边界与已知限制

- **错误消息没有长度上限**：`NewError`/`Wrap`/`Join` 不截断文本，`Message()` 直接拼接各段，深链会生成与链长成正比的字符串。JSON 侧有边界：深度上限 64，超过时节点带 `"truncated":true`；环/共享子图的重复节点序列化为 `null`；`Message()`/`UnwrapAll()` 抛异常时降级为 `"<message unavailable>"` / `"children_error":true`。`error.cpp` 用线程局部释放队列摊平深链析构，10 万层 `Wrap` 不会爆栈；查询接口用地址去重防环，但 `Message()` 明确假定无环。
- **注册表并发与失败处理**：`ErrorCodeRegistry` 与 `ErrorTypeRegistry` 都用互斥量保护，可并发注册/查询；`Create` 复制工厂后在锁外调用，允许工厂重入注册表。`LastError()` 只是诊断快照，并发失败时只保证「最后一次写入可见」，不保证因果顺序；成功不会自动清零。类型深度超过 `kMaxErrorDepth`（3）只返回 false 并写 `LastError`，**不 assert**，调用方必须检查返回值。
- **反射支持的字段类型只有 `int`/`double`/`bool`/`std::string`**：不支持嵌套结构、容器、枚举、`const` 字段；字段名区分大小写；`Bind` 要求 `T` 可复制构造且可赋值，失败时对象保持原值（强保证），代价是一次额外拷贝。
- **UDP 语义限制**：不保证送达、不保证顺序、不重传、不合并；接收缓冲不足时内核丢弃超出部分且无法补读，`ITransport::Read` 的截断是静默的；connected socket 收到 ICMP port unreachable 后可能返回 `ECONNREFUSED`，应视为对端不可达而非传输层故障。`UdpTransport` 不加锁：`Read`/`RecvFrom` 与 `Write`/`SendTo` 可分线程，`Bind`/`Connect`/`Close` 与其它操作并发需调用方协调。
- **keystore 边界补充**：只接受 16/32 字节；`max_input_bytes` 默认 4096；`LoadFromFile` 对宽松权限只告警仍加载；`KeyStore` 非线程安全，作为配置期对象使用；`Key::ToHex()`/`data()` 暴露明文，禁止写日志，日志与错误只用 `Masked()`。

## 6. 测试数据

| 测试文件 | 用例 / 断言处（静态统计） | 覆盖点 |
|---|---|---|
| `tests/bento/test_error.cpp` | 40 用例 / 118 处 `BENTO_ERROR_CHECK*` | 错误图、展平、`Is`/`As`/`Has`/`WalkErrors`、注册表幂等与并发、HTTP/日志/JSON 映射、10 万层深链与环 |
| `tests/bento/test_reflect.cpp` | 18 用例 / 64 处 | `TypeTraits`/`Describe`/`NameOf`/`SizeOf`、四种 caster 的合法/边界/非法输入、`Bind` 完整/部分/失败/未知/缺失语义、失败不改对象、随机往返 |
| `tests/bento/test_udp_transport.cpp`、`tests/bento/test_ipv6.cpp` | 15 + 2 用例 / 153 + 27 处 `BENTO_CHECK*` | Bind/LocalEndpoint/重复 Bind、100 包回环、超时与非阻塞、16 KiB 大包、截断检测、Connect/PeerEndpoint、未连接错误、EMSGSIZE、多发送方、ShutdownWrite、IPv6 `::1`；`test_ipv6.cpp` 另测 `Link` 双向 STREAM，不是 `UdpTransport` 专项 |
| `tests/bento/test_keystore.cpp` | 20 用例 / 200 处 | 加载/拒绝路径/脱敏/常量时间/轮换/泄漏审计/`PeerHub`+`Link` 集成；错误码映射只在断言中间接覆盖 |

口径说明：用例数取 `grep -c '^void Test'`，断言处取 `grep -c 'BENTO_ERROR_CHECK('` 或 `grep -c 'BENTO_CHECK'`（含 `BENTO_CHECK_EQ`），均为**源码静态出现次数**，循环内实际断言更多；[bento-modules.md](bento-modules.md) 的 58 用例/16657 断言与 15/158 是运行期口径，两者不矛盾。运行 `ctest -R '^bento_'`（注册名 `bento_error`、`bento_reflect`、`bento_udp_transport`、`bento_ipv6`、`bento_keystore`，见 `cmake/Go2CppBento.cmake`）；本次仅做静态统计与注册名核对，**未实际执行 ctest**。

## 7. 示例

示例 1：构造错误图，注册/查找类型，走三种映射。

```cpp
#include <cstdio>
#include <string>
#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/bento/error/mapping.hpp"
#include "go2cpp/bento/error/registry.hpp"
using namespace go2cpp::bento::error;
class TimeoutError final : public Error {
 public:
    BENTO_ERROR_CLASS("demo.TimeoutError", "bento.Error", 1)
    std::string Message() const override { return "timeout"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kDeadlineExceeded; }
};
int main() {
    ErrorCodeRegistry::Instance().Register("demo.protocol");
    if (!ErrorTypeRegistry::Instance().Register<TimeoutError>()) return 1;
    ErrorPtr root = Wrap(Join({NewError("read failed", ErrorCode::kUnavailable),
                               ErrorTypeRegistry::Instance().Create("demo.TimeoutError")}),
                         "session 42");
    std::printf("http=%d log=%s json=%s\n", MapHttpStatus(root),
                std::string(LogLevelName(MapLogLevel(root))).c_str(), MapJson(root).c_str());
    return As<TimeoutError>(root) && Has(root, ErrorCode::kDeadlineExceeded) ? 0 : 1;
}
```

编译（仓库根目录，示例落盘为 `demo_error.cpp`）：`g++ -std=c++17 -Iinclude demo_error.cpp src/bento/error/{error,registry,mapping}.cpp -o demo_error`

示例 2：用 `FieldList` + `BENTO_BIND_FIELD` 绑定结构体字段。

```cpp
#include <cstdio>
#include <map>
#include <string>
#include "go2cpp/bento/reflect/bind.hpp"
using namespace go2cpp::bento::reflect;
struct Config { std::string host = "127.0.0.1"; int port = 8080; };
namespace go2cpp::bento::reflect {
template <> struct FieldList<Config> {
    static std::vector<FieldBinder<Config>> Fields() {
        return {BENTO_BIND_FIELD(Config, host), BENTO_BIND_FIELD(Config, port)};
    }
};
}
int main() {
    const std::map<std::string, std::string> values{{"host", "0.0.0.0"}, {"port", "9090"}};
    Config config;
    BindError error;
    if (!Bind(config, values, BindOptions{false, true}, &error)) return 1;
    std::printf("%s:%d\n", config.host.c_str(), config.port);
    return 0;
}
```

编译（`reflect` 为 header-only，示例落盘为 `demo_bind.cpp`）：`g++ -std=c++17 -Iinclude demo_bind.cpp -o demo_bind`

两个示例均按头文件签名核对；**未在本机编译验证**（本任务只写文档）。

## 8. 相关文档

- [bento-transport.md](bento-transport.md)：`ITransport`/TCP/内存传输与帧编解码。
- [bento-security.md](bento-security.md)：`KeyStore` 的完整 API、加载/轮换/常量时间比较与安全属性。
- [bento-session.md](bento-session.md) / [bento-app.md](bento-app.md)：会话表、心跳、重连与会话编排；应用层 fetch/file_transfer/backup/session。
- [bento-scenarios.md](bento-scenarios.md)：跨模块场景与端到端用例。
- [PROTOCOL.md](../../tests/bento/PROTOCOL.md)：Bento 线上协议与帧格式。
- [bento-modules.md](bento-modules.md)：模块覆盖矩阵与验证口径。
