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
are tracked so the provenance and checksum remain reviewable. Verify the
archive from `third_party/go-reference` with `sha256sum -c SHA256SUMS`.

## Runtime capability replacement

| Go dependency or mechanism | C++17 replacement in this project | Boundary |
|---|---|---|
| Garbage collector and stack maps | `std::shared_ptr`, RAII, explicit ownership and weak child links | No moving GC or Go heap ABI |
| Segmented/user stacks and `morestack` | Explicit `Task` callable and `Frame` objects | No compiler-generated stack growth |
| `mcall`, `gogo`, `gopark`, internal scheduler ABI | Cooperative C++ worker loop with G/M/P state transitions | No assembly or asynchronous compiler preemption |
| `sudog`, futex and netpoller waits | Heap wait nodes, mutex/condition variables and bounded timer waits | No kernel netpoller integration |
| Runtime atomics | `std::atomic` plus transition mutexes where a state handoff spans fields | C++ memory model applies |
| cgo and Go internal ABI | No dependency; public C++ headers only | cgo interoperation is out of scope |
| Compiler-generated defer/panic calls | Explicit `panic_defer::Frame` registration and unwind protocol | Source transformation must create boundaries |

The only external link dependency is the platform C++ standard library and
POSIX thread support exposed by CMake's `Threads::Threads` target. No Go
runtime, cgo, bundled GC, or third-party scheduler backend is linked.
