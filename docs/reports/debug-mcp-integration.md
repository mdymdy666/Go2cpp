# Go2Cpp Debug × MCP 联调报告

## 结论

采用**方案 B（DebugServer 内置 HTTP）**：MCP 侧复用 `go2cpp::http::Get`，Debug 侧复用既有
组件注册表与 `Dump()`，不新增观测代码；Unix socket 协议保留，方案 A 作为 fallback。端口默认
**39101**（`GO2CPP_DEBUG_HTTP_PORT` 覆盖，`0`=禁用）：任务书的 9101 与 MCP 监控 Manager 端口
冲突，按铁律改落 39xxx 段。

## 架构

```text
┌──────────────── MCP 客户端（AI）────────────────┐
│ go2cpp_debug_fibers / _scheduler / _sync /      │
│ _resources / _events / _dump（6 个工具）         │
└───────────────────────┬─────────────────────────┘
                        │ HTTP GET 127.0.0.1:39101（http::Get，默认 3s）
                        ▼
┌──────────────── 目标 Go2Cpp 进程 ────────────────┐
│ DebugServer::Start()                             │
│   ├─ Unix socket /tmp/go2cpp-<pid>.sock（既有）  │
│   └─ HTTP 线程（单线程 accept，每连接 1s 超时）  │
│        ├─ /debug/fibers    DebugRegistry + scheduler dump
│        ├─ /debug/scheduler 已注册 scheduler 组件
│        ├─ /debug/sync      mutex/CV/parking dump
│        ├─ /debug/resources /proc/self + 资源注册表
│        ├─ /debug/events    EventRing::DumpJson(limit)
│        └─ /debug/dump      Dump("all")
└──────────────────────────────────────────────────┘
```

## 端点表

| 端点 | 数据源 | 关键字段 | 边界 |
|---|---|---|---|
| `/debug/fibers` | `DebugRegistry` + scheduler/fiber 组件 | `fiber_count`、`fibers[]`、`scheduler_summary.tasks` | 无数据时带 `hint` |
| `/debug/scheduler` | schema 含 `scheduler` 的组件 dump | `processor_count`、`worker_count`、`runnable_total`、`tasks` | 未注册时 `available=false` |
| `/debug/sync` | mutex/condition/parking/waitgroup 组件 | `primitives`、`total_waiters`、`held`、`native_waiters` | 只读，不等待锁 |
| `/debug/resources` | `/proc/self/status`、`/proc/self/fd`、`getrlimit` | `vm_rss_kb`、`threads`、`fd_count`、`by_kind` | 采集失败字段缺省 |
| `/debug/events` | `EventRing::DumpJson` | `published_events`、`events.events[]` | `?limit=` 1..1024，默认 64 |
| `/debug/dump` | `Dump("all")` | 全部组件 JSON | `?format=text` 附带 text 段 |

响应统一带 `schema`/`version`/`pid`/`steady_ns`/`uptime_ns`；未知路径 404，非 GET/HEAD 405。

## MCP 工具（`mcp/src/tools/go2cpp_debug_tools.cpp`）

| 工具 | 端点 | 参数 |
|---|---|---|
| `go2cpp_debug_fibers` | `/debug/fibers` | `base_url`、`timeout_ms` |
| `go2cpp_debug_scheduler` | `/debug/scheduler` | 同上 |
| `go2cpp_debug_sync` | `/debug/sync` | 同上 |
| `go2cpp_debug_resources` | `/debug/resources` | 同上 |
| `go2cpp_debug_events` | `/debug/events` | +`limit` |
| `go2cpp_debug_dump` | `/debug/dump` | +`limit`、`format` |

默认地址取 `GO2CPP_DEBUG_HTTP_URL`，否则 `http://127.0.0.1:<GO2CPP_DEBUG_HTTP_PORT|39101>`；返回服务端 JSON 原文，连接失败/非 2xx/超 8 MiB 转 `isError` 中文诊断。

## 联调结果（2026-11-05，真实进程）

联调程序：2P/2W Scheduler + 4 个长期 fiber + 3 个 DebugFiber（DebugRegistry）+ 持锁/等锁线程。

- `/debug/fibers`：`fiber_count=3`、`registry_count=3`、2P/2W、`runnable=2`；`/debug/scheduler`：`available=true`、`machines=2`；
- `/debug/sync`：`primitives=1`、`total_waiters=1`、`sync.main.held=true`、`native_waiters=1`；
- `/debug/resources`：`vm_rss_kb=8664`、`threads=7`、`fd_count=10`、`fd_limit=10240`、`by_kind={kMutex:1,kParkingCondition:1}`；`/debug/events`：`published_events=75`、返回 5 条；
- `/debug/dump`：4 个组件（events/runtime/scheduler/sync.main）；MCP 真实调用（`Registry::InvokeTool`）6/6 成功（697..64344 字节）；并发 `/debug/sync` 8 线程 × 80 请求全部 200，wall 0.183s，p50 5.7ms、p95 149ms、max 154ms；socket `/tmp/go2cpp-6647.sock` 仍在。

已知边界：本实例 `scheduler_summary.tasks=0`（G 仍在 runqueue，`metrics.task_runs=0`），`fiber_count` 以 DebugRegistry 为准，两者都如实暴露、不互相掩盖。

## AI 使用示例

```text
1) go2cpp_debug_resources  → 确认进程活着、RSS/线程/fd 是否异常
2) go2cpp_debug_scheduler  → 读 runqueue 与 metrics，判断是否积压
3) go2cpp_debug_sync       → total_waiters>0 且 held=true → 锁竞争/死锁候选
4) go2cpp_debug_events {"limit":64} → 还原 park/wake/runqueue 时序
5) go2cpp_debug_dump       → 需要全量现场时一次取齐
```

参数示例：`{"base_url":"http://127.0.0.1:39101","limit":64}`；未开
`GO2CPP_DEBUG_DUMP=ON` 时工具返回连接失败，并提示检查构建开关与端口。
