# Go upstream reference snapshot

The compatibility work was checked against Go `go1.23.0`, released 2024-08-13.

- Upstream repository: <https://go.googlesource.com/go>
- Tag: `go1.23.0`
- Release source archive: <https://go.dev/dl/go1.23.0.src.tar.gz>
- Archive SHA-256: `42b7a8e80d805daa03022ed3fde4321d4c3bf2c990a144165d01eeecd6f699c6`
- Release/tag commit (source mirror): `6885bad7dd86880be6929c02085e5c7a67ff2887`
- License: BSD-3-Clause; the upstream `LICENSE` is retained beside the extracted files.

Only the files relevant to this compatibility layer were extracted from the
source archive. They are reference material and are not compiled or linked.
The runtime implementation is an independent C++ work and does not copy Go's
internal ABI, assembly, stack maps or garbage collector.

Relevant upstream files:

- `src/runtime/proc.go`, `src/runtime/runtime2.go`
- `src/runtime/chan.go`, `src/runtime/chan_test.go`
- `src/runtime/panic.go`, `src/runtime/panic_test.go`, `src/runtime/defer_test.go`
- `src/context/context.go`, `src/context/context_test.go`

The complete Go source tree is expanded locally at `../go1.23.0-full/` (13,222 files) and is ignored by Git.
