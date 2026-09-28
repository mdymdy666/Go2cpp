# Verification Matrix

This document records commands executed against the canonical WSL checkout:

```text
/UserData/CodexWorkSpace/Go2Cpp
```

The project is C++17. Builds use `-Wall -Wextra -Wpedantic`; every CTest
target has a registered timeout and the stress loops use an external watchdog.
Test-level joins and wait loops are also audited for bounded failure paths; the
reference Go source is research input only and is not linked.
## Environment

| Item | Observed value |
|---|---|
| OS | Ubuntu 24.04 under WSL2/Linux |
| GCC | 13.3.0 |
| Clang | 18.1.3 |
| CMake | 3.28.3 |
| Ninja | available |
| Boost.Context | 1.83.0 (minimum 1.70) |
| Valgrind | 3.22.0 |
| Go executable | not required for the C++ build |

## Build and tests

GCC Debug shared build, Linux hook, all examples and all registered tests:

```sh
cd /UserData/CodexWorkSpace/Go2Cpp
cmake -S . -B build-check -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-check --parallel 4
ctest --test-dir build-check --output-on-failure --timeout 60
```

Historical baseline on 2026-09-18: **7/7 passed** (the result below includes
the later mixed-runtime example and is the current result). The unit executable
reported all ten module groups passed.

The Release static configuration intentionally disables the interposer:

```sh
cmake -S . -B build-static-current -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-static-current --parallel 4
ctest --test-dir build-static-current --output-on-failure --timeout 60
```

Historical result: **6/6 passed**. A strict GCC build with
`-DCMAKE_CXX_FLAGS=-Werror` (`build-werror-current`) also built and passed
**7/7**.

Clang 18 was checked independently:

```sh
CC=clang CXX=clang++ cmake -S . -B build-clang-current -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DGO2CPP_BUILD_TESTS=ON \
  -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-clang-current --parallel 4
ctest --test-dir build-clang-current --output-on-failure --timeout 60
```

Historical result: **7/7 passed**, warning-clean under the configured warning flags.

## Focused boundary checks

The test executable accepts one exact module name in `GO2CPP_TEST_FILTER`.
The following current-source runs passed under a 45-second watchdog:

```sh
for name in hook sync fiber dynamic scheduler context channel error; do
  GO2CPP_TEST_FILTER="$name" timeout 45s ./build-check/go2cpp_tests
done
```

These cover default-enabled socket hooks and close/dup/fd reuse,
scheduler-aware mutex/condition-variable/waitgroup, stack migration and
natural Fiber destruction, dynamic M growth/shrink/affinity/stealing, the G
state machine and cancellation queue cleanup, context trees, channels and
select, ordinary C++ exception boundaries, and error chains. The hook regression also checks
native-thread poll fallback timeout reporting, `dup2` replacement-before-close
notification, `dup2(fd, fd)` no-op identity, tracked `FIONBIO` `EFAULT`,
managed `MSG_OOB` rejection. The scheduler regression includes raw Fiber
parking coverage; it uses `Fiber::Suspend(Park)` cancellation and bounded waits
for the externally observable state transitions. The test support join helper
aborts on a stuck thread instead of allowing a failed assertion to continue
into an unbounded join.

## Sanitizers

ASan and UBSan were rebuilt from the current tree after the final scheduler,
hook, and smoke-test changes. Both the full unit executable and scheduler
smoke passed with no diagnostics:

```sh
cmake --build build-asan-current --parallel 4
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
  ./build-asan-current/go2cpp_tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
  ./build-asan-current/go2cpp_scheduler_smoke

cmake --build build-ubsan-current --parallel 4
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-ubsan-current/go2cpp_tests
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-ubsan-current/go2cpp_scheduler_smoke
```

