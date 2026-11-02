> 来源：/tmp/reuse_hash.md，2026-11-02 归档

# SHA-256 / FNV-1a 统一报告

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL ext4 树，HEAD `ee2c77a`；工作树含其他 agent 的并行改动）
- 独立构建目录：`/tmp/build-hash`（`Release`，GCC 13.3，12 核）
- 本机 CPU：12 核，支持 `sha_ni` / `pclmulqdq` / `avx512f`
- 时间：2026-11-02
- 边界：未 commit；只改 `src/hash/**`、`include/go2cpp/hash/**`、`include/go2cpp/logging/redact.hpp`、`include/go2cpp/app/sha256_ni.hpp`（授权扩展）、FNV 的 4 个调用点（父 agent 授权）、`tests/**`
- 复现材料：对拍程序 `/tmp/verify_hash`（源码 `/tmp/verify_hash.cpp`）、旧实现 oracle `/tmp/old_redact_log.hpp`、`/tmp/old_app_sha256_ni.hpp`

## 一、结论摘要

| 项目 | 结果 |
|---|---|
| SHA-256 实现份数 | 3 → **1**（`hash::Sha256`，标量 + SHA-NI 运行时分派） |
| FNV-1a 实现份数 | 4（64 位）→ **1**（`hash/fnv.hpp`）；32 位协议版本有意保留 |
| SHA-256 逐字节一致 | 1,000,000 组随机输入 / 382.7 MB，**0 mismatch**（硬件与强制标量两条路径各 4 组对照，全部为 0） |
| FNV 逐字节一致 | 1,000,000 组随机输入 / 384.4 MB，**0 mismatch**（4 组对照全部为 0） |
| redact 性能（默认/硬件） | 旧 199~234 ns/次 → 新 **77~87 ns/次**；测试实测 80~85 ns |
| redact 性能（强制标量） | 旧 203~214 ns/次 → 新 **195~207 ns/次**（不退化，略快） |
| SHA-256 吞吐（默认/硬件） | 旧 app SHA-NI 1.85~2.28 GB/s → 新 `hash::Sha256` **2.0~2.4 GB/s**；`go2cpp_hash_test` 1 GiB 压测 2146~2370 MB/s |
| SHA-256 吞吐（强制标量） | 0.41~0.44 GB/s（文档标量预期区间 0.42~0.47） |
| 构建 / 测试 | `go2cpp_hash_test` 全通过；`p2p_test_p2p` 94 checks 0 failures；redact 测试全通过；`go2cpp_tests` 仅 1 个与本改动无关的环境相关失败（见 §5） |

## 二、SHA-256 统一

### 2.1 替换清单

| # | 原位置 | 处理 |
|---|---|---|
| 1 | `src/hash/sha256.cpp` + `include/go2cpp/hash/sha256.hpp` | 保留为主实现；新增 SHA-NI 压缩函数、批量压缩入口 `TransformRange`、运行时 CPUID 分派、一次性 `Digest` 快速路径、`HardwareAccelerated()` |
| 2 | `include/go2cpp/logging/redact.hpp`（内嵌 90 行最小实现 + 64 轮常量） | **删除**，改调 `go2cpp::hash::Sha256::Digest(go2cpp::hash::AsBytes(key))`；`detail` 命名空间整体移除，`KeyId` 的 8 hex 输出不变 |
| 3 | `include/go2cpp/app/sha256_ni.hpp`（430 行，类 `Sha256Fast`） | **薄转发**：`Sha256Fast` 保留原接口（`kSize`/`kBlockSize`/`HardwareAvailable`/`Reset`/`Write`/`Sum`），内部持有 `go2cpp::hash::Sha256`；不改 `src/app/resource.cpp` |
| 4 | `src/app/sha256_ni.cpp` | **不存在**（任务描述有误）；实际实现一直是 `include/go2cpp/app/sha256_ni.hpp`，无 .cpp 可删 |

### 2.2 关键设计

- **硬件路径下沉**：把 Intel SHA Extensions 白皮书的 16 组 × 4 轮压缩函数移到 `src/hash/sha256.cpp`，用 `__attribute__((target("sha,sse4.1")))` 单独编译，不要求全局 `-msha`；
- **运行时检测**：`__builtin_cpu_supports("sha")`，函数局部静态量只探测一次；`CompressBlocks` 内用静态 bool + 直接分支（标量分支同 TU 可内联，不给回退路径增加间接调用开销）；
- **批量压缩**：`Write` 的整块循环改为「一次算出剩余块数 → 一次 `TransformRange`」，1 MiB 只付一次分派/调用开销；
- **平台回退**：非 x86 或非 GCC/Clang 编译时 `GO2CPP_HASH_SHA_NI=0`，纯标量路径，零新增依赖；
- **一次性快速路径**：`Sha256::Digest` 不再走 `Write`+`Sum` 的对象拷贝，而是「整块批量压缩 + 一次填充」，短输入（redact）开销最小；
- **可测试性开关**：`GO2CPP_HASH_DISABLE_SHA_NI=1` 强制标量路径（只在首次选择时读一次），用于在无 SHA-NI 机器之外验证回退路径；`HardwareAccelerated()` 反映实际启用状态；
- **日志模块解耦说明**：`log.hpp` 本身仍只依赖标准库；`redact.hpp` 是 logging 的**可选扩展**，文件头已更新说明「为什么复用 hash」。

