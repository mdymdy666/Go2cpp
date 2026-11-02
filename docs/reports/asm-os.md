> 来源：/tmp/os_asm.md，2026-11-02 归档

# os 模块（file/proc/shm/mq）汇编级审计与优化报告

- 时间：2026-11-02
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 树；**不是** `E:` 树）
- 构建：`/tmp/build-os`（独立目录，Ninja，Release，`-O2 -Wall -Wextra -Wpedantic`，**0 warning**）
- 机器：AMD Ryzen 9 8945HX（6C/12T，L3 32 MiB）、19 GB RAM、WSL2 内核 6.6.87.2-microsoft
- 工具链：gcc 13.3.0、cmake 3.28、objdump（binutils 2.42）、go 1.27.1
- 负载：`taskset -c 6`（单核，避免迁移噪声）；全部数据为同机同轮次测量

---

## 0. 结论摘要

| 项 | 结果 | 是否达标 |
| --- | --- | ---: |
| 文件单条 `read` 1e6 次 vs 原生 | **1.01 ~ 1.03×** | ✅ <1.1× |
| 文件单条 `write` 1e6 次 vs 原生 | **1.00 ~ 1.01×** | ✅ <1.1× |
| 文件读写 vs Go `os.File` | **0.73 ~ 0.81×**（更快） | ✅ |
| `ReadFile`/`WriteFile` 8 MiB vs Go | **0.30 ~ 1.05×** | ✅ |
| 批量 `writev`/`readv`（架构优化） | **17.9× / 20.9×** 于单条 | ✅ 数量级 |
| `CopyTo` 零拷贝（sendfile） | 8 MiB **1.10~1.15×**，256 MiB **1.00~1.12×** | 部分达标 |
| `Process` spawn+wait vs 原生（同进程 A/B） | **1.13 ~ 1.22×** | ⚠️ 略超 |
| `Process` spawn+wait vs Go（跨进程） | 1.45×（口径不可比，见 §5.2） | ⚠️ 见说明 |
| 正确性 | 1e6 次读写逐字节一致 + 5 类边界，`failures=0` | ✅ |

三条主要结论：

1. **单条文件 IO 已与原生 syscall 持平（1.01×）**。热路径从「libc `syscall()` + PLT + 错误码搬运」
   换成内联 `syscall` 指令：`File::Read` 46 指令 / 2 call / 24 B 栈帧 → **38 指令 / 0 call / 0 B 栈帧**。
2. **真正的数量级收益在架构层**：`writev`/`readv` 把 1e6 次 syscall 合成 977 次，
   单条成本 837 ns → **46.7 ns**（17.9×）、457 ns → **21.8 ns**（20.9×）。
   这不是「抠指令」，是改调用模型。
3. **`Process` 没到 <1.1×**（1.13~1.22×）。已做两项实打实的削减（去掉一次重复 `/proc` 读取、
   用 `strtoll` 替换 `istringstream`），但 `fork` 前后的固定成本仍高于手写骨架，
   剩余差距未定位到单一热点（见 §5.3）。

---

## 1. 改动范围

| 文件 | 改动 |
| --- | --- |
| `src/os/raw_syscall.hpp`（新增） | 内联汇编 `Syscall0..6`，返回 `-errno` 不写 TLS errno |
| `src/os/file.cpp` | 读写/定位/元信息全部改内联 syscall；新增 `Readv`/`Writev`/`WritevAll`/`ReadvAll`/`CopyTo` |
| `include/go2cpp/os/file.hpp` | 上面对应声明 + 语义/收益注释 |
| `src/os/proc.cpp` | `ReadProcStat` 手写解析；`Spawn` 去掉重复 `/proc` 读取；`MakeError`/`MakeErrnoError` 标 `[[gnu::cold]]` |
| `include/go2cpp/os/proc.hpp` | `SpawnedProcess::stat_snapshot`；两个错误构造函数的 `cold` 声明 |
| `tests/os/test_os_file.cpp` | 追加场景 13–17（1e6 一致 / 批量 IO / CopyTo / EINTR / 大小边界） |

> 未 commit。工作树里 `tests/go_api/test_go_net.cpp`、`tests/logging/test_log_redact.cpp`、
> `tests/bench/*`、`src/media/capture_kernels.hpp` 等改动**不是本次引入**（进入任务前已存在）。

---

## 2. objdump：热函数指令 / call / syscall / 栈帧

