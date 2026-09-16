# GMP scheduler module

The scheduler is a small, independent C++17 implementation of the part of
the Go G/M/P model that can be represented without compiler-generated stacks.
`Task` is the G record, `Machine` is a worker OS thread, and each processor
owns a local run queue.  A bounded local queue spills to the scheduler's
global FIFO queue; an idle worker steals from the back of another processor's
queue.

## State and ownership

Only the following transitions are accepted:

```text
new       -> runnable | cancelled
runnable  -> running | cancelled
running   -> runnable | waiting | dead | cancelled
waiting   -> runnable | cancelled
dead      -> (terminal)
cancelled -> (terminal)
```

An unhandled panic (or a C++ exception translated at the goroutine boundary)
may finalize a task that has already marked itself runnable/waiting; the
worker clears deferred requeue state before entering `dead`.

`Task::m_queued` is a single queue-membership claim. A worker removes a node
and claims `m_queued=false`, `m_execution_claim=true`, and `runnable -> running`
under one scheduler-admission transaction. A task is permanently bound to the
first scheduler that admits it; attempts to enqueue or wake it through another
scheduler are rejected without changing its state.
`Task::m_execution_claim`
is a separate run claim, so an external wake or yield cannot cause two workers
to enter one callable concurrently.  Queue insertion and shutdown admission
are serialized by the scheduler mutex. Queue nodes own `shared_ptr<Task>`;
workers never retain a raw task pointer after releasing that ownership. The
scheduler registry stores weak task entries only while a G is live; worker
completion, admission, and shutdown prune `dead`/`cancelled` entries even when
the caller retains a terminal `Task` handle, so registry metadata does not
grow with the lifetime of a long-running scheduler.

An M holds one preferred P for its lifetime. `max_workers` is clamped to the
configured P count, so each P has at most one worker M in this backend. `yield`
and `park` are cooperative operations available only to the currently
executing G; an external caller must not mutate a running G because this
backend has no resumable C++ stack. `wake` transitions a waiting G directly;
when it races with a running G's `park`, it records a pending wake token that
`park` consumes and hands back to the worker loop. A native blocking call
inside a task still occupies its M until it returns.

## Shutdown

Shutdown closes admission, marks all queued tasks cancelled, and wakes parked
workers. A worker-initiated call publishes the stop request and returns without
joining any M, preventing concurrent worker callers from joining each other;
the owning external thread performs all joins. A task already executing on an
M is allowed to return; a task removed concurrently but not yet claimed is
cancelled. Shutdown is one-way and idempotent. The scheduler object must stay
alive until every worker has returned; a worker may request shutdown but must
not destroy its owning scheduler. Final destruction belongs to the external
owner after the join.

The implementation deliberately does not provide segmented stacks,
compiler-level asynchronous preemption, or transparent detachment around
blocking system calls.  Those facilities require a compiler/ABI integration
that is outside this standalone compatibility library.
