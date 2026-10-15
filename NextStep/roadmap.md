# 路线图：从「运行时语义库」到「能转译真实 Go 业务」

> 本文件把 `go-stdlib-map.md`（标准库映射）、`third-party-plan.md`（第三方生态预研）与
> `../ArchitectureDiagram/`（现状架构）收敛成**可执行的分阶段计划**，并给出每阶段的
> **计划产出时间、完成定义（DoD）、验收方式与风险缓解**。
> 时间单位为「周」，以「阶段」而非日历日期排期（避免与真实排期假设冲突）。

---

## 0. 现状基线（起点）

| 维度 | 现状 | 证据 |
|---|---|---|
| 运行时语义 | **已完整**：Fiber/GMP/Channel/Select/Context/同步原语/Timer/EPOLL+Hook | 架构图 L1~L5 与 9 份模块报告 |
| 协议层 | **解析完整、生态缺失**：HTTP/WebSocket/SSE 的解析与基本收发有；无 TLS、无连接池、无路由/中间件 | `modules/G7-protocols.md` |
| 标准库 | **基本空白**：`fmt`/`strings`/`strconv`/`io`/`os`/`time`/`sort` 均无对应 | `NextStep/go-stdlib-map.md` |
| 第三方生态 | **无** | 本文件 §3 |
| 工程保障 | 构建 0 warning、ctest 69/69、示例 40+ | 2026-10-15 全量验证 |

**结论**：转译「真实 Go 业务」的瓶颈**不在运行时，而在标准库**。

---

## 1. 关键路径（不可跳过的顺序）

```text
              ┌─ fmt / strings / strconv / bytes / sort   （P0，无依赖）
              │
 标准库底座 ──┼─ io（Reader/Writer 接口）──┬─ bufio
              │                            ├─ os（File/Getenv/Args）
              │                            └─ net 抽象（Conn/Listener）
              │
              └─ time（Duration/Now/Sleep/Ticker，复用 TimerService）
                                    │
                        encoding/json 结构体编解码（需先定策略）
                                    │
              ┌─────────────────────┴──────────────────────┐
              │                                            │
        http 生态（ServeMux/Handler/中间件/连接池）     日志与配置生态
              │                                            │
        Web 框架移植（gin/chi 类）                     zap/viper 类移植
```

**为什么 io 必须先于 os/bufio/http**：`io::Reader/Writer` 是这三者的公共接口；
先立接口，后续模块才能「面向接口」实现，避免各写一套缓冲逻辑。

---

## 2. 阶段计划

### S1｜基础工具（P0，第 1 周）

| 项 | 内容 |
|---|---|
| 交付 | `tools/fmt.hpp`、`tools/strings.hpp`、`tools/strconv.hpp`、`tools/bytes.hpp`、`tools/sort.hpp` |
| DoD | 每个模块：头文件 + `example/tools/*_{demo,full}.cpp`（demo ≥3 场景、full ≥6 场景）+ CMake 与 `example/README.md` 登记 + 测试（大数据量/随机/多场景） |
| 验收 | `build-quality` 0 warning；`ctest` 全绿；示例输出逐条与注释预期一致 |
| 风险 | `fmt` 的 `%v` 语义（Go 的通用格式化）需要类型分派 → **用模板 + `if constexpr`，不做反射** |
| 复用优先 | 先检索 C++ 侧实现（fmtlib/fmt 是 MIT）；若引入，必须记录来源/版本/许可证并评估头文件体积（参考 nlohmann 的先例） |

### S2｜IO 抽象与 OS（P0，第 1–2 周）

| 项 | 内容 |
|---|---|
| 交付 | `io.hpp`（`Reader`/`Writer`/`Closer`/`Copy`/`ReadAll`/`LimitReader`）、`os.hpp`（`File`/`ReadFile`/`WriteFile`/`Getenv`/`Args`/`Exit`）、`time.hpp`（`Duration`/`Time`/`Now`/`Since`/`Sleep`/`Ticker`） |
| DoD | `time.Sleep` 必须接到 Hook 的协作式睡眠（`cooperative_sleep_for`），不得裸 `nanosleep` 占住 M |
| 验收 | 示例覆盖：文件读写往返、超大文件分块、`Sleep` 在 Fiber 中让出 M（用 `Scheduler::metrics()` 轮询验证） |
| 风险 | `os::File` 与 `FdManager`/Hook 的非阻塞标志交互 → 复用 `FdContext{m_user_nonblock, m_system_nonblock}` 语义 |

