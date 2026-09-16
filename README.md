# Go2Cpp runtime compatibility layer

This repository contains a small, independently usable C++17 library that models a documented subset of Go runtime semantics. It is a runtime/library, not a Go parser or source-to-source translator.

The public API is under the `go2cpp` namespace. It provides:

- an M/P/G-style scheduler with bounded processors, local/global queues, work stealing, parking and shutdown;
- cancellation contexts with deadlines, values and cancellation causes;
- typed channels with blocking operations, close semantics and select helpers;
- immutable, chainable errors with `Is`, `As`, `Unwrap` and `Join`;
- an explicit frame/defer/panic/recover protocol that does not use C++ exceptions or `longjmp`.

Build and run the default test suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The compatibility matrix, design boundaries, and reproducible verification
matrix are in [`docs/compatibility.md`](docs/compatibility.md),
[`docs/design.md`](docs/design.md), and [`docs/testing.md`](docs/testing.md).
A source/dependency inventory and the Go reference provenance are in
[`docs/dependencies.md`](docs/dependencies.md) and
[`third_party/go-reference/README.md`](third_party/go-reference/README.md).
A runnable end-to-end example is in `example/runtime_demo.cpp`.

For translated code that wants the complete public surface, include
`go2cpp/runtime.hpp`. The module targets (`go2cpp::error`,
`go2cpp::context`, `go2cpp::channel`, `go2cpp::scheduler`, and
`go2cpp::panic_defer`) are available in the source tree and exported
individually in addition to the `go2cpp::runtime` umbrella target.
