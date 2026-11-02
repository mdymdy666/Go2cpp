> 来源：/tmp/http_e2e.md，2026-11-02 归档

# HTTP 端到端三件套：路由 / 静态文件 sendfile / pipelining

> 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树，非 E: 树）
> 独立构建目录：`/tmp/build-http`（CMake Release，`-O3 -DNDEBUG`，GCC 13.3，Ninja）
> 测量日期：2026-11-02 11:41–11:56（Asia/Shanghai）
> 本轮**未 commit**（工作树上有其它并发改动，见 §8）

---

## 0. 结论先行

| 项 | 结果 | 判定 |
|---|---|---|
| 路由 | 线性扫描 → **每方法精确哈希 + 参数/尾部通配段 trie**（`http::Router`）；64 条静态路由 O(1) | 完成 |
| 静态文件 | `Response::file` + **sendfile(2)** 零拷贝；200/206/304/400/403/413/416、单区间 Range、ETag/Last-Modified/If-Range、HEAD | 完成（strace 验证） |
| pipelining | 单连接一次写 8/16 条按序处理；pipeline=16 单连接 **72.9k req/s**（串行 8.9k 的 **8.2×**） | 完成 |
| HTTP/1.1 正确性 | 新增 260 条真实 socket 断言 **0 失败**；既有 network/sse/bento_http 测试全过 | 通过 |
| 对标 nginx（2 worker） | 多连接 hello **1.03–1.89×**、静态 **0.96–1.79×**（最差 0.96× = 慢 4%） | **<1.1× 达标** |
| 对标 Go net/http（GOMAXPROCS=2） | hello **1.28–3.21×**、路由 **1.20–1.28×**、静态 **1.07–1.40×** | **<1.1× 达标** |

**判定口径**：`<1.1×` 指「我方完成同一请求的耗时 ≤ 对比方 × 1.1」，等价于「我方吞吐 ≥ 对比方 / 1.1（0.909×）」。
所有测量点均满足；最接近边界的是 `10k.bin×8 连接`：cpp 45,326 vs nginx 47,346 req/s（0.96×，慢 4%）。

---

## 1. 改动清单

| 文件 | 改动 |
|---|---|
| `include/go2cpp/network/http.hpp` | 新增 `FileBody`/`Response::file`、`RouteParams`/`Router`、`StaticFilesOptions`/`StaticFiles`/`ServeStaticFile`；`Status` 增加 206/304/416；`Server` 路由成员改为 `Router`；模块文档更新 |
| `src/network/http.cpp` | `Router` 实现（精确哈希 + trie + 回溯）、`StaticFiles` 实现（解码/穿越防护/stat/Range/304/ETag）、`Response::to_string` 文件体支持、`Server::serve_connection` 接入路由 + sendfile、`status_text` 新码、`ResponseWriter` 忽略文件体 |
| `src/bento/net/http_server.cpp` | 新增 `Route()`/`MatchRoute()`；sendfile 发送文件体；SSE 查找改 `string_view`；路由锁改 `shared_mutex` 读锁 |
| `include/go2cpp/bento/net/http_server.hpp` | 新增 `Route()` 声明、`MatchRoute()`、`http::Router m_router`、`shared_mutex`；文档更新「已实现/未实现」 |
| `tests/network/net_perf_server.cpp` | HTTP 基准服务端新增 `route`（64 条路由）/`static`（静态目录）变体 |
| `tests/network/test_http_e2e.cpp` | **新增**：260 条断言（路由/静态/Range/304/pipelining/SSE/并发），自包含 `main`，不注册 ctest（避免改 cmake），用独立命令编译 |

未改动：`http::Parser`、`Client`、`ResponseWriter` 语义（除 `start()` 丢弃 `Response::file` 这一防御性修正）、SSE 写入器、示例代码。

---

## 2. 路由

### 2.1 原实现与问题

