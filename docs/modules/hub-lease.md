# 模块文档：Hub / Lease / Heartbeat

> 需求：`collab/jinshan-requests.md` **REQ-003（Hub）**、**REQ-005（Lease/Heartbeat）**。
> 源码：`include/go2cpp/relay/hub.hpp`（157 行）、`src/relay/hub.cpp`（112 行）、
> `include/go2cpp/relay/lease.hpp`（210 行）、`src/relay/lease.cpp`（278 行）。
> 测试：`tests/relay/test_hub.cpp`、`test_lease.cpp`、`test_lease_90s.cpp`、
> `test_lease_clockjump.cpp`、`test_heartbeat.cpp`、`test_relay_scenarios.cpp`、
> `bench_relay.cpp`；共享辅助 `relay_test_util.hpp`、`fake_clock_jump.cpp`。
> 构建：`cmake/Go2CppRelay.cmake`（聚合入口）→ `cmake/Go2CppRelayHubLease.cmake`
> （目标 `go2cpp_relay`，别名 `go2cpp::relay`；ctest 标签 `relay` / `relay;long` / `relay;tsan`）。
> 状态：**现行**（2026-11-01，未提交）。

## 1. 概述与定位

这两个组件都只做「机制」，不碰业务语义，正好对应 JinShan 里中继侧的两件小事：

| 组件 | 回答的问题 | JinShan 里的典型用途 |
|---|---|---|
| `Hub` | 「这个 key 现在挂着哪些连接」 | 房间/会话注册表：`room → server CONTROL + viewer CONTROL + DATA 槽位` |
| `Lease` | 「这个窗口还在不在」 | 90s 没收到对端数据 → 判半开 → 关连接 |
| `Heartbeat` | 「该发心跳了吗；对端是不是死了」 | 每 30s 发一次 RDCH 心跳帧；`send()` 连续失败 → 判死 |

三者的共同约束：**并发安全、单调时钟、终态回调最多一次**。它们不实现：

- 业务配额（「一个房间最多 4 条连接」——用调用方计数或限流组件）；
- 路由/配对/token 语义（JinShan 自己的状态机）；
- 业务超时与重试（`Context`/deadline 是另一回事，见 §6）；
- 字节搬运（那是 `RelayPipe` / REQ-002 的事）。

## 2. Hub（连接注册表）

### 2.1 接口

```cpp
class Hub final {
public:
    using Stream = ::go2cpp::streams::Closer;   // 等价 Go 的 io.Closer
    bool Register(std::string_view key, std::shared_ptr<Stream> conn);
    std::vector<std::shared_ptr<Stream>> Lookup(std::string_view key) const;
    void Unregister(std::string_view key, const std::shared_ptr<Stream> &conn);
    bool CloseKey(std::string_view key);
    std::vector<std::string> Keys() const;
};
```

### 2.2 为什么 `Stream` 是 `streams::Closer`

REQ-003 的 `conn io.Closer` 在本项目里已有对应物 `go2cpp::streams::Closer`
（`Close() -> ErrorPtr`）。Hub 直接复用它，而不是另造流接口：

- 任何实现 `streams::Closer` 的类型都能注册；
- RelayPipe（REQ-002）的 `go2cpp::relay::Stream` 继承自 `streams::Closer`，
  `shared_ptr<relay::Stream>` 可**隐式转换**后注册，不需要适配层；
- Hub 只需要「可关闭」，不逼着 CONTROL 连接去实现用不到的 `Read/Write`。

`Close()` 的返回值在 `CloseKey` 里被忽略——Hub 不是 I/O 层，单条连接的关闭
错误没有可做的重试/回滚；需要统计的调用方在自己的 `Close()` 实现里记账。

### 2.3 语义

| 方法 | 语义 | 幂等性 |
|---|---|---|
| `Register` | 追加到 key 下；key 不存在则创建；`conn` 为空返回 `false`；同一对象重复登记返回 `false` | 重复登记不改表 |
| `Lookup` | 返回 `shared_ptr` 快照（插入顺序）；key 不存在返回空 vector | 只读 |
| `Unregister` | 按**对象地址**摘除一条，**不关闭**；最后一条摘除后 key 自动删除 | 重复调用无操作 |
| `CloseKey` | 锁内摘表、锁外逐条 `Close()`；返回 key 原本是否存在 | 第二次返回 `false`，不重复关闭 |
| `Keys` | 全部 key 的快照，**按字典序排序**（输出稳定，便于日志/断言） | 只读 |

补充约定：

- `Destructor` **不关闭**任何连接（析构期做 I/O/回调会延长生命周期并可能重入）；
- 同一条连接可以挂在多个 key 下；`CloseKey` 对每个 key 各调一次 `Close()`，
  因此实现方必须保证 `Close()` 幂等；
