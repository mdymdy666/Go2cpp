# 第三方库预研：候选、取舍与依赖顺序

> 读者：决定「Go2Cpp 要不要接某个生态」的人。
> 结论先行：**第三方库的移植不可能早于标准库底座**（见 `roadmap.md` §3），
> 而且 Web 框架 / ORM 这一类**在 `reflect` 缺位的前提下无法直接移植** ——
> 必须先把「反射需求」前移到转译器（代码生成/显式绑定）。

---

## 1. 方法与判据

### 1.1 许可证核验方式

- **已核验**：直接抓取仓库 LICENSE 原文（URL 见下表），属于**一手证据**。
- **未核验**：标注为「公开已知，动手前须核验」——本项目规范要求移植前记录
  「仓库地址 + commit/tag + 许可证 + 移植日期」，因此**核验动作发生在动手那一刻**，不在本文件里假装做完。

### 1.2 可移植性判据（五项，逐项打分）

| 判据 | 说明 | 为何关键 |
|---|---|---|
| **R 反射依赖度** | 是否用 `reflect` 做字段映射/动态调用/标签解析 | Go2Cpp **没有反射**（`go-stdlib-map.md` §4.3 已列为不做），高依赖即不可直接移植 |
| **S 标准库依赖面** | 是否要求 `net/http` 完整语义、`database/sql`、反射版 `encoding/json` | 这三者 Go2Cpp 目前只有「解析/传输」层，没有生态层 |
| **C 并发模型依赖** | 是否假定「goroutine 可无界创建 + GC 兜底」 | Go2Cpp 无 GC，生命周期必须显式（`shared_ptr`/RAII） |
| **P 协议复杂度** | 是否含 TLS / HTTP2 / HTTP3 / 私有二进制协议 | TLS 与 HTTP/2 已声明为边界 |
| **L 规模与许可证** | 代码行数、依赖树大小、许可证兼容性 | 决定「移植」还是「自研一个子集」 |

评级：**A 可直接移植**（R/S/C 低）→ **B 需改写适配**（1 项高）→ **C 延后**（2 项高）→ **D 不做**（≥3 项高）。

---

## 2. 已核验许可证的关键库（一手证据）

| 库 | 仓库 | 许可证（LICENSE 原文核验） | 类别 |
|---|---|---|---|
| `gin-gonic/gin` | https://github.com/gin-gonic/gin | **MIT**（Copyright (c) 2014-present Manuel Martínez-Almeida） | Web 框架 |
| `spf13/cobra` | https://github.com/spf13/cobra | **Apache-2.0** | CLI |
| `go-gorm/gorm` | https://github.com/go-gorm/gorm | **MIT**（Copyright (c) 2013-present Jinzhu） | ORM |

（核验方式：`curl -s https://raw.githubusercontent.com/<owner>/<repo>/<branch>/LICENSE`，2026-10-15 执行。）

---

## 3. 分层候选与取舍

### T1｜工具类（无 Web 依赖，**最先做**）

| 库 | 类别 | R | S | C | P | 许可证 | 结论 |
|---|---|---|---|---|---|---|---|
| `google/uuid` | UUID 生成/解析 | 低 | 低（`crypto/rand`+`encoding/hex`） | 低 | 无 | BSD-3（公开已知，待核验） | **A 可直接移植**：纯算法，约 500 行 |
| `shopspring/decimal` | 定点十进制定点算术 | 低 | 低（`math/big` 可选） | 低 | 无 | MIT（公开已知，待核验） | **A**：金融场景刚需；可直接用 C++ 定点/`__int128` 实现 |
| `gopkg.in/yaml.v3` | YAML 解析 | 低 | 中（`reflect` 仅用于解码到结构体） | 低 | 无 | MIT + Apache-2.0（部分文件，公开已知） | **B**：解析部分可移植（可参考 libyaml，MIT）；解码到结构体需 S5 的绑定策略 |
| `spf13/pflag` | GNU 风格参数解析 | 无 | 低 | 低 | 无 | BSD-3（公开已知，待核验） | **A**：纯解析；是 `cobra`/`viper` 的前置 |
| `BurntSushi/toml` 或 `pelletier/go-toml` | TOML 解析 | 低 | 低 | 低 | 无 | MIT（公开已知，待核验） | **A/B**：解析易，映射到结构体同上 |
| `roaring/roaring` 等位图库 | 数据结构 | 无 | 低 | 低 | 无 | Apache-2.0 | **A**（按需） |

