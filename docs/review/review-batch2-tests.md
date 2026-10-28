# 批2 测试审查：bento fetch / file_browser / zip / backup / upload / capability / session / udp / x25519

> 基线：`e3e5d4b`（2026-10-28），9 个测试文件与 `test_backup_support.hpp` 均为**未跟踪的新文件**（`git status` 显示 `??`），属工作树中间态，行号以当前工作树为准。
> 审查对象：`tests/bento/{test_fetch,test_file_browser,test_backup,test_zip,test_upload,test_capability,test_session,test_udp_transport,test_x25519}.cpp`（4783 行）+ `test_backup_support.hpp`（238 行）。
> 审查方式：只读代码 + 重新 configure/build 后运行 ctest，未改动任何源码与测试。

## 0. 结论摘要

- **运行结果**：`cmake --build build-review` 9 个目标构建成功；`ctest -R` 9/9 通过，总耗时 1.50s；运行时断言 4068 次，覆盖 117 个测试场景。当前环境（uid=1000、有 curl/unzip/IPv6）下**没有实际触发 SKIP**。
- **总体水平**：正常路径、文件沙箱、损坏归档、RFC 向量、逻辑时钟状态机等做得扎实，`fetch` 的 4 线程并发、`file_browser` 的 500+200 路径 fuzz、`zip` 的 10 轮随机树是亮点。
- **最严重问题**：`zip` 只测了**写入端**拒绝非法条目名，**读取端 Zip Slip 防御与解压炸弹/总量上限完全没有恶意归档用例**；一旦读取端校验回归，现有测试不会失败（P0）。
- **次严重问题**：`fetch` 的大小上限只测 `Content-Length` 预检，流式累计超限、`allow_redirects=false`、3xx 无 Location、连接拒绝均未测（P1）；`session` 的 `WaitFor`、`AcceptPairing` 正常路径、`auto_accept_pairing=false`、`max_outbox_frames` 为公开 API 零覆盖（P1）。
- **断言质量**：整体不是"只断言 OK"，但有 1 条恒真断言、2 条阈值过宽的断言、多处只校验"非空/子串"；`BackupResult::archive_path`、各模块 `message/error` 的语义几乎没有断言。
- **SKIP 逃逸**：7 处环境不满足时 `printf` 后 `return`，ctest 仍记 PASS，无法区分"全跑"与"大面积跳过"；`upload` 缺 curl 时反而是硬失败，策略不一致。

## 1. 运行与基线

| 项 | 结果 |
|---|---|
| 配置 | `cmake -S . -B build-review -DGO2CPP_WITH_BENTO=ON`，`Bento transport: ON (tests=ON)` |
| 构建 | 9 个目标 + 依赖库全部成功，`-j 8`，无警告失败（`-Werror`） |
| ctest | `bento_{fetch,file_browser,zip,backup,upload,capability,session,udp_transport,x25519}` 9/9 Passed，1.50s |
| 断言 | fetch 138、file_browser 1881、zip 245、backup 85、upload 97、capability 124、session 180、udp 158、x25519 1160，合计 4068 |
| SKIP | 直接运行二进制并 grep `SKIP/skip`：无输出（当前环境全部可执行） |
| 外部依赖 | 系统 `curl`（upload 必需、fetch https 分支）、`unzip`（zip/backup 交叉验证）、IPv6 `::1`、非 root（权限用例） |

## 2. 功能覆盖矩阵

图例：✅ 覆盖；⚠️ 部分覆盖/断言偏弱；❌ 未覆盖。

### 2.1 fetch（17 场景，138 断言）

| 能力/分支 | 状态 | 证据（test_fetch.cpp） |
|---|---|---|
| `ResolveFetchPath` 合法、`..`、绝对、分隔符、空、超长、0 上限、目录不存在 | ✅ | 400-417 |
| `ParseSha256Hex` 大写归一、非法、长度 | ✅ | 419-423 |
| 参数校验：scheme、空 URL、`http://`、max_bytes=0、read_timeout=0、retries=99、坏摘要 | ✅ | 425-447 |
| 1MB 下载 + SHA-256 + 逐字节 + attempts/http_status + 目录条目数 | ✅ | 453-479 |
| 404 不重试、不落盘 | ✅ | 483-503 |
| 读超时（200ms vs 800ms 慢响应） | ✅ | 506-525 |
| 503 两次后成功（attempts=3） | ✅ | 528-550 |
| 持续 503 用尽重试（attempts=2） | ✅ | 553-572 |
| 文件名逃逸 7 种 + URL `%2e%2e` 解码 | ✅ | 575-603 |
| 大小上限（**仅 Content-Length 预检**） | ⚠️ | 606-625；流式累计未测 |
| chunked 下载 | ✅ | 628-646 |
| Content-Length 大于实际 → kTruncated | ✅ | 649-665 |
| 302 跟随 + 落盘文件名 | ✅ | 668-685 |
| 重定向次数上限 | ⚠️ | 688-705；只断言 kHttpError，未断言跳数/message |
| 摘要不符（attempts=1、bytes 正确） | ✅ | 708-727 |
| 目标已存在拒绝 / overwrite 覆盖且请求数=2 | ✅ | 730-754 |
| https 走 curl：**只测握手失败** | ⚠️ | 757-785；curl 缺失时 printf 跳过 |
| EOF 无 Content-Length、gzip 编码、超大头部 | ✅ | 788-817 |
| 4 线程并发 1MB 下载 | ✅ | 820-858 |
| 0 字节文件（`/empty` 路由已定义） | ❌ | 路由 299 行定义但从未调用 |
| `allow_redirects=false` / 3xx 无 Location / 3xx→https | ❌ | 字段 171 行未使用 |
| 连接拒绝 / DNS 失败 / connect_timeout / total_timeout 触发 | ❌ | 仅参数赋值（518、769-770），未真正触发 |
| 流式（chunked/EOF）累计超 max_bytes | ❌ | BodySink 实现有上限，测试无对应用例 |
| URL userinfo、fragment、IPv6 字面量、超长 URL、query | ❌ | 头文件 227-228 承诺的语法边界未测 |
| 目标为目录/符号链接时 rename 失败（kIoError） | ❌ | — |
| 408/425/429 可重试状态码 | ❌ | 只测 503 |

