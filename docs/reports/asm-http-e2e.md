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

---

## 汇编级取证（2026-11-04）

> 取证对象：`/tmp/build-asm`（CMake Release，`-O3 -DNDEBUG`，GCC 13.3，Ninja）构建的
> `libgo2cpp_network.so.0.1.0` 与 `libgo2cpp_bento_http.a`；`src/app/gateway.cpp` 另以
> `g++ -O2 -std=c++17 -fno-inline-functions -fno-inline-small-functions -fno-inline-functions-called-once`
> 单独编译为 `/tmp/asmobj/gateway_noinline.o`（gateway 的匹配/分派函数是头内 inline，生产 `-O2`
> 下全部被内联，无法单独 objdump；该文件仅为**分离取证**，生产仍为 `-O2` 内联口径）。
> 测量时间：2026-11-04 12:42–13:00（Asia/Shanghai）。命令统一为
> `objdump -d --no-show-raw-insn -C <binary> | sed -n '/<symbol>:/,/^$/p'`。
> 本轮**未改代码、未 commit**。

### A. 取证口径

| 列 | 定义 |
|---|---|
| 指令 | 函数体内 `地址:` 开头的指令行数（不含 `.cold` 冷段；不展开被调函数） |
| call | `call` 指令数（含 `call *%rip` 间接调用与 PLT）；**不递归**计入被调函数 |
| new | `call` 目标为 `operator new` / `_Znwm` 的条数（**只统计直呼**） |
| lock | 带 `lock` 前缀（原子 RMW）或 `lock` 类指令的条数 |
| 栈帧 | 函数序言 `sub $0x..,%rsp` 的字节数；多条 `sub` 累加，记为上限 |

### B. 热函数指令表（22 个）

**HTTP 服务端主路径（`libgo2cpp_network.so`，`-O3`）**

| # | 符号 | 指令 | call | new | delete | lock | 栈帧 |
|---|---|---|---|---|---|---|---|
| 1 | `http::Parser::feed_impl`（请求解析） | **1714** | 123 | 0 | 0 | 0 | 440 B |
| 2 | `http::Parser::feed(Request&)`（入口） | 23 | 2 | 0 | 0 | 0 | 24 B |
| 3 | `http::parse_method` | 149 | 10 | 0 | 0 | 0 | 40 B |
| 4 | `http::Router::match`（路由匹配） | 118 | 5 | 0 | 0 | 0 | 88 B |
| 5 | `http::Response::to_string`（响应序列化） | **633** | 52 | 0 | 0 | 0 | 360 B |
| 6 | `http::status_text` | 64 | 5 | 0 | 0 | 0 | 16 B |
| 7 | `http::Server::serve_connection`（主循环/写回） | **452** | 44 | 0 | 2 | 0 | **≈8.4 KiB**（2×4096+0x368+0x20） |
| 8 | `http::ResponseWriter::start` | 137 | 15 | 0 | 1 | **1** | 0（红区，`add $0x80`） |
| 9 | `http::ResponseWriter::write` | 154 | 6 | 0 | 0 | 0 | 168 B |
| 10 | `http::ResponseWriter::flush` | 9 | 0 | 0 | 0 | 0 | 0 |
| 11 | `http::ResponseWriter::close` | 70 | 5 | 0 | 0 | 0 | 72 B |
| 12 | `net::Socket::send` | 17 | 2 | 0 | 0 | 0 | 40 B |
| 13 | `net::Socket::send_impl`（sendmsg 路径） | 111 | 6 | 0 | 0 | 0 | 24 B |
| 14 | `net::Socket::sendv`（writev 路径） | 77 | 7 | 0 | 0 | 0 | 88 B |
| 15 | `net::Socket::send_file`（sendfile 路径） | 151 | 14 | 0 | 0 | 0 | 600 B |
| 16 | `http::Client::do_request`（客户端连接/编码） | 844 | 88 | **1** | 0 | 0 | 4096 B |

**`app::gateway`（`-O2` 分离口径，`/tmp/asmobj/gateway_noinline.o`）**

| # | 符号 | 指令 | call | new | delete | lock | 栈帧 |
|---|---|---|---|---|---|---|---|
| 17 | `Gateway::Impl::Dispatch`（分派主干） | **510** | 71 | 0 | 0 | **4** | 664 B |
| 18 | `Gateway::Impl::Match`（路由匹配） | 289 | 27 | 0 | 0 | 1 | 136 B |
| 19 | `Gateway::Impl::MatchPath`（逐段匹配） | **316** | 38 | 0 | 0 | 0 | 168 B |
| 20 | `(anon)::SplitPath` | 81 | 4 | 0 | 0 | 0 | 56 B |
| 21 | `(anon)::SerializeResponse` | 345 | 49 | 0 | 0 | 0 | 232 B |
| 22 | `(anon)::WriteAll` | 36 | 1 | 0 | 0 | 0 | 0 |

### C. call 归因（汇编级发现）

