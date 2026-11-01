# audio_e2e：音频端到端演示

> 对应 `include/go2cpp/media/audio.hpp` / `src/media/audio.cpp` 与
> `docs/modules/media-audio.md`。工作树 `/UserData/CodexWorkSpace/Go2cpp`。

把音频链路的五段串起来跑一遍并产出可检查的统计：

```
AudioSource ──s16le PCM──► AudioEncoder（Opus / G.711 / PCM）
    ──► AudioRtpPacketizer（12B RTP 头 + 4B 分片头，MTU 1200）
    ──► UdpAudioSender 或 InProcessAudioLink
    ──► AudioJitterBuffer（seq 重排/去重/丢包）
    ──► AudioRtpReassembler（timestamp 拼分片）
    ──► AudioDecoder（含 PLC）──► AudioSink（pacat / 空后端）
```

## 场景

| 场景 | 采集 | 编码 | 传输 | 播放 | 验证点 |
|---|---|---|---|---|---|
| `inproc-pcm` | 合成 440Hz | PCM 直通 | 进程内 | 空 | 基线：零丢失、逐字节可校验 |
| `inproc-g711` | 合成 440Hz | G.711 µ-law | 进程内 | 空 | 8:1 压缩、PLC |
| `inproc-opus` | 合成 440Hz | Opus 32kbps | 进程内 | 空 | dlopen libopus、重采样、延迟 |
| `weaknet-opus` | 合成 440Hz | Opus | 进程内 ±20ms 抖动 + 丢包 | 空 | 抖动缓冲重排、PLC 掩盖 |
| `udp-opus` | 合成 440Hz | Opus | 真实 UDP 回环 | 空 | 内核收发、RTP 序号连续 |
| `pulse-opus` | Pulse `RDPSource` | Opus | 进程内 | Pulse `RDPSink` | WSLg 真实设备链路（`--real`） |

## 构建与运行

```bash
# 主工程（首次）
cmake -S . -B .build-bento-wsl -G Ninja -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
cmake --build .build-bento-wsl -j

# 本示例
cmake -S examples/audio_e2e -B .build-audio-e2e -G Ninja
cmake --build .build-audio-e2e -j

# 跑全部内存/UDP 场景
./.build-audio-e2e/audio_e2e_demo --frames 120 --artifacts .build-audio-e2e/artifacts

# 加跑 PulseAudio 真实设备（WSLg 默认可用）
./.build-audio-e2e/audio_e2e_demo --real --artifacts .build-audio-e2e/artifacts
```

或一条命令：`./examples/audio_e2e/build_and_run.sh --real`。

## 产物

- `artifacts/session.json`：每个场景的编码器/后端/播放帧数/收发包数/丢包/延迟 p50/p99/输出 RMS；
- `artifacts/captured.wav`：合成源采集的 PCM（44100Hz / 16bit / mono），可直接试听。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--frames N` | 120 | 每个场景的帧数（10ms/帧，120 = 1.2s） |
| `--jitter-ms N` | 20 | `weaknet-opus` 的额外到达抖动上限 |
| `--drop-every N` | 50 | `weaknet-opus` 每 N 个分片丢 1 个（0/1 = 不丢） |
| `--artifacts DIR` | `artifacts` | 产物目录 |
| `--real` | off | 追加 PulseAudio 真实采集/播放场景 |
| `--no-udp` | off | 跳过 UDP 场景（CI 里更快） |

## 已知限制

- WSL2 的 `RDPSource` 只在有音频活动时产出数据；`--real` 场景若 1 秒内收不到包会
  标记为 SKIP（不是失败）。
- `parec`/`pacat` 是子进程，退出时可能打印 `write() failed: Broken pipe`（Pulse
  客户端日志），不影响统计。
- Opus 走 `dlopen("libopus.so.0")`；缺失时 `inproc-opus`/`weaknet-opus`/`udp-opus`
  会 SKIP，其余场景仍可跑。
