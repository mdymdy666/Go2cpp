# Go2Cpp 开发方向（2026-10-23 起）

> 基线：HEAD `fc287c6`，ctest 135/135，TSAN CLEAN。每轮（30 分钟）追加 2–3 个方向，保留历史。

## 第 1 轮（2026-10-23）

### 方向 1：C++20 协程适配层（co_await 接 GMP）

**价值**：让现代 C++ 代码用 `co_await`/`co_return` 直接跑在 Go2Cpp 的 GMP 调度器上——C++ 生态的 async 语法 + Go 式调度器（线程级 work stealing + Fiber 切换 ~500ns）。解决「C++20 协程库（cppcoro/asio）没有 Go 式调度器」的空白。

**复用现有能力**：
- Scheduler（P/M/G、work stealing）、Fiber（growable 栈）、Timer、IOManager（epoll）、Channel（协程间通信）。

**技术难点**：
- awaiter 与 Fiber park 的集成：`await_suspend` 里把当前 Fiber 挂起（`Scheduler::park`），`await_resume` 恢复；
- 协程帧在堆上（`std::coroutine_handle`）与 Fiber 栈的关系（协程帧不随 Fiber 栈迁移——**与迁移能力冲突**，需明确边界）；
- 异常传播、生命周期、与 `std::stop_token`/`Cancellation` 的集成。

**成本**：2–4 周（1 人）。

**风险**：C++20 协程与自定义栈 Fiber 混用的语义复杂度（协程帧堆分配 + Fiber 栈切换）；需严格限定「协程内不迁移」。

**最小验证**：
```cpp
Task<int> f() { co_await Sleep(1s); co_return 42; }
// 跑在 Scheduler 上，1s 后完成，验证 Fiber 切换与暂停
```

**创造力**：把「Go 式调度」和「C++20 协程语法」结合——市面上几乎没有成熟实现（cppcoro 只有单线程调度、asio 的 awaitable 绑线程池）。

---

### 方向 2：分布式协程迁移（多节点）

**价值**：把单机跨进程迁移（`G2CMIG03`）扩展到**多节点**：协程在等待下游响应时迁到边缘节点/备用节点（弹性伸缩、故障转移、边缘调度）。这是 Go 做不到的（goroutine 不可迁移）。

**复用现有能力**：
- 迁移格式（符号化 + safepoint + 签名）、Doctor（dump/force-wake）、HTTP/3/QUIC（picoquic 传输）、持久化（快照落盘）。

**技术难点**：
- 跨节点传输（QUIC stream，已有 HTTP/3 client）；
- 外部资源（fd/socket）不可迁移 → 需「绑定重建」（迁移后重新建立连接）；
- 状态一致性（迁移期间原节点放弃协程、目标节点接管，需 fencing）；
- 版本兼容（符号表/build-id）。

**成本**：4–8 周（1–2 人）。

**风险**：外部资源语义（迁移后连接重建）、故障场景（迁移中断）。

**最小验证**：
1. 节点 A：Fiber 等 channel → dump（`G2CMIG03`）；
2. 通过 QUIC/文件传到节点 B；
3. 节点 B：restore → 发消息 → Fiber 继续 → 完成；
4. 对比单机迁移，验证跨节点只增加传输延迟。

**创造力**：全球独一份的「协程级分布式调度」；Erlang 进程迁移的 C++ 版。

---

### 方向 3：gRPC 服务端（GMP 原生 HTTP/2）

**价值**：C++ gRPC 的线程池模型在**阻塞型 RPC**（DB/下游调用）下线程膨胀；Go2Cpp 的 GMP + HTTP/2 可以做到「协程级 gRPC 服务端」——阻塞场景吞吐 1.9x–6.1x 优势（已有数据）。

**复用现有能力**：
- HTTP/2（HPACK、流状态机、MAX_CONCURRENT_STREAMS、GOAWAY）、Fiber（每 RPC 一个协程）、Channel（流控）、Context（deadline/cancel）。

**技术难点**：
- gRPC 协议帧（HTTP/2 DATA + 5 字节压缩/长度前缀 + trailers）；
- protobuf 编解码（引入 protobuf-lite 或手写子集）；
- 流控（HTTP/2 window + gRPC 流控）；
- 与现有 Server/route 的集成（或独立 gRPC server）。

