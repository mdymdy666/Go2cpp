# media_e2e：媒体端到端真实验证（采集 → 编码 → 分片 → 传输 → 重组 → 解码 → 渲染）

本目录是 `go2cpp::media` 的**可复现端到端 demo**，把「采集 → 编码 → VideoChannel
1200B 分片 → UDP 回环 / 包级弱网 → JitterBuffer 重排 → 重组 → 解码 → PPM 渲染」
整条链路真正跑通，并产出：

- **逐像素证据**：采集帧 vs 渲染帧的 SHA-256 + 逐字节 memcmp；
- **延迟**：p50/p99/max，以及采集/编码/分片/收发/重组/解码/渲染分段耗时；
- **弱网**：±50ms 抖动 + 5% 丢包 + 乱序下 jitter 开/关对照；
- **降级**：带宽阶梯驱动 `full → reduced → low → minimal`，尺寸/帧率真实变化；
- **截图**：采集源与渲染结果并排 PNG（`artifacts/*_compare.png`）。

## 构建与运行

```bash
# 0) 先有主工程媒体库
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
cmake --build .build-bento-wsl -j"$(nproc)"

# 1) 一键（构建 + 跑 demo + 跑回归测试 + PPM→PNG）
bash examples/media_e2e/build_and_run.sh

# 2) 或分步
cmake -S examples/media_e2e -B .build-media-e2e -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build-media-e2e -j"$(nproc)"
.build-media-e2e/media_e2e_demo --artifacts examples/media_e2e/artifacts
.build-media-e2e/media_test_e2e_jitter
```

`build_and_run.sh` 会用 Pillow 把 PPM 转 PNG，并生成
`*_compare.png`（采集 | 渲染）与 `*_bad_diff.png`（错帧差异热图）。

## 场景

| 场景 | 内容 | 关键断言 |
|---|---|---|
| `x11-real` | 真实 X11 窗口（SDL 画面源）→ `XGetImage` 采集 → delta → UDP 回环 → 渲染 | 30/30 帧 I420 逐字节一致 |
| `raw-bgra-strict` | 合成彩色棋盘/帧号 → raw-bgra → 包级 jitter → 渲染 | 90/90 帧 BGRA SHA-256 相同、memcmp 差异 0B |
| `delta-1000` | 320x240 delta → pipeline（`MediaSender/Receiver`）→ 真实 UDP | 1000/1000 交付、零错序/零花屏、p50/p99 |
| `weaknet-delta` | ±50ms 抖动 + 5% 丢包 + 乱序，delta | 关键帧 100% 正确；jitter 交付 897 vs 无 jitter 700 |
| `weaknet-raw` | 同上，raw-i420（每帧独立） | 交付帧 100% 逐字节正确（353 帧） |
| `degradation` | 带宽 2M→700k→320k→120k | full→reduced→low→minimal，320x240→160x120，30→15→15→10fps |
| `latency` | 逐段计时 + memcpy/sendto 基准 | p50 0.44ms；搬运+syscall 下限 0.034ms → 12.8x |

## 关键设计

- **包级 jitter 的位置**：在 `VideoChannel` **重组之前**按 Bento 包序号重排
  （`docs/modules/jitter-degradation.md` §2.1），避免「缺一片就丢整帧」与
  「帧乱序导致旧帧 stale」。`PacketReceiver` 就是这条路径的最小实现。
- **pipeline 内的 jitter**：`src/media/pipeline.cpp` 按 §5 接线，整帧切 8KB 块
  （12B 块头）进 `JitterBuffer`，`Pop` 拼回整帧后再解码；默认关闭。
- **pipeline 内的 degradation**：发送端 `OnNetworkSample` 驱动
  `DegradationController`，档位变化时重建采集源/编码器并强制关键帧；收端用
  `ShouldDeliverFrame` 做帧率门控。
- **WSLg 限制**：WSLg 的 X11 root 是 Weston 黑底，`XGetImage(root)` 拿不到
  XWayland 窗口内容；demo 用 `XGetImage(具体窗口)` 采集真实窗口像素，root 全黑
  的截图只能作为 fallback。

## 产物

```
examples/media_e2e/artifacts/
  x11_compare.png                 # 采集 | 渲染（真实 X11）
  raw_strict_compare.png          # 采集 | 渲染（raw-bgra 逐像素）
  delta1000_mid_compare.png       # 1000 帧场景中帧
  weaknet_delta_jitter_compare.png
  weaknet_delta_nojitter_bad_compare.png   # 错帧 vs 同帧采集
  weaknet_delta_nojitter_bad_diff.png      # 错帧差异热图（×8）
  degradation_{full,reduced,low,minimal}_compare.png
  *.ppm                           # 原始渲染/采集证据
/tmp/media_e2e_data.md            # demo 自动生成的数据表
/tmp/media_e2e_degradation.md     # 降级阶梯表
/tmp/media_e2e_latency.md         # 分段延迟与下限
/tmp/media_e2e.md                 # 汇总报告（含结论与复现命令）
```

## 已知边界

- 弱网是**包级确定性模拟**（丢包 5%、每帧 0..100ms 随机延迟），不是 `tc netem`
  （WSL2 无 `CAP_NET_ADMIN`）；
- `JitterBuffer` 的每个空洞独立等待 `target_delay`，5% 丢包 + 大帧（24 片/帧）
  时吞吐会不足；demo 的 raw 弱网用 80x60（4 片/帧）规避该实现边界，
  delta（2–5 片/帧）不受影响；
- 分段延迟未绑核，p99 受 WSL2 调度噪声影响；关键数字以 p50 为准，复现可用
  `taskset -c 2`。
