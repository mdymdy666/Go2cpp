# TODO（2026-11-07 收集）

> 按「谁 / 什么 / 为什么」记录，做完打勾。
> 优先级：P0 = 阻塞/正确性，P1 = 性能/体验，P2 = 完善。

---

## 一、JSON 库（`JinShanHomeWork/week2/repo/LessonCode/week02/homework`）

| # | 事项 | 优先级 | 说明 |
|---|---|---|---|
| J1 | **深嵌套优化** | P1 | 实测 **2054 ns / 100 层**，simdjson 553 ns（**慢 3.7×**）——真实短板 |
| J2 | **数字密集优化** | P1 | 实测 148 ms / 117 KB，simdjson 102 ms（**慢 1.4×**）——真实短板 |
| J3 | Reflect `bench_reflect` 注册表查询 **+566%** | P1 | 旧版有 `thread_local` 缓存，Go2Cpp 无 → 加回缓存 |
| J4 | Reflect 3 个缺失符号（`kDecimalType` / `TypeTraits<Decimal>` / `TypeName<T>()`） | P2 | 当前保留库实现 + 标「待合并」 |
| J5 | `bench_error` `WalkErrors` **+47%**（分配 16→200014） | P2 | Go2Cpp 版遍历更重（不在热路径） |
| J6 | **合并进 Go2Cpp**（`docs/plans/json-lib-merge.md` 已写方案） | P1 | 作为「数据解析板块」；命名空间 `extened::json` → `go2cpp::json` |
| J7 | `ReadMe.md` 编译命令补 `escape/writer/pointer.cpp` | P2 | d4a9d307 已补 `src/go2cpp` 部分 |

---

## 二、douyin（`app/douyin/`）

| # | 事项 | 优先级 | 说明 |
|---|---|---|---|
| D1 | **举报接口 `\uXXXX` 转义** | P0 | 中文存成 `u6f14...`（JSON 库已修，**douyin 自己的 JSON 解析还没接**） |
| D2 | 封面纯黑重抽 | P2 | demo3 等 |
| D3 | 直播/私信/举报 **补更多演示数据** | P2 | 已有 2 房间 / 3 会话 / 4 举报 |
| D4 | `app/douyin\r/` 垃圾目录（又出现） | P2 | 加 `.gitignore` |
| D5 | 待核实功能（WS Resume / 视频压缩 / 草稿箱） | P2 | 摘要声称做过，代码零命中 |

---

## 三、跨平台（Windows）

| # | 事项 | 优先级 | 说明 |
|---|---|---|---|
| W1 | **`platform/` 三件**（`WakeEvent`/`LastError`/`SteadyClock`） | P0 | **头文件已写**（`include/go2cpp/platform/`），**cpp 未写** |
| W2 | hook 白名单（Windows 上只 hook 可模拟的） | P0 | 解 `hook.cpp:64` 的 `#error` 硬阻断 |
| W3 | IOBackend Windows 后端（WSAPoll 短期 / IOCP 长期） | P0 | 解 `DefaultBackendName()` 返回空的硬阻断 |
| W4 | 进程子层（`fork` → `CreateProcess`） | P1 | `os::pool` 依赖 |
| W5 | `/proc` 依赖降级（monitor/debug 标 Linux-only） | P2 | 3 个模块 |
| W6 | `rename`/`unlink` 语义适配（WAL/SST/日志轮转） | P1 | `MoveFileEx` + `FILE_SHARE_DELETE` |

---

## 四、Go2Cpp 基座

| # | 事项 | 优先级 | 说明 |
|---|---|---|---|
| G1 | **`Context` 回调列表换 `KInlineVector<Callback,4>`** | P1 | 实测小容器快 5–9×；每请求一个 Context = 每请求一次 malloc |
| G2 | 非流式 RPC Cancel（已修，待复验） | — | `0c6ac1a` |
| G3 | `bench_tunnel` / `tests/relay/test_relay_review.cpp` 注册 CMake | P2 | 未注册 |

---

## 五、流程 / 规范

| # | 事项 | 优先级 | 说明 |
|---|---|---|---|
| P1 | **`READ-ME-FIRST.md` 已放旧副本**（`E:\CodexWorkspace\Go2Cpp\`） | ✅ 完成 | 防 agent 走错路径 |
| P2 | 删除旧副本 `E:\CodexWorkspace\Go2Cpp` | P2 | 只有 log/Start/Testing，删了不会再误入 |
| P3 | `docs/plans/next-day-plan.md` 的清单更新 | P2 | 与本 TODO 合并 |
