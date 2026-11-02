> 来源：/tmp/debug_asm.md，2026-11-02 归档

# debug 事件环 / replay 指令级取证与对标

> 日期：2026-11-02 13:00–13:15（Asia/Shanghai）
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL，分支 `feat/isolated`，HEAD `5be9a9c`）
> 独立构建：`/tmp/build-debug/off`（全 OFF）与 `/tmp/build-debug/on`
> （`GO2CPP_DEBUG_DUMP=ON` + `GO2CPP_REPLAY=ON`），Release `-O3 -DNDEBUG`，12 核 WSL2。
> 指令级：`objdump -d --demangle --no-show-raw-insn` 反汇编**链接后**的
> `libgo2cpp_debug_event.so` 与 `replay_bench` 可执行文件（避免 .o 未重定位
> 的 `call +0xNN` 假象）。
> 运行时：新增 `tests/debug/debug_asm_bench.cpp`、`tests/debug/debug_replay_bench.cpp`、
> `tests/debug/run_debug_asm.sh`；原始 TSV 在 `/tmp/debug-asm/`。
> **未修改任何产品代码，未 commit。**

---

## 0. 结论（先看这里）

1. **debug 没有退化，反而大幅改善**：同一套 6 场景 × 3 配置 × 20 万次 × 3 轮，
   ON_FULL 慢倍数从 v5 的 **3.6–40.3×** 降到 **1.44–8.76×**（channel 20.0→3.14、
   mutex 18.9→6.65、resources 40.3→8.76、select 9.34→3.28、spawn 4.09→1.63、
   timer 3.60→1.44）。无任何场景比 v5 更差。
2. **OFF 是真正的零开销**：`EventRing::Record` 链接后只剩
   `endbr64; ret`（统计 4 条含对齐 nop），0 call、0 原子、0 栈帧。
3. **emit 热路径每条事件**：171 条指令、12 次 call（其中 **6 次
   `__tls_get_addr` + 1 次 `__tls_init`**）、**1 次 `lock addq`**、144 B 栈帧。
   实测单线程 25.9 ns/事件、4 线程 7.2 ns/事件（4 个分片计数互不争用）。
4. **序列化是 debug 侧最贵的部分**：`DumpText` 3873 条指令 / 206 call，
   `DumpJson` 2327 条指令 / 313 call；实测 20 480 条事件时 DumpText
   0.69 µs/条、DumpJson **4.35 µs/条**（JSON 8.2 MB，约为文本的 6.8 倍）。
   call 明细里每事件约 3 次 `operator delete` + 2 次 `std::string::_M_append`
   + 0.6 次 `to_string`——是「临时 string + 逐段 append」的典型形态。
5. **replay 录制每条事件**：92 条外层指令 + `EncodeEvent` 1356 条 +
   `Crc32` 127 条 + `WriteRaw` 775 条（静态合计约 2350 条），
   **2 次内联 `lock` RMW + `pthread_mutex_lock/unlock` 各 1 次**；
   实测 415.8 ns/条（2.41 M 条/s、163.6 MB/s）。
6. **回放速度基线**：仅解码（含逐条 CRC）721.0 ns/条，完整静态回放
   814.6 ns/条（仅比解码慢 13%）；对比纯 `fread` 上限（1108 MB/s）
   回放处理 83.5 MB/s，**是 I/O 物理上限的 1/13.3**。
7. **正确性**：1e6 条事件累计计数守恒（单线程/4 线程均
   `delta == 1 000 000`）；20 480 条快照排序/唯一/文本行数/JSON 字段数/
   两次 dump 逐字节/`ReadSince` 分页全部通过；1e6 条 replay 事件
   **时间戳归一化后重新编码逐字节一致**，两次回放 JSON 报告逐字节相同，
   独立 FNV 校验和与 500 000 条接收序列逐项一致。

---

## 1. objdump：debug 事件环（emit / flush / 序列化）

反汇编对象：`/tmp/build-debug/on/libgo2cpp_debug_event.so`（与生产 ON 构建同参数）。

