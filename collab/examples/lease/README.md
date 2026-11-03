# lease 示例（JinShan REQ-005）

`main.cpp` 用 `include/go2cpp/relay/lease.hpp` 演示租约的三条关键语义：

- **到期恰好一次**：100 ms 租约，等 250 ms 后回调计数 == 1，再等 250 ms 仍 == 1；
- **Renew 延后**：构造后 60 ms 调 `Renew()`，再过 60 ms 未触发（原到期点已被重置），
  再过 120 ms（距 Renew 180 ms）触发一次；
- **Stop 后不再触发**：`Stop()` 连调两次（幂等），等 250 ms 回调计数仍为 0。

租约只用 `steady_clock`，回调在计时线程执行；示例用 `std::atomic<int>` 计数，
避免与回调的数据竞争。示例不做性能测量，只验证行为。

## 运行

```bash
collab/examples/lease/build_and_run.sh
```

脚本复用 `.build-bento-wsl/libgo2cpp_relay.so`，不改动任何 CMake 文件。
退出码 0 表示三条断言全部通过；整例约 0.9 秒。

## 依赖

- 头文件：`include/go2cpp/relay/lease.hpp`
- 链接：`-lgo2cpp_relay -lgo2cpp_context -lgo2cpp_error`（`-pthread`）