TSan was configured with `GO2CPP_ENABLE_TSAN=ON`, `-fno-pie -no-pie`, and
`setarch x86_64 -R` to avoid the WSL loader's unexpected-mapping startup
failure. Earlier current-source filtered runs (`context`, `channel`,
`scheduler`, `dynamic`, `fiber`, `sync`, `io`) completed without a diagnostic;
the final rebuilt run did not complete under the available WSL watchdog, and
the full suite and `hook` filter are not claimed as clean TSan results. A
native Linux runner remains the required independent TSan gate.

## Valgrind Memcheck

The required checks were run separately so leaks in one executable cannot hide
in another:

```sh
timeout 300s valgrind --tool=memcheck --leak-check=full \
  --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect,possible --error-exitcode=99 \
  ./build-check/go2cpp_tests > build-check/valgrind-tests-final5.log 2>&1
timeout 300s valgrind --tool=memcheck --leak-check=full \
  --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect,possible --error-exitcode=99 \
  ./build-check/go2cpp_scheduler_smoke > build-check/valgrind-smoke-final5.log 2>&1
```

Historical final5 result: both exited zero with no definite, indirect, or possible leaks. The current direct rerun and its intentional still-reachable FiberLocal registry are recorded in the final boundary section below.
The deliberate invalid-pointer `FIONBIO` test is skipped when
`RUNNING_ON_VALGRIND` is set; the same `EFAULT` assertion runs in normal,
ASan, and UBSan builds so Memcheck reports only runtime faults.

## Stress and watchdog

The following inline Python loop was used (no temporary script file):

```sh
python3 -c '
import subprocess
for name, count in (("go2cpp_tests", 40), ("go2cpp_scheduler_smoke", 80)):
    for _ in range(count):
        subprocess.run(["timeout", "45s", f"./build-check/{name}"], check=True)
    print(name, count, "runs passed")
'
```

All 40 unit runs and 80 scheduler-smoke runs passed in the current audit after
the final scheduler and hook changes. Earlier 200/300-run watchdog campaigns
are retained in `codex.md`.

## Install/export smoke

The modular install was regenerated with:

```sh
cmake --install build-check --prefix build-install-current
```

The install contains the module/umbrella libraries, public headers and
`lib/cmake/go2cpp_runtime` package files. The static Hook-off and shared
Hook-on configurations are separate by design. A consumer must link the
installed `go2cpp::runtime` target (or individual module targets) and use a
shared build when transparent hook symbols are required.

An independent temporary consumer was also configured with
`find_package(go2cpp_runtime CONFIG REQUIRED)`, linked to
`go2cpp::runtime`, built with Ninja, and executed with the installed library
directory on `LD_LIBRARY_PATH`; it exited zero (`installed package consumer
passed`). The temporary source/build directory under
`build-check/pkg-consumer-*` was removed after the run.

## Coverage and known gaps

The tests cover P=1 and multi-P scheduling, local/global queues, stealing,
dynamic worker waves, cancellation/shutdown, context trees/deadlines/values,
ordinary C++ exception boundaries, error chains, channels/select/close, Fiber
migration and destruction, coroutine synchronization, epoll readiness and
timer/close races, and transparent socket wrappers.

The suite does not establish Go compiler/ABI/GC equivalence, asynchronous
preemption, segmented stack growth, exact Go fairness, an exact
blocking-region M/P handoff (bounded replacement-M admission is covered
separately), every Linux socket-adjacent API, or a native-Linux TSan result.
A non-cooperative C++ callable can still keep shutdown waiting because safely
discarding a live C++ stack would skip RAII destructors. Channel destruction
must not race raw member calls; throwing element moves/custom select callbacks
have no rollback contract after a wait node is claimed. Managed
`MSG_WAITALL`/`MSG_OOB` are explicit `ENOTSUP` boundaries, while ordinary
threads using a previously adopted runtime-nonblocking socket may not receive
full libc `MSG_WAITALL` blocking semantics. `IOManager*`/hook bindings also
require the caller to honor the documented owner lifetime.