### 2.2 file_browser（12 场景，1881 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| 构造 valid/root、无效根、根为文件 | ✅ | 201-258 |
| List 排序、类型、隐藏项、空目录、非目录、不存在、nullptr | ✅ | 201-259 |
| Stat 根/文件/符号链接/不存在/null | ✅ | 262-290 |
| Get 内容、覆盖、类型错误、不存在、空参数、父目录缺失 | ✅ | 293-333 |
| Put 自动建父目录、覆盖、源缺失/目录/空路径/`..` | ✅ | 336-377 |
| 路径逃逸 13 种 × List/Get/Put | ✅ | 380-412 |
| 符号链接逃逸默认拒绝、follow=true 根内链接成功 | ✅ | 415-446 |
| 1MB Get/Put 往返 + 无 `.part` 残留 | ✅ | 449-475 |
| 大小上限 Get/Put + 恰好等于上限 | ✅ | 478-503 |
| 权限拒绝（EACCES） | ⚠️ | 506-534；root 下直接 return |
| 目录条目上限 + 输出条目数 | ✅ | 537-552 |
| 非法选项（4 个上限为 0/过大）+ 空参数 | ✅ | 555-588 |
| 路径 fuzz 500 非法 + 200 合法 | ⚠️ | 591-652；合法只断言 `kOk || kNotFound` |
| 自由函数 `List/Get/Put(root, ...)` | ❌ | 头文件 221-239，测试全部走类方法 |
| `options()` getter | ❌ | 169 行 |
| `kOther`（FIFO/socket/设备） | ❌ | FileEntryType 第 4 类从未构造 |
| Get 目标已存在且为目录；覆盖预置符号链接 | ❌ | — |
| Put 覆盖已存在符号链接（O_NOFOLLOW 语义） | ❌ | — |
| 相对路径根、根为符号链接、`max_path_bytes` 恰好边界 | ❌ | — |
| 并发 List/Get | ❌ | — |

### 2.3 zip（18 场景，245 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| CRC-32/IEEE 标准向量 `123456789→0xCBF43926` | ✅ | 31-51 |
| 单文件 0..255 全字节 + stats | ✅ | 54-84 |
| 0 字节文件（CRC=0、仍生成条目） | ✅ | 87-106 |
| 多层目录/空目录/中文空格名 + 树比对 | ✅ | 109-149 |
| 1MB 往返 + 系统 `unzip -t`/`unzip -p` 交叉 | ✅ | 152-185 |
| 条目数上限 + Abort 删除半成品 | ✅ | 188-202 |
| 单条大小上限 / 归档总量上限（写入端） | ✅ | 205-234 |
| 非法条目名（**写入端**） | ✅ | 237-258 |
| 输入缺失 / writer 状态机 / Abort | ✅ | 261-282、368-378 |
| CRC 篡改、EOCD 破坏、截断 | ✅ | 285-322 |
| 符号链接与 `loop` 不跟随、计数 2 | ✅ | 325-347 |
| 中央目录顺序、覆盖同名归档 | ✅ | 350-365、381-403 |
| 随机树 10 轮往返 | ✅ | 406-443 |
| 读取端上限 | ⚠️ | 446-466；用 0 字节条目 + `max_total_bytes=1` 仍成功，未真正触发 |
| **读取端 Zip Slip**（恶意 `../`/绝对名归档） | ❌ | 校验实现在 `src/bento/app/zip.cpp:955`，无测试 |
| **解压炸弹/声明超大 size**（数据不足、总量超限） | ❌ | — |
| 非 store/加密/数据描述符（kUnsupportedCompression） | ❌ | 实现 941-947 有分支 |
| `AddExclusion` 直接 API / `AddDirectory` 空 prefix / `limits()` | ❌ | 头文件 209-232 |
| 空归档（仅 EOCD）的 List/Extract | ❌ | — |
| Extract 覆盖已存在文件、目录 | ❌ | 文档 255 行承诺"覆盖同名文件" |
| `AddBytes(nullptr, size>0)`、Close 后 Add、析构自动清理 | ❌ | — |
| 输出不可写 → kIoError；FIFO → special_skipped | ❌ | — |
| `ZipStatusToString` | ❌ | — |

