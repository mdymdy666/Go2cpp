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
