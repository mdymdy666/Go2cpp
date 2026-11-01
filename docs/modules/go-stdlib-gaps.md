# Go 标准库缺口补齐（os/signal、hash/crc32、crypto/rand、flag）

> 模块：`include/go2cpp/go/{os_signal,crc32,crypto_rand,flag}.hpp` + `tests/go_api/**`
> 状态：新增（不修改既有模块；仅扩展 `cmake/Go2CppGoApi.cmake` 的测试清单）
> 构建开关：`-DGO2CPP_WITH_GO_API=ON`（默认 OFF，与既有 Go 接口层一致）
> 日期：2026-11-01
> 关联文档：[go-api-compat.md](go-api-compat.md)（既有 net/sync/chan/context/error/time/io 层）

---

## 0. 结论先行

1. **四个缺口全部落地，header-only**：不需要新增库目标，链接依赖只有
   `go2cpp_hash`（crc32 的查表与硬件路径）与既有的 channel/error 运行时。
2. **crc32 不做二次实现**：`go2cpp/hash/crc32.hpp` 已有 PCLMULQDQ（IEEE）与
   SSE4.2 `crc32q`（Castagnoli）加速，本层只做 `hash/crc32` 的符号映射。
   实测 Release 下 **IEEE 18.4 GB/s / Castagnoli 12.2 GB/s / Koopman 3.2 GB/s**
   （64 × 1 MiB，单核）。
3. **信号用 `sigaction` 而不是 `signalfd`**：`signalfd` 依赖调用线程
   `pthread_sigmask(SIG_BLOCK)`，在多线程进程里任何未屏蔽的线程都会按默认处置
   接收信号，`signalfd` 收不到。理由是硬的，见 §1。
4. **不是「能跑通」而是**与 Go 对拍**：crc32 的 1e6 随机字节、crypto/rand 的
   `Int`（75 个值）与素性判定（400 个候选）都由 **Go 1.27 现场生成 oracle**
   逐值比对；flag 的解析规则按 Go `FlagSet.parseOne` 逐条实现并用 11 个场景覆盖。
5. **已知差异只有两类**：错误处理不退出进程（C++ 库的底线）与少数 API 形态
   （`BigInt` 只做无符号、`Signal` 不带 `siginfo_t`）。逐条列在 §6。

---

## 1. os/signal（`include/go2cpp/go/os_signal.hpp`）

### 1.1 为什么用 sigaction 而不是 signalfd

| 方案 | 问题 |
|---|---|
| `signalfd` | 语义要求调用线程先 `pthread_sigmask(SIG_BLOCK)`。`pthread_sigmask` **只影响当前线程**；进程里还有 fiber 调度、timer、IO 等线程，只要有一个没屏蔽该信号，内核就会把它投递给那个线程并按默认处置（多数是直接杀进程），`signalfd` 永远读不到。要做到全进程屏蔽，必须在**每个**线程创建处插钩子，改动面远超本任务的「只新增不改现有」边界。 |
| `sigaction`（采用） | 处理器是**进程级**的：任何线程收到信号都走同一个 handler。handler 只做 async-signal-safe 的事：读原子掩码 → `write(2)` 一字节到自建 pipe（满则丢弃）→ 无监听者时恢复 `SIG_DFL` 并 `raise(2)`。独立分发线程阻塞读 pipe，把信号转成 `Signal` 后对每个注册通道做**非阻塞** `TrySend`。 |

这个结构对照的是 Go runtime 的 `signal_recv`/`sigsend`：信号先入内部队列，
再由 signal goroutine 分发到 channel；channel 满时丢弃，绝不阻塞信号路径。

### 1.2 API

