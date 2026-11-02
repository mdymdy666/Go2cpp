# docs/reports 索引

本目录保存 Go2cpp 各专题的汇编级取证、工业库对标与开源复用报告。除特别注明外，数据均来自本机 WSL2 实测（AMD Ryzen 9 8945HX / 12 vCPU / GCC 13.3 / Release `-O3 -DNDEBUG`），判定口径默认「相对工业库 < 1.1×」。

## 2026-11-02 归档（15 份）

源文件为当日各 agent 产出的 `/tmp/*.md`；归档时在文首追加一行 `> 来源：/tmp/xxx.md，2026-11-02 归档`，正文逐字节未改动。

| # | 报告 | 模块 / 主题 | 一句话摘要 |
|---:|---|---|---|
| 1 | [asm-tools.md](asm-tools.md) | base64 / gzip / reflect JSON | Base64 编码 0.84×、解码 1.01×，gzip 压缩 0.77×，reflect 二进制编码 0.97× 均达标；gzip 解压 1.59× 未达标，后续由 `asm-compress.md` 收敛到 0.696×。 |
| 2 | [asm-http-e2e.md](asm-http-e2e.md) | HTTP 路由 / 静态文件 / pipelining | 路由改为精确哈希 + 参数 trie（64 条静态路由 O(1)）、静态文件走 `sendfile(2)`、pipeline=16 单连接 **72.9k req/s**（串行 8.9k 的 8.2×）；vs nginx 最差 0.96×、vs Go net/http 1.07–3.21×，260 条真实 socket 断言 0 失败。 |
| 3 | [asm-storage-sqlite.md](asm-storage-sqlite.md) | SQLite 封装 | 7 场景汇编级取证，文件库 1e6 行插入 **1.078×**，6/7 场景 < 1.1×（7 轮中位数 + 波动区间口径）。 |
| 4 | [asm-distributed.md](asm-distributed.md) | 租约 / 分布式锁 / 选主 | 热路径 **0 条 `lock` 前缀原子指令**，线性化靠单 mutex；callgrind 显示续约 44% 耗时在字符串拷贝、mutex 仅 7%；结论是与 etcd/Consul 非替代关系。 |
| 5 | [asm-media-capture-render.md](asm-media-capture-render.md) | X11 XShm 采集 / SDL 渲染 | 采集零拷贝 p50 **0.137 ms**（0.965×）、纹理直传 0.287 ms（快 1.74×）、vs FFmpeg 60 fps 满帧 1.007×；10 万帧逐像素 **0 差异**，RSS 增长 < 1 MB。 |
| 6 | [asm-media-audio.md](asm-media-audio.md) | Opus / 重采样 / µ-law | Opus 包装开销 1.006×（48k）、1.037×（44.1k），重采样 0.098×（快 10.2×）；µ-law SIMD 5.54×、重采样 SIMD 3.15×；栈帧 1304 B → **8 B**。 |
| 7 | [asm-os.md](asm-os.md) | file / proc / shm / mq | 文件单条读写 1.00–1.03×、vs Go `os.File` 0.73–0.81×（更快）；批量 writev/readv 架构优化 **17.9× / 20.9×**；`CopyTo` sendfile 8 MiB 1.10–1.15×（部分达标）。 |
| 8 | [asm-base.md](asm-base.md) | container / math / metrics / path | path 快 `std::filesystem` 3–9×，Heap 1.03×；List::Next 1.49×、Ring 1.18–1.57× 未达标；math 四项经 **LTO 对照**证为共享库跨 TU 边界代价（LTO 后 0.99–1.04×）。 |
| 9 | [asm-debug-replay.md](asm-debug-replay.md) | debug 事件环 / replay | OFF 配置链接后仅 `endbr64; ret`（0 call / 0 原子 / 0 栈帧）；ON_FULL 慢倍数 3.6–40.3× → **1.44–8.76×**；emit 单事件 171 指令 / 12 call / 1 次 `lock addq`。 |
| 10 | [asm-tls.md](asm-tls.md) | TLS 传输层 vs OpenSSL | 握手 mean 1.070×、32 KB 写 0.933–1.008×、64 B 小包 0.946–1.064×（优化前 1.29–1.35×），**全维度 < 1.1×**。 |
| 11 | [asm-compress.md](asm-compress.md) | gzip 解压 CRC | 2420 → **5219.7 MiB/s**（2.16×），耗时比 1.49× → **0.696×**（快于 zlib 1.44×），超 2713 MiB/s 目标 1.92×。 |
| 12 | [asm-reuse-random-endian.md](asm-reuse-random-endian.md) | 安全随机 / 大端读写 | 随机源统一（bento 与 p2p 共享同一熵源）、大端读写抽取；新旧实现输出不同但均合格。 |
| 13 | [asm-reuse-hash.md](asm-reuse-hash.md) | SHA-256 / FNV-1a | SHA-256 3 → 1 份、FNV-1a 4 → 1 份；各 100 万组随机输入 **0 mismatch**；redact 199–234 → **77–87 ns/次**。 |
| 14 | [asm-reflect.md](asm-reflect.md) | reflect 紧凑二进制 / 文本直读 | 二进制编解码 0.63–0.70×、文本编解码 0.54–0.67×，**四项全部 < 1.1×**。 |
| 15 | [asm-legacy.md](asm-legacy.md) | Link 多队列 / PeerHub FEC | 单 peer 多队列无吞吐收益（持平 / 4 队列空转 −70%）；FEC 使重传 17 → 6（−64.7%）、另一轮 42 → 4；LSM 读路径取消（改 LevelDB）；`sync::Mutex` 迁移因 p99 2 → 76 ms 回退。 |

## 同目录既有报告（未纳入本批）

| 类别 | 文件 |
|---|---|
| 汇编级专题 | [asm-sfu.md](asm-sfu.md)、[asm-signaling.md](asm-signaling.md)、[asm-storage-features.md](asm-storage-features.md)、[asm-storage-kv.md](asm-storage-kv.md) |
| 对标 / 复用 | [reuse-audit.md](reuse-audit.md)、[vs-cache.md](vs-cache.md)、[vs-concurrency.md](vs-concurrency.md)、[vs-network.md](vs-network.md)、[vs-webrtc.md](vs-webrtc.md) |
| 日期化报告 | `2026-10-30-*.md`、`2026-10-31-*.md`、`2026-11-01-*.md`（见目录列表） |

## 归档约定

1. 报告以 `asm-<模块>.md` 命名，正文含环境、方法论、实测数据与「离物理下限几倍」的判断。
2. 归档文件保留原始正文，仅文首追加来源行；后续修订在正文内以日期小节追加，不改历史数据。
3. 索引按归档批次累加，新批次追加新章节，不覆盖旧批次。