- `key` 允许为空串、允许包含 `\0`（内部按 `std::string` 长度保存）；
- 不检查连接是否已关闭——`streams::Closer` 不暴露状态。

### 2.4 并发模型

单一 `std::mutex` 保护整张表，临界区里**只做容器操作**：

```
Register/Unregister : lock → 改 unordered_map → unlock
Lookup/Keys         : lock → 复制 shared_ptr/字符串快照 → unlock
CloseKey            : lock → move 出连接列表 + erase key → unlock → 逐条 Close()
```

`CloseKey` 之所以把 `Close()` 放到锁外：`Close()` 是用户代码，JinShan 会在
里面回调 `Hub::Unregister` 做自我摘除；持锁调用必然自锁死。测试
`test_close_key_reentrancy` 专门用一个「Close 里注销自己 + 登记新连接」的
Stream 覆盖这条路径。

## 3. Lease（租约）

### 3.1 接口与状态机

```cpp
class Lease final {
public:
    Lease(std::chrono::milliseconds timeout, std::function<void()> on_expire);
    ~Lease();                       // 等价 Stop() + 等线程退出
    void Renew();                   // 重置为「现在 + timeout」
    void Stop();                    // 取消，幂等
};
```

```
  [Armed] --Renew()--> [Armed]
     |                    |
     | timeout 到期        | Stop()
     v                    v
  [Expired] --on_expire（一次）--> 回调返回后线程结束     [Stopped]（永不再触发）
```

语义要点：

- **构造即布防**：从构造时刻开始计时，覆盖验收「90s 无 Renew 触发一次」；
- `timeout <= 0` 合法：下一次检查立即到期；
- 到期后 `Renew()` 无效（不复活已宣布死亡的租约）；
- `on_expire` 可以为空（静默到期）；
- `Stop()` / 析构幂等；若回调正在执行，`Stop()` 不会中断它，但不会再触发第二次。

### 3.2 单调时钟

等待一律走 `std::condition_variable::wait_until(steady_clock::time_point)`，
实现里没有任何 `system_clock`/墙钟读取。续约与停止通过 **generation 计数器**
唤醒：计时线程醒来先核对 generation，只有「同一代且真正到点」才判到期，
避免「续约恰好与到期竞争」被误判成超时（丢失唤醒的经典坑）。

对应的验收不是读代码，而是**注入墙钟跳变**：
`tests/relay/fake_clock_jump.cpp` 编译成 `LD_PRELOAD` 垫片，只改写
`clock_gettime(CLOCK_REALTIME)`（±3600s），完全不碰 `CLOCK_MONOTONIC`。
`test_lease_clockjump` 在租约窗口中间触发跳变，断言：

| 场景 | 若用墙钟会怎样 | 实测（正确实现） |
|---|---|---|
| 窗口中间 +1h | 立即到期（约 250ms） | 800ms 到期 |
| 窗口中间 −1h | 约 1 小时后才到期（5s 超时失败） | 700ms 到期 |
| 跳变后 `Stop()` | —— | 立即停，0 次回调 |
| 跳变后 `Renew()` | —— | 仍按单调时钟再计 600ms |

### 3.3 线程模型与代价

每个 `Lease` 一个后台线程（条件变量等待，不轮询）。适合「每连接一两个」
的量级（10²~10³）；**不要拿它做十万级定时器**——那种场景应换成中心定时器，
接口不变、只换内部调度即可。回调在计时线程上执行；`~Lease()` 会 join 该线程，
所以回调里不要长时间阻塞。回调里调用 `Stop()` 是安全的（实现检测「当前就是
计时线程」后 detach，不能自 join）。

## 4. Heartbeat（心跳）

### 4.1 接口

```cpp
class Heartbeat final {
public:
    explicit Heartbeat(std::chrono::milliseconds interval);
    ~Heartbeat();
    void Start(::go2cpp::Context *ctx,
               std::function<bool()> send,
               std::function<void()> on_dead);
    void Stop();
};
```

### 4.2 语义

- 启动后**先等一个 interval** 再发第一次（Go `time.Ticker` 语义），避免刚连上
  就多打一包；
- `send()` 返回 `true` = 存活；返回 `false` 或抛异常 = 死亡：触发一次
  `on_dead` 并自行退出；
- 传入的 `Context` 取消 → **静默停止**，不触发 `on_dead`（这是调用方主动关停，
  不是对端死亡）；
- `Start` 时 `ctx` 已取消 → 不启动、不触发；
- `Stop()` 幂等；`Stop` 之后不会再调用 `send()`/`on_dead`（正在执行的一次
  `send()` 会跑完，但结果被丢弃）；
