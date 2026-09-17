# Go2Cpp runtime design

## Scope and build contract

Go2Cpp is an independent C++ runtime/library. It does not embed Go, cgo, a Go garbage collector, compiler-generated stack maps, or a Go internal ABI. The baseline is C++17, GCC 13 or Clang 18 on Linux, with the C++ standard library and POSIX threads. Public headers are stable at the source/API level for the 0.x series; ABI stability is not promised before 1.0.

The implementation never throws C++ exceptions for runtime control flow and never uses `setjmp`/`longjmp`. Panic propagation is an explicit cooperative protocol at runtime call boundaries. Ordinary C++ destructors therefore run normally. User callbacks should return from a boundary after requesting a panic; code that continues doing work after `panic()` is outside the supported subset.

## Ownership and concurrency

Runtime objects are owned by `std::shared_ptr` handles or RAII value wrappers. Context parents retain weak child entries and prune expired entries during child creation/cancellation. Scheduler G objects are reference-counted task records; each G is bound to the first Scheduler that admits it, is enqueued at most once while runnable, and has one owner M while running. Channel waiters are heap nodes owned by the operation until wake-up and removed under the channel mutex. No API returns a reference whose lifetime depends on an internal lock.

The default clock is `std::chrono::steady_clock`; `Context::SetNowFunctionForTesting` can inject the clock used to calculate new timeout deadlines and synchronous-expiry checks. The timer service itself uses the monotonic steady clock and is shared by all timed contexts. Notifications happen after a state transition protected by the corresponding state lock; wait predicates recheck terminal state, and channel cancellation is rechecked at a bounded wait quantum, preventing permanent lost wake-ups while keeping the backend portable.

## GMP scheduler model

- **G** (`Goroutine`): one callable task, lifecycle state (`new`, `runnable`, `running`, `waiting`, `done`, `cancelled`), and an atomic run claim.
- **M** (`Machine`): one worker OS thread. An M may execute only one G at a time and is bound to one P while executing.
- **P** (`Processor`): a scheduling token with a bounded local deque. The scheduler owns exactly `P` processors; an M must hold a P before taking a G.

Each P has a mutex-protected FIFO local queue. A global FIFO queue handles overflow and wake-up. Idle workers steal from the back of another P's queue. Under the scheduler admission mutex, a worker removes one queue node and invokes a single Task transition that clears the queued reservation and sets the execution claim plus `runnable -> running` state atomically. An external enqueue therefore sees either the reservation or the execution claim and cannot publish a duplicate while the callable is active; the runnable counter is decremented in the same queue-admission transaction. Only the currently executing G may call `yield` or `park`; an external `wake` transitions a waiting G immediately, or records a one-bit pending wake when the G is still running. `park` consumes that token during the running-to-waiting handoff and requeues the G, so a wake racing with park cannot be lost. This prevents a second queue publication while a C++ callable is still executing. Blocking user work should call `yield`/return; an arbitrary blocking syscall cannot be transparently detached from its M in this subset. Shutdown closes admission, wakes all workers, drains queued tasks as cancelled, and joins workers.

The state transition invariant is:

```
new -> runnable -> running -> (runnable | waiting | done | cancelled)
waiting -> runnable | cancelled

Unhandled panic terminal edge: running | runnable | waiting -> dead
```

An unhandled panic or translated C++ exception is a terminal event and may
force an executing task from `runnable`/`waiting` bookkeeping to `dead`; this
path clears deferred requeue state. There is no transition out of `dead` or
`cancelled`; only `runnable` tasks may appear in a queue.

This backend does not preserve a C++ call stack across `yield` or `park`.
Those calls change the logical G state and the worker requeues the task after
the current callable returns. A task that blocks in an arbitrary native call
therefore retains its M until that call returns; it is not a transparent
stackful-fiber suspension. C++ RAII locals created by the callable are cleaned
when that callable returns or unwinds. Captures held by the callable remain
owned by the `Task` until its last external/runtime owner releases it, even
after the G state is terminal. The runtime cannot force-unwind a non-returning
callable or invent a continuation for it.

The `Scheduler` object is the external owner of the worker table and must
outlive every worker thread. A task may call `shutdown()` to publish the stop
request, but destroying the scheduler from one of its own workers is not
supported; the owner thread performs the final join and destruction after the
workers return.

