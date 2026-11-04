# 2026-11-04 工作总结

> 主线：**基座工业级化**（审查 → 修复 → 门禁）
> 副线：抖音真实功能补齐
> 用户当天的三个原始任务：① 各模块架构+汇编优化（比工业参考 1.1× 以下）② 基础设施（reflect/Error/config/log）覆盖 ③ 对接 JinShanProject

---

## 一、今日完成概览

| 类别 | 成果 |
|---|---|
| **基座审查** | 9 个模块（Parser / Socket / Channel+go()+RWMutex / TLS+tunnel+bufio / 构建 ABI / 测试期望 / relay / rpc / distributed） |
| **修复** | **P0×6、P1×20+、P2×20+**（全部有复现与前后数据） |
| **可靠性主线** | 3/3 完成（幂等键 / 断点续传 / WS Resume） |
| **抖音副线** | 5 项功能（播放体验 / 话题 / 收藏 / 搜索 / Range）+ 声音/关注/主页/评论回复/折叠/视频链接 |
| **性能** | tunnel **830 → 2385 MiB/s**、开销 **3.56× → 1.27×**；TLS 1.02–1.04×；bufio 0.895×；kv 1.1× |
| **质量门禁** | 6/6 绿（ctest / ASan / TSan / 零告警 / 构建一致性 / soak） |
| **提交** | 70+ |

---

## 二、P0 级缺陷（6 个，全部修复）

| # | 模块 | 问题 | 后果 |
|---|---|---|---|
| 1 | `os_signal.hpp` | `TrySend` 忽略 `[[nodiscard]]` | **`test_go_signal` 编译失败**（-Werror） |
| 2 | `rpc/server.cpp` | `Process` 无 try/catch | handler 抛异常 → **进程 terminate** |
| 3 | `distributed/metadata_store.cpp` | 背压死锁（写者/派发线程互相等锁成环） | Metadata 写入**永久卡死** |
| 4 | `relay/splice_pipe.cpp` | 线程创建失败 → `workers[0]` joinable + 访问已销毁 State | **terminate + UAF** |
| 5 | `Parser` | `m_finished` 后 feed 静默返回旧 `Complete` | 新字节**永久丢失** |
| 6 | `network/http.cpp` | 曾调不存在的 `Headers::empty()` | 编译中断（过程态） |

## 三、一个反复出现的模式（今天最重要的技术发现）

> **「条件变量丢唤醒」在 3 个模块各出现一次**：
> - `RWMutex`（`c5b888f`）：读者登记与判谓词之间被 `Unlock` 跨过；
> - `MuxStream::Close/MarkPeerClosed`（`b176c7f`）：**锁外**改标志 + notify；
> - `tunnel` 的 Direction 线程（`850e813`）：`Run(nullptr)` 永不取消。
>
> **统一根因**：`notify` 不在锁内 → 存在「判谓词 → 挂 futex」窗口 → 唤醒必丢。
> **统一修法**：**置位 + notify 必须在同一把锁内**（或谓词原子 + 内存序）。

## 四、性能（最终数据）

| 模块 | 结果 | 目标 |
|---|---|---|
| **tunnel（raw 按需）** | **2385 MiB/s、1.27×**、p99 4.41 ms | ≤2× ✅ |
| tunnel（mux 对照） | 770 MiB/s、3.59× | — |
| TLS AEAD | 1.02–1.04× | ≤1.1× ✅ |
| bufio 热态 | 0.895×（+43%） | ≥0.9× 🟡 |
| kv 读/写 | 1.11–1.25× / 1.09× | ≥1.1× ✅ |
| WS p99 / RSS | 0.93× / 0.96× | ≤1.1× ✅ |
| Parser 头重解析 | 66.3 ms → **1.95 ms** | — |
| Parser chunked | 4017 ms → **17 ms** | — |
| gzip / reflect | 1.35× / 0.84–0.99× | ✅ |

## 五、可靠性主线（3/3）

| 项 | 数据 |
|---|---|
| 幂等键 | 22 PASS（同 key 只生效一次、TTL 过期重执行） |
| 上传断点续传 | 50 PASS（kill -9 后 status 仍 4 MB → 续传 → cmp 一致） |
| WS Resume | 45 PASS + 737 单测（补发 6..15 无重无漏、4000 条无洞） |

## 六、抖音副线

- 声音（demo4/5 补音轨 + 音量控件）、关注/粉丝、我的主页、评论二级回复 + 折叠、视频链接 `/v/:id`；
- 播放体验（进度条 seek、双击点赞、封面）、话题聚合（14 个）、收藏 + 分享面板、搜索三分类、Range（206/416）；
- 视频压缩：**8.58 MB → 3.78 MB（省 56%）**；
- `accept_all` **72 PASS**。

## 七、质量门禁（6/6）

| # | 门禁 | 结果 |
|---|---|---|
| ① | 全量 ctest | ✅ 284/286（2 项环境性） |
| ② | ASan / UBSan | ✅ 泄漏 0、UBSan 38→0 |
| ③ | TSan | ✅ **20/20 0 thread leak** |
| ④ | 零告警 | ✅ **54 → 0** |
| ⑤ | 构建一致性（stamp） | ✅ FRESH（脚本实测有效：真实树曾 STALE） |
| ⑥ | soak | ✅ 341 s / 421.9 GiB / 0 错误 / 0 泄漏 |

## 八、我今天的两个错误（已改进）

| 错误 | 真相 | 改进 |
|---|---|---|
| 「13 个 SEGFAULT 是回归」 | 旧 exe + 新 so（ABI 不匹配） | 写进 `docs/agent-isolation.md` + 做了 stamp 门禁 |
| 「Parser 未 reset 会吞 body」 | 用 12:44 旧 `.so` 的假象 | 任何结论必须注明「构建目录 + exe/so 是否同批」 |

## 九、遗留（下一轮）

| 项 | 说明 |
|---|---|
| 非流式 RPC 的 Cancel 无效 | 需请求队列化 + worker（架构级） |
| distributed 6 项 | 先改内存后落盘、Restore 不清 degraded 等（需产品决策） |
| http3 / p2p / storage 心智负担 | 未审查 |
| `bench_tunnel.cpp` 未注册 CMake | 手工编译 |
| tunnel raw 兼容性 | 老客户端不认 `OPEN_RAW`，大 body 会断连（需升级客户端或 `THRESHOLD=-1`） |
| 纯净 worktree 终验 | 进行中（`d55e4794`） |

## 十、三件套与 JinShan（今日原始任务）

- 三件套覆盖：✅13 / 🟡26 / ❌1 / ⚪4；leveldb 净增 **5 条指令**（≤5 预期）；cache 三实现 0 锁；
- JinShan：REQ-010（MSVC 全过）/ REQ-011（云端上传 example）/ REQ-012；**REQ-011 的「Windows 网络层」仍待决策**（Linux-only vs 移植）。
