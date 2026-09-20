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
for name in hook sync fiber dynamic scheduler context channel panic error; do
  GO2CPP_TEST_FILTER="$name" timeout 45s ./build-check/go2cpp_tests
done
```

These cover default-enabled socket hooks and close/dup/fd reuse,
scheduler-aware mutex/condition-variable/waitgroup, stack migration and
natural Fiber destruction, dynamic M growth/shrink/affinity/stealing, the G
state machine and cancellation queue cleanup, context trees, channels and
select, and panic/error boundaries. The hook regression also checks
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
defer/panic/recover boundaries, error chains, channels/select/close, Fiber
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

新增 `timer`、`beginner` 过滤测试各重复 10 轮通过；Clang 18 对新手测试执行
`-fsyntax-only -Wall -Wextra -Wpedantic` 通过。TSan 仍沿用此前 WSL 镜像的启动限制记录，
没有把未启动的完整套件宣称为通过。Valgrind 全套本轮仍有 416B intentional
`still reachable` 进程级状态，`definite/indirect/possible` 均为 0。
