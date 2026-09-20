# Go2Cpp runtime compatibility layer

This repository contains a small, independently usable C++17 library that models a documented subset of Go runtime semantics. It is a runtime/library, not a Go parser or source-to-source translator.

The public API is under the `go2cpp` namespace. It provides:

- an M/P/G-style scheduler with bounded processors, local/global queues, work stealing, dynamic M growth/shrink, explicit BlockingRegion replacement-M admission, parking and shutdown;
- cancellation contexts with deadlines, values and cancellation causes;
- stackful Fibers, migration-safe FiberLocal values, scheduler-aware mutex/condition-variable/waitgroup facilities, and typed channels with blocking operations, close semantics and select helpers;
- a Linux epoll IOManager and default-enabled socket syscall hook with FD-generation close protection;
- immutable, chainable errors with `Is`, `As`, `Unwrap` and `Join`;
- an explicit frame/defer/panic/recover protocol that does not use C++ exceptions or `longjmp`.

Build and run the default test suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The default Linux build enables shared modules and the transparent hook. For a
static build use `-DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF`.

The compatibility matrix, design boundaries, and reproducible verification
matrix are in [`docs/compatibility.md`](docs/compatibility.md),
[`docs/design.md`](docs/design.md), and [`docs/testing.md`](docs/testing.md).
A source/dependency inventory and the Go reference provenance are in
[`docs/dependencies.md`](docs/dependencies.md) and
[`third_party/go-reference/README.md`](third_party/go-reference/README.md).
Runnable examples are in `example/`: `runtime_demo.cpp`,
`fiber_sync_demo.cpp`, `mixed_runtime_demo.cpp`, `managed_pipeline_demo.cpp`,
`dynamic_gmp_demo.cpp` and `io_hook_demo.cpp` (Linux hook build).
`mixed_runtime_demo.cpp` exercises native-thread/Fiber synchronization,
FiberLocal values, per-thread hook policy, and a declared BlockingRegion.

For translated code that wants the complete public surface, include
`go2cpp/runtime.hpp`. The module targets (`go2cpp::error`,
`go2cpp::context`, `go2cpp::channel`, `go2cpp::scheduler`,
`go2cpp::panic_defer`, `go2cpp::fiber`, `go2cpp::sync`, `go2cpp::io` and
`go2cpp::hook`) are available in the source tree and exported individually in
addition to the `go2cpp::runtime` umbrella target.
