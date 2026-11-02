# keystore 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/keystore.hpp`、`src/bento/net/keystore.cpp`、
  `tests/bento/test_keystore.cpp`
- 构建：`.build-bento-wsl`（Ninja，Release `-O2`，GCC 13.3，12 vCPU / 19 GiB，
  AMD Ryzen 9 8945HX）
- 结论：**无 P0**。修复 2 个 P1（析构后密钥字节残留、`warnings()` 无界增长）与
  4 个 P2（无 sink 时构造日志、解码多余分配、`Masked` 明文临时串、文件内容缓冲
  未清零）。测试 20 用例 / 2719 断言 → **28 用例 / 5672 断言**；`ctest -R bento`
  48 项 47 过 + 1 环境 SKIP（`bento_netem` 无 CAP_NET_ADMIN）；ASan/UBSan/TSan/
  valgrind 全过。

## 1. 压测数据

工具：`tests/bento/audit_keystore_stress.cpp`（独立程序，不注册 CTest）。
条件：Release `-O2`，单线程循环；base（HEAD 实现）与 opt（本次修复）交替各 3 轮，
取中位数；延迟样本 20 万次（含 `clock_gettime` 开销，偏保守）。

### 1.1 吞吐（3 轮中位数）

| 项目 | base ns/op | opt ns/op | 提升 | base M ops/s | opt M ops/s |
|---|---:|---:|---:|---:|---:|
| `LoadFromString` hex 32B | 328.02 | 127.38 | **2.58×** | 3.05 | 7.85 |
| `LoadFromString` base64 32B | 311.65 | 127.95 | **2.44×** | 3.21 | 7.82 |
| `ConstantTimeEquals` 32B | 14.95 | 12.13 | 1.23×* | 66.87 | 82.44 |
| `Masked` 32B | 53.10 | 14.65 | **3.62×** | 18.83 | 68.26 |
| `Accepts`（窗口内 A/B/C） | 122.54 | 121.78 | 1.01× | 8.16 | 8.21 |
| 混合 `Load/Accepts/Masked` | 385.10 | 154.26 | **2.50×** | 2.60 | 6.48 |

\* 比较代码本次未改，差异来自编译布局与机器负载，不影响结论。

- 规模：Load 两项各 100 万次、`Masked` 100 万、`Accepts` 100 万、混合 100 万、
  `ConstantTimeEquals` 2000 万次，均满足 ≥100 万要求。
- opt 延迟（单轮）：Load hex P50/P95/P99=144/144/154 ns；`Masked`=31/31/31 ns；
  `Accepts`=134/144/144 ns。

### 1.2 内存与分配

同规模：100 万×2 Load + 2000 万比较 + 100 万 `Masked` + 100 万 `Accepts` +
100 万混合 + 20 万轮长稳（Load/Rotate/Masked/析构）。

| 指标 | base | opt |
|---|---:|---:|
| 分配次数 | 41,332,075 | **3,903,030（-90.6%）** |
| 释放次数 | 41,332,075 | 3,903,030（净 +17 为进程级静态） |
| 累计分配 | 2492 MiB | 251 MiB |
| 长稳 20 万轮 RSS 增量 | +0 KiB | +0 KiB |
| valgrind（1000 轮 leaks 模式） | — | 3026 allocs = 3026 frees，0 errors / 0 leaks |

### 1.3 随机测试

5000 组（seed=20261029）：成功 1257 / 非法参数 3249 / 资源上限 494 / 其他 0。
与独立参考模型（独立 hex/base64 解码、长度与错误码规则）逐字节对拍；错误文本与
日志 0 次泄漏完整输入。

### 1.4 并发

8 线程共享只读 `Accepts/Current/Previous` 160 万次 + 8 线程各自 `LoadFromString`
40 万次，0 错误。TSan（WSL 上 `setarch $(uname -m) -R` 规避高熵 ASLR）0 数据竞争
报告。

## 2. 五个真实场景

| 场景 | 输入 | 期望 | 实际 |
|---|---|---|---|
| 1 加载 | String（`HEX:`/`base64:`/空白）、File（0600/0644/0400/0000/空/目录/超限）、Env（缺失/空/合法/5000 字符） | 成功加载；0644 告警 1 条；错误码 `kNotFound`/`kPermissionDenied`/`kInvalidArgument`/`kResourceExhausted` | 全部符合 |
| 2 脱敏 | 16/32B Key；捕获成功/轮换/失败日志 | 前 4 后 4、中间全 `*`、长度 32/64；日志含 masked、不含完整 hex/base64 | 全部符合 |
| 3 常量时间比较 | 相同/首字节/中间/末字节/长度不同/空指针；100 万次 | 结果正确；差异位置耗时一致（同一轮 same 14.68 vs other 14.69 ns/op；多轮 12–15 ns 波动，差异 <1%） | 全部符合 |
| 4 轮换 | Current/Previous/Accepts、窗口 0/30ms/5min、相同 key、无效 key | 窗口内接受新旧；窗口 0 立即失效；过期后 Previous 空；无效 Rotate 不改状态 | 全部符合（相同 key 语义已记录） |
| 5 并发读 | 8 线程共享只读；8 线程独立 Load | 无崩溃、结果无错误 | 0 错误（TSan 0 报告） |

