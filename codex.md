> 当前快照（2026-09-24）：在此前删除旧 panic_defer 模块后，新增
> control_flow 模块。它提供 RAII defer、显式 panic/recover 状态和
> LastDeferException()；这是可验证的 C++ 等价协议，不是 Go 编译器级自动
> 栈展开。Channel select 新增 SelectValue/SelectCaster/SetAny，IOManager
> 新增 Fiber-only WaitAny/WaitMany；普通线程多 fd 等待应使用原生 poll/select。
> 下方历史记录中的“未实现”结论以本说明和最新兼容性矩阵为准。

## 2026-09-26：Context 局部回滚

- 新增 `ContextRollback`/`RollbackScope`：临时子 Context、LIFO undo、savepoint、
  显式 rollback/commit 和独立 `RollbackDone()` 完成通知。
- Context 独立取消现在会从父节点弱登记表中移除 child；deadline timer 注册失败
  不再留下永远等待的 Context。`WithValue` 的异常路径会取消并清理半发布 child。
- undo 只表示局部补偿，不恢复父 Context 或外部 IO；回调在线程锁外执行，异常隔离
  并记录为 `kFailed`。取消先发布 child `Done`，再执行内部 undo，普通 Done 观察回调
  在 undo 后运行；undo 不得等待自身 `RollbackDone()` 或尚未发布的信号。仍禁止
  longjmp、跨 Fiber/线程跳转和在回调中阻塞。
- Context 取消新增前置 claim：在 child `Done` 可观察前冻结 action 日志，避免
  取消窗口追加/提交新动作；未取消的短命 child 会从 parent 的 weak 登记表及时移除。
- 新增 ContextRollback 单元覆盖 savepoint LIFO/ABA、析构/父取消、deadline、移动
  所有权、managed P=1、异常隔离、取消与显式 rollback 竞态；新增
  `example/context_rollback_demo.cpp`，详细契约见 `docs/context_rollback.md`。
- 验证：Debug 示例 CTest 11/11；Release 2/2；`-Werror` 2/2；ASan+UBSan 2/2；
  Context 定向重复 30/30；TSan 使用 `setarch x86_64 -R` 的 context 过滤通过，
  全量 TSan 在 WSL native waiter watchdog/地址映射限制下不作为全量通过结论；
  Valgrind Memcheck `ERROR SUMMARY=0`，definite/indirect/possible 均为 0，
  416B/4 blocks 为进程级 still reachable 缓存。
>
> 当前快照说明（2026-09-23）：以下早期记录是追加式审计历史，其中的
> panic/defer/recover 示例和模块名称不代表当前 API。当前源码已删除
> `panic_defer` 模块、`SendOrPanic`/`CloseOrPanic` 和对应测试；Fiber 只
> 捕获并记录普通 C++ 异常，使用 RAII/`try/catch`。当前交付还包含失败终态、
> 有界 `shutdown_for()`、常见 Hook/sysmon 边界、Fiber claim 析构栅栏以及
> 错误父级的 Failed 放弃路径。

> 本轮收尾还修复了手动 Fiber 在已取消 Context 上等待条件变量的 carrier 阻塞竞态，
> 将 `Channel<T>` 限制为不抛 move/析构并使 `Close()` 不抛，且修正 Fiber 快照中
> main_fiber 的 active 标记。最终 Debug/Release/ASan+UBSan/Valgrind 均通过；
> TSan 在本 WSL 的映射或 native waiter watchdog 约束下未作为通过结论。

## 2026-09-20：新手接口与混合等待边界

- 新增 `go2cpp/go.hpp`：显式/默认 `go()`、默认 Scheduler 生命周期、关闭后新实例重启、轻量小写 `fiber` 和 `Scheduler::add()` 接入。默认调度器使用进程生命周期状态锚点；`shutdown_default_scheduler()` 会移出旧句柄，避免向终态 Scheduler 排队。
- 新增 `go2cpp/event.hpp`：`EventBatch`/`SelectLoop` 薄封装已有 channel `Select`，用协程感知 `sync::Mutex` 串行 `work()`，native 线程和 Fiber 可并发调用（work 本身串行）；handler/Select 异常转为显式错误结果，不强制销毁挂起 Fiber；stop 通过内部 Context 唤醒等待。
- 新增 `tests/test_timer.cpp`，覆盖 P=1 下 TimerService 回调唤醒 Fiber、`Channel::WaitForChange` 不占 M、定时器取消；新增 `tests/test_beginner_api.cpp` 和 `example/beginner_demo.cpp`，覆盖 go/fiber/select、Fiber 内等待、native/Fiber 混用、handler 异常和默认调度器重启。
- `runtime.hpp` 导出新手头文件；CMake/CTest 注册新测试和示例。
- 本次验证：GCC Debug Hook-on 9/9，GCC Release 静态 Hook-off 8/8，`-Werror` Debug Hook-on 9/9，`GO2CPP_TEST_FILTER=beginner` 和 `timer` 均通过。
- 明确边界：Timer 回调在 native 定时器线程执行，只通知等待节点，不直接恢复 Fiber 栈；任意阻塞业务必须经 `go()`/`Scheduler::spawn()`，EventBatch 事件列表生命周期由调用方保证。
- 追加修复：`EventBatch::make_stop_config()` 现在把剩余总时长传递给当前 `Select`，空 channel 在总时长到期时不会无限等待；新增 20ms watchdog 回归。
- 追加修复：EventBatch 增加内部 stop Context；`stop()` 会取消 DoneSignal 并唤醒正在 Select 的 Fiber，按正常返回路径结束，不强制销毁挂起栈；新增 P=1 stop 唤醒回归。
- Release CTest 首次运行出现一次 `mixed_runtime_demo` native-owner watchdog 超时，随后独立运行 5 次并完整 CTest 重跑均通过；记录为宿主资源抖动，未观察到可复现代码回归。