The post-boundary Debug rebuild in build-docs-check compiled all 57 targets and
registered eight tests after adding mixed_runtime_demo. CTest completed **8/8**
within the 60 second per-test timeout. The mixed example was then run 20 times
with a 15 second process watchdog; all 20 runs reported native_reacquired=1,
external_policy=1, peak_workers=2 and all_done=1.

The Hook-off Release static rebuild in build-docs-static compiled 43 targets and CTest completed 7/7, including mixed_runtime_demo.

## Newly audited boundary paths

The current boundary audit adds a mixed-mode synchronization regression and a
runnable go2cpp_mixed_runtime_demo. The regression covers native ownership
followed by Fiber acquisition, Fiber ownership followed by native acquisition,
native notification of a managed condition waiter, managed notification of a
native condition waiter, and both directions of WaitGroup completion. Fiber-local
value isolation/lifetime and per-thread hook/participation restoration are
checked separately.

BlockingRegion is intentionally explicit. The scheduler may admit a replacement
M for queued work while the declaring M is in a native blocking call, but it
does not detect arbitrary blocking calls or release the P token with the exact
Go runtime handoff. ScopedThreadParticipation likewise records policy only; it
does not attach an external thread or run a scheduler queue. Known interposed
socket and sleep fallbacks can mark M as blocking automatically, but this
coverage does not extend to arbitrary unhooked native calls.

## Final boundary rerun (2026-09-20, superseded)

The final source rerun used the canonical WSL checkout after the mixed-mode and hook-boundary fixes:

- GCC Debug shared Hook-on: `cmake --build build-check --parallel 4`; CTest **8/8** passed, including the mixed-runtime example and the plain-Scheduler lazy-adoption timeout regression.
- GCC Release static Hook-off (`build-release-current`): CTest **7/7** passed. Strict GCC `-Werror` (`build-werror-current`) and Clang 18 (`build-clang-current2`) each passed **8/8**.
- ASan (`build-asan-current`) full unit plus scheduler smoke passed with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`. UBSan (`build-ubsan-current`) full unit plus scheduler smoke passed with `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- The current direct Memcheck logs are `build-check/valgrind-boundary-tests-final4.log` and `build-check/valgrind-boundary-smoke-final4.log`. The unit run used `1,668,998` allocations and `1,668,995` frees, with `360 bytes in 3 blocks still reachable`; smoke used `3,396` allocations and `3,395` frees, with `96 bytes in 1 block still reachable`. Both had `0` definite, indirect, and possible bytes and `ERROR SUMMARY: 0`. The still-reachable blocks are intentional process-lifetime FiberLocal registry and context-timer service state.
- Final watchdog stress passed `go2cpp_tests` **40/40** and `go2cpp_scheduler_smoke` **80/80**, each with a 45-second process timeout. The native context waiter now uses its own 2-second `WaitFor` watchdog before joining.
- TSan was rebuilt with `GO2CPP_ENABLE_TSAN=ON` and `-fno-pie`; scheduler, dynamic, and fiber filters passed under `setarch x86_64 -R`. The full current suite exceeded a 120-second Python watchdog without a diagnostic in this WSL image; the normal invocation also hits ThreadSanitizer `unexpected memory mapping` startup failure. No full-suite TSan pass is claimed; a native Linux runner remains the race-detector gate.
- The install/export smoke was rerun with `cmake --install build-check --prefix build-install-final` and an isolated `find_package(go2cpp_runtime)` consumer; the consumer compiled, linked and printed `7`.


## Final boundary rerun after race/watchdog hardening (2026-09-20)

This is the latest source state. It includes the Valgrind-only pause used by
test Fiber loops, the strict single-worker cancelled-queue regression, and the
scheduler machine-slot container change from std::vector to std::deque. The
latter was made after TSan reported a machine-slot relocation race while a
managed sleep entered BlockingRegion.

