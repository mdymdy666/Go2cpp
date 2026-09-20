# Runnable example

`example/runtime_demo.cpp` is the executable end-to-end example. It creates a
G/M/P scheduler, passes a value through a typed channel, binds a timeout
context, demonstrates `WithValue` and explicit child cancellation,
demonstrates `select` with send/receive-only channel views, formats and
identity-checks a wrapped error, and demonstrates an explicit defer/recover
boundary. Build it
with:

```sh
cmake -S . -B build -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build build --target go2cpp_runtime_demo
./build/go2cpp_runtime_demo
```

For boundary cases involving ordinary threads and managed Fibers, run `example/mixed_runtime_demo.cpp`.

```sh
cmake --build build --target go2cpp_mixed_runtime_demo
./build/go2cpp_mixed_runtime_demo
```

The example uses `sync::Mutex`, `ConditionVariable` and `WaitGroup` from both sides
of a shared wait queue, checks that `FiberLocal` state survives a cooperative
yield, scopes hook participation per native thread, and declares a short
`BlockingRegion` so a replacement M can service queued work. External
`ScopedThreadParticipation` is intentionally a policy/eligibility scope; it
does not attach that native thread as a scheduler worker.