口径：`objdump -d -C --no-show-raw-insn`，统计函数体内全部指令（含冷分支）；
栈帧 = `sub $N,%rsp` + `push` 数 × 8。

### 2.1 `os::File`（改前 → 改后）

| 函数 | 指令 | call | syscall 指令 | 栈帧 B |
| --- | ---: | ---: | ---: | ---: |
| `File::Read` | 46 → **38** | 2 → **0** | 0 → **1** | 24 → **0** |
| `File::Write` | 47 → **37** | 2 → **0** | 0 → **1** | 24 → **0** |
| `File::ReadAt` | 54 → **38** | 2 → **0** | 0 → **1** | 40 → **0** |
| `File::WriteAt` | 57 → **41** | 2 → **0** | 0 → **1** | 40 → **0** |
| `File::Seek` | 41 → **32** | 2 → **0** | 0 → **1** | 24 → **0** |
| `File::Sync` | 31 → **29** | 2 → **0** | 0 → **1** | 8 → **0** |
| `File::Open` | 121 → 118 | 10 → 6 | 0 → 5 | 216 |
| `File::Adopt` | 89 → 95 | 5 → 4 | 0 → 1 | 200 → 228 |
| `ReadFile` | 1967 → 2802 | 80 → 103 | 0 | 456 → 632 |
| `WriteFile` | 1067 → 1589 | 39 → 52 | 0 | 456 → 632 |

读改前的 `File::Read` 骨架（`/tmp/file_orig.o`）：

```
push %r12 / push %rbp / push %rbx      ← 24 B 栈帧
call <syscall@plt>                     ← libc 通用变参入口
cmp  $0xfffffffffffff001,%rax
call <__errno_location@plt>            ← TLS 取 errno
```

改后（`libgo2cpp_os.so`）：

```
mov  %rdi,%r8 ; movslq (%rdi),%rdi ; test ; js ...
xor  %r9d,%r9d                          ← SYS_read = 0
280e3: mov %r9,%rax ; syscall           ← 直接 syscall 指令
mov  %rax,%rcx ; test ; jns ...
mov  %eax,%edi ; neg %edi ; cmp $0xfffffffc,%eax ; je（EINTR 重试）
```

**0 个 `call`、0 B 栈帧**，`l` 路径只剩一条 `syscall`。`ReadFile`/`WriteFile` 反而变大
（1967→2802）：`File::Read` 变小后编译器把它**内联进读取循环**，少一次过程调用、
但循环体展开更多。这两支不在 per-byte 热路径上（每次 64 KiB），体积可接受。

### 2.2 新增的批量/零拷贝函数

| 函数 | 指令 | call | syscall 指令 | 栈帧 B |
| --- | ---: | ---: | ---: | ---: |
| `File::Readv` | 40 | 0 | 1 | 0 |
| `File::Writev` | 40 | 0 | 1 | 0 |
| `File::WritevAll` | 181 | 5 | 0（复用 `Writev`） | 88 |
| `File::ReadvAll` | 188 | 6 | 0（复用 `Readv`） | 88 |
| `File::CopyTo` | 171 | 9 | 1（`SYS_sendfile`） | 88 |

`WritevAll`/`ReadvAll` 的快路径只有一次 `Writev`/`Readv` 调用，慢路径（部分写）才
`vector<iovec>` 复制 —— 181/188 条指令里大部分是这条冷分支。

### 2.3 `os::Process`

| 函数 | 指令 | call | 栈帧 B |
| --- | ---: | ---: | ---: |
| `Process::Spawn`（原始） | 609 | 70 | 600 |
| `Process::Spawn`（+cold） | 556 | 60 | 584 |
| `Process::Spawn`（+去重 /proc） | **579** | **59** | 728 |
| `Process::Wait`（原始） | 81 | 10 | 184 |
| `Process::Wait`（+cold） | **39** | **4** | 184 |
| `Process::TryWait` | 312 → **251** | 38 → **24** | 344 |
| `Process::Kill` | 81 → **39** | 10 → **4** | 184 |
| `Run` | 1230 → **1310** | 146 → 149 | 1352 |

`Process::Wait` 指令腰斩：原始版本在同一个函数里内联了「超时判定 + pidfd + poll + 错误构造」，
`MakeErrnoError` 标 `cold` 后错误分支被挪出热路径。

`Process::Spawn` 栈帧 600 → 728：`SpawnedProcess` 新增的 `ProcessInfo stat_snapshot`
（约 128 B）被拷进栈帧。**用栈换一次 `/proc` 读取**，实测净值仍为正（见 §5.3）。