| 函数 | 指令 | call | 原子 | 栈帧 (B) | 说明 |
|---|---:|---:|---:|---:|---|
| `EventRing::Record`（emit） | 171 | 12 | 1 | 144 | 清 seq + 批量写槽位 + 分片计数 |
| `EventRing::RecordTimer` | 98 | 5 | 1 | 136 | timer 事件（不走采样） |
| `EventRing::RecordSampled` | 66 | 2 | 0 | 0 | 采样判定 + 转调 Record |
| `EventRing::Snapshot` | 284 | 12 | 0 | 280 | 采集全部线程环 + 排序 |
| `EventRing::ReadSince(cursor)` | 252 | 2 | 0 | 16 | 订阅分页 |
| `EventRing::DumpText`（序列化） | 3873 | 206 | 0 | 536 | 文本 dump |
| `EventRing::DumpJson`（序列化） | 2327 | 313 | 0 | 568 | JSON dump |
| `EventRing::WriteToFile`（flush/落盘） | 700 | 44 | 0 | 712 | ofstream 打开+写+flush |
| **OFF 对照**：`Record` / `RecordTimer` / `RecordSampled` | **4** | **0** | **0** | **0** | `endbr64; ret` + 对齐 nop |

### 1.1 emit 的指令构成（`Record`）

- 栈帧：`sub $0x90,%rsp` = **144 B**，另有 5 个 `push`（40 B）+ 返回地址，
  实际栈占用约 192 B；带 `%fs:0x28` 栈保护（`__stack_chk_fail` 冷路径）。
- 12 个 call 的明细（`@plt` 表示未内联的跨 DSO 调用）：

  | 次数 | 目标 | 性质 |
  |---:|---|---|
  | 6 | `__tls_get_addr@plt` | 每次访问 `thread_local` 都走通用 TLS 解析 |
  | 1 | `__tls_init` | thread_local 动态初始化守卫 |
  | 2 | `SchedEventCodeIsCritical@plt` | 同一 TU 的全局函数，`-fPIC` 下未内联 |
  | 1 | `SchedEventSampleEvery@plt` | 同上（v7 采样判定） |
  | 1 | `std::chrono::steady_clock::now@plt` | 时间戳 |
  | 1 | `__stack_chk_fail@plt` | 冷路径（栈保护失败） |

- 唯一的内联原子（累计计数分片，`t_totals->codes[shard][code].fetch_add`）：
  ```asm
  lock addq $0x1,(%rdx,%rax,8)
  ```
- 槽位写入是 **2 条 cacheline 的 memcpy**（`WriteSlot` 已内联），
  没有逐字段 store；`seq` 先清零、最后写回，读侧二次校验。
- 关键结论：**v6 去掉的全局 seq `fetch_add` 确实不在了**——每事件只剩
  1 次无争用的分片计数原子；剩下 6 次 `__tls_get_addr` 是 `-fPIC`
  TLS 通用模型的固定成本，不是事件环语义成本。

### 1.2 序列化（`DumpText` / `DumpJson`）

`DumpText` 的 206 个 call 中占比最高的：

| 次数 | 目标 | 含义 |
|---:|---|---|
| 54 | `operator delete` | 临时 string / to_string 的释放（每事件约 2.6 次） |
| 41 | `std::string::_M_append` | 每事件约 2 次追加 |
| 40 | `std::__throw_length_error` | 异常冷路径（`append` 预留失败时才执行） |
| 13 | `_M_replace` | 追加/拼接 |
| 12 | `std::__cxx11::to_string(unsigned long)` | 数值转字符串（每事件 0.6 次） |
| 11 | `_M_construct` | 新 string 构造 |
| 5 | `append(const char*)` | 常量片段追加 |

`DumpJson` 的 313 个 call：`operator delete` 60、`to_string` 27、
`_M_replace` 27、`_M_dispose` 25、`_M_append` 22、`_M_append`/`append` 共 40、
`insert` 11、`to_string(unsigned int)` 9。

实测（20 480 条事件、64 线程环、200 次重复中位数；下表为 3 次独立运行的
代表值，run-to-run 波动 <10%）：

| 指标 | 值 |
|---|---:|
| `Snapshot` | 3.70 ms/次（181 ns/条） |
| `DumpText` | 14.13 ms/次（0.69 µs/条），1.21 MB，85.8 MB/s |
| `DumpJson` | 89.01 ms/次（**4.35 µs/条**），8.20 MB，92.1 MB/s |
| `WriteToFile(text)` | 0.29 ms/次（320 条），69.0 MB/s |
| `WriteToFile(json)` | 0.73 ms/次（320 条），177.8 MB/s |

> JSON 每条约 400 B（完整字段），文本每条约 59 B；JSON 的 `to_string` 调用
> 是文本的 2.25 倍，与 6.8 倍体积、7.7 倍时间同向。`WriteToFile` 的 MB/s
> 高于 `DumpText` 的 MB/s 是因为两者分母不同（落盘只测一次序列化后的写，
> 且文件页缓存命中）；落盘调用本身仍是「每次重建 ofstream」。

### 1.3 采集/订阅侧