| 函数 | call 总数 | 其中 |
|---|---|---|
| `Parser::feed_impl` | 123 | 其他 79、**`isalnum/isspace/isdigit` 13**、string 构造/扩容 10、**`memchr` 7**、**`tolower/toupper` 6**、异常 3、`append` 2、`reserve` 2、`memcpy` 1 |
| `Response::to_string` | 52 | **`tolower/toupper` 25**、`append` 12、其他 9、异常 4、`reserve` 1、string 构造 1 |
| `Router::match` | 5 | 其他 4、哈希/查找 1 |
| `Server::serve_connection` | 44 | 其他 40、`operator delete` 2、`memcpy` 2 |
| `Socket::send_impl` | 6 | 其他 4、锁相关 2 |
| `gateway::Impl::Dispatch` | 71 | 4 条为 `lock addq $0x1,0x2xx(%rbp)`（**Stats 计数器原子自增**，不是互斥锁） |
| `gateway::Impl::MatchPath` | 38 | 逐段比较 + `std::map` 参数插入（`map<string,string>` 临时对象） |

三条硬事实：

1. **`to_string` 每响应做 25 次 `tolower` 调用**：`equal_ci` 对 header 名逐字符走 libc locale 表，
   且为「Connection / Content-Length / Transfer-Encoding」三次扫描 + 逐 header 再扫一遍。
   hello 响应只有 2 个 header，仍付 25 次调用。
2. **`feed_impl` 每请求 19 次 ctype 调用**（13 次 `isalnum/isspace/isdigit` + 6 次 `tolower`）+ 7 次 `memchr`，
   全部是 PLT 直呼、无法内联。另有 10 次 `std::string` 构造/扩容调用（header 名/值的拷贝链）。
3. **`feed_impl` 直呼 `operator new` = 0 次**：分配全部藏在 `string 构造/扩容` 与被调的 map 节点构造里
   （间接分配）；因此「每请求堆分配次数」本轮**未测**（需 ltrace/堆采样，不属 objdump 口径）。
   与 `2026-11-01-asm-network-review.md` 的「每请求 14 次堆分配」不能直接相减，只能定性说明：
   `ostringstream` 已消失（见 D），但 header 拷贝链仍在。

### D. 与前值对比（`2026-11-01-asm-network-review.md:16-17`）

| 项 | 前值（2026-11-01） | 本轮 objdump | 判定 |
|---|---|---|---|
| `ResponseWriter::write` | 每 chunk 构造 `ostringstream` + 3 次 `send`，1.4–2 µs 固定开销 | 154 条指令 / 6 call / **0 个 ostringstream 相关 call** / 0 个 `Socket::send` 直呼 / 0 new | **已修复**（不再有流对象） |
| `serve_connection` 栈 | 每请求 8 KB 栈零初始化 | 2×4096 + 0x368 + 0x20 ≈ 8.4 KiB | **未变**（仍是 8 KB 级） |
| 解析分配 | 每请求 14 次堆分配 | 直呼 new = 0；间接分配链 10 处 | **部分**（无法从 objdump 定量） |

### E. 物理下限与差距

下限口径（写清假设，便于复核）：

- **解析**：每字节 1–2 条指令（状态机 + 分隔符判断，无分配）。hello 请求实测 **87 B**
  （`GET /hello HTTP/1.1` + Host + User-Agent + Accept，端口 5 位）→ 下限 **87–174** 条。
- **序列化**：每字节 1 条指令（memcpy 级），hello 响应实测 **77 B** → 下限 **77** 条；
  另一口径「每 header 6–8 条 store + 状态行 15 条」→ 下限 **27–31** 条。
- **路由**：O(1) 哈希 = 一次 hash + 一次桶探测 + 一次 string_view 比较 ≈ **20** 条。
- **写回**：一次 `sendmsg`/`writev` 的 iovec 构造 + 错误处理 ≈ **15–20** 条。
- **每请求固定开销**（`serve_connection` 循环 + recv + 2 次调用 + 写）≈ **80** 条。

| 函数 | 实测 | 下限 | 倍数 |
|---|---|---|---|
| `Parser::feed_impl` | 1714 | 87–174 | **9.9×–19.7×** |
| `Response::to_string` | 633 | 77（每字节 1 条）/ 27–31（header 口径） | **8.2×** / 20.4–23.4× |
| `Gateway::MatchPath` | 316 | 25 | **12.6×** |
| `Gateway::Impl::Match` | 289 | 30 | 9.6× |
| `Gateway::Impl::Dispatch` | 510 | 50 | 10.2× |
| `http::status_text` | 64 | 5 | 12.8×（绝对量小） |
| `http::parse_method` | 149 | 15 | 9.9× |
| `Router::match` | 118 | 20 | 5.9× |
| `Server::serve_connection` | 452 | 80 | 5.7× |
| `Socket::send_impl` | 111 | 20 | 5.6× |
| `Socket::sendv` | 77 | 15 | 5.1× |
| `Socket::send_file` | 151 | 20 | 7.6× |
| `ResponseWriter::write` | 154 | 30 | 5.1× |
| `Client::do_request` | 844 | 100 | 8.4× |

**最差 3 个（按绝对指令数 × 倍数，工程意义优先）**