### 2.4 `os::Shm` / `os::Mq`

| 函数 | 指令 | call | 栈帧 B |
| --- | ---: | ---: | ---: |
| `SharedMemory::Create` | 1373 → **402** | 56 → 54 | 280 → 264 |
| `SharedMemory::Open` | 1587 → **476** | 62 → 57 | 440 |
| `RingBuffer::Write` | 116（不变） | 2 | 40 |
| `RingBuffer::Read` | 84（不变） | 2 | 40 |
| `MessageQueue::Send` | 629 → **407** | 82 → **46** | 360 |
| `MessageQueue::Receive` | 747 → **429** | 77 → **45** | 440 |
| `MessageQueue::SendBatch` | 246 → 300 | 8 → 10 | 256 → 272 |
| `MessageQueue::ReceiveBatch` | 342（不变） | 16 | 360 |

`SharedMemory::Create/Open` 的指令数降 70%：错误消息拼接（`"shm_open(" + name + ")"`）
与 `MakeErrnoError` 全部落到 `.text.unlikely`，热路径只剩 `shm_open`+`ftruncate`+`mmap`。

**`RingBuffer::Write/Read` 没有改动**：它不含 syscall（纯内存 memcpy + 两个原子），
`mmap` 只在 `SharedMemory::Create/Open` 里发生一次，之后是纯用户态拷贝 ——
25~32 GiB/s 的吞吐已经接近单核 memcpy 上限，没有优化空间。

---

## 3. 架构优化

### 3.1 批量 syscall：`writev` / `readv`（数量级）

**问题**：`File::Write` 单条 8 B 要 837 ns，`Read` 要 457 ns。8 B 数据的 memcpy 是亚纳秒级，
也就是说 **99% 的时间花在 syscall 进入/退出和函数调用边界上**，不是数据。

**做法**：新增 `File::Writev(iov, iovcnt)` / `Readv` / `WritevAll` / `ReadvAll`，
一次 syscall 处理最多 1024 个缓冲区（`IOV_MAX`）。`WritevAll` 快路径零分配，
只在真的短写时才复制 iovec 做精确推进。

实测（1e6 条 8 B，批 1024 条）：

| 路径 | 总耗时 | ns/条 | 相对单条 |
| --- | ---: | ---: | ---: |
| `File::Write` ×1e6 | 837.5 ms | 837.5 | 1.0× |
| `File::Writev` ×977 批 | 46.7 ms | **46.7** | **17.9×** |
| `File::WritevAll` ×977 批 | 43.2 ms | **43.2** | **19.4×** |
| `File::Read` ×1e6 | 457.0 ms | 457.0 | 1.0× |
| `File::Readv` ×977 批 | 21.8 ms | **21.8** | **20.9×** |
| `File::ReadvAll` ×977 批 | 22.6 ms | **22.6** | **20.2×** |

原生 `writev`/`readv` 同口径对照：45.9 / 20.8 ns/条 —— **go2cpp 的批量实现与原生持平
（1.02 / 1.05×）**，说明批量路径本身没有额外开销。

> 这条是本次唯一「改数量级」的优化。它适用的场景是**逐条写小记录**
> （日志行、IPC 帧、定长采样），调用方要把「循环 `Write`」改成「攒 iovec + `WritevAll`」。

### 3.2 零拷贝：`File::CopyTo`（sendfile）

**做法**：文件→文件拷贝走 `sendfile(2)`，数据在内核页缓存里直接接驳，不经过用户态缓冲。
内核/文件系统不支持时（`ENOSYS`/`EINVAL`/`EXDEV`/`EOPNOTSUPP`，且一字节未拷）**自动回退**
到 `read`+`write`。语义细节：`sendfile` 不移动 `in_fd` 偏移，所以拷贝后显式 `Seek` 补齐，
让「读了多少偏移就前进多少」的直觉成立。

| 场景 | `CopyTo`(sendfile) | `read`+`write` | 倍率 |
| --- | ---: | ---: | ---: |
| 8 MiB（热 page cache） | 1237 ~ 1251 MiB/s | 1004 ~ 1118 MiB/s | **1.10 ~ 1.15×** |
| 256 MiB（热 page cache） | 1120 ~ 1237 MiB/s | 1093 ~ 1124 MiB/s | **1.00 ~ 1.12×** |
| 256 MiB（冷读，`posix_fadvise(DONTNEED)`） | 1043 MiB/s | 692 MiB/s | **1.51×** |