`Snapshot` 284 条指令、280 B 栈帧、12 call：每线程环遍历 + 复制
320 条 × 136 B 记录 + 全量排序（`std::sort`），实测 223 ns/条。
`ReadSince` 252 条指令、16 B 栈帧、0 原子，分页 97 条一遍
20 480 条不重不漏（`read_since_ok=1`）。

---

## 2. objdump：replay（记录 / 回放）

反汇编对象：`/tmp/build-debug/bin/replay_bench`（静态链接
`format/recorder/replay_engine/timer/split/auto_entry`，`-O3 -DNDEBUG`，
`GO2CPP_REPLAY_LINK_RUNTIME=1`）。

| 函数 | 指令 | call | 原子 | 栈帧 (B) | 位置 |
|---|---:|---:|---:|---:|---|
| `Recorder::Write`（记录入口） | 92 | 9 | **2** | 8 | 每事件 |
| `Recorder::WriteRaw` | 775 | 22 | 0 | 456 | 每事件（fwrite + 错误处理） |
| `EncodeEvent`（编码） | 1356 | 107 | 0 | 168 | 每事件 |
| `Crc32` | 127 | 2 | 0 | 8 | 每事件 |
| `LoadFile` | 801 | 98 | 0 | 728 | 每文件一次 |
| `DecodeEvent`（解码） | 2402 | 74 | 0 | 776 | 每事件 |
| `AnalyzeLoaded`（静态重演） | 2798 | 227 | 0 | 1336 | 每文件一次 |
| `ReplayFile`（回放入口） | 672 | 16 | 0 | 1064 | 每文件一次 |
| `Engine::Run`（完整重放） | 1818 | 148 | 0 | 408 | 每文件一次 |

### 2.1 记录热路径

`Recorder::Write` 的完整前缀（每条事件都执行）：

```asm
call   <pthread_mutex_lock@plt>        ; 一把互斥锁
lock   xadd %rax,0x90(%rbx)            ; event.seq = m_seq.fetch_add(1)
...
call   <EncodeEvent>                   ; 1356 条指令 / 107 call
call   <Crc32>                         ; 127 条指令
call   <Recorder::WriteRaw>            ; 775 条指令 / fwrite
call   <pthread_mutex_unlock@plt>
lock   addq $0x1,0xa0(%rbx)            ; m_records.fetch_add(1)
```

- 每条事件 **1 次 pthread mutex + 2 次 `lock` RMW**，且 `EncodeEvent`
  本身有 **81 次 `vector<unsigned char>::push_back`（逐字节）**、6 次
  `memmove`、2 次 `operator new`、3 次 `operator delete`。
- 静态指令合计约 **2350 条/事件**（92+1356+127+775，不含 PLT 内部），
  与实测 415.8 ns/条（2.41 M 条/s、163.6 MB/s）一致：瓶颈是编码的分支与
  分配，而不是磁盘（/tmp 为 tmpfs）。

### 2.2 回放热路径

`ReplayFile` → `LoadFile`（801 条 / 98 call / 728 B）→ 每事件
`DecodeEvent`（2402 条 / 74 call / 776 B，含 12 次 `operator delete`、
7 次 `memmove`、1 次 `operator new`——每条事件的 `std::string`/`std::vector`
构造析构）→ `AnalyzeLoaded`（2798 条 / 227 call，一次性）。

- 回放侧**没有原子**（单线程只读重演）；`AnalyzeLoaded` 的 227 个 call
  是 `std::map<std::set>` 配对、`vector` 统计与 `std::to_string` 错误路径。
- 完整重放 `Engine::Run` 1818 条 / 148 call / 408 B，只在
  `ReplayFileWith` 路径使用（本次 1e6 事件用静态重演，不走 ucontext）。

---

## 3. 对标：debug 开 / 关（复跑 c3cf26f 场景）

`tests/bench/run_debug_overhead.sh`（副本在 `/tmp/go2cpp-bench`，结果不落仓库），
N=200 000、3 轮中位数、6 场景 × 3 配置，loadavg 全部有效（18/18）。

| 场景 | OFF (ops/s) | ON_FULL (ops/s) | 全开慢倍数 | v5 慢倍数（c3cf26f） | 变化 |
|---|---:|---:|---:|---:|---:|
| channel | 5 022 083 | 1 600 749 | **3.14×** | 20.0× | −84% |
| mutex | 48 466 202 | 7 282 709 | **6.65×** | 18.9× | −65% |
| spawn | 3 844 550 | 2 355 417 | **1.63×** | 4.09× | −60% |
| timer | 1 141 341 | 795 341 | **1.44×** | 3.60× | −60% |
| resources | 467 846 693 | 53 397 341 | **8.76×** | 40.3× | −78% |
| select | 995 114 | 303 420 | **3.28×** | 9.34× | −65% |

