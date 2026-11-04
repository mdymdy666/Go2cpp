# 存储（storage）

> **一句话定位**：五类持久化能力——SQLite 连接/ORM、LSM 风格 KV、LevelDB 对照、
> 顺序队列、前缀 Watch、分片 MemCache。
> 深入文档：[存储模块 · 深入学习](../../deep/04-storage.md) ·
> 源码 [`include/go2cpp/storage/`](../../../include/go2cpp/storage/)、
> `src/storage/`

## 能力清单

| 能力 | 关键 API | 编译开关 |
| --- | --- | --- |
| SQLite 连接与执行 | `DB::Open/Exec/Query/Prepare/Begin/Commit/Rollback` | `GO2CPP_WITH_SQLITE`（默认 OFF） |
| ORM / 迁移 | `Session::Find/All`、`Query<T>::All/First`、`Migration::Up/Down` | `GO2CPP_WITH_SQLITE` |
| LSM 风格 KV | `KV::Open/Put/Get/Delete/Write/NewIterator/Flush` | `GO2CPP_WITH_KV`（默认 ON） |
| LevelDB 对照实现 | `ldb::DB::Open/Put/Get/NewIterator/Flush` | `GO2CPP_WITH_LEVELDB`（默认 OFF） |
| 顺序队列 | `Queue::Append/Consume/Ack/FlushCursors/Sync` | 随 KV |
| 前缀 Watch | `Watcher::Open/Watch/Put/Delete/Notify/Replay` | 随 KV |
| 分片 MemCache | `MemCache::Set/Get/Peek/Delete/Expire/PurgeExpired` | 随 KV |

> 选型速记：要 SQL/事务用 `DB`+`ORM`；要「写多读少、有序扫描」用 `KV`；
> 要「多消费者、游标确认」用 `Queue`；要「订阅变更」用 `Watcher`；
> 要「进程内 TTL 缓存」用 `MemCache`（需要持久化就用 `KV`）。

## 最小示例

KV 是默认开启、依赖最少的一条路径（`KV::Open` 自动恢复 WAL）：

```cpp
#include "go2cpp/storage/kv.hpp"
#include <iostream>

int main() {
    using go2cpp::storage::KV;
    auto db = KV::Open("/tmp/go2cpp-kv-demo");
    if (!db) { std::cerr << "open failed\n"; return 1; }

    if (!db->Put("greeting", "hello")) return 1;   // 写 MemTable + WAL
    std::string value;
    if (db->Get("greeting", &value)) std::cout << value << '\n';
    db->Delete("greeting");
    db->Close();                                    // Flush + WAL fsync
    return 0;
}
```

编译（头文件 + 运行时库）：

```sh
g++ -std=c++17 -I include user/modules/storage/README-snippet.cpp \
    build/libgo2cpp_runtime.a -lpthread -o /tmp/kv_demo
```

> 片段仅用于说明 API；仓库没有为 storage 单独注册 user 示例（见下节）。

## 3 个常见异常

### 1. `KV::Open` / `DB::Open` 返回空
- **现象**：程序启动即失败，拿不到句柄。
- **原因**：路径不可写、目录不存在、文件被另一个进程独占，或对应编译开关没打开
  （SQLite 默认 OFF，LevelDB 默认 OFF）。
- **处理**：先看 `LastError()`；确认进程有写权限；确认构建时 `-DGO2CPP_WITH_SQLITE=ON`。
  KV 是默认 ON 的，只有 SQLite/LevelDB 需要显式打开。

### 2. 事务没提交，进程退出后数据消失
- **现象**：`Exec("INSERT ...")` 明明成功，重开数据库查不到。
- **原因**：`Begin` 之后必须显式 `Commit`；析构**不会**自动提交，异常路径上很容易漏。
- **处理**：用 RAII 守卫（析构时 `Rollback`，正常路径 `Commit`），或每步都检查 `Error`
  并在失败分支立刻 `Rollback`。

### 3. 关库时丢最后一批写
- **现象**：`Put` 返回 true，`kill -9` 后部分 key 不见了。
- **原因**：`KV::Put` 只保证进 WAL，是否 fsync 取决于刷盘策略；`Close()` 才做
  Flush + WAL fsync。
- **处理**：正常退出路径必须 `Flush()`（或 `Close()`）；对可靠性要求高的场景
  每条写后 `Flush`，代价是吞吐下降。

## 示例清单

本目录已提供 `01-write-failure.cpp` / `02-read-miss.cpp` 两个可运行异常示例；其余可运行代码在：

| # | 位置 | 场景 |
| ---: | --- | --- |
| 01 | `tests/storage/kv_test.cpp` | KV 基本读写、覆盖、删除、迭代器 |
| 02 | `tests/storage/kv_recovery_test.cpp` | 崩溃恢复：WAL 重放与 SSTable 合并 |
| 03 | `tests/storage/kv_concurrent_test.cpp` | 多线程并发读写边界 |
| 04 | `tests/storage/memcache_test.cpp` | 分片、TTL、过期清理 |
| 05 | `tests/storage/bench_db.cpp`、`kv_bench.cpp` | 基准与容量曲线 |
| 06 | [`../app/cache_demo/`](../app/cache_demo/) | 进程内缓存用法（缓存策略见 cache 模块） |