### 2.4 backup（10 场景，85 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| 1MiB 目录打包/解包 + 系统 unzip | ✅ | 46-76 |
| 多输入（目录 + 独立文件）+ basename | ✅ | 79-108 |
| 输出在输入目录内自排除 | ⚠️ | 111-134；`second.stats.archive_bytes < first_size + 4096` 过宽 |
| 空目录归档与解包 | ✅ | 137-154 |
| 失败保留旧包 + 无 `.part` | ✅ | 157-171 |
| 顶层重名 → kInvalidArgument | ✅ | 174-183 |
| 符号链接跳过 | ✅ | 186-204 |
| 上限透传 → kTooLarge + 无残留 | ✅ | 207-219 |
| 非法参数（缺失/输出目录/输入=输出/空列表） | ✅ | 222-239 |
| 16MiB 吞吐 | ⚠️ | 242-262；阈值 `> 1.0 MiB/s` 形同虚设 |
| `BackupResult::archive_path` | ❌ | 全测试零引用 |
| 输出父目录不存在自动创建 | ❌ | 头文件 92 行承诺 |
| 特殊文件（FIFO/socket）计入 special_skipped | ❌ | — |
| 大量文件/深目录/并发不同输出 | ❌ | — |
| 磁盘/权限失败 → kIoError | ❌ | — |
| `BackupStatusToString` | ❌ | — |

### 2.5 upload（10 场景，97 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| PUT 成功：方法/路径/Authorization/token/Content-Type/body 逐字节 | ✅ | 332-363 |
| POST 成功 | ✅ | 366-385 |
| 两次 503 后 201（attempts=3） | ✅ | 388-412 |
| 始终 500：attempts=4、包保留、error 非空 | ✅ | 415-436 |
| 连接拒绝：attempts=3、包保留 | ✅ | 439-456 |
| 本地 zip 缺失：attempts=0 | ✅ | 459-466 |
| 参数校验 9 种（scheme/空白/header/token/重试/延迟/超时） | ✅ | 469-500 |
| 已有 Authorization 不重复 | ✅ | 503-523 |
| Content-Type 覆盖且不重复 | ✅ | 526-547 |
| 空归档上传 | ✅ | 550-567 |
| `kCurlMissing` | ❌ | 头文件 124 行；需清空 PATH 才能测 |
| `kTimeout` | ❌ | 服务器不应答即可触发 |
| 3xx 重定向 / 401 / 413 | ❌ | 只测 2xx/500/503 |
| 大于 512KB 的请求体与 `Expect: 100-continue` 边界 | ⚠️ | 512KB 已有，未测分块/大 body |
| 并发上传 / https 成功 / `UploadStatusToString` | ❌ | — |

### 2.6 capability（6 场景，124 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| 正常环境 15 项、≥8 可用、名称/reason/Summary/Reasons | ⚠️ | 31-78；环境强假设 + 恒真断言（37-39） |
| 手工矩阵 Size/Count/Find/IsDegraded/Reasons/Summary 空值 | ✅ | 81-117 |
| `/proc` 缺失 → 4 项不可用且 reason 含路径 | ✅ | 120-152 |
| 低 RLIMIT_NOFILE → degraded + soft=64 | ⚠️ | 155-190；两处可 SKIP |
| cgroup 缺失 → 3 项 degraded | ✅ | 193-209 |
| 关闭 epoll/interfaces/cgroup/rlimit | ✅ | 212-235 |
| `ProbeOptions::tmp_path` / `root_path` | ❌ | 143-145 |
| `min_mem_available` / `min_nproc=0` | ❌ | 147-148 |
| 并发 Probe（注释声称线程安全） | ❌ | 头文件 56-57 |
| 伪造 cgroup v1/v2 布局、proc 存在但不可读 | ❌ | — |
| Summary 精确计数（degraded/unavailable 数字） | ⚠️ | 64-66 只做 substring |