### T2｜配置与日志（依赖 T1 + S5）

| 库 | 类别 | R | S | C | P | 许可证 | 结论 |
|---|---|---|---|---|---|---|---|
| `spf13/viper` | 配置中心 | **高**（`mapstructure` 解码） | 高（yaml/json/toml/properties + fsnotify） | 低 | 无 | MIT（公开已知，待核验） | **C 延后**：Go2Cpp **已有 `config` 模块**（INI + 两阶段事务 + 热重载），viper 的价值主要是「多格式 + 合并优先级」；建议**自研薄层**而非移植 |
| `uber-go/zap` | 结构化日志 | 中（`reflect` 用于 `Any` 字段的编码） | 中（`sync/atomic`+`time`） | 中（后台 flush goroutine） | 无 | MIT（公开已知，待核验） | **B**：核心（`Logger`/`Field`/`SugaredLogger`）可移植，`Any` 走显式绑定；Go2Cpp 已有 `log` 四层组合，可作 sink 适配 |
| `rs/zerolog` | 零分配日志 | 低-中 | 低 | 低 | 无 | MIT（公开已知，待核验） | **B**：链式 API 适合薄封装到现有 `log` |
| `sirupsen/logrus` | 结构化日志 | 中 | 低 | 低 | 无 | MIT | **B/C**：已被 zap/zerolog 取代，优先级低 |

### T3｜CLI 与服务（依赖 S6）

| 库 | 类别 | R | S | C | P | 许可证 | 结论 |
|---|---|---|---|---|---|---|---|
| `spf13/cobra` | 命令行框架 | 低（命令树是显式结构） | 低（`os.Args`/`flag`） | 低 | 无 | **Apache-2.0（已核验）** | **A/B**：依赖 `pflag`；Go2Cpp 补 `flag`（S6）后可移植，约 3–5k 行 |
| `urfave/cli` | 命令行框架 | 低 | 低 | 低 | 无 | MIT | **B**：比 cobra 更小，可作为备选 |
| `grpc-go` | RPC | 中 | **极高**（HTTP/2 + protobuf + net） | 中 | **高（HTTP/2）** | Apache-2.0 | **D 不做**：Go2Cpp 无 HTTP/2，且 TLS 已声明边界 |
| `google.golang.org/protobuf` | 序列化 | 高（反射式编解码 + 代码生成） | 中 | 低 | 无 | BSD-3 | **C/D**：需要 protoc 插件与反射替代方案；若要做，先做「代码生成路线」 |

### T4｜Web 与数据（依赖 T2+T3+S6，**最后做**）

| 库 | 类别 | R | S | C | P | 许可证 | 结论 |
|---|---|---|---|---|---|---|---|
| `gin-gonic/gin` | Web 框架 | **高**（`binding`/`validator` 反射） | **高**（`net/http` 完整语义） | 中 | 中（无 HTTP/2 也可跑 HTTP/1.1） | **MIT（已核验）** | **C 延后**：路由树（radix）+ Context 可移植，但 `binding`/`ShouldBindJSON` 依赖反射 → 必须先有 S5 策略 |
| `go-chi/chi` | 轻量路由 | 低-中 | 中（`net/http`） | 低 | 无 | MIT | **B/C**：比 gin 轻，是无反射路由的首选移植对象 |
| `labstack/echo` | Web 框架 | 中 | 中 | 中 | 无 | MIT | **C**：与 gin 同层，择一即可 |
| `gorilla/websocket` | WebSocket | 低 | 中（`net`） | 低 | 无 | BSD-2 | **不需要**：Go2Cpp **已有自研 `net::WebSocket`**（RFC 6455 握手/帧/分片），可对照补齐边界 |
| `go-redis/redis` | Redis 客户端 | 低-中 | 中（`net`+`context`） | 中（连接池 + goroutine） | 中（RESP 协议，无 TLS 时简单） | BSD-2 | **B**：协议层可移植（RESP2/3 是文本协议），连接池需接 Go2Cpp 的 `sync`/`Channel` |
| `IBM/sarama`（Kafka） | 消息队列 | 低 | 中（`net`） | 中 | 中（Kafka 协议） | MIT | **B/C**：协议编解码可移植，但协议面大；按需 |
| `nats-io/nats.go` | 消息队列 | 低 | 中 | 中 | 低（文本协议） | Apache-2.0 | **B**：协议简单，是「消息队列方向」的最佳首个目标 |
| `jmoiron/sqlx` | SQL 扩展 | **高**（`StructScan` 反射） | **高**（`database/sql`） | 中 | 无 | MIT | **D 不做**（当前）：`database/sql` 已声明不做 |
| `gorm.io/gorm` | ORM | **极高**（全反射 + 钩子） | **极高**（`database/sql`+`reflect`） | 中 | 无 | **MIT（已核验）** | **D 不做**（当前）：除非先交付「反射替代 + database/sql 抽象」 |
| `prometheus/client_golang` | 指标 | 中 | 中（`net/http`） | 低 | 无 | Apache-2.0 | **B/C**：指标模型简单；可作为「HTTP 生态」的第一个真实接入点 |