# Change log

## 2026-09-15

- Initialized the standalone Go2Cpp C++17 runtime project in `/UserData/CodexWorkSpace/Go2Cpp`.
- Added the design and compatibility matrix describing GMP, context, channel, error and explicit panic/defer boundaries.
- Added the umbrella CMake runtime target, warning policy, install/export rules, and test/example options.
- Added `docs/testing.md` with reproducible Debug/Release/Clang, sanitizer,
  Valgrind, example, watchdog and stress-test commands.
- Linked the verification matrix from `README.md` for discoverability.
- Key documentation/test files for this verification unit: `docs/testing.md`,
  `README.md`, `codex.md`, and `tests/test_scheduler.cpp`.
- Verification recorded on 2026-09-15: GCC Debug and Release, Clang 18
  Debug, ASan, UBSan, CTest (2/2), the runtime example, and Valgrind Memcheck
  passed. The final Memcheck runs found zero errors and zero bytes in use at
  exit for both the unit suite and scheduler smoke test (661,087 and 846
  allocations/frees in the final sample; allocation counts can vary with the
  libc/thread implementation).
- TSan configured and compiled, but cannot start in this WSL image because of
  `ThreadSanitizer: unexpected memory mapping`; no TSan race conclusion is
  claimed.
- Synchronized the self-referential scheduler test task publication before
  callback access (assignment before enqueue plus a release/acquire gate);
  200 full-suite runs and 200 scheduler-smoke runs passed with a ten-second
  watchdog per process. The smoke test's always-evaluated checks are warning
  clean in Release as well as Debug.
- Added iterative context cancellation and iterative parent-state release so
  deep cancellation/destruction chains do not consume the native call stack;
  the 20,000-node test and an independent 100,000-node audit passed.
- Added explicit expired-deadline behavior, root-context cancellation
  protection, channel cancellation-priority coverage, panic-body exception
  translation, and the scheduler yield-then-unhandled-panic regression.
- Restricted scheduler `yield`/`park` to the current G and finalized unbound P
  states as dead during shutdown; sparse-P and never-started shutdown tests now
  cover those transitions.
- Made `error::Is`/`As` chain traversal iterative and added a 100,000-layer
  wrapping regression; built-in wrapper release is iterative, while custom
  error ownership/message graphs remain required to be acyclic.
- Split CMake into option/module/test/install files and verified umbrella,
  individual-module, and relocated-prefix package consumers; all configured,
  built, and exited zero. The final smoke and full-suite watchdog runs each
  completed 200/200 iterations.
- Status: implementation and stress verification are complete for this
  snapshot; native-Linux TSan remains a follow-up because the WSL image cannot
  start the instrumented process.

## Pause handoff (2026-09-15)

The requested pause is at the pre-commit handoff. The latest canonical WSL
source includes the fake-clock expiry fix, named/typed context-key isolation,
current-G-only scheduler yield/park admission, sparse-P shutdown finalization,
iterative built-in error-chain release, and panic-handler exception isolation.

Latest completed gates:

- `build-final-modular`: GCC Debug build and CTest 2/2 passed; the example
  prints the GMP/channel/error/panic demonstration output.
- `build-final-asan` and `build-final-ubsan`: rebuilt after the latest runtime
  changes; the full unit executable passed with no diagnostics.
- Valgrind Memcheck on the unit and scheduler-smoke executables: both exit 0,
  `0 bytes in 0 blocks`, `ERROR SUMMARY: 0`; the sampled allocation counts
  were 661,087/661,087 and 846/846.
- Watchdog stress: `go2cpp_tests` 200/200 and
  `go2cpp_scheduler_smoke` 200/200 passed, with a ten-second timeout per run.
- The existing package-consumer audit passed for the umbrella target,
  individual module targets, and a relocated install prefix; its temporary
  source/install directories were removed.

## Resume verification (2026-09-16)

- Reconfigured and rebuilt GCC Release after the final context/panic edits;
  CTest passed 2/2 and the runtime example exited zero with the expected
  GMP/channel/error/recover output.
- Reconfigured with `CC=clang CXX=clang++`, rebuilt with Clang 18, and passed
  CTest 2/2. Both final compiler configurations were warning-clean under the
  configured `-Wall -Wextra -Wpedantic` policy.
- Rechecked the Go `go1.23.0` source archive against
  `third_party/go-reference/SHA256SUMS`; SHA-256 verification passed.
- Remaining handoff action is repository staging/diff review and the requested
  Git commit. Native-Linux TSan remains an external environment follow-up.

Known boundaries to preserve in the final report: cooperative rather than
asynchronously preemptive scheduling, native blocking calls occupy their M,
polling/API-based select, explicit nil-channel status instead of an infinite
nil-channel block, copyable values for `SendCase`, no Go compiler/ABI/GC, and
the documented inability of a C++ throw during explicit `Frame` destruction
to be recovered by that frame. Custom error ownership/message graphs must be
acyclic. The default WSL TSan invocation remains loader-sensitive and can report
`ThreadSanitizer: unexpected memory mapping` before the test starts; the final
non-PIE/ASLR-disabled WSL run is recorded below, and a native Linux gate is
still recommended.

