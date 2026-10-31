# 验收记录（JinShanProject ↔ Go2Cpp）

- **版本**：v2（2026-10-31 20:40，对齐 `jinshan-requests.md` v2）
- **维护**：JinShan agent 填写结论；Go2Cpp agent 只读
- **原则**：没有原始输出/数据不算通过；「编译通过」≠「行为正确」

---

## 一、状态总表（v2 五条）

| REQ | 内容 | 类型 | 优先级 | 状态 | 验收日期 | 证据 |
|---|---|---|---|---|---|---|
| REQ-001 | **FrameCodec**（可配置帧编解码 + 拆包器） | 可抽象 | P0 | ⏳ 待实现 | — | — |
| REQ-002 | **RelayPipe**（双向字节中继 + 背压 + 慢 peer） | 可抽象 | P0 | ⏳ 待实现 | — | — |
| REQ-003 | **Hub**（key → 多连接注册表） | 可抽象 | P1 | ⏳ 待实现 | — | — |
| REQ-004 | 有界队列 / 背压原语 | 通用 | P0 | ✅ 已有（补文档+示例） | — | — |
| REQ-005 | 超时 / 取消 / 租约 / 心跳 | 通用 | P1 | ⚠️ Context 已有；Lease/Heartbeat 缺 | — | — |

**已移出（不再跟踪）**：旧 REQ-004/005/006/007/008/011/012/013 —— 均为 RDRL/RDCH/配对/配额专属，JinShan 自己做（详见 `jinshan-requests.md` §3/§4）。
**明确不需要**：P2P（CON-09）。

---

## 二、验收记录格式

```markdown
### REQ-00x: <名称>

- **验收人**：
- **日期**：
- **环境**：WSL2 发行版 / 编译器 / commit
- **用例**：
  1. <正常路径>
  2. <边界值>
  3. <畸形输入>
- **原始输出**：<命令 + 输出摘要 / 日志路径>
- **结论**：通过 / 不通过（不通过写缺哪项）
- **补充**：不通过时在 requests 追加 REQ-00y
```

---

## 三、JinShan 侧的替代方案（Go2Cpp 交付前不阻塞）

| 能力 | JinShan 当前实现 | 交付后如何切 |
|---|---|---|
| 帧编解码 | `product/common/rdrl`（C++，28/28 单测） | 用 FrameCodec 重写内部，对外接口不变 |
| 中继管道 | `product/relay`（Go，93 PASS / 1 SKIP） | 用 RelayPipe 替换 pump |
| 连接表 | relay `sessionTable`（Go） | 用 Hub 替换 |
| 队列/背压 | `Chan` 已在用 | 补 TrySend 验收 |
| 超时/心跳 | context + 手写 ticker | 换 Lease/Heartbeat |

**原则**：接口层（`rd::net` / `rd::sync`）先立住，底层实现随时换。

---

## 四、验收前置条件（Go2Cpp 侧）

1. `collab/examples/` 里给**完整可编译 `main`**，不是伪代码；
2. 测试覆盖正常 + 边界 + 畸形；
3. Linux（WSL2）通过即可交付；Windows 由 JinShan `rd::net` 适配兜底；
4. 不引入第三方库；
5. 每个 REQ 附一份「最小用法」示例（10–30 行）。