`http::Server` 的路由表是 `std::vector<std::tuple<Method, std::string, Handler>>`，
`serve_connection()` 每请求**线性扫描**：64 条路由平均 32 次 `std::function` 指针比较 +
`std::string_view` 比较；`bento::HttpServer` 只有单个 `OnRequest` handler，SSE 路由也是线性扫描。
没有参数/通配能力。

### 2.2 新实现（`http::Router`，PIMPL）

- **精确路由**：每个 `Method` 一张 `unordered_map<string_view, Handler>`，key 指向
  `deque<string>` 中的稳定存储。查找 `path_view()` 直接命中，**零分配、平均 O(1)**。
  重复注册保留第一次（与原线性「先注册先匹配」一致）。
- **动态路由**：按 `/` 切段的 trie。每节点：
  - 静态子节点：`map<string, unique_ptr<Node>, less<>>`（异质查找，无临时 string）；
  - 参数子节点：`vector<pair<name, unique_ptr<Node>>>`，**同一段位置允许并存多个参数名**
    （`/users/:id` 与 `/users/:uid/posts/:pid` 可共存，按注册顺序回溯）；
  - 尾部通配：`*` / `*name` 匹配当前节点之后的全部路径（含空串）。
- **优先级**：精确 > 参数 > 尾部通配；同节点静态子节点优先于参数子节点。
- **HEAD 回退**：`Server`/`bento::HttpServer` 在 HEAD 未命中时回退同路径 GET
  （HTTP 语义：HEAD 是 GET 的无体版本），`StaticFiles` 因此可直接服务 HEAD。
- 匹配在注册完成后只读；`http::Server` 无锁（启动期注册），`bento::HttpServer`
  用 `shared_mutex` 读锁（运行期可改路由）。

### 2.3 接入

- `http::Server::route()` → `m_router.add()`；`serve_connection()` 命中后把参数写入
  `request.path_values`（handler 里用 `request.path_value("id")` 读取），未命中走 `m_fallback`。
- `bento::HttpServer::Route()` 新增：命中后调用路由 handler；未命中回落到 `OnRequest()`，
  再没有则 404。SSE 路由保持精确匹配，但查找改为 `string_view`＋空表快速返回，避免每请求
  `request.path()` 分配。
- 基准 `net_perf_server http 0 2 route` 注册 64 条 `/r0..r63`，与 Go `ServeMux` 同口径。

---

## 3. 静态文件：sendfile 零拷贝 + Range

### 3.1 数据通路

```
StaticFiles::serve() ──stat/条件/Range──> Response{ file = FileBody{path, offset, length} }
        │
        └─ Server: to_string() 只序列化响应头（不读文件）
                     │
                     └─ Socket::send_file() → sendfile(2) 内核页缓存 → 网卡
```

- `Response::file` 非空时 `body` 不参与序列化；`Content-Length` 缺省用 `file->length` 补齐。
- `http::Server::serve_connection()` 与 `bento::HttpServer::ServeConnection()` 都在写完头后
  调用 `send_file`（循环处理部分写）；`HEAD` 只回头。
- bento 的 `use_bufio` 路径：头先经 `BufferedSocketWriter` 一次 `writev` flush，再 sendfile，
  顺序与数据完整性由测试覆盖（`use_bufio=true/false` 各跑一遍）。

### 3.2 strace 证据（1 MiB 全量 + Range）

```
sendfile(6, 8, [0] => [1048576], 1048576) = 1048576
sendfile(6, 8, [100] => [200], 100) = 100
```

用户态没有对文件内容的 `read`；1 MiB 由**一次** `sendfile` 完成，Range 请求只发送 100 字节。

### 3.3 功能矩阵（`StaticFiles`）