## Finalization (2026-09-16)

- Added a persistent per-Scheduler owner token to `Task`; a G is bound to the
  first scheduler that admits it, and foreign `enqueue`/`wake` calls are
  rejected without changing the task state. Added the cross-scheduler
  ownership regression in `tests/test_scheduler.cpp`.
- Added the full-buffer select-send regression in `tests/test_channel.cpp`.
  It verifies that draining a full capacity-1 channel refills the slot from an
  armed select sender and preserves FIFO order.
- Updated `docs/design.md`, `docs/scheduler.md`, and
  `docs/compatibility.md` with the ownership and bounded-wait contracts. Added
  `docs/dependencies.md` with the Go provenance and GC/ABI/stack/cgo
  replacement inventory; linked it from `README.md`.
- Rebuilt and tested the final source with GCC 13 Debug (`build-final-modular`),
  GCC 13 Release (`build-final-release`), Clang 18 (`build-final-clang-env`),
  and a strict `-Werror` build (`build-final-werror`). Every CTest run passed
  2/2; the example printed the GMP, select/directional-channel, error and
  recover output.
- Rebuilt ASan and UBSan configurations and ran both unit and scheduler-smoke
  executables with no diagnostics. Valgrind Memcheck reported zero errors and
  zero bytes in use at exit for both binaries (1,161,500 and 983 allocations/
  frees in the recorded sample). The Go archive checksum verified `OK` from
  `third_party/go-reference`.
- TSan compiled successfully. The default WSL layout can fail at process
  startup with `unexpected memory mapping`; running the non-PIE build through
  `setarch x86_64 -R` allowed both test executables to exit zero with no TSan
  report. A native Linux runner remains the recommended independent gate.
- Watchdog stress completed 200 `go2cpp_tests` runs and 300
  `go2cpp_scheduler_smoke` runs, each with a ten-second process timeout. The
  refreshed `build-final-install` prefix contains all module/umbrella targets,
  headers and package configuration files.
- Status: all requested compatibility-subset modules, examples, tests and
  documentation are complete for this snapshot. Remaining work is optional:
  run TSan on a native Linux kernel/loader and expand the explicit API if a
  future translator needs semantics outside the documented subset.

## Final audit continuation (2026-09-16)

- Closed the scheduler admission window identified during review: queue
  removal, runnable accounting, stop observation, and the Task execution claim
  now occur under one scheduler admission transaction. Added regressions for
  duplicate enqueue during a yielded callable and wake-after-park before the
  callable returns.
- Made internal requeue/wake failure handling idempotent with
  `cancel_if_runnable_unqueued()`. A concurrent successful public enqueue is no
  longer mistaken for a failure and cannot be cancelled by the worker.
- Added terminal task-registry pruning and moved shutdown queue-task
  destruction outside scheduler/P/join locks. The smoke suite now verifies a
  task capture whose destructor re-enters the Scheduler; it exits under the
  ten-second watchdog.
- Refreshed the example to print context value/cancellation and error identity
  checks. The final example output and exit status are successful.
- Final GCC Debug and `-Werror` CTest runs passed 2/2 after the audit fixes;
  ASan, UBSan and non-PIE/ASLR-disabled TSan were rebuilt and passed both test
  executables. Valgrind Memcheck reported zero errors and zero bytes in use at
  exit: unit `1,161,500` allocs/frees and smoke `983` allocs/frees.
- The watchdog loop passed 200 unit runs and 300 scheduler-smoke runs after
  the audit fixes. The installed umbrella, individual-target and relocated
  package consumers all exited zero. `sha256sum -c` still reports `OK` for the
  pinned Go archive.
- No required implementation task remained for the compatibility subset
  documented at that snapshot. Its then-known future work was limited to a
  native-Linux TSan gate and translator/ABI features outside this standalone
  C++17 library. The following boundary audit supersedes that statement for
  the newly requested Fiber, socket-hook, coroutine-sync, and dynamic-M scope.

## Boundary audit and HOOK clarification (2026-09-17, superseded)

This historical snapshot predates the implementation below. Its "not
implemented" conclusions are retained for audit history; use the 2026-09-18
section for the current status.

- Rechecked the four newly raised boundary cases against the source, tests and
  public headers. A returning/panicking callable or a cancelled queued/waiting
  task reaches a terminal G state, and queue/wake/shutdown races are covered.
  This does not force-unwind a running C++ stack or destroy a `Task` retained
  by user ownership. The scheduler does not preserve a C++ stack across
  `yield`/`park`; a callable that never returns cannot be resumed by this
  backend, and a blocking syscall occupies its M.
- The socket requirement is **not implemented**. There is no `IOManager`, FD
  readiness backend, `addEvent`/`cancelEvent`, `wait_for_event`, or syscall
  hook registry. The requested hook semantics include original-syscall
  fallback, readiness-before-timer registration, idempotent timeout/cancel
  wake-up, nonblocking-mode handling, FD reuse generations, and
  FD-close/`EINTR`/`EBADF` rules. Required transparent interception covers
  connect/accept, read/recv, write/send, close, fcntl/ioctl, and socket timeout
  options; explicit wrapper-only I/O is not sufficient.