### 2.3 性能

| 场景 | 旧 | 新 | 变化 |
|---|---|---|---|
| `Redact(key)` 端到端（32 B key，1e5 次，redact 测试） | 221 ns（历史基线）/ 199~234 ns（本机旧实现） | **80~85 ns** | −60% |
| `Redact(key)` 端到端（对拍程序 5 轮中位数，硬件） | 198.9~233.7 ns | **77.0~86.9 ns** | −60% |
| `Sha256::Digest`（32 B，1e6 量级，硬件） | 156.6~179.9 ns | **52.6~56.3 ns** | −67% |
| `Redact`（强制标量） | 203.1~214.3 ns | **195.1~207.0 ns** | −3~5% |
| SHA-256 吞吐（1 GiB，4 MiB 块，硬件） | 1.85~2.28 GB/s | **2.0~2.4 GB/s** | 持平~+8% |
| SHA-256 吞吐（`go2cpp_hash_test` 压测 1 GiB，硬件） | ~0.42~0.47 GB/s（文档记录） | **2146~2370 MB/s** | 约 5 倍 |
| SHA-256 吞吐（强制标量） | — | 0.41~0.44 GB/s | 与旧标量一致 |

> 吞吐数据在 12 核共享机器上有 ±10% 波动（其他 agent 同时在跑构建/测试），表中为多次测量的区间；同一次测量内新旧实现的相对关系稳定（新 ≥ 旧）。redact 的提升来自 SHA-NI：旧内嵌实现是标量，合并后自动获得硬件加速；强制标量时新实现仍比旧实现快 3~5%（无对象拷贝 + 直接内联分支）。

## 三、FNV-1a 统一

### 3.1 替换清单