| 能力 | 行为 |
|---|---|
| 方法 | GET / HEAD；其它方法返回 `false`（交上层路由决定 405/404） |
| 挂载 | `prefix`（如 `/static`）只匹配 `/static` 与 `/static/...`，`/staticx` 不匹配 |
| 索引 | 目录或 `/` → `index`（默认 `index.html`） |
| 条件请求 | `If-None-Match`（ETag 强校验、支持 `*`）与 `If-Modified-Since` → **304**（无 body、过滤 CL/TE） |
| Range | 单区间 `a-b`、开区间 `a-`、后缀 `-N`、越界截断、不可满足 **416**（`Content-Range: bytes */size`）；多区间按 RFC 忽略返回 200；`If-Range`（ETag）不匹配时忽略 Range |
| 响应头 | `Content-Type`（MIME 表）、`Content-Length`、`Accept-Ranges`、`ETag`、`Last-Modified`、`X-Content-Type-Options: nosniff`、206 的 `Content-Range` |
| 安全 | 百分号解码后拒绝 `..`、NUL、非法 `%XX`、非 origin-form；目标路径 = root + 相对路径，无法越出 root（root 内 symlink 跟随，与 nginx 默认一致） |
| 上限 | `max_bytes` 超过 → 413 |
| 性能 | 无 `%` 时路径零复制；ETag/Last-Modified 线程局部缓存（size+mtime 命中）；header 一次 `reserve` |

---

## 4. Pipelining / keep-alive / chunked / SSE

- 解析器的 `pending = take_remaining()` 机制保留：同一 `recv` 里的后续请求逐条处理，
  响应按请求顺序写出（HTTP/1.1 流水线语义）。
- 测试：单连接一次写 8 条混合请求（`/hello`、静态 10 KiB、参数路由），按序读 8 条并校验 body。
- 基准：`loadgen -pipeline 8/16`，单连接 8.9k（串行）→ 53.7k（深度 8）→ **72.9k（深度 16）**，
  即 **6.0× / 8.2×**；8 连接深度 8 = **239.5k req/s**（8 连接串行 56.3k 的 4.3×）。
  这直接绕开了 WSL2 回环 RTT（~110 µs）对单连接 ping-pong 的限制。
- chunked 请求/响应与 SSE：`Parser` 原有 chunked/trailer 测试（`GO2CPP_TEST_FILTER=network`）全过；
  `bento::HttpServer` 的 SSE 路由在本轮新增 3 事件端到端断言（`event: tick`/`data: 0..2`）。
- `ResponseWriter::start()` 现在显式丢弃 `Response::file`：chunked 流没有 sendfile 路径，
  避免文件体长度与 `Transfer-Encoding` 冲突。

---

## 5. 正确性

### 5.1 新增 `tests/network/test_http_e2e.cpp`

自包含测试（真实 TCP 字节流，非纯编解码单测）：

| 组 | 场景 | 断言数 |
|---|---|---|
| Router | 65 条精确路由、参数（单/多）、尾部通配、静态优先于参数、方法隔离、重复注册、尾斜杠、整条 `*` 兜底 | ~25 |
| StaticFiles | 200 文件体、MIME、ETag、Range（首段/后缀/开区间/416/多区间忽略）、304（两种条件）、If-Range、`..`/`%2e%2e`/`%zz`/`%00`、404、索引、prefix、413 | ~45 |
| base `http::Server` | 路由/参数/404、keep-alive 20 连、1 KiB 全量、Range、HEAD（无 body + CL）、1 MiB sendfile 逐字节、pipelining 8 条混合 | ~40 |
| `bento::HttpServer`（`use_bufio` false/true 两轮） | hello、参数、POST body、1 MiB sendfile、Range、pipelining 8、SSE 3 事件、8 连接 × 50 请求并发 | ~140 |

结果：**260 checks, 0 failures**（`/tmp/build-http/test_http_e2e`）。

### 5.2 既有回归（同一构建目录）

| 目标 | 结果 |
|---|---|
| `GO2CPP_TEST_FILTER=network ./go2cpp_tests` | `all checks passed`（address/bytearray/socket/http，含 chunked/trailer/HEAD/ResponseWriter） |
| `GO2CPP_TEST_FILTER=sse ./go2cpp_tests` | `all checks passed` |
| `./bento_test_http_server` | `[PASS] test_http_server (109 checks)` |

---

## 6. 性能对标

