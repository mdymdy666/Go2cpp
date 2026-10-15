# Go 标准库 → Go2Cpp 逐包映射与缺口计划

> 目标读者：决定「下一步做什么库」的人。
> 本文件的所有频率数据都**可复现**（命令见 §1），不依赖任何外部排行站的结论。

---

## 1. 方法与依据（先证据，后结论）

### 1.1 频率指标：标准库内部「被多少个包 import」

Go2Cpp 的参考源码归档里有官方 Go 1.23.0 完整源码（`third_party/go1.23.0.src.tar.gz`，28,163,301 字节，
SHA-256 记录在 `docs/dependencies.md`），解压后 `src/` 共 **638 个含 .go 的包目录**。
用下面两条命令即可复现本文件的排行（不依赖网络）：

```sh
mkdir -p /tmp/go-src && tar -xzf third_party/go1.23.0.src.tar.gz -C /tmp/go-src
# 统计：每个标准库包被多少个「其他包」import（排除 testdata；不统计 _test.go）
python3 /tmp/stdlib_stats.py        # 脚本逻辑见本文件 §1.2
```

### 1.2 统计口径（避免误读）

- 只统计**非测试** `.go` 文件的 `import`（单行与 import 块都算）；
- 排除 `internal/*`、`cmd/*`、含域名前缀的路径（非标准库）；
- `importer` 去重到**包**级（同一个包 import 多次只算一次）；
- 因此「337」的含义是：**标准库里有 337 个不同的包 import 了 `fmt`**。

### 1.3 交叉验证与可信度标注