The M table is allocated once by `start()`; `max_workers` is clamped to P
count and is not a dynamic growth target. Local/global queues and stealing
balance queued work, but there is no blocked-M replacement, idle-thread
retirement, or task-class-aware worker reuse. A future dynamic M manager must
keep P execution exclusive while allowing an M blocked outside the scheduler
to release its P, bound the total M count and idle cache, and verify scale-up,
scale-down, shutdown, and load balance under bursty and mixed-class loads.
Task-class locality should be a soft affinity that never prevents stealing or
starves other classes; no task-class identifier exists in the current API.

## Context

`Background` and `TODO` are immortal roots and ignore direct cancellation. `WithCancel`, `WithDeadline`, and `WithTimeout` create child nodes. Cancellation is idempotent, propagates parent-to-child through an iterative work queue, closes a shared `Done` event exactly once, and records `Canceled` or `DeadlineExceeded` plus an optional cause. `WithValue` stores a type-erased value behind a typed key identity; values are immutable after construction. The compatibility string-key overload also permits the empty string as a real key. A child holds a strong parent anchor so cancellation and values remain available even if the caller drops its parent handle. Parent child entries are weak and pruned, so this ownership direction does not form a cycle; a custom state deleter releases very deep parent chains iteratively.

## Channels and select

`Channel<T>` supports capacity zero (rendezvous) and capacity N (FIFO ring buffer), multiple producers/consumers, close, and cancellation-aware operations. A receive after close returns the type's default value with `ok=false`; a send after close returns a `SendResult::closed` status and can be promoted to a panic by the caller. Repeated close returns an error. Nil-channel behavior is represented by an invalid handle and is explicitly reported rather than hanging forever. `AsSendOnly` and `AsRecvOnly` create source-level directional views that omit the opposite operation set; the views can also be used to build matching select cases. `select` provides a non-blocking/default form and a bounded wait form over send and receive cases, with an optional context and timeout supplied to the select call. There is no separate `TimeoutCase` object. Ready cases are rotated with an atomic round-robin cursor; this is deterministic fairness rather than cryptographic randomness. Cases that are not immediately ready publish heap wait nodes under the channel mutex. A one-shot `SelectWaitState` claims exactly one case, pairs two independent unbuffered selects atomically, and is disarmed on every exit; close and context callbacks wake the state. Because `SelectProbe` is type-erased with `std::any`, channel element types used by select cases must be copyable; direct channel operations support move-only values. A nil select case reports `kNil`, and when no channel case is ready the supplied context is checked before `DefaultCase` by policy. The wait loop still uses a short bounded condition-variable interval for custom probe cases, so this is not a netpoller-grade wake-up path.

## Socket I/O and hooks

The current library has no socket wrapper or hook registry. In particular,
there is no public `IOManager`, FD readiness backend, `addEvent`/
`cancelEvent`, or `wait_for_event` implementation. Socket operations are
therefore outside the supported subset and must not be described as
non-blocking G waits. A future I/O module is expected to expose a syscall hook
contract and scheduler adapter: readiness is registered before a timeout, the
wait state owns an idempotent ready/timeout/cancel transition, and FD close,
`EINTR`, `EBADF`, nonblocking mode, and original-syscall fallback are
specified. The hook must resume a valid continuation or return an explicit
status; it must never leave a G suspended after a timer/readiness race.

The intended default is transparent interception, not only opt-in socket
wrapper classes. The hook surface must cover at least `connect`, `accept`,
`read`/`readv`/`recv*`, `write`/`writev`/`send*`, `close`,
`fcntl`, `ioctl`, and socket timeout options. Calls made outside a managed
G, calls on unsupported descriptors, and explicitly user-nonblocking calls
must fall back to the original libc/syscall behavior. An FD registry must
separate runtime nonblocking state from user-visible nonblocking state, retain
send/receive timeouts, and use a generation/token so close plus descriptor
reuse cannot wake the wrong waiter. `EINTR` is retried according to the
operation contract; `EAGAIN` registers epoll readiness before the timer and
parks the current Fiber; `connect(EINPROGRESS)` resumes on writability and
checks `SO_ERROR`. `close` invalidates the generation and cancels all
registered waits. This transparent call/return shape requires a resumable
stackful Fiber (or an equivalent compiler-generated continuation) and cannot
be implemented correctly by the current logical re-entry task model alone.
The acceptance suite must include socketpair readiness, a zero/short timeout
racing registration, readiness-versus-timeout/cancellation, concurrent close
and FD-number reuse, user-nonblocking fallback, `EINTR`, `EBADF`, and
leak/race instrumentation. No such tests exist in this release.

