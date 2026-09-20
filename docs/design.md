# Runtime Design

## Scope and contract

Go2Cpp is an independent C++17 runtime/library, not a translator. It targets
Linux with GCC 13 or Clang 18 and uses the standard C++ library, POSIX threads,
Linux epoll/eventfd for I/O, and Boost.Context for the replaceable stackful
Fiber backend. Public source/API names are under `go2cpp`; 0.x has no ABI
stability promise. The default build is shared and includes the Linux hook;
static builds must set `GO2CPP_BUILD_HOOK=OFF` so there is one process-wide
descriptor/TLS registry.

The runtime's own scheduling, panic, cancellation and waiting paths do not use
C++ exceptions, futures, promises, `setjmp` or `longjmp` as control flow. An
accidental exception thrown by user code is caught at the outer Fiber/body
boundary and translated to an unhandled panic result; ordinary C++ destructors
still run. A translator must explicitly create `panic_defer::Frame` boundaries.

## Ownership model

Tasks, channels, contexts, wait nodes and Fiber state use RAII and smart
pointers. A scheduler registry strongly owns every non-terminal Task, including
parked Gs; queue nodes and wait nodes hold `shared_ptr` references. A started
terminal Task is removed from the scheduler registry only after its Fiber
trampoline has returned; an unstarted terminal Task has no stack and may be
pruned during queue/admission cleanup. User-held Task handles may keep captures
alive longer. Context parents retain a strong parent anchor while child entries
are weak and pruned, so cancellation propagation does not form a cycle. Timer
callbacks capture weak wait nodes and all scheduler-facing wait nodes have a
gate/disarm step
before their owner returns.

The owning `Scheduler`/`IOManager` object must remain alive until all worker
threads and external member-callers have stopped. Destruction (and a final
`shutdown()`) must not run concurrently with `start`, `enqueue`, snapshots,
or other member calls; the reaper holds its join gate through retired-thread
joins, but it cannot make an object lifetime race safe. Destruction from one of
its own managed Gs fails fast, because a C++ destructor cannot safely defer the
owner object's lifetime.
Ordinary `Channel` member calls require the object to outlive every concurrent
caller and waiter; `~Channel()` is not a concurrent cancellation primitive and
must not run while a raw/stack channel call is active. `SelectCase` captures a
`ChannelPtr` when a shared channel is used, but a raw or stack channel cannot
self-retain.

## GMP scheduler

`Task` is G, `Machine` is an OS worker M, and each `Processor` is a P scheduling
token. A P local queue is bounded; overflow uses a global FIFO queue. Workers
prefer their local queue, then global work, then bounded victim scans from the
back. A task has exactly one scheduler owner, one `m_queued` queue claim and
one `m_execution_claim`; dequeue, runnable accounting and the running claim are
committed under the admission mutex. Thus a wake racing with dequeue cannot
publish a second execution of one G.