- GCC Debug Hook-on (build-check): CTest **8/8**, 3.71 seconds.
- GCC Release static Hook-off (build-release-current): CTest **7/7**, 2.75 seconds.
- GCC strict -Werror (build-werror-current): CTest **8/8**, 3.71 seconds.
- Clang 18 (build-clang-current2): CTest **8/8**, 3.72 seconds.
- ASan (build-asan-current): CTest **8/8** with
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1.
- UBSan (build-ubsan-current): unit and scheduler smoke **2/2** with
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1.
- Valgrind unit log build-check/valgrind-boundary-tests-final8.log:
  1,516,975 allocations, 1,516,972 frees, 360 bytes in 3 blocks still
  reachable; definite, indirect and possible lost bytes were all zero and
  ERROR SUMMARY: 0.
- Valgrind scheduler-smoke log
  build-check/valgrind-boundary-smoke-final8.log: 3,406 allocations,
  3,405 frees, 96 bytes in 1 block still reachable; all three lost
  categories were zero and ERROR SUMMARY: 0. The reachable blocks are the
  intentional FiberLocal registry and process-lifetime Context TimerService.
- TSan with setarch x86_64 -R: scheduler **10/10**, plus context, channel,
  sync, io, dynamic and fiber filters passed without a diagnostic. The normal
  WSL invocation fails before tests with unexpected memory mapping. The full
  Hook-on suite exceeded the 120-second WSL watchdog after producing all test
  announcements; no full-suite TSan pass is claimed. Native Linux remains the
  release-gate race run.
- A 10-run unit plus 10-run scheduler-smoke watchdog stress loop passed after
  the final source rebuild. The installed package consumer rebuilt through
  find_package(go2cpp_runtime CONFIG REQUIRED), linked go2cpp::runtime, and
  printed 7.

## Shutdown lock hardening (2026-09-20)

The final scheduler-only change avoids an allocation during external shutdown:
with `join_mutex` held, shutdown moves each published `std::thread` out under
`Impl::mutex`, releases that mutex, and joins the moved thread. The final
processor/state scan is lock-protected. This preserves the worker-epilogue lock
ordering and avoids leaving joinable workers behind if an OOM occurs while
building a temporary snapshot.

After this change, `build-check` rebuilt and CTest passed **8/8**; the dynamic
and synchronization filters and scheduler smoke passed under 45-second
watchdogs. Memcheck was rerun as `valgrind-boundary-tests-final9.log` and
`valgrind-boundary-smoke-final9.log`: unit `1,517,159` allocs / `1,517,156`
frees with 360 bytes intentionally still reachable, smoke `3,415` / `3,414`
with 96 bytes intentionally still reachable; definite, indirect and possible
lost bytes were all zero and both logs reported `ERROR SUMMARY: 0`.

The reaper was then hardened in the same source pass: its retired-thread
vector is reserved before any thread move, and `join_mutex` remains held until
all retired workers have joined. The current Release/Clang/Werror/ASan/UBSan
and TSan-filter reruns passed; final10 Memcheck reported the same zero lost
categories (unit 1,560,975/1,560,972 allocs/frees, smoke 4,042/4,041) with
only the documented 360B and 96B still reachable.

## 2026-09-20 追加验证

以下命令均在 `/UserData/CodexWorkSpace/Go2Cpp` 执行：

```text
cmake -S . -B build-current2 -DCMAKE_BUILD_TYPE=Debug -DGO2CPP_BUILD_HOOK=ON -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-current2 -j2
ctest --test-dir build-current2 --output-on-failure --timeout 60        # 9/9
cmake -S . -B build-release2 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
ctest --test-dir build-release2 --output-on-failure --timeout 60        # 8/8
cmake -S . -B build-werror2 -DCMAKE_BUILD_TYPE=Debug -DGO2CPP_BUILD_HOOK=ON -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON -DCMAKE_CXX_FLAGS='-Werror' # 构建通过
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan2/go2cpp_tests     # 全组通过
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 build-ubsan2/go2cpp_tests # 全组通过
valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible build-current2/go2cpp_tests # 0/0/0，0 errors
```