## Coroutine synchronization

There are no thread-like coroutine `Mutex`, `ConditionVariable`, `Fiber`, or
`WaitGroup` APIs in this release. The existing condition variables protect
runtime data structures and may block an OS thread. Adding these facilities
requires either a stackful-fiber backend (kept behind a replaceable executor
interface) or an explicitly callback/state-machine API; a blocking
`std::condition_variable` wrapper would not provide coroutine semantics.

## Defer, panic and recover

`Frame` owns a LIFO defer stack. Arguments are captured by value when `defer_call` is registered. `Frame::finish` runs defers on normal return. `panic` marks the current goroutine's panic state; `Frame::unwind` runs defers in reverse order. A defer can call `recover` only while it is the active frame during that unwind; the first successful recover clears the panic and returns its payload. A panic in a defer replaces the current panic (re-panic) and unwinding continues. `run` translates an accidental C++ exception from a body into an unhandled panic after preserving the outer runtime frame path. A C++ exception thrown while a user-created `Frame` is itself being destroyed is already in C++ stack unwinding; that frame finishes normally and cannot recover the later translated panic. Such code must call `panic()` explicitly when it needs Go-style recovery. Unhandled-panic observers are isolated from accidental C++ exceptions. Panic state is thread/goroutine local and is never recoverable across scheduler tasks. `panic(nil)` is represented by a non-null `PanicValue` with `is_nil=true`, so it remains distinguishable from "no panic".

## Error boundary

Errors are immutable `shared_ptr<const Error>` values. A null handle is the only nil error; typed-nil is not manufactured by the library. `Wrap` and `Join` retain causes, `Unwrap` exposes a single cause, and `Is`/`As` walk the chain with an explicit worklist (cycles and very deep generated chains do not consume the native call stack). Built-in `WrappedError` formatting and ownership release are iterative for deep chains. Custom error graphs must remain acyclic for ownership reclamation; a shared-pointer cycle cannot be collected. `AsMutable` exists only as an explicitly unsafe adapter escape hatch and must not be used for concurrently shared errors. Errors are ordinary results and are never implicitly converted into panic requests. Context cancellation uses an error value and channel operations return status/result objects. Arbitrary custom `Message()` implementations are expected to describe acyclic chains; callers needing hostile/cyclic custom formatting should use `Is`/`As` rather than recursively formatting the message.

## Non-goals and explicit differences

The library does not provide segmented/user-mode stacks, asynchronous compiler-level preemption, exact Go scheduler fairness, reflection-visible goroutine identities, garbage collection, Go interface ABI compatibility, or automatic panic transfer through arbitrary C++ call frames. These require compiler/ABI/GC integration and are recorded rather than simulated unsafely.

Go runtime facilities are replaced as follows: GC and stack maps use `shared_ptr`, RAII and explicit frame ownership; `mcall`/`gogo`/`gopark`/`morestack` use the C++ worker loop and cooperative task states; `sudog`, futexes and timer waits use heap wait nodes, mutex/condition-variable notifications and the shared timer service. There is intentionally no netpoller/socket hook replacement yet. cgo, sysmon, assembly ABI details and exact traceback generation are outside this library.

## Extension and plugin boundary

The 0.x package intentionally exposes source-level extension points rather than a binary plugin ABI. `SchedulerConfig` is the registration contract for the default executor (P count, worker bound, queue limit and idle wait), while `Context::SetNowFunctionForTesting` injects a clock for deterministic timeout construction. `SelectCase` accepts custom non-blocking probes, which can bridge another wait backend for select only; it is not a syscall/socket hook. A future I/O hook must be registered through an explicit adapter contract and preserve the wait-state result/idempotence rules above. The scheduler worker loop, timer service and channel storage backend remain private implementation details; replacing one requires an adapter at these public boundaries and must preserve the state/result contracts in this document. A future plugin ABI is out of scope until the library has a 1.0 ABI policy.
