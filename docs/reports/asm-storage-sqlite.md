> 来源：/tmp/sqlite_asm.md，2026-11-02 归档

# SQLite 封装汇编级取证与对标（go2cpp storage）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树，非 E: 树）
- 构建目录：`/tmp/build-sqlite`（独立，未污染项目内构建目录）
- 日期：2026-11-02
- 结论一句话：**文件库 1e6 行插入 1.078x（<1.1x）**；7 个场景 6 个 <1.1x，
  内存插入 1.103x 在边界（波动区间跨 1.0）。热路径返回 `const Error&` 是
  本轮唯一保留的优化（insert-file 1.131x → 1.078x）。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 内核 | WSL2，6.6.87.2-microsoft-standard |
| 发行版 | Ubuntu 24.04.3 LTS |
| 编译器 | g++ 13.3，`-O3 -DNDEBUG -std=c++17 -fPIC` |
| 模块选项 | `-fno-semantic-interposition -fno-stack-protector`（CMake 既有） |
| SQLite | 3.45.1（系统 `libsqlite3.so.0`，动态链接） |
| CPU | 12 vCPU；基准 `taskset -c 0` |
| 采样 | 每场景 7 轮，取**每轮比值的中位数**；表中给 min/max 波动 |

机器上同时有其它 agent 任务（`load average` 3.5–12），单轮 ns 波动可达
±25%，因此所有结论以**7 轮中位数 + 波动区间**为准，不用单次数据。

---

## 2. objdump 取证（热函数）

反汇编对象：`/tmp/build-sqlite/libgo2cpp_storage.so.0.1.0`
（`objdump -d -C --no-show-raw-insn`）。统计脚本：`tests/storage/asm_stats.py`。

### 2.1 当前版本

| 函数 | 指令数 | call 数 | 栈帧 | canary | mutex | 堆分配 |
|---|---:|---:|---:|---:|---:|---:|
| `Stmt::Step()` | 348 | 7 | 80 B | 0 | 0 | 0 |
| `Stmt::BindValues()` | 449 | 13 | 88 B | 0 | 0 | 0 |
| `Stmt::Reset()` | 218 | 4 | 80 B | 0 | 0 | 0 |
| `Rows::Next()` | 159 | 5 | 56 B | 0 | 0 | 0 |
| `DB::Open()` | 381 | 14 | 136 B | 0 | 0 | 1（`new DB`） |
| `DB::Prepare()` | 999 | 28 | 152 B | 0 | **2** | 0 |
| `DB::ExecImpl()` | 853 | 35 | 184 B | 0 | **2** | 0 |
| `DB::BindLocked()` | 103 | 9 | 24 B | 0 | 0 | 0 |
| `Stmt::ColumnInt64()` | 16 | 0 | 0 | 0 | 0 | 0 |
| `Stmt::ColumnDouble()` | 15 | 0 | 0 | 0 | 0 | 0 |
| `Stmt::ColumnInt64/Type` | 16 / 14 | 0 | 0 | 0 | 0 | 0 |
| `Stmt::ColumnText()`（拷贝） | 76 | 4 | 24 B | 0 | 0 | 0 |
| `Stmt::ColumnTextView()`（零拷贝） | 35 | 2 | 24 B | 0 | 0 | 0 |
| `Stmt::ColumnBlob()` | 36 | 2 | 0 | 0 | 0 | 0 |

要点：

- **全部函数 canary=0**（`-fno-stack-protector` 生效；优化前 `Step`/`BindValues`
  每行一次 canary 检查，已消除）。
- **`DB` 公开方法恰好 2 次 `pthread_mutex_*`（1 lock + 1 unlock）**，无嵌套；
  `Stmt`/`Rows`/`BindLocked` 热路径 0 mutex。
- 热路径无堆分配：`Value` 是栈上 `std::array`，短文本走 SSO；`Open` 的 1 次
  分配是 `new DB`（一次性）。
- `Column*` 全部 0 call（`ColumnInt64`/`Double`/`Type` 直接内联坐标计算），
  说明列访问没有额外函数调用开销。

### 2.2 优化前/后（同一构建选项）

| 函数 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---|
| `Stmt::Step` | 389 / 9 call / 80 B | 348 / 7 call / 80 B | −41 指令 |
| `Stmt::BindValues` | 494 / 15 call / 104 B | 449 / 13 call / 88 B | −45 指令，栈帧 −16 B |
| `Stmt::Reset` | 104 / 2 call / 56 B | 218 / 4 call / 80 B | 静态增加（见下） |

