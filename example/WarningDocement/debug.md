# debug 使用限制

> 依据：`example/debug/debug_module_demo.cpp` + 实际实现；每条标题末尾给出 file:line。

## 1. dump CLI 的 argc 必须与数组元素个数一致 —— `example/debug/debug_module_demo.cpp:218-223`
早期写 6 而数组只有 5 个元素，`argv[5]` 越界读到 nullptr。
```cpp
const int rc = RunDumpCli(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv, ...);
```
**后果**：`basic_string: construction from null` 崩溃。
## 2. argv 字符串必须具名存活 —— `example/debug/debug_module_demo.cpp:210-216`
`("--output=" + path).c_str()` 的临时 string 语句结束即析构，argv 变悬垂指针。
```cpp
const std::string output_arg = "--output=" + path;
const char* argv[] = {"go2cpp", "dump", "scheduler", "--format=ini", output_arg.c_str()};
```
**后果**：运行期崩溃（悬垂指针）。
## 3. `--follow` 有批次上限 —— `example/debug/debug_module_demo.cpp:304-308`
`--follow=N` 解析为 `follow_batches`，不是无限跟随。
```cpp
ParseDumpCommand({"dump", "events", "--follow=2", "--follow-interval=0"});
```
**后果**：以为会持续跟随，实际到批次就返回。
## 4. SIGUSR1 handler 只置位 —— `example/debug/debug_module_demo.cpp:330-335`
信号处理器里不写文件，真正 dump 由 `PollOnce()` 完成。
```cpp
DebugSignalDumper::Install(scheduler, collector, "/tmp", &error);
// 业务循环里必须周期性 PollOnce()
```
**后果**：不轮询就永远不产生 dump 文件。
## 5. 默认 json 的 fibers 为 null —— `example/debug/debug_module_demo.cpp:293-295`
要枚举 G 必须显式 `DumpOptions.include_fibers = true`。
```cpp
DumpOptions opt; opt.include_fibers = true;
const auto snap = proxy.SnapshotWith(opt);
```
**后果**：默认导出看不到任何 Fiber。
## 6. Debug 代理是只读快照 —— `example/debug/debug_module_demo.cpp:8`
`DebugMutex` / `DebugConditionVariable` / `DebugWaitGroup` 只读观测，不改变调度行为。
```cpp
DebugMutex probe(mutex);   // 观测 Waiters()，不参与加锁
```
**后果**：拿它当同步原语会破坏语义。
## 7. 该示例注册为 ctest：必须快速结束 —— `example/debug/debug_module_demo.cpp:12-13`
失败时返回非零，且不能长时间挂起。
```cpp
return g_failures == 0 ? 0 : 1;   // 不要 sleep 等超时
```
**后果**：CI 超时或假失败。
