# 基础设施统一规范（logging / config / error）

> 适用范围：`src/` 下所有模块的新增与修改代码，以及 `include/go2cpp/` 中带实现的头文件。
> 目的：日志、配置、错误三条横切关注点只有一套入口，才能做到统一过滤、热更新、
> 链路追踪与错误聚合；散落的 `printf` / `getenv` / 裸错误码会让排障退化成 grep。

## 1. 三条硬性规则

### 1.1 日志：必须用 `go2cpp::logging`

- 唯一入口：`GO2CPP_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL(GO2CPP_LOG_NAME("module"))`
  （`include/go2cpp/logging/log.hpp`）。
- **禁止**：`printf` / `fprintf(stdout|stderr, ...)` / `std::cout` / `std::cerr` /
  直接写日志文件。日志模块自身的实现（`src/logging/`）是唯一例外。
- 理由：统一级别过滤、格式化、轮转、脱敏、重入保护与测试 sink 注入；
  `std::cout` 绕过全部策略，且多线程下会与 `StdoutSink` 交错。

### 1.2 配置：必须用 `go2cpp::config`

- 唯一入口：`go2cpp::config::Config`（`include/go2cpp/config/config.hpp`），
  需要支持热更新时用 `watch.hpp` / `so_reloader.hpp`。
- **禁止**：模块内散落 `getenv` / `secure_getenv` / 自己解析 ini、json、命令行。
- 环境变量作为**外部来源**由配置层统一读取，模块不得直接 `getenv`：
  - 批量注入：`Config::FromEnv(prefix)` 按 `GO2CPP_<PREFIX>_<MODULE>_<KEY>`
    覆盖已注册变量（启动期在文件加载之后调用，优先级最高）；
  - 单值读取：`go2cpp::config::GetEnv(name)` / `GetEnvOr(name, fallback)`
    （`include/go2cpp/config/env.hpp`）。该头是 header-only，供不能依赖
    `go2cpp_config` 的底层库（scheduler/sync/fiber）使用，避免
    `config -> scheduler` 的循环依赖；返回值区分「未设置」与「空串」。
- 理由：配置来源、优先级、类型转换、默认值、热更新与审计集中在一处；
  散落 `getenv` 无法被配置系统观测，也无法在测试中隔离。

### 1.3 错误：必须用 `go2cpp::error`

- 唯一入口：`ErrorPtr`（`std::shared_ptr<const Error>`）、`ErrorCode`、
  `MakeError<T>` / `Wrap` / `Join`（`include/go2cpp/error/error.hpp`）。
- **禁止**：用 `int` 返回码、`bool` + 出参、模块私有 `enum class` 表达失败；
  也禁止把错误文本拼成字符串后只返回 `std::string`。
- 理由：错误需要携带类型链、错误码、原因链与上下文；只有统一的 `ErrorPtr`
  才能被 `ErrorScope` 聚合、被日志系统结构化打印、被上层用 `Is()` 判定。
- 同一 Fiber/线程内连续发生多个错误时，用 `ErrorScope`
  （`include/go2cpp/error/error_scope.hpp`）收集，详见第 4 节。

## 2. 正反例

### 2.1 日志

```cpp
// 正例 1：按模块取 logger，级别语义明确
GO2CPP_LOG_INFO(GO2CPP_LOG_NAME("io")) << "reactor started, workers=" << workers;

// 正例 2：错误路径带上下文，不吞异常
GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("net")) << "dial failed: " << err->Message()
                                        << " target=" << address;

// 正例 3：测试注入 MemorySink 后断言日志内容
auto sink = std::make_shared<go2cpp::log::MemorySink>();
```

```cpp
// 反例 1：直接 std::cout，绕过级别/轮转/脱敏
std::cout << "reactor started" << std::endl;

// 反例 2：printf 拼参数，格式串与实参不匹配时是 UB
printf("dial failed: %s\n", err->Message().c_str());

// 反例 3：把日志写到固定文件，测试与其他模块都无法接管
std::ofstream("debug.log") << "something happened\n";
```

### 2.2 配置

```cpp
// 正例 1：从统一配置读取，缺失时显式给默认值
const int workers = config.GetInt("io.workers", 4);

// 正例 2：配置项集中声明，便于生成文档与校验
config.Declare("net.timeout_ms", config::Type::kInt, /*default=*/3000);

// 正例 3：需要热更新时订阅变更，而不是每次调用 getenv
watcher.OnChange("net.timeout_ms", [this](const config::Value& value) {
    m_timeout_ms.store(value.AsInt());
});
```

```cpp
// 反例 1：散落 getenv，测试无法隔离、配置系统看不到
const char* level = std::getenv("LOG_LEVEL");

// 反例 2：每个模块自己解析同名环境变量，默认值各不相同
const int timeout = std::getenv("TIMEOUT_MS") ? std::atoi(std::getenv("TIMEOUT_MS")) : 1000;

// 反例 3：运行期改配置只能重启进程
static const int kWorkers = std::atoi(std::getenv("WORKERS") ?: "4");
```

### 2.3 错误