1. `Parser::feed_impl`：**1714 条 / 9.9–19.7× 下限**，每请求 19 次 locale 调用 + 10 处 string 拷贝链；
2. `Response::to_string`：**633 条 / 8.2×**（header 口径 20×+），25 次 `tolower` 是纯浪费；
3. `gateway` 匹配链 `Dispatch+Match+MatchPath`：**1115 条 / 9.6–12.6×**，线性逐段匹配 + `map` 参数。

（若只看倍数：`feed_impl` 19.7× > `status_text` 12.8× > `MatchPath` 12.6×；但 `status_text` 仅 64 条，
修复优先级低于前三个。）

### F. 与工业参考对比（1.1× 目标）

**吞吐口径（已有数据，非本轮新测）**

| 对比 | 数据 | 倍数 | 判定 |
|---|---|---|---|
| cpp vs Go net/http | `asm-http-e2e.md` §6.2–6.5：1.07–3.98× | cpp 快 1.07–3.98× | ≤1.1× 达标 |
| cpp vs nginx | 同上：0.96–1.89× | 最差 0.96×（慢 4%） | ≤1.1× 达标 |
| M05 pipeline=16 单连接 | **120,755 rps**（6 轮中位，p50 7–8 µs、p99 15–16 µs） | 相对 §6.5 旧值 72,937 rps = **+65.6%** | 达标 |

**汇编口径**

- 参考方（Go net/http、nginx）的**每请求指令数**：本轮**未测**——无同口径 `objdump` 数据，
  不以文献数字冒充。故「指令级是参考的几倍」= **未测**。
- 可给出的替代证据是**指令预算占比**：按 M05 120,755 rps、2 worker、Ryzen 9 8945HX 取 3.7 GHz，
  每请求可用 **61,281 cycles**；热路径每条请求指令合计
  （`serve_connection` 452 + `feed_impl` 1714 + `Router::match` 118 + `to_string` 633 + `send` 17 + `send_impl` 111）
  = **3,045 条**，占周期 **4.97%**（即使 IPC=1）。hello 64 连接 154,943 rps 时预算 47,760 cycles，
  占比 **6.4%**。
- 结论：**用户态指令数不是当前瓶颈**（IPC 估算 0.05–0.06，说明 CPU 大量时间在等 syscall/网络栈）。
  即使把 3,045 条压到下限 ~400 条，理论收益也只有个位数百分比——**不能仅靠汇编优化达成 1.1×**，
  需与架构/系统调用优化一起做。这与「优化顺序：架构 > 汇编」一致。

### G. 可能的优化点（只列，本轮不改代码）

按「预计省下的指令数 × 触发频率」排序：

1. **消灭 locale 调用**：`to_string` 的 25 次 `tolower` + `feed_impl` 的 19 次 `isalnum/isspace/tolower`
   → 改成 256 字节 `constexpr` 查表 + 内联比较；header 名比较改「长度 + 首字符 + `memcmp`」。
   预计 `to_string` −15–20%、`feed_impl` −10–15% 指令。
2. **解析零拷贝**：`feed_impl` 的 10 处 string 构造/扩容 + `substr` 切片 → header 名/值直接
   `string_view` 指向接收缓冲（生命周期与请求绑定），header 容器一次性 `reserve`；可去掉每请求
   10+ 次构造。需要 `Request` 增加「缓冲所有权」或 arena。
3. **gateway 路由 O(1)**：`MatchPath` 316 条线性逐段 + `map` 参数插入 → 与 `http::Router` 同款
   「前缀哈希 + 段 trie」；`Dispatch` 的 4 次 `lock add` 统计计数器 → per-worker 计数、结束时汇总。
4. **写回合并**：`send_impl` 111 条 / `sendv` 77 条；`to_string` 已把头+体合成单 buffer，
   确认 `serve_connection` 走 `sendv` 一次写出（而不是 2 次 `send`），可省一次 syscall。
5. **栈帧**：`serve_connection` ≈8.4 KiB（2×4 KiB 缓冲）、`Client::do_request` 4096 B；
   小请求可降到 4 KiB 或复用每连接堆缓冲，减少栈零初始化与 L1 压力。
6. **`send_file` 151 条/14 call**：sendfile 前后仍有一次 `stat`+`open`（未实现 `open_file_cache`），
   属 §7 已记录项。

### H. 限制

1. gateway 为 `-O2 -fno-inline-*` 分离口径，与生产 `-O2` 内联后的真实指令数**不可直接比较**；
   生产口径下这些函数会被摊进 `Dispatch`/`Run`，总数只会更小、分布不可分。
2. `new` 只统计**直呼**；`std::string` 内部 `operator new`、map 节点分配不计入 → 「每请求分配数」未测。
3. 未做前后 A/B：本轮只有「当前值」，没有「优化后」数据；§D 的前值对比仅限 `ResponseWriter::write`
   与 `serve_connection` 两项可对照项。
4. 指令数为静态计数，不含分支预测失败/缓存缺失；未跑 `perf stat`。
5. 本轮未改代码、未 commit；`/tmp/build-asm` 与 `/tmp/asmobj` 为临时产物。