| Go | 本文件 | 备注 |
|---|---|---|
| `signal.Notify(c, sigs...)` | `signal::Notify(c, {sigs...})` / `Notify(c, sig)` | 空列表 = 全部可捕获信号（1..63） |
| `signal.Stop(c)` | `signal::Stop(c)` | 移除该通道全部注册；信号无人监听时恢复默认处置 |
| `signal.Ignore(sigs...)` | `signal::Ignore({sigs...})` | 信号被吞掉，不投递 |
| `signal.Reset(sigs...)`（Go 1.18） | `signal::Reset({sigs...})` | 移除监听者 + 清除 Ignore + `SIG_DFL` |
| `os.Interrupt` / `os.Kill` | `go::Interrupt` / `go::Kill` | 与 Go 相同的两个导出信号 |
| `syscall.Signal(n)` / `sig.String()` | `go::Signal(n)` / `String()` | 名字表与 Go `syscall` 一致（`"interrupt"`、`"terminated"` …） |
| `signal.NotifyContext(ctx, sigs...)` | **未提供** | 需要 context 依赖；JinShan 的用法是 `Notify` + `select`，本轮不做 |

### 1.3 语义边界

- **通道满即丢**：与 Go 一致（`Notify` 不做阻塞发送）。惯例给通道容量 1。
- **向已关闭通道发送**：Go panic；本实现静默丢弃（`TrySend` 失败）。**行为差异**。
- **多次 Notify 是并集**：与 Go 1.16+ 一致；换集合必须先 `Stop`（或单信号 `Reset`）。
- **`Notify` 覆盖同信号的 `Ignore`**：与 Go 的「后来者生效」一致。
- **`SA_RESTART`**：被这些信号打断的 `read`/`write` 自动重启（Go runtime 自己
  处理 `EINTR`，对用户代码等价）。
- **信号编号 1..63**：64 位原子掩码的取舍；64 及以上实时信号不注册。
- **`SIGKILL`/`SIGSTOP`**：不可捕获，`Notify` 对它们 no-op（Go 同样）。
- **Windows**：`Supported() == false`，接口保留、实现为空（`std::signal` 无法表达
  「多通道 + 默认处置恢复」）。
- **进程级接管**：`Notify` 会覆盖宿主已有的 `sigaction`（Go 亦然）。

### 1.4 测试（`tests/go_api/test_go_signal.cpp`，8 场景）

| 场景 | 验证点 | 结果 |
|---|---|---|
| SIGINT 端到端 | fork 子进程 `kill(2)` 父进程 → 通道收到 `Interrupt`、`Number()==SIGINT`、`String()=="interrupt"` | PASS |
| Stop | `Stop` 后旧通道无消息；重新注册后新通道收到 | PASS |
| Ignore | `Ignore(SIGUSR1)` 后子进程发信号，进程存活（默认处置本应终止） | PASS |
| 多通道 | 两个通道监听同一信号，**都**收到 | PASS |
| String 表 | HUP/INT/QUIT/KILL/SEGV/PIPE/ALRM/TERM/CHLD + 未知 → `"signal N"` | PASS |
| Stop 恢复默认 | `Notify → Stop` 后 fork 的子进程 `raise(SIGTERM)`，父进程 `waitpid` 看到 **WIFSIGNALED** | PASS |
| 无缓冲通道 | 连续 100 次 `raise(SIGUSR2)` 不阻塞、不崩溃 | PASS |
| 多次 Notify | SIGINT 与 SIGUSR1 并集，两者都能收到 | PASS |

---

## 2. hash/crc32（`include/go2cpp/go/crc32.hpp`）

### 2.1 定位

**零新增算法代码**。`include/go2cpp/hash/crc32.hpp` 已实现：

- 三条路径：IEEE 的 PCLMULQDQ 折叠 + Barrett 归约、Castagnoli 的 SSE4.2
  `crc32q`、所有平台的 slicing-by-8 回退；
- 与 Go 一致的「进入取反 / 离开取反」语义（空输入 0、`Update` 可拼接）；
- `Table`（256 基础表 + 7 张派生表）、`MakeTable`（`constexpr`）、`Crc32`。

本层只做**符号映射**，让 Go 代码可以机械替换。

### 2.2 API

| Go | 本文件 |
|---|---|
| `crc32.IEEE` / `Castagnoli` / `Koopman` | 同名常量 |
| `crc32.IEEETable` / `CastagnoliTable` / `KoopmanTable` | 同名 `const Table&` |
| `crc32.MakeTable(poly)` | `MakeTable(poly)` |
| `crc32.ChecksumIEEE(p)` | `ChecksumIEEE(p)`（`ByteSpan` / `std::string` / `vector` / 字面量） |
| `crc32.Checksum(p, tab)` | `Checksum(p, tab)` |
| `crc32.Update(crc, tab, p)` | `Update(crc, tab, p)` |
| `crc32.New(tab)` | `New(tab)` → `Hash`（`hash::Crc32` 的别名） |

