# HTTP 链路审计修复：Gateway 两个高危 + 流水线缺陷

- 审计基线：`src/app/gateway.cpp` @ HEAD `19562e9`
- 修复日期：2026-11-04
- 范围：仅网关生产代码（`src/app/gateway.cpp` + `include/go2cpp/app/gateway.hpp`）与
  `tests/app/test_gateway.cpp` 测试；未 commit，未改动本轮无关文件。

## 结论（先看这里）

| 编号 | 缺陷 | 修复前实测 | 修复后实测 | 状态 |
|---|---|---|---|---|
| TOP1（高） | `Dispatch`/`WorkerLoop` 无异常保护，handler `throw` → `std::terminate` → 进程 SIGABRT | `terminate called after throwing ... Aborted (core dumped)` | HTTP 500，进程存活，后续 `/health` 仍 200 | 已修 |
| TOP2（高） | 慢连接独占 worker；30s 是**单次 recv** 空闲超时，可被 slowloris 无限续期 | `worker=2` + 2 条半开连接，新请求 **29773 ms** 才响应 | 默认总时限 10s：**9712 ms**；配置 800ms：**601 ms** | 已修（有界） |
| TOP3（中） | `pending` 恒空、`take_remaining()` 从未调用，一次 send 3 个请求只回 1 个 | 3 请求 → 1 响应 | 3 请求 → **3 响应** | 已修（同包流水线） |

## TOP1：Dispatch 异常兜底

**根因**：`ServeConnection` 直接调用 `Dispatch`，`WorkerLoop` 直接调用
`ServeConnection`；自定义 handler / 中间件抛出的异常没有任何 `catch`，穿过
`std::thread` 入口触发 `std::terminate`。

**修法**（`src/app/gateway.cpp`）：
- `ServeConnection` 内 `Dispatch` 外包 `try/catch (const std::exception&)` +
  `catch (...)`：记 `internal_errors`、日志、返回 `500 internal server error`，
  并置 `response.close = true`（异常后协议状态不可信，必须断开）；
- `WorkerLoop` 调用点再包一层 `try/catch` 兜底（覆盖序列化/写回等边缘路径），
  捕获后计数、打印日志，并尽力写一条最小 500 响应再关闭连接；
- `Stats` 新增 `internal_errors` 计数（`include/go2cpp/app/gateway.hpp:378`）。

**验证**：注册一个 `throw std::runtime_error` 的自定义路由后 `GET /boom`：
- 修复前：`terminate called after throwing an instance of 'std::runtime_error'`
  → `Aborted (core dumped)`；
- 修复后：`status=500`、错误体 `{"code":500,...}`、`internal_errors >= 1`，
  紧接一次 `/health` 仍 200（进程未退出）。

## TOP2：请求级总时限（替代单次 recv 30s）

**根因**：`connection->recv(..., 30000ms)` 每次 recv 重新计时。slowloris 每隔
<30s 发一个字节即可永久占住 worker；`worker_threads=4` 时 4 条半开连接打满全站。

**修法**：
- `Config` 新增 `request_timeout{10000}`（`gateway.hpp:352`），0 在 `Create`
  归一化为 10s；
- `ServeConnection` 每个请求在**第一次 recv 前**建立
  `deadline = steady_clock::now() + request_timeout`，覆盖头部 + body；
- 每次 recv 的超时取 `min(deadline - now, 30s)`（`gateway.cpp:1205` 起）。
  每次 recv 后都回到同一 deadline，慢连接无法续期；到期计数
  `request_timeouts` 并直接关闭；
- 仍保留 100 次/连接上限，防止单连接恶意流水线长期占用 worker。

**验证**（`worker_threads=2`，2 条只发 `"GET /health HTTP/1.1\r\nHost: slow\r\n"`
半开连接，200ms 后发正常 `/health`）：
- 修复前：**29773 ms**（≈30s 单次 recv 超时释放 worker）；
- 修复后默认 10s：**9712 ms**；
- 测试配置 `request_timeout=800ms`：**601 ms**，且 `request_timeouts >= 1`。

**边界说明**：本修复把「无限」压到「≤ 总时限」，但线程/连接一一占用的架构仍在：
worker 满时新连接要等到某个慢连接被淘汰。彻底消除需要事件循环/连接池化
（见 TODO），本轮按任务要求先做时限与快速淘汰。

## TOP3：同包流水线不再丢请求

**根因**：解析完第一个请求后没有调用 `parser.take_remaining()`，同一 recv 里
后续请求的字节被丢弃；`pending` 分支是死代码。

**修法**：`ServeConnection` 改为**流水线排空循环**：
- 每轮用新 `Parser` 解析一个请求；解析完成后
  `pending = parser.take_remaining()` 交给下一轮；
- 只有缓冲里确实还有下一条请求时才继续处理并回
  `Connection: keep-alive`，最后一条回 `Connection: close`；
- 不做跨包空闲 keep-alive 等待：空闲连接若继续占 worker，会以新的方式放大
  slowloris 风险；客户端后续请求重连即可；
- `SerializeResponse` 的 `Connection` 头改为跟随 `Response::close`，
  保证响应头与实际关闭行为一致。

**验证**：一条连接一次 `send` 三个 `GET /health`，读到 EOF：
修复前 1 个 `HTTP/1.1 200`，修复后 **3 个**。

## 验证与回归

```bash
cd /UserData/CodexWorkSpace/Go2cpp
tests/app/build_and_run_gateway.sh
```

- 编译：`-O2 -g -Wall -Wextra -Wpedantic -Werror`，**0 warning**（`grep -ci warning` = 0）；
- 结果：`checks=62 failures=0`（原有 50 项 + 本次新增 12 项）；
- 覆盖：原有路由/鉴权/限流/404/透传/504/502/健康聚合/Resolver/LoadConfig/1000 次压测
  全部保持通过；新增 throw、slowloris、pipeline 三组修复验证；
- `test_gateway` 未注册进 CMake/CTest（审计前既存问题），故按任务要求直接运行
  `tests/app/build_and_run_gateway.sh`。

## TODO / 未覆盖

1. **handler 执行时间不计入 `request_timeout`**：总时限目前覆盖读取阶段；
   慢 handler 仍占 worker，需配合上游超时或异步化。
2. **连接模型仍是 thread-per-connection**：建议后续改事件循环或连接池，
   把「连接占用」与「请求处理」解耦；届时可安全启用跨包 keep-alive。
3. **`test_gateway` 未进 CTest**：建议在 `cmake/Go2CppApp.cmake` 注册
   `add_test(NAME app_gateway ...)`，否则 CI 不会跑这 62 项。
4. **跨包 keep-alive 明确未启用**：本轮的「流水线」只消费已到达字节；
   若产品需要连接复用，需先解决空闲连接占 worker 的问题。
5. **pipeline 中间件语义**：同一连接内多个请求各自独立走完整中间件链/装饰器，
   与单请求行为一致；未做请求间共享状态测试。
