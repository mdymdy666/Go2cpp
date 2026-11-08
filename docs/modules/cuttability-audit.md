# 可裁剪性审计（第一版，只读分析）

> 目标：**每个模块能单独使用，不引入无关模块**。
> 方法：读 `cmake/Go2CppModules.cmake` 的 `target_link_libraries`（**真实链接**，不是文档声称）。
> 状态：**只读审计，未改代码**（W13 正在改 `network`，依赖图会变）。

---

## 一、已确认「干净可裁剪」的模块

| 模块 | 依赖 | 评价 |
|---|---|---|
| **`go2cpp_json`** | `go2cpp_error` | ✅ **干净**——只依赖错误基础设施，**可单独用** |
| **`go2cpp_platform`** | 无（仅标准库） | ✅ **零依赖**——`WakeEvent`/`LastError`/`SteadyClock`/`AtomicRename`/`SetReuseAddr` |
| **`go2cpp_error`** | 无（仅标准库） | ✅ **零依赖** |
| **`go2cpp_math`** | 无 | ✅ 零依赖 |
| **`go2cpp_rand`** | 无 | ✅ 零依赖 |

**结论**：**`json` 已经可以单独用**（`target_link_libraries(xxx PRIVATE go2cpp_json)` 即可，**不拖任何重组件**）。

---

## 二、重依赖模块（需要评估能否拆）

| 模块 | 依赖 | 是否合理 |
|---|---|---|
| **`go2cpp_network`** | `io` + `context` + `scheduler` + `sync` | ⚠️ **半合理**：非阻塞 IO 确实需要事件循环（`io`/`scheduler`），但**纯地址解析**（`address.cpp`）/`byte_array` **不需要** |
| **`go2cpp_os`** | `config` + `io`（PRIVATE） | ⚠️ `config` 是 PUBLIC（`os` 的 API 暴露了 config 类型？**待验证**） |
| **`go2cpp_os_monitor`** | `network` + `scheduler` + `channel` | 🚩 **可疑**：采集器（CPU/内存/磁盘）**为什么要 scheduler**？ |

---

## 三、可裁剪性问题清单

| # | 问题 | 影响 |
|---|---|---|
| **A1** | **`go2cpp_network` 把 `address`/`byte_array` 和 `socket`/`http` 捆在一个 target** | 只想用 `Address` 的人被迫拖入 `scheduler`/`fiber` |
| **A2** | **`go2cpp_os_monitor` 依赖 `scheduler`** | 采集器是「定时读 `/proc`」，**不需要协程调度** |
| **A3** | **`go2cpp_runtime` 是 INTERFACE 聚合**（把所有模块捆一起） | 用 `go2cpp_runtime` 的人**无法只取一部分**——**但可以绕开它，直接链接单模块** |
| **A4** | **`GO2CPP_MODULE_TARGETS` 是整体列表** | 无法 `-DGO2CPP_MODULE_JSON=ON` 只建一个模块 |
| **A5** | **`go2cpp_json` 的 `parse/scan/escape` 是否 header-only** | 如果 `ParseJson` 需要 `.cpp`，那「只用头文件」不可能——**已验证：需要 `src/json/*.cpp`** |

---

## 四、建议的拆分（按优先级）

| 优先级 | 动作 | 收益 |
|---|---|---|
| **P0** | **给 `go2cpp_network` 拆出 `go2cpp_net_addr`**（`address.cpp` + `byte_array.cpp`，**零依赖**） | 只想用 `Address`/`ByteArray` 的人**不用拖 scheduler** |
| **P0** | **`go2cpp_os_monitor` 去掉 `scheduler`/`channel`**（如果验证确实不需要） | 采集器**可独立用于简单进程** |
| **P1** | **`go2cpp_json` 增加 `examples/minimal/json/`** | **证明可单独用**（当前**已具备条件**） |
| **P1** | **`go2cpp_platform` 增加 `examples/minimal/platform/`** | **证明零依赖** |
| **P2** | **`GO2CPP_MODULE_<NAME>` 选项** | 只建需要的模块（**CI 时间 ↓**） |
| **P2** | **`go2cpp_runtime` 改为可选** | 不强制聚合 |

---

## 五、验收（第一版）

| # | 标准 | 现状 |
|---|---|---|
| V1 | `json` 能单独链接 | ✅ **已具备**（只依赖 `error`） |
| V2 | `platform` 能单独链接 | ✅ **已具备**（零依赖） |
| V3 | `network` 拆出 `net_addr` | ❌ **待做** |
| V4 | `os_monitor` 去 scheduler | ❌ **待做** |
| V5 | 每个模块的最小示例 | ❌ **待做**（`examples/minimal/`） |

---

## 六、下一步

1. **等 W13（`int → intptr_t`）完成**（它会改 `network` 的类型，**依赖图会变**）；
2. **写 `examples/minimal/json/`**（**最简单，1 个文件 + CMakeLists，能立刻证明「json 可单独用」**）；
3. **拆 `net_addr`**（`address.cpp` + `byte_array.cpp` → 新 target，**零依赖**）；
4. **验证 `os_monitor` 是否真不需要 `scheduler`**（**看代码里调用了什么**）。