```cpp
// 正例 1：返回不可变错误指针，带类型与错误码
go2cpp::error::ErrorPtr Open(const std::string& path) {
    if (fd < 0) {
        return go2cpp::error::MakeError<go2cpp::error::IOError>("open failed", path);
    }
    return nullptr;
}

// 正例 2：保留原因链，便于上层按类型判定
return go2cpp::error::Wrap(ParseConfig(path), "load config");

// 正例 3：Fiber 内多个错误统一收集，退出时自动打印
auto& scope = go2cpp::error::ErrorScope::Current();
scope.Add(MakeError<TimeoutError>("probe timeout"), "probe");
```

```cpp
// 反例 1：int 返回码，错误码含义只存在于注释里
int Open(const std::string& path);  // 0 = ok, -1 = fail, -2 = timeout ...

// 反例 2：模块私有错误枚举，上层必须 include 实现细节才能判断
enum class NetResult { kOk, kTimeout, kRefused };

// 反例 3：把错误吞成 bool，丢失原因与上下文
bool Parse(const std::string& text) {
    try { ParseInternal(text); return true; } catch (...) { return false; }
}
```

## 3. 允许的例外

1. `src/logging/`、`src/config/` 自身的底层实现可以使用 `std::cout` / `getenv`，
   因为它们是这两个入口的实现者；
2. 测试代码（`tests/`）可以用 `std::cout` 输出测试进度；但断言日志行为仍必须
   走 `MemorySink`；
3. 第三方库适配层（如 `src/bento/`、`third_party/` 封装）在**无法**避免时，
   必须在文件头注释说明原因，并把输出转发到 `go2cpp::logging`。
4. `src/os/proc.cpp` 的子进程环境遍历（`environ` 继承）属于 POSIX 语义，
   传递的是**子进程环境**而不是应用配置，保留 `::getenv`；
5. `include/go2cpp/config/env.hpp` 是配置系统的环境变量来源适配层，其内联
   `getenv` 属于 `src/config/` 实现的一部分（header-only 是为了避免
   `go2cpp_config -> go2cpp_scheduler` 与底层库之间的循环依赖）。

## 4. ErrorScope：Fiber 链级多重错误

- 一个 Fiber/线程内共享一份收集器，按 `Add` 顺序保存，不去重；
- `TakeAll()` 取走并清空；未取走的错误在 Fiber 正常结束时由 trampoline
  自动逐条打 WARN（错误文本、错误码、类型、上下文），随后析构错误对象；
- 自动抽干可用 `GO2CPP_ERROR_SCOPE_NO_AUTO_DRAIN`（编译期）或
  `SetErrorScopeAutoDrainEnabled(false)`（运行期）关闭；
- 收集器基于 `ErrorLocal`：链接了 Fiber 运行时则随 G 迁移，普通线程退化为
  线程局部；同一 Fiber 内单线程访问，不需要加锁。

## 5. 检查命令与当前基线

原始命令（统计 `src/` 下绕过统一入口的写法）：

```bash
grep -rn "printf\|std::cout\|getenv" src/ | grep -v "logging/config"
```

**当前结果（2026-11-04 11:42）：`scripts/check_infra_usage.sh` 报告 1 处**，即
`src/os/proc.cpp:633` 的子进程环境继承（已列入第 3 节例外清单）。第二批
迁移把 scheduler/sync/fiber/debug/hook/hash/bento/os-file/audio 的 13 处
`getenv` 收敛到 `go2cpp::config::GetEnv`，加上首批 tunnel/os 的 42 处，
基线从 91 行降到 1 行；config 新增 `FromEnv(prefix)` 批量环境变量来源。

> 历史基线（2026-11-04 11:05）：`grep` 原始命令 98 行，排除
> `src/logging/`、`src/config/` 后 96 行，再排除纯注释行 91 行。仅作量级参考。

热点文件（迁移前基线，2026-11-04 11:05；首批与第二批已清零，仅供了解历史分布）：

| 文件 | 命中 |
|---|---:|
| `src/tunnel/server_main.cpp` | 16 |
| `src/tunnel/client_main.cpp` | 16 |
| `src/os/proc.cpp` | 9 |
| `src/frame/frame_codec.cpp` | 7 |
| `src/concurrency/scheduler.cpp` | 6 |
| `src/debug/diagnostic_agent.cpp` | 4 |
| `src/debug/debug_event_ring.cpp` | 4 |
| `src/os/monitor/monitor.cpp` | 3 |
| `src/storage/seglog.cpp` | 2 |
| `src/storage/leveldb_kv.cpp` | 2 |

统一自检脚本（输出违规文件列表；有违规退出 1，干净退出 0）：

```bash
scripts/check_infra_usage.sh          # 只看文件列表
scripts/check_infra_usage.sh --verbose  # 附带 file:line 明细
```

## 6. 迁移顺序建议

1. **入口/CLI**（`src/tunnel/*_main.cpp`）：先把 stdout 输出换成 logging，
   行为不变、风险最低；
2. **OS/框架层**（`src/os/`、`src/frame/`）：把 `getenv` 收敛到 `config`，
   默认值在配置层声明；
3. **热路径**（`src/concurrency/`、`src/network/`）：只替换 `printf`，
   错误返回值另行迁移到 `ErrorPtr`，避免同时改动语义；
4. 每批改完跑 `scripts/check_infra_usage.sh` 与对应模块测试，数字只降不升。
