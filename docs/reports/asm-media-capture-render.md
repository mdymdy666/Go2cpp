> 来源：/tmp/media_capture_render.md，2026-11-02 归档

# 媒体采集（X11 XShm）/ 渲染（SDL）架构+汇编+正确性报告

- 日期：2026-11-02
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL，非 E: 树）
- 独立构建：`/tmp/build-media-cr`（Release，`-O3 -DNDEBUG`，gcc 13.3）+ `/tmp/build-sdl-cr`
- 环境：WSLg XWayland 2560x1600 depth-24，MIT-SHM 可用；SDL2 2.30；FFmpeg 6.1.1（xcbgrab）
- 关联数据：`/tmp/media_capture_render_data.md`（1e5 帧）、`/tmp/media_capture_render_data2.md`（2e4 帧含拷贝对照）、`/tmp/media_ffmpeg_vs_xshm.md`（FFmpeg 对比）、`/tmp/asm_media_before_after.txt`（objdump 全量）

---

## 0. 结论摘要

| 项目 | 结果 | 判定 |
|---|---|---|
| **采集零拷贝** | XShm 段直接 `VideoFrame::Borrow()`，不再整帧 memcpy；1e5 帧 p50 **0.137 ms**（owned 0.142 ms）、2272 fps（owned 2244 fps）；2e4 帧复跑 p50 0.132 / 0.139 ms | **0.965×（1e5）/ 0.95×（2e4）**，<1.1× ✅ |
| **渲染纹理直传** | BGRA `Bytes()`/`EffectiveStride()` 直接进 `SDL_UpdateTexture`；p50 **0.287 ms**（旧路径拷贝基线 0.437 ms，2e4 帧口径；1e5 帧直传 0.291 ms） | **0.66×（快 1.74×）**，<1.1× ✅ |
| **vs FFmpeg x11grab（60 fps 稳态）** | 双方都满帧：59.6 fps vs 60 fps | **1.007×**，<1.1× ✅ |
| **vs FFmpeg 无节流吞吐** | 不可比：FFmpeg 6.1 x11grab 是 xcbgrab（无 MIT-SHM）+ 重复帧补齐，`-framerate 1000` 抓 3000 帧 `dup=3903` | 标注为不可比，不虚报 |
| **逐像素 0 差异** | 采集 vs owned 逐字节 0；采集→SDL 直传→回读逐字节 **0 B 差异**（真实窗口 + 320×240 + 100000 帧抽样） | ✅ |
| **1e5 帧压测** | 采集 100000 帧 / 渲染 100000 帧，0 失败、0 帧不符、RSS 增长 <1 MB | ✅ |
| 媒体测试套件 | `ctest -R '^media_'` 12/12 通过（含 capture 1318 checks、render 2056 checks、sdl 13 checks） | ✅ |

---

## 1. 改动清单（全部在允许边界内）

| 文件 | 改动 |
|---|---|
| `include/go2cpp/media/capture.hpp` | `X11CaptureConfig` 新增 `zero_copy` / `fill_alpha_in_place` / `target_window`；`VideoSource` 新增 `last_capture_borrowed()`；补零拷贝生命周期文档 |
| `src/media/capture_x11.cpp` | XShm 零拷贝分支（`Borrow` + 原地 alpha）；owned 路径改 SSE2 行拷贝；XShmGetImage 瞬态失败重试 1 次 + 1 s 冷却重建；支持抓取任意 window drawable |
| `src/media/capture_kernels.hpp`（新） | `CopyBgraRowScalar/Simd`、`FillAlphaRowScalar/Simd`（SSE2 16 B/轮 + 标量尾） |
| `include/go2cpp/media/render.hpp` | `FrameRenderer` 新增 `Readback()`、`last_present_zero_copy()` |
| `src/media/render.cpp` | BGRA 直传 `SDL_UpdateTexture(frame.Bytes(), EffectiveStride())`，删除 `m_bgra = frame` 整帧拷贝；I420 转换缓冲复用；新增 `Readback`（`SDL_RenderReadPixels`） |
| `tests/media/test_capture.cpp` | 新增内核标量/SIMD 对照、XShm 零拷贝（借用 vs owned 逐字节、200 帧稳定、alpha 开关、窗口内容变化） |
| `tests/media/test_render.cpp` | 新增直传断言（自有帧/借用视图/I420）、1000 帧直传计数 |
| `tests/media/test_sdl_render.cpp` | 新增库级闭环：XShm 零拷贝采集 → SDL 直传 → `ReadPixels` 回读 0 差异 |
| `examples/sdl_render/media_capture_render_bench.cpp`（新） | 采集/渲染专项基准（1e5 帧、p50/p90/p99、RSS、回读校验、拷贝基线） |
| `examples/sdl_render/bench_vs_ffmpeg.sh`（新） | vs FFmpeg 对比脚本（含 dup 语义说明） |
| `examples/sdl_render/CMakeLists.txt` | 注册新基准目标 |

