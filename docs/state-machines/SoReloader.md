# SoReloader 状态机

- 源文件：`src/config/so_reloader.cpp`（类定义在 `include/go2cpp/config/so_reloader.hpp`）
- 关键函数：`Reload()`、`ForceRescan()`、`SoManifest::Validate()`、
  `ConfigVarBase::NotifyPending()`、`ConfigVarBase::RollbackPending()`
- 设计文档：`docs/so-reload-design.md`

## 状态转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Idle | `Reload()` 且 `revision < committed_revision` | Rejected | 旧 manifest，直接丢弃，不触碰任何 SO |
| Idle | `Reload()` 且 `revision == committed_revision` | Idle | 幂等命中，返回 `ok=true, applied=false` |
| Idle | `ForceRescan()` 且 `revision == committed_revision` | Validated | E3 溢出重扫：跳过幂等短路，manifest 全部 SO 进入变更集 |
| Idle | `Reload()` 且 `revision < highest_seen` | Rejected | 已有更高 revision 进入，旧事务不加载 |
| Idle | `Reload()` 且静态校验失败 | Rejected | `Validate()` 失败，loader 0 次 |
| Idle | `Reload()` 且静态校验通过 | Validated | 变更集、A1 依赖一致性、拓扑排序均已就绪 |
| Validated | `SoLoader()` 返回 false | RollingBack | 只对本次已成功加载的句柄调用 `SoUnloader` |
| Validated | 全部 `SoLoader()` 返回 true | Staged | 已加载的新句柄待提交；尚未调用 activator |
| Staged | `ApplyString()` 返回 false | RollingBack | 逆序 `RollbackPending` + 卸载新句柄 |
| Staged | `revision < highest_seen` | RollingBack | 被更高 revision 抢占 |
| Staged | 全部 `ApplyString()` 成功且仍是最大 revision | Notified | 统一调用 `NotifyPending()`，监听器看到完整一代 |
| Notified | `SoActivator()` 执行 | Committed | vtable 原子切换；本层无法回滚此阶段 |
| Notified | `SoActivator()` 抛异常 | Committed | 异常被吞掉；配置值已发布，契约要求 activator 不失败 |
| Committed | 提交 `current_` / retention | Idle | 返回 `ok=true, applied=true` |
| RollingBack | 回滚完成 | Rejected | `current_` / retention 不变，旧版本继续服务 |

## 不变量

- `highest_seen` 在进入 `m_reload_mutex` **之前**单调抬高；
- 只有 `revision == highest_seen` 的事务能到达 Notified；
- `ForceRescan` 只跳过 `revision == committed` 的幂等短路，revision 回退
  与 highest_seen 栅栏仍然生效；
- `RollingBack` 只卸载本次 `loaded_` 列表里的 spec，**从不**卸载 `current_` 的旧句柄；
- `NotifyPending()` 一定在 `SoActivator()` 之前；
- `Committed` 之后不会再执行 `RollbackPending()`。

## 非法转化

- `Rejected -> Committed`：不存在；失败事务不会修改 `current_`/retention；
- `Notified -> RollingBack`：禁止；配置值已经对监听器可见，回滚会制造
  「监听器见过新值、ConfigVar 又变回旧值」的撕裂，本层选择不回滚；
- `Committed -> RollingBack`：禁止；下一次更新必须使用更大的 `revision`（
  `ForceRescan` 成功时 revision 可以不变，但同样不会回滚已提交的旧句柄）；
- `ForceRescan -> Idle`（幂等命中）：不存在；强制入口必须实际走一遍
  `Validate` 与 `SoLoader`。