**成本**：3–6 周（1 人）。

**风险**：protobuf 依赖（可子集化）、gRPC 兼容性（grpcurl 验证）。

**最小验证**：
1. 定义 `echo.proto`（unary）；
2. `grpcurl -d '{"msg":"hi"}'` 调通；
3. 压测：1000 并发阻塞 RPC vs 线程池 gRPC（对比线程数/吞吐）。

**创造力**：商业价值直接（阻塞场景的 gRPC 替代），且复用现有 HTTP/2 栈。

---

## 本轮推荐

**优先方向 1（C++20 协程适配层）**：成本最低（2–4 周）、复用最多（Scheduler/Fiber/Timer/IO）、生态价值大（C++ 开发者立刻能用）；方向 2（分布式迁移）是差异化护城河但成本高；方向 3（gRPC）商业价值明确。

## 历史轮次

- 第 1 轮（2026-10-23）：C++20 协程适配 / 分布式协程迁移 / gRPC 服务端。

## 第 2 轮（2026-10-23）：跳出 C++ 后端，AI 结合

### 方向 4：AI 并发故障诊断助手 ⭐ 本轮推荐

**价值**：把 Doctor / 事件环 / 资源历史 / 回放作为 AI 的「眼睛」——AI 自动分析死锁/卡死/性能退化，给出根因与修复建议。C++ 生态没有「运行时 + AI 诊断」的组合；这些结构化数据天然是 AI 的上下文。

**复用**：Doctor（status/dump/等待图/ctx.cancel）、事件环（per-thread id + ts）、资源历史（10 类结构 + 栈/Fiber 链）、回放（G2CRPLY v5）、持久化。

**落地**：
- `go2cpp doctor --ai`：把 dump/等待图/资源历史转成结构化 prompt → 本地 LLM（Ollama/llama.cpp）或 MCP → 输出「根因 + 修复建议 + 验证步骤」；
- 闭环：AI 看到死锁 → 建议 `ctx.cancel` 或 `mutex-unlock --force` → 验证恢复；
- 已有素材：本仓库大量死锁/卡死 dump（`docs/bug-report-2026-10-20.md`、场景输出）。

**难点**：模型幻觉（用结构化 dump 约束输出）、本地模型部署（CPU 推理慢，可用小模型）。

**成本**：2–3 周；**风险**：幻觉（加 schema 校验）。

**最小验证**：给 AI 一份「mutex 锁顺序死锁 dump」，它输出「gid A 持 L1 等 L2、gid B 持 L2 等 L1，建议 ctx.cancel A」——人工核对正确率 ≥80%。

### 方向 5：AI Agent 执行引擎（GMP 调度工具调用）

**价值**：AI agent 的瓶颈是 **IO 等待**（调 API/查库）；用 GMP 协程并发调度工具调用（切换 ~500ns、百万级），替代线程池/async 框架。C++ 生态没有「Go 式 agent 调度器」。

**复用**：Scheduler/Fiber、Channel/select（agent 间通信）、Timer（超时）、HTTP/2/3 客户端（工具调用）、Context（取消）。

**落地**：
- `AgentRuntime`：一次 agent 任务 = 协程；工具调用 = 协程 + Channel 结果；
- 支持 fan-out/fan-in（多工具并行）、超时/取消、结果聚合；
- 压测：1000 个并发工具调用 vs 线程池（线程数/延迟/内存）。

**难点**：与 Python 生态（LangChain）集成（C ABI/HTTP）；结果聚合的语义。

**成本**：3–4 周；**风险**：生态集成。

### 方向 6：AI 推理服务运行时（协程化推理）

**价值**：推理服务（LLM/图像）是「等 GPU + 等下游」的 IO 密集场景；GMP 协程替代线程池，避免线程膨胀。阻塞场景数据（1.9x–6.1x）直接适用。

**复用**：Scheduler/Fiber、IO（epoll）、HTTP/2（gRPC 或 REST）、Context（deadline）。

**落地**：
- Go2Cpp + ONNX Runtime / vLLM C++ API：每请求一个协程，等 GPU 时 park；
- 压测：并发 1000 请求 vs 线程池（线程数/吞吐/内存）。

