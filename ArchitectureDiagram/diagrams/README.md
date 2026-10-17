# 模块一图（diagrams）

> 本目录是**每一模块一张图**的交付位置。
> 每份文件包含四类 Mermaid 图，对应使用者的四点要求：
>
> | 图 | 回答的问题 |
> |---|---|
> | ① 类架构 | 每个类**用了底层哪些能力**、**主要职责**是什么 |
> | ② 重要函数 | 关键函数的**功能**与**具体实现要点** |
> | ③ 向上层提供 | 本模块**最终向上层提供了什么功能** |
> | ④ 状态/流程 | 关键**状态流转**与**执行流程图**（如 Fiber 在 Scheduler 中如何流转与调度） |
>
> 图用 Mermaid（Markdown 直接渲染）；确有必要的形状/时序细节另附 ASCII。

## 模块图清单（按依赖方向，沿用 G1..G10 分组）

| 组 | 主题 | 图文件 | 状态 |
|---|---|---|---|
| G1 | 地基层（fiber / timer / parking_condition / context_backend） | `G1-foundation.md` | ✅ 已完成 |
| **G2** | **GMP 调度（Task/Scheduler/P/M/worker_loop/sysmon）** | **`G2-scheduler.md`** | **✅ 已完成** |
| G3 | 取消与错误（context / error） | `G3-context-error.md` | ✅ 已完成 |
| G4 | 并发原语（channel / future / sync） | `G4-concurrency.md` | ✅ 已完成 |
| G5 | IO 后端（io / io_backend / fd_manager / hook） | `G5-io-backend.md` | ✅ 已完成 |
| G6 | 网络基础（socket / address / framing / endian） | `G6-network-base.md` | ✅ 已完成 |
| G7 | 应用协议（http / websocket / sse） | `G7-protocols.md` | ✅ 已完成 |
| G8 | 横切设施（log / config） | `G8-log-config.md` | ✅ 已完成 |
| G9 | 工具与新手层（control_flow / go / event / thread_policy） | `G9-tools.md` | 待做 |
| G10 | 调试模块（本次新增的 go2cpp::debug） | `G10-debug.md` | 待做 |

> 说明：原 `modules/G1..G10-*.md` 是**分析讲解文档**（约 178KB 文字），保留作为图的**文字注解来源**；
> 本目录的图才是使用者要求的交付主体。
