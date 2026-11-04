# 分布式 P1：MetadataStore + Coordinator 快照持久化

> 日期：2026-11-04 · 分支 `feat/isolated`（HEAD `0b21ffc`，P0 `99ed212` 之上）
> 范围：`include/go2cpp/distributed/metadata_store.hpp`、`src/distributed/{metadata_store,coordinator_snapshot}.cpp`、
> `src/distributed/snapshot_codec.hpp`、`coordinator.{hpp,cpp}` 增量、两个新测试。
> 约束：未改 `transport.cpp`、未改 rpc、未 commit。

---

## 1. 结论

P1 要修的唯一静默正确性问题是「协调者重启后 fencing 计数回退」。本轮把它落成两条机制：

1. **Coordinator 快照**：fencing 表（lock → 已授予最大 token）、租约表、选举 epoch
   在每次影响单调性的写返回前落盘；重启后 `m_fencing` 从快照恢复，新 token 严格大于旧值；
2. **fail-stop**：快照存在但损坏（魔数/版本/CRC/长度/记录 kind 任一不符）→ 构造期
   `healthy() == false`、`load_error()` 非空、所有写操作返回 `kUnavailable`。
   不会静默退化成「从 0 开始计数」——那正是被修的 bug。

实测：旧 token=1 → 重启 → 新 token=2；再重启 → 3。截断快照 → 拒绝启动。
10 个 distributed 测试全部通过（含既有 8 个回归）。

---

## 2. 架构

### 2.1 MetadataStore（`metadata_store.hpp`）

| 项 | 决定 |
|---|---|
| 接口 | `Open/OpenMemory`、`Get/List`、`Put/Delete`（返回 revision）、`Watch(prefix, cb, stable_id)`、`Unwatch`、`Replay(id, from_revision)`、`Flush`、`GetStats` |
| revision | 全局单调，每次成功写 +1；先落 WAL 再改内存，返回成功 ⇔ 崩溃后可读回 |
| 后端 | 内存 map（`std::map`，有序支持前缀 List）+ 可选 WAL 文件（追加 + CRC32 + 截断恢复） |
| 派发 | 默认异步单线程队列（严格 revision 顺序，回调内可重入写）；`sync_callbacks=true` 时写线程内回调（不可重入） |
| 所有权 | `Result<unique_ptr<MetadataStore>>` 工厂；不可拷贝/移动；WAL fd 与派发线程由实例独占 |
| 线程安全 | `m_mutex`（map/revision/订阅）、`m_queue_mutex`（队列/计数）、`m_dispatch_mutex`（串行化回调）；锁序 `m_mutex → m_queue_mutex`，回调在锁外执行 |
| 失败语义 | 参数非法 `kInvalidArgument`；WAL 写失败 `kUnavailable`（内存未改）；中间记录损坏 `kProtocolError`（拒绝打开）；尾部撕裂 → 截断后成功 |

**为什么自带 WAL 而不是直接用 `storage::KV`/`storage::DB`**：两者是独立可选目标
（`go2cpp_kv`、`go2cpp_storage`=SQLite，后者默认 OFF），`distributed` 目前不链接存储层；
硬引入会把 SQLite/KV 变成分布式模块的强制依赖并破坏 standalone 构建。本实现复刻
`storage::Watcher` 的语义（前缀订阅 + 游标重放 + 同步/异步派发 + 有界历史），格式自描述。

### 2.2 Coordinator 快照

| 项 | 决定 |
|---|---|
| 入口 | `CoordinatorOptions::{snapshot_path, snapshot_on_write, snapshot_fsync}`；`Restore()` / `Snapshot()` / `healthy()` / `load_error()` / `persist_error()` |
| 恢复语义 | 恢复 fencing/epoch/`m_next_lease`；**不恢复租约持有权**（旧持有者一律失效，客户端重新抢锁拿到旧 token+1） |
| 写放大控制 | 只在「fencing++、分配新 lease id、删除租约、epoch 变化」时置 dirty；续约/查询零 I/O |
| 落盘点 | 授予类写（RegisterService/AcquireLock/JoinElection/ReleaseLease/SweepExpired）返回前；析构再尽力写一次 |
| 并发 | `m_snapshot_mutex` 串行化写者；序列化在 `m_mutex` 内完成并清 dirty；写盘在锁外，不阻塞读路径 |
| 失败 | 写盘失败 → dirty 置回、`persist_error()` 记录、本次调用返回 `kUnavailable`（token 已消耗，只多不少）；加载失败 → fail-stop |

### 2.3 快照格式（版本 1，全大端）

```
0    8   魔数 "G2CSNP01"
8    2   版本 = 1
10   2   保留（写 0）
12   4   记录数 N
16   ... N 条记录（kind u8 + payload）
末尾 4   CRC32（覆盖除 CRC 外全部字节）

kind 1 fencing：u32 key + u64 token
kind 2 lease  ：u64 id + u8 kind + u64 ttl_ms + u64 fencing + u64 deadline_unix_ms
                + 四个 u32 长度前缀字符串 name/instance/owner/endpoint
kind 3 election：u32 name + u64 epoch
```

