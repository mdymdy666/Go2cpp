# Upstream and dependency inventory

## Go reference

The implementation was designed against Go `go1.23.0` from
`https://go.googlesource.com/go` (tag commit
`6885bad7dd86880be6929c02085e5c7a67ff2887`). The release archive is
`https://go.dev/dl/go1.23.0.src.tar.gz` with SHA-256
`42b7a8e80d805daa03022ed3fde4321d4c3bf2c990a144165d01eeecd6f699c6`.
The upstream license is BSD-3-Clause; its `LICENSE` is retained in
`third_party/go1.23.0/LICENSE`. The extracted reference files are research
inputs only and are not part of any CMake target.

The local archive is intentionally ignored by Git because it is a large
source tarball. `third_party/go-reference/SHA256SUMS` and the extracted files
are tracked so the provenance and checksum remain reviewable. From the
repository root, verify it with `(cd third_party/go-reference && sha256sum -c
SHA256SUMS)`; the relative archive path in the checksum file is intentional.

## Runtime capability replacement

| Go dependency or mechanism | C++17 replacement in this project | Boundary |
|---|---|---|
| Garbage collector and stack maps | `std::shared_ptr`, RAII, explicit ownership and weak child links | No moving GC or Go heap ABI |
| Segmented/user stacks and `morestack` | Boost.Context protected Fiber stacks and explicit `Task`/`Frame` ownership | Fixed per-Fiber stack; no compiler-generated growth or Go stack maps |
| `mcall`, `gogo`, `gopark`, internal scheduler ABI | Cooperative C++ worker loop with G/M/P state transitions and raw `fcontext` switches | No assembly or asynchronous compiler preemption |
| `sudog`, futex and netpoller waits | Gated heap wait nodes, `ParkingCondition`, shared timer service and Linux epoll IOManager | Backend is Linux epoll; no exact Go netpoller ABI |
| Runtime atomics | `std::atomic` plus transition mutexes where a state handoff spans fields | C++ memory model applies |
| cgo and Go internal ABI | No dependency; public C++ headers only | cgo interoperation is out of scope |
| Compiler-generated defer/panic calls | Explicit `panic_defer::Frame` registration and unwind protocol | Source transformation must create boundaries |

## Build dependencies

The runtime links `Threads::Threads`, Boost.Context (minimum 1.70; verified in
this workspace with 1.83.0), and on Linux the system `dl` library for the hook.
Boost.Context is distributed under the Boost Software License 1.0; Go2Cpp does
not vendor or modify it, and the system package remains the consumer's license
responsibility.
The IO module uses Linux epoll/eventfd/syscalls. Valgrind headers are detected
optionally to register protected Fiber stacks; ASan and TSan integration is
compiled only when the corresponding compiler sanitizer is enabled. No Go
runtime, cgo, bundled GC or GNU GMP mathematics library is linked.

The hook is a shared target by default so its TLS and descriptor registry have
one process instance. Static consumers must configure
`-DGO2CPP_BUILD_HOOK=OFF` and use `IOManager`/explicit APIs; the C hook symbols
are not provided in that mode.

## Local Sylar reference

Clean-room design review also examined the user-provided local checkout at
`/UserData/CodexWorkSpace/sylar2/sylar`. The relevant file is
`iomanager.cc` (lowercase), alongside `fiber.cc`, `scheduler.cc` and `hook.cc`;
there is no `IOManager.cc` with that exact case. The checkout was not added as
a dependency, has no project license file in the supplied directory, and no
source was copied into a build target. Its concepts informed the Fiber swap,
epoll registration-before-timer ordering and hook surface. Go2Cpp adds explicit
ownership/generation gates, natural suspended-stack completion, idempotent
outcomes and safe unknown-variadic-command handling where the reference code
did not define those contracts.