## 3. 模块解耦

- `keystore.hpp` 只 include 标准库（`array/cstddef/cstdint/functional/string/
  string_view/vector`）与 `go2cpp/bento/error/error.hpp`；`error.hpp` 只依赖标准库。
- 单独 TU（只 include `keystore.hpp`）`-fsyntax-only`、链接、运行全部通过；
  `-MM` 依赖只有 `keystore.hpp` + `error.hpp`。
- `nm -u libgo2cpp_bento_keystore.a` 只有 libc/libstdc++/`error::` 符号；无
  crypto/ssl/aes/sha/x25519/link/peer_hub/transport。
- CMake 目标只链接 `go2cpp_bento_error` + `Threads`；`ApplyPreSharedKey` 模板解耦
  传输层，只有测试链接 `go2cpp_bento`。
- 线程模型：并发只读安全（TSan 验证）；Load/Rotate/SetLogSink 等写操作必须串行，
  文档已由「读取也必须串行化」修正为准确表述。

## 4. 问题清单

### P0（无）

### P1-1 密钥字节在对象析构后残留（已修复）

- 证据：HEAD 的 `Key` 析构是平凡的。`placement new` 构造 32B Key、`volatile` 读一次
  首字节后析构，在对象存储中仍能搜到完整 32B 密钥序列（探针输出
  `old_impl destructor_secret_found=1`；新实现 `0`）。
- 影响：core dump、堆/栈复用、换页都可能泄漏密钥，与模块「不泄漏明文」的设计
  承诺不一致。
- 修复：`Key::~Key()` 与两个赋值运算符先用 `explicit_bzero` 清零 32B；`FromBytes`
  未使用区保持零；`LoadFromFile` 内容缓冲、`ParseKeyText` 去空白副本/解码栈缓冲、
  `Masked` 临时串在离开作用域前清零。

### P1-2 `warnings()` 无界增长（已修复）

- 证据：HEAD 同一 0644 文件加载 100 次 → `warnings().size()==100`（探针输出
  `old_impl warnings_after_100_loads=100`）。按 30s 热加载折算，一天 2880 条、
  一年约 105 万条。
- 修复：`AddWarning` 按文本去重；同一告警只累计 1 条且重复不再刷日志。新增
  `warnings_deduplicated` 用例（100 次加载断言 1 条）。

### P2-1 未安装 LogSink 时仍构造日志字符串（已修复）

- 证据：HEAD 每次成功 Load 都构造 `"loaded ... masked=..."`（`Masked()` 内两次
  分配），即使没有 sink；压测总分配 41.3M 次。修复后仅在有 sink 时构造，
  分配降至 3.9M 次（-90.6%）。

### P2-2 解码路径多余分配（已修复）

- HEAD：两次 `StripWhitespace` + `std::vector` 解码。改为按需单次去空白、32B 栈
  缓冲、先校验格式再检查长度。Load hex 328→127 ns、base64 312→128 ns。

### P2-3 `Masked()` 生成完整明文临时串（已修复）

- HEAD：`ToHex()` 生成完整 hex 再替换；新实现直接写前后 4 字符。53.1→14.7 ns，
  且堆上不再出现完整明文临时串。

### P2-4 `m_sink_failures` 不可观测（记录）

- 只在 `Emit` 的 catch 中自增，无 getter，属诊断死状态。保留以避免 API 变化。

### P2-5 文件打开/符号链接与 TOCTOU（记录）

- `open` 跟随符号链接；`fstat` 后内容变化无法完全检测（长度变化会报 `kDataLoss`）。
  配置期加载、路径可信时风险低。

### P2-6 共享写非线程安全 / 其他语义（记录）

- `KeyStore` 明确为配置期对象：Load/Rotate 与并发读混用是数据竞争（文档已声明；
  并发只读经 TSan 验证安全）。
- `Rotate` 相同密钥刷新窗口且 `Previous()==Current()`（记录，不修）。
- `DecodeBase64` 接受非规范尾部填充位（与旧行为一致，兼容性保留）。
- `Key` 因自定义析构不再 trivially copyable；`KeyStore` 不可移动（配置期对象，无影响）。

## 5. 测试复查

- 原 20 用例 / 2719 断言；新增 8 用例（内存卫生、告警去重、并发只读、输入上限、
  权限拒绝、相同 key 轮换、base64 边界、随机规则）→ **28 用例 / 5672 断言**。
- 断言有效性：用 HEAD 实现跑扩充后的测试，恰好 **2/5672 失败**（`key_memory_hygiene`
  的析构残留、`warnings_deduplicated` 的 100 条），其余 5670 通过 → 新断言能捕获
  缺陷，且未破坏既有语义。
- `TailIsZero` 在 HEAD 也通过（赋值按整块 32B 复制，源尾部为零），属防御性断言，
  **不**作为回归证据；析构检查（volatile + 32B 序列搜索）才是有效回归断言。