- `interval <= 0` 夹到 1ms，防止忙等；
- 落后一个周期（send 阻塞过久）时重新对齐到「现在 + interval」，不补发一串。

### 4.3 与 Lease 的关系（为什么别拿 send 成功当存活）

心跳包**发出去**只证明本端写成功，不证明对端还活着。正确用法是：
收到对端数据（或收到 pong）时 `lease.Renew()`；`lease` 到期才判半开。
`send()` 失败只说明**这一端**发现了问题（对端 RST/写超时），此时可以立刻
`on_dead`。两者互补，不要互相替代——这也正是 REQ-005 要求「与业务超时分离」
的含义（见 §6）。

### 4.4 Context 生命周期的处理细节

`Start` 只保存 `Context*`（调用方保证它比 Heartbeat 活得久）。取消回调通过
`ctx->Done().AddCallback(...)` 注册，**只捕获 `weak_ptr<State>`**：

- 回调先于 `Stop` 执行 → 拿强引用置 `stop` 并唤醒线程，安全；
- 回调晚于对象销毁 → `weak_ptr` 已失效，直接返回，不碰已释放内存；
- `Stop()` 会 `RemoveCallback(id)`，避免长期占着 Context 的回调表。

## 5. 构建与测试

### 5.1 构建

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl \
  --target go2cpp_relay_hub_test go2cpp_relay_lease_test \
           go2cpp_relay_heartbeat_test go2cpp_relay_scenarios_test \
           go2cpp_relay_lease_90s_test go2cpp_relay_lease_clockjump_test \
           go2cpp_relay_hub_tsan_test go2cpp_relay_bench -j 8