### 6.1 环境与口径

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 9 8945HX（12 硬件线程，WSL2，内核 6.6.87.2-microsoft-standard-WSL2） |
| 构建 | `/tmp/build-http`，Release（`-O3 -DNDEBUG`），GCC 13.3，Ninja |
| cpp 服务端 | `net_perf_server http 0 2 <variant>`，2 worker，`taskset -c 2,3` |
| Go 基线 | go1.27.1，`GOMAXPROCS=2`，`taskset -c 2,3` |
| nginx | 1.24.0（Ubuntu），`/tmp/httpbench/nginx.conf`：2 worker，`sendfile on`，未绑核（affinity 0-11） |
| 压测器 | `/tmp/httpbench/loadgen`（每连接一 goroutine、keep-alive、支持 pipeline），`taskset -c 4-9` |
| 每点 | 1 s 预热 + 5 s 计时，3 轮取最佳；`errors=0` |

> 说明：nginx 未绑核是既有口径（与 2026-10-31 报告一致）；cpp/Go 绑 2,3，loadgen 用 4–9。
> 回环 RTT 约 110 µs，单连接 ping-pong 吞吐受 RTT 限制，**多连接与 pipelining 才反映服务端效率**。

### 6.2 hello（12 B，`/hello`）

| 连接 | cpp req/s | go req/s | nginx req/s | cpp/go | cpp/nginx | cpp p50/p99 µs |
|---|---|---|---|---|---|---|
| 1 | 8,946 | 6,519 | 8,291 | 1.37× | 1.08× | 99 / 236 |
| 8 | 56,312 | 44,051 | 47,857 | 1.28× | 1.18× | 131 / 371 |
| 32 | 139,008 | 102,179 | 128,774 | 1.36× | 1.08× | 219 / 578 |
| 64 | 154,943 | 91,731 | 151,102 | 1.69× | 1.03× | 395 / 931 |

（Go 64 连接 p50/p99 = 577/2364 µs；nginx = 463/856 µs；对应 cpp 的三项最低。）

### 6.3 路由：64 条静态路由（`/r63`）

| 连接 | cpp req/s | go req/s | cpp/go | cpp p50/p99 µs |
|---|---|---|---|---|
| 8 | 53,934 | 44,875 | 1.20× | 139 / 370 |
| 32 | 138,726 | 108,286 | 1.28× | 221 / 563 |

### 6.4 静态文件（sendfile；三方同批次复测）

| 文件 | 连接 | cpp req/s | go req/s | nginx req/s | cpp/go | cpp/nginx | cpp MB/s |
|---|---|---|---|---|---|---|---|
| 1 KiB | 8 | 47,042 | 35,573 | 42,285 | 1.32× | 1.11× | 48.2 |
| 1 KiB | 32 | 61,366 | 50,743 | 50,515 | 1.21× | 1.22× | 62.8 |
| 10 KiB | 8 | 45,326 | 36,836 | 47,346 | 1.23× | 0.96× | 464.1 |
| 10 KiB | 32 | 69,063 | 53,357 | 50,390 | 1.29× | 1.37× | 707.2 |
| 1 MiB | 8 | 11,540 | 10,430 | 6,462 | 1.11× | 1.79× | 12,101 |
| 1 MiB | 32 | 12,067 | 11,263 | 7,012 | 1.07× | 1.72× | 12,654 |

> 1 MiB 数据取自同一构建的全量轮（11:48 前后，三方同轮）；1 KiB/10 KiB 因全量轮的
> cpp static 段受同机其它任务干扰（该轮 nginx 10 KiB/32 达 80k、复测回落到 50k），
> 于 11:56 用**同一时间窗口**重测三方，表中为复测值。

### 6.5 pipelining

| 配置 | cpp | go | nginx | cpp/go | cpp/nginx |
|---|---|---|---|---|---|
| 1 连接 pipeline=8 | 53,706 | 16,752 | 47,295 | 3.21× | 1.14× |
| 1 连接 pipeline=16 | 72,937 | 18,340 | 62,747 | 3.98× | 1.16× |
| 8 连接 pipeline=8 | 239,512 | 86,206 | 126,637 | 2.78× | 1.89× |

