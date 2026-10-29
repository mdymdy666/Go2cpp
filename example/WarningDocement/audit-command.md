# command 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/command.hpp`、`src/bento/net/command.cpp`、
  `tests/bento/test_command.cpp`
- 压测工具：`tests/bento/audit_command_stress.cpp`（不注册 CTest）
- 构建：`.build-bento-wsl`（Ninja，Release `-O3`，GCC 13.3.0，12 vCPU / 19 GiB，
  WSL2 Ubuntu 24.04）；基线为 HEAD `4b0c5fe` 的 `libgo2cpp_bento_app_base.a`
- 结论：**无 P0**。修复 5 个 P1（`timeout=0` 绕过上限、空 cwd 绕过沙箱、
  EOF 后 5ms 空转、2×64KiB 栈缓冲、wait 错误被吞）与 4 个 P2
  （硬上限/单路上限/status 往返、NUL 非规范帧），记录 5 个设计风险。
  `test_command` 由 **14 场景 / 93 checks → 16 场景 / 153 checks**；
  `ctest -R bento` 48 项 47 过 + 1 环境 SKIP（`bento_netem` 无 CAP_NET_ADMIN）。

## 1. 压测数据

### 1.1 帧编解码（20 万请求 + 10 万结果 + 200×1MiB，3 轮中位数）

| 项目 | base | opt | 说明 |
|---|---:|---:|---|
| 请求帧 encode（81B） | 59 ns/op（16.9 Mops/s） | 56 ns/op（17.8 Mops/s） | 无回归 |
| 请求帧 decode | 82 ns/op（12.2 Mops/s） | 84 ns/op（11.9 Mops/s） | +2 ns 来自新增 NUL 扫描，噪声内 |
| 结果帧 encode（640B） | 61 ns/op | 55 ns/op | 无回归 |
| 1MiB 结果帧 encode+decode | 950 MiB/s | 1006 MiB/s | 无回归 |

固定开销主要是 `vector` 分配与两次 `memcpy`；解码对每段长度做精确校验后一次性
构造，未发现可去掉的热点。机器与其他审查任务共享，单轮波动约 ±10%。

### 1.2 顺序执行（1000 次 `/bin/true`）

| 指标 | base | opt |
|---|---:|---:|
| 平均延迟 | 1097 µs | 1016 µs |
| P50 / P95 / P99 | 1057 / 1517 / 1710 µs | 978–1087 / 1378–1530 / 1572–1686 µs |
| 吞吐 | 885–917 次/s | 903–992 次/s |
| fd / RSS / 僵尸 | 7→7 / 5520→5520 KiB / 0→0 | 6–7→6–7 / 5448→5692 KiB / 0→0 |

`/bin/echo hello`（有输出，触发双管道）延迟从 base **6.22 ms** 降到 opt
**1.15 ms（5.4×）**；原因是修复了「两路 EOF 后仍 `poll(5ms)`」的空转。
顺序执行下限仍是 fork/exec 的 ~1.0 ms（裸 `fork+execve+/bin/true` 基线 0.95 ms，
命令模块额外开销约 7–10%）。

### 1.3 并发

- **1024 条 `/bin/true` 同时执行**（1024 线程 + 栅栏）：全部 `kOk`，墙钟
  8.5–8.9 s（115–120 次/s），峰值 fd **6918–7003**（`ulimit -n=10240`），
  执行后 fd 回落 6–7、RSS +8.2 MiB、僵尸 0。
- **对照 64 worker × 1024 条**：616–742 次/s；**裸 fork/exec** 同并发为
  1470 次/s（8 worker 2803 次/s、1024 线程风暴 447 次/s）。结论：瓶颈在
  fork/exec 与容器调度，不在编解码；1024 线程同时爆炸时，每命令 4 条管道 +
  `/proc` 记账会放大内核侧争用。调用方应把并发限制在 8–32 个 worker。
- **128 线程混合状态**（成功/超时/拒绝/启动失败各 32）：32/32/32/32，僵尸 0。

### 1.4 随机与 fuzz

- 3000 组随机 argv（含 NUL/超长/空项）/超时/cwd/白名单策略，与独立参考模型
  （按头文件规则重写，不复用被测函数）对拍：**0 不一致**。
- 5199 个随机变异帧：3233 个成功解码的变异帧**全部满足 `Encode(Decode(x))==x`**；
  其余按 magic/长度/保留位正确拒绝；无崩溃。
- 5000 个随机字节缓冲区（长度 0–511）双解码器 fuzz：无崩溃、无越界
  （ASan/UBSan 复跑）。
