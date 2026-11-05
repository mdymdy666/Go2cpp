# 无性能报告模块的补齐基准（2026-11-05）

> 覆盖台账中**没有性能条目**的 12 个模块：json / orm / config / error / log / hook /
> io(IOManager) / resilience / relay / trace / metrics / memory(arena)。
> 每个模块一个最小微基准，`g++ -O2` 可直接编译运行；源码在
> `tests/bench/perf_uncovered/bench_<module>.cpp`。

## 方法与环境

- 机器：WSL Ubuntu，宿主 12 vCPU / 20 GB，测量时 loadavg 0.0–0.3；**串行**执行，无并行基准。
- 绑核：`taskset -c 1`（避开 core 0 中断）；每模块连续 **5 轮**，取中位并算 CV（样本 std/mean）。
- 窗口：脚手架按实测吞吐自适应迭代数，**单样本落在 100 ms–1 s**（规范区间）；
  极端快的原子操作（metrics ~3 ns）自动放大到数千万次操作。
- 构建：`-O2 -std=c++20 -DNDEBUG`，链接 `.build-bento-wsl` 的 Release 共享库；
  orm/hook 例外：orm 直编 `src/storage/{db,orm}.cpp`（该构建未产出 storage 库），
  hook 用 LD_PRELOAD 对照（同一二进制跑两次）。

## 结果（12 模块 × 5 轮中位 + CV）

| 模块 | 指标（一次 op 的含义） | 5 轮中位 | CV | 结论 |
|---|---|---|---|---|
| json | 反序列化 5 字段小对象（含深度栅栏） | **873.1 ns** / 1.145 M ops/s | 3.18% | ✅ 稳定；开销主要在 nlohmann 堆分配，栅栏扫描占比小 |
| json | 序列化同一对象 `dump()` | **261.8 ns** / 3.820 M ops/s | 3.78% | ✅ 稳定；约为反序列化的 30% |
| orm | 单行 insert（事务内，1e5 行/轮） | **520.0 ns** / 1.923 M ops/s | 1.43% | ✅ 稳定；含语句缓存命中与主键回填 |
| orm | 主键 select `Find<Sample>` | **2278.3 ns** / 0.439 M ops/s | 2.89% | ✅ 稳定；比 insert 慢 4.4×（Result/行对象构造成本） |
| config | 整份 INI 解析（100 段 × 100 键 = 1e4 键） | **1.511 ms** / 6.62 M keys/s | 4.72% | ✅ 稳定；解析吞吐约 6.6M 键/s |
| config | 单键 `Get(section,key)` | **17.4 ns** | 4.53% | ✅ 稳定（unordered_map 查找 + string 返回） |
| error | `NewError` + `Message()` | **28.2 ns** / 35.4 M ops/s | 5.42% | ⚠️ 略超 5%：shared_ptr 分配抖动，可重复 |
| error | `Wrap(cause, msg)` 单层错误链 | **62.6 ns** / 16.0 M ops/s | 5.31% | ⚠️ 略超 5%；两层 shared_ptr 构造 |
| log | `LogItemWorker::Submit` 一行（过滤+格式化+Sink） | **18.8 ns** / 53.1 M 行/s | **21.67%** | ❌ **CV>20%：基准不合格**（每行 string 渲染分配导致抖动） |
| hook | `read(2)` 1 字节，Hook 关闭（基线） | **698.6 ns** | 4.02% | ✅ 稳定；约 1.4 M read/s |
| hook | `read(2)` 1 字节，Hook 打开 | **760.6 ns** | 1.97% | ✅ 稳定；**包装净开销 +62.1 ns（+8.9%）** |
| io | IOManager 空任务（提交→执行→完成） | **549.8 ns** / 1.819 M ops/s | 18.92% | ⚠️ 5–20%：worker 唤醒受调度抖动影响 |
| resilience | `CircuitBreaker::Allow()`（Closed） | **19.6 ns** / 51.1 M ops/s | 4.03% | ✅ 稳定；1 锁 + 1 次 steady_clock |
| resilience | `RateLimiter::TryAcquire(1.0)` | **26.7 ns** / 37.5 M ops/s | 3.45% | ✅ 稳定；懒补充令牌的浮点运算 |
| relay | Hub `Register+Lookup+Unregister` | **77.6 ns** / 12.9 M ops/s | 2.45% | ✅ 稳定（1e3 房间轮转、shared_ptr 传递） |
| trace | `TraceContext::CreateRoot()` | **5.0 ns** / 198 M ops/s | 2.77% | ✅ 稳定；两次 64 位随机数生成 |
| trace | `ChildSpan()` 派生 | **1.4 ns** / 699 M ops/s | 15.45% | ⚠️ 5–20%：绝对值太小，计时噪声占比高 |
| trace | `ToHeader()` 72 字节编码 | **55.3 ns** / 18.1 M ops/s | 4.30% | ✅ 稳定 |
| metrics | `Counter::Inc()` | **3.60 ns** / 278 M ops/s | 3.70% | ✅ 稳定（relaxed fetch_add） |
| metrics | `Gauge::Add(1.0)` | **3.39 ns** / 295 M ops/s | 3.70% | ✅ 稳定（CAS 循环） |
| metrics | `Gauge::Set(1.0)` | **3.02 ns** / 331 M ops/s | 4.05% | ✅ 稳定（原子 store） |
| arena | `Arena::Allocate(64)` | **36.0 ns** / 27.8 M ops/s | 19.73% | ⚠️ 5–20%：块边界分配放大了内存抖动 |

## CV 分布

- **CV < 5%（稳定）**：16 项 —— json 2、orm 2、config 2、hook 2、resilience 2、relay 1、
  trace 2、metrics 3。
- **CV 5–20%（可重复，未达 CV<5%）**：5 项 —— error 2（5.3–5.4%）、io 1（18.9%）、
  trace_child（15.5%）、arena（19.7%）。
- **CV > 20%（基准不合格）**：**1 项** —— log（21.67%）。根因：每行 `PatternFormatter`
  产生一次 `std::string`，分配器抖动在小窗口内无法摊薄；下一轮应改用预分配缓冲的
  Formatter 或增大窗口后复测。

## 备注与口径边界

- `config` 的 1e4 键用的是 `IniFile::Parse`（`Config::LoadFromIni` 只接受静态注册项，
  无法凭空加载未注册键）；`keys/s = ops/s × 1e4`。
- `log` 的 Sink 是原子计数器，**不落盘**；测的是日志管线（过滤/格式化/分发），
  不是磁盘写吞吐。
- `hook` 的两行是同一二进制在 `LD_PRELOAD` 开/关下的对照；差值 62 ns 即 Hook
  的 fd 元数据查找 + `OpEnabled` 判定净开销，不含 park/Fiber 调度。
- `io` 测的是 IOManager **空任务**的事件循环上限；真实 IO 吞吐由 fd 就绪频率决定，
  不在微基准可覆盖范围。
- `orm` 用 SQLite 文件库（WAL / synchronous=NORMAL），每轮先 `DELETE` 清表再插入
  1e5 行，行数恒定；select 按主键 1..1e5 顺序循环（全命中）。
- 报告只记录本轮数值；机器/负载不同不可直接对比绝对值，需按同命令复跑。

## 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# 见 tests/bench/perf_uncovered/README.md 的编译/运行命令
bash tests/bench/perf_uncovered/run_5rounds.sh   # 5 轮串行 + 落盘 results/
```