### S3｜容器与工具补齐（P1，第 2–3 周）—— **全部按薄封装实施**

`bufio.hpp`、`path/filepath.hpp`、`slices.hpp`、`maps.hpp`、`cmp.hpp`、`math.hpp`、`math/bits.hpp`、
`encoding/binary.hpp`（补 `Read`/`Write`/`Varint`）。

> **实现方式已定为「薄封装」**（2026-10-15 经两轮反馈收敛）：不重写算法，只提供与 Go 同名的 API
> 并在内部一行调用 C++ 标准库（`std::ranges`、`std::unordered_map`、C++20 `operator<=>`、
> `std::filesystem`、`<cmath>`、`<bit>`）。这样转译后 API 名一一对应，转译器不必硬编码标准库类型。
> **每个模块动手前必须先做「差别核对」**（逐项对比 Go 与标准库语义，有差别处显式处理；
> 已知差别：`sort.Float64s` 的 NaN 语义、`sort.Search`/`Find` 的返回语义、`math.Mod` 与 `%` 的符号规则），
> 核对结果写进该模块文件头。差别清单见 `go-stdlib-map.md` §4.4。

DoD：每个模块 ≤300 行（薄封装通常远小于此）+ 示例 + 测试 + **差别说明**。

### S4｜并发与编码补齐（P1，第 3–4 周）

`sync` 补 `Once`/`Pool`/`Map`；`sync/atomic` 薄封装；`math/rand`；`encoding/base64`/`hex`。

DoD：`sync::Map` 必须给出「Fiber + 线程混合」的并发测试（≥3 生产者 ≥3 消费者 + 随机键 + 10 万次操作）。

### S5｜JSON 结构体编解码（P1，第 4–6 周，**含一次技术决策**）

| 项 | 内容 |
|---|---|
| 决策点 | **策略 A**：代码生成（Go 结构体 → C++ 结构体 + 映射代码），**策略 B**：手写 tag 注册（`JSON_BIND(field, "name")` 宏） |
| 交付 | `tools/json.hpp` 扩展：`Marshal`/`Unmarshal` 到结构体（含嵌套/切片/指针/`omitempty`） |
| DoD | 与 Go `encoding/json` 的语义对齐测试：字段名大小写、tag 优先、空值、数字精度、转义、深层嵌套（复用 `JsonScanDepth` 栅栏） |
| 风险 | Go 的 `encoding/json` 依赖反射 + struct tag；**策略 A 需要转译器配合**，策略 B 需要用户标注 → 建议先做 B（可独立交付），A 作为转译器侧后续工作 |

### S6｜HTTP 生态与模板（P2，第 6–8 周）

`http` 补 `ServeMux`/`Handler`/`HandleFunc`/中间件链/`Client` 连接池与 keep-alive/重定向/Cookie；
`text/template`；`os/exec`；`flag`；`container/*`（薄封装：`heap`→`std::priority_queue`、
`list`→`std::list`，`ring` 无等价物需自行实现约 30 行）；`hash/*`（`fnv`/`crc32` 无标准等价物，需实现）。

> `container/*` 按薄封装实施，见 `go-stdlib-map.md` §4.4。

DoD：用 Go2Cpp 写一个**真实小服务**（路由 + JSON API + 优雅关闭），并作为示例登记。

### S7｜长尾与移植（P3，第 9 周起）