### 2.3 测试与数据（`tests/go_api/test_go_crc32.cpp`，7 场景）

| 场景 | 内容 | 结果 |
|---|---|---|
| 标准向量 | `"123456789"` → IEEE `0xCBF43926` / Castagnoli `0xE3069283` / Koopman `0x2D3DD0AE`；空输入 0 | PASS |
| **Go oracle 1e6** | 10 × 100000 字节，xorshift64\* 种子 `0x9E3779B97F4A7C15`，与 Go 1.27 `hash/crc32` 的 10 组三种多项式 + 总和逐项一致 | PASS |
| 逐位参考 | 同一 1e6 字节与不查表的逐位实现交叉验证 | PASS |
| Update 拼接 | 2000 次随机切分（1..97 字节），`Update` 拼接 == 一次性 `Checksum` | PASS |
| 流式 Hash | 随机分片 `Write` / `Sum`（大端 4 字节）/ `Reset` / `Size` / `BlockSize` | PASS |
| 边界长度 | 0/1/2/3/7/8/15/16/17/31/32/33/63/64/65/255/256/257/1000 + 基础表首/末项 | PASS |
| 吞吐 | 64 × 1 MiB：**IEEE 18.38 GB/s、Castagnoli 12.18 GB/s、Koopman 3.20 GB/s** | 参考值 |

> oracle 由 `tests/go_api/scripts/gen_crc32_oracle.go` 生成（Go 1.27.1），
> 文件在 `tests/go_api/testdata/crc32_oracle.txt`，可随时重新生成复核。

---

## 3. crypto/rand（`include/go2cpp/go/crypto_rand.hpp`）

### 3.1 熵源

Linux 优先 `getrandom(2)`（内核 CSPRNG，无 fd、不受 chroot/ulimit 影响），
失败（`ENOSYS` 等）回退 `/dev/urandom`（fd 只开一次）。Windows 用
`std::random_device`（MSVC 基于 `BCryptGenRandom`）。

### 3.2 API

| Go | 本文件 | 备注 |
|---|---|---|
| `rand.Reader` | `crypto::rand::Reader()` → `Source&` | 进程级 CSPRNG |
| `rand.Read(b)` | `Read(buf, size)` → `std::size_t` | 返回实际字节数（正常 == size） |
| `rand.Int(r, max)` | `Int(source, max)` → `BigResult` | `[0, max)`，拒绝采样 |
| `rand.Prime(r, bits)` | `Prime(source, bits)` → `BigResult` | 最高两位置 1 |
| `*big.Int` | `BigInt`（无符号） | 只实现 crypto/rand 需要的子集 |

`Source` 是抽象基类，测试可以注入确定性 reader（与 Go 的 `io.Reader` 对应）。

### 3.3 BigInt 的范围

32 位 limb 无符号大整数：`FromUint64/FromBytes/FromHex`、`BitLen/Bit`、`Bytes`、
`String/Text(base)/BitString`、`Cmp`、`Add/Sub/Mul/Shl/Shr/Mod`、
`ProbablyPrime(reps)`（小素数试除 + Miller-Rabin + Montgomery 模幂）。

**刻意不做**：有符号语义、完整 `Div`（只暴露 `Mod`）、Lucas 测试、GMP 级吞吐。
定位是「测试与轻量密钥生成」：512 位素数约 5~20 ms，2048 位约 1 s 量级。

### 3.4 与 Go 1.27 的对齐细节（踩过的坑）

1. **`Int` 用 `max-1` 的位长**（Go 1.27 的实现），不是 `max` 的位长：`max = 2^32`
   时只读 4 字节而不是 5 字节；`max == 1` 时直接返回 0 且**不消耗**随机源。
   第一版按旧写法实现，对拍时 32 位以上的 max 全部错位，已修正。