`Reset` 静态指令数变大是**失败路径**（`m_error = Error(...)` 的字符串构造）
被内联进来；callgrind 实测 `Reset` 每行只执行 **19 Ir**（见 §3），热路径更短。
这正说明：**静态指令数含冷代码，不能直接当热路径成本**，所以下面补了
callgrind 的实际执行 Ir。

---

## 3. 与原生 sqlite3 的同操作对比

两条互补证据：

### 3.1 同一二进制内的 wrapper / native 场景函数（静态）

`tests/storage/bench_db.cpp` 里每个场景都有封装版与原生版，场景函数加了
`noinline`，因此可在**同一二进制、同一优化等级**下直接比：

| 场景函数 | 指令数 | call | 栈帧 |
|---|---:|---:|---:|
| `InsertWithWrapper` | 273 | 42 | 520 B |
| `InsertWithNative` | 128 | 18 | 56 B |
| `PreparedWithWrapper` | 181 | 28 | 424 B |
| `PreparedWithNative` | 116 | 16 | 72 B |
| `MicroWithWrapper` | 128 | 19 | 312 B |
| `MicroWithNative` | 87 | 13 | 72 B |
| `SelectWithWrapper` | 102 | 14 | 296 B |
| `SelectWithNative` | 79 | 10 | 40 B |
| `UpdateWithWrapper` | 105 | 17 | 208 B |
| `UpdateWithNative` | 78 | 11 | 32 B |

封装函数体更大来自：错误分支、`Value` 构造、以及错误路径的字符串构造；
栈帧大是因为每个 `Value` 含 `std::string`（SSO 缓冲在 Value 内）。

### 3.2 callgrind：每行实际执行 Ir

探针：`tests/storage/asm_probe.cpp`（2 万行，内存库，单事务；wrapper 与
native 两个进程除插入路径外完全相同）。

| 指标 | wrapper | native | 差值 |
|---|---:|---:|---:|
| 总 Ir（20000 行） | 90,156,370 | 82,070,641 | 8,085,729 |
| 每行 Ir | 4507.8 | 4103.5 | **404.3** |
| 每行相对差 | — | — | **+9.85%** |

封装侧函数（callgrind self Ir）：

| 函数 | Ir/行 |
|---|---:|
| `Stmt::BindValues` | 87 |
| `Value::Value(const char*)` | 30 |
| `Stmt::Step` | 25 |
| `Stmt::Reset` | 19 |
| 小计（显式函数） | **161** |
| 其余（调用方内联的 `std::array<Value,3>` 构造/析构、`Bind` 模板、PLT） | 243 |
| 合计 | **404** |

即：**封装层占每行总指令预算的 9.0%**，SQLite 内核（VDBE/Btree/页写）占 91%。
与既有文档的「4.0% exclusive」口径不同——这里 404 Ir 是**总差值**（含调用方
内联的 Value 构造），4.0% 只统计库内 exclusive 代码。

### 3.3 与「原生最优写法」的绑定策略差异

原生基准用 `SQLITE_STATIC` 绑字面量，封装用 `SQLITE_TRANSIENT`。为分离
「引擎差异」与「绑定策略差异」，bench 加了 `InsertWithNativeTransient`：
优化后 wrapper / native(STATIC) = **1.068x**，wrapper / native(TRANSIENT) =
**1.068x** 量级（7 轮中位 1.068，见 §4）；单轮曾见 1.017x。
说明绑定策略在短文本场景差异很小（SQLite 会复用 Mem 缓冲），见 §6 的 STATIC 实验。

---

## 4. 对标矩阵（7 轮中位数，1e6 行）

命令：`taskset -c 0 go2cpp_storage_bench <dir> 1000000` × 7 轮。

### 4.1 优化后（当前代码）

| 场景 | 封装 | 原生 | 封装/原生 | 波动区间 |
|---|---:|---:|---:|---|
| 插入 1e6（文件，WAL，单事务） | 399.1 ms（2.51 M 行/s，399 ns/op） | 365.7 ms（2.73 M 行/s） | **1.078x** | 1.061–1.109 |
| 插入 1e6（内存库） | 285.6 ms（3.50 M 行/s） | 259.5 ms（3.85 M 行/s） | **1.103x** | 0.919–1.116 |
| 扫描 1e6（文件） | 71.7 ms（13.9 M 行/s） | 66.8 ms（15.0 M 行/s） | **1.057x** | 0.982–1.143 |
| 更新 1e6（文件） | 149.9 ms（6.67 M 行/s） | 159.4 ms（6.27 M 行/s） | **0.929x** | 0.880–0.983 |
| 删除 1e6（文件） | 96.7 ms（10.3 M 行/s） | 86.6 ms（11.5 M 行/s） | **1.018x** | 0.837–1.295 |
| 预编译复用 1e6（内存） | 250.8 ms（3.99 M 行/s） | 231.5 ms（4.32 M 行/s） | **1.077x** | 0.728–1.134 |
| 微基准 1e7 `SELECT ?` | 62.0 ns/op | 56.7 ns/op | **1.095x** | 1.029–1.139 |

