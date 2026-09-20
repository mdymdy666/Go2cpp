# GMP Scheduler Contract

## Data and ownership

| Record | C++ object | Owner and invariant |
|---|---|---|
| G | `scheduler::Task` | Strongly retained by the scheduler registry while non-terminal; one `m_execution_claim`; one queue claim `m_queued`; first admitting scheduler owns it permanently |
| M | private `Machine` plus `std::thread` | One worker thread, one preferred P, dynamically admitted/retired under the scheduler mutex |
| P | private `Processor` | Bounded local queue and active-M count; M records may be overcommitted only for declared BlockingRegion replacement workers |

The transition table is:

```text
new -> runnable -> running -> runnable | waiting | dead | cancelled
waiting -> runnable | cancelled
dead/cancelled -> terminal
```

The worker-state transitions exposed by snapshots are:

```text
M: idle -> running -> idle -> parked -> idle
                 \-> stopping -> dead
    parked -> stopping -> dead
P: idle <-> running; idle -> dead during shutdown
```

A declared BlockingRegion publishes M::blocking around a known native
blocking call and asks the scheduler to admit a replacement M when queued work
exists. P remains a scheduling token owned by the blocked M; the replacement
M may share that P, so this is bounded overcommit rather than an exact Go P
handoff. A retiring or shutdown worker publishes M::stopping before M::dead.

Queue insertion, removal, runnable accounting, shutdown admission and the
`runnable -> running` claim are serialized by the scheduler admission mutex.
The task transition mutex protects state/claim fields. Code never destroys a
user capture while holding a scheduler, P, queue or completion lock.

The queue/claim invariant is:

* a runnable G has either one queue/rescue node (`m_queued=true`) or is in the
  admission handoff; it is never present in two queues;
* a running G owns `m_execution_claim=true` and is absent from all queues;
  dequeue clears `m_queued` and claims execution in one transition;
* `runnable_count` counts admitted queue/rescue nodes, while a pending wake
  token does not count as runnable work; terminal Gs have no queue or execution
  claim.

The short admission window may expose an intermediate field value only while
the scheduler mutex is held; workers and notifiers cannot observe that window.

## Wake and park protocol

Only the current G may call `yield` or `park`. A notifier seeing `waiting`
changes it to `runnable` and enqueues it. A notifier racing with a running G
sets one pending wake bit; `park` consumes that bit and keeps the continuation
running. A wake racing with shutdown either enters the queue/rescue list or is
observed as cancellation. Wait nodes use a separate wake gate so `disarm()`
waits for an in-flight callback before a scheduler pointer can become invalid.

Each worker admission resumes the Fiber continuation; repeated admissions
continue it until the callable returns. The scheduler does not call the
callable again after a yield; the continuation resumes at the instruction
after the switch. A direct `Fiber::Suspend` is mapped to runnable for `Yield`
and to waiting for other reasons, except that a concurrent wake/cancellation is
converted to a runnable continuation so shutdown cannot strand the G between
the context switch and state publication. Raw Fiber suspension remains an
advanced API and must still return cooperatively.

## Dynamic M policy

min_workers is the retained floor. Ordinary queued work grows the pool only
up to the P-bounded ceiling. When allow_worker_oversubscription is enabled,
each active BlockingRegion contributes one replacement-M slot, up to the
configured max_workers. Setting it false restores a strict P ceiling. Idle M
records wait idle_worker_timeout, reserve retirement under the admission
mutex, and decrement both global and P-local active counts exactly once. Dead
records are joined outside scheduler locks and removed from snapshots. Task
class IDs are a bounded scan preference with hit/miss counters; stealing
remains available, so class locality cannot starve other work.

The replacement slot is opt-in and scoped. BlockingRegion is non-movable
and records the entering M for cleanup; it still must not span a Fiber
yield/park or migration. The scheduler cannot detect an arbitrary native syscall
or force-preempt a C++ continuation. The interposed
socket and sleep fallbacks use the same blocking accounting automatically;
unhooked native calls need an explicit BlockingRegion and must not span a
Fiber yield or park. External ScopedThreadParticipation only records
per-thread eligibility and Scheduler association; it does not attach an M or
run a queue.

## Shutdown and join

Shutdown is one-way. It closes admission, cancels unstarted queued Gs, marks
started Gs for cooperative cancellation, wakes parked Gs, and waits until every
registered G is terminal. Worker-initiated shutdown only publishes the drain
request; once the last G is terminal, a worker marks the scheduler stopping and
exits without joining itself. The owning thread performs the final joins.

`Task::wait`/`Join` and timed variants use `core::ParkingCondition` when called
inside a managed G, so a one-P scheduler can run the joined child. Unmanaged
callers use the same predicate with a native condition-variable fallback.
Self-join and a cancelled managed waiter return false. Destroying a suspended
Fiber is a cancellation request followed by natural completion; a body that
never reaches a cooperative/return boundary can therefore make join or
shutdown wait, which is preferable to skipping C++ RAII.

## Diagnostics and tests

`processors()` and `machines()` expose snapshots only; they do not grant queue
ownership. Tests cover P=1 and multi-P execution, duplicate enqueue/wake races,
cross-scheduler rejection, parked cancellation, raw Fiber park cancellation,
cancelled queue capture release, dynamic growth/shrink/regrow, strict P caps, BlockingRegion replacement-M growth,
task-class counters, worker shutdown, task destructor re-entry and
watchdog-bounded stress.