**难点**：推理框架集成（CUDA/ONNX 依赖）、GPU 流与协程的绑定。

**成本**：3–5 周；**风险**：依赖重。

### 方向 7：AI 生成代码的确定性验证

**价值**：AI 生成的并发代码最容易死锁/竞态；用 Go2Cpp 的 record/replay **确定性验证**（同一交错重放，检查死锁/结果一致）。这是「可验证的正确性」。

**复用**：回放（G2CRPLY v5 含锁/IO 事件）、记录器、Doctor、测试框架。

**落地**：
- `go2cpp verify --replay <file>`：跑 AI 代码 → 录制 → 重放 → 自动检查卡死/不一致；
- 集成 AI 编程助手（生成 → 验证 → 修复循环）。

**难点**：AI 代码需插桩（跑在 Go2Cpp 运行时）；覆盖范围（只覆盖 channel/锁/IO 事件）。

**成本**：2–3 周；**风险**：插桩成本。

### 本轮推荐

**方向 4（AI 并发故障诊断助手）**：数据已就绪（dump/等待图/资源历史/回放），成本最低（2–3 周），能直接演示「AI 看 dump 定位死锁」——本仓库的 agent 已经这么做过了，产品化即可。

## 第 3 轮（2026-10-23）：跨平台 / 跨语言 / 运维

### 方向 8：WASM 编译目标（浏览器里跑协程）

**价值**：把 Go2Cpp 运行时编译到 WebAssembly（Emscripten/wasi-sdk），在浏览器里跑协程 + 可视化诊断——「C++ 并发运行时 + 浏览器时间线」的演示/教学/前端仿真场景。

**复用**：Scheduler/Fiber（需替换上下文切换为 wasm 的 asyncify/异常）、Channel、诊断视图（JSON → 前端渲染）。

**难点**：
- 上下文切换：wasm 没有 `ucontext`/汇编跳转 → 需 asyncify（Emscripten）或 wasm 异常/协程提案（JSPI）；
- 阻塞 IO：浏览器无 epoll → 用 JS 回调/SharedArrayBuffer；
- 体积/性能（wasm 单线程）。

**成本**：4–8 周；**风险**：上下文切换方案（asyncify 性能差、JSPI 兼容性）。

**最小验证**：一个 `channel` ping-pong demo 编译成 wasm，在浏览器里跑 + 输出时间线。

### 方向 9：跨语言绑定（Python/Rust 用 Go2Cpp 协程）

**价值**：把 Go2Cpp 的协程调度能力暴露给 Python（AI 生态）/Rust——Python 的 asyncio 是单线程，Go2Cpp 提供真并行协程；Rust 的 async 需要运行时，Go2Cpp 提供现成的。

**复用**：Scheduler/Fiber/Channel、C ABI（导出 `go2cpp_spawn/go2cpp_channel_*`）、诊断（跨语言 dump）。

**难点**：
- C ABI 稳定（句柄/生命周期/异常映射）；
- Python 的 GIL（协程回调跨 GIL）；
- Rust FFI（`extern "C"` + 生命周期）。

**成本**：3–6 周；**风险**：GIL/FFI 复杂度。

**最小验证**：Python ctypes 调用 `go2cpp_spawn` 跑 1000 个协程 + channel 通信；或 Rust FFI 压测。

### 方向 10：不停机升级（热重载 + 协程迁移）

**价值**：把现有 SO 热重载（F3 跨进程/灰度）与协程迁移（G2CMIG03）结合——**升级时把在途协程迁到新版本 SO**，真正「不停机升级」。

**复用**：SO 热重载（已有骨架）、迁移（符号化/safepoint/签名）、Doctor、持久化。

**难点**：
- 新旧 SO 的符号兼容（safepoint id 跨版本映射）；
- 在途协程状态（等待 channel/锁）的迁移点；
- 版本回滚（失败时切回旧 SO）。

**成本**：4–8 周；**风险**：状态迁移边界（外部资源）。

**最小验证**：SO v1 里一个协程等 channel → 热切到 v2 → 协程在 v2 继续 → 完成（对比「重启中断」）。