### 2.7 session（13 场景，180 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| `ParseEndpoint` IPv4/IPv6/空格/端口边界/11 种非法/null | ✅ | 245-268 |
| `Start` 配置校验（key/endpoint/channel/peer/server/重复 Start） | ✅ | 271-327 |
| 正常路径：状态序列合法性、双向数据、peer/channel/stats、优雅关闭 | ✅ | 330-416 |
| 配对超时（FakeClock + 无对端） | ✅ | 419-438 |
| 配对拒绝（双向容错断言） | ✅ | 441-469 |
| Connect 超时/失败（StubTransport） | ✅ | 472-497 |
| 心跳发送/接收；心跳超时单侧关闭 | ✅ | 500-530 |
| 控制权 FirstCome 仲裁、双端一致 | ✅ | 533-563 |
| 关闭窗口超时 | ✅ | 566-580 |
| 写失败 → kWriteFailed（未写任何字节） | ✅ | 583-595 |
| 非法 magic → kProtocolError | ✅ | 598-611 |
| 未激活发送/非法 channel/空指针/超大消息/关闭后 API/Close 幂等 | ✅ | 614-656 |
| `WaitFor()` | ❌ | 头文件 325；全测试零调用 |
| `AcceptPairing()` 正常路径 + `auto_accept_pairing=false` | ❌ | 只测未连接时 kInvalidState（626） |
| `max_outbox_frames` / `kQueueFull` | ❌ | 225 行配置、984/996 分支 |
| `transport_factory` 返回 nullptr → kTransportUnavailable | ❌ | 只测 server 无 factory |
| `pair_id` 固定、心跳/IO 超时 0 默认回落 | ❌ | 209-214 |
| 控制租约/参与者上限（`SessionControlConfig`） | ❌ | 218 行 |
| 真实 TCP 端到端、帧分片/粘包/半包、多客户端 | ❌ | 全部用 MemoryTransport/Stub |
| `SessionStateName` / `SessionErrorName` | ❌ | 118/146 行 |

### 2.8 udp_transport（15 场景，158 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| Bind/LocalEndpoint/重复 Bind(EALREADY)/Close 幂等/重 Bind | ✅ | 93-122 |
| 单包、100 包、1/7/200 边界、peer 端口 | ✅ | 128-232 |
| RecvFrom 超时（80ms，50..2000ms 窗口）、零超时非阻塞 | ✅ | 238-272 |
| 16KiB 大包 | ✅ | 274-298 |
| 截断（64B 缓冲收 4096B）+ 零缓冲探测/丢弃 | ✅ | 300-336 |
| 零长度数据报 | ✅ | 338-356 |
| Connect/Read/Write/PeerEndpoint/重复 Connect(EALREADY) | ✅ | 362-418 |
| 未连接/未打开错误、非法参数、EINVAL/ENOTCONN/EDESTADDRREQ | ✅ | 420-459 |
| 多发送方来源地址区分 | ✅ | 465-508 |
| ShutdownWrite 半关闭、Close 幂等 | ✅ | 510-551 |
| 65508B → EMSGSIZE | ✅ | 553-568 |
| IPv6 `::1` 回环 | ⚠️ | 574-606；不可用则 SKIP |
| `Bind(reuse_address=true)` | ❌ | 128 行参数 |
| 通配 host、域名解析、EADDRINUSE | ❌ | — |
| 连接不存在端口后 ECONNREFUSED、混合地址族 EAFNOSUPPORT | ❌ | 头文件 140-141 |
| `Read(capacity=0)`、未打开时 ShutdownWrite、`LocalEndpoint(nullptr)` | ❌ | — |
| 并发发送/接收、万包压测、双栈 | ❌ | — |

### 2.9 x25519（16 场景，1160 断言）

| 能力/分支 | 状态 | 证据 |
|---|---|---|
| RFC 7748 §6.1 Alice/Bob 公钥 | ✅ | 102-116 |
| RFC §6.1 共享密钥双向一致 | ✅ | 118-129 |
| RFC §5.2 两组标量乘向量 | ✅ | 131-145 |
| RFC 迭代 1 次/1000 次 | ✅ | 147-159 |
| 16 轮随机密钥对一致 + 非零 | ✅ | 161-185 |
| 私钥 clamp（低 3 位/高 1 位） | ✅ | 187-200 |
| 对端公钥最高位忽略 | ✅ | 202-216 |
| 低阶点 0/1、两组大点、p-1/p/p+1 拒绝并清零 | ✅ | 218-271 |
| 6 种 null 参数、生成私钥 null | ✅ | 273-285 |
| 确定性、输入不被修改 | ✅ | 287-300 |
| 生成私钥非恒定 | ⚠️ | 302-309；只比较 2 次 |
| 生成私钥"不 clamp"与分布/唯一性 | ❌ | 头文件 76-82 明确语义 |
| 私钥全 0/全 FF、输入输出同缓冲 | ❌ | — |

### 2.10 随机与真实场景（维度 4）

| 模块 | 随机用例 | 真实场景 | 评价 |
|---|---|---|---|
| fetch | 固定 PRNG 数据（非随机种子）；4 线程 | 本地 HTTP 协议、1MB、重试、重定向 | 真实场景 ≥8，随机性弱但可复现 |
| file_browser | mt19937 固定种子 500+200 | 真实 FS/权限/符号链接 | 达标 |
| zip | xorshift 10 轮随机树 | 真实 FS + 系统 unzip | 达标 |
| backup | 无（固定 16MiB） | 真实 FS | 随机性缺 |
| upload | 无（固定 zip） | 真实 curl + 本地 HTTP | 达标 |
| capability | 无 | 真实 /proc / cgroup / rlimit | 随机性缺，环境强依赖 |
| session | 无 | **MemoryTransport 模拟** | 真实网络端到端缺 |
| udp | 确定性 pattern | 真实 socket 回环 | 真实，随机长度/并发缺 |
| x25519 | 16 轮随机密钥 | RFC 官方向量 | 达标 |

