# 缓存三件套汇编级取证（cache：LRU / 2Q / TinyLFU）

- 日期：2026-11-04（Asia/Shanghai）
- 工作树：WSL `/UserData/CodexWorkSpace/Go2cpp`（真树；E: 副本未读取）
- 性质：**只读取证**，未改代码、未跑基准、未 commit
- 对象：`include/go2cpp/cache/{lru,two_queue,tiny_lfu}.hpp`（header-only 模板）
- 实例化来源：`.build-bento-wsl/CMakeFiles/go2cpp_cache_test.dir/tests/cache/*.o`
  （`K=int, V=int`，`-O3 -DNDEBUG -std=c++17 -O2`，后者生效；GCC 13.3.0）
- 反汇编：`objdump -d -C --no-show-raw-insn`；计数脚本 `/tmp/asmstat.py`
  （剔除 `nop*` / `xchg %ax,%ax` 对齐填充）

复现命令：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
python3 /tmp/asmstat.py \
  .build-bento-wsl/CMakeFiles/go2cpp_cache_test.dir/tests/cache/test_lru.cpp.o \
  "LruCache<int, int"
python3 /tmp/asmstat.py \
  .build-bento-wsl/CMakeFiles/go2cpp_cache_test.dir/tests/cache/test_two_queue.cpp.o \
  "TwoQueueCache<int, int"
python3 /tmp/asmstat.py \
  .build-bento-wsl/CMakeFiles/go2cpp_cache_test.dir/tests/cache/test_tiny_lfu.cpp.o \
  "TinyLfuCache<int, int"