`deadline_unix_ms` 是 steady_clock deadline 换算出的墙钟时间，仅审计用（恢复不重建租约）。

### 2.4 fsync 策略

`snapshot_fsync=true`（默认）：写 `<path>.tmp` → `fsync(file)` → `rename(tmp, path)` →
`fsync(目录)`。rename 保证原子替换（读者不会看到半个文件），两次 fsync 保证
「Snapshot 返回成功 = 掉电后仍在」。关闭 fsync 时只剩 rename 原子性，崩溃可能退回
上一份快照（fencing 仍单调，但可能少推进一步）。

---

## 3. 汇编（`objdump -d --demangle`，-O2 Release，libgo2cpp_distributed.so）

| 函数 | 指令数 | call 数 |
|---|---:|---:|
| `Coordinator::Snapshot()`（含内联序列化 + 原子写文件） | 1050 | 73 |
| `Coordinator::Restore()`（含内联 CRC 校验/解析） | 2533 | 108 |
| `Coordinator::SerializeSnapshotLocked()` | 0（内联进 Snapshot） | — |
| `Coordinator::PersistAfterWrite()` | 72 | 6 |
| `Coordinator::EnsureWritable()` | 138 | 11 |
| `Coordinator::AcquireLock(...)` | 662 | 79 |
| `Coordinator::RegisterService(...)` | 644 | 75 |

以上为**静态指令数**（含错误/冷分支），不是动态执行数；`Snapshot()` 路径的符号引用为
`open×2 + write×1 + fsync×2 + close×3 + rename×1 + unlink×1 + pthread_mutex_{lock×4,unlock×3}`。

**实测耗时**（WSL2，200 次，100 把锁 + 10 个选举，快照 9190 字节）：

| 场景 | 每次耗时 |
|---|---:|
| `Snapshot()` + fsync | 2718 µs |
| `Snapshot()` 无 fsync | 194 µs |
| `Restore()`（构造，fsync off 快照，不含析构写盘） | 163 µs |

结论：fsync 成本（~2.5ms/次）比序列化（~0.19ms）高一个数量级，是快照写入的绝对大头；
WSL2 的 fsync 本身偏慢，裸机应显著更低。`snapshot_on_write=true` 把这一次 fsync 放到
**抢锁成功路径**上（正确性优先）；高频抢锁场景可设 `false` 或改为按 key 增量 WAL（未决项）。

---

## 4. 正确性（真实输出）

```
$ ctest --test-dir .build-bento-wsl -R 'distributed_(...)' 
100% tests passed, 0 tests failed out of 10

restart fencing: old token=1 new token=2 (monotonic=yes)
second restart fencing: prev=2 new=3
election epoch: old=1 (new epoch checked > old)
truncated snapshot rejected: snapshot crc mismatch
```

新用例覆盖：

| 文件 | 用例 |
|---|---|
| `tests/distributed/test_metadata_store.cpp` | KV 往返、revision 单调（100 次严格 +1）、前缀 List、Watch 顺序/过滤、回调异常隔离（`callback_errors>=2` 且后续写成功）、`Replay(from=2)` 补发 rev2/rev3、历史窗口外 `kNotFound`、WAL 重启（revision=3 续接）、尾部截断恢复（`truncated_bytes>0`）、中间损坏拒绝（`kProtocolError`） |
| `tests/distributed/test_coordinator_snapshot.cpp` | 重启 token 单调（1→2→3）、不同 key 独立计数、epoch 恢复并只升不降、无快照文件正常启动、截断快照拒绝（`healthy=false` + `AcquireLock kUnavailable`）、CRC 翻转拒绝（`load_error` 含 "crc"） |

编译：`-Wall -Wextra -Wpedantic` 全量构建零告警；三份新实现另以
`-O2 -Wall -Wextra -Wpedantic -Werror` 单独编译通过（`WERROR_CLEAN`）。

---

## 5. 未决项（如实）

1. **活租约不恢复**：重启后旧租约一律失效（客户端幂等重注册）。若要「重启不中断持有」，
   需要把 wall-clock deadline 恢复进 `m_leases` 并加 `restore_leases` 选项；当前格式已存
   `deadline_unix_ms`，缺的是恢复策略与时钟回拨处理。
2. **全量快照而非增量**：每次落盘重写整表（9KB 测试规模）；大规模锁表应改为按 key 增量
   WAL（或 fencing 单独 WAL + 周期性全量压缩）。
3. **fsync 成本**：2.7ms/次落在抢锁路径；生产可调 `snapshot_fsync=false` 或批量提交，
   代价是崩溃可能退一步（token 仍单调）。
4. **未接入 `metrics`**：快照次数/失败次数尚未注册为指标（P0 的 metrics 注册表可扩展）。
5. **MetadataStore 未复用 `storage::KV` 实例**：语义对齐但代码独立；若后续 `distributed`
   链接 `go2cpp_kv`，可替换 WAL 后端而保持接口不变。
6. **单进程假设**：快照文件无文件锁，同一 `snapshot_path` 不得被两个协调者同时打开。