## 3. 缺口表

级别：P0 安全/数据正确性关键路径；P1 公开 API 零覆盖或重要错误分支；P2 边界/并发/随机不足或断言弱；P3 维护性/一致性。

| ID | 级别 | 模块 | 缺失场景 | 建议用例（可直接落为测试） | 证据 |
|---|---|---|---|---|---|
| T-01 | P0 | zip | 读取端 Zip Slip 无测试：只验证写入端拒绝非法名，`ExtractZip` 从未收到恶意归档 | 生成 `a.txt` 归档，二进制把本地头与中央目录的名字等长改写为 `../x.t`；断言 `ListZip`/`ExtractZip` 返回 `kPathRejected`，且 `out` 之外不存在 `x.t`；再测绝对名 `/tmp/x`、反斜杠、重复名 | `test_zip.cpp:237-258` 只测写入；防御在 `src/bento/app/zip.cpp:955` |
| T-02 | P0 | zip | 读取端大小上限/解压炸弹未触发：测试用 0 字节条目 + `max_total_bytes=1` 仍成功 | 生成两个 600B 条目，读取端 `max_total_bytes=1024` → `kArchiveTooLarge`；单条 2048B + `max_entry_bytes=1024` → `kEntryTooLarge`；再构造声明 `uncompressed_size=1<<30` 但数据不足的归档 → `kCorruptArchive`/`kArchiveTooLarge` 且不分配内存 | `test_zip.cpp:446-466`；实现 `zip.cpp:960-964` |
| T-03 | P1 | session | `WaitFor()` 公开 API 零覆盖；`AcceptPairing()` 只测非法状态 | ① server 线程循环 `Poll`，主线程 `client.WaitFor(kActive, 2000)==true`；② `WaitFor` 超时返回 false（对端不 Poll）；③ `auto_accept_pairing=false` 时 `server.AcceptPairing()==kNone` 后双方 Active，未收到 CONNECT 时返回 kInvalidState | `test_session.cpp:626` 仅非法态；`session.cpp:1095` |
| T-04 | P1 | fetch | 大小上限只测 `Content-Length` 预检，流式累计超限未测 | 新增 `/chunked-big`（2MB chunked）与 `/no-length-big`（2MB 无 CL）路由，`max_bytes=1MB` → `kTooLarge`、`attempts=1`、目录无残留 | `test_fetch.cpp:606-625`；`fetch.cpp:963-964` |
| T-05 | P1 | fetch | `allow_redirects=false`、3xx 无 Location、连接拒绝、DNS 失败未测；`/empty` 路由定义了却没用 | ① `allow_redirects=false` + `/redirect` → `kHttpError`；② 302 无 Location → `kHttpError`；③ `http://127.0.0.1:1/` + `max_retries=0` → `kConnectFailed`；④ `/empty` 下载 → `kOk`、0 字节、SHA-256 为 `e3b0c442…` | `test_fetch.cpp:688-705`、299 |
| T-06 | P1 | file_browser | 自由函数 `List/Get/Put` 与 `options()` 零覆盖 | 三条自由函数各一例并与类方法结果对比；`FileBrowser(root, opts).options().max_entries == opts.max_entries` | 头文件 221-239、169；测试只用 `browser.*` |
| T-07 | P1 | backup | `BackupResult::archive_path` 从未断言；输出父目录自动创建未测 | `Backup({src}, tmp/"nested/deep/b.zip")` → `ok()`、`archive_path == 该路径`、文件存在；失败时 `archive_path.empty()` 且 `error` 非空 | `test_backup.cpp` 全文无 `archive_path`；头文件 92/101 |
| T-08 | P1 | upload | `kCurlMissing`、`kTimeout`、3xx 未测 | ① `setenv("PATH","",1)` 后 `Upload` → `kCurlMissing`、`attempts=0`、包保留（恢复 PATH）；② 服务器 sleep 超过 `request_timeout` → `kTimeout`；③ 返回 301/302 → `kHttpRejected`（curl 默认不跟随） | `test_upload.cpp:469-500`；头文件 124、128 |
| T-09 | P2 | session | `max_outbox_frames`/`kQueueFull`、`transport_factory` 返回 nullptr、`pair_id`、超时 0 回落未测 | 扩展 StubTransport 让 Write 返回可重试的 `kTimeout`，`max_outbox_frames=1` 连发两帧 → 第二帧 `kQueueFull`；factory 返回 nullptr → `kTransportUnavailable` | `session.cpp:336-337、884、996`；`test_session.cpp` 无相关断言 |
| T-10 | P2 | capability | `tmp_path`/`root_path`/`min_mem_available`/`min_nproc` 未注入；无并发 | `tmp_path` 指向不存在目录 → 对应项 degraded；`min_mem_available=UINT64_MAX` → degraded 且 reason 含阈值；8 线程并发 `Probe()` 结果 Size/Summary 相同；`min_nproc=0` 时 nproc 项不受限 | `test_capability.cpp` 无这些字段；头文件 141-153 |
| T-11 | P2 | udp | `reuse_address`、通配 host、EADDRINUSE、ECONNREFUSED、EAFNOSUPPORT、`Read(0)`、未打开 ShutdownWrite 未测 | ① `Bind({"",0})` 通配成功；② 两个 socket 绑同端口：第二个 `reuse_address=false` 失败/`true` 成功；③ `Connect` 到已关闭端口后 `Write` → `ECONNREFUSED`；④ IPv4 socket `Connect` 到 `::1` → `EAFNOSUPPORT` | `test_udp_transport.cpp:93-122`；头文件 128、140-141 |
| T-12 | P2 | 全部 | 9 个 `*ToString` 零覆盖（Fetch/FileBrowser/FileEntryType/Backup/Zip/Upload/HttpMethod/SessionState/SessionError） | 每个枚举遍历合法值：返回非空、不等于 `"unknown"`；抽查 2 个非法值的稳定输出 | 全仓库 `grep` 仅 `CapabilityStatusName` 被使用 |
| T-13 | P2 | zip | 直接 API 与边界：`AddExclusion`、`AddDirectory` 空 prefix、`limits()`、空归档 List/Extract、覆盖已存在文件、`AddBytes(nullptr,1)`、Close 后 Add、析构清理 | 逐条单用例；空归档断言 `entries.empty()` 且 Extract 成功；析构未 Close 后目标文件不存在 | 头文件 209-232、252-265 |
| T-14 | P2 | session | 帧层错误矩阵只有非法 magic；无分片/粘包 | StubTransport 依次注入 1 字节碎片：正常帧重组成功；长度字段非法/payload 超 `kFrameMaxPayload`/未知 type/channel 不匹配 → `kProtocolError` 且仅一次回调 | `test_session.cpp:598-611` |
| T-15 | P2 | 多模块 | 除 fetch 4 线程外无并发/压测 | zip 8 线程各写独立归档；file_browser 8 线程 List/Get；udp 4 线程各 1000 包；backup 并发不同输出。断言总成功数/无交叉污染 | 全仓库仅 `test_fetch.cpp:820-858` 有线程 |
| T-16 | P2 | 断言 | 恒真/过宽/弱断言 | 见 §4.2：删除恒真等式，吞吐阈值改 20MiB/s，fuzz 合法路径断言 `kOk` 并校验内容 | `test_capability.cpp:37-39`、`test_backup.cpp:122,261`、`test_file_browser.cpp:648` |
| T-17 | P3 | x25519 | 生成私钥的熵/未 clamp 未验证 | 生成 1000 个私钥，唯一数 > 990；统计 `b[0]&7`、`b[31]&0xC0` 不被强制清零（因不 clamp）；同一缓冲重复生成会变化 | `test_x25519.cpp:302-309`；头文件 76-82 |
| T-18 | P3 | 全部 | SKIP 静默通过 | 统一 `return 77` + `SKIP_RETURN_CODE 77`，或引入 `bento_test::Skip()` 在 `Finish` 打印 `[SKIP] N` | 见 §5 |
| T-19 | P3 | fetch | URL 语法边界（userinfo/fragment/IPv6/超长） | `http://u:p@host/` → `kInvalidArgument`；`http://host/a#f` → `kInvalidArgument`；8193B URL → `kInvalidArgument`；`http://[::1]:port/` 可用时成功 | 头文件 227-228；测试 425-447 未覆盖 |
| T-20 | P3 | file_browser | `kOther`、Get 目标为目录、Put 覆盖符号链接、相对根 | `mkfifo` 后 List 类型为 `kOther`；Get 到已存在目录 → 明确状态；Put 覆盖符号链接不写到链接目标外；相对根构造后 `root()` 为绝对路径 | 头文件 88-93、154-160 |