### 本轮推荐

**方向 9（跨语言绑定，Python/Rust）**：成本中等、生态价值大（Python AI 生态 + Rust 系统生态都能用 Go2Cpp 的协程）；方向 8（WASM）演示强但上下文切换方案不确定；方向 10（不停机升级）是差异化但成本最高。

## 第 4 轮（2026-10-23）：深层可观测 / 形式化 / 生态

### 方向 11：eBPF 原生集成（全栈可观测）

**价值**：内核侧追踪（syscall 延迟、调度延迟、futex 竞争）+ 用户态协程事件（park/wake/资源）**关联成一条时间线**——回答「这个协程慢是因为内核调度、锁竞争还是下游 IO」。

**复用**：事件环（per-thread id + ts）、资源历史、Doctor、持久化。

**难点**：
- eBPF 程序（bcc/libbpf）+ 用户态事件关联（clock 对齐、线程 id 映射）；
- 权限（root/BPF 挂载）、内核版本兼容；
- 开销（采样频率）。

**成本**：4–6 周；**风险**：依赖内核/eBPF 工具链。

**最小验证**：一个协程等 futex 的 demo → eBPF 抓到 futex 等待时长 + 用户态 park 事件 → 拼成时间线。

### 方向 12：协程级性能剖析器（pprof 式）

**价值**：C++ 没有 Go 的 pprof；用现有事件（调度延迟/阻塞/资源等待）生成**火焰图 + 延迟直方图 + 阻塞分析**——「哪个协程/锁/通道最慢」。

**复用**：事件环（ts）、资源历史（持有/等待时长）、6 视图、持久化。

**难点**：
- 采样与聚合（10 万+ 协程的火焰图渲染）；
- 与 Linux perf 的整合（可选）。

**成本**：2–3 周；**风险**：数据量（采样策略）。

**最小验证**：`go2cpp prof --pid <pid> --type flame|latency|block` 输出 SVG/文本报告，能定位「Mutex#7 持有最长」或「协程 G5 等 channel 30ms」。

### 方向 13：形式化验证（TLA+ 调度器/锁协议）

**价值**：用 TLA+/模型检验**数学证明**调度器（park/wake）和锁协议（两态 Mutex）无死锁/无丢唤醒——比测试更强的保证，也是「正确性」叙事的顶点。

**复用**：现有协议（Mutex 两态、PC futex、park/wake 状态机）、TSAN/ctest 作为对照。

**难点**：
- 把 C++ 实现抽象成 TLA+ 模型（状态/转移/公平性）；
- 模型检验状态爆炸（需对称性/约束）；
- 模型与实现的偏差证明。

**成本**：4–8 周；**风险**：模型抽象偏差。

**最小验证**：Mutex 两态协议的 TLA+ 模型 + TLC 检验（无死锁、无饥饿、互斥成立）；与实现对照。

### 本轮推荐

**方向 12（协程级性能剖析器）**：成本最低（2–3 周）、数据现成、实用价值直接（定位慢协程/热点锁）；方向 11（eBPF）全栈可观测但依赖重；方向 13（TLA+）是正确性叙事的顶点但成本高。

## 第 5 轮（2026-10-23）：生产治理 / 混沌工程

### 方向 14：协程级服务治理原语（限流 / 熔断 / 舱壁 / 超时）

**价值**：微服务生产必备的治理能力——限流（令牌桶/滑动窗口）、熔断（失败率触发）、舱壁（按下游隔离并发）、超时（Context deadline）。C++ 生态没有「与协程调度一体的治理原语」；现有实现多为线程池 + 第三方库。

**复用**：`Context`（deadline/取消）、`Channel`（令牌桶/信号量）、`Timer`（窗口/超时）、`select`（超时与业务竞争）、诊断（限流/熔断状态可 dump）。

**难点**：
- 限流器的高并发精度（滑动窗口 vs 令牌桶）；
- 熔断状态机（closed/open/half-open）与协程取消的配合；
- 舱壁的 per-downstream 并发计数（无锁）。

**成本**：2–3 周；**风险**：语义细节（熔断恢复策略）。

