# 上传 error 复现与修复（2026-11-05）

## 结论

单用户上传 error 的根因不是并发，而是**网关读取整个请求的总时限 10 s**：它覆盖「请求头 + 完整 body」，
32 MiB 上限下要求 ≥3.2 MiB/s 的稳定上行。手机/家宽慢于此，网关在 body 中途直接关连接且**不写任何
HTTP 响应**，前端只能报 network error。高并发只是把 8 个 worker 同时占满 10 s，放大为全站不可用。

## 复现矩阵（真实网关代码 + 裸 socket，127.0.0.1）

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 1 MiB 正常 | 200 / 5 ms | 200 / 5 ms |
| 10 MiB 正常 | 200 / 47 ms | 200 / 47 ms |
| 32 MiB（等于上限） | 200 / 144 ms | 200 / 144 ms |
| 33 MiB | 400 | 400 |
| 64 MiB | 400 | 400 |
| CL 比实际大（声明 2 MiB，实发 1 MiB 后 half-close） | 无响应 / 1 ms / EOF | 同（框架层已知限制） |
| CL 比实际小（声明 512 KiB，实发 1 MiB） | 200（多余字节按 pipeline 处理） | 200 |
| chunked 1 MiB | 200 | 200 |
| **慢速 1 MiB（4 KiB/50 ms，需 12.8 s）** | **无响应，10.08 s 断开，仅发出 823,394 B** | **200 / 12.85 s** |
| **4 并发慢速 1 MiB** | **4/4 无响应，10.09 s** | **4/4 200，最慢 12.84 s** |
| 对照：request_timeout=2 s + 慢速 512 KiB | 2.06 s 断开（证明是时限触发，与数值无关） | —（已删，避免误导） |

## 代码路径（file:line）

- `include/go2cpp/app/gateway.hpp:358`：`Config::request_timeout{10000}`，注释写明「覆盖头部+body」。
- `app/douyin/src/gateway.cpp:777-781`：只设 worker/queue/health，**未覆盖** request_timeout；
  上传路由只把**上游转发**超时放大到 120 s（`gateway.cpp:837`），读取阶段仍是 10 s。
- `src/app/gateway.cpp:1300-1301`：每请求 deadline = now + request_timeout。
- `src/app/gateway.cpp:1370-1373`：到期 `request_timeouts++` 后**直接 return**，不回 400/408。
- 对照 `app/douyin/include/douyin_http.hpp:93`（video_service 的 30 s 总时限）有同样结构，
  但它读的是环回、不是瓶颈。

## 修法

`app/douyin/src/gateway.cpp`：`config.request_timeout = 120000ms`，与上传路由的上游超时（120 s）
对齐——273 KiB/s 即可传完 32 MiB。`LimitUploadSize`（32 MiB，gateway.cpp:306-334）与 Parser 上限不动。
回归测试：`app/douyin/tests/test_upload_timeout.cpp` + `app/douyin/scripts/run_upload_timeout_test.sh`
（真实 `src/app/gateway.cpp` + 裸 socket，覆盖上表全部场景，`-Werror` 零告警，failures=0（9 PASS + 1 INFO））。

## 遗留（属通用组件 `src/app/gateway.cpp`，本次未改）

1. body 短读 / 客户端中断时静默断连，不回 400——客户端无法区分「网关拒绝」与「网络故障」。
2. `LimitUploadSize` 在 body 完整读入后才判，33–64 MiB 会先读进内存再回 400（带宽/内存浪费）。
3. 每连接 worker 阻塞读 body，8 worker × 120 s 是高并发上传上限。根治方向：前端改用
   video_service 已有的分片接口 `/api/video/upload/init|chunk|finish`，或网关改流式转发。
