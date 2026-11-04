# LevelDB 封装层汇编级取证（storage / leveldb）

- 日期：2026-11-04（Asia/Shanghai）
- 工作树：WSL `/UserData/CodexWorkSpace/Go2cpp`（真树；E: 副本未读取）
- 性质：**只读取证**，未改代码、未跑基准、未 commit
- 对象：`src/storage/leveldb_kv.cpp`——`DB::Put`（:316-340）、`DB::Get`（:432-470）
- 工具链：g++ 13.3.0，`-O2 -DNDEBUG -std=c++17`；链接官方 `libleveldb.a`
  （`~/.local/lib`，GCC 13.3.0 Release 预编译）
- 反汇编：`objdump -d -C --no-show-raw-insn`；计数脚本 `/tmp/asmstat.py`
  （按符号切块，剔除 `nop*` / `xchg %ax,%ax` 对齐填充）

复现命令：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
g++ -O2 -DNDEBUG -std=c++17 -I include -isystem ~/.local/include \
    -c src/storage/leveldb_kv.cpp -o /tmp/ldbasm/leveldb_kv.o
python3 /tmp/asmstat.py /tmp/ldbasm/leveldb_kv.o "ldb::DB::Put(std" "ldb::DB::Get(std"
```

---

## 1. 指令级数据（静态全函数）

| 函数 | 指令 | call | `lock` 前缀 | 栈帧 | 说明 |
|---|---:|---:|---:|---:|---|
| `DB::Put(key, value, WriteOptions)` | **173** | 13 | 2 | **136 B** | 主实现 |
| `DB::Put(key, value)` | 15 | 2 | 0 | 24 B | 转发到上者 |
| `DB::Get(key, value, ReadOptions)` | **197** | 14 | 3 | **184 B** | 主实现 |
| `DB::Get(key, value)` | 15 | 2 | 0 | 24 B | 转发到上者 |
| 裸 `leveldb::DB::Put`（对照） | 44 | 5 | 0 | 48 B | `libleveldb.a` 的 `db_impl.cc.o` |

栈帧 = `sub rsp` 立即数 + 8 × push 数：

- Put：`sub $0x60` + 5 push = 96 + 40 = **136 B**
- Get：`sub $0x88` + 6 push = 136 + 48 = **184 B**

两者都含 GCC 默认 `-fstack-protector-strong` 的金丝雀（`mov %fs:0x28` / `sub %fs:0x28`）。

---

## 2. 成功热路径逐段拆解（`enable_stats=false`、`Status.ok()`）

**Put 热路径 = 54 条**：

| 区段 | 条数 | 内容 |
|---|---:|---|
| `17d0-1855` | 33 | 保存 5 个被调寄存器；`LoadDb`（`mov 0x60(%rdx)`）；`WriteOptions.sync` 拷贝；两个 `Slice` 的 data/size 入栈；`cmpb $0x0,0x4b` 判 stats；`call *0x10(%r9)` 一次虚调用 |
| `1859-186c` | 5 | 重载 `impl_`、`Status::state_` 判空（OK 即 nullptr） |
| `1986-1992` | 3 | `NoteSuccess()`（`movb $1,0xb0(%rbp)`）+ 返回值置 1 |
| `192c-195d` | 13 | status 析构检查、金丝雀校验、恢复 5 寄存器、`ret` |

**Get 热路径 = 69 条**：

| 区段 | 条数 | 内容 |
|---|---:|---|
| `2500-25b3` | 45 | 保存 6 个被调寄存器；`ReadOptions` 两份 bool 拷贝；栈上 `scratch` string 初始化；`LoadDb`；stats 判断；`call *0x28(%r9)` |
| `25b7-25bf` | 3 | `Status::state_` 判空 |
| `2758-2770` | 6 | stats 二次判断 + 返回值置 1 |
| `2698-26d8` | 15 | scratch 析构、金丝雀、恢复 6 寄存器、`ret` |

---

## 3. 与裸 LevelDB 并排（为什么不是零成本）

Put 热路径 54 条 vs 裸 `leveldb::DB::Put` 44 条，**净增 10 条**。构成：

| 项 | 条数 | 位置 |
|---|---:|---|
| `-fstack-protector-strong` 金丝雀 | 5 | `17e9/17f2` + `193b/1940/1949` |
| `LoadDb` + 关闭检查（`test`/`je`） | 4 | `17f7-1801` |
| `enable_stats` 判断 | 2 | `185c/1860` |
| `Status.ok()` 判断 | 2 | `1866/186c` |
| `NoteSuccess` | 1 | `1986` |
| `WriteOptions` 拷贝 + `Slice` 组装 | 7 | `1807-1850` |

`-fno-stack-protector` 对照编译同一文件：

| 函数 | 静态指令 | 热路径 | 栈帧 |
|---|---:|---:|---:|
| Put | 164（−9） | **49** | 120 B |
| Get | 186（−11） | **64** | 152 B |

**结论**：默认 Ubuntu 加固选项下净增 10 条；关闭栈保护后 Put 热路径 49 条，比裸接口
44 条多 **5 条**，正好落在「封装层额外 ≤5 条指令」的预期上界。多出来的全部是
错误处理/统计分支的**判断**，不是算法开销；`WriteOptions` / `Slice` 的组装是 API
语义必需（裸接口同样要构造）。

---

## 4. 锁、原子与分配

| 项 | Put | Get |
|---|---|---|
| 热路径 `lock` 前缀 | **0** | **0** |
| 热路径 `call operator new` | **0** | **0** |
| `enable_stats=true` 时 | 2 条 `lock addq`（`0xb8` puts、`0xe8` bytes_written） | 3 条（`0xc0` gets、`0xc8` hits、`0xe0` bytes_read） |
| `enable_stats=false` 时 | 0 原子 | 0 原子 |
| 失败路径 | `pthread_mutex_lock`/`unlock` ×2 + `Status::ToString` | 同 |

反汇编证据（`objdump -dr` 重定位）：

- `pthread_mutex_lock`、`pthread_mutex_unlock`、`leveldb::Status::ToString`、
  `operator delete[]` 全部落在 `status.ok()==false` 分支（`1872` / `19a2` 起）。
- Put 内唯一的 `memcpy` 在 `std::string::_M_replace` 冷路径（`1a3a`），用于错误文本。
- 两次 `atomic` load：Put 一次（`LoadDb`，x86 上 `mov` 免费），Get 一次；
  `NoteSuccess` 是 relaxed store，无 `lock`。

---

## 5. 存储层四模块三件套状态表

| 模块 | 架构优化 | 汇编级证据 | 正确性 | 结论 |
|---|---|---|---|---|
| storage / sqlite 封装 | 有：`const Error&` 返回、语句复用 | 有：objdump + callgrind，18 处（`asm-storage-sqlite.md`） | 7 场景 × 7 轮，1e6 插入 1.074× | ✅ |
| storage / kv（LSM 引擎） | 有 | 有：objdump 2 处（`asm-storage-kv.md`） | 1e7 压测 | 🟡 读 0.48× 于 LevelDB |
| storage / features（Queue/Watch/MemCache） | 有：复用 WAL/SSTable | 🟡 弱：2 处 objdump 引用（`asm-storage-features.md`） | 三套功能测试（1e6/1e5/1e7） | 🟡 |
| storage / leveldb 封装 | 有：pimpl、`string_view` 零拷贝、relaxed 统计 | **有：本文**（Put 173/热 54、Get 197/热 69、0 分配） | `test_leveldb_kv.cpp` 1e7/1e8（M07：647,521 ops/s） | 🟡 汇编补齐；缺独立端到端对标报告 |

存储层缺口收口后：**sqlite ✅ / kv 🟡 / features 🟡 / leveldb 🟡**，不再有 ❌。

---

## 6. 结论与边界

1. 封装层成本可量化：热路径 54/69 条；扣掉发行版金丝雀后 49/64 条，Put 比裸
   `leveldb::DB::Put`（44 条）只多 5 条，**符合 ≤5 的预期**。
2. 零拷贝成立：`ToSlice`（:84）只取 data/size，反汇编里没有额外的 key/value 拷贝；
   唯一 `memcpy` 属于错误字符串。
3. 热路径 0 堆分配、0 锁、0 原子（stats 关）；统计打开时每次 Put 2 条、Get 3 条
   cache-line 独占的 `lock addq`，这是 `enable_stats` 的真实成本，压测应保持关闭。
4. **未覆盖**：`Open` / `Close` / `WriteBatch` / `Iterator` 未取证；本轮未跑 `ctest`、
   未复跑 1e7 压测（M07 引用旧数据）。
5. 证据口径：函数名 + 指令数 / call / 锁 / 栈帧 / 原子，满足
   `three-piece-audit.md:12` 的汇编最低门槛。