```

---

## 1. 指令级数据

| 实现 | 函数 | 指令 | call | `lock` | 栈帧 |
|---|---|---:|---:|---:|---:|
| **LruCache** | `Get` | **73** | 0 | 0 | **0 B** |
| | `Put` | **227** | 4 | 0 | 88 B |
| | `Erase` | 95 | 2 | 0 | 24 B |
| **TwoQueueCache** | `Get` | **373** | 6 | 0 | 56 B |
| | `Put` | **251** | 7 | 0 | 72 B |
| | `ReclaimForInsert`（淘汰入口） | 287 | 6 | 0 | 72 B |
| **TinyLfuCache** | `Get` | **306** | 2 | 0 | 56 B |
| | `Put` | **420** | 8 | 0 | 88 B |
| | `AdmitToMain`（晋升+淘汰） | 346 | 5 | 0 | 104 B |

栈帧 = `sub rsp` 立即数 + 8 × push 数（例如 Lru `Put`：`sub $0x28` + 6 push = 40 + 48 = 88 B）。

**Evict 没有独立符号**：三者都把 `EvictTail` / `EvictAmTail` / `EvictA1inTail` /
`EvictProbationTail` 内联进唯一调用点（`Put` 或 `ReclaimForInsert` / `AdmitToMain`），
`nm` 里查不到 `Evict*` 符号。表里的 227 / 287 / 346 是包含淘汰循环的完整入口函数。

---

## 2. 逐实现解读

### 2.1 LruCache：Get 73 条、0 栈帧、0 调用

`Get`（`lru.hpp:90-105`）的完整路径：

1. 64 位 mix 哈希（`0x9e3779b97f4a7c15` 起点 + 两次 `imul`/`shr`/`xor`）——约 12 条；
2. `FlatMap` 线性探测：`and 0x40(%rcx)` 取槽位、`mov (%r8,%rax,8)` 取节点、比较
   `0x20(%rax)`/`0x24(%rax)` 命中判断；
3. `move_to_front`：4 个指针写（`prev->next`、`next->prev`、`head`、`size`）；
4. 可选 value 拷贝（`test %rdi,%rdi` 后一条 `mov`）；
5. `stats_.RecordHit()`：一条 `addq $1,0xa8(%rcx)`。

**0 次 call、0 B 栈帧**：整条命中路径全在寄存器内完成，没有函数调用、没有栈对象。
73 条里包含未命中分支（`e8` 处 `RecordMiss` + `xor eax,eax` + `ret`）。

`Put` 227 条，4 个 call 的落点是：

| 重定位符号 | 触发条件 |
|---|---|
| `operator new(unsigned long)` | `NodePool` 首次/每 64 个节点扩块，**不是每次 Put** |
| `std::vector<...>::_M_realloc_insert` | 节点块表扩容，同样按 64 节点摊销 |
| `__stack_chk_fail` | 金丝雀失败（不可达） |

稳态（容量满、自由表非空）下 `Put` 走 `pool_.Create` 的自由表分支，**0 堆分配**；
这与 `lru.hpp:12` 的「插入 0 分配」一致，反汇编给出了对应证据。

### 2.2 TwoQueueCache：Get 373 条 = 两次哈希

`Get`（`two_queue.hpp:108-140`）先查 `am`、未命中再查 `a1in`、ghost 命中只置
`pending_` 标记。两次独立哈希 + 两次线性探测把指令数推到 373，是 LRU 的 5.1 倍；
这是 2Q 用「多一次查找」换 scan resistance 的必然成本，不是退化。

`Put` 251 条，比 `Get` 少：更新分支不查 ghost；插入分支把淘汰集中到
`ReclaimForInsert`（287 条，含 `EvictA1inTail` → `EvictAmTail` 两级回收）。

### 2.3 TinyLfuCache：Get 306 条 = 4 路素描

`Get`（`tiny_lfu.hpp:121-180`）除哈希与链表外，还要更新 4 路 count-min sketch
（4 次索引 + 4 次比较 + 年龄衰减判断），所以是三者中最重的 Get。`Put` 420 条与
`AdmitToMain` 346 条共同承担 window → probation → protected 的晋升与淘汰。

---

## 3. 并发与分配口径

- 三者 `Get`/`Put`/`Evict` 的 `lock` 前缀均为 **0**：缓存接口非线程安全，
  由调用方（factory 分片锁 / 外部 mutex）保护，`lru.hpp:86-88` 已写明。
- 栈帧：Lru `Get` 为 **0 B**，其余 24–104 B，均为保存被调寄存器与金丝雀，
  没有大栈对象或动态数组。
- 分配：唯一 `operator new` 在 `NodePool` 扩块路径；淘汰 `Destroy` 只把节点挂回
  自由表，不调用 `delete`，因此稳态无分配/释放。

---

## 4. 正确性（已有，未复跑）

| 测试 | 覆盖 |
|---|---|
| `tests/cache/test_lru.cpp` / `test_two_queue.cpp` / `test_tiny_lfu.cpp` | 逐条命中率与淘汰顺序对拍 |
| `tests/cache/test_interface.cpp` | 统一 `Cache` 接口语义 |
| `tests/cache/test_scenarios.cpp` / `test_stress.cpp` | 混合负载与并发场景 |
| `tests/cache/cache_bench.cpp` | 命中率与吞吐（`--quick` / `--full`） |

本轮未运行 `ctest`，只引用既有测试文件的存在性与审计报告中的「命中率逐位一致」结论。

---

## 5. 结论

| 项 | 判定 |
|---|---|
| 架构优化 | 有：侵入式双向链表 + 开放寻址 `FlatMap` + 64 节点块池（`lru.hpp:1-21`） |
| 汇编级证据 | **有：本文**（9 个函数，指令/call/锁/栈帧齐全） |
| 正确性 | 有：命中率逐位一致对拍 + 场景/压力/bench |
| 综合 | **✅**（A39 由 🟡 升 ✅） |

边界：实例化类型是 `int/int`，生产用 `string` 时哈希与 value 拷贝会更重，但容器层
（哈希、探测、链表、淘汰）的指令结构不变；未覆盖 ARC/GDSF/CLOCK 与 `lru_k`。
