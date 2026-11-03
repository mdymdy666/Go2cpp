# 代码审查地图（CODE REVIEW MAP）

> 目的：项目实现约 **25.8 万行** + 测试 **23.4 万行**，逐行审查不现实。
> 本文件给出一张「按模块抽查」的路线图：每个模块先看哪个头文件、跑哪条测试、重点看哪几个函数。
> 审查原则：**看接口契约、看测试覆盖、看高风险点、看可复现数据**，不逐行读实现。

## 0. 一分钟总览

| 类别 | 行数 | 文件数 |
|---|---:|---:|
| 实现（include + src） | 257,699 | 594 |
| 测试 | 234,304 | 429 |
| demo（app/） | 6,316 | — |

**审查入口优先级**：P0（并发/网络/内存）→ P1（缓存/存储/序列化）→ P2（工程化/工具）→ P3（demo）。

---

## 1. 模块地图

| 模块 | 实现行数 | 入口头文件 | 关键实现 | 测试 | 审查命令 |
|---|---:|---|---|---|---|
| **concurrency**（P0） | 33,258 | `concurrency/scheduler.hpp`、`channel.hpp`、`fiber.hpp`、`sync.hpp` | `src/concurrency/scheduler.cpp`（6528）、`fiber.cpp`（4074）、`sync.cpp`（3068） | `tests/concurrency/`（含 stress_matrix 5636 行） | `ctest -R 'scheduler|channel|fiber|sync'` |
| **network**（P0） | 24,610 | `network/socket.hpp`、`http.hpp`、`websocket.hpp`、`io.hpp`、`hook.hpp` | `src/network/io.cpp`（2949）、`hook.cpp`（2979） | `tests/network/` | `ctest -R 'net_|http|ws|io'` |
| **bento**（P0，最大块） | 50,035 | `bento/net/*.hpp`、`bento/app/*.hpp` | `src/bento/net/link.cpp`（3377） | `tests/bento/`（含 bench_protocols 4710） | `ctest -R 'bento'` |
| **os**（P1） | 19,112 | `os/file.hpp`、`proc.hpp`、`monitor` | `src/os/` | `tests/os/` | `ctest -R 'os_'` |
| **debug**（P1） | 15,430 | `debug/*.hpp` | `src/debug/diagnostic_agent.cpp`（3493） | `tests/debug/` | `ctest -R 'debug|diagnostic'` |
| **storage**（P1） | 10,689 | `storage/db.hpp`、`orm.hpp`、`kv.hpp` | `src/storage/` | `tests/storage/` | `ctest -R 'storage|db|kv'` |
| **app**（P0 rd_net） | 10,906 | `app/gateway.hpp`、`app/realtime_room.hpp`、`rd_net/*.hpp` | `src/app/gateway.cpp`、`src/rd_net/` | `tests/app/`、`tests/rd_net/` | `tests/app/build_and_run_gateway.sh`、`tests/rd_net/*.sh` |
| **cache**（P1） | 4,016 | `cache/cache.hpp`、`factory.hpp` | header-only | `tests/cache/` | `ctest -R 'cache'` |
| **tunnel**（P1） | 5,648 | `tunnel/*.hpp` | `src/tunnel/` | `tests/tunnel/` | `ctest -R 'tunnel'` |
| **p2p**（P1） | 6,577 | `p2p/*.hpp` | `src/p2p/` | `p2p_*` ctest | `ctest -R 'p2p'` |
| **rpc / encoding / hash / compress / bufio / logging / config / tools**（P2） | 约 20,000 | 各自 `*.hpp` | 各自 `src/` | 各自 `tests/` | `ctest -R '<name>'` |
| **demo**（P3） | 6,316 | `app/douyin/REQUIREMENTS.md`、`app/bilibili/REQUIREMENTS.md` | `app/*/src/` | `app/*/scripts/accept_*.sh` | 跑验收脚本即可，不逐行审 |

---

## 2. 五分钟审查一个模块（流程）

1. **读入口头文件**（10–50 行的类声明）：接口是否清晰、所有权是否明确、线程安全是否写清；
2. **跑该模块测试**：`ctest -R '<模块>'` 或对应 `build_and_run_*.sh`，看是否全绿；
3. **抽查 2–3 个高风险函数**（见 §3）；
4. **看数据**：性能类看 `docs/metrics-ledger.md` 对应条目，用 `tools/verify_metrics.sh --only Mxx` 复跑；
5. **记录结论**：在本文件 §5 的「审查状态」里标「已审/发现问题」。