`EventBatch` 总时长回归（空 channel、20ms）通过；Release CTest 首次有一次 mixed-runtime watchdog 偶发超时，独立 5 轮和随后完整重跑均通过。
EventBatch `stop()` 唤醒 Fiber 的 P=1 回归通过。
新增 `timer`、`beginner` 过滤测试各重复 10 轮通过；Clang 18 对新手测试执行
`-fsyntax-only -Wall -Wextra -Wpedantic` 通过。TSan 仍沿用此前 WSL 镜像的启动限制记录，
没有把未启动的完整套件宣称为通过。Valgrind 全套本轮仍有 416B intentional
`still reachable` 进程级状态，`definite/indirect/possible` 均为 0。


## sysmon 长系统调用专项验证（2026-09-23）

本次补充了受限 Go 风格 sysmon 的回归测试。测试只把显式
`BlockingRegion` 和 Hook 能识别的 native fallback 当作可观测阻塞边界：

- `cmake --build build-concurrency-audit -j2`
- `GO2CPP_TEST_FILTER=dynamic timeout 45s ./build-concurrency-audit/go2cpp_tests`，重复 10 次通过；覆盖 P=4、单个长 syscall、逻辑 detach、替代 M 保留、原 G 唤醒和 shutdown。
- `ctest --test-dir build-concurrency-audit --output-on-failure`：2/2 通过。
- Hook-on 构建 `build-sysmon-hook`：`ctest --test-dir build-sysmon-hook --output-on-failure`，9/9 通过；新增 IOManager + 未跟踪 pipe FD fallback 测试，确认 sysmon 能看到可能阻塞的 libc 调用。
- ASan `build-sysmon-asan`：2/2 通过；UBSan `build-sysmon-ubsan`：2/2 通过。
- Valgrind dynamic：0 definite/indirect/possible lost，ERROR SUMMARY 为 0；仅保留已有进程生命周期 reachable 块。

该实现不能安全地从另一个线程强制打断任意 C++ 系统调用或迁移其栈；raw
`syscall`、未 Hook 的第三方阻塞库和 `max_workers` 已耗尽时属于明确边界。此前
WSL + Boost.Context 偶发的 `unexpected memory mapping` 已在本轮绕过地址随机化
后复测；当前全量 TSan 连续 3 次通过，历史限制仍作为环境风险保留。


## Fiber 父链与 sysmon 多 M 交接复核（2026-09-23）

- `GO2CPP_TEST_FILTER=fiber timeout 60s ./build-concurrency-audit/go2cpp_tests`：通过；新增父对象结束后的 `alive=false` 墓碑快照，以及嵌套 Fiber 唤醒后的父链顺序和失败隔离回归。
- `GO2CPP_TEST_FILTER=dynamic timeout 90s ./build-concurrency-audit/go2cpp_tests`：重复 10 次通过；新增两个并发长阻塞 G 的 detached 计数、替代 M 保留和共同唤醒验证。
- `ctest --test-dir build-concurrency-audit --output-on-failure --timeout 60`：2/2；Hook-on 9/9；Release 8/8；Werror 2/2；Clang 2/2；ASan 2/2；UBSan 2/2。
- TSan 全量命令连续 3/3 通过：`setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 timeout 90s ./build-tsan-concurrency/go2cpp_tests`。
- Valgrind 全量 Hook-on：`in use at exit` 416 bytes/4 blocks，definite/indirect/possible lost 均为 0，ERROR SUMMARY 0；dynamic：96 bytes/1 block 为进程级可达状态，三类 lost 均为 0，ERROR SUMMARY 0。
- 运行时新增约束：`BlockingRegion` 跨 Fiber yield/park 或 G 任务边界会 fail-fast，不再静默留下错误的 blocking_workers/P 记账。

