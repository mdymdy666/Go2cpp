# user 文档 API 用法全量复查（2026-11-05）

## 范围与方法

- 核对 **46 篇**：`user/deep/*.md` 18 篇、`user/modules/*/README.md` 11 篇、`user/docs/*.md` 17 篇。
- 以**当前源码**为准逐项复核：`scheduler/scheduler.hpp`、`concurrency/channel.hpp`、
  `network/socket.hpp`、`network/http.hpp`（Parser）、`tools/go.hpp`、`app/gateway.hpp`。
- 只改 `user/` 文档，未改 `src/`；每处改动标「2026-11-05 修正」。

## 结论

| 指标 | 数量 |
|---|---:|
| 核对文档 | 46 |
| 发现不一致 | 20 |
| 已改 | 20 |
| 无法验证 | 0（gateway 3 项在 user/ 无描述，见「未覆盖」） |

## 不一致明细（按 API）

| API | 数量 | 旧描述 → 修正 |
|---|---:|---|
| Scheduler / 默认值 | 7 | `Scheduler s(N)` 示例改默认单例（3）；`fiber_stack_size` 2048/2 KiB → 128 KiB（2）；Fiber 栈「固定不增长」→ 默认 growable=true（1）；构造注释补「高级用法」（1） |
| `go()` 判空 | 3 | 3 个示例 `go(...)` 直接 `wait()` → 增加 `if (!task) return 1;`（shutdown 后返回 nullptr） |
| Channel | 3 | `ChannelSendResult Close()` → `ChannelCloseResult`（1）；手动 Fiber `kInvalid` → `kNoScheduler`（2） |
| Parser | 4 | `consumed` 误写为「本次喂入字节数」→「本条消息在缓冲中占用字节数，跨 feed 累积」（3）；`parser.Complete()` → `ParseResult.state` / `finished()`（1） |
| Socket | 2 | API 清单缺 `SendAll`（1）；缺超时三态：max=回落配置、0=立即、负数=EINVAL（1） |
| Address | 1 | `local_address()->port()` → `dynamic_pointer_cast<net::IPAddress>` 后取 port（1） |

## 验证

**编译验证 5/5 PASS**（`g++ -std=c++17 -fsyntax-only -I include`）：

| 片段 | 来源 | 结果 |
|---|---|---|
| s1 | `user/docs/01-concurrency.md` 最小用法 | PASS |
| s2 | `user/docs/02-network.md` 最小用法 | PASS |
| s3 | `modules/concurrency/README.md` go 判空 | PASS |
| s4 | Channel `Close`/`SendOrThrow`/`kNoScheduler` | PASS |
| s5 | Parser `consumed`/`take_remaining`/`SendAll`/`recv(0)` | PASS |

**docs_consistency：未通过（4 项断言）**，与本次改动无关：
- 测试在 `/UserData/build/go2cpp-mcp-integrated` 运行，失败项为 `mcp/README.md` 声明 33 个工具
  而实际 29、`docs/testing.md` profile 数 21 对 32、测试地图缺 18 条用例；
- `mcp/README.md` 与 `docs/testing.md` 的 `git status` 干净，属**仓库基线问题**，
  按铁律（只改 user/ 文档）未修改，建议另开任务修复。

## 未覆盖（user/ 无对应描述，无可改）

- `Config::request_timeout` 默认 10000 ms：`modules/http/README.md` 已正确（10s）。
- `ShouldSendContinue`：`src/app/gateway.cpp` 内部静态函数，不是公开 API。
- gateway metrics（`request_timeouts` 等）：user/ 文档未描述，无过时说法。

## 改动文件

```
 6 files changed, 61 insertions(+), 29 deletions(-)
```

## 后续

1. 修复 docs_consistency 基线（mcp/README.md 工具数、docs/testing.md 测试地图）。
2. 其余模块 README 的 C++ 示例（.cpp）不在本次「文档」范围，未逐行编译。