运行方式：`cmake --build build --target go2cpp_tests -j && ./build/go2cpp_tests --gtest_filter='*kv*'`
（测试框架以仓库实际注册名为准，见 `cmake/Go2CppStorage.cmake`）。

## 深入阅读

- [存储模块 · 深入学习](../../deep/04-storage.md)（五层地图、逐函数、失败路径）
- [交接摘要](../../docs/04-storage.md)
- 编译开关：`cmake/Go2CppStorage.cmake`、`cmake/Go2CppKv.cmake`、`cmake/Go2CppLevelDb.cmake`

## 边界与注意

- **`DB` 不是连接池**：一个 `DB` 对象对应一个 SQLite 连接，多线程共享要自己加锁或每线程一个。
- **KV 是单写者模型**：并发写由内部锁串行化，但批量场景应使用 `WriteBatch` 降低 WAL 次数。
- **`Queue` 的游标要显式推进**：`Consume` 之后必须 `Ack`，否则重启后会重复消费（至少一次语义）。
- **`MemCache` 是进程内缓存**：不持久化、不跨进程，重启即失效；需要共享就用 KV 或外部缓存。

## 异常处理示例（本目录，可编译运行）

| # | 文件 | 真实场景 | 默认行为（file:line） | 可选策略 |
| ---: | --- | --- | --- | --- |
| 01 | [`01-write-failure.cpp`](01-write-failure.cpp) | 磁盘满 / 目录只读 / 权限不足，写不进去 | `Put()` 返回 **false** + `LastError()`（`include/go2cpp/storage/kv.hpp:135`、`143-144`）；Flush 创建 SSTable 失败记 `open sstable failed`（`src/storage/kv.cpp:846-849`）；WAL 失败记 `wal append failed`（kv.cpp:648） | 重试（恢复后最多 3 次）、降级到 `MemCache`、告警 + 健康检查降级 |
| 02 | [`02-read-miss.cpp`](02-read-miss.cpp) | 读未命中：缓存穿透 / 记录不存在 | `Get()` 返回 **false**、`value` **保持原值不变**、`LastError()` 保持空（kv.hpp:136；kv.cpp:769-821）；`MemCache::Get` 同理（`memcache.hpp:80-81`） | 回源上游、L1 `MemCache` + L2 `KV` 两层、未命中明确返回 404 |

### 编译与运行

前置：项目已构建过一次（产生 `.build-bento-wsl/libgo2cpp_kv.so`）。
用 `build/` 目录时把 `-L/-rpath` 换成对应路径即可。

```sh
cd /UserData/CodexWorkSpace/Go2cpp
FLAGS="-std=c++20 -O2 -Wall -Wextra -I include"
LIB="-L .build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl -lgo2cpp_kv -lpthread"

g++ $FLAGS user/modules/storage/01-write-failure.cpp $LIB -o /tmp/storage01 && /tmp/storage01
g++ $FLAGS user/modules/storage/02-read-miss.cpp    $LIB -o /tmp/storage02 && /tmp/storage02
```

两个示例都在 `/tmp` 下用 `mkdtemp` 建临时目录，退出自动清理；不注册 ctest、不占端口。

### 实测输出摘要

```
# 01（约 0.1s）
[正常] 3 条写入成功（memtable_size=8KiB）
[默认] 写失败 failed=1 at=1 LastError="open sstable failed: /tmp/.../000002.sst"
[重试] 恢复权限后 attempt 重试成功=1
[降级] 写内存缓存 degraded=1 hit=1 value={"status":"pending"}
[告警] write_failures=1 first_reason="open sstable failed: ..." health=degraded
[write-failure] 通过

# 02（约 0.05s）
[默认] found=0 value="sentinel" last_error=""（未命中不是错误）
[回填] product:1 第一次 source=kv 第二次 source=cache
[回源] product:2 第一次 source=upstream 第二次 source=cache
[报错] product:404 status=not_found value=""
[统计] cache hits=2 misses=3 upstream_queries=2
[read-miss] 通过
```

> 01 需要**非 root** 运行（root 有 `CAP_DAC_OVERRIDE`，`chmod 0555` 拦不住；
> 示例会检测并改用「关闭后写入」的 `closed` 错误码兜底）。02 对运行用户无要求。

### 选择建议

- 强一致数据（订单/支付）→ 重试 + 告警 + 返回 5xx，**不要**只降级到内存；
- 可丢/可重放数据（指标/日志/会话）→ 降级 `MemCache` + 后台补偿；
- 读未命中是正常业务分支 → 返回 `not_found`（404），必要时缓存空值防穿透；
- 底层读失败（`LastError` 非空）→ 返回 5xx，**不要**降级成 404。
