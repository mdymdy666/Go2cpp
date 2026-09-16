# Compatibility matrix

| Go feature | Implemented subset | Deliberate difference / boundary | Verification |
|---|---|---|---|
| G/M/P scheduling | Configurable P count, worker M threads, local/global queues, stealing, park/unpark with a pending-wake handoff, single-scheduler G ownership, cancellation and shutdown | No async stack growth or transparent preemption of blocking native calls; fairness is best effort | scheduler unit/invariant, wake-race regression, ownership rejection, stress and demo |
| `context` roots | `Background`, `TODO` | Roots are process-lifetime shared handles and ignore direct cancellation | context tests |
| `WithoutCancel` | Not included in the 0.x API | Use a new `Background`/`TODO` root explicitly; preserving values while detaching cancellation needs a separate policy | documented non-goal |
| `WithCancel` | Idempotent cancel, parent propagation, Done event, error/cause | Cause is an explicit optional C++ error handle | context tests |
| `WithDeadline`/`WithTimeout` | Steady-clock timer, deadline/error propagation, synchronous expiry for past deadlines | A shared timer service owns deadline entries; injected clock affects timeout calculation, not the service's monotonic wait | context deadline tests |
| `WithValue` | Typed key template, string keys (including empty), and immutable lookup | Keys are C++ type/identity objects, not Go interface values | context value tests |
| `defer` | Registration-time capture, LIFO, normal/panic unwind | Requires explicit `Frame` boundaries; no compiler transformation | panic/defer tests |
| `panic`/`recover` | Per-task state, nested frames, re-panic replacement, recover boundary, C++ body-throw translation | No C++ exceptions/longjmp as runtime control flow; explicit frames are required. C++ cannot enforce Go's lexical "direct deferred function" rule for a helper called from an active defer, and a C++ throw during explicit `Frame` destruction cannot be recovered by that frame | panic/defer tests |
| `panic(nil)` | Distinct nil payload marker | Exact Go runtime version-specific formatting is not reproduced | panic tests |
| `error` | Immutable values, wrapping, `Unwrap`, iterative cycle-safe `Is`/`As`, iterative release for built-in deep wrapping, `Join` | No Go interface/typed-nil ABI; null shared handle is nil. Built-in/custom message formatting still assumes an acyclic description chain | error tests |
| channels | Typed buffered/unbuffered, FIFO, blocking, close, multi-producer/consumer, cancellation | Send-after-close reports status; caller may request panic explicitly | channel tests |
| directional channels | `AsSendOnly` and `AsRecvOnly` views expose only the permitted operation set | Views are source-level C++ wrappers; the originating bidirectional `ChannelPtr` can still be retained by code that owns it | channel compile/use tests |
| nil channel | Invalid handle detected and reported | Operations do not block forever as Go nil channels do | channel edge tests |
| `select` | Ready/default and timed/context-aware cases, round-robin ready-case rotation, armed wait nodes, close/cancel wake-up, and unbuffered select-to-select rendezvous | No compiler syntax; API is builder/case based and wake-up latency is bounded by the wait interval. Channel element types used by `RecvCase`/`SendCase` must be copyable because probes use `std::any`; a closed `SendCase` reports status rather than implicitly panicking; a nil case reports `kNil`. When no channel case is ready, the supplied context is checked before `DefaultCase`. Select probes ready channel cases before checking cancellation; direct `Send`/`Recv` check an already-done context first. | channel select tests |

The supported API is a semantic compatibility layer, not a claim of full language equivalence. Any unsupported behavior should be surfaced as a status/error or documented precondition.