Legal G transitions are:

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled
waiting -> runnable | cancelled
dead/cancelled -> terminal
```

`yield` and `park` are valid only for the current G. The Fiber backend preserves
the C++ continuation, so a G resumes after the call instead of re-entering its
callable. A readiness, timer, channel or synchronization notifier either
transitions waiting to runnable or records one pending wake token while the G
is still running. Shutdown closes admission, cancels unstarted queue entries,
wakes started waiting Gs, and waits for started Fibers to return naturally.
Worker-initiated shutdown publishes the same drain request; once the registry is
terminal the workers transition to stopping and the owner thread joins them.

M management starts at min_workers, grows while runnable backlog and busy M
count demand it, and retires idle M records down to the floor. With
allow_worker_oversubscription=false, the configured maximum is strictly
clamped to P. With the default setting, ordinary runnable bursts remain
P-bounded; each active BlockingRegion contributes one replacement-M slot,
up to max_workers. This is an explicit declaration for a short native
blocking call, not asynchronous preemption or a full Go-style P handoff:
the blocked M keeps its P accounting token while the replacement M may
share that P. Retired records are joined/reaped, so repeated load waves do
not grow metadata without bound. Task class IDs provide a bounded soft
affinity scan and counters; they never prevent stealing or promise a
thread-local cache.

## Fiber and execution context

`go2cpp::Fiber` uses Boost.Context raw `fcontext_t`, a protected fixed-size
stack, and an explicit trampoline. It has `Ready`, `Running`, `Suspended`,
`Completed` and `Failed` states. Resume calls are serialized; errno is saved and
restored around every switch. A Fiber may migrate between M threads,
so normal thread_local values must not be treated as G-local state.
FiberLocalCache<T> (also available as FiberLocal<T>) stores a shared
value under the logical Fiber identity, survives migration, and releases
it after the Fiber trampoline completes. Calls from ordinary threads use
a normal per-thread fallback. This is a value-local facility, not a
reusable stack or Fiber-object pool; stack reuse remains a replaceable
backend concern. Transient cache objects on a long-lived Fiber intentionally
retain their key/value entry until Fiber completion, so stable cache instances
are preferred for hot paths. Destruction requests cancellation and resumes a
Ready/Suspended Fiber until it completes, preserving stack destructors and
defers. A Fiber that ignores cancellation can make its destructor wait; there is
intentionally no unsafe forced stack disposal.

`panic_defer::ExecutionContext` is carried by the Fiber and installed by a
short `Binding` around each resume. This replaces the thread-local-only state
used by simple callback runtimes and prevents panic/recover state leaking when
an M is reused by another G.

## Thread policy and native blocking

ThreadParticipationMode and ScopedThreadParticipation let an external thread
record whether it is eligible to cooperate with a Scheduler, while
ThreadHookMode and ScopedThreadHookMode override the process-wide socket hook
for that thread. Runtime-created workers mark themselves as managed workers
automatically. An external participation scope is deliberately policy metadata
only: it does not attach the thread as an M, run a scheduler queue, or provide
an external run_one loop.

BlockingRegion is the explicit bridge for a managed G that must call a known
native blocking function. It marks the current M as blocking and permits one
replacement M per active region, subject to max_workers. The RAII object is
non-movable and records the entering M so its destructor can repair that M's
state even if a misuse lets the Fiber migrate; nevertheless it must not span a
Fiber yield, park, or migration. Arbitrary unhooked blocking calls are still
not detected automatically.

`panic_defer::panic()` records runtime state but does not rewrite ordinary C++
control flow. Generated code must return through the active frame boundary
after recording a panic; arbitrary statements after `panic()` are outside the
supported Go-like subset.

## Context and timers

Context cancellation is an iterative work queue: each state is marked once,
its timer is removed, descendants are marked, and then `Done` callbacks are
signaled from the leaves back toward the parent. `DoneSignal` provides callbacks and `Wait`/`WaitFor`/`WaitUntil`;
when called by a managed G those methods use `core::ParkingCondition`, which
parks the Fiber and leaves the M available. Channel and synchronization waits
use the same parking primitive and a named process-wide `core::TimerService`.
The context module retains its own weak-state deadline service so injected
clock construction remains independent of the monotonic timer worker.

Typed Context keys use an identity token retained by the context node; values
are immutable `std::any` objects. A string-key overload is provided for dynamic
translated code. Error handles are immutable shared values; `Join` preserves a
singleton `JoinError` wrapper, while `Wrap` with a null cause returns null.
Cancellation never implicitly becomes a panic.

This subtree-first callback order lets a callback wait on a descendant without
deadlocking; it is a deliberate C++ extension rather than a promise of the
Go runtime's exact callback timing. A null parent maps to `Background()`.
Unlike Go's `Background().Done()`/`TODO().Done()` nil channel, a root here
returns a non-null `DoneSignal` that remains permanently unsignaled.
Callbacks should not synchronously wait on siblings or ancestors. The
context deadline service is intentionally process-lifetime so static Context
destruction can safely remove pending timers; an exit callback stops and joins
the timer thread while the intentionally retained service object remains valid.

## Channels and synchronization

`Channel<T>` uses a mutex-protected FIFO ring/deque, separate sender/receiver
wait queues and one-shot select states. A send/receive/close changes a
generation and wakes only the claimed node(s); close drains waiters, receives
after buffer drain report the zero value plus `ok=false`, and sends after close
return a closed status unless `SendOrPanic` is selected. `SelectWaitState`
prevents two cases or two unbuffered selects from claiming one operation.

An empty select returns an invalid status rather than blocking forever like Go's
`select {}`. Select probes/arms/disarm callbacks and select transfer copies are
expected not to throw; there is no rollback contract for a user type whose
copy/move constructor throws after a wait node has been claimed. Ordinary
blocking channel calls require the `Channel` object to outlive the call;
destruction is not a concurrent wake-up mechanism and cannot extend the
lifetime of a raw object.
Call `Close()` while the object is still alive when waiters must be released;
the destructor itself must run only after all member calls and waiters have
ended.

`sync::Mutex`, `ConditionVariable` and `WaitGroup` use FIFO wait nodes and the
same disarm gate. Managed calls release the external mutex before parking and
reacquire it on a normal wake.

An ordinary thread that contends on the same object waits on the wait node
native condition_variable; both caller classes share the FIFO handoff, and
unlock is intentionally not thread-owned. This avoids transferring ownership
of a std::mutex across a Fiber that may migrate after a hooked I/O wait.
lock() can throw logic_error only when a managed G cannot continue waiting,
for example during shutdown; an unmanaged thread may block until its timeout
or notification.
WaitGroup zero transitions are wave-based, so a new `Add` cannot consume an old
wave's notifications.

## Socket hook and wait protocol

The Linux hook is a shared C ABI interposer. It wraps `socket`/`socketpair`;
a Fiber owned by an IOManager uses epoll, while a managed Fiber on a plain
Scheduler lazily adopts the descriptor and uses bounded native poll fallback.
An ordinary thread falls through to the original libc function. Each managed
socket has an open description recording user-visible versus runtime
`O_NONBLOCK` and send/receive timeouts.
follows this shape:

1. Hold `DescriptorGuard` briefly, validate the live generation, and call the
   original syscall.
2. On `EAGAIN`/`EWOULDBLOCK`, release the guard and publish epoll readiness
   and the absolute-deadline index in one State-mutex transaction. The poller
   cannot expire that index until the readiness registration is visible, which
   is equivalent to readiness-before-timer even though both records are
   committed before the mutex is released.
3. Park the current G. Readiness, timeout, cancellation and close all use one
   atomic outcome claim; the wake path is idempotent and disarms its raw
   scheduler pointer before the waiter leaves.
4. Retry the original syscall with the same absolute deadline. `connect` also
   checks `SO_ERROR` after writability.

`DescriptorToken` generations and epoll registration IDs prevent stale events
from reviving a reused numeric fd. Hooked `close` invalidates the token and
broadcasts to every live IOManager before the real close. `dup2`/`dup3` preflight
the target close and broadcast it before the replacement syscall, then publish
cloned metadata only after success; deterministic no-op/error forms are passed
through without a false close notification. Unknown variadic command contracts
return `ENOTSUP` rather than reading an argument of an unknown ABI type. The
hook intentionally does not claim to intercept every Linux socket-adjacent API
(`poll`/`select`, `sendfile`, `splice`, `*mmsg`, `io_uring`, `close_range`,
raw/glibc no-cancel entry points and similar helpers remain outside this
subset). This is a deliberate safety boundary.
`MSG_WAITALL` is passed through to libc for ordinary threads, but returns
`ENOTSUP` in a managed Fiber because this bounded hook does not accumulate
partial reads. Managed urgent-data receives (`MSG_OOB`) likewise return
`ENOTSUP`; `EPOLLPRI` is intentionally not registered by this subset. Direct
raw syscalls, fork/exec descriptor state and metadata allocation failures are
outside exact transparent semantics; metadata allocation failures are caught
so a successful syscall remains usable. `ioctl(FIONBIO)` lets the kernel
validate the caller pointer first, preserving `EFAULT`, then reconstructs the
requested state from the raw file flags while restoring runtime nonblocking.
Readiness deadlines are rounded up to the poller's millisecond granularity;
native-thread fallback uses bounded millisecond slices, so sub-millisecond
timeouts are lower-bounded by that scheduling granularity.
Known interposed calls that fall back to a native syscall or poll use invoke_native_blocking and automatically publish M::Blocking; arbitrary unhooked native calls still require an explicit BlockingRegion.

## Extension boundaries

The default scheduler, Fiber backend, timer service, channel storage and epoll
manager are concrete implementations behind public result/state contracts.
SchedulerConfig, TaskOptions, BlockingRegion, FiberLocalCache<T>,
thread-policy scopes, Context::SetNowFunctionForTesting,
`SelectCase`, `DescriptorGuard`/tokens and the C hook controls are the supported
replacement/embedding boundaries. There is no binary plugin ABI before 1.0;
replacement implementations must preserve state transitions, one-shot wake
claims, ownership and shutdown contracts.

## Sylar clean-room audit

The local Sylar2 files were consulted as design references only:
`/UserData/CodexWorkSpace/sylar2/sylar/fiber.cc`, `scheduler.cc`,
`iomanager.cc` and `hook.cc` (the requested `IOManager.cc` is named
`iomanager.cc` in that checkout). The checkout has no project license file and
is not a Go2Cpp dependency; its source is not copied or built here. The audit
identified useful concepts (stackful swap, scheduler queues, epoll readiness,
readiness-before-timer hook ordering) and boundaries that needed stronger
contracts in this project: suspended-stack destruction, raw manager pointers
on close, fd-generation ABA, timer/readiness races, and incomplete variadic
hook handling. Go2Cpp addresses those with raw Boost.Context ownership,
weak/gated callbacks, generation tokens, one-shot outcomes and explicit
`ENOTSUP` for unknown command shapes.

## Boundary API usage

The recommended mixed-mode pattern is to use the runtime synchronization
classes for state shared by Fibers and ordinary threads. Do not hold a
thread-owned std::mutex across a Fiber yield or hooked I/O operation: the
Fiber may resume on another M and unlocking that native mutex from a different
thread is undefined behavior. Use sync::Mutex (or release the native lock
before yielding) and pair every wait with a timeout or cancellation policy.

Use ScopedThreadHookMode(kDisabled) only for a deliberately native call; this
scope changes hook admission for the current thread and does not change G/M/P
ownership. Use BlockingRegion around a known native blocking call from a
managed G. The region is RAII and non-nestable, and it must end before the
Fiber parks or yields.