未改 `pipeline.cpp` / `codec.cpp`（边界外）：`codec.cpp:109` 仍直接读 `frame.data.data()`，因此 **零拷贝默认关闭**，避免破坏现有编码链路；需要零拷贝的调用方显式 `config.zero_copy = true` 并用 `Bytes()` 消费。

---

## 2. 架构

### 2.1 采集（XShm 零拷贝）

改动前：`XShmGetImage` → 逐像素/自动向量化循环写 `out->data`（307 KB/帧分配 + 写）。
改动后（`zero_copy=true`）：

```
XShmGetImage(display, target, shm_image, x, y, AllPlanes)   // 服务器直接写共享段
  → out->Borrow(shm_image->data, shm_image->bytes_per_line)  // 仅指针+stride
  → FillAlphaRowSimd(row)                                    // 原地写每 4 B 的第 4 B
```

- 零整帧 memcpy、零 307 KB 分配；借用视图有效到下一次 `Capture()`。
- `fill_alpha_in_place=false` 可再省一遍原地写（下游只用 BGR 时，如 BGRA→I420）。
- `use_shm=false`（XGetImage 回退）时缓冲随 `XDestroyImage` 释放，仍走 owned 拷贝。
- 失败自愈：`XShmGetImage` 失败先 `XSync` 重试一次；仍失败 `CloseShm()` 并 1 s 冷却后重建（WSLg 窗口重绘期实测存在瞬态 BadMatch）。

### 2.2 渲染（纹理直传）

改动前：`m_bgra = frame`（Vector 深拷贝 307 KB，可能触发重新分配）→ `SDL_UpdateTexture(m_bgra.data())`。
改动后：BGRA 帧直接 `SDL_UpdateTexture(m_texture, nullptr, frame.Bytes(), frame.EffectiveStride())`；只有 I420 输入做一次格式转换（目标缓冲跨帧复用）。整条渲染路径无文件 IO、无 PPM（旧 `sdl_render_bench` 的 PPM 只是验证落盘证据，不在渲染路径上）。

---

## 3. 汇编级（objdump，`-O3 -DNDEBUG`，gcc 13.3）

统计口径：`objdump -d -C --no-show-raw-insn`；指令数含内联展开；call 含 PLT 调用。

### 3.1 采集热函数

| 指标 | 改动前（HEAD） | 改动后 |
|---|---|---|
| `X11CaptureImpl::Capture` | **954 ins** / 11 call / 栈帧 **552 B** | **437 ins** / 32 call / 栈帧 **88 B** |
| `CaptureFromShm`（新，零拷贝/拷贝分支） | — | **298 ins** / 3 call / 栈帧 **40 B** / SIMD 8 |
| 合计（同一编译单元内布局） | 954 ins / 552 B | **735 ins（-23%）** / 128 B（-77%） |
| 原子指令 / 堆分配 | 0 / 0 | **0 / 0** |

> call 数上升是因为新增了「重试 + 1 s 冷却重建 + chrono」错误路径；这条路径只在 XShm 失败时执行，成功热路径不触发。

热路径指令（`CaptureFromShm` 内联后的 SSE2 alpha 填充）：

