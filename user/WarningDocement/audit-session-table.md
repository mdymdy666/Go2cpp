# session_table 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/session_table.hpp`、`src/bento/net/session_table.cpp`、
  `tests/bento/test_session_table.cpp`
- 压测工具：`tests/bento/audit_session_table_stress.cpp`（独立程序，不注册 CTest；
  刻意不 include `test_util.hpp`，以免被 `link.hpp` 污染依赖）
- 构建：`.build-bento-wsl`（Ninja，CMAKE_BUILD_TYPE=Release，`-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，GCC 13.3，12 vCPU
- 结论：**无 P0**。修复 1 个 P1 性能缺陷（写路径 O(n) 全表扫描，Touch **18.8×**、
  AddClient **12.1×**）+ 1 个 P1 边界（租约加法溢出回绕）；补 9 个测试用例
  （135 → **355–356 checks**，并发用例断言数随表状态浮动）；P2 记录 8 项。`ctest -R bento` **48/48 通过**
  （`bento_netem` 按设计 SKIP，与本次无关）。

## 1. 压测数据

### 1.1 百万级操作吞吐（同程序、同机器，优化后 3 次取中位；优化前 2 次）

| 操作 | 规模 | 优化前 ns/op | 优化后 ns/op | 变化 |
|---|---|---|---|---|
| `AddServer` | 1024 key/表 × 977 表 ≈ 100 万 | 1684.0 | **177.0** | **9.5×** |
| `AddClient` | 1024 key × 4 client × 245 批 ≈ 100 万 | 3562.6 | **294.9** | **12.1×** |
| `Find` 命中 | 1024 key 稳态，100 万 | 79.0 | 77.1 | 1.02× |
| `Find` 未命中 | 100 万 | 43.5 | 39.9 | 1.09× |
| `List`（8 key） | 100 万 | 138.0 | 128.9 | 1.07× |
| `List`（1024 key） | 1 万次（每次 1024 条） | 28046.7 | 28144.5 | 持平 |
| `Touch` | 1024 key 稳态，100 万 | 3589.5 | **191.2** | **18.8×** |
| `Tick` 空转 | 100 万 | 23.7 | **6.3** | **3.8×** |
| `Tick` 全过期 | 1024 条/轮 × 976 轮 | 1739.7 | **197.0** | **8.8×** |
| 混合读写 | 1024 key，100 万 | 7472.9 | 4981.6 | 1.50× |

- 合计约 800 万次被测操作，AddServer/AddClient/Find/List/Tick 每类均 ≥100 万。
- 读路径（Find/List）优化前后持平是预期：它们本来就是 O(log n)，成本在值拷贝。
  `List(1024)` 单次 28 µs 是「1024 个 `SessionEntry`（含 key 字符串 + clients
  vector）全部拷贝」的固有成本，见 P2-2。
- 混合读写只提升 1.5×，因为其中 1/6 是 `List`（单次 28 µs），摊薄了其余操作。
- 基线二进制保留在 `.build-bento-wsl/audit_session_table_stress`（优化前）与
  `..._opt`（优化后），可直接复跑对比。

### 1.2 内存与泄漏

| 项目 | 结果 |
|---|---|
| 压测全程 RSS | 3.7 → 4.2 MiB（峰值 4.2 MiB，delta 512 KiB） |
| 过期索引额外占用 | 1024 key ≈ +64 KiB（`set<pair<deadline,string>>` 节点） |
| 60s 长稳 RSS | 3712 KiB（起始）→ 3840 KiB（峰值）→ 3968 KiB（结束），**+256 KiB** |
| 60s 长稳（全量流程内复跑） | 17,249,496 ops，RSS 恒定 4480 KiB（起始=峰值=结束） |
| valgrind（4.3 万轮增删 + 长驻表反复扩容） | 43,574 allocs / 43,574 frees，`in use at exit: 0 bytes`，0 错误 |
| ASan + UBSan（scale 0.02 全量） | 583 checks，0 失败，无 sanitizer 报告 |

### 1.3 随机测试（影子模型）

20 seeds × 5000 步 = **100,000 次随机操作**，233,132 checks，**0 失败**。
每个操作与影子模型逐项比对：返回值、`size()`、`Contains`；每 97 步全表 `List`
比对（顺序、server_id、client 列表、lease_deadline）；每 499 步比对 9 个计数器。
随机序列覆盖重复 key、重复 client、超限、过期限界、删除不存在项、Tick 抖动。

### 1.4 五个真实场景

| # | 场景 | 结果 |
|---|---|---|
| 1 | 1:N 上限：1 server + 4 client | 4 个成功；第 5/6 个 `kFull`；`RemoveClient` 后第 5 个补位成功 |
| 2 | 90s 租约边界（假时钟） | 89999 ms 有效 / 90000 ms 过期；`Touch`/`AddClient`/`RemoveClient` 续租均验证；过期后重注册拿到干净条目 |
| 3 | 16 线程并发注册/查询 | 8 写线程 × 50 次抢注 → 8 成功 / 392 `kAlreadyExists` / 0 意外；8 读线程 4 万次查询；最终 8 key × 4 client，全部 server_id 非 0 |
| 4 | 重复 key 注册 | 同 server_id 重放仍 `kAlreadyExists`（非幂等 `kOk`，与文档口径一致），首个 server 保留；16 线程抢同一 key：1 成功 / 15 拒绝 |
| 5 | 60s 长稳（真实时钟） | 4 线程 17,356,602 次操作（≈289K ops/s）；RSS 3712→3968 KiB；收尾 `Tick(UINT64_MAX)` 精确清空全部条目 |

## 2. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁或内存泄漏（ASan/UBSan/valgrind/影子模型全绿）。

### P1-1（已修）写路径每次都 O(n) 扫全表

- 证据：`RemoveExpiredLocked` 被 `AddServer`/`AddClient`/`Remove`/`RemoveClient`/
  `RemoveServer`/`Touch` 六个写操作无条件调用，内部遍历整张 `m_entries`。
  1024 key 时 `Touch` **3589.5 ns/op**、`AddClient` **3562.6 ns/op**，
  且随 key 数线性上升。
- 修复：新增 `std::set<std::pair<deadline, key>> m_expiries` 过期索引。
  - 写路径先看队头：`deadline > now` 直接 O(1) 返回，不再扫表；
  - `RefreshLeaseLocked` 先摘旧索引项再插新项，`EraseEntryLocked` 同步移除；
  - 索引项命中前校验表内 deadline 一致，陈旧索引只会被丢弃，不会误删；
  - `lease_ms == 0`（永不过期）完全不建索引。
- 效果：Touch **18.8×**、AddClient **12.1×**、AddServer **9.5×**、Tick 全过期 **8.8×**、
  Tick 空转 **3.8×**。
- 回归：`TestLeaseIndexNoStale`（同一 key 连续续租 100 次后，`Tick(100000)` 不得误删，
  `Tick(190000)` 只能删 1 条）。

### P1-2（已修）租约加法溢出回绕

- 证据：`entry.lease_deadline_ms = now_ms + lease_ms`。当 `now_ms` 接近
  `UINT64_MAX` 时回绕成小值，条目在「未来」被判定为早已过期。
- 修复：`SaturatingAddMs` 饱和加法（`now > max - lease` 时钳到 `UINT64_MAX`）。
- 回归：`TestLeaseOverflow`（`now = UINT64_MAX - 10` 注册后 `Find`/`Contains`/`Tick` 均正常）。

### P1-3（已修）测试覆盖缺口

原 9 场景 135 checks 未覆盖：状态名映射、溢出边界、索引残留、List 过滤过期、
`RemoveServer` 后名额回收、返回值副本语义、长 key、默认时钟、并发计数精确性。
已补 9 个用例，checks **135 → 355–356**（并发混合用例的断言数随最终表状态浮动 1 条），详见第 4 节。

### P2（记录，本次不改）

1. **`SessionStatus::kExpired` 是死枚举**：所有路径都不会返回它（过期统一表现为
   `kNotFound`/`false`/`nullopt`）。头文件注释称「保留给调用方显式判断」，
   但没有任何 API 能产生该值。建议后续要么在 `Find` 的变体里返回，要么删除。
2. **`Find`/`List` 值拷贝成本**：每次调用复制 key 字符串 + clients vector；
   `List(1024)` 单次 28 µs。若调用方高频轮询，建议加回调式遍历或 `ListKeys()`。
   当前无调用方（模块未被 `app::Session` 集成），暂不优化。
3. **同一 key 三份副本**：`m_entries` 的 map key、`SessionEntry::key`、索引的
   `pair<deadline,string>`。1024 key 约 +64 KiB。会话表规模小，可接受。
4. **`RemoveClient` 续租整条条目**：Viewer 离开会延长 Host 租约。文档已声明该语义，
   但若 Host 心跳已失联，仅靠 Viewer 频繁进出即可让条目「续命」。建议后续评估
   改为只更新 clients、不刷新 lease。
5. **单元测试的依赖隔离不足**：`test_session_table.cpp` 经 `test_util.hpp` 间接
   include `link.hpp`，并链接整个 `go2cpp_bento`，因此该测试无法证明
   session_table 的解耦性。本次新增的 `audit_session_table_stress.cpp` 不依赖
   `test_util.hpp`，已独立证明（见第 3 节）。建议后续给该模块加一道
   「只链 session_table.cpp + protocol.cpp」的编译检查。
6. **`List` 的 `reserve(m_entries.size())`**：按「含过期项」的数量预留，最多可能
   过配 1 倍。1024 key 时影响 <100 KiB，记录。
7. **头文件依赖 `protocol.hpp`（305 行）只为 `NowMs()`**：`protocol.hpp` 本身只含
   标准库（`<array> <cstddef> <cstdint> <vector>`），无 link/session/transport，
   所以解耦结论成立；但严格说 session_table 并非「只依赖标准库」。建议后续把
   `NowMs` 拆到轻量 `clock.hpp`，本类只 include 那一个。本次受「只改
   session_table.*」的边界约束，未动 `protocol.hpp`，记录。
8. **文档行数基线已过时**：`docs/modules/bento-session.md` 记录
   `session_table.hpp` 209 行 / `session_table.cpp` 264 行，改动后为 231/303 行。
   本次边界只允许改 `bento/net/session_table.*` 与 `tests/bento/**`，docs 未同步，
   记录待后续更新。

## 3. 解耦审查

| 检查项 | 方法 | 结果 |
|---|---|---|
| 头文件自包含 | 单独 `#include "session_table.hpp"` 的 TU，`-Wall -Wextra -Wpedantic -Werror -fsyntax-only` | **通过** |
| include 树 | `g++ -H` | 只有 `session_table.hpp → protocol.hpp`；**无** `link.hpp`/`session.hpp`/`transport.hpp` |
| 目标文件符号 | `nm -C -u session_table.cpp.o` | 未定义符号全部是 libstdc++/libc/pthread；**0 个 go2cpp 符号** |
| 最小链接 | audit 程序只链 `session_table.cpp + protocol.cpp` | 构建通过；二进制无 `Link`/`Transport`/`PeerHub`/`Pairing` 符号，只有 `go2cpp::bento::NowMs()` |

结论：**无 link/session/transport 依赖，头文件自包含**；唯一非标准库依赖是
`protocol.hpp` 的 `NowMs()`（P2-7）。

## 4. 测试复查

### 4.1 断言有效性

- 原场景 7（16 线程混合）的 `table.size() <= 8U` 是**恒真断言**（配置上限就是 8），
  不可能失败。已改为：并发结束后 `List(75000)` 快照条数必须等于 `size()`，
  且每个 key 的 client 数 ≤ 4、client_id 在合法区间。
- 原场景 5 只断言 `server_id != 0`，audit 场景 4 补了「胜出者 id 必在 1..16」的范围断言。
- 其余断言均可失败（边界值、精确计数、状态机返回码），无空断言。

### 4.2 覆盖缺口（已补 9 个用例，+220~221 checks）

| 用例 | 覆盖点 |
|---|---|
| `TestStatusNames` | 7 个枚举名 + 未知值 `unknown`（日志/指标标签契约） |
| `TestLeaseOverflow` | 饱和加法，`now` 接近 `UINT64_MAX` 不回绕 |
| `TestLeaseIndexNoStale` | 反复续租后索引不残留（P1-1 的回归测试） |
| `TestListFiltersExpired` | `List` 过滤过期但 `size()` 保留、读路径不删除 |
| `TestRemoveServerReleasesSlot` | `RemoveServer` 成功后 `max_keys` 名额回收 |
| `TestFindReturnsCopy` | 修改 `Find` 返回值不影响表内状态 |
| `TestLongKey` | 4 KiB key 的注册/查找/删除 |
| `TestDefaultClock` | 不传 `now_ms` 的默认 `NowMs()` 路径 |
| `TestConcurrentStatsConsistency` | 8 线程 × 200 次写后 9 个计数器精确对齐 |

### 4.3 SKIP 逃逸

`test_session_table.cpp` 中 **无 `Skip()` 调用**（`grep Skip(` 为空）。CTest 的
`bento_session_table` 未设置 `SKIP_RETURN_CODE 77`，因此将来若有人加 `Skip()`，
CTest 会以 **FAIL** 暴露而不是静默跳过——没有逃逸通道。

### 4.4 未覆盖项

- 真实网络/多进程：本模块是纯内存表，无 IO，不适用。
- 时钟单调性：`now_ms` 由调用方传入，测试用假时钟覆盖了边界与回拨（`Find(0)`）。
- `SessionTable` 的拷贝/移动被 `= delete`，属编译期约束，运行期不可测。

## 5. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 单元测试
cmake --build .build-bento-wsl --target bento_test_session_table
./.build-bento-wsl/bento_test_session_table          # 355–356 checks
(cd .build-bento-wsl && ctest -R bento)              # 48/48

# 压测工具（独立编译，不注册 CTest）
g++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
    tests/bento/audit_session_table_stress.cpp src/bento/net/session_table.cpp \
    src/bento/net/protocol.cpp -o .build-bento-wsl/audit_session_table_stress
./.build-bento-wsl/audit_session_table_stress --mode all --scale 1.0
./.build-bento-wsl/audit_session_table_stress --mode soak --seconds 60
./.build-bento-wsl/audit_session_table_stress --mode random --scale 1.0 --seed 20261029
./.build-bento-wsl/audit_session_table_stress --mode leaks --scale 0.02   # valgrind

# ASan/UBSan
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude \
    tests/bento/audit_session_table_stress.cpp src/bento/net/session_table.cpp \
    src/bento/net/protocol.cpp -o .build-bento-wsl/audit_session_table_stress_asan
ASAN_OPTIONS=detect_leaks=1 ./.build-bento-wsl/audit_session_table_stress_asan \
    --mode all --scale 0.02
```

## 6. 改动文件

| 文件 | 改动 |
|---|---|
| `include/go2cpp/bento/net/session_table.hpp` | +`<set>`/`<utility>`；新增过期索引成员与 `EraseEntryLocked`；`RefreshLeaseLocked` 去 const；文档补复杂度说明 |
| `src/bento/net/session_table.cpp` | 重写 `RemoveExpiredLocked`（索引 + 队头剪枝）；`RefreshLeaseLocked` 同步索引；`AddServer` 用 `lower_bound` + `emplace_hint`；删除路径走 `EraseEntryLocked`；新增 `SaturatingAddMs` |
| `tests/bento/test_session_table.cpp` | +9 个用例（+191 行），135 → 355–356 checks；增强并发混合场景断言 |
| `tests/bento/audit_session_table_stress.cpp` | 新增审查压测工具（1118 行，不注册 CTest） |