## 2026-09-23 monitor、嵌套 IO 与错误边界复核

以下命令均在 /UserData/CodexWorkSpace/Go2Cpp 执行，测试进程均带有超时 watchdog：

- cmake --build build-sysmon-hook -j2：通过；新增 sysmon 独立等待锁和三层嵌套 IO/写错误回归后重新编译。
- ctest --test-dir build-sysmon-hook --output-on-failure --timeout 90：9/9 通过；包含 unit、smoke、Hook 示例和所有运行时示例。
- ctest --test-dir build-sysmon-release --output-on-failure --timeout 90：8/8 通过；Release 为静态、Hook-off 配置。
- ctest --test-dir build-sysmon-werror --output-on-failure --timeout 90：2/2 通过，GCC -Werror。
- ctest --test-dir build-sysmon-clang --output-on-failure --timeout 90：2/2 通过，Clang 18。
- ASan：env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ctest --test-dir build-sysmon-asan --output-on-failure --timeout 120，2/2 通过。
- UBSan：env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-sysmon-ubsan --output-on-failure --timeout 120，2/2 通过。
- timeout 45s env GO2CPP_TEST_FILTER=hook ./build-sysmon-hook/go2cpp_tests：通过；包含两层和三层 Fiber 链、第一次 ETIMEDOUT、第二次 readiness 唤醒、父链返回顺序、send(MSG_NOSIGNAL) 的 EPIPE/ECONNRESET/EBADF 错误保持，以及已有 Hook/FD 竞态。Hook 过滤器随后连续 10 次通过。
- timeout 30s env GO2CPP_TEST_FILTER=fiber ./build-sysmon-hook/go2cpp_tests、GO2CPP_TEST_FILTER=dynamic：各自通过；Fiber 过滤器连续 10 次、dynamic 过滤器连续 10 次通过。dynamic 测试覆盖 sysmon_running、心跳计数、高负载任务、长阻塞 detach/替代 M、唤醒和 shutdown。
- TSan 当前 build-tsan-concurrency 在 setarch x86_64 -R 下完整套件单次通过；sync 过滤器 5/5、fiber 和 dynamic 过滤器各 3/3 通过。重复完整套件时，WSL 主机曾在 test_sync.cpp 的 native waiter/CV watchdog 处超时，无 ThreadSanitizer race 报告；gdb 下同一 sync 套件正常完成。该环境抖动没有被宣称为“全量重复稳定”，native Linux 仍是 TSan 发布门槛。
- Valgrind Memcheck 全 Hook-on：build-sysmon-hook/valgrind-nested-final.log，1,577,888 allocs / 1,577,884 frees，416 bytes/4 blocks still reachable，definite/indirect/possible lost 均为 0，ERROR SUMMARY 0。scheduler smoke：valgrind-nested-smoke-final.log，96 bytes/1 block reachable，三类 lost 均为 0，ERROR SUMMARY 0。可达块属于已有 FiberLocal/TimerService 进程生命周期状态。

当前补充验证只证明已声明/Hook 的阻塞边界；monitor 不能异步终止任意 C++ 系统调用，不能迁移其栈，也没有自动 Fiber 栈增长。`WaitAny/WaitMany` 已覆盖同一 Fiber 的多 FD epoll 等待，但不替换 Hook 的 libc poll/select ABI。这些是设计限制而不是未观察到的测试通过。

## 2026-09-23 最终复测修订

- 当前工作树源码重编译后，Hook/Fiber/dynamic 过滤器各连续 5/5 通过；Hook 全套 CTest 9/9、Release 8/8、Werror 2/2、Clang 2/2、ASan 2/2、UBSan 2/2 通过。
- 当前 TSan 构建使用 setarch x86_64 -R，完整 go2cpp_tests 单次通过且没有 ThreadSanitizer 报告；WSL 下重复全套曾出现不同 native waiter watchdog 超时，因此不能把 WSL 重复运行描述为稳定发布门槛。
- 当前 Memcheck 日志为 build-sysmon-hook/valgrind-final-current.log：416 bytes/4 blocks still reachable，definite/indirect/possible lost 均为 0，ERROR SUMMARY 为 0。