**最小验证**：`RateLimiter`（1000 QPS）+ `CircuitBreaker`（失败率 50% 打开）+ `Bulkhead`（下游并发 10）+ `Timeout`，压测验证限流精度/熔断恢复/舱壁隔离。

### 方向 15：并发混沌工程 + 自动验证平台

**价值**：主动注入并发故障（延迟、丢唤醒、锁顺序反转、channel 关闭竞态、Timer 抖动），配合确定性回放**自动验证**「系统在故障下仍然正确/可恢复」——把「并发测试」从被动变主动。本仓库已有 `GO2CPP_REPLAY_DROP_WAKE` 注入、死锁检测、回放验证，正好是地基。

**复用**：回放（G2CRPLY v5）、故障注入（drop_wake/延迟）、Doctor（检测/恢复）、5 轮随机测试框架、graph（可视化故障环）。

**难点**：
- 故障注入点覆盖（park/wake/锁/通道/Timer/IO）；
- 「正确」的判定（无死锁 + 结果一致 + 恢复成功）；
- 注入与业务线程的隔离（只影响目标协程）。

**成本**：3–4 周；**风险**：注入对生产代码的侵入（编译开关）。

**最小验证**：对 `tests/scenarios/deadlock_routes` 注入「丢唤醒 + 锁顺序反转」→ 自动跑 100 轮 → 输出「检测到 N 次故障、恢复率 M%、无数据损坏」。

### 本轮推荐

**方向 14（服务治理原语）**：成本最低（2–3 周）、生产刚需（限流/熔断/舱壁/超时）、完全复用现有 Context/Channel/Timer；方向 15（混沌工程）差异化强但依赖注入框架。

## 第 6 轮（2026-10-23）：分布式追踪 / 极限规模

### 方向 16：协程级分布式追踪（OpenTelemetry 原生）

**价值**：让 trace 的传播**穿过协程边界**——`Context` 携带 trace_id/span_id，`Channel` 传递时自动注入，`Scheduler` 切换时不丢 span。C++ 的 OTel SDK 是线程局部 context，跨协程（Fiber 栈切换）会丢；本项目的 Context/Channel 是自研的，可以做到「协程感知的追踪」。

**复用**：`Context`（值传播）、`Channel`（消息携带 trace）、`Scheduler`（fiber 切换时恢复 TLS span）、事件环（span 事件）。

**难点**：
- Fiber 切换时 OTel context 的保存/恢复（TLS 跟随当前 Fiber）；
- 与标准 OTel SDK 的兼容（导出 OTLP）；
- 采样策略（head-based vs tail-based）。

**成本**：3–4 周；**风险**：OTel SDK 依赖（或手写 OTLP）。

**最小验证**：服务 A（HTTP/2）→ 协程等待 → 服务 B（HTTP/3），trace 贯穿两个服务 + 中间协程，OTLP 导出到 Jaeger 能看到完整链路。

### 方向 17：单机百万连接（demo + 内存优化）

**价值**：用现有 GMP + Fiber + growable 栈冲「单进程百万并发连接」——Go 的招牌能力在 C++ 生态里几乎没有；同时倒逼 s3 内存优化（当前 8000 连接 190MB vs epoll 54MB）。

**复用**：GMP/Fiber（百万协程）、epoll IO、growable 栈（4KB 初始）、Channel（连接管理）、诊断（百万协程的 dump/视图——正好检验可观测性极限）。

**难点**：
- 内存：每连接 4KB 栈 + 缓冲 → 百万 × ~2KB 目标（需共享栈/极小栈/缓冲池化）；
- fd 上限（ulimit）、内核参数（somaxconn/epoll）；
- 压测工具（多机 client）。

**成本**：3–5 周（含优化）；**风险**：内存/内核限制。

**最小验证**：单进程 100 万空闲连接（echo server）+ 内存 < 3GB；对比 epoll 状态机实现的连接数上限。

### 本轮推荐

**方向 17（百万连接）**：数字最震撼、倒逼真实优化（s3 内存）、且是 Go2Cpp 技术栈的极限展示；方向 16（分布式追踪）是生态刚需但依赖 OTel。

## 第 7 轮（2026-10-23）：数据访问 / 多租户

### 方向 18：协程友好的数据库驱动（MySQL / Redis）