- Coroutine-level `Fiber`, mutex, condition-variable and waitgroup facilities
  are not implemented. Existing `std::condition_variable` instances are
  internal OS-thread coordination, not coroutine suspension. A stackful-fiber
  backend or explicit callback/state-machine API is required.
- M management is fixed at `Scheduler::start()`: one stable worker per
  configured slot, with local/global queues and stealing. There is no dynamic
  scale-up/scale-down, blocked-syscall handoff, idle-M reclamation, or
  same-task-type thread-cache contract.
- Status: the existing compatibility subset remains intact, but these four
  boundary requests are only partially satisfied. The gaps are now explicit
  in `docs/compatibility.md` and `docs/design.md`.

## Boundary implementation and final verification (2026-09-18)

- Replaced the earlier callback-only scheduler path with a stackful
  Boost.Context Fiber backend. `Task` now owns a resumable C++ continuation;
  G/M/P admission, local/global queues, bounded stealing, park/wake pending
  permits, owner binding, dynamic M growth/shrink and task-class affinity are
  implemented in `src/scheduler.cpp`. Shutdown requests cooperative
  cancellation, wakes parked Gs, waits for started Fibers to return through
  their trampoline, releases completed Fiber stacks immediately, and never
  force-discards a live C++ stack.
- Added scheduler-aware `sync::Mutex`, `ConditionVariable` and `WaitGroup`,
  shared `ParkingCondition` and process-wide timer service. Managed G waits
  suspend their Fiber and leave the M available; unmanaged contended mutex
  calls deliberately return false/throw rather than silently blocking an
  unknown OS thread.
- Added Linux `IOManager`/epoll/eventfd readiness and the shared default-on
  socket hook. The hook covers connect/accept/read/recv/write/send,
  close/dup/fcntl/ioctl, socket timeout options and sleep. It registers
  readiness before deadline timers, uses one-shot outcomes and generation
  tokens, and has original-call/native-poll fallbacks. A boundary audit fixed
  `dup2`/`dup3` pre-replacement close notification, `dup2(fd, fd)` identity,
  native fallback `ETIMEDOUT`, and extreme-deadline conversion overflow.
- Added runnable examples under `example/`: `fiber_sync_demo.cpp`,
  `managed_pipeline_demo.cpp`, `dynamic_gmp_demo.cpp` and `io_hook_demo.cpp`.
  Added saturating relative-deadline arithmetic for Context and channel/select
  APIs, with regression coverage for `ContextDuration::min()/max()`.
  Updated the compatibility/design/dependency/scheduler/testing documents to
  distinguish implemented semantics from unsupported Go/compiler/ABI behavior.
- Current verification from the canonical WSL path:
  GCC Debug shared CTest **7/7**, Clang 18 Debug **7/7**, Release static
  Hook-off CTest **6/6**, strict `-Werror` CTest **7/7**; ASan and UBSan full
  unit/smoke runs passed; Valgrind unit and smoke runs both exited zero with
  `0 bytes in 0 blocks` and `ERROR SUMMARY: 0` (final alloc/free totals
  `1,883,691` and `3,387`); `(cd third_party/go-reference && sha256sum -c
  SHA256SUMS)` returned `OK` for the Go 1.23.0 archive; install/export smoke
  completed; serial watchdog stress passed
  `go2cpp_tests` **40/40** and `go2cpp_scheduler_smoke` **80/80**.
- TSan was rebuilt with non-PIE/ASLR-disabled WSL invocation. Filtered
  context/channel/scheduler/dynamic/fiber/sync/io runs passed. The full suite and hook filter
  exceeded the WSL watchdog without a diagnostic, so no full-suite TSan pass
  is claimed; native Linux remains the appropriate final TSan gate.
- Remaining explicit boundaries: no Go parser/compiler/GC/ABI, no
  asynchronous preemption or blocking-region M>P handoff, cooperative rather
  than forced Fiber cancellation, no exact Go fairness, and no transparent
  interception of every Linux socket-adjacent API (`poll`/`select`/`ppoll`,
  `*mmsg`, `sendfile`, `splice`, and signal-restart policy remain outside the
  subset). Static consumers must disable the shared Hook target.

## Final boundary regression and handoff (2026-09-18)

- Fixed cancellation/shutdown lost-wake windows for unstarted queued Gs and
  for advanced raw `Fiber::Suspend(Park)` users. Failed queue claims now prune
  terminal tasks and release callable captures; scheduler shutdown is notified
  when an external cancellation changes the terminal predicate. Fiber creation,
  pointer publication, resume observation and terminal ownership transfer are
  serialized against cancellation by the Task transition mutex.
- Added regressions for cancelled-queue capture release, raw Fiber park
  cancellation, managed `MSG_OOB` (`ENOTSUP`), tracked `FIONBIO` invalid-pointer
  `EFAULT`, empty/inert select, singleton `errors.Join`, context callback
  subtree marking, and zero/negative timed mutex acquisition.
- Hook boundary fixes include applying the configured connect timeout to native
  fallback, kernel-first `FIONBIO` validation, split close lifecycle locking
  around potentially blocking `SO_LINGER`, and explicit documentation for
  raw syscall/`io_uring`/`close_range`/fork-exec gaps. Ordinary-thread
  `MSG_WAITALL` on a previously adopted runtime-nonblocking descriptor remains
  a known semantic limitation; managed `MSG_WAITALL` and `MSG_OOB` return
  `ENOTSUP`.
