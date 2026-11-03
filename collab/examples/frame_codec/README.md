# frame_codec 示例（JinShan REQ-001）

`main.cpp` 用 `include/go2cpp/frame/frame_codec.hpp` 演示帧编解码的四类行为：

- 两种 `FrameSpec`：`RDRL`（max_payload 64 KiB）与 `RDCH`（4 MiB），帧头 12B + CRC32 4B；
- 半包：一帧拆成 1 字节 × N 次 `Feed`，每次 `consumed == 1`，最终拼出完整帧；
- 粘包：3 帧拼成一个 buffer，一次 `Feed` 返回 3 帧且 `consumed` 等于总长；
- 畸形：坏 magic / 坏 CRC / length 65537 越界，分别断言 `kBadMagic` / `kChecksumMismatch` / `kLengthExceeded`。

## 运行

```bash
collab/examples/frame_codec/build_and_run.sh
```

脚本复用 `.build-bento-wsl` 里已有的 `libgo2cpp_frame.a`（静态库）与 `libgo2cpp_hash.so`，
用 `-Wl,-rpath` 指向构建目录，不改动任何 CMake 文件。退出码 0 表示全部断言通过。

## 依赖

- 头文件：`include/go2cpp/frame/frame_codec.hpp`
- 链接：`-lgo2cpp_frame -lgo2cpp_hash`（另加 `-pthread`）
