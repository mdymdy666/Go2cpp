# capability 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/app/capability.hpp`、
  `src/bento/app/capability.cpp`、`tests/bento/test_capability.cpp`
- 审计工具：`tests/bento/audit_capability_stress.cpp`（独立程序，不注册 CTest，
  与既有 `audit_*_stress.cpp` 一致）；共用夹具 `tests/bento/capability_fixture.hpp`
- 构建：`.build-bento-wsl`（Ninja，Release，`-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU，19 GB 内存，GCC 13.3，uid 1000（非 root）
- 结论：**无 P0**。修复 2 个 P1（文件读取边界、status 假可用）与 6 个 P2；
  探测吞吐约 4.7k ops/s，瓶颈是 `/proc/cpuinfo` 读取（109 µs，占 55%）与
  `getifaddrs`（39.8 µs），字符串解析仅 1.6 µs。`ctest -R bento` 48/48 通过
  （`bento_netem` 按设计因无 CAP_NET_ADMIN SKIP）；ASan+UBSan 与 valgrind 干净。

## 1. 压测数据

### 1.1 10 万次 Probe（15 项，Release，单线程）

| 指标 | 数值 |
|---|---|
| 总耗时 | 21,368.9 ms |
| 吞吐 | **4,680 probes/s** |
| 平均延迟 | 213.69 µs |
| P50 / P95 / P99 | 201.84 / 263.56 / 392.49 µs |
| RSS 前→后 | 3,712 → 3,968 KB（+256 KB） |
| 峰值 RSS | 3,968 KB |

- RSS +256 KB 来自压测程序自身 `reserve(100000)` 的 800 KB 采样数组与分配器
  复用，不是 `Probe` 增长；ASan/valgrind 与 10 万次重复探测均未发现泄漏。
- 对照：同一程序只做轻量夹具（文本约 1 KB）为 **83.3 µs / 12,009 ops/s**；
  关闭 epoll/接口/cgroup/rlimit 后为 **152.5 µs / 6,559 ops/s**。
- 机器为 WSL2：`/proc/cpuinfo` 由内核 seq_file 现场生成，读取成本远高于裸机。

### 1.2 修复前 / 修复后 A/B（各 5 轮，每轮 20,000 次，`taskset -c 3`）

| 指标 | 修复前 | 修复后 | 变化 |
|---|---|---|---|
| 平均延迟（5 轮均值） | 214.05 µs | 201.85 µs | **−5.7%** |
| P50（5 轮均值） | 202.23 µs | 190.61 µs | −5.7% |
| 吞吐（5 轮均值） | 4,673 ops/s | 4,955 ops/s | **+6.0%** |

单轮原始数据（修复前 → 修复后，µs）：210.87→203.22、218.28→199.14、
212.09→198.74、216.85→204.36、212.19→203.81。未 pin 的单次 10 万运行受
宿主噪声影响可偏离 ±6%，上表为交替运行的平均值。

### 1.3 瓶颈分解（5,000 次/项，单次运行，噪声约 ±5%）

| 项目 | ns/op | 说明 |
|---|---:|---|
| `Probe` 默认 | 199,933 | 基线 |
| `Probe` 关接口 | 158,688 | 省 `getifaddrs` |
| `Probe` 关 cgroup | 194,530 | 省版本/内存/CPU 探测 |
| `Probe` 关闭全部四类开关 | 147,906 | 仍读 /proc |
| 复制到 ext4 的同样文本 | 78,584 | 隔离 /proc 生成成本 |
| 复制文本 + 关开关 | 27,058 | 纯解析/文件成本 |
| 读 `/proc/cpuinfo` | **109,252** | 占默认探测 **54.6%** |
| `getifaddrs` + free | **39,753** | 占 **19.9%** |
| 读 `/proc/self/status` | 12,815 | |
| 读 `/proc/meminfo` | 10,043 | |
| `opendir+readdir` `/proc/self/fd` | 6,840 | |
| `stat cgroup.controllers` | 2,144 | 修复后每 Probe 只 1 次版本检测 |
| `epoll_create1+close` | 2,536 | |
| `statfs` / `statvfs` | 1,307 / 1,085 | |
| 解析 `/proc/cpuinfo` 文本 | **1,586** | 字符串解析不是瓶颈 |
| `uname` / `getrlimit` | 245 / 246 | |

结论：瓶颈是**内核/宿主 IO**，不是字符串解析。本轮去掉每行 `std::string`
分配与重复 cgroup 版本 `stat`（2 次/Probe），所以 A/B 只有约 6% 提升；
要再显著降低必须改变探测语义（不完整读 cpuinfo、改用 netlink 等），
本轮明确记录、不修改语义。

### 1.4 内存与泄漏

| 检查 | 结果 |
|---|---|
| valgrind（100 个随机变体，9,711 checks） | exit 0，无错误、无确定泄漏 |
| ASan+UBSan（ctest 全量 10,667 checks） | 0 失败、0 sanitizer 报告 |
| ASan+UBSan（1,200 随机变体，116,331 checks） | 0 失败、0 报告 |
| ASan+UBSan（7 场景，1,058 checks） | 0 失败、0 报告 |
| 10 万次 Probe RSS | 增量 256 KB（采样数组开销），无逐次增长 |

## 2. 随机测试

`audit_capability_stress --random-only --variants 1200`：每个变体在独立临时目录
搭出随机环境，再用**独立 oracle**（不调用实现内部函数）推导 15 项的期望状态：

- `/proc` 文件：存在/缺失/空/垃圾/目录冒充/权限 000，覆盖 meminfo、cpuinfo、
  self/status、self/fd；
- cgroup：无挂载、v2 正常/缺文件、v1 正常/缺文件、v1+v2 混合；
- 限制值：`max`、0、32 MB、256 MB、`9223372036854771712`、非法文本、空文件；
- 阈值与开关：`min_mem_available`、`min_nofile`、`min_nproc` 随机，四类
  `check_*` 随机开关；
- 不变量：15 项固定顺序、名称唯一、`Find/Count/IsDegraded/Reasons/Summary`
  一致、非可用必须有 reason、可用 detail 非空、状态三态划分。

| 版本 | 变体 | 断言 | 失败 |
|---|---:|---:|---:|
| 修复前 | 1,200 | 116,507 | **399** |
| 修复后 | 1,200 | 116,331 | **0** |

修复前失败集中在两类：可读但缺 `Uid/CapEff` 的 status 被假判 available；
v1 无限内存值被误报成 9.2 EB 具体限额（详见 P1-2、P2-1）。确定性版本的 96
个变体也已加入 CTest（见 §6）。

## 3. 复杂场景（7 个）

| # | 场景 | 输入/操作 | 期望 | 实际 |
|---|---|---|---|---|
| 1 | 正常环境 | 真实 `/proc`、`/sys/fs/cgroup` | 15 项，三态合法 | **12 available / 3 degraded**（cgroup.memory、cgroup.cpu、fs.tmpfs），0 unavailable |
| 2 | rlimit 受限 | 子进程 `RLIMIT_NOFILE=64`、`RLIMIT_AS=128 MiB`，`min_nofile=128`，再探测 2,000 次 | nofile degraded 含 `soft=64`/`min_nofile=128`；不崩溃 | 通过；子进程 exit 0 |
| 3 | `/proc` 不可读 | 4 个 proc 文件/目录 chmod 000 | 4 项 unavailable，其余可用 | 通过；epoll/根文件系统仍 available |
| 4 | cgroup 格式 | v2（`memory.max`/`cpu.max`）、v1（`limit_in_bytes`/`cfs_quota_us`）、混合 | v2 识别；v1 无限值显示 unlimited；混合时 v2 优先、缺失限制降级 | 全部通过 |
| 5 | 容器限制 | v2 32 MB + `cpu.max=0 100000`；`max`；v1 quota 25000 | 低内存/零配额 degraded，max/有限配额 available | 全部通过 |
| 6 | 畸形输入 | meminfo 为目录、8 MiB+ 文件、垃圾字段、status 缺 Uid/CapEff、`cpu.max` 缺 period | 分别 unavailable / degraded，不崩 | 全部通过 |
| 7 | 并发 | 8 线程 × 500 次 `Probe`，与单线程结果比对 | 无崩溃、无状态漂移 | 4,000/4,000 一致 |

场景 2 的 ASan 版本会跳过 `RLIMIT_AS`（ASan shadow 需要极大数据段虚拟地址，
128 MiB 会直接 mmap 失败），`RLIMIT_NOFILE` 分支仍执行；这是测试工具限制，
不是实现问题，已写入程序注释。

## 4. 解耦检查

| 检查 | 结果 |
|---|---|
| 头文件自包含（只 include 头文件单独编译） | `g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude -fsyntax-only` 通过 |
| 实现 include | 仅自身头文件 + C++ 标准库 + POSIX 头；无 `link/app` 其他模块 |
| 静态库未解析符号 | `nm -C libgo2cpp_bento_capability.a \| grep ' U ' \| grep go2cpp` 为空 |
| 测试二进制动态依赖 | 仅 `libstdc++`、`libgcc_s`、`libc`、`libm`、`ld-linux` |
| CMake 链接面 | `bento_test_capability` 只链 `go2cpp_bento_capability`；库目标无 `target_link_libraries` |

## 5. 问题清单

### P0

无。未发现崩溃、越界、UB、死锁、内存泄漏或并发状态漂移。

### P1-1（已修）`ReadFile` 无边界，且把目录当普通文件读成功

- 证据（修复前场景 6）：`meminfo` 是目录时仍返回 `degraded`（“缺少字段”），
  期望 `unavailable`；8 MiB+ 的 meminfo 会被整块读入内存再解析。libstdc++
  的 `ifstream` 能打开目录，读错误只落在输出流 `ostringstream` 的 failbit，
  原代码只检查 `input.bad()`，因此返回 true + 部分/空内容。
- 修复：`open(O_RDONLY|O_CLOEXEC)` + `fstat` 拒绝非普通文件；`read` 循环按
  8 KiB 追加，单文件上限 **8 MiB**，超限按不可用；fd 用 `UniqueFd` RAII 关闭。
- 验证：`scenario:malformed-dir`、`malformed-huge` 由 FAIL 转为 PASS。

### P1-2（已修）status 可读但内容不可用时被假判 available

- 证据（修复前随机 #82 等）：文件存在但只有 `Name/Threads`（无 `Uid`、
  无 `CapEff`）时，`proc.self.status` 为 `available`、detail 为空，并让
  `unix.privilege` 以 `cap_eff=0x0` 展示，掩盖了 proc 视图不完整。
- 修复：两者都缺失时 `degraded`（“status 缺少 Uid/CapEff 字段”）；
  `capability_known` 只在 CapEff 解析成功时置位，不再由“可读”推断。
- 验证：随机 399 个失败中的 status 类全部归零。

### P2-1（已修）cgroup v1 的“无限”值被误报成 9.2 EB 限额

- v1 无限值是 `PAGE_COUNTER_MAX * PAGE_SIZE = 9223372036854771712`，小于
  原阈值 `UINT64_MAX/2 = 9223372036854775807`，因此 detail 显示具体字节数。
- 改为 `>= 2^62` 判定，detail 输出 `unlimited`；状态原本就是 available，
  本次修的是对外可见的数值语义。v1/v2 场景已覆盖。

### P2-2（已修）`check_cgroup=false` 仍做版本检测，且子项原因不一致

- 原 `Probe()` 无条件 `DetectCgroupVersion`，`ProbeCgroupVersion` 内部又检测
  一次；每次 Probe 多 2 次 `stat`。关闭 cgroup 时 memory/cpu 报“cgroup 不可用”
  而不是“探测被配置禁用”。
- 修复：版本只检测一次且受开关控制；三个子项统一返回“禁用”原因。
- 验证：`TestDisabledChecks` 覆盖 cgroup.version/memory/cpu 三个原因。

### P2-3（已修）`cpu.max` 缺少 period 字段仍判 available

- 内核格式固定 `<quota> <period>`；原实现 `stream >> quota >> period` 后
  period 为空也继续，可能把截断/伪造文件当成可用配额。
- 修复：period 为空 → degraded（“cpu.max 缺少 period 字段”），并补测试。

### P2-4（已修）字符串逐行分配与重复 stat 的性能开销

- `LookupLineValue`/`ProbeCpuinfo` 原先每行构造一个 `std::string`；
  `/proc/cpuinfo` 在 64 核机器上可达数千行。现改为 `std::string_view`
  逐行扫描，只在命中键时构造值字符串。
- cgroup 版本检测去重（2 次 stat/Probe）。
- 实测：A/B 平均延迟 −5.7%，吞吐 +6.0%；但受 `/proc` IO 主导，收益上限有限。

### P2-5（已修）`readdir` 错误被忽略

- 原实现只判断 `opendir` 成功，循环结束后无论 `errno` 都报 available。
- 修复：循环前清零 `errno`，结束后非 0 记 unavailable 并带错误文本。

### P2-6（已修）`Reasons()` 条件表达式复制 reason

- `const std::string& reason = cond ? std::string(...) : item.reason;` 在
  condition 为 false 时仍会复制 `item.reason`；改为直接拼接 `entry`，避免
  每个非可用项一次多余分配。

### 记录（未修）

1. `unix.privilege` 恒为 available：`getuid/geteuid/getgid` 没有可注入的失败
   路径，该项本质是信息项；头文件“取不到身份（极罕见）”措辞可后续收敛。
2. `/proc/cpuinfo`（109 µs）与 `getifaddrs`（39.8 µs）是宿主成本；缩短
   cpuinfo 读取会削弱“proc 视图完整”语义，本轮不改。
3. 不可注入的失败分支：seccomp 拒绝 `epoll_create1`、`getifaddrs`/`uname`
   失败、`readdir` 中途错误——现有 `ProbeOptions` 没有对应注入点，测试无法
   覆盖，需后续评估是否加测试专用 seam。
4. cgroup 只探测 `cgroup_root` 根层限制文件；v2 若限制在子 cgroup（未做
   cgroup namespace），会报 degraded。属设计取舍，已在文档记录。
5. 权限场景在 uid 0 且具备 DAC override 时会打印本地 `[SKIP]`（不调用
   `bento_test::Skip`，不改变退出码）；CI 以 uid 1000 运行，实际执行。

## 6. 测试复查

| 维度 | 修复前 | 修复后 |
|---|---|---|
| 场景函数 | 6 | **14**（含 96 个确定性随机变体） |
| 断言 | 125 checks | **10,667 checks** |
| SKIP | `TestLowNofile` 一次 `setrlimit` 失败 → 整份测试 exit 77 | **无 `bento_test::Skip` 调用**；阈值通过 `min_nofile=soft+1` 注入，不再改进程限制 |
| cgroup v1/v2 | 仅“根目录缺失” | v2、v1 无限值/配额、混合优先级、缺 period、缺限制文件 |
| 容器限制 | 无 | 32 MB 内存、0 CPU 配额、max、v1 有限配额 |
| 权限拒绝 | 仅“路径不存在” | proc 文件/目录 chmod 000、cgroup 限制文件 chmod 000 |
| 畸形输入 | 无 | 目录冒充、8 MiB+、垃圾字段、status 缺字段 |
| 随机 | 无 | 96 个变体（CTest）+ 1,200 个变体（独立审计程序） |
| 断言有效性 | `Size()>=8` 与 `available>=8` 偏弱；部分 `Find!=nullptr` 不可失败 | 保留宿主无关下限，另加固定顺序/名称唯一/三态划分/Find 指针/Summary 精确串/可用 detail 非空；删除 `observed==Size` 恒真断言 |
| 覆盖缺口 | 无 cgroup 格式、权限、容器、畸形 | 仍存：epoll seccomp、getifaddrs/uname 失败、readdir 中途错误、64+ 核真实 cpuinfo、子 cgroup 限制 |

`TestRlimitThresholds` 现在同时验证“低于阈值 → degraded 且 reason 含阈值”
与“等于阈值 → available”；不修改 `RLIMIT_NOFILE/NPROC`，因此不存在
“测试失败就 SKIP 整份”的逃逸路径。

## 7. 验证命令与结果

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl -j4          # 0 警告（-Werror）
ctest --test-dir .build-bento-wsl -R bento -j4
# 48/48 通过；bento_netem 因无 CAP_NET_ADMIN 按设计 SKIP
ctest --test-dir .build-bento-wsl -R bento_capability --output-on-failure
# Passed 0.28s；bento_test_capability 输出 10,667 checks

# 独立审计程序
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -Itests/bento \
    tests/bento/audit_capability_stress.cpp src/bento/app/capability.cpp \
    -o /tmp/audit_capability_stress
/tmp/audit_capability_stress --scenario-only     # 1,058 checks, 0 failures
/tmp/audit_capability_stress --random-only --variants 1200  # 116,331 checks, 0 failures
/tmp/audit_capability_stress --bench-only --iters 100000    # §1.1 数据
/tmp/audit_capability_stress --profile-only      # §1.3 数据
```

- ASan+UBSan：上述 CTest 与两类审计模式全部 0 报告（场景 2 的 RLIMIT_AS
  子项按注释跳过）。
- valgrind：`--random-only --variants 100`，9,711 checks，exit 0。
- 说明：审查期间同工作树有其他 agent 同时修改 session 文件；一次 `-j4` 全量
  运行出现 `bento_session` 瞬时失败，单跑与随后两次全量重跑均通过（48/48）。
  该用例与 capability 改动无交集，记录在此以免隐瞒。

## 8. 改动文件

- `include/go2cpp/bento/app/capability.hpp`：文档补充 8 MiB 读取上限与
  “可读但缺字段记 degraded”的状态语义。
- `src/bento/app/capability.cpp`：P1/P2 修复 + `string_view` 扫描 + 单次
  cgroup 版本检测。
- `tests/bento/test_capability.cpp`：6 场景/125 checks → 14 场景/10,667 checks。
- 新增 `tests/bento/capability_fixture.hpp`（共用夹具）。
- 新增 `tests/bento/audit_capability_stress.cpp`（压测/随机/场景/分解计时，
  不注册 CTest）。

未修改 base、CMake、其他模块；未 commit。
