# Parser chunked +35% 定位与修复（2026-11-05）

## 1. 结论（先给结果）

- **不是** e84bbec 之后某次提交的解析逻辑回归：`d538326` 与 `HEAD` 在同一独立基准上无差异（15.5 / 15.0 ms）。
- 真实原因是 **chunked 增量解析把整条已收 wire 留在 `m_buffer`**：6 MiB body → 6.3 MiB wire 一直驻留，
  每次 1 KiB feed 都触发 `std::string` 扩容与整体 memcpy（累计约 2× 总量），并把解析期间工作集推高 6 MiB 以上。
  close 路径只累积不解析，最后一次 `assign`，所以对照组（9→10 ms）没有同等开销。
- 修法：**NeedMore 时擦除已解析前缀**（`m_chunk_erased` 累计），`consumed` 仍返回整条报文长度。
  效果：gtest 口径 **24 → 11 ms**（5 轮中位，目标 ≤20），独立基准 **15.0 → 10.1 ms**。

## 2. 定位过程

1. `git log --oneline -8 -- src/network/http.cpp`：`f62601e / a5bf9db / 6cc5524 / c2069ae / be0d0a3 / d538326 / e84bbec`。
2. `git diff e84bbec..HEAD -- src/network/http.cpp`（214+/40−）：chunked 分支只多出 `m_header_parsed` 快路径
   与 framing 快照复位，`m_chunk_scan_pos`/`m_chunked_body` 的语义没有改动。
3. **关键发现**：e84bbec 的 `http.cpp` 引用 `m_chunk_scan_pos`/`m_chunked_body`，但 e84bbec 的
   `include/go2cpp/network/http.hpp` 只有 `m_expected_body`/`m_chunk_remaining`——**该 commit 单独 checkout
   编译不过**，所以「17 ms 基线」无法在仓库内复现；`d538326` 是第一个可编译的版本（字段在此时补齐）。
4. 独立基准（只链接 `http.cpp` 的 Parser，微秒计时，`taskset -c 6`）：`d538326` 15.5 ms vs `HEAD` 15.0 ms
   （交替各 2 组 × 5 轮）→ **源码层面无回归**；参考值 17 ms 与 15 ms 属同量级。
5. gtest 口径 24 ms 与独立基准 15 ms 的差来自过程常驻内存：该用例里 `close_response`/`chunked_response`
   两份 6 MiB body 与两份 wire 同时存活，Parser 再累积 6.3 MiB → 约 31 MiB 工作集，memcpy 全部压 L3 之外。
   这一步解释了「close 一致、chunked 单独慢 9 ms」而不是「整机慢」。

## 3. 修改（只动 3 个文件）

- `include/go2cpp/network/http.hpp`：新增 `m_chunk_erased`（已擦除字节数）、`m_chunk_cursor_valid`
  （区分「游标未开始」与「相对偏移恰为 0」）。
- `src/network/http.cpp`：
  - 游标初始化由 `m_chunk_scan_pos == 0` 改为 `!m_chunk_cursor_valid`（擦除后 0 是合法偏移）；
  - 3 个 NeedMore 返回点调用 `trim_parsed_prefix(chunk_line_start)`：`erase(0, keep_from)` + 累计擦除量；
  - `Complete` 时 `consumed = m_chunk_erased + body_end`，缓冲内只 `erase(0, body_end)`；
  - `reset()` 与消息完成路径清零新字段，跨消息复用不残留。
- `tests/network/test_network.cpp`：逐字节喂 chunked（含 trailer）两组用例，断言
  `consumed == wire.size()`、`body`/`trailer` 正确——锁定「擦除前缀后 consumed 仍为整条报文长度」的契约。

## 4. 前后数据

| 口径（6 MiB body / 1 KiB chunk / 1 KiB feed） | 修前 | 修后 |
|---|---|---|
| `go2cpp_tests` 5 轮（ms） | 24 / 24 / 23 / 22 / 23 → 中位 **23–24** | 10 / 11 / 10 / 13 / 13 → 中位 **11** |
| close-delimited 对照（ms） | 9 | 10–12（中位 11） |
| 独立基准 5 轮中位（µs） | 15000 | 10113 |

- 修复后 chunked 中位 **11 ms**，目标 ≤20 ms 达成；也比 e84bbec 声称的 17 ms 基线低 35%。
- 零告警：`g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic` 编译 `http.cpp` 无输出。
- `go2cpp_tests`：`stats.failed=0`。

## 5. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
bash scripts/wait_low_load.sh 6
taskset -c 6 ./.build-bento-wsl/go2cpp_tests 2>&1 | grep 'chunked 6MiB'
```

未提交（按要求 `git status` 保留工作区改动）：`src/network/http.cpp`、`include/go2cpp/network/http.hpp`、
`tests/network/test_network.cpp`。