- 200 组 shell 元字符 argv（`$();|&` 引号/换行/反引号）经 `/bin/printf "%s\n"`
  原样返回，哨兵文件未创建：**0 注入**。

### 1.5 长稳与工具链

- 300 轮混合状态（echo/超时/拒绝/启动失败）：fd 6→6，RSS 8156→8156 KiB，
  僵尸 0，失败 0。
- ASan+UBSan（全量 ×0.02，随机 ×0.2 = 600 轮 / 1000 缓冲）：0 报告；
  TSan（`setarch $(uname -m) -R` 规避 ASLR）：随机与 64 线程池 0 报告；
  valgrind：**4048 allocs = 4048 frees，0 errors，0 leaks**，fd 14→14
  （14 中含环境继承的 `/dev/ptmx`，非模块泄漏）。
- `ctest -R bento`：48/48 执行，47 Passed + 1 Skipped（`bento_netem`），24.6 s。

## 2. 六个真实场景

| 场景 | 输入 | 期望 | 实际 |
|---|---|---|---|
| 1 白名单拒绝 | 默认策略；`allow_all` 下的 rm/curl/sh/dd/sudo/kill/nc；`denied_commands` 覆盖 allow_all | 全部 `kDenied`，`on_spawn` 不触发 | 符合（未启动任何被拒进程） |
| 2 超时杀进程组 | `sh` 忽略 TERM 死循环，timeout 300ms、grace 150ms；普通 `sleep 30` | 前者 300→450ms 升级 SIGKILL，message 含 SIGKILL；后者 SIGTERM；进程组消失、无僵尸 | 454–459ms 升级 SIGKILL；普通 sleep 走 SIGTERM；组已消失、僵尸 0 |
| 3 输出截断 | `cat` 6MiB，stdout 上限 64KiB；`sh` 同时写 6MiB 到 stdout+stderr，上限 32KiB/路 | 缓冲截断、`output_truncated`；回调仍收全量、不阻塞 | 64KiB/32KiB 正确截断；回调各收到 6291456 B；7–13ms 完成 |
| 4 cwd 沙箱 | root 内 root/sub；`..`、`/etc`、`root/escape -> /etc` 符号链接、空 cwd、root 内不存在目录 | 前两者放行；越界/空 cwd 拒绝；不存在目录 `kSpawnFailed` | 全部符合；`pwd` 输出与 canonical 路径逐字节相等 |
| 5 并发执行 | 1024 条短命令同时；128 线程四类混合 | 全成功、无 fd/僵尸泄漏 | 1024/1024 `kOk`；混合 32×4；fd 峰值 6918–7003，回落正常 |
| 6 命令注入 | 200 组随机 `$();|&` 引号/换行 argv | 原样传递，不执行第二命令 | 0 失败，哨兵文件不存在 |

## 3. 模块解耦

- `command.hpp` 只 include `<sys/types.h>` + `cstddef/cstdint/functional/string/
  string_view/vector`；**单独 include 该头的 TU** 在 `-Wall -Wextra -Wpedantic
  -Werror -fsyntax-only` 下通过（头文件自包含）。
- `command.cpp` 的 `-MM` 依赖只有 `command.hpp` + `go2cpp/os/proc.hpp`；
  `nm -u command.cpp.o` 的 `go2cpp::` 符号只有 `os::Process*`、`os::ProcError`、
  `os::SignalName`，**无 Link/PeerHub/Channel/transport/crypto**。
- 打包耦合（记录）：`command.cpp` 编入 `go2cpp_bento_app`，该目标 `PUBLIC` 链接
  `go2cpp_bento`（含 link/peer_hub）与 `go2cpp_os`。源码/符号层解耦成立，
  但库级依赖仍要求完整 Bento 构建；CMake 不在本次改动范围，未拆分
  `go2cpp_bento_command`。
- 执行器同步阻塞、无内部可变状态；`m_policy` 构造后只读，`Execute` 多线程并发
  安全（TSan 64-worker 池 0 报告；`on_*` 回调是调用方线程的局部状态）。

## 4. 问题清单

### P0（无）

### P1-1 `timeout_ms=0` 绕过 `max_timeout_ms`（已修复）

- 证据：HEAD 的 `ValidateCommand` 只检查 `timeout > max_timeout_ms`；`0` 被当作
  不限时直接放行。远端只要填 0，就能让执行线程永久阻塞，策略上限形同虚设。
- 修复：新增 `CommandPolicy::allow_unlimited_timeout{false}`；`timeout==0` 默认
  `kInvalidArgument`，需显式打开。新增正反断言（`test_command` 场景 2/3）。

