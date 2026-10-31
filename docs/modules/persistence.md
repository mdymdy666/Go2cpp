# persistence 模块（原型，tools/persistence）

## 1. 定位

`tools/persistence` 是**持久化写路径原型**（不进入运行时库）：解决「录制/快照写入
放大」——把逐条 `write` 换为 **MPSC 队列 + 批量提交 + fdatasync**。源：
`tools/persistence/**`（独立 CMake 工程）。

## 2. 核心设计

| 组件 | 说明 |
|---|---|
| MPSC 队列 | 多生产者（任意线程）→ 单消费者（写线程）；批量出队 |
| 段（segment） | 8 MiB 定长段文件；段满滚动，manifest 记录段序 |
| 112B 定长编码 | 事件定长编码（类型/ts/gid/fd/bytes），零解析开销 |
| 批量写 + fdatasync | 一次 `writev` 多事件，周期性 `fdatasync`（默认 1s/4MiB） |
| 崩溃恢复 | manifest + 段尾校验：截断/重放未完成段 |

## 3. API

| API | 位置 | 说明 |
|---|---|---|
| `Writer::Open(dir, options)` | `tools/persistence/include/*.hpp` | 打开写入器 |
| `Writer::Write(event)` | 同上 | 入队（MPSC，非阻塞） |
| `Writer::Flush()/Sync()` | 同上 | 批量落盘/强制 fdatasync |
| `Writer::Close()` | 同上 | 落盘 + 写 manifest |
| `Reader::ReadSegments(dir)` | 同上 | 段范围读（用于回放/校验） |

## 4. 实测数据（原型基准）

| 场景 | 相对 recorder 基线 |
|---|---|
| 1 线程写入 | **2.96x** |
| 4 线程写入 | **13.83x** |
| 段范围读 | **9.9x** |

（来自旧 `docs/persistence-benchmark.md`，已合并至本文。）

## 5. 边界与限制

- **原型**：未接入运行时录制器（`tools/replay` 仍用独立 writer）；
- 单消费者写线程：极高并发下队列可能成为瓶颈（容量满即阻塞生产者）；
- fd 固定打开（段滚动时切换），不处理磁盘满/只读；
- 无校验和（段尾只有长度/魔数），位翻转不可检测。

## 6. 测试

`tools/persistence` 独立构建（`build.sh` 或 CMake），基准脚本
`tools/persistence/bench.cpp`（对比 recorder 基线）；无 ctest 注册。

## 7. 相关

- 回放：[replay-service](../replay-service.md)（`tools/replay`，G2CRPLY v5）；
- 工具总览：[tools.md](tools.md)；
- 旧文档 [persistence-benchmark.md](../persistence-benchmark.md) 已合并至本文。
