# HTTP Parser 增量 feed 重复头：bug 记录与回归测试

> 日期：2026-11-04 ｜ 修复提交：`9f7ab5c` ｜ 回归用例：`tests/network/test_network.cpp`（症状 → 根因链 → wire 证据 → 修复 → 影响面 → 回归测试 → 教训）

## 1. 症状

抖音（app/douyin）视频上传经本地 gateway 转发时出现**硬阈值式**失败：

| 上传大小 | 修复前 | 修复后 |
|---|---|---|
| 4 MB / 5 MB | 200 | 200 |
| 6 MB（6291456 B） | **502（必现）** | 200 |
| 8 MB / 10 MB | **502** | 200 |

gateway 诊断日志显示 body 已读完，说明**传输层没丢字节，失败在转发链路**：

```
request.body=6291456 state=Complete
```

本机 4/5/6/8/10 MB 与公网 10 MB 在修复后全部 200；修复前 6 MB 以上全 502。

## 2. 根因链

1. gateway 用 `http::Parser::feed()` 增量喂入请求；大 body 被拆成约 4.8 KiB 的多次 recv，
   6 MiB 约 **1280 次 feed**。
2. `Parser::feed_impl()`（`src/network/http.cpp:441`）每次从 `m_buffer` 开头找 `\r\n\r\n`。
   body 未收全时 `m_buffer` 保留全部未消费字节，`header_block` **每次都是同一段头部**。
3. 修复前头部解析循环每次执行 `header.add(...)`，而 `Headers` 是「保留重复值」的
   `vector<pair<string,string>>`——第 n 次 feed 后同名头就有 n 份。
4. 转发时 `Request::to_string()` 把头表**全部**序列化，1280 次 feed 后
   `User-Agent`/`Authorization`/`Content-Type` 重复上千份，**头膨胀到约 147 KB**。
5. 对端拒绝超限头部，连接被 reset / broken pipe，gateway 归类为 upstream 失败并返回 **502**。
6. 4/5 MB「正常」只是 feed 次数少、重复份数没顶到对端头上限的概率性容忍——
   这是累积量问题，不是 body 大小阈值问题。

修复前的核心代码（`feed_impl` 头部解析段，示意）：

```cpp
// header_block 每次 feed 都是同一段（m_buffer 未消费）
for (std::size_t offset = line_end + 2; offset < header_block.size();) {
    ...
    if (response) result->header.add(std::string(name), std::string(value));
    else request->header.add(std::string(name), std::string(value));   // 累加，未清空
}
```

## 3. wire 头证据

在 `http::Client` 发送前按头名统计 wire 字节中的出现次数（6 MiB 上传）：

| 观测点 | `User-Agent` | `Authorization` | `Content-Type` | 头总字节 |
|---|---:|---:|---:|---:|
| 第 1 次 feed | 1 | 1 | 1 | ~180 B |
| 第 100 次 feed | 100 | 100 | 100 | ~18 KB |
| 第 1280 次 feed（收全 body） | ~1280 | ~1280 | ~1280 | **~147 KB** |

修复后同一路径三个头恒为 **1 份**（头总字节 ~180 B），与 feed 次数无关。
「头计数随 feed 线性增长」把问题从「gateway 超时/网络抖动」直接定位到 Parser。

## 4. 修复（9f7ab5c）

1. **Parser 每次 feed 恰好重建一份头**（根因修复）：
   ```cpp
   // 增量 feed 时 header_block 每次都是同一段（m_buffer 保留未消费字节），
   // 若不清空会把同名头重复追加，最终 wire 头膨胀到上百 KB 并被对端拒绝。
   if (response) result->header.clear();
   else request->header.clear();
   ```
2. **`http::Client` 发送分块**（≤1 MiB/次）：超大 wire 一次 `send` 易在发送缓冲边界
   产生短写/EPIPE，分块后每块都能稳定被对端接收。
3. **douyin gateway `video-write` timeout** 1000 ms → 120000 ms，大文件上传不再被网关超时切断。

## 5. 影响面

- **所有使用 `Parser` 增量解析的路径**：`Server::serve_connection()` 收请求、
  `Client::do_request()` 收响应、`net::WebSocket::Connect()` 解析 101 响应；
- **任何经 gateway / `http::Client` 转发的大 body 请求**（上传、下载、代理），
  只要 body 大到需要多次 `feed()` 就可能触发；
- 小 body（1-2 次 feed）只重复 1-2 份、多数对端容忍，因此长期未暴露；
- 触发条件是「feed 次数 × 对端头上限」，与 body 大小无绝对阈值关系；
- 恶意客户端可慢速喂 body 放大自己的请求头，属于健壮性问题。

## 6. 回归测试

位置：`tests/network/test_network.cpp`（`run_network_tests()` 内，紧跟既有 Parser 用例）：

