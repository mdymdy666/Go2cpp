# sdl_render：真实 SDL 窗口 + vsync 渲染延迟验证

本目录补上 `docs/reports/vs-webrtc.md` §4.3 指出的**最大未验证项**：
媒体链路的 0.16–0.72 ms 是 **headless 校验和**口径，不含
`SDL_UpdateTexture` + `SDL_RenderCopy` + `SDL_RenderPresent` + 60 Hz vsync。

程序把整条链路真正跑一遍：

```
X11 窗口绘制（XPutImage，真实 X11）
  → XGetImage 采集
  → delta-I420 编码
  → Bento VideoChannel 1200B 分片
  → UDP 127.0.0.1 回环
  → 重组
  → delta 解码
  → I420→BGRA
  → SDL_UpdateTexture
  → SDL_RenderClear + SDL_RenderCopy
  → SDL_RenderPresent（真实窗口，opengl 渲染器 + PRESENTVSYNC）
```

## 构建与运行

```bash
# 0) 主工程（媒体库 + Bento core）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
cmake --build .build-bento-wsl -j"$(nproc)"

# 1) 一键（构建 + 跑基准 + PPM→PNG + 回归测试）
bash examples/sdl_render/build_and_run.sh

# 2) 或分步
cmake -S examples/sdl_render -B .build-sdl-render -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build-sdl-render -j"$(nproc)"
DISPLAY=:0 .build-sdl-render/sdl_render_bench --artifacts examples/sdl_render/artifacts
.build-sdl-render/sdl_test_render
```

需要 WSLg（`DISPLAY=:0`）与 SDL2。无 DISPLAY 时基准只跑 `headless-fixed`，
测试整体 SKIP（退出码 0）。

## 场景

| 场景 | 节拍 | 说明 |
|---|---|---|
| `headless-fixed` | 60 Hz | 只做 I420 校验和（vs-webrtc 报告的原始口径） |
| `sdl-novsync-fixed` | 60 Hz | 真实窗口 + 纹理上传 + present，**关闭 vsync** |
| `sdl-vsync-throttled` | 60 Hz | 同上 + vsync；采集由 60 Hz 节拍驱动 |
| `sdl-vsync-continuous` | 不节流 | 连续采集 + present，由 vsync 自身背压（最坏等待） |
| `sdl-vsync-random` | 随机相位 | 采集时刻在帧周期内随机（等价相机/网络帧与 vblank 无关） |
| `present-only` | 不节流 | 无媒体链路，纯 SDL present（量实际 swap 周期） |
| `present-phase` | 随机相位 | 无媒体链路，随机相位 present（相位模型证据） |

每个场景记录：

- `capture_ns` → `SDL_RenderPresent` 返回的 **p50/p90/p99/max**；
- 逐段耗时：采集、编码、分片、sendto、收+重组、解码、I420→BGRA、上传、Clear+Copy、Present；
- **vsync 等待占比** = present / e2e；
- **帧率与帧间隔**（是否被锁到显示刷新率）；
- **离下限**：e2e − 16.667 ms（标称 60 Hz）与 e2e − 实测 swap 周期；
- 跨场景逐帧 I420 校验和一致性（证明 SDL 路径没有改变渲染输入）。

## 像素验证

`ValidateSdlPixels` 在计时循环之外单独做一次：

1. 真实 X11 窗口绘制 → `XGetImage` 采集；
2. `ToI420` → `I420ToBgra` 得到参考 BGRA；
3. 上传纹理 → `SDL_RenderReadPixels` 读回；
4. **逐字节 memcmp**，并落盘 `artifacts/sdl_validation_{capture,reference,readback}.ppm`。

`test_sdl_render` 另外回归：X11 `XPutImage→XGetImage` 往返逐字节一致、
不同图案的采集 checksum 变化、readPixels 与参考一致、vsync 开/关的 present 耗时差异。

## 产物

```
examples/sdl_render/artifacts/
  sdl_validation_capture.ppm    # X11 采集帧
  sdl_validation_reference.ppm  # I420→BGRA 参考
  sdl_validation_readback.ppm   # SDL readPixels 回读
  sdl_validation_*.png          # 上面三张的 PNG（build_and_run.sh 转）
/tmp/sdl_render_data.md         # 基准自动生成的数据表
/tmp/sdl_render.md              # 验证报告（结论/数据/对 vs-webrtc 的影响）
```

## 已知边界

- **WSLg 的 vsync 周期不是精确 60.000 Hz**：虚拟显示由 Weston 合成，
  `present-only` 实测 swap 周期约 14.3–15.6 ms（64–68 fps 等效），
  随系统负载有 ±1 ms 波动。因此报告同时给「标称 16.667 ms」与「实测周期」两个下限。
- **`present` 返回 ≠ 光子出屏**：从提交到扫描输出还有 0–16.7 ms，WSLg 下无法
  直接从应用侧测量；报告只覆盖「submit → present 返回」这一段，并明确标注。
- **llvmpipe 软件 GL**：WSLg 无 `/dev/dri`，opengl 渲染器落在 llvmpipe，
  纹理上传/合成成本明显高于真实 GPU（约 1 ms/320x240）。真实 GPU 上这段更小。
- 分段延迟未绑核；p99 受 WSL2 调度噪声影响，关键数字以 p50 为准，
  复现可用 `taskset -c 2` 降低抖动。
