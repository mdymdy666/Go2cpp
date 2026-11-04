# Go2Cpp 外部市场调研：到底应该做什么

> 调研日期：2026-11-04（所有链接访问日期均为 2026-11-04）
> 调研范围：Go 生态痛点、C++ 生态痛点、竞品定位、需求信号、场景结论
> 立场：本文不复述项目内部文档，所有外部结论要么带链接，要么显式标注 **[推断]**。

---

## 0. 一句话结论

**Go2Cpp 现在最缺的不是能力，是「一个能被人用一句话说清、并且能被外部数据验证的战场」。**

外部证据指向同一个空位：**Go 证明了「同步写法 + 运行时调度」是服务端正确的心智模型，C++ 证明了「零 GC + 可控内存」是 p99 与内存占用的唯一解，但两者之间没有任何一个成熟项目把「Go 的写法」搬到「C++ 的运行时」上并做成可用产品。** Seastar 做了运行时但没做写法（futures 仍然难写），bRPC 做了 RPC 但没做通用并发，Centrifugo 做了产品但是 Go 的。

**[推断]** 因此最有胜算的不是「再补一个协议模块」，而是选一个**第三方已经有付费产品验证过需求、但 C++ 侧完全空白**的场景，把库变成可对标、可交付的东西。本文第 6 节给出 5 个候选并排序。

---

## 1. 方法与证据边界（先说清楚哪些话不能说）

本次调研可用的证据类型与他用限制：

| 证据类型 | 是否取到 | 说明 |
|---|---|---|
| Go 官方 GC 文档与博客 | ✅ 一手 | `go.dev` 可直连，数据可直接引用 |
| C++ 框架官方文档 | ✅ 一手 | bRPC（Apache）、Seastar、go-zero、Centrifugo、NSQ 官网均可直连 |
| 政府/安全机构报告 | ✅ 二手转述 | CISA/NSA 原始 PDF 由 CPO Magazine 转述并给出原文链接 |
| GitHub star / 贡献者数据 | ❌ 取不到 | 本机 DNS 把 `github.com` / `api.github.com` / `raw.githubusercontent.com` 解析到非公网地址 |
| 招聘岗位数量、语言市场份额原始数据 | ❌ 取不到 | 招聘站点与统计站点在本环境不可直连，**因此本文不给任何岗位数/份额百分比** |
| 项目内部性能台账 | ⛔ 不使用 | 按铁律，项目内部数字不得当作外部市场结论 |

**这条边界很重要**：本文凡是出现数字，都能点开链接看到出处；凡是没有链接的数字，都标了 **[推断]**，或干脆不写。

---

## 2. Go 生态的真实痛点

### 2.1 GC 是 Go 最硬的成本项，而且官方自己承认了

Go 团队 2025-10-29 的官方博客《The Green Tea Garbage Collector》里有一句非常罕见的重话：

> "It's not that uncommon to see Go programs spending **20% or more of their CPU time** in the garbage collector."

同一篇博客给出了 GC 成本的拆解（均为官方数据）：

- GC 成本约 **90% 在 mark 阶段**，只有约 10% 在 sweep；
- 在 mark 的时间里，**通常至少 35% 纯粹卡在访存上**（memory stall），而不是在做有用功；
- 新 GC（Green Tea）让多数负载的 GC CPU 时间**减少约 10%**，部分负载**最多减少 40%**；
- 计划在 **Go 1.26 默认启用**。