2. **`Prime` 无法逐值对拍**：Go 1.26 起 `crypto/rand.Prime` 经
   `crypto/internal/rand.CustomReader`，默认**忽略**传入 reader 改用全局 DRBG；
   即使 `GODEBUG=cryptocustomrand=1`，`randutil.MaybeReadByte` 也会以 50% 概率多读
   一个字节。因此 `Prime` 的验证改为位长/最高两位/奇偶/素性/边界。
   **本实现始终使用传入的 `Source`**——保留可测试性，语义与 Go 1.25 及更早一致。
3. **`ProbablyPrime` 的底数**：前 60 个素数；对 `< 2^64` 是确定性判定，更大范围
   误判率上界 `4^-reps`。Go 用 Baillie-PSW，两者在 400 个候选（2..256 位）上结论一致。

### 3.5 测试与数据（`tests/go_api/test_go_crypto_rand.cpp`，9 场景）

| 场景 | 内容 | 结果 |
|---|---|---|
| Read 随机性 | 1e6 字节 → 125000 个 8 字节块**零重复**；256 桶卡方 `χ²(255)=297.3`（p>0.001） | PASS |
| Read 边界 | 0/1/4096 字节；逐字节 Source 循环补齐；EOF Source 短读返回 0，`Int`/`Prime` 报错 | PASS |
| **Int 逐值对拍** | 15 个 max × 5 个样本 = **75 个值**与 Go 1.27 完全一致（共享 PRNG 流重放） | PASS |
| Int 均匀性 | 1e6 次 `Int(1000)`，`χ²(999)=1050.2`；max=1 恒 0；max=0 报错 | PASS |
| Prime 性质 | 2/3/4/5/8/16/31/32/33/64/65/127/128/192/256 位：位长、最高两位、奇数、素性 | PASS |
| Prime 边界 | 连续 3 个 64 位素数互不相同；2 位只能是 3；5 位范围 | PASS |
| BigInt 代数 | 500 轮随机：加减互逆、交换律、分配律、移位、取模、字节/十六进制/十进制往返 | PASS |
| **素性对拍** | 400 个候选（2..256 位，60 个素数）与 Go `ProbablyPrime(20)` 结论一致 | PASS |

> oracle：`tests/go_api/scripts/gen_rand_oracle.go` → `testdata/rand_oracle.txt`。

---

## 4. flag（`include/go2cpp/go/flag.hpp`）

### 4.1 API

| Go | 本文件 |
|---|---|
| `flag.String/Int/Bool/Int64/Float64/Duration` | 同名自由函数（返回 `shared_ptr<T>`，用法同 `*T`） |
| `flag.Parse()` | `flag::Parse(argc, argv)` → `ParseResult` |
| `flag.Args/NArg/Arg/NFlag` | 同名 |
| `flag.Visit/VisitAll/Lookup/Set` | 同名 |
| `flag.PrintDefaults` | 同名（`SetOutput` 可重定向） |
| `flag.Usage`（包级变量） | `flag::Usage`（inline 变量，可直接赋值） |
| `flag.CommandLine` | `CommandLine()` → `FlagSet&` |
| `flag.NewFlagSet(name, mode)` | `FlagSet(name)`（无 `ErrorHandling`，不退出进程） |

### 4.2 解析规则（逐条对齐 `FlagSet.parseOne`）

| 输入 | 结果 |
|---|---|
| `-x=1` / `--x=1` | 值 `"1"` |
| `-x 1` / `--x 1` | 值 `"1"`（消费下一个参数） |
| `-b`（bool） | `true`，不消费下一个参数 |
| `-b=false` / `-b=0` / `-b=1` | 按 `strconv.ParseBool` 的全部字面量 |
| `-b false` | `b=true`，`false` 留在 `Args()` |
| `--` | 终止解析，自身丢弃 |
| 第一个非 flag 参数 | 终止解析，剩余全部进 `Args()` |
| `-h` / `-help` | 调用 Usage，返回 `kHelp`（不是错误） |
| 未定义 | `flag provided but not defined: -x` |
| 缺参数 | `flag needs an argument: -x` |
| 值非法 | `invalid value "abc" for flag -x` |
| 语法非法 | `bad flag syntax: ---bad` |

### 4.3 Duration 文法