### P1-2 启用 cwd 沙箱后空 cwd 继承服务进程目录（已修复）

- 证据：HEAD 只在 `!command.cwd.empty()` 时做根目录包含判断；`cwd=""` 时子进程
  继承执行器 cwd（可能是 `/` 或服务目录），沙箱边界被绕过。
- 修复：`allowed_cwd_roots` 非空且 `cwd` 为空 → `kDenied`；新增空 cwd、符号链接
  逃逸、root 内不存在目录三类断言。

### P1-3 两路管道 EOF 后固定空转 5ms（已修复，5.4×）

- 证据：HEAD 循环在 `TryWait` 未回收时无条件 `poll(fds, count, 5)`；两路 EOF 后
  `count==0`，变成纯 `sleep(5ms)`。`/bin/echo hello` 平均 **6.22 ms**，而裸
  fork/exec 仅 ~0.95 ms。
- 修复：两路 EOF 时改用 `Process::Wait(slice)`（内部 pidfd 等待子进程退出），
  slice 受剩余超时与 5ms 轮询粒度约束；超时阶梯期间仍每 5ms 检查 TERM/KILL。
  echo 平均降至 **1.15 ms（5.4×）**，`/bin/true` 也由 1097→1016 µs。

### P1-4 两个 64KiB 栈缓冲（已修复）

- 证据：HEAD `PumpOnce` 内 `char buffer[64*1024]`，stdout/stderr 两个泵各调一次；
  在 64KiB 栈的 Fiber 中调用 `Execute` 会直接顶栈（模块文档明确允许 Fiber 调用）。
- 修复：`Execute` 内单个 `std::array<char,16KiB>` 复用；每次 `PumpOnce` 最多
  16×16KiB=256KiB（> 64KiB 管道容量），大输出场景 6MiB 双流仍 7–13ms。

### P1-5 `TryWait` 错误被丢弃，可能死循环（已修复）

- 证据：HEAD `reaped = process.TryWait(&exit_status, nullptr)`；若 `wait4` 返回
  `ECHILD`（外部 SIGCHLD 回收器）等错误，`reaped` 永远 false，无超时命令会永久
  循环。修复：捕获 `ProcError`，非 `kOk/kTimeout` 立即跳出并返回 `kInternal`
  + 错误文本；`Process` 析构仍负责兜底 kill/reap。

### P2-1 请求帧解码未执行 64KiB 硬上限（已修复）

- 证据：字段各自合法的 17×3855B argv 可拼出 65619B 帧，HEAD 解码成功（编码器
  却拒绝），最大可到 263KB。修复：`DecodeCommand` 在解析前拒绝
  `size > kCommandMaxFrameBytes`；新增手工帧断言。

### P2-2 结果帧 status/单路输出往返不对称（已修复）

- 证据：HEAD `EncodeCommandResult` 接受 `status=99`、单路 8MiB 输出；解码器却
  拒绝越界 status/4MiB+，同一模块编出自己解不回的帧。修复：编码前拒绝
  `status > kInternal` 与单路 `> kCommandMaxOutputCeiling`；解码先按单路 4MiB
  硬上限拒绝。新增 4MiB+1 精确帧断言（解码不分配大字符串）。

### P2-3 解码器接受 NUL，产生不可重编码帧（已修复，fuzz 发现）

- 证据：随机变异 5199 帧中 25 帧解码成功但 `Encode` 返回空（payload 被改成
  NUL）；HEAD 解码器不查 id/cwd/argv 的 NUL，编码器查。修复：解码后对
  id/cwd/argv（请求）与 id/message（结果）做 NUL 拒绝；输出流仍允许任意字节。
  修复后 3233/3233 成功解码的变异帧全部可逐字节重编码。
- 说明：`ValidateCommand` 本就会拦 NUL，因此该问题不构成执行面漏洞，只破坏
  协议层「解码成功 ⇒ 可重编码」的不变式。

### P2-4 PATH 搜索 + basename 白名单（记录）

- `use_path_search` 默认 true，白名单按 argv[0] 原文或 basename 匹配；服务 PATH
  含可写目录时，植入同名程序可绕过 basename 白名单。头文件已注明高安全场景
  置 false 并使用绝对路径；本次不改默认值（会破坏现有调用与测试）。

### P2-5 cwd 校验与 spawn 之间的 TOCTOU（记录）

- `weakly_canonical` 在父进程解析符号链接，随后 `chdir` 在子进程执行；攻击者
  可在两者之间替换路径。彻底修复需 `openat2(RESOLVE_BENEATH)`/以目录 fd 传递，
  依赖 `os::proc` 扩展，超出本次范围。