```
903: movdqu (%rax,%rdx,1),%xmm0     ; 读 4 像素（16 B）
908: pand   %xmm1,%xmm0              ; 保留 BGR
90c: por    %xmm2,%xmm0              ; A=0xFF
910: movups %xmm0,(%r15,%rdx,1)      ; 原地写回共享段
```

owned 路径（`CopyBgraRowSimd`，在 `Capture` 内联）同样的 `movdqu/pand/por/movups`，但目标是 `out->data`。

### 3.2 渲染热函数

| 指标 | 改动前（HEAD） | 改动后 |
|---|---|---|
| `SdlRenderer::Present` | **306 ins** / 18 call / 栈帧 80 B | **227 ins（-26%）** / 11 call（-39%）/ 栈帧 88 B |
| `SdlRenderer::Readback`（新） | — | **72 ins** / 2 call / 栈帧 8 B |
| 原子 / 堆分配 | 0 / 0 | 0 / 0 |

Present 少掉的 7 个 call 即 `std::vector` 拷贝构造/赋值/析构与 stride 重算；热路径只剩 1 次 `SDL_UpdateTexture` + `SDL_RenderClear/Copy/Present` 与事件 poll。

### 3.3 离物理下限几倍

- **采集零拷贝 CPU 侧**：alpha 原地填充内存流量 = 读 307 KB + 写 76.8 KB ≈ 384 KB；按 ~30 GB/s 估算 ≈ **13 µs**。实测 p50 **132 µs** → **约 10× 下限**，差额是 `XShmGetImage` 的 X 往返（同步 round-trip，不受本进程控制）。**结论：采集瓶颈在 X 协议往返，不在拷贝**；再想降延迟必须换机制（XDamage/不轮询/DPMS 级共享），而不是继续抠指令。
- **采集 owned 路径**：多一次 307 KB 读+写（≈ 614 KB，≈ 20 µs 下限），实测 139 µs → 7×；与零拷贝差 5–7 µs，与内存估算一致。
- **渲染直传**：自有代码热路径 227 条指令（≈ 0.1 µs 量级），实测 p50 **287 µs** 全部花在 SDL 软件后端（`SDL_UpdateTexture` 307 KB memcpy + `RenderCopy` blit）→ 自有代码开销可忽略；对比拷贝基线 437 µs，省掉的一次整帧分配+拷贝约 **150 µs**（理论 307 KB 拷贝仅 ~10–20 µs，差额来自 `std::vector` 重新分配）。

---

## 4. 正确性：逐像素 0 差异

| 场景 | 结果 | 证据 |
|---|---|---|
| XShm 零拷贝 vs owned（同窗口同帧，含 alpha） | **0 B 差异**，checksum 相同 | `test_capture`（1318 checks） |
| 采集内核标量 vs SSE2（宽 97 尾像素 + 宽 64 对齐） | **0 B 差异**，A 恒 0xFF | `test_capture` |
| 零拷贝连续 200 帧（静止窗口） | 借用 200/200，checksum 0 次变化 | `test_capture` |
| 窗口内容变化后零拷贝采集 | checksum 变化（非静态缓存） | `test_capture` |
| 采集 → SDL 直传 → `RenderReadPixels`（真实窗口 320×240） | **0 B 差异** | `test_sdl_render` 13 checks PASS |
| 1e5 帧渲染压测每 1000 帧回读（100 次） | **0 B 差异**，`zero-copy presents=100000/100000` | `media_capture_render_bench` |
| 全媒体回归 | `ctest -R '^media_'` **12/12 Passed** | `/tmp/build-media-cr` |

---

## 5. 1e5 帧压测（320×240，WSLg，单进程单线程）

采集 100000 帧 + 渲染 100000 帧（数据：`/tmp/media_capture_render_data.md`）：