`regexp`（评估移植 [google/re2](https://github.com/google/re2)，BSD-3）、`crypto/sha256`·`hmac`·`rand`（移植成熟 C 实现）、
`testing` 风格、`math/big`（评估 GMP，LGPL 需谨慎）。

---

## 3. 第三方生态阶段（依赖 S1~S6）

> 详细取舍见 `third-party-plan.md`。这里只给**依赖顺序**。

| 阶段 | 计划产出时间 | 前置 | 第三方目标 | 说明 |
|---|---|---|---|---|
| **T1** | **第 4–5 周** | S1+S2+S4 | 工具类：`google/uuid`、`shopspring/decimal`、`yaml.v3`、`pflag`、`toml` | 无 Web 依赖、纯算法/解析，**最容易移植**；逐库时间见 `third-party-plan.md` §3.5 |
| **T2** | **第 6–7 周** | T1+S5 | 配置与日志：`viper`（改为自研薄层）、`zap`、`zerolog` | 依赖 `encoding/json`/`yaml`/`reflect`；`zap` 需要 `sync/atomic` 与结构化字段 |
| **T3** | **第 8–9 周** | S6 | CLI 与服务：`cobra`、`urfave/cli`、`grpc-go`（评估） | `cobra` 依赖 `pflag`；`grpc` 依赖 HTTP/2（Go2Cpp 无）→ **暂不移植** |
| **T4** | **第 9 周起** | T2+S6+S5 决策 | Web 与数据：`chi`/`gin`/`echo`、`go-redis`、`nats.go`、`prometheus` | `gin` 依赖 `net/http`+`reflect`+`validator`；ORM 依赖 `database/sql`+反射 → **需先有 S5 的策略决策** |

**结论**：第三方库的移植**不可能早于标准库底座**，且 Web/ORM 类还额外要求 `reflect` 替代方案
（本项目用代码生成/显式绑定，见 `go-stdlib-map.md` §4.3）。

---

## 4. 里程碑与验收

| 里程碑 | 计划产出时间 | 验收标准 |
|---|---|---|
| **M1「能写工具程序」** | 第 2 周末（S1+S2） | 用 Go2Cpp 写一个读文件 → 处理字符串 → 输出 JSON 的命令行程序，示例登记并进 ctest |
| **M2「能用容器与并发」** | 第 4 周末（+S3+S4） | 上述程序加上「多 goroutine 分片处理 + WaitGroup + 原子计数」，压测 10 万条 |
| **M3「能做 JSON 服务」** | 第 6 周末（+S5） | 一个 JSON API 服务：路由 + 结构体编解码 + 优雅关闭 |
| **M4「能接入生态」** | 第 8 周末（+S6+T1+T2） | 用 T1/T2 移植件（uuid/decimal/yaml/zap 类）重构 M3 的服务 |
| **M5「有转译闭环」** | 第 9 周起 | 至少 3 个「Go 源文件 → 手工转译 → Go2Cpp 编译运行」的端到端样例，纳入仓库 |

---

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| `fmt`/`json` 的 Go 语义细节多（`%v`、tag、精度） | 转译结果「看起来对但边界错」 | 用**对拍测试**：同输入在 Go 与 Go2Cpp 下输出必须逐字节一致（Go 可作离线参考，不引入构建依赖） |
| `reflect` 缺失导致 ORM/框架不可移植 | T4 无法开始 | 先做「显式绑定」路线（宏/代码生成），把反射需求**前移到转译器**；T4 延后 |
| 大量薄封装模块导致维护面膨胀 | 长期维护成本 | 统一模板（头文件 + demo/full + 测试 + 登记），并复用同一套 `tools` 测试骨架 |
| 每模块都要写测试与示例，工时被放大 | 排期延误 | 明确 DoD 模板，按 §2 的阶段顺序串行推进，避免并行铺开 |
| 上游 C++ 库（fmtlib/yaml-cpp 等）许可证与体积 | 合规与编译时间 | 沿用 nlohmann 的先例：单头/源码 + `docs/dependencies.md` 记录来源与许可证 + 不进聚合头 |
| 性能结论被误用 | 决策失真 | 性能一律 `build-bench`（Release）测量，写入 `docs/performance.md` 的日期分节 |

---

## 6. 与现有交付物的关系

| 本文件章节 | 上游依据 |
|---|---|
| §0 现状基线 | `../ArchitectureDiagram/modules/G1~G10`、`00-architecture.svg` |
| §2 S1~S7 | `go-stdlib-map.md` §4.2（同一套阶段划分，此处补 DoD 与验收） |
| §3 T1~T4 | `third-party-plan.md` |
| §5 风险 | 项目规范（复用优先/测试要求/性能门禁）+ 本轮实测（`codex.md` 第 25 轮的 nlohmann 先例） |