---

## 4. 为什么 Web 框架 / ORM 现在不能移植（根因）

```text
gin.ServeHTTP → net/http（Go2Cpp 只有 http::Parser/Server，没有完整的 http.Handler 生态/中间件/连接管理）
gin.Bind    → encoding/json 的反射解码（Go2Cpp 的 json.hpp 是「解析到 json 树」，不是「解码到结构体」）
gorm        → database/sql（驱动接口+连接池+事务）+ reflect 扫描结构体标签
zap.Any     → reflect 决定字段类型与编码方式
viper.Unmarshal → mapstructure（反射）把 map 映射到结构体
```

四者共同的拦路石只有两个：**反射** 与 **`database/sql`**。
本项目对这两者的既定策略是：

1. **反射 → 代码生成 / 显式绑定**（`JSON_BIND(field, "name")` 之类），把「动态映射」前移到转译器；
2. **`database/sql` → 不做驱动生态**，只提供接口抽象（`go-stdlib-map.md` §4.3）。

因此 T4 的启动条件是 **S5 的绑定策略落地**，而不是「有时间就做」。

---

## 5. 移植工作流（每个候选都必须走完）

1. **选点**：从本文件 §3 取一个评级 A/B 的库，确认其依赖闭包（`go mod graph` 或仓库 `go.mod`）。
2. **核验**：抓取 LICENSE 原文 + 记录 commit/tag 与日期（写进 `docs/dependencies.md`，格式参照 nlohmann 那一节）。
3. **裁剪**：明确「移植子集」——例如 `yaml.v3` 只移植解析器，不移植反射解码。
4. **适配**：把阻塞调用接到 Go2Cpp 的 Hook/IOManager，把等待接到 `ParkingCondition`，
   把生命周期改成 `shared_ptr`/RAII（**不得**保留 Go 的「靠 GC 兜底」假设）。
5. **测试**：按项目规范——大数据量 + 随机输入 + ≥5 真实场景（覆盖全部移植功能点），
   并做**对拍**（同输入在 Go 与 Go2Cpp 下输出一致）。
6. **交付**：示例（demo/full）+ CMake 与 `example/README.md` 登记 + `codex.md` 记录改动与差异点。

---

## 6. 结论

| 结论 | 内容 |
|---|---|
| **最先做** | T1 的 `pflag`（若要做 CLI）与 `uuid`/`decimal`（纯算法）；但**优先级低于标准库 S1** |
| **自研优于移植** | 配置（`viper`）与日志（`zap`）：Go2Cpp 已有 `config`/`log` 底座，移植收益低于增强现有模块 |
| **不要碰（当前）** | `gorm`/`sqlx`（依赖反射 + `database/sql`）、`grpc-go`（依赖 HTTP/2） |
| **不要重复造** | `gorilla/websocket`：已有自研实现，按 RFC 补边界即可 |
| **最有价值的首个第三方目标** | `nats.go`（文本协议、无反射、Apache-2.0）或 `go-redis`（RESP 协议）—— 二者都能在 S6 之后立刻体现「生态接入」价值 |
| **附带发现（未核实）** | 检索时发现 [ccbrown/gostd](https://github.com/ccbrown/gostd)（自称「用 C++ 重写 Go 包」）。其 raw README 返回 404，**本轮未能核实其范围、许可证与可用性**；若后续确认可用，它可能是 `fmt`/`strings` 等模块的移植来源，应优先评估（符合项目「复用优先」原则） |