来源：[The Green Tea Garbage Collector — go.dev/blog/greenteagc](https://go.dev/blog/greenteagc)（访问 2026-11-04）

**[事实]** 这三条合起来说明：Go 的 GC 开销不是「配置问题」，是**算法与硬件趋势的结构性冲突**。同一篇博客里，Go 团队自己写下了对 GC 语言最不利的判断：

> "Wait two years and your code will get slower."

理由是 NUMA 化、每 CPU 可用内存带宽下降、核数增加导致 mark 的共享队列成为瓶颈。**这是 Go 官方对「GC 语言在未来硬件上会越来越吃亏」的公开表态。**

### 2.2 GC 的调优空间是「时间换空间」的死区间，不是免费午餐

Go 官方 GC 指南把权衡写成了公式：

```
目标堆 = Live heap + (Live heap + GC roots) × GOGC / 100
```

并明确给出结论：**GOGC 翻倍，堆内存开销翻倍，GC CPU 成本大约减半**；反之亦然。

来源：[A Guide to the Go Garbage Collector — go.dev/doc/gc-guide](https://go.dev/doc/gc-guide)（访问 2026-11-04）

同一文档里还有几个常被忽略的硬数字：

- 64 位平台上，Go runtime 若干内部数据结构的**虚拟内存下限约 700 MiB**；32 位平台会预留 128–512 MiB 地址空间；
- 开启透明大页（THP）时，**小堆应用可能多用高达 50% 的内存**；只有 1 GiB 以上的大堆才有明显收益（最多 +10% 吞吐、+1–2% 内存）；
- 当 `runtime.gcAssistAlloc` 的累计 CPU 占比超过 5%，说明**分配速度已经跑赢 GC**——这是生产事故的前兆信号，而不是「性能有待优化」。

**[推断]** 这一段对产品定位的意义是：**Go 在「小内存 + 高分配率 + 低 p99」三者同时成立的场景里没有解**。GOMEMLIMIT（1.19+）只能让 Go 更早 GC（更费 CPU），不能消除 GC。这不是版本问题，是可预测性的天花板。

### 2.3 CGO、交叉编译、启动时间、热更新：普遍抱怨，但缺一手数据

以下四条是行业里长期存在的抱怨，本次调研**没有取到可引用的一手数据**，因此全部标 **[推断]**，并且刻意不给具体数字：

| 痛点 | 机制（推断） | 影响谁 |
|---|---|---|
| CGO 开销 | 每次跨语言调用有固定切换成本，且调用点无法内联；调用密集时会把 Go 的调度优势吃掉 | 需要复用 C/C++ 库（加解密、媒体编解码、推理引擎、GPU）的团队 |
| 交叉编译 | 纯 Go 交叉编译很顺，一旦引入 CGO 就要配整套目标平台工具链 | 边缘设备、嵌入式、多架构交付 |
| 启动时间 | runtime 初始化 + 大二进制加载；冷启动敏感场景（Serverless、CLI、边缘函数）吃亏 | FaaS、CLI 工具、边缘 |
| 热更新 | Go 官方无进程内热替换方案，`plugin` 包限制多、跨平台差 | 游戏服、长连接网关、需要不停机更新的场景 |

**[推断]** 这四条的共同点是：**它们都不是「Go 不够快」，而是「Go 不够可控」**。这决定了 Go2Cpp 的卖点不该是「比 Go 快 X%」，而应是「把不可控变成可控」。

### 2.4 小结：Go 的痛点是一句话

> **Go 用「不可预测的尾延迟 + 不可压缩的内存下限」换来了开发效率。当业务对 p99 或 RSS 有硬指标时，这笔交易就做不下去了。**

---

## 3. C++ 生态的真实痛点

### 3.1 异步编程的复杂度，是 C++ 服务端框架公认的第一难题

Seastar 官方教程开篇就用一整节讨论这件事，并把异步服务端开发者的困难总结为两条：

> - **Complexity:** ... writing a *complex* asynchronous server is notoriously difficult. The handling of a single connection ... now involves a large number of small callback functions, and a complex state machine ...
> - **Non-blocking:** ... if we only have one thread per core, the event-handling functions must *never* block, or the core will remain idle.

来源：[Asynchronous Programming with Seastar — docs.seastar.io](https://docs.seastar.io/master/tutorial.html)（访问 2026-11-04）

同一份教程还暴露了「即使是最先进的 C++ 异步框架也仍然难写」的具体证据：

- 默认任务配额 **500 µs**，超过就要让出；单个任务不 yield 超过 **20 ms** 就叫 *reactor stall*；
- C++23 之前，**lambda 协程用作 continuation 会导致 use-after-free**，官方专门写了一节 `lambda-coroutine-fiasco` 来教用户规避；
- 默认把**除 max(1.5 GiB, 7%) 之外的全部物理内存**预分配给应用，按 NUMA 节点分片。

**[推断]** 这些不是 Seastar 的设计缺陷，而是**在「无 GC + 多核 + 零拷贝」三重约束下必然产生的复杂度**。它必须由框架承担，但 Seastar 选择把复杂度部分转移给了使用者（futures 链、share-nothing 重构、绑核）。

### 3.2 内存安全：政策压力已经是既成事实，不是预测

CISA 与 NSA 2025-06 联合报告（原文 PDF 由美国国防部媒体站点托管）的核心数据，经 CPO Magazine 转述：

- Google Project Zero：**75% 的在野利用 CVE 涉及内存安全**；
- 微软的同类占比从 2016 年的约 **70% 降到近年的 50%**；
- Android 从 2019 年的 **76% 降到 2024 年的 24%**，靠的是「新代码用 Rust/Java + 与旧代码互操作」，**不是重写**。

来源：[New CISA/NSA Joint Report Reiterates Call for Memory-Safe Languages — CPO Magazine](https://www.cpomagazine.com/cyber-security/new-cisa-nsa-joint-report-reiterates-call-for-memory-safe-languages/)（访问 2026-11-04），原文 PDF 链接在文中。

同一篇转述里，报告自己列出的三大迁移障碍值得逐字注意：

1. **依赖管理**：现有库不是用内存安全语言写的，甚至没按这个前提设计；
2. **遗留系统紧耦合**；
3. **与非内存安全组件通信时的性能损失**。

**[推断]** 这对 Go2Cpp 是双刃剑：政策在推「离开 C++」，但报告同时承认「重写不现实、只能新代码用安全语言」。所以 **「把 Go 的写法搬到 C++」在政策叙事里是逆风的**——正确的叙事不是「继续写 C++」，而是「在你不得不保留 C++ 的那部分里，把内存安全与并发安全做成默认」。

### 3.3 依赖与构建：C++ 的「最后一公里」比语言本身更难

Seastar 教程里，编译一个 Hello World 需要：

- 用 Docker 构建镜像，或者
- `pkg-config --cflags --libs --static $SEASTAR/build/release/seastar.pc`，并且**必须加 `--static`**，因为 Seastar 目前只构建静态库。

来源同上（Seastar 教程 §2 Getting started）。

另有第三方分析指出，C++ 项目生成 SBOM 特别困难，原因是构建系统碎片化、头文件与静态链接导致依赖边界不清。

来源：[Why C++ Development Makes SBOM Generation So Difficult — RunSafe Security](https://runsafesecurity.com/blog/cpp-development-sbom-generation/)（访问 2026-11-04）

**[推断]** 这意味着：**任何 C++ 库的竞争者，真正的门槛不是 API 设计，而是「装得上、编得过、依赖可控」**。一个需要用户自己解决 boost/sqlite/openssl 的库，在评估阶段就会被淘汰一半。

### 3.4 现有框架的答案是「把复杂度封进框架」，而不是「教用户写对」

bRPC 的做法很典型：只暴露 **3 个主要头文件**（`Server` / `Channel` / `Controller`），并把「你不需要关心 XXXManager 怎么初始化、这些组件怎么分层」直接写进了卖点。

来源：[bRPC overview — brpc.apache.org](https://brpc.apache.org/docs/overview/)（访问 2026-11-04）

同一页里有一段对「IO 线程 + 处理线程」模型的尖锐批评，值得引用，因为它是 C++ 框架竞争的真实技术分水岭：

> If you have 10 IO-threads, one fd may affect 10% of all fds, which is unacceptable to industrial online services (requiring 99.99% availability).

bRPC 的解法是 bthread（M:N 调度）+ 单 fd 内消息级并行 + 近乎无锁的写路径，宣称 50 万+ QPS 下框架自身竞争极少。

**[推断]** 这段批评反过来证明了 **「调度模型」才是 C++ 服务框架的核心竞争力**，而不是协议支持数量。Go2Cpp 的 GMP 复刻正好落在这一点上——这是它相对「再写一个 RPC 框架」的结构性优势。

### 3.5 小结：C++ 的痛点也是一句话

> **C++ 什么都能做，但每件事都要先付出「理解执行模型」的代价；而绝大多数业务团队付不起这个代价。**

---

## 4. 竞品定位

### 4.1 C++ 侧

| 项目 | 定位 | 优势（有来源） | 谁在用 | 与 Go2Cpp 的差异 **[推断]** |
|---|---|---|---|---|
| **bRPC**（Apache） | 工业级 C++ RPC 框架 | 3 个头文件；同端口多协议（http/h2/gRPC/thrift/redis/memcached/rtmp/flv/hls/rdma）；bthread；内置 cpu/heap/contention profiler 与 bvar、rpcz | 百度内部：map-reduce、表存储、HPC 与模型训练、索引与排序 | bRPC 是 **RPC 框架**，并发模型服务于 RPC；Go2Cpp 是**通用并发运行时 + Go 语义 API**。bRPC 不给你 `chan`/`select`/`context` 那套写法 |
| **Seastar** | share-nothing 高性能 C++ 框架 | Scylla 用它重写 Cassandra，吞吐最高 10×、延迟更低更稳；每核一引擎线程绑核；零拷贝 TCP 栈；协程是首选写法 | ScyllaDB 生态 | Seastar 要求**重构为 share-nothing**，跨核只能消息传递；Go2Cpp 是共享内存 + 调度器（更接近 Go），迁移成本低但峰值扩展性不如 share-nothing |
| **sogou/workflow** | C++ 异步任务流引擎 | 以「任务流」组织异步，弱化回调 | 搜狗系业务 | 抽象层次不同：workflow 是**任务编排**，Go2Cpp 是**运行时语义**。workflow 不解决「怎么写一个长驻服务」 |
| **Tars**（腾讯） | 微服务治理框架 + RPC | 服务治理、部署、运维一套 | 腾讯系 | 重治理、重部署；Go2Cpp 是库，不带治理面 |
| **trpc-cpp**（腾讯） | 企业级 RPC 框架 | 插件化、可观测、与 tRPC 生态打通 | 腾讯内部 | 同 Tars，偏「框架 + 生态」 |
| **Boost.Asio** | 通用异步 IO 库 | C++ 标准候选、协程支持、生态最广 | 几乎所有人 | Asio 给你**机制**（executor/strand/io_context），不给你**语义**（没有 channel、没有 goroutine 级取消树）。心智负担最高的选项 |
| **libuv** | 跨平台事件循环 | Node.js 的底座，极稳 | Node.js、大量 C 项目 | C 风格回调；无 C++ 类型安全，无协程，无调度语义 |

**结论（推断）**：C++ 侧存在一个明显空位——**没有任何一个主流项目提供「Go 风格的类型化并发原语 + 运行时调度」**。bRPC 最接近，但它的并发原语是 bthread（底层），不是给业务用的 `chan`/`select`/`context`。

### 4.2 Go 侧

| 项目 | 定位 | 优势（有来源） | 谁在用 | 与 Go2Cpp 的差异 |
|---|---|---|---|---|
| **Centrifugo** | 自托管实时消息服务 | WS/HTTP-streaming/SSE/WebTransport/gRPC；宣称单机级别硬件 **100 万 WS 连接、3000 万条/分钟**；内置 Redis/Postgres/NATS 扩展 | VK、Badoo、ManyChat、OpenWeb、Grafana | **这是最值得研究的竞品**：它把「长连接网关」做成了产品，且有 PRO 商业版（ClickHouse 分析、实时追踪、性能优化、推送 API、SSO） |
| **go-zero** | Go 微服务框架 | `goctl` 代码生成；内置熔断/限流/自适应降载/超时；零分配路由 | 「数千家公司在生产使用」（官方说法） | 纯 Go，不解决 C++ 场景；但它证明了**「框架自带韧性」是卖点** |
| **kratos** | Go 微服务框架 | 面向 API/HTTP/gRPC 的工程化脚手架 | B 站系生态 | 同上，脚手架定位 |
| **gin** | Go HTTP 路由 | 事实标准、极简 | 极广 | 只是 HTTP 路由，不涉及运行时 |
| **NSQ** | 分布式消息平台 | 无中心 broker、内置发现、二进制无运行时依赖；用户含 Bitly/BuzzFeed/Stripe/Mozilla/Docker/Segment/Fastly/ByteDance | 大量中小规模部署 | 文档停留在 2023（v1.3.0），生态老化 |
| **one-api** | LLM API 网关 | 多模型统一入口 | LLM 应用团队 | 纯 Go 网关，无性能诉求 |

来源：[Centrifugo 官网](https://centrifugal.dev/) / [NSQ 官网](https://nsq.io/) / [go-zero 官网](https://go-zero.dev/)（均访问 2026-11-04）

### 4.3 空位在哪（这是全文最关键的一张表）

| 维度 | Go 侧有 | C++ 侧有 | 空位 |
|---|---|---|---|
| 同步写法 + 运行时调度 | ✅ goroutine | ❌（Seastar 用 futures，bRPC 用 bthread 但不暴露给业务） | **★ 空位** |
| 类型化 channel / select / context | ✅ 语言级 | ❌ 无主流实现 | **★ 空位** |
| 实时长连接网关（产品化） | ✅ Centrifugo（有 PRO 收入） | ⚠️ bRPC 有 media-server 但偏流媒体协议 | **★ 空位（C++ 版）** |
| 工业级 RPC + 可观测 | ✅ gRPC-Go / kratos | ✅ bRPC / trpc-cpp | 已拥挤 |
| 消息队列 | ✅ NSQ / NATS | ⚠️ 多为自研 | 半空 |
| 内存安全叙事 | ✅ Go 天然 | ❌ 政策逆风 | 不可打 |

**[推断]** 三个空位里，只有前两个是「Go2Cpp 的结构性优势能直接命中」的。第三个（长连接网关）是**产品空位**，前两个是**能力空位**。能力空位不产生收入，产品空位才产生收入。

---

## 5. 需求信号

### 5.1 有硬证据的信号

1. **实时长连接是能收钱的**。Centrifugo 有明确的 PRO 商业版，卖的是「分析、追踪、性能优化、推送 API、SSO」，且官网列出 VK/Badoo/ManyChat/OpenWeb/Grafana 等客户。[来源](https://centrifugal.dev/)
2. **长连接的规模门槛在持续被推高**。Centrifugo 2026-08 的博客标题就是「200k idle 连接起步，五个选项之后单节点撑住 100 万 idle 连接」——说明**「空闲连接的内存与 CPU 成本」是真实且持续的工程痛点**（每一条 idle 连接即使在无消息时也消耗 CPU 与内存）。[来源](https://centrifugal.dev/blog/2026/08/17/tuning-idle-connections)
3. **AI 流式输出正在成为长连接的新负载**。Centrifugo 专门做了 AI 落地页，关键词是「可恢复流（resumable streams）、多观看者（multi-viewer）、传输回退、跨节点扩展」。[来源](https://centrifugal.dev/docs/getting-started/ai_apps)
4. **GC 成本是 Go 团队自己投人解决的问题**。Green Tea 从 2018 年的想法做到 2025 年，投入了多名核心成员、写 AVX-512 汇编内核。一个「不值得解决」的问题不会这么做。[来源](https://go.dev/blog/greenteagc)
5. **内存安全已经是采购与合规语言**。CISA/NSA 报告明确把内存安全语言描述为「secure by design 的结构性控制」，并提到即将到来的采购规则与 SBOM 透明度要求。[来源](https://www.cpomagazine.com/cyber-security/new-cisa-nsa-joint-report-reiterates-call-for-memory-safe-languages/)
6. **C++ 框架的竞争焦点是调度模型，不是协议数量**。bRPC 用整段篇幅论证「IO 线程模型在 99.99% 可用性下不可接受」，并用 bthread 作为解法。[来源](https://brpc.apache.org/docs/overview/)

### 5.2 明确取不到的信号（不要编）

- **Go vs C++ 岗位数量对比**：无一手数据。
- **语言市场份额百分比**：无一手数据。
- **GitHub star 趋势**：`api.github.com` 在本环境被 DNS 拦截，无法核实任何 star 数。本文因此**不写任何 star 数字**。
- **云原生/边缘/AI infra 的市场规模**：未取到可核实的原始报告。

**[推断]** 拿不到这些数据并不致命，因为**它们本来就只能证明「市场大」，不能证明「这个产品有胜算」**。真正决定胜算的是 5.1 的六条：有人已经在为同类需求付钱（Centrifugo PRO）、痛点在持续恶化（idle 连接、AI 流式）、技术在进步但没有免费解（Green Tea 只减 10–40%）。

---

## 6. 结论：5 个「Go2Cpp 最有胜算」的场景

> 排序依据：**需求已被第三方产品验证 > 我方能力直接命中 > 竞品缺位 > 2 周可验证**。
> 每个场景都写了「为什么可能失败」，因为只有一条风险都不成立的场景是不存在的。

### 场景 A：C++ 版实时长连接网关（WS / SSE / WebTransport）

**一句话**：给需要百万级长连接、且不能接受 GC 抖动的团队，一个用 Go 写法写的 C++ 网关库。

- **目标用户**：IM/社交/协作类产品的后端团队；已有 C++ 技术栈但被 Asio 回调拖慢的团队；对 p99 有 SLA 的实时业务。**付钱方是「有 SLA 的公司」，用的人是「写业务 handler 的工程师」。**
- **为什么是我们**：`Fiber` + `Channel` + `Context` + `netpoll` 正好是「每条连接一个协程」模型的最小完备集；已有 WebSocket/SSE/HTTP2/tunnel/frame 组件，不需要从零写协议。这是 4.3 表里「同步写法 + 类型化原语」两个空位的交集。
- **现有竞品与差距**：Centrifugo 是 Go 的，GC 与内存是它的天花板（官方自己在优化 idle 连接的 CPU/内存）；bRPC 的 media-server 偏 RTMP/FLV/HLS 流媒体，不是 WS 长连接网关；Seastar 要 share-nothing 重构，业务方迁移成本过高。**差距：我们没有 admin UI、没有多语言 SDK、没有集群化 pub/sub。**
- **最小验证（2 周）**：复现 Centrifugo 公开的「idle 连接」场景——单节点 20 万 idle WS 连接，只测两个数：RSS 与心跳 p99。对照物是同等硬件上的 Go 版本。**2 周足够产出「同连接数下 RSS 与 p99」的一张表**，这是唯一能打动 C++ 团队的东西。
- **风险（为什么可能失败）**：C++ 长连接网关的客户通常**自研**，不自研的会直接买 Centrifugo/云服务；我们只有库，没有运维面（部署、扩缩容、灰度、监控面板），而 Centrifugo PRO 卖的恰恰是这些。**最可能的失败方式是：技术指标赢了，但客户说「我还得自己写运维」。**

### 场景 B：存量 C++ 服务的「Go 式并发」改造层

**一句话**：不换语言、不换框架，把回调式 C++ 服务改成同步写法。

- **目标用户**：维护 10 年以上 C++ 服务、代码里到处是回调与状态机的团队；被 Asio `strand`/`executor` 折磨的中型团队。付钱方是「技术债压力大的公司」。
- **为什么是我们**：`include/go2cpp/go/**` 提供与 Go 标准库同形的 API（`chan`/`context`/`error`/`io`/`net`/`sync`/`time`），设计目标是「改 import 与少量语法，不改并发/网络结构」。这是 Asio 与 Seastar 都不提供的抽象层次。
- **现有竞品与差距**：Asio 给机制不给语义；Seastar 要重构为 share-nothing；bRPC 只覆盖 RPC 路径。**差距：我们没有「把现有回调代码机械改写成协程」的迁移工具，全靠人手改。**
- **最小验证（2 周）**：挑 1 个真实开源 Asio 项目（例如某个 HTTP/WS 服务），用 Go2Cpp 重写一个 handler 链路，度量三件事：代码行数变化、需要改动的非并发代码比例、重写后是否引入新 bug（用原有测试跑）。**2 周能得出「改写成本」的第一个真实数字。**
- **风险**：**工具型产品最难收钱**——团队宁愿自己扛技术债，也不愿引入新依赖；而且「把 C++ 写成 Go」在纯 C++ 团队里是文化冲突。最可能失败的方式是：demo 很漂亮，但没人愿意在自己的生产代码上试点。

### 场景 C：小内存边缘设备 / 嵌入式网关

**一句话**：在 64–256 MiB 内存、需要确定性延迟的设备上，替掉 Go。

- **目标用户**：边缘网关、工业采集、车载/机顶盒类厂商；已经用 Go 写了原型但发现内存压不下去的团队。
- **为什么是我们**：无 GC、固定栈（默认 128 KiB 可调）、RAII。Go 在同等场景下的下限是硬的——官方文档写明 64 位 runtime 内部结构虚拟内存下限约 700 MiB、THP 在小堆上可能多用 50% 内存；GC 的调优只能时间换空间，换不出「确定性」。
- **现有竞品与差距**：这一格几乎没有「Go 写法」的竞争者，但**有大量更轻的替代品**（直接写 C、用 libuv、用 RTOS 自带抽象）。差距：我们的依赖太重（boost-context、sqlite3、openssl、zlib），交叉编译链未验证，二进制尺寸未优化。
- **最小验证（2 周）**：在 256 MiB 容器里跑一个 MQTT/CoAP 网关 demo，与 Go 版本对比 RSS 峰值与 p99；同时验证一次 aarch64 交叉编译能否成功。**如果交叉编译在 2 周内过不了，这个场景直接判死。**
- **风险**：**依赖与工具链是硬门槛**。嵌入式客户对「多一个第三方库」的容忍度极低；而且这个市场单价低、定制多、销售周期长。最可能失败的方式是：技术适配成立，但商业化不成立。

### 场景 D：AI 推理前置 / token 流式代理

**一句话**：做 LLM 流式输出的 C++ 侧代理层，解决断线续传、背压、多观看者扇出。

- **目标用户**：自建推理服务的公司（vLLM/SGLang/TensorRT-LLM 用户）；做 AI 应用但对首 token 延迟与流稳定性有要求的团队。
- **为什么是我们**：`Channel` 天然适合做背压，`Context` 天然适合做取消传播（客户端断开 → 取消上游生成），Fiber 适合「每连接一协程」。Go 侧 Centrifugo 已经把「可恢复流、多观看者、传输回退」定义成了需求，说明这不是伪需求。
- **现有竞品与差距**：Centrifugo（Go）已经在打这个场景，且明确列出 AI streaming；Python 生态（FastAPI + SSE）是默认选择但性能与内存差；C++ 侧没有对位产品。**差距：我们没有任何 LLM 相关的协议适配（OpenAI 兼容格式、SSE 事件格式、token 计费）。**
- **最小验证（2 周）**：搭一个最小代理——上游是假 token 流（每 20 ms 一个 chunk），下游 1000 个并发 SSE 客户端，测：断线重连后能否补齐、上游取消是否及时、内存是否随连接数线性增长。**这四项都是纯工程指标，2 周可测。**
- **风险**：**AI infra 客户极度倾向自研**，且生态锁定在 Python；这个赛道的赢家往往是推理引擎自己（vLLM 内置 API server），中间层容易被吞掉。最可能失败的方式是：做出来的东西，vLLM 下一个版本就内置了。

### 场景 E：p99 敏感的帧同步 / 行情推送

**一句话**：游戏帧同步、行情推送这类「每秒固定 tick + 严格尾延迟」的场景。

- **目标用户**：棋牌/MMO 后端、量化行情的分发侧。
- **为什么是我们**：固定栈 + 无 GC + 协作式调度，尾延迟可预测；Channel 做 tick 间通信比回调直观。
- **现有竞品与差距**：这是 C++ 的传统地盘，客户基本自研或用以太网/DPDK 方案；Skynet（Lua）在游戏服领域占了很大心智。**差距：我们没有热更新、没有脚本层、没有成熟的游戏服生态。**
- **最小验证（2 周）**：跑一个 1000 房间 × 每房间 10 人的 tick 广播，测 p99.9 帧间隔抖动。**但这个验证的说服力有限**——因为对手会说「我们用自研的更好」。
- **风险**：**这是五个场景里胜算最低的**。客户自研意愿最强、对新技术最保守、且已有成熟替代（Skynet、自研 epoll 框架）。列出来只是为了说明「p99 敏感」不等于「我们的市场」。

---

## 7. 排序表

| 排名 | 场景 | 胜算 | 验证成本 | 主要风险 | 判断依据 |
|---|---|---|---|---|---|
| 1 | **A. C++ 版实时长连接网关** | **高** | 低（2 周，只需 RSS + p99 两个数） | 只有库没有运维面；客户倾向自研或买 SaaS | 需求已被 Centrifugo 商业版验证；C++ 侧完全空白；我方 WS/SSE/HTTP2/Fiber 直接可用 |
| 2 | **D. AI token 流式代理** | **中高** | 低（2 周，纯工程指标） | 被推理引擎内置吞掉；客户自研 | 需求新鲜且增长；Channel/Context 天然匹配背压与取消；但生态锁定 Python |
| 3 | **B. Go 式并发改造层** | **中** | 中（2 周只能验一个 handler 链路） | 工具难收钱；C++ 团队文化抵触 | 能力空位最大（无人提供 Go 语义），但「能力」不等于「收入」 |
| 4 | **C. 小内存边缘设备** | **中低** | 中（交叉编译是硬门槛） | 依赖太重；市场单价低、定制多 | Go 的内存下限是硬事实，但我们的依赖与工具链是硬伤 |
| 5 | **E. 帧同步 / 行情** | **低** | 低 | 客户自研意愿最强；已有成熟替代 | p99 敏感 ≠ 我们的市场；无差异化抓手 |

**如果只能做一件事**：做 **A**，并且**只做「同连接数下的 RSS 与 p99 对标」这一件事**。理由：它是唯一一个「需求已被付费产品验证 + 竞品在 C++ 侧缺位 + 我方组件齐备 + 2 周能出数」四项同时成立的场景。

---

## 8. 反面意见：为什么以上可能全错

诚实地说，本文的结论有三个可能致命的漏洞：

1. **「C++ 侧空白」可能不是空白，而是「没人要」**。C++ 团队如果要长连接，会直接用 Go 写网关（跨语言调用很常见），而不是在 C++ 里复刻 Go。空位有时是「墓地」而不是「蓝海」。
2. **「Go 的 GC 是痛点」不等于「Go 用户会换 C++」**。Green Tea 证明的是 Go 团队在持续改进；真正的替代路径更可能是 Rust（内存安全 + 无 GC），而不是 C++。CISA 报告里点名的是 Rust，不是 C++。
3. **我们最大的资产（50 万行、300+ 测试）在市场眼里是「负债」**。一个新库越大，评估者越怕；他们真正想知道的是「能不能在 30 分钟内跑起来一个 hello world」。**这一条如果为真，那么当前最该做的不是加场景，而是把「30 分钟上手」做到极致。**

**[推断]** 综合来看，最理性的下一步不是写更多代码，而是：**用 2 周把场景 A 的对标数据做出来，同时把「30 分钟上手」的路径压缩到可验证的程度。** 如果这两件事都做成了，再谈第二个场景；如果做不成，说明问题不在场景选择，而在交付形态。

---

## 9. 来源清单（全部访问于 2026-11-04）

**Go 侧（一手）**
- [The Green Tea Garbage Collector — Go Blog, 2025-10-29](https://go.dev/blog/greenteagc)
- [A Guide to the Go Garbage Collector — go.dev](https://go.dev/doc/gc-guide)

**C++ 侧（一手）**
- [bRPC overview — Apache bRPC](https://brpc.apache.org/docs/overview/)
- [Asynchronous Programming with Seastar — docs.seastar.io](https://docs.seastar.io/master/tutorial.html)

**Go 框架/产品（一手）**
- [Centrifugo 官网](https://centrifugal.dev/)
- [Centrifugo：100 万 idle WS 连接调优, 2026-08-17](https://centrifugal.dev/blog/2026/08/17/tuning-idle-connections)
- [Centrifugo for AI apps](https://centrifugal.dev/docs/getting-started/ai_apps)
- [NSQ 官网](https://nsq.io/)
- [go-zero 官网](https://go-zero.dev/)

**安全/政策（二手转述 + 原文链接）**
- [New CISA/NSA Joint Report Reiterates Call for Memory-Safe Languages — CPO Magazine, 2025-06-30](https://www.cpomagazine.com/cyber-security/new-cisa-nsa-joint-report-reiterates-call-for-memory-safe-languages/)
- [Why C++ Development Makes SBOM Generation So Difficult — RunSafe Security](https://runsafesecurity.com/blog/cpp-development-sbom-generation/)

**取不到的证据（如实记录）**
- GitHub star / 贡献者数据：`github.com`、`api.github.com`、`raw.githubusercontent.com` 在本环境被解析到非公网地址，无法访问。
- 招聘岗位数、语言市场份额原始报告：未取到可直连的一手来源。

---

## 10. 一页纸摘要（给没时间的人）

- **Go 的痛点是可预测性**：官方承认 20%+ CPU 在 GC、mark 中 35% 时间卡在访存，且硬件趋势（NUMA、带宽下降）对 GC 语言不利。新 GC 只能减 10–40%，不是消除。
- **C++ 的痛点是心智负担**：Seastar 官方承认复杂异步服务「notoriously difficult」，连 lambda 协程都有 UAF 陷阱；bRPC 的答案是「只给 3 个头文件」。
- **空位**：没有任何主流 C++ 项目提供 Go 风格的并发语义（`chan`/`select`/`context` + 运行时调度）。bRPC 最接近但只服务 RPC，Seastar 要求 share-nothing 重构。
- **最该做的场景**：C++ 版实时长连接网关（WS/SSE/WebTransport），因为需求已被 Centrifugo 的 PRO 商业版验证，而 C++ 侧空白。
- **2 周验证**：同连接数（20 万 idle WS）下的 RSS 与心跳 p99，对照 Go 版本。
- **最大风险**：C++ 团队对长连接网关倾向自研，或直接用 Go 写；「空白」可能是墓地。
- **最该补的不是模块，是「30 分钟上手」**：新库越大，评估者越怕。