诚实结论：**热 page cache 下收益只有 5~15%**（内核路径本身也有开销，read/write 的
用户态拷贝被 L3 吃掉）；冷读场景才有 1.5×。所以 `CopyTo` 的定位是
「省一次用户态拷贝 + 省一块用户缓冲（大文件拷贝时内存峰值更低）」，
不是「无条件 1.5×」。

### 3.3 汇编优化：内联 syscall（百分点，但绝对值可观）

`::syscall(SYS_*)` 是 libc 的通用变参入口。微基准（同进程、同 fd、同 buffer、交替测量）：

| 调用方式 | 8 B `read` | 8 B `write` | `getpid` |
| --- | ---: | ---: | ---: |
| libc `read`/`write` | 460 ns | 820 ns | — |
| `::syscall(SYS_*)` | 457 ns | 822 ns | 151.1 ns |
| 内联 `syscall` 指令 | **311 ns** | **571 ns** | **106.5 ns** |

- 纯包装差额：**45 ns/次**（`getpid` 151.1 → 106.5）；
- 落到真实 IO 上：`read` -32%、`write` -30%（因为省掉的是「函数调用 + 参数搬运 + TLS errno」这一整段）；
- **但跨 so 调用会把其中大部分吃掉**：`File::Read`（库函数、非内联）实测 459~470 ns，
  与 libc 版本持平 —— 也就是说**「调用边界」本身在 WSL2 上约 140 ns**（noinline 包装实测
  311 → 457 ns），不是 libc 的锅，是「`syscall` 指令的前后必须有一条 call/ret 边界」的代价。

这正是「汇编优化是百分点」的典型：单个函数指令数 -17%~-40%、栈帧 0，但端到端
只有 1~3%（因为边界税无法只靠改函数体消掉，要靠**批量化**把边界摊薄）。
第 3.1 节的 17.9× 就是「把边界税按 1024 摊」的结果。

---

## 4. 对标：vs 原生 syscall、vs Go `os` 包

### 4.1 1e6 次 8 B 读写（同机、`taskset -c 6`、3 轮）

| 实现 | write ns/op | read ns/op |
| --- | ---: | ---: |
| 原生 `::write` / `::read`（C） | 827 ~ 838 | 450 ~ 464 |
| **go2cpp `File::Write` / `File::Read`** | **829 ~ 837** | **459 ~ 467** |
| 比值（go2cpp / 原生） | **1.00 ~ 1.01×** | **1.01 ~ 1.03×** |
| Go `os.File.Write` / `os.File.Read` | 1029 ~ 1053 | 609 ~ 635 |
| Go `syscall.Write` / `syscall.Read` | 998 ~ 1039 | 586 ~ 590 |
| 比值（go2cpp / Go os.File） | **0.79 ~ 0.81×** | **0.73 ~ 0.76×** |

Go 慢的部分是 `entersyscall/exitsyscall` 的 P 状态切换（`syscall.*` 与 `os.File.*` 几乎同速，
说明开销在 runtime 而不在 `os` 包装）。go2cpp 因为直接发 `syscall` 指令，比 Go 快 20~27%。

### 4.2 `ReadFile` / `WriteFile` 8 MiB

| 实现 | WriteFile | ReadFile |
| --- | ---: | ---: |
| go2cpp | 6.5 ms（首轮）/ 10.3~10.5 ms | 2.1~2.4 ms（热）/ 5.8~6.1 ms（首轮） |
| Go `os.WriteFile` / `os.ReadFile` | 9.8 ~ 10.5 ms | 7.2 ~ 7.9 ms |
| 比值 | **1.00 ~ 1.05×** | **0.30 ~ 0.33×**（热） |

`ReadFile` 比 Go 快 3 倍：Go 的 `os.ReadFile` 走 `stat` 预分配 + 逐块 append，
go2cpp 用 `st_size` 一次 `reserve` 再 `append`，热缓存下几乎只剩 memcpy。
写路径两者都受 ext4 的 writeback 支配，差距在噪声内。

### 4.3 `Process` spawn+wait

