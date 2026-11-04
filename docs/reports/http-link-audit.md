# HTTP 链路 bug 全清单（应用驱动排查，2026-11-04）

> 触发：抖音上传 6 MB+ 视频经 gateway 全 502。
> 方法：症状 → 按链路拆 4 环节 → 逐环节只读排查 + 最小复现 → 修 → 回归。
> 结论：一次应用故障暴露 **34 个底层问题**，其中 1 个数据损坏、5 个高危崩溃/DoS、4 个数量级性能缺陷。

## 一、总览

| 环节 | 条数 | 已修 | 待修 | 已排除/设计 |
|---|---:|---:|---:|---:|
| `http::Parser` | 8 | 5 | 1 | 2 |
| `app::gateway`（HttpServer 连接循环） | 10 | 3 | 5 | 2 |
| `http::Client`（转发） | 7 | 2 | 3 | 2 |
| `douyin_http`（6 个业务服务） | 9 | 4 | 3 | 2 |
| **合计** | **34** | **14** | **12** | **8** |

## 二、Parser（8 条）

| # | 级别 | 问题 | 位置 | 状态 |
|---|---|---|---|---|
| P1 | 高 | 增量 feed 重复追加 header（6 MB body → 1280 份、wire 头 147 KB） | `http.cpp` header 解析段 | ✅ `9f7ab5c`（clear 重建）+ `19562e9` 回归 |
| P2 | 高 | 头部每次 feed 重解析（64 KiB 头 + 1 MiB body 分片 **66.3 ms**） | `http.cpp:518-558` | ✅ `d538326`（`m_header_parsed` 快路径，**1.95 ms**） |
| P3 | 高 | chunked 每次 feed 从头重扫（6 MiB **4017 ms**） | `http.cpp:585-621` | ✅ `e84bbec`（游标复用，**17 ms**） |
| P4 | 中 | 零头 1xx 必失败（`100 Continue\r\n\r\n` → Parse(7)） | `http.cpp:469-471` | ✅ `e84bbec`（`line_end` 兜底） |
| P5 | 中 | `reset()` 漏 `m_response_to_head`（HEAD 后复用吞 body） | `http.cpp:389` | ✅ `be0d0a3` |
| P6 | 低 | trailer 不清空（复用 Request 残留） | header 解析段 | ✅ `be0d0a3` |
| P7 | 低 | 304+CL / HEAD+TE 合法响应被拒 | `http.cpp:562-567` | ✅ `be0d0a3`（RFC 7230 §3.3.3） |
| P8 | — | `consumed` 语义不一致 | `http.cpp:629/632` | ✅ `be0d0a3`（改为实际消费量） |

## 三、app::gateway（10 条）

| # | 级别 | 问题 | 位置 | 状态 |
|---|---|---|---|---|
| G1 | 高 | Dispatch 无 try/catch → handler throw **SIGABRT** | `gateway.cpp:1203` | ✅ `76f47be`（500 + 计数） |
| G2 | 高 | 慢连接独占 worker → 全站 DoS（2 条半开 → 29.7 s） | `gateway.cpp:1140-1181` | ✅ `76f47be`（`request_timeout` 10 s → 9.7 s） |
| G3 | 中 | pipeline 静默丢弃（3 请求只回 1） | `gateway.cpp:1175-1211` | ✅ `76f47be`（同包流水线 1→3） |
| G4 | 中 | 无 `Expect: 100-continue` 响应 | — | ⏳ 待修 |
| G5 | 中 | 无 101/Upgrade 隧道（`PrepareOutgoing` 删 Upgrade） | `gateway.cpp:1013` | ⏳ 待修 |
| G6 | 低 | 背压满 256 直接 close 无响应 | `gateway.cpp:1528` | ⏳ 待修 |
| G7 | 低 | parser Error 不回 400 | — | ⏳ 待修 |
| G8 | 低 | 30 s 是单次 recv 空闲超时（slowloris 可拖） | `gateway.cpp:1181` | ✅ `76f47be` 已加总时限 |
| G9 | 低 | 库层 `serve_connection` 无 recv 超时 | `http.cpp:1509` | ⏳（网关未用） |
| G10 | 设计 | 连接一一占用 worker（无事件循环/池化） | 架构 | ⏳ 记 TODO |