```

顶层入口是 `cmake/Go2CppRelay.cmake`（只 include）；Hub/Lease 的目标定义在
`cmake/Go2CppRelayHubLease.cmake`。RelayPipe（REQ-002）由
`cmake/Go2CppRelayPipe.cmake` 独立成目标 `go2cpp_relay_pipe`，两者互不覆盖。
`GO2CPP_WITH_RELAY=OFF` 可整体关掉本模块。

### 5.2 用例与结果（WSL2，12 vCPU，Release -O3）

```bash
ctest --test-dir .build-bento-wsl -L relay          # 全部（含 90s 慢用例）
ctest --test-dir .build-bento-wsl -L relay -LE long # 跳过 90s
```

| ctest 名 | 内容 | 实测 |
|---|---|---|
| `relay_hub` | 功能/幂等/重入/快照/1e5 并发/CloseKey 竞态/随机对拍 | 0.12s 通过 |
| `relay_lease` | 到期一次/续约重置/Stop/析构/0 超时/自停止/并发续约/300 租约/随机 | 5.28s 通过 |
| `relay_heartbeat` | 周期/首拍/死亡一次/异常/ticker/ctx 取消/自停止/重启/并发 Stop/无突发 | 2.20s 通过 |
| `relay_scenarios` | 7 个真实场景（见 §5.3） | 1.11s 通过 |
| `relay_lease_clockjump` | LD_PRELOAD 墙钟 ±1h 跳变 | 3.52s 通过 |
| `relay_hub_tsan` | 同一份 1e5 并发用例，TSan 编译 | 3.36s 通过（无报告） |
| `relay_lease_90s` | 真实 90s 不 Renew | 通过，`elapsed=90000 ms`、`fired=1` |

说明：WSL 下 TSan 默认会因 `mmap_rnd_bits` 较大报
`FATAL: ThreadSanitizer: unexpected memory mapping`，ctest 里用
`setarch --addr-no-randomize` 包了一层；手动跑时同样要加。

### 5.3 场景测试（`test_relay_scenarios.cpp`）

1. **房间生命周期**：`room:alpha` 挂 2 条、`room:beta` 挂 1 条，`CloseKey(alpha)`
   只关 alpha 的 2 条，beta 不受影响；
2. **半开判定**：收到数据后 `Renew`，静默超过 200ms（模拟 90s）→ 租约关闭房间；
3. **心跳续期**：心跳成功即 `Renew`；第 5 次发送失败后停止续租 → 租约恰好到期一次；
4. **重连竞态**：旧连接 `CloseKey` + 旧租约 `Stop` 后新连接登记同 key，
   `Lookup` 只返回新连接；
5. **心跳死亡**：`send()` 恒 false → `on_dead` 关连接、停租约、清空 Hub；
6. **1000 房间 churn**：每个连接恰好关闭一次，`weak_ptr` 全部过期（无泄漏）；
7. **并发 churn**：8 工作线程 + 2 清扫线程并发 `Register/Lookup/Unregister/CloseKey`，
   每条连接至多被 `Close()` 一次，最终 Hub 为空。

### 5.4 压测（`go2cpp_relay_bench`）

条件：WSL2、12 vCPU、Release（`-O3 -DNDEBUG`）、**load average ≈ 3.8**（同树还有
其它 agent 的构建在跑，数字偏保守，仅作量级参考）。

```
[hub ] 单线程 Register+Lookup+Unregister: 1e6 次, 95.5 ns/迭代, 10.47 Mops/s
[hub ] 8 线程 独立 key 同操作:            8e5 次, 457.2 ns/迭代, 2.19 Mops/s
[hub ] 8 线程 共享 1 key 同操作:          8e5 次, 626.1 ns/迭代, 1.60 Mops/s
[hub ] 8 线程只读 Lookup:                 1.6e6 次, 262.1 ns/次, 3.82 Mops/s
[hub ] CloseKey 批量: 10 万 key / 40 万连接, 72.1 ns/连接, 全部关闭
[lea ] Renew 热路径: 2e6 次, 80.5 ns/次, 12.43 Mops/s
[lea ] 200 个 50ms 租约迟到: P50=5 / P95=10 / P99=10 / max=10 ms
[hb  ] 10ms 心跳 200 拍间隔抖动: P50=0 / P95=0 / P99=0 / max=0 ms
```

解读：

- Hub 是**房间级**注册表，不是逐字节热路径。每次迭代 3 次字符串构造 + `shared_ptr`
  分配 + 单锁，95ns/迭代是合理量级；8 线程下吞吐下降主要是分配器与单锁竞争，
  不打算做分片（见 §7）；
- 租约/心跳的定时精度在空载时是毫秒级（P99 ≤ 10ms），远小于 30s/90s 的业务窗口。

## 6. 与业务超时的边界

| 关注点 | 由谁负责 |
|---|---|
| 对端是否还活着（30s 心跳、90s 半开） | `Heartbeat` + `Lease` |
| 这一次请求还值不值得等（读写 deadline、重试） | 调用方自己的 `Context`/`deadline` |
| 协议帧合法性、token、配额 | JinShan 自己 |
| 字节搬运与背压 | RelayPipe（REQ-002） |

`Lease` 不取消任何业务操作，也不生成 `Context`；它只维护一个窗口。
把「租约到期」当成「取消当前请求」会让半开判定与业务超时互相污染，
REQ-005 明确要求两者分离。

## 7. 已知限制与后续

1. **每对象一个线程**：10⁴ 级以上租约应换中心定时器（时间轮/最小堆），接口不变；
2. **Hub 单锁**：读多写少场景（广播）仍要串行复制快照；若将来成为热点，
   先考虑分片/RCU 快照，而不是把锁换细；
3. **没有并入安装/导出集合**：按本次边界「不改现有 cmake 集合」，`go2cpp_relay`
   未加入 `GO2CPP_MODULE_TARGETS`，`install(EXPORT)` 暂不含 relay 头与库；
   需要发布时把它加进该列表并补 `Go2CppInstall.cmake` 的导出；
4. **Hub 不感知连接状态**：已关闭的连接仍可登记/查询；调用方负责不这么用；
5. **`CloseKey` 忽略 `Close()` 错误**：需要错误统计时在实现方记账；
6. **Heartbeat 重启**：死亡后需先 `Stop()` 再 `Start()`，重复 `Start` 是空操作。

## 8. 最小示例

```cpp
#include "go2cpp/relay/hub.hpp"
#include "go2cpp/relay/lease.hpp"

using namespace std::chrono_literals;

go2cpp::relay::Hub hub;
auto control = std::make_shared<MySocketStream>(...);   // 实现 streams::Closer
hub.Register("room:42", control);

// 收到对端数据 → 续约；90s 静默 → 关房间
go2cpp::relay::Lease lease(90s, [&hub] { hub.CloseKey("room:42"); });

// 每 30s 发心跳；send 失败（对端 RST/写超时）→ on_dead
go2cpp::relay::Heartbeat heartbeat(30s);
heartbeat.Start(ctx, [&] { return SendPing(); }, [&] { hub.CloseKey("room:42"); });

// 收到对端 pong：
lease.Renew();
```

## 9. 参考

- `collab/jinshan-requests.md` §REQ-003 / §REQ-005；
- `include/go2cpp/tools/streams.hpp`（`streams::Closer`）；
- `include/go2cpp/concurrency/context.hpp`（`DoneSignal::AddCallback/RemoveCallback`）；
- 同目录 `docs/modules/channel.md`（本仓库模块文档格式参考）。