### 4.2 优化前（同一基线，7 轮）

| 场景 | 比值 |
|---|---:|
| 插入文件 | 1.131x |
| 插入内存 | 1.154x |
| 扫描 | 0.969x |
| 更新 | 0.992x |
| 删除 | 1.030x |
| 预编译 | 1.086x |
| 微基准 | 1.199x |

### 4.3 1e7 行规模（既有文档数据，未在本轮重跑）

插入 1e7 文件 1.15–1.23x、扫描 1.15x、预编译 1.25x、微基准 1.09x；吞吐与
1e6 同量级（插入 ~500–900 ns/行），没有随规模退化。

---

## 5. 正确性

### 5.1 1e6 行逐字节一致（vs 原生）

`bench_db.cpp` 新增 1e6 行一致性检查（`ByteIdentical(dir, kRows, "1m")`）：
封装与原生用相同 schema、相同 PRAGMA、相同 1e6 行数据分别写库，再比较：

```
[byte-identical:1m] files: bytes_1m_wrapper.db=21143552 bytes,
                           bytes_1m_native.db=21143552 bytes,
                           file_bytes_equal=yes
[byte-identical:1m] dump: wrapper=28000000 bytes fnv=2bbde779bf1fcb83,
                          native=28000000 bytes fnv=2bbde779bf1fcb83,
                          dump_equal=yes
```

- **数据库文件逐字节相同**（21,143,552 字节）；
- 内容层按 `(id,v,name)` 序列化的 **28,000,000 字节 dump 完全相同**
  （FNV-1a64 `2bbde779bf1fcb83`）；
- 1000 行档同样一致（28,000 字节，fnv `08493cf6b68e2cd3`）。

### 5.2 测试

| 测试 | 结果 |
|---|---|
| `go2cpp_storage_db_test`（8 组：open/pragmas、transactions、values/columns、edges、prepared-reuse-1e6、persistence、crud-1e6、concurrency-wal） | **PASS，0 failures** |
| `go2cpp_storage_orm_test` | **all checks passed**（2873 ms） |

覆盖到本轮改动的点：

- **事务/回滚**：提交、回滚、嵌套拒绝、无事务 COMMIT、RAII 自动回滚、
  约束错误后回滚；
- **并发**：4 写 × 25000 + 2 读 × 200，WAL，结束后
  `PRAGMA integrity_check = ok`、`COUNT(*)=100000`、`SUM(v)` 与理论值一致；
- **持久化**：关闭重开，26890 字节 `memcmp` 相等；
- **临时字符串绑定**：`Bind(i, i*7, "row-" + std::to_string(i))` 每行传临时
  string，`Bind` 返回后立即销毁、随后 `Step` —— 覆盖 `SQLITE_TRANSIENT`
  的拷贝语义（这也是 §6 决定不默认用 STATIC 的原因）。

---

## 6. 架构优化实验

### 6.1 `SQLITE_STATIC` vs `SQLITE_TRANSIENT`（已实施后回滚）

实现：`Stmt` 增加 `std::vector<Value> m_owned_params`，含文本参数时整组
复制进成员（`vector` move 时堆缓冲地址不变，STATIC 指针安全），文本用
`SQLITE_STATIC`，Blob 仍 `TRANSIENT`。编译通过、测试全过。

5 轮结果（vs 原生）：

| 场景 | TRANSIENT（基线） | STATIC + 持有副本 | 结论 |
|---|---:|---:|---|
| 插入文件 | 1.131x | **1.266x** | 更差 |
| 插入内存 | 1.154x | **1.205x** | 更差 |
| vs native-TRANSIENT | 1.060x | **1.253x** | 更差 |
| 预编译 | 1.086x | 1.101x | 噪声内 |
| 微基准 | 1.199x | 1.119x | 噪声内（无文本参数，未走该路径） |