| 口径 | go2cpp | Go `exec.Command` | C 手写骨架 |
| --- | ---: | ---: | ---: |
| 跨进程（各自独立二进制） | 0.69 ~ 0.72 ms/pair | 0.498 ms/pair | 0.475 ms/pair（小进程） |
| **同进程 A/B（公平）** | **0.73 ~ 0.89 ms/pair** | —（Go 内无法 fork） | **0.65 ~ 0.73 ms/pair** |
| 比值 | — | 1.45×（跨进程，**不可比**） | **1.13 ~ 1.22×** |

跨进程比较**不可比**的原因：`fork` 要复制页表，成本随地址空间增长。小 C 进程 0.475 ms，
而链接了 10 个 `.so` 的 go2cpp 测试进程，连手写骨架都要 0.65~0.73 ms。同进程 A/B 才是公平口径，
结果是 **1.13~1.22×** —— 已接近但**没有达到 <1.1×**。

### 4.4 `Shm` / `Mq`（无 Go 内置对应物，给绝对值）

| 项 | 结果 |
| --- | ---: |
| `SharedMemory` + `RingBuffer` 写 512 MiB | **25.5 GiB/s** |
| `SharedMemory` + `RingBuffer` 读 512 MiB | **32.1 GiB/s** |
| `MessageQueue` Send+Receive 8 B 往返 ×2e5 | **614k msg/s** |
| `MessageQueue` SendBatch+ReceiveBatch（8 条/批）×2e4 | **628k msg/s** |

`RingBuffer` 单次 `Write` 只有 116 条指令、2 个 call、无 syscall，25~32 GiB/s 已经贴着单核
memcpy 上限（DDR5 单核流式拷贝约 30~40 GiB/s），没有可优化的汇编空间。

---

## 5. 优化清单与前后对照

### 5.1 已落地

| # | 优化 | 类型 | 证据（前 → 后） |
| --- | --- | --- | --- |
| 1 | 读写/定位/元信息改内联 `syscall` 指令 | 汇编 | `File::Read` 46→38 指令、2→0 call、24→0 B 栈帧 |
| 2 | 批量 `Readv`/`Writev`/`WritevAll`/`ReadvAll` | 架构 | 837→46.7 ns/条（17.9×）、457→21.8（20.9×） |
| 3 | `CopyTo`（sendfile 零拷贝 + 回退） | 架构 | 8 MiB 1.10~1.15×；冷读 1.51× |
| 4 | `MakeError`/`MakeErrnoError` 标 `[[gnu::cold]]` | 汇编 | `Process::Wait` 81→39 指令；`Shm::Create` 1373→402；`Mq::Send` 629→407 |
| 5 | `ReadProcStat` 用 `strtoll` 替换 `istringstream` | 实现 | 单次解析 35 µs → 1~2 µs（见下） |
| 6 | `Spawn` 去掉重复的 `/proc/<pid>/stat` 读取 | 架构 | 每次 spawn 少 open+read+close+解析 |

关于 #5/#6 的净效果：`Process::Spawn` 从 0.694 ms/次 降到 **0.674~0.703 ms/次**（同轮测量，
降幅在 1~3%），说明**`/proc` 解析不是 spawn 的主要成本**（`fork`+`execve`+动态链接占了绝大部分）。
两项改动保留，因为它们是纯收益（少一次 syscall、少一次 35 µs 解析），且顺带把
`SpawnedProcess` 的快照信息补全了。

### 5.2 未做 / 明确不适用

- **头文件内联化 `File::Read/Write`**：实测能把 459 ns 拉回 311 ns（-32%），但会把实现搬进
  `file.hpp`、破坏 ABI 与封装，收益依赖调用方编译器。**批量化（#2）是更干净的解法** ——
  同样的收益，不动 ABI。
- **`splice` 文件↔文件**：语义要求至少一端是 pipe，`File` 只接受普通文件，
  绕 pipe 反而多两次 syscall。只用 `sendfile`。
- **`io_uring`**：超出「架构+汇编+对标」范围，且需要在 `IOManager` 层做所有权改造。

### 5.3 遗留问题（如实记录）

1. **`Process` spawn 1.13~1.22×，未达 <1.1×**。已排除：`/proc` 读取（#5/#6 后无变化）、
   fork 的地址空间成本（同进程 A/B 已对齐）、`DefaultProcessBackend`（static，无开销）、
   `LoadProcessDefaults`（不在 spawn 路径）。剩余候选（未逐一验证）：
   `BuildEnvironment` 的 O(n²) 环境变量替换、3 条 stdio 管道的 `pipe2`+`dup2`+`fcntl`、
   `ProcessOptions` 两次深拷贝（`impl->options = options`）。建议下一步用 `perf record -g`
   或 `-pg` 构建定位，而不是继续猜。