- 原 `TestRandomInputs` 的 500 组随机 ASCII 成功概率≈0（只覆盖失败路径）；新增
  `random_inputs_rules`：400 组构造成功 + 400 组构造失败 + 200 组随机。
- SKIP：测试无 `Skip()`/77 退出；`file_permission_denied` 在 root 下静默 return
  （不计 SKIP），本环境 uid=1000 实际执行 EACCES 路径。
- 覆盖率（audit + `--coverage`，all/leaks 两次运行合并）：keystore.cpp 行 91.04%
  （402 行）、分支执行 90.86%；未覆盖主要为不可达/防御分支（Decode 的 empty、
  `failed to construct key`、`Masked` ≤8 字符、`OpenErrorCode` default、fstat/read
  失败、文件变化、自赋值、`FromBytes(nullptr)`）。
- 既有断言抽查：`TestBenchmarks` 的 20 万次比较累加有效；`TestRejectWrongLength`
  同时校验错误码与 `"not 16 or 32"`；`TestNoPlaintextInLogsOrErrors` 覆盖成功/失败/
  轮换日志；`TestPeerHubIntegration`/`TestLinkIntegration` 跑真实 UDP 回环。

## 6. 修改清单

| 文件 | 修改 |
|---|---|
| `include/go2cpp/bento/net/keystore.hpp` | `Key` 析构/赋值声明；线程模型改为「并发只读安全」；`warnings()` 去重语义；内存清零与 `Masked` 文档 |
| `src/bento/net/keystore.cpp` | Key 生命周期清零；`SecureClear`（`explicit_bzero`）；hex/base64 栈缓冲解码；单次按需去空白；无 sink 时跳过日志构造；`Masked` 直接写掩码；文件内容/解码缓冲清零；告警去重 |
| `tests/bento/test_keystore.cpp` | 新增 8 用例、+2953 断言（20/2719 → 28/5672） |
| `tests/bento/audit_keystore_stress.cpp` | 新增独立压测/随机/场景/并发/泄漏程序（不注册 CTest） |

未改动 `bento` 其他模块、base、构建脚本；未 commit。

## 7. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 无警告构建 + 全量 bento 测试
cd .build-bento-wsl && ninja && ctest -R bento -j4 --output-on-failure
cd ..

# 压测程序（独立编译，不依赖 CMake 目标；-Wno-mismatched-new-delete 仅用于
# 压测工具自带的分配计数 operator new，属工具伪影）
c++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
    -Wno-mismatched-new-delete -I include -I tests/bento \
    tests/bento/audit_keystore_stress.cpp src/bento/net/keystore.cpp \
    src/bento/error/error.cpp src/bento/error/registry.cpp \
    src/bento/error/mapping.cpp -o .build-bento-wsl/audit/audit_keystore_stress -pthread
./.build-bento-wsl/audit/audit_keystore_stress 1.0 --mode=all
./.build-bento-wsl/audit/audit_keystore_stress 1.0 --mode=bench
./.build-bento-wsl/audit/audit_keystore_stress 1.0 --mode=random --seed=12345

# ASan/UBSan（关闭工具内的分配计数，避免与 sanitizer 的 new 拦截冲突）
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DAUDIT_NO_ALLOC_COUNTER -I include -I tests/bento \
    tests/bento/audit_keystore_stress.cpp src/bento/net/keystore.cpp \
    src/bento/error/error.cpp src/bento/error/registry.cpp \
    src/bento/error/mapping.cpp -o .build-bento-wsl/audit/audit_keystore_asan -pthread
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./.build-bento-wsl/audit/audit_keystore_asan 0.02 --mode=all

# TSan（WSL 上需禁用 ASLR）
c++ -std=c++17 -O1 -g -fsanitize=thread -I include -I tests/bento \
    tests/bento/audit_keystore_stress.cpp src/bento/net/keystore.cpp \
    src/bento/error/error.cpp src/bento/error/registry.cpp \
    src/bento/error/mapping.cpp -o .build-bento-wsl/audit/audit_keystore_tsan -pthread
setarch $(uname -m) -R ./.build-bento-wsl/audit/audit_keystore_tsan 0.002 --mode=scenarios

# valgrind
c++ -std=c++17 -O1 -g -DAUDIT_NO_ALLOC_COUNTER -I include -I tests/bento \
    tests/bento/audit_keystore_stress.cpp src/bento/net/keystore.cpp \
    src/bento/error/error.cpp src/bento/error/registry.cpp \
    src/bento/error/mapping.cpp -o .build-bento-wsl/audit/audit_keystore_vg -pthread
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
    ./.build-bento-wsl/audit/audit_keystore_vg 0.01 --mode=leaks
```

## 8. 未覆盖 / 后续

- 共享写并发（文档声明非线程安全，未做压力测试，避免把 UB 当用例）。
- 符号链接/TOCTOU、`check_file_permissions=false`、真实时序侧信道未量化。
- P2-4/P2-5/P2-6 未改（记录）；如需 `sink_failures()` 观测接口另行提案。