完整实现 Go `time.ParseDuration` 的 `[-+]?([0-9]*(\.[0-9]*)?[a-z]+)+`，
单位 `ns`/`us`/`µs`/`μs`/`ms`/`s`/`m`/`h`。小数部分的计算顺序照搬 Go 的
`uint64(float64(f) * (float64(unit) / scale))`——第一版写成
`f / (scale * unit)`，`1.5s` 会静默变成 `1s`，测试直接抓到。

> `go2cpp/tools/time.hpp` 的待办表里 `ParseDuration` **尚未实现**；本文件内联了
> 一份，未来 tools 层补齐后应改为转发（届时删掉 `detail::ParseDurationText`）。

### 4.4 测试（`tests/go_api/test_go_flag.cpp`，10 场景）

注册/默认值/类型名、四种赋值形式、bool 三种形态、Duration 11 个合法 + 8 个非法、
5 条错误路径、`--` 与位置参数、NFlag/Visit/VisitAll/Lookup/Set、`-h` 自定义与内置
Usage、PrintDefaults 输出、全局 CommandLine —— **全部 PASS**。

---

## 5. 构建与运行

```bash
# 配置（主构建目录）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_GO_API=ON
cmake --build .build-bento-wsl --target \
    go_api_crc32 go_api_crypto_rand go_api_flag go_api_signal -j 8

# 运行
ctest -R 'go_api_(crc32|crypto_rand|flag|signal)' --output-on-failure
```

- 四个测试都带 `-Wall -Wextra -Wpedantic -Werror`（与既有 Go API 测试一致）。
- `go_api_signal` 只在 `UNIX` 上注册（依赖 `fork`/`sigaction`/`waitpid`）。
- crc32 测试链接 `go2cpp_hash`；standalone 构建（无该目标）时直接编译
  `src/hash/crc32.cpp` + `src/hash/hash.cpp` 进测试目标。
- oracle 路径通过编译宏 `GO_API_CRC32_ORACLE` / `GO_API_RAND_ORACLE` 传入，
  由 `tests/go_api/scripts/gen_*.go` 生成（`GO111MODULE=off go run ...`）。

---

## 6. 与 Go 的差异汇总（完整）

| # | 模块 | 差异 | 影响 |
|---|---|---|---|
| 1 | signal | 向已关闭通道发送：Go panic，本实现丢弃 | 低（避免库内 panic） |
| 2 | signal | 未提供 `NotifyContext` | 中（可自行 `Notify` + `Context` 组合） |
| 3 | signal | 信号编号上限 63（64 位掩码） | 低 |
| 4 | signal | `SA_RESTART` 语义 | 低（对用户代码等价） |
| 5 | signal | Windows 不支持 | 中（JinShan 目标是 Linux） |
| 6 | crc32 | `Table` 内含 8 张表（对外仍只暴露基础 256 项） | 无（`operator[]` 语义不变） |
| 7 | crc32 | `Write` 返回 `void` 而非 `(n, error)` | 无（不会失败） |
| 8 | rand | `Int`/`Prime` 返回 `BigResult{value, err}` 而非 `(v, error)` | 低 |
| 9 | rand | `BigInt` 无符号、无完整 `Div` | 低（crypto/rand 用不到） |
| 10 | rand | `Prime` 始终使用传入 Source（Go 1.26+ 忽略它） | 无（更可测） |
| 11 | rand | 素性用 20 轮 MR（Go 用 Baillie-PSW） | 极低（400 候选结论一致） |
| 12 | flag | 解析失败**不退出进程**、不自动打印用法（`-h` 除外） | 关键差异（见 §4.2 示例） |
| 13 | flag | 错误文本不含底层 `strconv` 原因 | 低 |
| 14 | flag | 重复注册抛 `std::invalid_argument`（Go panic） | 低 |

---

## 7. 后续可做

1. `signal::NotifyContext`（与 `go2cpp/go/context.go.hpp` 对接）；
2. `tools/time.hpp` 补齐 `ParseDuration` 后，`flag` 改为转发、删除内联实现；
3. `crypto/subtle.ConstantTimeCompare`（JinShan protocol §7.2 用到，不在本轮四个缺口内）；
4. `crypto/rand.Text()`（Go 1.24 的 base32 便捷函数）。
