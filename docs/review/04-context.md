# 04 context 模块问题文档

> 基线：`bd99603`｜关键文件：`src/concurrency/context.cpp`（2103 行）、`include/go2cpp/concurrency/context.hpp`（532）
> 阅读状态：待读

## 1. 模块速览

- 职责：取消树（`WithCancel/WithTimeout/WithDeadline`）、`DoneSignal`、deadline/value Context、`ContextRollback` 补偿。
- 入口/主题：
  - 取消传播迭代工作队列：`DrainCancellation`（`context.cpp:650`）
  - `WithCancel`（`:1902`）、`WithDeadline`（`:1921`）、`WithTimeout`（`:1993`）
  - `DoneSignal::Wait/WaitFor/WaitUntil` 走 `ParkingCondition`
  - 根 Context 的 Done 非空但永不触发（与 Go nil channel 的明确差异）
- 已知坑：`Duration::max()` 不饱和会回绕；`Context::Now()` 可注入而 TimerService 硬编码 steady_clock。

## 2. 问题记录

（待用户提问）

## 3. 已确认缺陷

暂无（待审）。

## 4. 待读 / 待确认

- [ ] 取消回调在锁外执行、且可能由 TimerService 线程触发，与 `ContextRollback` 的 undo 顺序
- [ ] `DoneSignal` 先 notify 再跑回调的可见性（`Done()` 返回不代表 undo 完成）
- [ ] 父子弱登记/强锚点的生命周期（取消后清理是否彻底）

## 5. 关联

- 导读：`docs/code-review-guide.md` §4/§5
- 文档：`docs/context_guide.md`、`docs/context_rollback.md`