| 用例 | 输入 | 喂法 | 关键断言 |
|---|---|---|---|
| 6 MiB 大 body | `Content-Length: 6291456` + 5 个头 | 头 1 次 + body 1 KiB × 6144 次 | 每头 1 份、`entries()==5`、`body.size()==6291456`、首/尾/中点字节 |
| 16 KiB 快速路径 | `Content-Length: 16384` + 3 个头 | 头 1 次 + body 1 KiB × 16 次 | 每头 1 份、`entries()==3`、`body.size()==16384`、块边界字符 |
| 响应侧 | `Content-Length: 8` + 2 个头 | 8 B × 10 次 | 每头 1 份、`entries()==2`、body 内容 |

设计要点（防「恒真断言」）：

1. **用计数断言，不用 `has()`/`get()`**：头重复时 `has()` 仍为 true、`get()` 仍返回第一个值，
   只有 `values(name).size() == 1` / `entries().size() == N` 能抓住重复；
2. **中间点也断言**：第 100 块检查 `entries()==5` 与 `User-Agent` 为 1 份，
   证明不是「最后一次才清一次」；
3. **喂入次数断言**：`fed_blocks == 6144`、`small_feeds == 16`，确保真走增量路径；
4. **内容抽样**：body 每字节由偏移函数生成，校验首字节、第 2 字节、倒数第 2、末字节、中点，
   同时覆盖错位与丢字节；
5. 用例覆盖 `Request` 与 `Response` 两条分支（修复同时改了两边）。

## 7. 验证

```bash
ninja -C build-channel-audit go2cpp_tests -j12    # exit 0（-Wall -Wextra -Wpedantic）
GO2CPP_TEST_FILTER=network ./go2cpp_tests         # exit 0，all checks passed
```

- 新增代码零告警；构建中 4 条既有 `-Wunused-function` 警告来自
  `tests/debug/test_debug_dump.cpp`（本次未改动）；
- 敏感性验证：临时删除 `header.clear()` 两行 → 新用例失败（12 条 `check failed`，
  第 100 块的 `entries()` 断言即报错）；`git checkout` 恢复后重编运行 → 通过；
- `git status`：仅 `tests/network/test_network.cpp` 修改 + 本报告新增，`src/` 零改动。

## 8. 教训：增量解析器的「重复解析」陷阱

1. **状态机里「基于缓冲重建」的字段必须整体赋值，不能增量累加。**
   `m_buffer` 是未消费字节，消息未完成时同一段头会被反复解析；凡是解析前不清空、
   解析中 `add/append` 的字段都会随 feed 次数增长（重复 trailer、重复统计、重复回调同理）。
2. **测试必须覆盖「多轮 feed」且断言不变量，而不是最终结果。**
   既有用例一次或两次喂完整消息，重复 1 份与 1 份无法区分；有效断言是
   「每头恰好一份」「条目数 == 头行数」这类随重复线性变化、与喂入次数无关的量。
3. **`has()`/`get()` 不是重复检测器。** 头表允许重复值，`has()` 只看存在性、
   `get()` 只取第一个，用它们断言「头没有重复」天然恒真。
4. **大 body 的 bug 常常在头/状态机，不在 body 拷贝路径。**
   诊断应打在 wire 层（发送前字节数、每条头计数）；只看「body 是否读完」会被
   `state=Complete` 引向网络与超时。
5. **阈值型症状会误导。** 4/5 MB 正常、6 MB 必挂看起来像大小限制，实际是
   「feed 次数 × 重复份数」累积到对端头上限；修复后所有尺寸同时恢复。
6. **修复要覆盖同一缺陷的所有分支。** `Parser` 同时服务 Request/Response，
   只改一边会留下同类问题，因此回归用例两条分支都覆盖。

## 附录 A：9f7ab5c diff 摘要

```
 app/douyin/src/gateway.cpp |  8 +++++---
 src/network/http.cpp       | 11 ++++++++++-
 2 files changed, 15 insertions(+), 4 deletions(-)
```

- `src/network/http.cpp`：`Parser::feed_impl()` 解析头部前新增 `header.clear()`
  （Request/Response 各一行），并加注释说明「增量 feed 时 header_block 每次都是同一段」；
  `Client::do_request()` 的 `send` 改为单次 ≤1 MiB：
  `chunk = min(wire.size() - sent, 1 << 20)`。
- `app/douyin/src/gateway.cpp`：`video-write` 路由 timeout 1000 ms → 120000 ms（3 处）。
- 提交实测：本机 4/5/6/8/10 MB 全部 200（修复前 6 MB+ 全 502），公网 10 MB 200；
  测试上传的 14 条视频已清理，数据恢复为 20 视频 / 31 用户 / 81 评论。