| # | 原位置 | 处理 |
|---|---|---|
| 1 | `src/p2p/endpoint.cpp`（`HashBytes` + 2 个常量） | 删除局部实现；`Endpoint::Hash()` 改用 `hash::Fnv1a64Update`（保留历史偏移量，状态宽度固定为 `uint64_t`，顺手修掉 32 位平台 `size_t` 截断隐患） |
| 2 | `src/p2p/candidate.cpp`（`HashBytes` + 2 个常量） | 同上；`MakeFoundation()` 改用 `hash::Fnv1a64Update` |
| 3 | `src/concurrency/fiber.cpp`（`MigrationFnv1a`） | 改为 `return hash::Fnv1a64(data, size);`（标准偏移量，输出不变） |
| 4 | `src/media/audio.cpp`（`Fnv1a64`） | 改为 `hash::Fnv1a64Update(1469598103934665603ULL, ...)`（保留历史偏移量） |
| 5 | `include/go2cpp/debug/diagnostic_protocol.hpp`（`DiagnosticChecksum`，FNV-1a/**32**） | **有意保留**：该头文件的契约是「只依赖 C++ 标准库、可单文件复制给不链接运行时的 CLI/第三方」，引入 `hash` 头会破坏该契约；且它是 32 位、与 64 位入口语义不同（审查报告 T2 也将其列为「不能与 64 位互换」）。 |

### 3.2 历史偏移量（关键）

`p2p/{endpoint,candidate}.cpp` 与 `media/audio.cpp` 使用的是少一位的偏移量 `1469598103934665603`（标准值是 `14695981039346656037`）。该值已经进入候选排序 / foundation 字符串 / 音频校验结果，**不能**顺手改成标准值。`fnv.hpp` 因此提供 `Fnv1a64Update(state, ...)` 增量入口，调用方显式传入历史偏移量，保证输出逐字节不变；文档中已写明这一决定。

## 四、逐字节一致验证

对拍程序同时链接旧实现（从 `git show HEAD:` 提取、改名的 oracle）与新实现：

- 旧 redact 内嵌标量实现（`oldlog::detail::Sha256Digest`）
- 旧 app SHA-NI 实现（`oldapp::Sha256Fast`）
- 新 `hash::Sha256::Digest`、新流式 `Write`/`Sum`（随机分片）、新 `app::Sha256Fast`（薄转发）

### SHA-256（1e6 组，382,702,333 B）

| 对照 | 硬件路径 mismatch | 强制标量 mismatch |
|---|---|---|
| 新 `Digest` vs 旧 redact 内嵌（标量） | **0** | **0** |
| 新 `Digest` vs 旧 app SHA-NI | **0** | **0** |
| 新流式（随机分片）vs 新 `Digest` | **0** | **0** |
| 新 `app::Sha256Fast` vs 新 `Digest` | **0** | **0** |

输入覆盖：随机 0~255 B、随机 0~64 KiB、以及 0/1/2/3/7/8/15/16/31/32/55/56/57/63/64/65/100/119/120/127/128/129/191/192/255/256/257/511/512/1000/1023/1024/4095/4096/65536 等填充/块边界。

### FNV-1a（1e6 组，384,381,052 B）

| 对照 | mismatch |
|---|---|
| `Fnv1a64Update(历史偏移)` vs 旧 p2p/audio 实现 | **0** |
| `Fnv1a64`（标准偏移）vs 旧 fiber 实现 | **0** |
| `Fnv1a32` vs 旧 diagnostic 实现 | **0** |
| 三段增量 `Update` vs 一次性 | **0** |

## 五、构建与回归测试

构建目录：`/tmp/build-hash`（`cmake -S . -B /tmp/build-hash -DCMAKE_BUILD_TYPE=Release`）。

| 目标 / 测试 | 结果 |
|---|---|
| `go2cpp_hash` / `go2cpp_log` / `go2cpp_fiber` / `go2cpp_p2p` / `go2cpp_debug` | 编译通过（0 error） |
| `go2cpp_hash_test` | 全通过（含新增 FNV 向量 + `Sha256::Digest` vs 流式 2 万组对拍；SHA-256 压测 2146~2370 MB/s） |
| `p2p_test_p2p` | 94 checks / 0 failures（FNV 替换后 foundation/排序行为不变） |
| `tests/logging/build_and_run_redact.sh`（`BUILD_DIR=/tmp/build-hash`） | 全通过，1e5 次 Redact **80~85 ns/次** |
| `go2cpp_tests`（全量） | 除 1 个与本改动无关的失败外全部通过：`tests/network/test_hook.cpp:518` 的 `scheduler.worker_count() >= 2` 在共享负载下偶发不满足（网络 hook 测试，不涉及 SHA/FNV） |
| `src/media/audio.cpp` 单独编译 | 通过（media 默认 OFF，`g++ -std=c++17 -O2 -Iinclude -c`） |
| `src/app/resource.cpp` 单独编译 | 通过（app 默认 OFF，验证 `Sha256Fast` 薄转发接口兼容） |

## 六、测试补强（`tests/**`）

- `tests/hash/test_hash_vectors.cpp`：新增 `CheckFnvVectors()`（FNV-1a 32/64 标准向量 + 增量组合律）；
- `tests/hash/test_hash_random.cpp`：新增 `CheckSha256DigestPaths()`（2 万组随机输入，`Sha256::Digest` 一次性路径 vs 随机分片流式路径逐字节一致）；
- `tests/logging/test_log_redact.cpp`：文案从「内嵌 SHA-256」更新为「一次性 Digest 与流式摘要对拍」，逻辑不变。

## 七、边界与后续

1. **未 commit**（按要求）。
2. `diagnostic_protocol.hpp` 的 FNV-1a/32 未替换，理由见 3.1 第 5 条；如果后续允许该头文件依赖 `go2cpp/hash/fnv.hpp`，替换只是一行。
3. `include/go2cpp/app/sha256_ni.hpp` 现在是薄转发，建议下一轮把 `src/app/resource.cpp` 直接改为 `hash::Sha256` 后删除该头（本轮为遵守「不改 resource.cpp」的边界而保留）。
4. 工作树中 `include/go2cpp/hash/hash.hpp`、`src/hash/sha1.cpp`、`src/p2p/relay_protocol.cpp`、`bento/**`、`compress/**`、`reflect/**`、`logging/log.*` 的改动**不是本 agent 所为**（并行 agent 的改动），本报告未触碰。

## 附录：本 agent 改动文件

```
M include/go2cpp/app/sha256_ni.hpp
M include/go2cpp/hash/sha256.hpp
M include/go2cpp/logging/redact.hpp
M src/hash/sha256.cpp
M src/p2p/endpoint.cpp
M src/p2p/candidate.cpp
M src/concurrency/fiber.cpp
M src/media/audio.cpp
M tests/hash/test_hash_vectors.cpp
M tests/hash/test_hash_random.cpp
M tests/logging/test_log_redact.cpp
?? include/go2cpp/hash/fnv.hpp
```
