# Verification Matrix

This document records commands executed against the canonical WSL checkout:

```text
/UserData/CodexWorkSpace/Go2Cpp
```

The project is C++17. Builds use `-Wall -Wextra -Wpedantic`; every blocking
test is registered with a CTest timeout and the stress loops use an external
watchdog. The reference Go source is research input only and is not linked.

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

Result on 2026-09-18 after the boundary fixes: **7/7 passed** (`go2cpp_tests`,
scheduler smoke, and the five examples). The unit executable reported all ten
module groups passed.

The Release static configuration intentionally disables the interposer:

```sh
cmake -S . -B build-static-current -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-static-current --parallel 4
ctest --test-dir build-static-current --output-on-failure --timeout 60
```

Result: **6/6 passed**. A strict GCC build with
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

Result: **7/7 passed**, warning-clean under the configured warning flags.

## Focused boundary checks

The test executable accepts one exact module name in `GO2CPP_TEST_FILTER`.
The following current-source runs passed under a 30-second watchdog:

```sh
for name in hook sync fiber dynamic scheduler context channel panic error; do
  GO2CPP_TEST_FILTER="$name" timeout 30s ./build-check/go2cpp_tests
done
```

These cover default-enabled socket hooks and close/dup/fd reuse,
scheduler-aware mutex/condition-variable/waitgroup, stack migration and
natural Fiber destruction, dynamic M growth/shrink/affinity/stealing, the G
state machine and cancellation queue cleanup, context trees, channels and
select, and panic/error boundaries. The hook regression also checks
native-thread poll fallback timeout reporting, `dup2` replacement-before-close
notification, `dup2(fd, fd)` no-op identity, tracked `FIONBIO` `EFAULT`, and
managed `MSG_OOB` rejection. The scheduler regression includes raw
`Fiber::Suspend(Park)` cancellation and a watchdog on every wait.

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
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  ./build-check/go2cpp_tests > build-check/valgrind-tests-final5.log 2>&1
timeout 300s valgrind --tool=memcheck --leak-check=full \
  --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  ./build-check/go2cpp_scheduler_smoke > build-check/valgrind-smoke-final5.log 2>&1
```

Both exited zero. The final unit run reported `1,809,665` allocations/frees,
`0 bytes in 0 blocks` at exit and `ERROR SUMMARY: 0`; the scheduler smoke run
reported `3,390` allocations/frees, `0 bytes in 0 blocks` and `ERROR SUMMARY: 0`.
There were no definite, indirect, or possible leaks requiring a suppression.
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
        subprocess.run(["timeout", "10s", f"./build-check/{name}"], check=True)
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
preemption, segmented stack growth, exact Go fairness, a blocking-region M>P
handoff, every Linux socket-adjacent API, or a native-Linux TSan result. A
non-cooperative C++ callable can still keep shutdown waiting because safely
discarding a live C++ stack would skip RAII destructors. Channel destruction
must not race raw member calls; throwing element moves/custom select callbacks
have no rollback contract after a wait node is claimed. Managed
`MSG_WAITALL`/`MSG_OOB` are explicit `ENOTSUP` boundaries, while ordinary
threads using a previously adopted runtime-nonblocking socket may not receive
full libc `MSG_WAITALL` blocking semantics. `IOManager*`/hook bindings also
require the caller to honor the documented owner lifetime.