## 4. 断言质量

### 4.1 做得好的地方

- 不是"只断言 OK"：`fetch` 1MB 用例同时校验字节数、SHA-256、逐字节内容、attempts、http_status、目录条目数、请求数（`test_fetch.cpp:468-478`）；`upload` 校验方法/路径/3 个 header/body 逐字节（`test_upload.cpp:349-362`）。
- `zip` 用系统 `unzip -t`/`unzip -p` 交叉验证，`backup` 用 `CompareTrees` 做路径集合+类型+大小+内容比对（`test_backup_support.hpp:141-204`），比"文件存在"强得多。
- `session` 在状态回调里断言 `IsSessionTransitionAllowed(from,to)`（`test_session.cpp:97-100`），把"状态机合法性"变成每次迁移的检查，而不是只查终态。
- `x25519` 用 RFC 官方向量 + 迭代 1000 轮，失败时能定位到具体向量名（`PrintCase`）。

### 4.2 弱断言 / 无效断言

| 位置 | 断言 | 问题 | 建议 |
|---|---|---|---|
| `test_capability.cpp:37-39` | `Unavailable + Degraded == Size - Available` | **恒真**（三状态互斥且穷尽），永远不可能失败 | 删除，或改为精确断言三个 Count |
| `test_capability.cpp:64-66` | `summary.find("total=15")`、`find("available=")` | 只验证子串，degraded/unavailable 数字写错也通过 | 精确匹配 `capabilities: total=15 available=%zu degraded=%zu unavailable=%zu` |
| `test_backup.cpp:122` | `second.stats.archive_bytes < first_size + 4096` | 只证明没把整包打进去，无法发现多打包了其他文件 | 断言 entries 精确集合 + `second.stats.files == 1` |
| `test_backup.cpp:261` | `mb_per_second > 1.0` | 4MiB 顺序写盘的 1MiB/s 门槛几乎不可能失败，性能回归不可见 | 改为 20MiB/s（CI 抖动可降到 10），或只打印不断言 |
| `test_file_browser.cpp:648` | `status == kOk || kNotFound` | 合法路径只要不返回 `kPathRejected` 就过，无法发现"解析到错误目录" | 对已知存在的 `docs` 断言 `kOk` 且 `entries.empty()`；对已知不存在的断言 `kNotFound` |
| `test_fetch.cpp:778-780` | `kTlsError || kConnectFailed || kTimeout` | 三个错误都接受，无法区分分支 | 固定服务器行为后收窄到 `kTlsError`（curl 对明文 HTTP 的 TLS 错误） |
| `test_udp_transport.cpp:168-195` | 100 包循环只汇总 `all_ok` | 断言数=1，失败定位靠 stderr；无法统计成功包数 | 保留 `all_ok`，另加逐包 `BENTO_CHECK_EQ`（或在失败时记录 index） |
| `test_x25519.cpp:308` | 两次生成私钥 `first != second` | 2 次样本，熵源退化（如固定种子）仍可能通过 | 1000 次 + 唯一性阈值 |
| `test_zip.cpp:462` | tiny limits 下 `ExtractZip(...).ok()` | 条目为 0 字节，上限 1 未被触发，等于没测读取端上限 | 换成非零条目（见 T-02） |