**价值**：C++ 的 MySQL/Redis 客户端多是同步阻塞（如 libmysqlclient），在线程池里等连接/等结果会线程膨胀；用 Go2Cpp 的协程 + epoll 把「连接等待 → 查询 → 结果解析」全协程化，一个线程跑数千连接。对标 Go 的 `database/sql` 生态在 C++ 的空白。

**复用**：epoll IOManager（socket 等待）、Fiber（每连接一协程）、Channel（连接池）、Context（超时/取消）、Timer（连接保活）。

**难点**：
- 协议编解码（MySQL 协议/Redis RESP）——可先做 Redis（RESP 简单）；
- 连接池（协程安全的借还/健康检查）；
- 与现有 syscall hook 的配合（阻塞读转协程等待）。

**成本**：Redis 2–3 周 / MySQL 4–6 周；**风险**：协议复杂度（MySQL 握手/认证）。

**最小验证**：Redis 客户端：1000 并发 GET/SET，对比 hiredis 线程池（吞吐/线程数/内存）；MySQL：简单 SELECT 1 + 连接池。

### 方向 19：Serverless / FaaS 多租户运行时

**价值**：单进程内多租户（函数）各自跑在独立协程组，**各自热重载**、资源配额（CPU 时间/并发/内存）、故障隔离（一个租户的协程卡死不拖垮其他）。Cloudflare Workers 用 V8 isolate，C++ 没有等价物；Go2Cpp 的 Fiber + 热重载 + 调度器可以做到「协程级多租户」。

**复用**：Scheduler（租户级配额/隔离）、SO 热重载（租户代码热更新）、迁移（租户状态迁移）、Doctor（租户级诊断）、Context（超时/取消）。

**难点**：
- 租户隔离（协程调度配额、内存上限、崩溃隔离）；
- 热重载时在途请求（迁移）；
- 调用入口（HTTP/事件 → 函数）。

**成本**：6–10 周；**风险**：隔离强度（同进程内无法完全隔离）。

**最小验证**：两个租户（A 正常、B 死循环/卡死）→ A 不受影响（配额/超时隔离）+ 热更新 B 的 SO。

### 本轮推荐

**方向 18（Redis 驱动）**：成本最低（2–3 周）、实用直接（生产常见）、与现有 epoll/协程能力完全匹配；方向 19（多租户运行时）创造力强但成本高、隔离强度受限。

## 第 8 轮（2026-10-23）：收口（第 20 个）

### 方向 20：协程 API 网关（HTTP/1/2/3 + 路由 + 治理 + 热重载）

**价值**：把现有全部网络栈（HTTP/1、HTTP/2、HTTP/3、TLS、WebSocket）做成一个**生产级 API 网关**：路由、限流/熔断（方向 14）、可观测（Doctor/事件环/图）、SO 热重载（不停机升级）、协程级连接管理（每连接一协程，阻塞上游用协程等）。C++ 生态的网关（Envoy 是 C++ 但线程+事件驱动模型、扩展用 WASM）没有「Go 式协程 + 热重载」的组合。

**复用**：HTTP/1/2/3 栈、TLS、Fiber、Channel、Timer、服务治理原语（方向 14）、Doctor/事件环/graph、SO 热重载、迁移（方向 10）。

**难点**：
- 路由/上游负载均衡/健康检查；
- HTTP/2/3 与 HTTP/1 的统一抽象（流/连接/请求）；
- 热重载时的连接 draining（GOAWAY + 迁移）。

**成本**：6–10 周；**风险**：范围大（但可 MVP：HTTP/1 + 路由 + 限流 + 热重载）。

**最小验证**：网关转发 HTTP/1 请求到上游（协程化），加限流（1000 QPS）+ 热重载（改路由不重启）+ Doctor 看连接/协程状态。

### 收口说明

至此 **20 个方向**（第 8 轮结束，按用户要求封顶）。后续若要落地，建议优先级：**方向 18（Redis 驱动，2–3 周）> 方向 14（服务治理，2–3 周）> 方向 12（pprof 剖析，2–3 周）> 方向 4（AI 诊断，2–3 周）**——都是低成本、复用现有能力、可快速验证的。