| 场景 | 帧数 | fps | p50 ms | p90 ms | p99 ms | max ms | RSS KB | 不符 |
|---|---|---|---|---|---|---|---|---|
| 合成源采集（无 X） | 100000 | 10759.9 | 0.091 | 0.098 | 0.125 | 0.937 | 7680→8900 | 0 |
| **XShm 零拷贝采集** | 100000 | **2272.4** | **0.137** | 0.221 | 0.305 | 1.407 | 8768→9408 | 0 |
| XShm owned 采集 | 100000 | 2244.2 | 0.142 | 0.215 | 0.280 | 0.736 | 9408→10432 | 0 |
| XShm root 零拷贝 | 20000 | 8443.5 | 0.106 | 0.189 | 0.257 | 0.724 | 10432→10688 | 0 |
| **SDL 直传渲染（dummy）** | 100000 | **2579.5** | **0.291** | 0.310 | 0.357 | 1.233 | 11452→12348 | 0 |

2e4 帧渲染对照（`/tmp/media_capture_render_data2.md`）：

| 场景 | 帧数 | fps | p50 ms | p99 ms |
|---|---|---|---|---|
| SDL 直传 | 20000 | 2601.2 | 0.287 | 0.377 |
| SDL 拷贝基线（旧路径） | 20000 | 1491.4 | 0.437 | 0.554 |

- 内存：1e5 帧后 RSS 增长 640 KB（采集零拷贝）/ 896 KB（渲染），无线性泄漏；0 帧不符、0 采集失败。
- root 采集吞吐在多次运行间波动（2.2k–9.3k fps），原因是 WSLg 合成器对 root 抓取的影响；窗口采集稳定在 2.2–2.4k fps。报告取多次运行的中位区间。
- 渲染吞吐用 `SDL_VIDEODRIVER=dummy`（无 vsync）测，真实窗口只做正确性闭环（`test_sdl_render`）——避免 60 Hz vsync 把吞吐钉死在 16.7 ms。

---

## 6. vs FFmpeg x11grab

数据：`/tmp/media_ffmpeg_vs_xshm.md`（脚本 `examples/sdl_render/bench_vs_ffmpeg.sh`）。

| 指标 | go2cpp XShm 零拷贝（root 320×240@0,0） | FFmpeg 6.1.1 x11grab |
|---|---|---|
| 实现 | MIT-SHM，服务器直写共享段 | **xcbgrab（libxcb），该版本无 MIT-SHM 选项** |
| 无节流单帧 p50 | **0.098 ms**（9319 fps） | 无独立单帧指标（xcbgrab 内部） |
| 60 fps 稳态 600 帧 | 可跑满（无节流 9.3k/2.4k fps） | **59.6 fps**（wall 10069 ms，utime 0.969 s ≈ 1.61 ms/帧 CPU，含 rawvideo 输出） |
| `-framerate 1000` 抓 3000 帧 | 无节流 2428.6 fps（窗口） | wall 194 ms，`frame=3000 / dup=3903`（**绝大多数是重复帧**） |

**判定**：
- 同一 60 fps 节拍下双方都满帧 → **1.007×**（<1.1× ✅）。
- 无节流吞吐**不可比**：FFmpeg 该版本没有 MIT-SHM，且 xcbgrab 用「重复上一帧」补齐定时器，`dup` 计数说明 194 ms 内并未真正抓 3000 帧。如实标注，不把 194 ms 换算成 15k fps。
- 参考量级：FFmpeg 60 fps 下 CPU 1.61 ms/帧（含 rawvideo 打包/写出），本实现采集 wall 0.098–0.137 ms/帧（不含编码）；口径不同，仅作量级参考。

---

## 7. 是否 <1.1×

| 对比 | 比值 | 判定 |
|---|---|---|
| 采集：零拷贝 / owned（p50，1e5 帧） | 0.137 / 0.142 = **0.965×** | ✅ <1.1× |
| 采集：零拷贝 / owned（p50，2e4 帧复跑） | 0.132 / 0.139 = **0.95×** | ✅ |
| 采集：零拷贝 / owned（吞吐，1e5 帧） | 2272.4 / 2244.2 = 1.013× | ✅ |
| 渲染：直传 / 拷贝基线（p50，2e4 帧） | 0.287 / 0.437 = **0.66×** | ✅ |
| 渲染：直传 / 拷贝基线（吞吐） | 2601 / 1491 = 1.74× | ✅ |
| vs FFmpeg 60 fps 稳态 | 60 / 59.6 = **1.007×** | ✅ |
| vs FFmpeg 无节流 | — | 不可比（已说明） |

