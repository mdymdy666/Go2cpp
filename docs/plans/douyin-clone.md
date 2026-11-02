# 抖音级应用：分布式落地路线图

> 目标：用 Go2Cpp 的基础能力，一步一步搭出「可水平扩展的抖音级应用」。
> 原则：**每一步都可运行、可验证**；不追求一次性完美。
> 更新时间：2026-11-03

---

## 0. 目标与非目标

**目标**：短视频平台的核心能力——上传 / Feed / 播放 / 互动（评论点赞弹幕）/ 直播 / 搜索——并且能多机水平扩展。

**非目标（用现成服务）**：
- CDN（用云 CDN）
- 负载均衡（用 nginx / 云 LB）
- 深度推荐算法（先用时间/热度排序）

---

## 1. 架构（6 层）

```
用户 ── CDN/LB ── 网关（HTTP/WS，多节点）
                     │ RPC（G2RP）
      ┌──────┬───────┼───────┬──────┬──────┐
     user   video   feed   interaction  live   search
      │      │       │        │         │      │
      └──────┴───────┴────────┴─────────┴──────┘
                     存储层
        SQLite（元数据）/ LevelDB（KV/缓存）
        Queue（异步：转码/事件/feed 更新）
        Watch（订阅：配置/feed 变更）
                     协调层
        ServiceRegistry（服务发现）
        DistributedLock（互斥，fencing）
        LeaderElection（选主）
                     可观测
        Prometheus（/metrics）+ 日志（脱敏/轮转）
```

---

## 2. 分期（每期独立可验收）

### P0：单机 6 服务（1-2 天）

**做什么**
- 网关（HTTP/WS）监听 8080；
- 6 个 RPC 服务：user/video/feed/interaction/live/search，各监听独立端口；
- 每个服务：SQLite 或 LevelDB + RPC + 简单业务；
- 网关 → RPC（静态地址，先不做发现）。

**验收**
- `curl` 打通：注册/登录 → 上传元数据 → feed 列表 → 评论 → 点赞 → 搜索；
- 每个服务单独 `ctest`/冒烟；
- 网关 1000 并发压测无错误。

**依赖的 Go2Cpp 能力**：`rpc/`、`network/http`、`storage/{db,kv}`、`cache/`。

---

### P1：服务发现（1 天）

**做什么**
- 起 `Coordinator`（`distributed/coordinator`）；
- 每个服务启动时 `ServiceRegistry::Register`（ttl/3 心跳）；
- 网关通过 `ServiceRegistry` 解析服务地址（不再写死）。

**验收**
- 杀掉一个服务实例 → 网关 3 秒内摘除（不再路由）；
- 新实例注册 → 网关自动发现；
- 多实例（同服务 2 份）→ 网关轮询。

**依赖**：`distributed/{registry,coordinator}`。

---

### P2：数据分片（2 天）

**做什么**
- 按 ID 哈希分片：`user_id % N`、`video_id % N`、`room_id % N`；
- 每个分片一个存储实例（SQLite/LevelDB 文件）；
- 网关按 ID 路由到对应分片节点。

**验收**
- 写 10 万用户/视频 → 数据均匀分布（各分片 ±10%）；
- 扩容（N→2N）→ 数据迁移脚本（或双写迁移）；
- 跨分片查询（如「某用户的所有视频」）→ 二次索引（user→video 列表）。

**依赖**：`storage/{db,kv}`、`container/hash`。

---

### P3：异步队列（1-2 天）

**做什么**
- 用 `storage/queue`（LSM Queue）做：
  - 上传 → 转码任务队列（worker 消费）；
  - 互动事件（点赞/评论）→ 异步更新计数（feed 热度）；
  - Feed 更新 → 推送给关注者（fanout）；
- 消费者：独立 worker 进程（Go2Cpp Fiber）。

**验收**
- 上传 1000 个视频 → 转码队列消费完成（1000/1000）；
- 点赞 1e6 次 → 计数最终一致（与同步写入对比）；
- 队列积压 → 消费者自动扩容（多 worker）。

