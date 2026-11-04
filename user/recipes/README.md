# recipes/ —— 跨模块组合场景

> 单模块手册回答「这个 API 怎么用」；recipes 回答「几个模块拼起来，线上会怎么坏」。

## 场景索引

| # | 场景 | 组合模块 | 关键风险 |
| ---: | --- | --- | --- |
| 01 | [HTTP 服务 + 优雅关闭](01-http-graceful-shutdown.md) | network + concurrency + app | 停 accept 时在途请求被截断、Scheduler 先关导致 Fiber 永久 park |
| 02 | [配置热更新驱动日志级别](02-config-hot-reload-logging.md) | config + logging | 监听线程里重入、旧快照引用、通知丢失 |
| 03 | [有界队列做背压](03-backpressure-bounded-queue.md) | concurrency + app | 无界队列 OOM、关闭时生产者永久阻塞 |

## 写法约定

每篇固定五节：

1. **场景**：要解决什么问题、为什么不能只用单模块示例；
2. **组合方式**：模块 → 职责 → 谁调用谁（可用 ASCII 图）；
3. **最小实现**：指向真实示例文件 + 关键代码片段（5–15 行）；
4. **验证**：怎么手工触发正常路径与失败路径；
5. **常见坑**：至少 3 条，写「现象 / 原因 / 处理」。

> 所有片段都来自仓库现有示例或头文件；新增 recipe 时同步更新本索引表。