### 4.3 缺失的断言

| 模块 | 未断言的字段/语义 | 影响 |
|---|---|---|
| fetch | `result.message` 在所有失败用例中从未断言；成功时 `path` 是否绝对路径未断言 | 错误诊断文案回归不可见；`path` 语义漂移不可见 |
| backup | `archive_path`、失败时 `error` 非空、成功时 `error` 为空 | 结果对象一半字段未被验证 |
| zip | `ExtractZip` 的 `stats`（files/directories/input_bytes）、失败时 `error` 非空 | 解压统计与诊断未验证 |
| upload | 成功时 `error` 为空、失败时 `http_status==0` 的语义 | 诊断字段未验证 |
| capability | `detail` 只验证 meminfo 非空，未与 `/proc/meminfo` 实际值交叉 | 探测数值错误不可见 |
| session | `stats` 多为 `>=1`；`last_error_message` 只在 connect failure 断言 | 计数精度/文案未验证 |
| udp | `RecvInfo.bytes` 在成功路径有断言（好），但 `LocalEndpoint` 的 host 只对 IPv4 断言 | 地址格式化回归风险低 |
| x25519 | 生成私钥未被 clamp 的语义（头文件明确"不 clamp"） | 文档与实现漂移不可见 |

### 4.4 断言宏与并发

- `BENTO_CHECK` 使用非原子 `inline int g_failures/g_checks`（`test_util.hpp:15-38`）。当前只有 `fetch` 的 4 线程用例，且断言全部在 `join` 之后主线程执行（`test_fetch.cpp:846-856`），因此安全；`x25519` 也注明"断言只在主线程"。
- 若按 T-15 增加并发断言，必须改为 `std::atomic<int>` 或每线程独立计数后合并，否则会产生数据竞争（UB），且计数丢失。建议在 `test_util.hpp` 注释中写明这一约束（现在只有 x25519 文件头提到）。
- `BENTO_CHECK` 失败后继续执行：对"失败后读取未初始化/空路径"的用例，当前都额外 `if` 保护（如 `test_fetch.cpp:385`、`test_file_browser.cpp:217`），做法正确。

## 5. SKIP 逃逸

