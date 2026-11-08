# 低依赖架构（给后续开发者）

> **核心目标**：让新模块**只依赖必要的下层**，让使用者**只链接需要的模块**。
> **现状**：`json` / `platform` / `error` / `math` / `rand` **已经裁剪干净**——**可以单独用**。
> **这份文档是「架构契约」**：新增模块必须遵守分层规则；使用者按「已裁剪模块」的用法接入。

---

## 一、分层（L0 → L4，**只能依赖同层或下层**）

```
L0  零依赖（仅标准库）
    go2cpp_platform    WakeEvent / LastError / SteadyClock / AtomicRename / SetReuseAddr
    go2cpp_error       Error / ErrorPtr / Wrap / Join
    go2cpp_math        数学函数
    go2cpp_rand        随机数
    go2cpp_control_flow  Go 风格控制流

L1  只依赖 L0
    go2cpp_json        Parser / Document / Writer / JsonBuilder / Pointer
                       → 只依赖 go2cpp_error
    go2cpp_log         日志（→ 只依赖 L0，待核实）
    go2cpp_config      配置（→ 只依赖 L0）
    go2cpp_sync        Mutex / WaitGroup（→ L0）
    go2cpp_context     Context（→ L0）

L2  依赖 L1
    go2cpp_io          IOManager / IOBackend（→ L0/L1）
    go2cpp_channel     Channel（→ L1）
    go2cpp_scheduler   Scheduler（→ L1）
    go2cpp_fiber       Fiber（→ L1，**平台相关**）

L3  依赖 L2
    go2cpp_network     Socket / Address / HTTP（→ L2）⚠️ **可拆：net_addr 应该下沉到 L1**
    go2cpp_os          Process / Env / Dir（→ L1/io）

L4  组合层
    go2cpp_runtime     INTERFACE 聚合（**可选**，不强制）
```

**规则**：
1. **新增模块只能依赖「同层或更低层」**——**禁止反向依赖**（如 `json` 不能依赖 `network`）；
2. **能在 L0/L1 解决的不许上抬**（如「字符串处理」不该依赖 scheduler）；
3. **CMake 加 `if(TARGET ...)` 守卫**——**不能假设 target 一定存在**（`BENTO=OFF` 的教训）。

---

## 二、已裁剪模块（**可以直接用**）

| 模块 | 依赖 | 用法 |
|---|---|---|
| **`go2cpp_platform`** | **无** | `target_link_libraries(app PRIVATE go2cpp_platform)` |
| **`go2cpp_error`** | **无** | 同上 |
| **`go2cpp_json`** | **仅 `go2cpp_error`** | `target_link_libraries(app PRIVATE go2cpp_json)` |

### 最小示例（`json`）

```cpp
// main.cpp —— 只链接 go2cpp_json，不拖 network/fiber/scheduler
#include "go2cpp/json/parser.hpp"
#include "go2cpp/json/writer.hpp"

int main() {
    auto doc = go2cpp::json::Parse(R"({"a": 1, "b": "x"})");
    if (!doc) return 1;
    return go2cpp::json::Write(*doc).empty() ? 2 : 0;
}
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(minimal_json CXX)
add_subdirectory(<path-to-go2cpp> go2cpp)
add_executable(minimal_json main.cpp)
target_link_libraries(minimal_json PRIVATE go2cpp_json)  # 只这一个！
```

**验证**：`cmake --build` 通过，且 **`ldd`/`nm` 里没有 `libgo2cpp_network.so` / `libgo2cpp_fiber.so`**。

---

## 三、给新模块作者的四条规则

| # | 规则 | 理由 |
|---|---|---|
| **R1** | **先问「我能只依赖 L0/L1 吗？」** | 依赖 L3 的模块**无法被轻量场景使用** |
| **R2** | **public header 里不暴露下层实现类型** | 否则「只需 header」的人被迫链接实现库（PIMPL/前向声明） |
| **R3** | **`target_link_libraries` 用 `PRIVATE` 除非类型出现在 public API** | `PUBLIC` 会传染依赖 |
| **R4** | **新模块必须带 `examples/minimal/<module>/`** | **可裁剪性是验收项**，不是「有空再说」 |

---

## 四、待改善（按优先级）

| # | 事项 | 收益 |
|---|---|---|
| **P0** | **`go2cpp_network` 拆出 `go2cpp_net_addr`**（`address.cpp`/`byte_array.cpp`，零依赖） | 只想解析地址的人**不拖 scheduler** |
| **P0** | **`go2cpp_os_monitor` 去掉 `scheduler`/`channel`**（待验证） | 采集器**可用于简单进程** |
| **P1** | **每个已裁剪模块的 `examples/minimal/`** | **可验证、可复制** |
| **P2** | **`GO2CPP_MODULE_<NAME>` 选项** | 只建需要的模块（CI 时间 ↓） |

---

## 五、如何验证「真的可裁剪」

**三步**（每个模块都要过）：

1. **写最小示例**（`examples/minimal/<module>/`，**只链接该模块**）；
2. **构建 + 运行**（`cmake --build` + `./minimal_<module>`）；
3. **检查链接产物**：
   ```bash
   ldd minimal_json | grep -c 'libgo2cpp_network\|libgo2cpp_fiber\|libgo2cpp_scheduler'  # 必须是 0
   ```

**任一不满足 → 该模块「未裁剪」**，不能写进本文档的「已裁剪模块」表。

---

## 六、与跨平台的关系

> **`platform/` 是这套架构的基石**——它把 `WakeEvent`/`LastError`/`SteadyClock`/`AtomicRename`/`SetReuseAddr` 的**平台差异**全部收口，
> **上层模块不用再写 `#if defined(_WIN32)`**。
>
> **所以「可裁剪」和「跨平台」是一体的**：**低依赖 = 少平台分支 = 好裁剪**。