## 四、http::Client（7 条）

| # | 级别 | 问题 | 位置 | 状态 |
|---|---|---|---|---|
| C1 | 高 | 零头 1xx 解析失败（gateway 遇上游 100 → 502） | 同 P4 | ✅ `e84bbec` |
| C2 | 高 | chunked O(n²)（6 MiB 211×） | 同 P3 | ✅ `e84bbec` |
| C3 | 中低 | 空 body POST 不补 `Content-Length: 0` | `http.cpp:313` | ⏳ 待修 |
| C4 | 低 | `classify_io` 不映射 `ECONNRESET/EPIPE` | `http.cpp:687` | ⏳ 待修 |
| C5 | 低 | connect 失败用 errno 兜底（fiber 迁移后可能旧值） | `http.cpp` | ⏳ 待确认 |
| C6 | 低 | 1xx 无计数上限（上游可无限 1xx） | `do_request` | ⏳ 待修 |
| C7 | 设计 | 101 返回后立即关连接（upgrade 转发不可用） | — | ⏳ 记 TODO |

## 五、douyin_http（9 条）

| # | 级别 | 问题 | 位置 | 状态 |
|---|---|---|---|---|
| D1 | **高（数据损坏）** | body 未读满按完整处理（CL 32 MB 发 1 KB → 200 + 1024 B 进 DB） | `douyin_http.hpp:359-375` + `video_service.cpp:303` | ✅ `71568c9`（400、0 文件、0 行） |
| D2 | 中 | DELETE 只删 DB 不删文件（孤儿 mp4） | `video_service.cpp:434-466` | ✅ `71568c9` |
| D3 | 中 | 重复 Content-Length 无冲突检测 | `douyin_http.hpp:167` | ✅ `71568c9`（400） |
| D4 | 中 | 超限静默 clamp 到 32 MB | `douyin_http.hpp:355-357` | ⏳ 待修 |
| D5 | 中 | 畸形请求行/70 KB 头 → 404（应 400/431） | `douyin_http.hpp:375-381` | ✅ `71568c9`（400/431） |
| D6 | 中 | 串行阻塞（慢上传期间其它请求 1.61 s） | `douyin_http.hpp:414-423` | ⏳ 待修 |
| D7 | 低 | chunked 不支持却报「请求体为空」 | — | ⏳ 待修 |
| D8 | 低 | 非法 CL 经 `atoll` 静默按 0 | — | ⏳ 待修 |
| D9 | 低 | pipeline 第二请求静默丢弃 | — | ⏳ 待修 |

## 六、影响面

| 问题 | 影响 |
|---|---|
| P1/P2/P3/P4 | **所有**经 gateway 转发的请求（不只抖音） |
| C1/C2 | 所有 `http::Client` 调用方（gateway 转发、上传 example、rpc 之外的 HTTP） |
| G1/G2 | 所有 gateway 实例（电商、抖音、任何用 app::gateway 的服务） |
| D1–D9 | 抖音 6 个业务服务（douyin_http 独立实现） |

## 七、回归索引

| Bug | 回归位置 |
|---|---|
| P1 | `tests/network/test_network.cpp`（6 MiB 分块 feed、header 计数） |
| P4/P3 | `tests/network/test_network.cpp`（空头 100、6 MiB chunked） |
| G1/G2/G3 | `tests/app/test_gateway.cpp`（62 checks，含 throw/slowloris/pipeline） |
| D1–D5 | `app/douyin/scripts/accept_video_feed.sh`（52 PASS） |
| 端到端 | `app/douyin/scripts/soak_matrix.sh`（断线重连/点赞风暴/slowloris） |

## 八、方法论（可复用）

1. **症状**：先拿到一个真实失败（本例：6 MB 上传 502），拿到最小复现；
2. **分层排除**：直连业务服务成功 → 问题在转发层（gateway/Client）；
3. **逐环节排查**：Parser / 连接循环 / Client / 应用层 HTTP，各派一个只读 agent；
4. **每环节三件**：读码 + 最小复现 + 前后对比；
5. **修一个补一条回归**，并在报告里留「影响面」——决定优先级的是影响面，不是难度。