### P2-6 正常退出的后台孙进程不被清理（记录）

- 只在超时路径杀进程组；`sh -c 'daemon &'` 直接子进程退出后，后台进程继续存活
  （管道 drain 8 轮后父侧关闭）。远程命令执行场景建议在调用方用 cgroup/会话
  兜底，或后续增加 `kill_group_on_exit` 策略；当前语义已在头文件说明。

### P2-7 子进程继承完整环境（记录）

- `ProcessOptions.env` 默认继承父环境，白名单程序可把环境中的密钥/令牌打印到
  stdout。建议高安全部署用 `env_clear` + 最小环境；`os::proc` 已支持，但
  command 模块没有对应策略字段（避免本次扩大 API）。

### P2-8 内置危险命令表不完整（记录）

- 表是安全下限，不含 `find/awk/sed/tar/cp/mv/ln/install/vim/less/gdb/strace` 等
  可间接执行命令的程序；头文件已声明「生产必须依赖白名单」。未扩容以免误伤
  现有 allow_all 部署。

### P2-9 打包耦合（记录）

- 见 §3：`go2cpp_bento_app` PUBLIC 依赖 `go2cpp_bento`；源码无 link/peer_hub
  依赖，但独立复用 command 需拆出专属 target（CMake 不在改动范围）。

## 5. 实现增强与瓶颈

- **已修**：EOF→`Wait`（-5ms/有输出命令）、16KiB 单缓冲（栈安全）、wait 错误
  上报、信号终止消息带 `SignalName`（诊断）、解码硬上限与 NUL 对称性。
- **保留瓶颈**：fork/exec 约 1.0 ms/命令，占顺序执行 90%+；1024 并发风暴下
  4 条管道 + `/proc` 记账 + 线程调度使吞吐降到 115–120/s（裸 fork 447/s）。
  命令模块自身无明显可省分配：请求帧 81B 编码 56ns，结果帧 640B 55ns。
- **建议调用方**：按机器核数限制 8–32 个执行 worker；不要把 `Execute` 放在
  小栈（<128KiB）Fiber；高安全场景 `use_path_search=false` + 绝对路径白名单，
  并考虑清空环境。

## 6. 测试复查

- 原 14 个场景 / 93 checks；新增/加强后 **16 个场景 / 153 checks**，0.88s。
- 断言有效性：
  - 旧测试对 `timeout=0`、空 cwd、>64KiB 解码、NUL 非规范帧、结果帧
    status/4MiB 上限、stderr 截断、符号链接逃逸 **均无覆盖**；新增断言逐项
    覆盖本次修复。
  - 删除了 `TestArgumentBounds` 的 `(void)allow_all;` 死代码（原变量构造后
    从未使用）。
  - `TestCwdSandboxExecution` 由「输出包含目录名」改为「与 canonical 路径
    逐字节相等」；`TestExecuteTimeout` 增加 SIGTERM 诊断与耗时下界；
    `TestOutputTruncation` 增加回调全量字节断言。
  - 新增 `TestPathSearchDisabled`、`TestFdAndZombieStability`（fd 不增长、
    无僵尸）。
- **SKIP 逃逸**：`test_command` 从不调用 `bento_test::Skip`，`Finish` 只会在
  `g_skips != 0` 时返回 77；本测试无 SKIP 路径。ctest 的 1 个 SKIP 是
  `bento_netem` 的环境权限判断，与 command 无关。
- 未覆盖（如实记录）：wait 错误路径无法在不改 `os::proc` 的前提下注入；
  TOCTOU 竞争、环境继承、后台孙进程、真实 Link 上的帧传输未做端到端测试。

## 7. 改动与验证

改动的文件（未 commit）：

- `include/go2cpp/bento/net/command.hpp`（策略字段 + 文档/生命周期约束）
- `src/bento/net/command.cpp`（5 个 P1 + 4 个 P2 修复）
- `tests/bento/test_command.cpp`（14/93 → 16/153）
- `tests/bento/audit_command_stress.cpp`（新增压测工具，不注册 CTest）
- `example/WarningDocement/audit-command.md`（本文）

复现命令：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl --target bento_test_command -j 8
./.build-bento-wsl/bento_test_command          # [PASS] 153 checks
./.build-bento-wsl/audit/audit_command_stress 1.0 --mode=all
ctest --test-dir .build-bento-wsl -R bento --output-on-failure
```

零警告：`go2cpp_bento_app` 与 `bento_test_command` 均以
`-Wall -Wextra -Wpedantic -Werror` 编译通过。