## 2026-09-23 本次交付最终工作树复测

以下结果对应本次提交前的最终源码（包含 panic_defer 删除、Channel<T> 不抛 move 约束、手动 Fiber 条件变量修复和 Fiber 快照 active 修复）：

- Debug Hook-on：`cmake --build build-postpanic --parallel 4`；`ctest --test-dir build-postpanic --output-on-failure --timeout 120`，9/9 通过；sync、fiber、channel 过滤测试分别通过。
- Release Hook-on：`cmake --build build-final-release --parallel 4`；CTest 9/9 通过。
- ASan+UBSan Debug：`build-final-asan`，使用 `-fsanitize=address,undefined -fno-omit-frame-pointer`，CTest 9/9 通过；未报告 sanitizer 错误。
- Valgrind Memcheck：`LD_LIBRARY_PATH=build-final-release valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect,possible --error-exitcode=99 --log-file=build-final-release/valgrind-final.log build-final-release/go2cpp_tests`，ERROR SUMMARY 0；definite/indirect/possible 均为 0；416 bytes/4 blocks 仍可达，属于 FiberLocal/TimerService 进程级状态。
- TSan：普通启动仍在当前 WSL 失败于 `unexpected memory mapping`；`setarch x86_64 -R` 启动可运行 scheduler/fiber 过滤，但最终 sync 过滤在 `tests/test_sync.cpp:158` 的 native waiter watchdog 超时，未产生 race 报告。因此本次不宣称 TSan 全量通过，native Linux 仍需作为发布门槛。
- 构建期间仅见 WSL 挂载时间偏差的 clock skew 警告，不影响上述退出码和测试结果。

## 2026-09-24 显式控制流、Caster 与多 FD 复测

以下命令在 /UserData/CodexWorkSpace/Go2Cpp 执行：

- cmake -S . -B build-check -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON -DGO2CPP_BUILD_HOOK=ON -DCMAKE_BUILD_TYPE=Debug
- cmake --build build-check --parallel 4
- ctest --test-dir build-check --output-on-failure --timeout 60：10/10 通过，包含 go2cpp_tests、scheduler smoke、全部示例和 control_flow_demo。
- GO2CPP_TEST_FILTER=control_flow ./build-check/go2cpp_tests：defer LIFO、注册时参数保存、显式 panic/recover、回调异常隔离、线程隔离和 Caster 通过。
- GO2CPP_TEST_FILTER=io ./build-check/go2cpp_tests：WaitAny/WaitMany 的 ready、timeout、cancel、close、普通线程 EPERM 和 FD generation 用例通过。
- WaitMany 曾暴露“第一个 epoll 回调先恢复 Fiber、第二个已就绪 fd 被 cleanup 取消”的竞态；现已增加零超时 readiness 汇总并重跑上述套件通过。

控制流模块不使用 setjmp/longjmp 或内部 C++ 异常；panic::call() 是显式状态发布，
不是自动栈展开。WaitAny/WaitMany 只服务 managed Fiber，普通线程仍使用原生
poll/select。后续 sanitizer 和 Memcheck 结果以本节追加记录为准。


### 最终复核补充（同一工作树）