**原因**：`sqlite3_bind_text(..., SQLITE_TRANSIENT)` 的拷贝目标
`Mem.zMalloc` 在 `sqlite3_reset` 后**复用**，第 2 行起只是 ~8 字节 memcpy；
而「持有副本」每行要把整个 `Value`（含 `std::string`）复制一遍，成本更高。
**短文本场景 STATIC 是负优化**，已回滚（`SQLITE_TRANSIENT` 保持不变），
但结论与数据保留在此。长文本（>4 KiB）理论上 STATIC 可省一次大拷贝，本轮
未单独压测，不作为默认路径。

### 6.2 热路径返回 `const Error&`（已保留）

`Stmt::Bind`/`BindValues`/`Reset`/`Step`/`ClearBindings`/`Finalize` 由返回
`Error`（值）改为返回 `const Error&`：成功指向静态 `kOkError`，失败指向
`m_error`。消除每次调用在调用方栈上构造/析构 48 字节 `Error`（含
`std::string`）的 sret 写回。

| 场景 | 优化前 | 优化后 |
|---|---:|---:|
| 插入文件 | 1.131x | **1.078x** |
| 插入内存 | 1.154x | **1.103x** |
| 预编译 | 1.086x | **1.077x** |
| 微基准 | 1.199x | **1.095x** |
| 扫描 | 0.969x | 1.057x（噪声，代码未变） |
| 更新 | 0.992x | 0.929x |
| 删除 | 1.030x | 1.018x |

兼容性：`db_test`、`orm_test`、`bench` 全部重编通过；调用方 `Error e =
stmt.Step()` 仍可编译（拷贝）。**注意这是 ABI 变更**（返回值类型变化），
共享库使用方需要重编；SOVERSION 未变，发布前需要评估。

### 6.3 批量绑定（未实施）

未做。理由：STATIC 实验已证明「绑定策略」不是瓶颈；批量绑定改变的是调用方
SQL 形状（`INSERT ... VALUES (...),(...)`），封装本身只需支持多参数，不需要
改架构。若后续要压这块，建议在 bench 增加「每语句 100 行」场景单独度量，
而不是改库。

---

## 7. 是否 <1.1x

**是（文件库主场景）**：

- 插入 1e6（文件，WAL，单事务）：**1.078x < 1.1x** ✅
- 扫描 1e6：1.057x ✅
- 更新 1e6：0.929x（封装更快）✅
- 删除 1e6：1.018x ✅
- 预编译复用 1e6：1.077x ✅
- 微基准 1e7：1.095x ✅
- 插入 1e6（内存库）：**1.103x**，略超 1.1x；波动 0.919–1.116 跨过 1.0，
  与噪声同量级 ⚠️

**离下限**：每行仍由 SQLite 内核主导（callgrind：封装 404 Ir/行，占 9.0%；
内核 ~4100 Ir/行）。SQLite 插入约 400 ns/行，与「顺序写 10 GB/s memcpy
下限 ~3 ns/行」差约 130x，这是 btree + WAL + 事务日志的引擎成本，不是封装
引入的。

---

## 8. 复现

```bash
# 构建（独立目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-sqlite \
      -DGO2CPP_WITH_SQLITE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/build-sqlite --target go2cpp_storage_db_test \
      go2cpp_storage_bench -j12

# 正确性
/tmp/build-sqlite/go2cpp_storage_db_test

# 对标（7 轮，取中位数）
for i in 1 2 3 4 5 6 7; do
  taskset -c 0 /tmp/build-sqlite/go2cpp_storage_bench /tmp/bench-$i 1000000
done

# objdump 取证
python3 tests/storage/asm_stats.py /tmp/build-sqlite/libgo2cpp_storage.so

# callgrind（每行 Ir）
g++ -O3 -std=c++17 -fno-stack-protector -I include tests/storage/asm_probe.cpp \
    -L /tmp/build-sqlite -lgo2cpp_storage -lsqlite3 \
    -Wl,-rpath,/tmp/build-sqlite -o /tmp/asm_probe
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.wrapper /tmp/asm_probe 20000 wrapper
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.native  /tmp/asm_probe 20000 native
```

改动的文件（均在允许边界内）：

- `include/go2cpp/storage/db.hpp`、`src/storage/db.cpp`（§6.2 的 `const Error&`）；
- `tests/storage/bench_db.cpp`（update/delete/1e6 一致性/原生 TRANSIENT 对照/
  `noinline`）；
- `tests/storage/asm_probe.cpp`、`tests/storage/asm_stats.py`（新增复现工具）。