- Earlier current-tree verification baseline: GCC Debug shared **7/7**, Release static
  Hook-off **6/6**, Clang 18 Debug **7/7**, and strict `-Werror` **7/7**;
  ASan and UBSan unit/smoke runs passed. Valgrind Memcheck logs
  `build-check/valgrind-tests-final3.log` and
  `build-check/valgrind-smoke-final3.log` both exited zero with 0 bytes in
  use, ERROR SUMMARY 0, and allocation totals `1,908,203` and `3,388`.
  The final serial watchdog run passed 40 unit and 80 smoke executions.
- TSan filtered runs completed earlier for context/channel/scheduler/dynamic/
  fiber/sync/io without a diagnostic; the rebuilt final WSL run did not finish
  within the watchdog, so no full-suite or hook TSan pass is claimed. Native
  Linux TSan remains a release-gate follow-up. Channel destruction, throwing
  select element/callback rollback, static-destruction ordering for global
  contexts, and owner lifetime for raw `IOManager*` bindings remain explicit
  documented preconditions or follow-up work.
- After the final `IOManager::wait` park-failure guard and select-formatting
  cleanup, Memcheck was rerun on the current tree. `valgrind-tests-final4.log`
  and `valgrind-smoke-final4.log` both exited zero with `0 bytes in 0 blocks`
  and `ERROR SUMMARY: 0`; allocation totals were `1,815,429` and `3,388`.
- After the public pre-cancelled-`Task` admission cleanup and its regression,
  Memcheck was rerun again on the current tree. `valgrind-tests-final5.log`
  and `valgrind-smoke-final5.log` exited zero with `0 bytes in 0 blocks` and
  `ERROR SUMMARY: 0`; allocation totals were `1,809,665` and `3,390`.

## Boundary API follow-up (2026-09-20)

- Extended the public documentation and runnable examples for the mixed native
  thread and managed Fiber boundary. Added mixed_runtime_demo.cpp and registered
  it as a CMake example/test target.
- Documented scheduler-aware mixed Mutex, ConditionVariable and WaitGroup
  semantics, including native condition-variable wait nodes, non-thread-owned
  unlock, timeout/cancellation expectations, and the prohibition on holding a
  thread-owned std::mutex across Fiber migration.
- Documented FiberLocalCache/FiberLocal value lifetime and migration behavior,
  per-thread hook and GMP participation scopes, and the intentional boundary
  that external participation records policy only and does not attach an M or
  run a queue.
- Documented BlockingRegion replacement-M admission. Known interposed socket
  and sleep fallbacks account for native blocking automatically; arbitrary
  unhooked native calls still require the explicit RAII region and remain
  cooperative.
- The post-boundary Debug build in build-docs-check compiled 57 targets; CTest
  passed 8/8, and the mixed example passed 20/20 watchdog runs. The Hook-off
  Release static build in build-docs-static passed 7/7, including the mixed
  example. Final sanitizer, Valgrind and compiler reruns are recorded below.

## Final verification after boundary hardening (2026-09-20, superseded)

- Made BlockingRegion non-movable and bound its cleanup to the entering M id.
  The destructor repairs the captured M even if an invalid Fiber migration
  occurs; the public contract still forbids yielding or parking inside the
  region. Native close fallback now uses the same blocking accounting.
- Worker participation scopes now preserve the scheduler-owned worker metadata
  when nested inside a runtime M. External participation remains an explicit
  policy/eligibility scope only; it does not attach an M or run a queue.
- Managed hook admission without an IOManager now lazily adopts sockets and
  uses the bounded native poll/deadline path when hooks are enabled. Explicitly
  disabled hooks preserve native blocking semantics and use BlockingRegion
  accounting. The recursion guard remains active.
- Added the plain-Scheduler lazy-adoption timeout regression and the worker
  nested-policy regression. The ASan test race that asserted Task::state
  immediately after a completion counter was made watchdog-based.
- Made the context deadline TimerService an intentionally process-lifetime
  singleton, removing static Context/TimerService destruction-order UAF risk;
  an exit callback stops and joins the service thread while the intentionally retained service object remains valid.

Final verification from /UserData/CodexWorkSpace/Go2Cpp:
- GCC Debug Hook-on CTest 8/8; Release static Hook-off 7/7; strict Werror 8/8;
  Clang 18 8/8.
- ASan and UBSan full unit/smoke runs passed with the documented environment
  variables.
- Memcheck logs build-check/valgrind-boundary-tests-final4.log and
  build-check/valgrind-boundary-smoke-final4.log exited zero with ERROR SUMMARY
  0. Definite, indirect and possible leaks were all zero. Intentional process-lifetime FiberLocal registry and context-timer service storage remained reachable: 360 bytes in three blocks for the unit run and 96 bytes in one block for smoke. The native context waiter now has a 2-second `WaitFor` watchdog before join.
- Watchdog stress passed go2cpp_tests 40/40 and go2cpp_scheduler_smoke 80/80.
  Rebuilt TSan scheduler, dynamic, and fiber filters passed with `setarch x86_64 -R`; the full suite exceeded a 120-second Python watchdog without a diagnostic, while the normal WSL invocation reports `ThreadSanitizer: unexpected memory mapping`. Native Linux remains the TSan gate.