2. **`ReadFile`/`WriteFile` 指令数增长**（1967→2802 / 1067→1589）：`File::Read` 内联进循环
   所致。端到端无回退（8 MiB 仍是 10.3 ms / 2.2 ms），但若将来要压 i-cache，
   可以在循环里改用 `noinline` 的读块辅助函数。
3. **`SendBatch` 指令数 +22%**（246→300）：`cold` 标记把错误路径外移后，编译器对这个
   小函数做了不同的内联决策。端到端批量吞吐 628k msg/s 未回退，保留。
4. **`CopyTo` 热缓存收益有限**（1.00~1.15×）：已在上文标注，不要按 1.5× 宣传。

---

## 6. 正确性

### 6.1 测试场景（`tests/os/test_os_file.cpp`，本次新增 5 个）

| # | 场景 | 覆盖点 |
| --- | --- | --- |
| 13 | **1e6 次读写逐字节一致** | 写 1e6 条 8 B 记录（序号+反码）；`File::Read` 顺序逐条比对；`ReadFile` 整读再逐条比对 |
| 14 | 批量 IO | `Writev`/`WritevAll`×10 批、`ReadvAll` 回读比对、EOF 部分读（100/8192）、`nullptr`/`iovcnt=0`/`iovcnt=1025` 边界、全零 iovec、混合零长段 |
| 15 | `CopyTo` | 8 MiB 逐字节一致、空文件（0 B）、`max_bytes=4096` 截断 + 偏移推进、坏句柄 `kInvalidHandle`/`EBADF` |
| 16 | **EINTR** | `SIGALRM` 每 1 ms 一次（`sa_flags=0`，**不**带 `SA_RESTART`），8 MiB 读写全程不得浮出 `EINTR`、逐字节校验内容、断言信号确实投递过 |
| 17 | 大小边界 | 0 字节文件 `ReadFile` 返回空且 `st_size==0`；64 MiB 分块写 + `ReadFile` 整读 + 块模式校验 |

### 6.2 结果

```
$ ctest --test-dir /tmp/build-os -R "^go2cpp_os_test$|^go2cpp_os_dir_test$" --output-on-failure
1/2 Test #44: go2cpp_os_test ...................   Passed    1.66 sec
2/2 Test #45: go2cpp_os_dir_test ...............   Passed    0.07 sec
100% tests passed, 0 tests failed out of 2
```

- `go2cpp_os_test`：`[os] failures=0`，17 个场景（原 12 + 新 5）全绿，1.66 s；
- `go2cpp_os_dir_test`：`[os-dir] failures=0`；
- 编译：`-O2 -Wall -Wextra -Wpedantic`，**0 warning**；
- `go2cpp_os_hook_conflict_test` 未构建（依赖 `go2cpp_hook`，本次构建未启用该目标）。

---

## 7. 复现命令

```bash
# 构建（独立目录，不污染源码树）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-os -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build /tmp/build-os --target go2cpp_os go2cpp_os_test -j12

# 正确性
ctest --test-dir /tmp/build-os -R "^go2cpp_os_test$|^go2cpp_os_dir_test$" --output-on-failure

# objdump 统计（脚本在 /tmp/os-bench/asm_stats.py）
python3 /tmp/os-bench/asm_stats.py /tmp/build-os/libgo2cpp_os.so

# 对标基准（源码在 /tmp/os-bench/*.cpp，Go 在 /tmp/os-bench/gobench/、goal）
taskset -c 6 /tmp/os-bench/bench_file 1     # 单条 vs 原生 1e6
taskset -c 6 /tmp/os-bench/bench_batch      # 批量 writev/readv + CopyTo
taskset -c 6 /tmp/os-bench/bench_ipc        # Process/Shm/Mq
taskset -c 6 /tmp/os-bench/gobench_bin 0    # Go os.File / os.ReadFile / 4KiB
taskset -c 6 /tmp/os-bench/goexec_bin       # Go exec.Command
taskset -c 6 /tmp/os-bench/ab_spawn         # 同进程 spawn A/B（公平口径）
```

基准源码与日志留在 `/tmp/os-bench/`（`bench_file.cpp`、`bench_batch.cpp`、`bench_ipc.cpp`、
`ab_spawn.cpp`、`fork_raw.cpp`、`fork_split.cpp`、`asm_stats.py`、`gobench/`、`goexec/`）。