**依赖**：`storage/{queue,watch}`。

---

### P4：多机部署（1-2 天）

**做什么**
- 网关多节点（2-4 个）+ 前置 LB（nginx）；
- 服务多节点（每服务 2 份，ServiceRegistry 负载均衡）；
- 协调层：Coordinator 单点（先用），后续可换 etcd（`ICoordinator` 接口不变）。

**验收**
- 压测：4 网关 × 10 万连接 = 40 万连接（或用 `wrk`/`k6` 模拟）；
- 杀掉一个网关 → LB 摘除，用户无感；
- 杀掉一个服务实例 → 请求转移到另一实例。

**依赖**：`distributed/*`、`network/http`（网关）。

---

### P5：压测与调优（1-2 天）

**做什么**
- 单机 10 万 WS 长连接压测（Fiber + epoll）；
- Feed QPS 压测（读多写少，缓存命中率）；
- 直播 SFU 压测（N 房间 × M 观众）；
- 对比：与「Go + Gin + Redis」的基线（同机）。

**验收**
- 单机 10 万连接（内存 < 4GB，CPU < 80%）；
- Feed p99 < 50ms（缓存命中 > 90%）；
- SFU 单房间 100 观众（延迟 < 200ms）。

**依赖**：全部。

---

## 3. 与 Go2Cpp 能力的映射

| 需要 | Go2Cpp 组件 | 状态 |
|---|---|---|
| 协程/高并发 | `concurrency/`（Fiber/Scheduler/Channel） | ✅ |
| HTTP/WS 网关 | `network/http`、`bento/net/ws_server` | ✅ |
| RPC | `rpc/`（G2RP） | ✅ |
| 服务发现 | `distributed/registry` | ✅ |
| 分布式锁 | `distributed/lock` | ✅ |
| 选主 | `distributed/election` | ✅ |
| 关系存储 | `storage/db`（SQLite）+ `orm` | ✅ |
| KV | `storage/kv`（LevelDB 同构） | ✅ |
| 消息队列 | `storage/queue` | ✅ |
| 订阅 | `storage/watch` | ✅ |
| 缓存 | `cache/`（7 策略） | ✅ |
| 媒体（直播） | `media/`（SFU/采集/编码） | ✅ |
| 可观测 | `metrics/`（Prometheus） | ✅ |
| 日志 | `logging/`（脱敏/轮转） | ✅ |

**缺口（用现成或后续补）**：
- CDN（云服务）
- LB（nginx/云 LB）
- 深度推荐（算法团队）
- 跨机房强一致（etcd）

---

## 4. 当前进度

| 期 | 状态 | 备注 |
|---|---|---|
| P0 单机 6 服务 | ⏳ 待开始 | 电商 demo 已验证「网关+3 服务+RPC」模式 |
| P1 服务发现 | ⏳ 待开始 | `dist_demo` 已验证 ServiceRegistry |
| P2 分片 | ⏳ 待开始 | — |
| P3 队列 | ⏳ 待开始 | — |
| P4 多机 | ⏳ 待开始 | — |
| P5 压测 | ⏳ 待开始 | — |

**已完成的周边验证**：
- 电商 demo（`/tmp/shop`）：网关 + 3 服务 + 跨服务回写 + 前端，8/8 验收；
- 分布式 demo（`/tmp/dist_demo`）：3 节点一致 + kill -9 容错 + 持久化；
- 隧道模块（`go2cpp/tunnel`）：公网穿透（进行中）。

---

## 5. 每期交付物

```
代码：/tmp/douyin/（独立目录，不污染 Go2Cpp）
文档：本文件更新「当前进度」
演示：每期结束给一个「能看的」东西（截图/终端输出）
```

---

## 6. 教训（写给自己）

1. **每一步都要能跑**——不写「大而全」的骨架；
2. **先单机再分布式**——单机跑通了再拆分；
3. **复用已有能力**——不重复造（`docs/engineering-standards.md` §1.1）；
4. **诚实标注缺口**——CDN/LB/推荐是现成服务，不硬造。