- Install/export smoke passed with the current `build-install-final` tree and a temporary package consumer.
- Known semantic boundaries remain: no exact Go P handoff or asynchronous
  preemption, no external-thread attach/run-one backend, no Fiber stack pool,
  and no guarantee of same-task-type cache reuse. Arbitrary unhooked native
  blocking still requires an explicit BlockingRegion; poll/select/mmsg,
  sendfile/splice, io_uring and similar socket-adjacent APIs remain outside
  the hook subset.


## Final boundary hardening and race-detector follow-up (2026-09-20)

- Added watchdog-safe test infrastructure in tests/test_support.hpp:
  bounded RequireEventually, aborting Require, and thread joins guarded by
  a separate watchdog thread. Long test Fiber loops use a 1 ms pause only when
  running under Valgrind, so Memcheck cannot starve the coordinating test
  thread while TSan keeps the original yield behavior.
- Hardened tests/test_scheduler.cpp and related module/smoke/example tests:
  cancellation and publication waits have absolute deadlines, critical waits
  fail fast instead of continuing into unsafe teardown, and the cancelled
  queue-capture case uses a strict one-worker/no-oversubscription scheduler.
- TSan found a real race during this pass: dynamic M growth relocated
  Scheduler::Impl::machines vector slots while a managed worker performed
  BlockingRegion lookup. The machine record container is now a deque, so
  published shared-pointer slots are not relocated during append; scheduler,
  dynamic and fiber TSan filters then passed repeatedly. This is reflected in
  src/scheduler.cpp and docs/scheduler.md.
- Final GCC Debug/Release, strict Werror, Clang, ASan and UBSan CTest matrices
  passed: 8/8 Debug, 7/7 Release static, 8/8 Werror, 8/8 Clang, 8/8 ASan,
  and 2/2 UBSan unit/smoke.
- Valgrind final8 logs passed with zero definite/indirect/possible leaks:
  unit 1,516,975 allocs / 1,516,972 frees and 360 bytes in three
  intentional reachable blocks; smoke 3,406 / 3,405 and 96 bytes in one
  intentional FiberLocal-registry block. Both report ERROR SUMMARY: 0.
- Final stress passed 10 unit and 10 scheduler-smoke executions with a
  45-second process watchdog. Install/export consumer smoke printed 7.
- Remaining boundaries are unchanged and intentional: no exact Go P handoff or
  asynchronous preemption, external participation is metadata-only, FiberLocal
  is a logical value cache rather than a Folly stack/object pool, arbitrary
  unhooked blocking needs explicit BlockingRegion, and the WSL full-suite
  TSan gate remains incomplete pending native Linux.

## Shutdown lock hardening (2026-09-20)

- External shutdown now moves each published `std::thread` out while holding
  `Impl::mutex`, releases the scheduler mutex, and joins the moved thread. The
  final processor/state scan is also lock-protected. This removes the temporary
  snapshot allocation from the OOM-sensitive shutdown path while preserving
  the `join_mutex -> Impl::mutex` ordering.
- After this change, Debug CTest remained 8/8; dynamic, sync and scheduler-smoke
  watchdog runs passed. Memcheck final9 reported zero definite, indirect or
  possible leaks (unit 1,517,159 allocs / 1,517,156 frees; smoke 3,415 / 3,414)
  with only the documented 360B and 96B process-lifetime reachable blocks.

## Reaper lifetime hardening (2026-09-20)

- `reap_dead_workers()` now reserves its retired-thread vector before moving
  any joinable `std::thread`, and retains `join_mutex` through the actual joins.
  This makes the OOM path non-terminating and keeps external shutdown from
  observing a half-finished reaper maintenance pass.
- After the reaper change, Release Hook-off 7/7, strict Werror 8/8, Clang 8/8,
  ASan 8/8 and UBSan 2/2 passed; TSan scheduler and dynamic filters passed
  under `setarch x86_64 -R`. Final10 Memcheck again had zero definite,
  indirect or possible leaks: unit 1,560,975/1,560,972 allocations/frees and
  360B reachable; smoke 4,042/4,041 and 96B reachable; both `ERROR SUMMARY: 0`.

## 2026-09-21 恢复工作验证

- 重新确认 WSL 工作目录为 `/UserData/CodexWorkSpace/Go2Cpp`，Git 根目录、
  分支和工作区状态正确；恢复时未发现未提交改动。
- 使用已有 `build-check` 运行
  `ctest --test-dir build-check --output-on-failure --timeout 60`，当前 Debug
  Hook-on 测试 **9/9** 通过，包含单元测试、调度器、Fiber 同步、动态 M、
  混合 native/Fiber、入门 API 和 IO Hook 示例。
- 本次只做恢复审计和回归验证，没有引入新的实现缺口。未改变已记录的边界：
  WSL 环境仍不能把完整 TSan 套件作为最终通过依据；异步抢占、完整 Go
  编译器/ABI/GC 语义和未覆盖的 Linux socket API 仍属于明确限制。


## Sysmon 长系统调用交接（2026-09-23）