采样配置（`GO2CPP_DEBUG_CATEGORIES=sched,park` + `SAMPLE_SHIFT=5`）：
channel 1.8×、mutex 1.2×、spawn 1.8×、timer 1.2×、resources 0.8×（噪声，
该场景事件被类别过滤为 0）、select 2.7×。

**判定：无退化。** 全部 6 个场景的全开倍数都低于 v5 基线；
事件量也同步下降（channel 34.1→13.7 条/操作，select 18.8→10.0，
resources 15.4→0.8，timer 14.3→6.8）。注意本机 OFF 绝对值比 v5 机器高
（channel 5.0 M vs 2.0 M），所以只比较**同机同轮的 ON/OFF 倍数**。

残余最高项仍是 `mutex`（6.65×）：它的实际事件只有 0.32 条/操作
（63 651 条 / 20 万 ops），说明开销主要来自**记录点判定**
（`GO2CPP_DEBUG_EVENT_SAMPLED` 的类别原子读 + 采样计数器），
而不是事件环写入；采样配置降到 1.2× 也印证这一点。

---

## 4. 对标：replay 回放速度

合成 1e6 条 send/recv 事件（gid=1、channel=7、xfer_id 配对），
分阶段独立进程测量（3 次运行取代表值，波动 <5%）：

| 阶段 | ns/事件 | 事件/s | 处理带宽 | 相对基线 |
|---|---:|---:|---:|---:|
| 录制（编码+CRC+fwrite） | 415.8 | 2.41 M | 163.6 MB/s | 1.00× |
| 仅解码（`LoadRecordingEvents`，含逐条 CRC） | 721.0 | 1.39 M | 94.3 MB/s | 1.73× 录制 |
| 完整静态回放（解码 + 分析 + 统计） | 814.6 | 1.23 M | 83.5 MB/s | 1.96× 录制 / 1.13× 解码 |
| 纯 `fread` 上限 | — | — | 1108 MB/s | **0.075× 回放** |
| 纯逐字节扫描上限 | — | — | 1196 MB/s | — |

- **回放速度基线取「仅解码」和「纯 I/O」两条**：完整回放只比解码慢 13%，
  说明分析层（`AnalyzeLoaded`）不是瓶颈；回放是纯 `fread` 上限的
  **1/13.3**，差距来自 `DecodeEvent`（2402 条指令/条、12 次 delete）
  与逐条 CRC 校验，而不是文件 I/O。
- 录制 163.6 MB/s ≈ 68 MB / 0.416 s；回放 83.5 MB/s ≈ 68 MB / 0.815 s。
- 1e6 事件文件 68 000 349 B（68 B/条），加载后 `AnalyzeLoaded` 常驻
  约 300 MB（每条 `Event` 含 `std::string`/`vector`），这是 1e6 规模的
  内存口径。

---

## 5. 正确性

### 5.1 1e6 事件（事件环）

`debug_asm_bench emit --n=1000000 --threads=4`：

| 指标 | 单线程 | 4 线程 |
|---|---:|---:|
| 记录条数 | 1 000 000 | 1 000 000 |
| 计数增量（`TotalCountAll`） | 1 000 000 | 1 000 000 |
| 墙钟 | 25.95 ms | 7.20 ms |
| ns/事件 | 25.95 | 7.20 |
| 吞吐 | 38.5 M/s | 138.8 M/s |
| 守恒校验 | PASS | PASS |

（空循环物理下限 0.096 ns/次；单线程 25.9 ns 中约 6 次 TLS 解析 +
1 次原子 + 时钟是主要部分。）

### 5.2 快照 / 序列化 / 订阅（20 480 条）

64 个线程各写满普通区 256 + 关键区 64 = 20 480 条，全部断言通过：

| 断言 | 结果 |
|---|---|
| `dump_events == 20 480` | 1 |
| 快照按 `(ts, thread_id, seq)` 严格升序 | 1 |
| 同线程 `seq` 唯一（无撕裂/无重复） | 1 |
| 文本 `event_count` 行 + 事件行数 == 快照 | 1 |
| JSON `"seq":` 出现次数 == 快照 | 1 |
| 无并发写入时两次 dump 逐字节相同 | 1 |
| `ReadSince` 分页（97 条/页）恰好覆盖快照 | 1 |

### 5.3 1e6 事件 replay 逐字节一致

`debug_replay_bench bytes --n=1000000`：

