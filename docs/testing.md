# Verification and test matrix

This page records commands run against the standalone Go2Cpp runtime. Commands
are intended to be run from the canonical WSL checkout:

```text
/UserData/CodexWorkSpace/Go2Cpp
```

The build is C++17 and uses the repository warning policy (`-Wall -Wextra
-Wpedantic`). Every potentially blocking test is bounded by either CTest's
timeout or an external `timeout` watchdog.

## Environment

The verification session used:

| Tool | Version / result |
|---|---|
| OS | WSL/Linux |
| GCC | 13.3.0 |
| Clang | 18.1.3 |
| CMake | 3.28.3 |
| Ninja | available |
| Valgrind | 3.22.0 |
| Go executable | not required and not available in the session |

The Go reference files under `third_party/go1.23.0/` are checked-in research
inputs; the C++ build does not link to Go, cgo, or a Go garbage collector.

## Standard builds and example

Debug GCC build, unit/invariant tests, and the end-to-end example:

```sh
cmake -S . -B build-final-modular -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-final-modular --parallel 2
ctest --test-dir build-final-modular --output-on-failure --timeout 60
./build-final-modular/go2cpp_runtime_demo
```

Observed result on 2026-09-15 and reconfirmed on 2026-09-16:

```text
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 0.48 sec
GMP workers=2 P=2 received=42
context_value=7 context_cancelled=true
select_index=0 select_value=7 recv_view_status=1
error=demo operation: root cause
error_is_root=true
recovered=demo panic
panic_recovered=true
```

The two registered CTest entries are `go2cpp_tests` and
`go2cpp_scheduler_smoke`. The test executable reports successful checks for
error chains, context cancellation/deadlines/values, channels/select,
GMP scheduling, and defer/panic/recover.

The install/export smoke also completed:

```sh
cmake --install build-final-debug --prefix \
  /UserData/CodexWorkSpace/Go2Cpp/build-final-install
```

The prefix contains all five static module libraries, the public headers, and
`lib/cmake/go2cpp_runtime/go2cpp_runtime{Config,ConfigVersion,-targets}.cmake`
(including the Debug target file). A package audit also built and installed a
consumers that use the umbrella `go2cpp::runtime` target and the individual
`go2cpp::scheduler` and `go2cpp::error` targets, then repeated both consumers
from a relocated install prefix; all four executables exited zero. The audit
used isolated temporary directories and removed them after the check.

Release and Clang checks were also run with the same test set:

```sh
cmake -S . -B build-final-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-final-release --parallel 2
ctest --test-dir build-final-release --output-on-failure --timeout 60

CC=clang CXX=clang++ cmake -S . -B build-final-clang-env -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build-final-clang-env --parallel 2
ctest --test-dir build-final-clang-env --output-on-failure --timeout 60
```

Both configurations were rerun on 2026-09-16, passed 2/2 tests, and were
warning-clean under the configured warning flags. The scheduler smoke test
uses an always-evaluated check helper, so Release does not silently remove its
safety assertions.

## Sanitizers

AddressSanitizer:

```sh
cmake -S . -B build-final-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-final-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
  ./build-final-asan/go2cpp_tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
  ./build-final-asan/go2cpp_scheduler_smoke
```

Result: all checks passed; no ASan error or leak report.

UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-final-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-final-ubsan --parallel 2
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-final-ubsan/go2cpp_tests
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./build-final-ubsan/go2cpp_scheduler_smoke
```

Result: all checks passed; no UBSan diagnostic.

ThreadSanitizer was configured and compiled with `GO2CPP_ENABLE_TSAN=ON`.
The default WSL address layout can make a TSan process fail before `main()`
with an unexpected-mapping diagnostic, so the reproducible WSL invocation also
uses non-PIE code and disables ASLR for that process:

```sh
cmake -S . -B build-final-tsan-nopie -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGO2CPP_ENABLE_TSAN=ON -DCMAKE_CXX_FLAGS="-fno-pie" \
  -DCMAKE_EXE_LINKER_FLAGS="-fno-pie -no-pie" \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-final-tsan-nopie --parallel 2
TSAN_OPTIONS=halt_on_error=1:exitcode=66 setarch x86_64 -R \
  ./build-final-tsan-nopie/go2cpp_tests
TSAN_OPTIONS=halt_on_error=1:exitcode=66 setarch x86_64 -R \
  ./build-final-tsan-nopie/go2cpp_scheduler_smoke
```

Compilation succeeded. Without the `setarch -R` workaround one smoke attempt
showed the environment-only failure:

```text
FATAL: ThreadSanitizer: unexpected memory mapping ...
```

With the non-PIE/ASLR-disabled invocation above, both executables exited zero
with no TSan report. A native Linux runner is still recommended for the final
TSan gate because the default WSL layout is not stable.

## Valgrind Memcheck

The required leak/error check was run separately for the unit suite and the
scheduler smoke binary:

```sh
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  ./build-final-debug/go2cpp_tests
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
  ./build-final-debug/go2cpp_scheduler_smoke
```

Both runs exited zero. Memcheck reported `0 bytes in 0 blocks` in use at
exit, `ERROR SUMMARY: 0 errors`, and no definite, indirect, or possible leaks
(1,161,500 allocations/frees for the unit suite and 983 for the scheduler
smoke run in this snapshot; allocation counts can vary with libc/thread
implementation, while the zero-error/zero-leak result is the invariant).

## Stress and watchdog gate

The intended watchdog stress command is an inline Python loop (so each binary
has a ten-second upper bound):

```sh
cd /UserData/CodexWorkSpace/Go2Cpp
python3 - <<'PY'
import subprocess
for name, count in (("go2cpp_tests", 200), ("go2cpp_scheduler_smoke", 300)):
    for _ in range(count):
        subprocess.run(["timeout", "10s", f"./build-final-debug/{name}"], check=True)
    print(name, count, "runs passed")
PY
```

Result on 2026-09-16 after synchronizing scheduler task publication and adding
execution-claim/requeue race regressions:

```text
go2cpp_tests_runs=200 result=passed
go2cpp_scheduler_smoke_runs=300 result=passed
```

The earlier intermittent failure was a test startup ordering race: a worker
could enter the lambda before the caller had assigned the `yielding`
shared pointer. The test now constructs and assigns the task before enqueue,
then uses a release/acquire publication gate so the callback cannot read the
self-reference until the caller has published it.

## Coverage and residual gaps

The tests exercise P=1/P=2 scheduling, local/global queues, stealing,
yield/park/wake (including a wake-before-park race), shutdown cancellation,
context parent cancellation and
deadlines (including a 20,000-node cancellation chain and an already-expired
deadline), channel capacities 0/1/N, close/drain, directional views,
ordinary-operation wake-up of armed cases, close/cancel cleanup, independent
unbuffered select rendezvous, full-buffer select-send refill,
same-select opposite-case handling, and
select/default/timeout, error wrapping/identity/type matching (including a
100,000-layer wrapping
chain), and explicit panic/defer/recover
boundaries (including a body exception translated to unhandled panic and a
yield-then-panic task). They do not establish Go compiler/ABI equivalence,
asynchronous preemption, segmented stacks, exact scheduler fairness, or a
native-Linux TSan run under a different loader/kernel. Those limits are part
of the compatibility matrix rather than hidden test claims.