- 在 `src/scheduler.cpp` 与 `include/go2cpp/scheduler/scheduler.hpp` 增加可配置 sysmon 监控线程：`enable_sysmon`、`sysmon_interval`、`long_syscall_threshold`。
- `BlockingRegion` 和 Hook native fallback 发布 M 的阻塞起点；达到阈值后记录 detached/task/count，从 P 的 attached 计数逻辑解绑，并按 `min_workers + detached_count` 申请替代 M；原 M 返回时恢复 P 计数。
- detached M 存在期间提高空闲回收底线，避免替代 M 在原系统调用返回前被回收；worker epilogue 防止 attached 计数下溢；shutdown 在 drain 完成后停止并 join sysmon。
- 修复 `IOManager` 已存在但未跟踪 FD adopt 失败时直接进入 libc 的路径，使 managed G 仍进入 `BlockingRegion`；新增 pipe fallback Hook 回归。
- 更新 `docs/scheduler.md`、`docs/design.md`、`docs/compatibility.md`、`docs/go_reference_audit.md`、`docs/testing.md`，明确这是安全的逻辑解绑，不是 Go 精确 `_Psyscall` 或异步抢占；raw syscall/未 Hook 阻塞仍是边界。
- 验证：Debug Hook-off 2/2、Hook-on 9/9、dynamic 重复 10 次、ASan 2/2、UBSan 2/2、Valgrind dynamic 0 definite/indirect/possible lost；TSan 全量受 WSL + Boost.Context 映射限制，未宣称通过。


## Fiber 父链生命周期与 sysmon 边界加固（2026-09-23）

- `src/fiber.cpp` 引入共享 `FiberRecord` 父链元数据。父 Fiber 销毁后，子 Fiber 的诊断快照和取消遍历不再解引用裸父对象；共享记录保留 `alive=false` 墓碑。真正的挂起栈恢复仍要求固定父对象存活。
- Ready Fiber 析构可跳过尚未进入的用户栈；Suspended Fiber 在错误父级之外析构或传播失败会标记 Failed 并清理可安全清理的状态；挂起栈上的 RAII 只能由固定父级正常收尾。
- worker 任务边界检测未销毁的 `BlockingRegion` 并 fail-fast，防止跨 yield/park 的错误记账污染后续 G。
- 新增 Fiber 墓碑/唤醒后父链顺序回归和双长 syscall sysmon 回归。Debug、Hook-on、Release、Werror、Clang、ASan、UBSan、TSan（全量 3 次）与 Valgrind 均按 `docs/testing.md` 记录的命令通过。

## 2026-09-23 monitor 活性、嵌套 IO 与栈边界复核

本轮在既有 sysmon/Fiber 父链实现上继续复核并修正：

- src/scheduler.cpp：sysmon 的周期等待改用独立 sysmon_wait_mutex/sysmon_wait_condition，不再因 Scheduler 主锁被高负载路径占用而阻塞在等待入口；扫描阶段保留 try_to_lock，sysmon_pass_count 明确为心跳尝试计数。仍保持单 monitor 线程，M 扩容由已有有界策略负责。
- include/go2cpp/scheduler/scheduler.hpp、src/io.cpp：IO park 通过 park_io() 标记 SuspendReason::Io，方便诊断嵌套等待链。
- src/fiber.cpp：恢复路径在嵌套 scheduler park 后按返回状态恢复 TLS 当前 Fiber，防止父 Fiber 创建第二个子 Fiber 时错绑已完成的兄弟 Fiber。
- tests/test_hook.cpp：增加两层连续 timeout/ready、三层 Fiber 父链连续 IO、Fiber 快照深度和 send(MSG_NOSIGNAL) 系统错误回归；修正 phase=2 瞬时状态的测试观察方式，避免把合法快速推进误报为 watchdog。

结论保持谨慎：嵌套 Fiber 的单 FD IO、timeout、ready、cancel/close 唤醒和父级自然返回已验证；没有公共多 FD wait_any/Fiber select。sysmon 不能安全地跨线程终止或迁移 C++ Fiber，也不能注入系统级异常。Fiber 栈仍是带保护页的固定大小栈，没有 Go morestack/copystack 自动增长；需显式设置 fiber_stack_size。Hook 的 write/send 错误保持 libc errno，不隐式转 C++ 异常；SIGPIPE 仍遵循调用者的 POSIX 策略。

## 2026-09-23 最终复测修订

- Fiber 恢复路径新增栈对象 RAII TLS guard；任何嵌套 scheduler park、取消或早退都恢复进入 resume 前的直接调用者，避免 TLS 残留把已完成兄弟 Fiber 当作后续子 Fiber 的父级。
- 当前源码重编译后的 Hook/Fiber/dynamic 过滤器各 5/5，通过 Hook 9/9、Release 8/8、Werror 2/2、Clang 2/2、ASan 2/2、UBSan 2/2；TSan 在 setarch x86_64 -R 下当前全量单次通过，无 race 报告。
- WSL + TSan 重复全套仍可能在不同 native waiter watchdog 超时，不能据此宣称重复稳定；这属于测试环境风险，生产实现未通过放宽同步语义来掩盖。
- 最新 Memcheck 为 build-sysmon-hook/valgrind-final-current.log，definite/indirect/possible lost 均为 0，416B 为已有进程级 reachable 状态。

## 2026-09-24：显式控制流、Caster 与多 FD 等价层

- 新增 control_flow 模块：defer 是不可复制 RAII 对象，构造参数按值保存，析构或
  run_now() 按 C++ 作用域逆序执行一次；panic/recover 使用共享 PanicInfo 状态，
  recover 只在 defer 回调边界消费，回调异常通过 LastDeferException() 查询。
