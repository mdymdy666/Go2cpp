# relay_pipe 示例（JinShan REQ-002）

`main.cpp` 用 `include/go2cpp/relay/relay_pipe.hpp` 演示双向字节中继：

- 用两对 `socketpair` 造两条字节流，把 relay 侧 fd 包成 `relay::Stream`（Read/Write/Close）；
- `RelayPipe` 同时搬 A→B 与 B→A 各 8 MiB（共 16 MiB 流量）；
- 两端分别对收到的字节流算 SHA-256，与发送端的 `Sha256::Digest` 逐字节一致；
- `Stats()` 断言 `a_to_b == 8388608`、`b_to_a == 8388608`，并打印 `dropped`/`timeouts`（本例应为 0）。

写端在数据写完后 `shutdown(SHUT_WR)`，让 relay 读到干净 EOF 而不是等写超时；
读端按固定 8 MiB 读满即返回，不依赖两端关闭顺序。

## 运行

```bash
collab/examples/relay_pipe/build_and_run.sh
```

脚本复用 `.build-bento-wsl` 的 `libgo2cpp_relay_pipe.so` / `libgo2cpp_hash.so` 等，
不改动任何 CMake 文件。退出码 0 表示字节数与 SHA-256 断言全部通过。

## 依赖

- 头文件：`go2cpp/relay/relay_pipe.hpp`、`go2cpp/hash/sha256.hpp`
- 链接：`-lgo2cpp_relay_pipe -lgo2cpp_hash -lgo2cpp_context -lgo2cpp_error`（`-pthread`）
