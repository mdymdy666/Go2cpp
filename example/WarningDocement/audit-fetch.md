# fetch 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/app/fetch.hpp`、`src/bento/app/fetch.cpp`、
  `tests/bento/test_fetch.cpp`
- 审计工具：`tests/bento/audit_fetch_stress.cpp`（压测 + 5 场景 + 并发同目标）、
  `tests/bento/audit_fetch_random.cpp`（1220 个随机响应）、
  `tests/bento/audit_fetch_support.hpp`（可脚本化 HTTP 服务器，不依赖 `link.hpp`）
- 构建：`.build-bento-wsl`（Ninja，Release `-O3`，GCC 13.3.0，12 vCPU / 19 GiB，
  WSL2 Ubuntu 24.04，`ulimit -n = 10240`，`somaxconn = 4096`）
- 结论：**无 P0**。修复 **2 个 P1**（`overwrite=false` 并发落盘 TOCTOU、HTTPS 的
  `read_timeout_ms` 从未生效）与 **2 个 P2**（IO 错误无 errno 诊断、读取路径两次多余
  拷贝），记录 6 个设计风险。`ctest -R bento` **48 项 47 过 + 1 环境 SKIP**
  （`bento_netem` 无 `CAP_NET_ADMIN`）；`bento_test_fetch` 249 checks 全过；
  新增审计程序 **139 checks + 3528 checks** 全过。

## 1. 压测数据

### 1.1 大文件下载（100 MiB，本地环回，服务端零生成开销）

| 项目 | 耗时 | 吞吐 | 峰值 RSS（增量） | fd 峰值 |
|---|---:|---:|---:|---:|
| 100 MiB（Content-Length） | 344 ms | 290 MiB/s | 5.8 MiB（+0.1） | 8 |
| 100 MiB（chunked，64 KiB 块） | 361 ms | 277 MiB/s | 5.7 MiB（+0.1） | 8 |
| 64 MiB（恰好等于默认上限） | 217–234 ms | 274–296 MiB/s | 31 MiB（含前面用例） | 5 |
| **SHA-256 纯计算基准（100 MiB）** | **246 ms** | **407 MiB/s** | — | — |

**结论：吞吐上限由 SHA-256 决定，而不是 IO/拷贝。** 摘要计算占 100 MiB 下载耗时的
**71%**（246/344 ms），因此读取路径的拷贝优化最多只能吃下剩余 29% 的一部分。
峰值 RSS 增量 0.1 MiB 证明「边读边写 + 边算摘要」确实流式，**没有把文件读进内存**。

### 1.2 千级并发

| 项目 | 数值 |
|---|---|
| 1000 并发 fetch（每次 4 KiB 成功落盘） | 107 ms，**9.4k QPS** |
| 峰值 RSS 增量 | 24.8 MiB（1000 × 512 KiB 固定栈，实际按需触页） |
| fd 峰值 | 17–18（12 vCPU 下真正同时在飞的请求数，不是 1000） |
| 落盘校验 | 1000 个文件、字节总数 4 000 000、SHA 全部一致、**零临时文件残留** |
| 100+ 混合并发（1 KiB / 256 KiB / 1 MiB，44.6 MiB 总量） | 25–41 ms，1.0–1.7 GiB/s |
| 8 线程同一目标（`overwrite=false`） | 修复前 **ok=8 / target_exists=0**；修复后 **ok=1 / target_exists=7** |

### 1.3 内存与 fd 基线

- 单次 100 MiB 下载只占 **1 个 socket + 1 个临时文件 fd**，峰值 8（含资源采样器自己
  的 `/proc` 句柄）。
- 1220 个随机用例跑完后 fd 6→4、RSS 11.9→12.5 MiB，**无泄漏**。

## 2. 场景数据（5 个复杂场景，全部实跑）