- Werror Debug：cmake --build build-werror-new --parallel 4，CTest 10/10 通过。
- Release Hook-off：cmake --build build-release-new --parallel 4，CTest 9/9 通过。
- ASan：ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build-asan-new/go2cpp_tests，全组通过。
- UBSan：UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build-ubsan-new/go2cpp_tests，全组通过。
- Valgrind Memcheck：build-check/valgrind-control-flow-final-2.log，完整套件通过；in use at exit 为 416 bytes/4 blocks，definite/indirect/possible lost 均为 0，ERROR SUMMARY 为 0。
- TSan：setarch x86_64 -R 下 control_flow、io、channel、scheduler_smoke 四个过滤分别通过；完整套件在当前 WSL 120 秒 watchdog 内无可用输出并以 124 超时，因此不宣称 TSan 全量通过，native Linux 仍是发布门槛。
- 为避免 Memcheck 下启动放大造成假失败，IO 用例先等待任务进入 GState::Waiting 再写入测试 fd；同时增加重复 fd/方向和混合无效 fd 的 WaitMany 边界用例。
## 2026-09-24 最终边界修复复测

- WaitMany 现在先验证完整请求集合，再发布任何 epoll 节点；这修复了 Memcheck 下“前一个可读 FD 先完成、后一个无效 FD 被跳过”的顺序竞态。混合无效 FD 用例整体返回 `kError/EBADF`。
- `cmake --build build-check --parallel 4`；Debug CTest 10/10 通过。
- `cmake --build build-werror-new --parallel 4`；Werror CTest 10/10 通过。
- `cmake --build build-release-new --parallel 4`；Release CTest 9/9 通过。
- ASan 和 UBSan 的全量 `go2cpp_tests` 均通过。
- Valgrind Memcheck：`build-check/valgrind-final-7.log`，416 bytes/4 blocks still reachable；definite/indirect/possible lost 均为 0，`ERROR SUMMARY: 0 errors`。
- TSan 在 `setarch x86_64 -R` 下的 `control_flow`、`io`、`channel`、`scheduler_smoke` 四个过滤用例通过；完整套件仍不宣称，WSL watchdog 限制需在原生 Linux 复核。
- 内建 Channel Select 对 move-only 类型现在立即返回 `kInvalid`，避免异步 handoff 悬挂；普通 `Send/Recv` 和独立 `SelectValue` 仍支持 move-only。EventBatch 的 const Handler 只提供观察接口，不能转移所有权。

- 新增 example/io_wait_many_demo.cpp：Hook-on Debug/Werror CTest 各 11/11 通过，
  Hook-off Release CTest 10/10 通过；示例同时验证两个 FD 的 WaitMany 就绪索引和
  WaitAny 超时路径，直接运行输出 wait-many=true。

## 2026-09-26 Context 局部回滚复测

以下命令均在 WSL 的 `/UserData/CodexWorkSpace/Go2Cpp` 执行，构建目录只用于本轮验证：

- Debug + examples：`cmake -S . -B build-rollback ...`、`cmake --build
  build-rollback -j2`、`ctest --test-dir build-rollback --output-on-failure`，11/11
  通过；新增 `go2cpp_context_rollback_demo` 通过。
- `-Werror` Debug：`build-rollback-werror`，CTest 2/2 通过。
- Release：`build-rollback-release`，CTest 2/2 通过。
- ASan + UBSan：`build-rollback-asan`，CTest 2/2 通过，无 sanitizer 报告。
- Context 定向测试重复 30 次通过；覆盖 LIFO/savepoint ABA、父取消/deadline、移动
  所有权、commit、异常 undo、P=1 managed wait 和取消/显式 rollback 竞态。
- TSan：普通启动在 WSL 报 `unexpected memory mapping`；`setarch x86_64 -R` 的
  context 过滤通过。全量启动曾在既有 native waiter watchdog 处中止，不能作为
  WSL 全量通过结论，需原生 Linux 复核。
- Valgrind Memcheck：`build-rollback/valgrind-context-final-2.log`，
  `ERROR SUMMARY: 0`，definite/indirect/possible lost 均为 0；416B/4 blocks
  still reachable 是进程级 TimerService/FiberLocal 缓存，不是 Context 回滚泄漏。