---

## 3. 高风险点清单（抽查重点）

### concurrency（并发正确性）

| 文件:行 | 风险 | 看什么 |
|---|---|---|
| `src/concurrency/scheduler.cpp` | 调度状态机 | `runnext` 抢占、work stealing、P 本地队列边界、park/wake 状态字 |
| `include/go2cpp/concurrency/channel.hpp` | 等待队列 | 关闭/select 回滚/Context 取消的竞态、单赢家语义 |
| `src/concurrency/fiber.cpp` | 栈与切换 | 栈边界检查、swapcontext 错误路径、FiberBin 复用 |
| `src/concurrency/sync.cpp` | 锁与 park | 持锁 park 检测、超时唤醒 |

### network（IO 正确性）

| 文件:行 | 风险 | 看什么 |
|---|---|---|
| `src/network/io.cpp` | epoll 生命周期 | EPOLLONESHOT、FD generation、关闭竞态 |
| `src/network/hook.cpp` | 系统调用 Hook | 非阻塞转换、EINTR、跨线程 |
| `src/rd_net/socket.cpp` | 四态 | FIN vs RST vs 截断 vs WouldBlock 的映射 |
| `src/rd_net/protocol.cpp` | 零拷贝 | ByteView 生命周期、SendTo 入队不拷贝 |

### storage（事务与并发）

| 文件:行 | 风险 | 看什么 |
|---|---|---|
| `src/storage/db.cpp` | SQLite 句柄 | busy_timeout、WAL、语句 finalize |
| `src/storage/orm.cpp` | 游标 | ForEachRow 提前结束的 reset（抖音 C3 踩过） |
| `src/douyin_store.cpp`（demo） | 多进程 WAL | `ResetStatements`/`FindUserName` 的修复点 |

### app（网关/实时房间）

| 文件:行 | 风险 | 看什么 |
|---|---|---|
| `src/app/gateway.cpp` | 路由/超时 | 最长前缀匹配、504/502、慢上游 |
| `src/app/realtime_room.cpp` | 广播 | 有界队列、慢连接踢出、Leave 幂等 |

---

## 4. 超大文件（可维护性隐患，待拆）

| 文件 | 行数 | 建议 |
|---|---:|---|
| `src/concurrency/scheduler.cpp` | 6,528 | 按「队列/窃取/park/sysmon」拆 4 个文件 |
| `include/go2cpp/concurrency/channel.hpp` | 4,313 | 拆「Channel / Select / 等待节点」 |
| `src/concurrency/fiber.cpp` | 4,074 | 拆「切换 / 栈 / 池」 |
| `src/debug/diagnostic_agent.cpp` | 3,493 | 拆「命令平面 / 等待图」 |
| `src/bento/net/link.cpp` | 3,377 | 拆「连接 / 帧 / 重传」 |
| `src/concurrency/sync.cpp` | 3,068 | 拆「Mutex / CV / WG」 |

---

## 5. 审查状态（人工维护）

| 模块 | 审查状态 | 审查人 | 日期 | 备注 |
|---|---|---|---|---|
| concurrency | 未审 | — | — | 建议先审 channel.hpp（4.3k） |
| network | 未审 | — | — | 先审 io.cpp + rd_net/socket.cpp |
| bento | 未审 | — | — | 最大块，可按子模块分 |
| os | 未审 | — | — | — |
| storage | 未审 | — | — | 注意 ORM 游标 |
| app/gateway | 未审 | — | — | 50 checks 已过 |
| demo | 验收脚本通过即可 | — | — | 不逐行审 |

---

## 6. 自动化门禁（替代人工的部分）

| 门禁 | 命令 | 覆盖 |
|---|---|---|
| 编译零告警 | `cmake --build .build-bento-wsl` | 全部 |
| 单测 | `ctest --test-dir .build-bento-wsl` | 290 项 |
| 指标复现 | `tools/verify_metrics.sh --quick` | 16 条台账 |
| 内存/并发 | `-DGO2CPP_ENABLE_TSAN=ON`、ASAN 构建 | 关键模块 |
| 契约边界 | `grep` G6 禁词（rd_net） | 交付物 |

> 说明：门禁能覆盖「可自动判定的正确性」，人工审查只需覆盖「设计取舍与边界语义」。