| 检查 | 结果 |
|---|---|
| 两次录制文件大小 | 68 000 275 B（相同） |
| 原始字节（排除 header `created_unix_ns`）差异 | **19 B / 68 000 275 B**（仅 `kMeta`/`kEnd` 的墙钟时间戳及覆盖它们的 CRC） |
| 解码 → 时间戳归一化 → 重新编码，逐条逐字节比较 | **1**（1 000 003 条全等） |
| 同一文件回放两次 → JSON 报告逐字节相同 | **1**（3 446 448 B） |
| 独立 FNV-1a 消息校验和 | 一致 |
| 接收序列（500 000 项）逐项 | 全部等于 1,3,5,…,999 999 |
| 文件级 CRC / `kEnd` 完整性 | `checksum_ok=1`、`complete=1`、`status=ok` |

> 说明：`Recorder` 在文件头写 `created_unix_ns`、在 `kMeta`/`kEnd` 写墙钟
> 时间戳，这是格式设计（不是不确定性）；把它们归一化后，事件流编码
> **逐字节完全确定**，满足「回放逐字节一致」的口径。

---

## 6. 证据文件与复现

```bash
# 构建（独立目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-debug/off \
  -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=OFF \
  -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_DEBUG_DUMP=OFF -DGO2CPP_REPLAY=OFF
cmake --build /tmp/build-debug/off -j6
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-debug/on \
  -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=OFF \
  -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_REPLAY=ON
cmake --build /tmp/build-debug/on -j6

# 指令级 + 微基准（事件环 emit/dump/write + replay record/decode/replay/bytes/iobase）
BUILD=/tmp/build-debug OUT=/tmp/debug-asm N=1000000 \
  bash tests/debug/run_debug_asm.sh

# 6 场景 × 3 配置 × 20 万 × 3 轮（结果在 /tmp/go2cpp-bench，不入仓库）
cd /tmp/go2cpp-bench && BUILD_OFF=/tmp/build-debug/off BUILD_ON=/tmp/build-debug/on \
  N=200000 ROUNDS=3 bash tests/bench/run_debug_overhead.sh
```

原始数据：

- `/tmp/debug-asm/objdump_stats.tsv`、`ring_on_stats.tsv`、`ring_off_stats.tsv`、
  `replay_stats.tsv`：每条函数的指令/call/原子/栈帧。
- `/tmp/debug-asm/asm_record_on.txt`、`asm_write.txt`、`ring_on_so.txt`：
  完整反汇编片段。
- `/tmp/debug-asm/asm_on_emit.tsv`、`asm_on_dump.tsv`、`asm_on_write.tsv`、
  `asm_off_off.tsv`、`replay_record.tsv`、`replay_decode.tsv`、
  `replay_replay.tsv`、`replay_bytes.tsv`、`replay_iobase.tsv`：运行时数据。
- `/tmp/go2cpp-bench/tests/bench/results/debug_overhead_20261102-130842*.{tsv,md}`：
  6 场景压测原始轮次与汇总。

---

## 7. 残余开销与可行方向（仅取证，未改代码）

| 热点 | 指令级证据 | 可行方向 |
|---|---|---|
| emit 的 6×`__tls_get_addr` + 1×`__tls_init` | `Record` 反汇编 | 热 TLS 字段聚合进一个 `thread_local` 结构；预加载共享库可用 `-ftls-model=initial-exec` |
| 同 TU 函数未内联 | `SchedEventCodeIsCritical@plt`（2 次/事件）、`SchedEventSampleEvery@plt` | 标 `inline`/`static`，或 `-fno-semantic-interposition` |
| JSON 序列化 4.6 µs/条 | `DumpJson` 313 call、60 delete、27 `to_string` | 复用 `std::to_chars` 直接写入预分配缓冲，取消临时 string |
| replay 编码逐字节 push_back | `EncodeEvent` 81 次 `push_back`、2 new/3 delete | 先 `resize` 再按下标写，或分段 `append` |
| 录制每事件一把互斥锁 | `Write` 的 `pthread_mutex_lock` | 每线程独立 buffer + 后台合并（会改变 seq 分配语义，需设计） |
| 回放每条 12 次 delete | `DecodeEvent` 776 B 栈帧、12 delete | 复用 `Event`/string 缓冲池 |

> 按项目「架构优化 > 汇编优化」的顺序：debug 侧的结构性收益（v6 去全局
> seq、v7 分层采样）已经在 c3cf26f→89f0d7c 拿到，剩下的是百分点级的
> 汇编/API 层面；replay 侧真正的架构点在「逐字节 push_back 编码」
> 与「每事件互斥锁」，不是指令选择。