- 恢复旧 panic_defer 模块并不安全，因此没有使用 longjmp、信号异常或 C++ 内部
  异常伪造 Go 栈跳转。该模块是显式状态协议，调用者必须在 call() 后自行 return
  或分支；普通 C++ 异常、Fiber failure 和 error 保持独立。
- Channel select 增加 SelectValue、SelectCaster、MakeCaster、SetAny 和栈对象
  Cast 重载；std::any 仍保持兼容字段，不可复制源值可通过共享 holder 访问。
- IOManager 增加 WaitAny/WaitMany 及 timeout/cancel/close 组合；多个请求登记到
  epoll，普通线程明确返回 EPERM。修复一次多 FD readiness 竞态：Fiber 恢复前用
  零超时 poll 汇总仍就绪节点，再执行其余 waiter 清理。
- 本轮 Debug CTest 为 10/10；更多 Release、Werror、ASan、UBSan、Valgrind
  结果在 docs/testing.md 追加，未把 WSL 环境限制冒充 sanitizer 通过。

已知边界：recover 不提供 Go 编译器级 G/栈帧隔离，panic 不自动终止 Fiber 或展开
外层 defer；SelectCaster 的可变函数对象需要调用方自行保证并发安全；固定 Fiber 栈
仍未实现 Go 式动态扩容；poll/select Hook 保持 libc ABI，不自动改写为 WaitMany。


## 2026-09-24 最终边界复核

- defer/panic/recover 增加显式状态协议测试：参数注册时保存、LIFO、run_once/dismiss、普通异常作用域展开、回调异常记录、Caster 异常隔离和线程边界。
- SelectResult::Value<T>() 对不可复制 T 不再实例化 std::any_cast；新增 TakeValue<T>()，避免 move-only 类型的编译期断言。
- WaitMany 的补采样改为只完成 fd/方向队列头节点，并拒绝同一集合内重复 fd/方向；新增混合无效 fd 回归，保持外部 waiter FIFO。
- Debug 10/10、Werror 10/10、Release 9/9、ASan/UBSan 全组通过；Valgrind 完整套件 416B/4 blocks still reachable，definite/indirect/possible 均 0，ERROR SUMMARY 0。
- TSan 在 setarch 下四个过滤用例通过；完整套件在 WSL 120 秒 watchdog 超时，未宣称全量通过。
- 已知语义边界仍保留：panic 不自动展开 C++ 栈，recover 不绑定 G/Fiber owner，固定 Fiber 栈无 Go 式动态增长，WaitAny/WaitMany 仅 managed Fiber 可用。
## 2026-09-24 最终修复记录

- 修改内容：为 WaitMany 增加完整请求集合预校验，避免在部分 epoll 节点已发布后才发现无效 FD；修复 Memcheck 下混合无效 FD 的顺序竞态。
- 修改内容：SelectWaitState::Take 直接移动 typed holder；内建 Channel Select 对不可复制元素明确返回 kInvalid，普通 Send/Recv 与独立 SelectValue 保留 move-only 支持。
- 验证：Debug CTest 10/10、Werror CTest 10/10、Release CTest 9/9；ASan/UBSan 全量测试通过。
- 验证：Valgrind Memcheck 完整套件 ERROR SUMMARY 0，definite/indirect/possible lost 均为 0，416 bytes/4 blocks still reachable。
- 验证：TSan 在 setarch 下 control_flow、io、channel、scheduler_smoke 四个过滤用例通过；完整套件仍受 WSL watchdog 限制，未宣称全量通过。
- 后续状态：构建目录和测试日志已清理，本轮提交已完成。固定 Fiber 栈、显式 panic/recover 协议、managed Fiber 专用 WaitAny/WaitMany 和 Caster 并发责任仍属于兼容性边界。

- 示例补充：新增 example/io_wait_many_demo.cpp，展示两个 socket FD 的 WaitManyFor
  和 WaitAnyFor；Hook-on Debug/Werror 各 11/11，Hook-off Release 10/10，直接运行
  输出 wait-many=true。

## 2026-09-30：工程化日志与配置模块

- 新增 `include/go2cpp/log.hpp`、`src/log.cpp`：自有 C++17 日志实现，按
  `LogFilter -> LogItemWorker -> LogFormatter -> LogSink` 解耦；默认使用线程安全
  文件 Sink，发布等级为 warn，默认写入 `log/go2cpp.log`，stdout 必须配置开启。
- 提供 `PatternFormatter`、`CallbackSink` 和大小写兼容的 `GO2CPP::Logger`/宏接口，
  可在回调中桥接 spdlog/fmt，不把第三方 ABI 强行加入运行时。
- 新增 `include/go2cpp/config.hpp`、`src/config.cpp` 和根目录 `go2cpp.ini`：支持
  section、key=value、注释、日志和调度器参数统一校验；`max_workers` 上限 32、
  `local_queue_limit` 必须大于 0 等约束在启动前检查。
- CMake 增加 `go2cpp_log`、`go2cpp_config` 目标及安装导出；新增日志并发/过滤/配置
  边界测试与 `example/log_config_demo.cpp`。
- 验证：WSL GCC 13.3 + Boost.Context 1.83 的工程化构建成功；日志过滤并发测试通过，
  `go2cpp_log_config_demo` 通过。完整 CTest 中已有 scheduler-aware synchronization
  用例一次出现既有的时序失败，单独重跑日志测试和其余目标成功；该失败未被隐藏。