| 来源 | 可信度 | 用途 |
|---|---|---|
| 本地 Go 1.23.0 源码扫描（§1.1） | **一手证据**（可复现） | 排行、包清单、代码行数 |
| [go101 标准库索引](https://docs.go101.org/std/index.html) | 第三方专业站（其 imported-by 与本地口径略有出入，量级一致） | 交叉验证 + 包功能一句话描述 |
| [Go 官方 2025 开发者调查](https://golang.google.cn/blog/survey2025) | 官方 | 生态趋势（不提供逐包频率） |
| 社区排行文章（如 datasea.cn 系列） | **低**（SEO 站点，未给数据采集口径） | 仅作旁证，不参与决策 |

> ⚠️ 诚实声明：「标准库内部 import 频率」**不等于**「用户代码使用频率」。
> 用户侧的高频包（`fmt`/`strings`/`os`/`time`/`encoding/json`/`net/http`）与本表高度重合，
> 但 `go/ast`、`go/types`、`math/big` 这类包在本表偏高，是因为标准库自身的编译器/工具链在用它。
> 本文件在优先级判定时按「**转译真实业务代码所需**」而非单纯频率排序（见 §4 的判据）。

---

## 2. 实测排行（Top 40，本地复现）

| # | 包 | 被 import 的包数 | 文件数 | 行数 | # | 包 | 被 import 的包数 | 文件数 | 行数 |
|---:|---|---:|---:|---:|---:|---|---:|---:|---:|
| 1 | `fmt` | 337 | 5 | 3521 | 21 | `flag` | 62 | 1 | 1231 |
| 2 | `strings` | 268 | 7 | 2251 | 22 | `reflect` | 59 | 13 | 8578 |
| 3 | `io` | 238 | 3 | 1065 | 23 | `context` | 57 | 1 | 792 |
| 4 | `os` | 213 | 100 | 9047 | 24 | `math/bits` | 52 | 6 | 918 |
| 5 | `errors` | 206 | 3 | 296 | 25 | `io/fs` | 51 | 8 | 916 |
| 6 | `bytes` | 183 | 3 | 2025 | 26 | `regexp` | 50 | 4 | 2718 |
| 7 | `strconv` | 163 | 16 | 5445 | 27 | `os/exec` | 49 | 9 | 1870 |
| 8 | `sync` | 149 | 12 | 2199 | 28 | `syscall` | 49 | 273 | 165466 |
| 9 | `runtime` | 113 | 428 | 102146 | 29 | `sync/atomic` | 46 | 3 | 678 |
| 10 | `unsafe` | 109 | 1 | 271 | 30 | `encoding/json` | 39 | 8 | 4196 |
| 11 | `log` | 103 | 1 | 458 | 31 | `net/http` | 24 | 32 | 28317 |
| 12 | `path/filepath` | 103 | 9 | 1351 | 32 | `net` | 23 | 141 | 18783 |
| 13 | `sort` | 103 | 8 | 2206 | 33 | `net/url` | 23 | 1 | 1309 |
| 14 | `time` | 98 | 21 | 6358 | 34 | `math/big` | 20 | 24 | 9584 |
| 15 | `unicode/utf8` | 87 | 1 | 583 | 35 | `cmp` | 19 | 1 | 77 |
| 16 | `bufio` | 77 | 2 | 1263 | 36 | `math/rand` | 19 | 6 | 1343 |
| 17 | `encoding/binary` | 76 | 4 | 1210 | 37 | `testing` | 19 | 12 | 4869 |
| 18 | `math` | 76 | 62 | 6314 | 38 | `crypto/sha256` | 17 | 7 | 465 |
| 19 | `go/token` | 67 | 3 | 1026 | 39 | `encoding/base64` | 16 | 1 | 661 |
| 20 | `slices` | 67 | 5 | 1774 | 40 | `crypto/subtle` | 16 | 6 | 177 |

完整 Top 60 与统计脚本见 `notes/stdlib-import-stats.md`（本轮同时产出）。

---

## 3. 逐域映射：Go 标准库 → Go2Cpp 现状

图例：**已有**＝能直接用；**部分**＝有底层能力但没有 Go 风格 API；**缺**＝需要新建模块。

### 3.1 基础工具域（转译代码出现率最高）

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `fmt` | 337 | 无（示例用 `std::cout`/`GO2CPP_LOG_*`） | **缺** | `Sprintf`/`Sprintln`/`Printf`/`Errorf`（含 `%v`/`%w`） |
| `strings` | 268 | 无（直接用 `std::string`） | **缺** | `Contains`/`Split`/`Join`/`Trim*`/`HasPrefix`/`Replace*`/`Builder` |
| `strconv` | 163 | 无 | **缺** | `Atoi`/`Itoa`/`ParseInt`/`ParseFloat`/`FormatFloat`/`Quote` |
| `bytes` | 183 | `net::ByteArray`（协议用） | **部分** | `Buffer`（读写游标+`String()`）、`Contains`/`Split`/`Fields`、`Equal` |
| `sort` | 103 | 无（可用 `std::sort`） | **缺** | `Slice`/`SliceStable`/`Search`/`Strings`/`Ints` |
| `slices` | 67 | 无（Go 1.21+ 泛型） | **缺** | `Contains`/`Index`/`Sort`/`SortFunc`/`Clone`/`Delete` |
| `maps` | 22 | 无 | **缺** | `Keys`/`Values`/`Clone`/`Equal` |
| `unicode/utf8` | 87 | 无 | **缺** | `RuneCount`/`DecodeRune`/`Valid`/`RuneLen` |
| `unicode` | 64 | 无 | **缺** | `IsLetter`/`IsDigit`/`ToUpper`/`ToLower`（可薄封装 `<cctype>`+UTF-8） |
| `cmp` | 19 | 无 | **缺** | `Compare`/`Or`/`Less`（三行模板） |
| `math` | 76 | 无 | **缺** | `Sqrt`/`Pow`/`Abs`/`Floor`/`Ceil`/`Max`/`Min`/常量 |
| `math/bits` | 52 | 无 | **缺** | `Len`/`OnesCount`/`RotateLeft`（直接映射 `std::popcount` 等） |
| `math/big` | 20 | 无 | **缺（大工程）** | `Int`/`Float`/`Rat`；建议**不实现**或用 GMP 移植（见 §4 P3） |
| `regexp` | 50 | 无 | **缺（中工程）** | RE2 语义；可移植 [google/re2](https://github.com/google/re2)（BSD-3） |
| `reflect` | 59 | 无 | **明确不实现** | Go 的反射依赖运行时类型信息；Go2Cpp 用「代码生成/模板」替代 |
| `unsafe` | 109 | 无需对应 | — | C++ 侧直接用指针/reinterpret_cast |
| `runtime` | 113 | `runtime.hpp` 聚合头（非 Go 的 runtime 包） | **部分** | `Gosched`/`NumGoroutine`/`GC`（后者不实现） |

### 3.2 错误与上下文域

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `errors` | 206 | `tools/error.hpp`：`NewError`/`Wrap`/`Join`/`Is`/`As`/`WalkErrors` | **已有** | `fmt.Errorf("%w")` 的等价入口（与 `fmt` 模块一起做） |
| `context` | 57 | `concurrency/context.hpp`：Background/WithCancel/WithTimeout/WithDeadline/WithValue + `ContextRollback` | **已有** | `context.AfterFunc`；`Err()` 目前只有两个单例 |
| `log` | 103 | `logging/log.hpp`：四层组合 + 单字符格式项 | **已有** | `log.SetFlags`/`Output` 语义；`slog` 结构化（见 §4 P2） |

### 3.3 IO 与文件域

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `io` | 238 | 部分：`net::Socket`/`SocketStream`/`ByteArray` | **部分** | `io::Reader/Writer/Closer` 接口 + `Copy`/`CopyN`/`ReadAll`/`LimitReader`/`TeeReader` |
| `os` | 213 | 部分：`FdManager`（fd 元数据）；Hook 已拦截 syscall | **部分** | `os::File`（`Open/Create/Read/Write/Seek/Stat`）、`ReadFile`/`WriteFile`、`Getenv`/`Setenv`、`Args`、`Exit` |
| `bufio` | 77 | 无（http 自己带缓冲） | **缺** | `Reader`（`ReadString`/`ReadBytes`/`Peek`）、`Writer`（`Flush`）、`Scanner` |
| `io/fs` | 51 | 无 | **缺** | `FS`/`File` 接口抽象（虚拟文件系统） |
| `path/filepath` | 103 | 无 | **缺** | `Join`/`Dir`/`Base`/`Ext`/`Clean`/`Abs`/`Walk` |
| `path` | 42 | 无 | **缺** | 与 filepath 共用（URL 路径版） |
| `os/exec` | 49 | 无 | **缺（中工程）** | `Command`/`Run`/`Output`（`fork`+`exec`+管道） |
| `embed` | 24 | 无 | **缺（小工程）** | 编译期嵌入文件（代码生成即可） |
| `testing` | 19 | 项目自研 `tests/test_support.hpp`（`check`/`require`/watchdog） | **部分** | 转译 Go 测试需要 `testing.T` 风格（`t.Fatalf`/`t.Run`/表驱动） |

### 3.4 时间与并发域

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `time` | 98 | `core/timer.hpp`：`TimerService`/`Timer`/`TimerOptions` | **部分** | `time::Duration`/`Time`/`Now`/`Since`/`Sleep`（Go2Cpp 里 `Sleep` 由 Hook 协作式接管）、`Ticker`、`Format`/`Parse` |
| `sync` | 149 | `sync.hpp`：`Mutex`/`ConditionVariable`/`WaitGroup` | **部分** | `Once`、`Pool`、`Map`（RWMutex 语义可用 `Mutex`，但转译需要同名 API） |
| `sync/atomic` | 46 | 直接用 `std::atomic` | **部分** | Go 风格 `atomic::Int64`/`Add`/`Load`/`Store`/`CAS` 薄封装 |
| `runtime`（goroutine 相关） | — | `scheduler.hpp`：`go`/`Task`/`WaitGroup` | **已有** | `GOMAXPROCS()` 运行期 setter（当前配置须启动前定） |
| `os/signal` | 5 | 无 | **缺** | `Notify`/`Signal`（可用 `sigaction` + channel） |

### 3.5 编码与序列化域

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `encoding/json` | 39 | `tools/json.hpp`（nlohmann 薄封装：`ParseJson`/`JsonScanDepth`） | **部分** | `Marshal`/`Unmarshal` 到结构体（需要 struct tag 支持 → 代码生成或手写映射）、`Encoder`/`Decoder` 流式 |
| `encoding/binary` | 76 | `net::ByteArray`（定长读写）+ `endian`（字节序） | **部分** | `binary::Read`/`Write`（小端/大端结构体）、`Varint`/`Uvarint` |
| `encoding/base64` | 16 | 无 | **缺** | `EncodeToString`/`DecodeString`（3 行映射到自研实现） |
| `encoding/hex` | 3 | `ByteArray::to_hex_string` | **部分** | `Encode`/`Decode`/`Dump` |
| `encoding/csv` | 0 | 无 | **缺（低优先）** | `Reader`/`Writer` |
| `encoding/xml` | 0 | 无 | **明确不实现** | 与 JSON 同理但需求低 |
| `encoding/gob` | 1 | 无 | **明确不实现** | Go 专有格式 |
| `text/template` | 11 | 无 | **缺（中工程）** | `Template.Execute`（大量 Web 场景需要） |
| `html/template` | 2 | 无 | **缺（依赖 text/template）** | 自动转义 |

### 3.6 网络域

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `net` | 23 | `net::Socket`/`DialTCP`/`ListenTCP`/`Address` | **部分** | `net::Conn`/`Listener` 接口抽象、`net::IP`/`TCPAddr` 别名、`LookupHost` DNS |
| `net/http` | 24 | `http.hpp`：`Parser`/`Client`/`Server`/`ResponseWriter` | **部分** | `http::Handler`/`ServeMux`/`HandleFunc`、中间件链、`Client` 连接池与 keep-alive、重定向跟随、Cookie 罐 |
| `net/url` | 23 | `http.hpp` 内的 query 编解码 | **部分** | `url::Parse`/`URL`/`Values` 独立成模块 |
| `net/http/httptest` | — | 无 | **缺** | 测试用假服务器（对转译测试很重要） |
| `crypto/tls` | 9 | 无 | **明确不实现（近期）** | TLS 需移植 OpenSSL/BoringSSL；边界已在 `docs/compatibility.md` 声明 |
| `crypto/*`（sha256/hmac/rand…） | 17..9 | 无 | **缺（可选）** | `sha256`/`hmac`/`rand` 可用成熟 C 实现移植（MIT/BSD） |

### 3.7 其它

| Go 包 | 频次 | Go2Cpp 落点 | 状态 | 缺口 |
|---|---:|---|---|---|
| `flag` | 62 | 部分：`config` 模块（INI） | **部分** | `flag::String`/`Int`/`Parse`（命令行） |
| `math/rand` | 19 | 无 | **缺** | `rand::Intn`/`Float64`/`Seed`（Go 1.20+ 全局源自动随机） |
| `container/heap`/`list`/`ring` | 3/3/0 | 无 | **缺（小）** | 泛型容器（`list`/`ring` 各约 100 行） |
| `hash/crc32`/`fnv`/`adler32` | 3/3/1 | 无 | **缺（小）** | 校验和工具 |
| `database/sql` | 0 | 无 | **明确不做** | 需要驱动生态（Go2Cpp 定位是运行时，不是驱动） |
| `debug/*`（elf/dwarf/pe） | 23..1 | 无 | **明确不做** | 工具链用途 |
| `go/*`（ast/parser/token/types） | 67..44 | 无 | **明确不做** | 属编译器/工具链，与运行时无关 |
| `plugin`/`embed`/`arena` | 0 | 无 | **边界** | 平台/实验特性 |

---

## 4. 缺口优先级与计划产出时间

### 4.1 优先级判据（三条，按权重）

1. **转译阻断度**（权重最高）：不补就没法把常见 Go 业务代码翻译过来 —— 例如 `fmt`/`strings`/`strconv`/`errors`（几乎每个 Go 文件都用）。
2. **已有底座的复用度**：能薄封装到现有模块或 C++ 标准库的，优先做（成本低、风险小）。
3. **依赖链长度**：被其它待做模块依赖的先做（例如 `io` 被 `bufio`/`http`/`os` 依赖；`fmt` 被 `errors` 的 `%w` 依赖）。

### 4.2 计划（按阶段，标注计划产出时间）

| 阶段 | 计划产出时间 | 模块（新建文件） | 预计工时 | 产出物 |
|---|---|---|---|---|
| **S1（P0）** | 第 1 周 | `tools/fmt.hpp`、`tools/strings.hpp`、`tools/strconv.hpp`、`tools/bytes.hpp`、`tools/sort.hpp` | 5×0.5–1 天 | 5 个头文件 + 5 个 `tools/*` 示例（demo/full）+ 登记 CMake |
| **S2（P0）** | 第 1–2 周 | `io.hpp`（Reader/Writer 接口 + Copy/ReadAll）、`os.hpp`（File/Getenv/Args）、`time.hpp`（Duration/Now/Sleep/Ticker） | 3×1–1.5 天 | 3 个模块 + 与 Hook 的协作式 Sleep 打通 |
| **S3（P1）** | 第 2–3 周 | `bufio.hpp`、`path/filepath.hpp`、`slices.hpp`、`maps.hpp`、`cmp.hpp`、`math.hpp`、`math/bits`、`encoding/binary` 补全 | 8×0.5–1 天 | 8 个薄封装模块 |
| **S4（P1）** | 第 3–4 周 | `sync` 补 `Once`/`Pool`/`Map`；`sync/atomic` 薄封装；`math/rand`；`encoding/base64`/`hex` | 6×0.5–1 天 | 现有模块扩展 + 新小模块 |
| **S5（P1）** | 第 4–6 周 | `encoding/json` 的 `Marshal`/`Unmarshal`（结构体映射，**需先定策略**：代码生成 vs 手写 tag 注册）、`net` 的 `Conn`/`Listener` 抽象、`net/url` 独立模块 | 3×2–3 天 | 转译可用性最大的一块（JSON 是 Web 业务的核心） |
| **S6（P2）** | 第 6–8 周 | `http` 的 `ServeMux`/`Handler`/中间件、`Client` 连接池、`text/template`、`os/exec`、`flag`、`container/*`、`hash/*` | 7×1–2 天 | 从「有协议解析」到「能写 Web 服务」 |
| **S7（P3/边界）** | 第 9 周起 | `regexp`（移植 RE2）、`crypto/sha256`·`hmac`·`rand`（移植成熟 C 实现）、`testing` 风格、`math/big`（评估 GMP） | 按需 | 逐项写清移植来源与许可证 |

### 4.3 不建议实现（写进文档边界，避免误期待）

| Go 包/能力 | 理由 |
|---|---|
| `reflect` | Go 反射依赖运行时类型元数据；Go2Cpp 用**代码生成 + 模板**替代（与 `encoding/json` 的 Marshal 策略绑定） |
| `crypto/tls` | 需要完整 TLS 栈；移植 OpenSSL/BoringSSL 的成本与审计负担远超本项目定位 |
| `database/sql` | 需要驱动生态；Go2Cpp 可提供接口抽象，但不做驱动 |
| `go/*`、`debug/*` | 编译器/调试工具链用途，与「运行时语义库」定位无关 |
| `encoding/gob`、`encoding/xml` | Go 专有/低需求格式 |
| GC、`runtime.GC` | `shared_ptr` + RAII 的所有权模型（见 `docs/dependencies.md` 的替代表） |

---

## 5. 结论：如果只做 6 件事

按「转译阻断度 × 复用度」排序，**最该先做的六个模块**：

1. **`fmt`**（频次 337，零对应）—— 含 `Sprintf`/`Errorf`，是 `errors` 的 `%w` 与所有日志/字符串拼接的前提。
2. **`strings` + `strconv`**（268 + 163）—— 纯函数，薄封装 C++ 标准库即可，投入产出比最高。
3. **`io`**（238）—— `Reader`/`Writer` 接口是 `bufio`/`os`/`http`/`net` 的共同依赖，必须先立起来。
4. **`os`**（213）—— 文件与环境变量是真实程序的入口；Go2Cpp 已有 `FdManager` 与 Hook 拦截可复用。
5. **`time`**（98）—— Go2Cpp 已有 `TimerService`，补 Go 风格 `Duration`/`Now`/`Sleep`/`Ticker` 成本低。
6. **`encoding/json` 的结构体编解码**（39）—— 已有解析底座，缺的是 Go 侧 `Marshal`/`Unmarshal` 到结构体；**策略需要先决策**（见 §4.2 S5）。

> 每个模块的落地都必须遵守项目规范：**先检索开源实现**（能移植就移植，记录来源/许可证/改动），
> 再按「大数据量 + 随机 + ≥5 真实场景」测试，并配 demo/full 两版示例 + CMake 与 `example/README.md` 登记。