| 位置 | 触发条件 | 当前行为 | 风险 |
|---|---|---|---|
| `test_fetch.cpp:775-783` | `kBackendUnavailable`（无 curl） | `printf("[SKIP]")` 后仍走 `Finish`，ctest PASS | https 分支 0 覆盖但结果不可见 |
| `test_file_browser.cpp:507-510` | `geteuid()==0` | 直接 `return` | root CI 下权限用例永久不跑 |
| `test_zip.cpp:172-184` | 无 `unzip` | 只跑内部读取，打印 `[skip]` | 交叉验证缺失但 PASS |
| `test_zip.cpp:330-333` | `symlink()` 失败 | `printf` 后 `return` | 符号链接用例不跑 |
| `test_backup.cpp:190-193` | `symlink()` 失败 | 同上 | 同上 |
| `test_capability.cpp:162-172` | rlimit 已 ≤64 / `setrlimit` 被拒 | `printf` 后 `return` | 低 rlimit 用例不跑 |
| `test_udp_transport.cpp:581-586` | IPv6 `::1` 不可用 | `printf` 后 `return` | IPv6 覆盖缺失但 PASS |
| `test_upload.cpp` | 无 curl | **没有 SKIP**，直接失败 | 与 fetch 策略不一致 |

建议（择一）：
1. 统一 `return 77`，在 CMake 里 `set_tests_properties(<test> PROPERTIES SKIP_RETURN_CODE 77)`；这是 CTest 原生语义，`ctest -N`/报告能区分 SKIP。
2. 或在 `bento_test` 增加 `Skip(name)` 计数，`Finish` 输出 `[PASS] name (N checks, M skipped)`，CI 解析 M。

## 6. 可复现性与 flaky 风险

| 维度 | 现状 | 评价 |
|---|---|---|
| 端口 | fetch/upload 均 `sin_port=0` 由内核分配（`test_fetch.cpp:172`、`test_upload.cpp:154`） | ✅ 无固定端口冲突 |
| 临时目录 | `pid + 自增 + tag`（`test_fetch.cpp:98-110`、`test_backup_support.hpp:30-46`） | ✅ 并行安全 |
| 时钟 | session 用注入 `FakeClock`（`test_session.cpp:44-47`） | ✅ 状态机用例完全可复现 |
| 真实时间 | fetch 读超时 200ms vs 服务器 800ms（4× 余量）；udp 80ms 超时断言 50..2000ms | ⚠️ 极端负载下 udp 上界 2000ms 可能误报；建议只保下界 |
| 外部依赖 | upload 硬依赖 curl；zip/backup 可选 unzip；udp 可选 IPv6；file_browser 依赖非 root | ⚠️ 环境差异导致覆盖漂移，见 §5 |
| 环境假设 | capability 硬断言 15 项、≥8 可用、meminfo 可读（`test_capability.cpp:33-36`） | ❌ 受限容器/非 Linux 直接失败而非 SKIP |
| 数据量 | fetch 2×2MB + 1MB、backup 16MB、zip 1MB、file_browser 1MB | ✅ 内存/磁盘压力可控 |
| 外网 | 无，全部 127.0.0.1/本地文件 | ✅ |
| 并行 ctest | 各测试独立临时目录与端口 0，理论上 `ctest -j` 安全 | 未实测，建议 `-j 4 --repeat until-fail:20` 验证 |
| 已知慢点 | fetch `/slow` 的 `Stop()` 需等服务器线程 25ms 粒度退出；总测试仍 0.42s | 可接受 |

建议补一条 CI 稳定性验证：`ctest --test-dir build-review -R "^bento_(...)$" --repeat until-fail:20 -j 4`，把结果记入报告；本次未执行（只跑单次）。

## 7. 优先补测 Top 3

1. **T-01/T-02（P0，zip 读取端恶意归档）**：Zip Slip + 总量/单条炸弹。当前写入端拒绝非法名给了"已经防住"的错觉，但读取端才是攻击面；这两个用例可用现有 writer 生成后二进制等长改名实现，成本低、价值最高。
2. **T-04/T-05（P1，fetch 流式上限与重定向/连接错误）**：`BodySink` 的流式上限、`allow_redirects=false`、3xx 无 Location、连接拒绝、`/empty` 0 字节。头文件把"流式累计上限"和"3xx 无 Location 归 kHttpError"写成契约，但测试只覆盖了 Content-Length 预检。
3. **T-03（P1，session WaitFor/AcceptPairing 正常路径）**：`WaitFor` 是公开阻塞 API，零覆盖；`AcceptPairing` 只测了错误态。用 server 线程 + 主线程 `WaitFor(kActive)` 即可覆盖，顺便验证 `auto_accept_pairing=false` 的显式配对流程。

## 8. 附：取证命令

```bash
# 配置 + 构建 9 个目标
cmake -S . -B build-review -DGO2CPP_WITH_BENTO=ON
cmake --build build-review -j 8 --target bento_test_fetch bento_test_file_browser \
  bento_test_zip bento_test_backup bento_test_upload bento_test_capability \
  bento_test_session bento_test_udp_transport bento_test_x25519

# 运行
ctest --test-dir build-review -R "^bento_(fetch|file_browser|zip|backup|upload|capability|session|udp_transport|x25519)$" \
  --output-on-failure

# 断言数/SKIP 取证
cd build-review && ./bento_test_fetch 2>&1 | tail -1   # 其他同理
```

本次结果：9/9 Passed，1.50s，4068 次断言，0 次实际 SKIP。
