#pragma once

/**
 * @file graph.hpp
 * @brief `go2cpp-cli graph`：并发结构状态图（wait-for / resource / fiber-state /
 *        gmp / sequence）的入口。
 *
 * @details 本子命令**不链接运行时库**：
 *   - 进程模式通过既有 `DebugServer`（`GO2CPP-DEBUG/1` 文本协议，`VIEW <name>`）
 *     与 `Doctor`（`G2CD/1` 帧协议，`deadlock-report json=1`）读取结构化数据；
 *   - 文件模式（`--replay <file>`）只解析 JSON dump 或 `# go2cpp-trace` 文本；
 *   - 图本身是纯字符串生成（Mermaid / Graphviz DOT / JSON），不引入任何依赖。
 *
 * ## 数据源与降级契约
 *
 * | 图 | 主数据源 | 缺失时 |
 * |---|---|---|
 * | wait-for | Doctor `deadlock-report` 的 edges（owner_gid/waiters） | 用 `VIEW history` 的事件推导 owner/waiter；两者都缺则输出带提示的空图 |
 * | resource | `VIEW resources` + `VIEW history <id>` | 用 Doctor 的 mutex edges；再缺则提示 |
 * | fiber-state | `VIEW sched` 的 flow（事件环） | 尝试 `VIEW gmp` + `VIEW park` 的快照；再缺则提示 |
 * | gmp | `VIEW gmp` | 尝试 Doctor `status` 的 machines/processors；再缺则提示 |
 * | sequence | 多个资源的 `VIEW history <id>` | `--replay` 的事件数组 / trace 文本；再缺则提示 |
 *
 * 数据源缺失时**仍然输出一份图**（含可见的 note 节点），并以退出码 3 明确
 * 「这是降级输出」；部分缺失（例如 Doctor 不可用但事件历史可用）仍返回 0，
 * 并在图的 notes 中记录来源与缺口。
 *
 * ## 边界与陷阱
 *
 * - 事件环是**有界窗口**：真实死锁现场里，park/mutex 事件可能已被高频
 *   timer/PC 事件冲刷掉。此时图只展示窗口内可见的关系，notes 会写明
 *   `window.truncated`，不伪造「完整等待图」。
 * - `VIEW resources` 会列出数千个内部 `ParkingCondition`；默认按「有统计或
 *   是有名字的原语」过滤并设上限（`--max-resources`），截断写入 notes。
 * - 图的节点/边都来自真实字段；不做「根据 gid 猜 owner」这类补全。
 */

#include <string>
#include <vector>

namespace go2cpp::cli {

/// 执行 `graph` 子命令；返回进程退出码。
int RunGraph(const std::vector<std::string>& args);

}  // namespace go2cpp::cli