| 场景 | 实测 |
|---|---|
| 重定向链 302→302→200 | 3 次请求、1 次尝试、SHA 精确匹配；落盘名取自**原始** URL 最后一段 |
| 5 跳链（= `max_redirects` 默认 5） | 6 次请求后成功 |
| 7 跳链（超上限） | 第 6 个 302 后 `kHttpError`，诊断含「重定向次数」，无残留 |
| 跨端口绝对 URL 重定向 | 命中另一 server，`http://127.0.0.1:<port2>` 重建正确 |
| **连接超时**（accept 队列占满 → SYN 被丢） | `kRetryExhausted`，attempts=4，1273 ms ≈ 4×300 ms + 退避 |
| 不可达地址 10.255.255.1:81 | `kRetryExhausted`，attempts=4，message「重试 3 次后仍失败: 连接超时」 |
| **读空闲超时** 200 ms | `kRetryExhausted`，4 次请求，872 ms ≈ 4×200 ms + 退避 |
| **总超时** 600 ms（read 故意放大到 1000 ms） | `kRetryExhausted`，4 次请求，2474 ms ≈ 4×600 ms + 退避 |
| `max_retries=0` 的读超时 | `kTimeout`、attempts=1 —— 分类不被重试包装吞掉 |
| 只读目录（0555） | `kIoError`、attempts=1，**未发起网络请求**（本地 open 先失败） |
| `RLIMIT_FSIZE=1 MiB` 下下载 8 MiB | `kIoError`、attempts=1，「写入临时文件失败: File too large」，零残留 |
| 65 MiB + `Content-Length`（默认 64 MiB 上限） | `kTooLarge`、attempts=1、**0 字节传输**、0.3 ms（预检即拒） |
| 65 MiB chunked | `kTooLarge`、attempts=1，传到 64 MiB 中止（服务端多写 3 MiB 内核缓冲） |
| 恰好 64 MiB | 成功，SHA 匹配 |
| `max_bytes = 4 GiB + 1` | `kInvalidArgument` |

## 3. 随机测试（1220 用例 / 3528 断言，全过）

| 类别 | 数量 | 断言结果 |
|---|---:|---|
| 合法 Content-Length（0–64 KiB 随机） | 180 | 全部 `kOk`，SHA/字节数精确匹配 |
| 合法 chunked（chunk 1–8 KiB 随机） | 180 | 全部 `kOk`，SHA/字节数精确匹配 |
| 无 Content-Length（EOF 模式） | 180 | 全部 `kOk`，SHA/字节数精确匹配 |
| 截断（声明 > 实际） | 150 | 全部 `kTruncated` |
| 重定向循环（自指 120 + 两跳互指 20 对） | 140 | 全部 `kHttpError` |
| 非法头 / 非法 chunk / 头超 64 KiB / 不支持的编码 | 150 | 全部 `kProtocolError` |
| 超时（静默 / 40 ms 一字节慢滴流） | 80 | 全部 `kTimeout` |
| 4xx / 5xx（10 种状态码） | 100 | 全部 `kHttpError`，`http_status` 正确 |
| 接受即关闭（连接重置） | 40 | `kProtocolError` / `kTruncated` / `kConnectFailed` |
| **合计** | **1220** | **3528 checks 全过；540 个成功用例逐字节可验；零临时文件残留** |

## 4. 问题清单

### P1（已修复）

**P1-1 `overwrite=false` 的「目标已存在则拒绝」在并发下完全失效（TOCTOU）**

- 证据：8 个线程同时下载到同一目标，**修复前 8 个全部成功落盘**（`ok=8,
  target_exists=0`）。头文件承诺「false 直接返回 `kTargetExists`，不会发起网络
  请求」，实际 8 个请求都发了网络请求、都写了临时文件、最后互相覆盖。
- 根因：`Fetch` 开头用 `std::filesystem::exists` 做快速拒绝，落盘在最后用
  `std::filesystem::rename`（覆盖语义），两步之间没有任何排他；检查与 rename 之间
  的窗口足够并发请求全部通过。
- 修复：新增 `CommitNoReplace()`，`overwrite=false` 时走
  `renameat2(AT_FDCWD, tmp, AT_FDCWD, target, RENAME_NOREPLACE)`；`EEXIST` 转
  `kTargetExists` 并清理临时文件；内核/文件系统不支持该标志（`ENOSYS`/`EINVAL`/
  `EOPNOTSUPP`）时回退普通 `rename`，不破坏老平台。
- 回归：`audit_fetch_stress` 的「8 线程同一目标」现在断言 `ok==1 && target_exists==7`。
- 位置：`src/bento/app/fetch.cpp` 的 `CommitNoReplace()` 与落盘分支。

**P1-2 HTTPS 路径的 `read_timeout_ms` 从未生效（与头文件契约不一致）**

- 证据：`BuildHttpsCurlConfig()` 只下发 `connect-timeout` 与 `max-time`，没有任何
  空闲/速率约束。一个 TLS 握手成功但停发数据的 HTTPS 服务器，会让下载一直挂到
  `total_timeout_ms`（默认 **10 分钟**），而同样条件的 HTTP 服务器 15 秒就
  `kTimeout`。
