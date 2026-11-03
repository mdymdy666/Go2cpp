# hub 示例（JinShan REQ-003）

`main.cpp` 用 `include/go2cpp/relay/hub.hpp` 演示 key → 连接注册表的完整生命周期：

- 注册 2 个 key（`room:a` / `room:b`），每个 key 各 2 条连接（`Hub::Stream` 即 `streams::Closer`）；
- `Lookup` 每个 key 返回 2 条、`Keys()` 返回 2 个 key；
- 同一连接对象重复 `Register` 返回 `false` 且表不变；
- `Unregister` 对同一连接连续调用两次不崩（幂等），表里只剩 1 条；
- `CloseKey` 返回 `true`、`Lookup` 立即为空、两条连接各被 `Close()` 一次；
- 对已关闭的 key 再次 `CloseKey` 返回 `false`，清空后 `Keys()` 为空。

## 运行

```bash
collab/examples/hub/build_and_run.sh
```

脚本复用 `.build-bento-wsl/libgo2cpp_relay.so`，不改动任何 CMake 文件。
退出码 0 表示全部断言通过。

## 依赖

- 头文件：`include/go2cpp/relay/hub.hpp`（自包含，继承 `streams::Closer`）
- 链接：`-lgo2cpp_relay -lgo2cpp_context -lgo2cpp_error`（`-pthread`）
