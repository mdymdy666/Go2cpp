# QUICKSTART · 一页速览

> 如果你只有 3 分钟，读这一页。
> 全部内容位于 `DebugTooling/`（19 个文件 / 约 3,100 行），本阶段**只做分析与设计，未写功能代码**。

## 1. 这是什么

Go2Cpp 的 GMP 与协程层**已经有一套完整的"看得见"能力**（Fiber 编号/状态/挂起原因/G-M-P 绑定/
调用栈含墓碑帧/P-M 快照/指标/可替换的事件观察者），但**几乎没有"改得动"的能力**。
本次交付的是一份**调试模块的设计**：34 个真实场景 + 20 个 dump 选项 + 五层架构 + 15 个接口 + 测试策略。

## 2. 按这个顺序读

| 顺序 | 文件 | 你会得到 |
|---:|---|---|
| 1 | `design/05-architecture-summary.md` | 交付物全景 + 五层架构 + P0-P7 阶段 + **证据索引表** |
| 2 | `FINAL-REPORT.md` | 三步对账 + 关键结论 + **做不到的 7 项** + 待定案 |
| 3 | `scenarios/00-scenario-index.md` | 34 个场景的分类索引 |
| 4 | `design/04-encoding-roadmap.md` | 编码走向（模块划分、插件协议、多机形态） |
| 5 | `design/06-interface-signatures.md` | 15 个接口的签名、前置条件、默认开关 |
| 6 | `design/08-verified-details.md` | 逐轮读码核验：9 处订正 + 4 条易误读 + 1 条覆盖边界 |

## 3. 只有 3 件事必须你先拍板（否则无法开工）

| # | 问题 | 建议 |
|---:|---|---|
| 1 | **元数据从哪来**（Fiber 的名字 / 函数名 / 行号） | 转译期自动注入 + 运行期可覆盖（**共存**） |
| 2 | **多机时钟方案** | 双时间戳为基础 + **明确声明不做绝对对齐** |
| 3 | **运行期可改的调度参数范围** | 只开放观测类/节律类；结构类（P 数/栈大小）**明确拒绝** |

另有 4 项"点头即可"（已给建议）：是否接受断点只停安全点（建议接受）、是否要 owner 追踪
（建议仅调试构建）、工具形态范围（建议库+CLI）、调试数据脱敏（建议默认仅结构信息）。

## 4. 现在就能开工的部分

**34 个场景里有 24 个没有阻塞项**，且第一步几乎零风险：

1. **只读快照接口**（R1/R2：枚举全部 G、某 P 队列明细）—
   数据源已确认存在（`task_registries` 强持有全部非终态 G）；
2. **`emit_event` 微基准** —— 成本最低，能最早回答「开了调试会慢多少」；
3. 接着做**序列化（text/ini/json）+ stdio/文件 sink** → `dump scheduler` 即可用。

**关键判断**：`P0（只读接口）+ P1（序列化）` 就是最小可用集，**除只读接口外完全不触碰调度器热路径**。

## 5. 我明确说"做不到"的 7 件事（不要期待）

1. 从运行时取**函数行号**（`context.cpp` 无位置 API，grep 计数 0）；
2. 显示**锁的持有者是谁**（`m_state` 不记录 owner，Go 风格所有权）；
3. **完整死锁环检测**（同上，缺 owner 信息）；
4. **任意行断点**（只能停在 **5 个挂起点**：Yield/Park/Io/通用）；
5. **跨机绝对时间对齐**（`timestamp` 是 `steady_clock`）；
6. **回放重建运行时**（事件流不足以恢复栈）；
7. **观测覆盖边界**：只能看到"已声明"的阻塞 → **「没显示阻塞」≠「没有阻塞」**。

另有两条易误用：**gdb 看不到 Fiber 用户栈**（只能看 M 的原生栈，Fiber 栈要用本模块的
`context_snapshot()`）；**`long_syscall_count` 是次数不是时长**。

## 6. 交付物地图

```
DebugTooling/
├── QUICKSTART.md                    ← 本文件
├── README.md                        任务、三步走、硬约束、交付结构
├── PROGRESS.md                      逐轮进度（18 轮，含证据）
├── FINAL-REPORT.md                  面向使用者的最终汇报
├── 01-existing-infrastructure.md    项目已有设施（源码证据）
├── design/
│   ├── 01-capability-matrix.md      能力矩阵（需求 × 现状 × 缺口）
│   ├── 02-dump-cli.md               dump 的 ini/json schema + 20 选项 + text 渲染示例
│   ├── 03-enumeration-and-registry.md  枚举全部 G 的机制（前置问题解法）
│   ├── 04-encoding-roadmap.md       编码走向：五层/模块/插件协议/多机/P0-P7
│   ├── 05-architecture-summary.md   一页总览 + 证据索引表
│   ├── 06-interface-signatures.md   15 个接口签名与硬约束
│   ├── 07-test-strategy.md          测试策略（含零开销证明方法）
│   └── 08-verified-details.md       逐轮读码核验与订正
└── scenarios/
    ├── 00-scenario-index.md         34 个场景索引 + 20 个 dump 选项
    ├── A-observability.md           A 只读观测 8
    ├── B-mutation.md                B 运行时变更 8
    ├── C-visualization.md           C 可视化输出 7
    ├── D-locks.md                   D 锁与同步 5
    ├── E-remote-and-tooling.md      E 远程/多机/gdb 6
    └── verification-matrix.md       34 场景 × 验证方式 × 判据 × 依赖接口
```