- 修复：下发 `speed-limit = "1"` 与 `speed-time = ceil(read_timeout_ms/1000)`。
  curl 语义是「窗口内平均速度低于 1 B/s 即中止」，退出码 28 → 既有映射已是
  `kTimeout`。仍在 10 分钟内完成的慢速下载不受影响（1 B/s 远低于任何正常速率）。
- 文档：`fetch.hpp` 新增「三层超时的实现方式」小节，明确 HTTPS 侧是**近似**而非
  精确空闲计时。
- 位置：`src/bento/app/fetch.cpp` 的 `BuildHttpsCurlConfig()`。

### P2（已修复）

**P2-1 本地 IO 错误没有 errno 诊断**

- 证据：`RLIMIT_FSIZE` 触发 EFBIG 时，`FetchResult::message` 只有
  `"io_error"`（`FetchStatusToString` 的兜底），调用方无法区分 ENOSPC / EFBIG /
  EPERM / EROFS。
- 修复：`BodySink::Append()` 增加 `std::string* message` 参数，写失败时填
  「写入临时文件失败: <strerror>」；`ReadExact` / `ReadUntilEof` 两个调用点透传。
- 实测（修复后）：「写入临时文件失败: File too large」。

**P2-2 读取路径每 64 KiB 多两次内存拷贝 + 一次 64 KiB 零初始化**

- 证据：`SocketReader::Fill()` 每次在栈上 `std::array<char, 64*1024> buffer{}`
  零初始化，`recv` 进临时缓冲后 `append` 到 `m_buffer`，`ReadSome()` 再 `memcpy`
  到调用方缓冲 —— 100 MiB 下载等于 100 MiB memset + 200 MiB memcpy。
- 修复：抽出 `WaitReadable()`（poll + 空闲/总超时）与 `ReadDirect()`；`ReadSome()`
  在 `capacity >= 16 KiB` 且内部缓冲为空时直接把字节 `recv` 进调用方缓冲；`Fill()`
  复用成员 `m_fill` 缓冲。
- **实测结论：端到端无可测量提升**（100 MiB 288 → 290 MiB/s，噪声内）。原因见
  §1.1：SHA-256 占 71%。改动保留（逻辑上更省、无副作用、降低小文件高并发下的
  CPU 拷贝压力），但**不宣称有吞吐收益**。

### P2（记录，未改）

| 编号 | 问题 | 影响 | 建议 |
|---|---|---|---|
| P2-3 | `ConnectTcp` 直接调用阻塞 `getaddrinfo`，**DNS 解析不在任何超时之内** | 黑洞 DNS 时卡住 5–10 s（resolv 默认重试），`connect_timeout_ms` 管不到 | 用 `getaddrinfo_a` 或独立线程 + 超时；改动面大，记录 |
| P2-4 | 临时文件名可预测（`<target>.go2cpp-part.<pid>.<seq>`） | 同目录其他用户可预置同名文件让 `O_EXCL` 失败 → 下载失败（**DoS，不能劫持内容**） | 改用 `mkostemp` 随机后缀 |
| P2-5 | 沙箱 `weakly_canonical` 与 `O_NOFOLLOW` 之间对**中间目录**无保护 | `dest_dir` 中途被换成符号链接时写入可能重定向（需同目录写权限 + 时间窗） | 用 `openat2(RESOLVE_BENEATH)` 或逐级 `openat` |
| P2-6 | HTTP→HTTPS 重定向后，`total_timeout_ms` 会**重新计时** | 跨协议重定向的单次尝试可能超出总超时约 1 倍 | 把剩余时间传给 `max-time` |
| P2-7 | 失败路径的 `fail()` lambda 每次返回 `FetchResult` 拷贝（4 个 std::string） | 只在失败路径，可忽略 | 不修 |
| P2-8 | 测试层 SKIP 逃逸：`TestHttpsViaCurl` 在无 curl 时 `Skip`，`Finish()` 因 `g_skips != 0` 让**整份** `bento_test_fetch` 以 77 退出 | 无 curl 的 CI 上，11 个 https 场景不跑、其余 checks 通过却只显示「Skipped」 | 拆独立测试目标；或 skip 只降级该场景 |

### P3（记录）

- 文件名上限 200 B + 临时后缀最长约 48 B（`NAME_MAX` 255），余量约 7 B。
- `NormalizeUrlPath` 把路径里的 `//` 折叠成 `/`，改变原始语义（对多数服务器无害）。
- `IsValidPort` 接受前导零（`0080`），`HostHeader` 按字符串比较默认端口，会输出
  `host:0080`。