---

## 8. 限制 / 未覆盖（如实列出）

1. **零拷贝默认关闭**：`codec.cpp:109` 等调用方直接读 `frame.data`，全链路零拷贝需改 `codec.cpp`/`pipeline.cpp`（本次边界外）。默认 owned 保持兼容，测试/基准显式开 `zero_copy`。
2. **XShmGetImage 瞬态失败**：WSLg 窗口刚重绘后立即抓取会偶发 BadMatch（测试里加了 50 ms 稳定等待）；生产路径已加「重试 1 次 + 1 s 冷却重建」，但没有在 1e5 压测里人为注入失败验证重建计数（只覆盖了正常路径）。
3. **FFmpeg 不可比部分**：该版本 x11grab 无 MIT-SHM、重复帧机制使无节流吞吐无法等价对比；60 fps 稳态对比成立。
4. **渲染吞吐口径**：1e5 帧用 dummy 驱动（software 后端）；真实窗口仅 1 帧闭环 + 13 项契约测试，未做 1e5 帧真实窗口（vsync 会把上限钉在 60 fps，且 WSLg 合成器开销不稳定）。
5. **未做**：DMA-BUF/EGL 直传（`SDL_UpdateTexture` 仍是客户端→纹理一次上传）、XDamage 事件驱动采集（当前仍是同步 `XShmGetImage` 轮询）、多线程采集/渲染。
6. **文档未更新**：`docs/modules/webrtc-media.md` 不在本次改动边界内，仍描述「一次 memcpy 转 BGRA」的旧行为。

---

## 9. 复现命令

```bash
# 构建（独立目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-media-cr -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_MEDIA=ON
cmake --build /tmp/build-media-cr --target go2cpp_media media_test_capture media_test_render -j"$(nproc)"

# 测试
DISPLAY=:0 /tmp/build-media-cr/media_test_capture
DISPLAY=:0 /tmp/build-media-cr/media_test_render
ctest --test-dir /tmp/build-media-cr -R '^media_' --output-on-failure

# 基准（1e5 帧）与 SDL 闭环
cmake -S /UserData/CodexWorkSpace/Go2cpp/examples/sdl_render -B /tmp/build-sdl-cr -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_DIR=/tmp/build-media-cr
cmake --build /tmp/build-sdl-cr -j"$(nproc)"
DISPLAY=:0 /tmp/build-sdl-cr/media_capture_render_bench --frames 100000 --render-frames 100000 \
      --out /tmp/media_capture_render_data.md
DISPLAY=:0 /tmp/build-sdl-cr/sdl_test_render

# vs FFmpeg
DISPLAY=:0 /tmp/build-sdl-cr/../bench_vs_ffmpeg.sh /tmp/build-sdl-cr 3000   # 见 examples/sdl_render/bench_vs_ffmpeg.sh

# objdump before/after
git -C /UserData/CodexWorkSpace/Go2cpp show HEAD:src/media/capture_x11.cpp > /tmp/old_capture.cpp
g++ -O3 -DNDEBUG -std=c++17 -I/UserData/CodexWorkSpace/Go2cpp/include \
    -I/UserData/CodexWorkSpace/Go2cpp/src/media -c /tmp/old_capture.cpp -o /tmp/old_capture.o
objdump -d -C --no-show-raw-insn /tmp/old_capture.o | sed -n '/X11CaptureImpl::Capture/,/^$/p'
```

---

## 10. 备注

- 本次未引入第三方代码/依赖；SSE2 内嵌函数来自标准 `<emmintrin.h>`，XShm 序列（`shmget/shmat/XShmAttach`）沿用项目既有实现。
- 未 commit（按任务要求）。
- 工作树与其它 agent 共享：`include/go2cpp/media/audio.hpp`、`src/media/audio.cpp`、`tests/media/test_audio.cpp` 的未提交改动不是本次产物。