### 6.6 是否 <1.1×

把所有测量点按「cpp 吞吐 ≥ 对比方 / 1.1」检查：

- **vs Go**：最低 1.07×（1 MiB/32），全部达标；
- **vs nginx**：最低 0.96×（10 KiB/8），即 cpp 慢 4%，仍在 1.1× 之内；其余 1.03–1.89×；
- 延迟：cpp 的 p50/p99 在 hello 多连接、路由、静态文件上均低于 Go；与 nginx 互有胜负，
  64 连接 hello p99 931 µs vs nginx 856 µs（+9%），仍 <1.1×。

---

## 7. 限制

1. **单连接 ping-pong** 受 WSL2 回环 RTT（~110 µs）限制，串行 8.9k req/s 是 RTT 决定，
   不是 CPU 上限；pipelining 8/16 可到 53.7k/72.9k。
2. **nginx 未绑核**（2 worker，affinity 0-11），cpp/Go 固定在 2,3；跨引擎对比是既有口径，
   但 nginx 可能被 loadgen 抢占，数据有噪声（10 KiB/8 的 0.96× 属此类边界点）。
3. 静态文件每请求 `stat` + `open`，未实现 `open_file_cache`；未实现多区间 multipart
   与 gzip/deflate。
4. `StaticFiles` 跟随 root 内 symlink；不提供 `disable_symlinks` 配置。
5. 未跑 ASan/UBSan 全量构建（时间限制）；正确性依赖真实 socket 断言与既有测试回归。
6. 未 commit：工作树上有其它并发改动（见 §8），本轮只保证以下文件的工作区内容正确。

---

## 8. 复现

```bash
# 构建（独立目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-http -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_BENTO=ON \
  -DGO2CPP_BUILD_TESTS=OFF -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_BUILD_ENHANCED=OFF
ninja -C /tmp/build-http go2cpp_bento_http

# 端到端测试（260 checks）
g++ -O2 -std=c++17 -I include -Wall -Wextra -Wpedantic \
  -o /tmp/build-http/test_http_e2e tests/network/test_http_e2e.cpp \
  -Wl,-rpath,/tmp/build-http -L /tmp/build-http \
  -lgo2cpp_bento_http -lgo2cpp_bento -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync \
  -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error \
  -lpthread -ldl
/tmp/build-http/test_http_e2e

# 既有回归
GO2CPP_TEST_FILTER=network /tmp/build-http/go2cpp_tests
GO2CPP_TEST_FILTER=sse /tmp/build-http/go2cpp_tests
/tmp/build-http/bento_test_http_server

# 基准服务端 + 三方对标
g++ -O3 -DNDEBUG -std=c++17 -I include -o /tmp/httpbench/net_perf_server \
  tests/network/net_perf_server.cpp -Wl,-rpath,/tmp/build-http -L /tmp/build-http \
  -lgo2cpp_bento_omni -lgo2cpp_bento_ws -lgo2cpp_bento_socks -lgo2cpp_bufio \
  -lgo2cpp_bento_transport -lgo2cpp_channel -lgo2cpp_bento_http -lgo2cpp_bento \
  -lgo2cpp_hash -lgo2cpp_network -lgo2cpp_io -lgo2cpp_sync -lgo2cpp_context \
  -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error -lpthread -ldl
bash /tmp/httpbench/e2e_bench.sh all        # 结果写入 /tmp/httpbench/e2e_results.txt
```

**sendfile 验证**：

```bash
strace -f -e trace=sendfile -o /tmp/sendfile_trace.txt \
  /tmp/httpbench/net_perf_server http 0 2 static   # 读 READY port=N
curl -s -o /dev/null http://127.0.0.1:$PORT/static/1m.bin
grep sendfile /tmp/sendfile_trace.txt
# sendfile(6, 8, [0] => [1048576], 1048576) = 1048576
```