- 落盘无 `fsync`：`rename` 原子但崩溃可能丢数据（与仓库其他模块一致的性能取舍）。
- 状态行只校验 `HTTP/` 前缀，不校验版本号（`HTTP/9.9` 也接受）。

## 5. 解耦

- **头文件自包含**：`fetch.hpp` 只 include `<cstddef>/<cstdint>/<string>/<string_view>`；
  单独 `#include` + `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` 通过。
- **项目内依赖只有 4 个**：`fetch.hpp` 自身、`app/detail/retry.hpp`、`os/proc.hpp`、
  `hash/sha256.hpp`（新增的是系统头 `<linux/fs.h>`）。**没有** `link.hpp` /
  `peer_hub` / `session` / `transport` / `frame`。
- **符号级证据**：`nm -C --undefined-only fetch.cpp.o` 的项目符号只有
  `go2cpp::hash::Sha256::{Reset,Write,Sum}` + vtable、`go2cpp::os::{Run,ProcError::ToString}`。
- **最小链接验证**：`fetch.cpp.o + libgo2cpp_os.so + libgo2cpp_hash.so`（**不链接**
  `libgo2cpp_bento.a`）链接成功，探针程序运行返回 `kConnectFailed`（符合预期）。

## 6. 测试复查（`tests/bento/test_fetch.cpp`）

- **实际规模：25 个场景函数 / 249 checks**（文件头注释写「16 个场景」，main 里 25 个
  调用，其中若干函数内含多个子用例）。任务书里的「17 场景 / 138 checks」是旧版数字。
- **断言有效性**：关键断言都有实质校验 —— SHA 逐字节比对、`CountEntries` 残留检查、
  `attempts` 次数、`server.requests()` 网络请求计数、`elapsed < 250 ms` 的无响应体时延。
  未发现恒真断言。
- **条件包裹的断言**（P3 形式问题）：`TestNoBodyStatusCodes` 与
  `TestHttpsRejectsUnexpectedStatus` 里 `if (result.ok()) { BENTO_CHECK_EQ(file_size…) }`，
  前置已有 `status == kOk` 断言所以不会静默跳过，但建议改成先断言再取值。
- **SKIP 逃逸**：见 P2-8。当前环境有 curl，249 checks 实际全跑。
- **覆盖缺口**（本次审计程序已覆盖，建议逐步回流）：
  1. 只有 1–2 MiB，无 100 MiB 级流式与内存断言；
  2. 并发只有 4 线程 / 1 MiB，无 1000 并发、无同目标竞争；
  3. 只测读空闲超时，无连接超时与总超时；
  4. 无磁盘写失败（ENOSPC/EFBIG/只读目录）；
  5. 无默认 64 MiB 上限边界（只有人为 1 MiB）；
  6. 无 DNS 失败、IPv6 回环、`100-continue`、HTTP/1.0 响应、chunked trailer；
  7. 无跨协议重定向（https→http 被 `proto-redir` 拒绝）；
  8. `elapsed < 250 ms` 这类时延断言在负载高的 CI 上可能假失败（P3 脆弱断言）。

## 7. 修复清单与验证

| 文件 | 改动 |
|---|---|
| `src/bento/app/fetch.cpp` | `SocketReader` 重构（`WaitReadable`/`ReadDirect`/`m_fill`）；`BodySink::Append` 带 errno 诊断；`BuildHttpsCurlConfig` 补 `speed-limit`/`speed-time`；新增 `CommitNoReplace()` + 排他落盘分支；`<linux/fs.h>` include |
| `include/go2cpp/bento/app/fetch.hpp` | 新增「三层超时的实现方式」；`Fetch` 的 overwrite 并发语义说明 |
| `tests/bento/audit_fetch_support.hpp`（新增） | 不依赖 link 的可脚本化 HTTP 服务器 + 确定性数据流 + 资源观测 |
| `tests/bento/audit_fetch_stress.cpp`（新增） | 100 MiB / 1000 并发 / 同目标竞争 / 5 场景 |
| `tests/bento/audit_fetch_random.cpp`（新增） | 1220 个随机响应 / 3528 断言 |

验证结果：

- `ninja`：**0 警告**（`-Wall -Wextra -Wpedantic -Werror`）。
- `bento_test_fetch`：`[PASS] 249 checks`。
- `audit_fetch_stress`：`[PASS] 139 checks`。
- `audit_fetch_random`：`[PASS] 3528 checks`。
- `ctest -R bento`：**48/48，100% passed**（`bento_netem` 因无 `CAP_NET_ADMIN`
  环境 SKIP，与 fetch 无关）。
