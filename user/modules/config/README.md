# 配置（config）

> **一句话定位**：INI 配置、强类型配置项、热加载与运行时快照。
> 本文是「模块 → 能力 → 异常」手册；[配置热更新与运行时快照](../../deep/10-observability.md) · [存储侧配置](../../deep/04-storage.md)


> 配置模块的示例：从「读一个 INI 文件」到「进程级配置中心 + 文件监听 + 两阶段提交」。

## 能力清单

配置模块解决四类问题，各有一层抽象：

| 层 | 类型 | 解决什么 |
| --- | --- | --- |
| 解析 | `config::IniFile` | 把 `[section] key=value` 文本解析成可查询的表（**不做类型转换、不做同步**） |
| 类型 | `config::ConfigVar<T>` | 强类型配置项：名字/描述/默认值/取值/设值/校验/监听/字符串往返 |
| 中心 | `config::Config` | 进程级注册表 + 文件监听 + **两阶段提交**（预检 → 写入 → 统一通知） |
| 快照 | `config::RuntimeConfig` | 调度器/日志/定时器阈值的**只读**一致视图，`FromIni` 可从内存构造 |

另有 `ConfigEncode`/`ConfigDecode`（`config_codec.hpp`）把结构体配置委托给
Reflect 的字段反射 + JSON，用于旧配置兼容。

## 底层架构

```
   app.ini / 内存 INI
          │
          ▼
   ┌─────────────┐   查询     ┌──────────────────┐
   │  IniFile    │ ─────────► │ ConfigVar<T>      │  强类型 + 校验 + 监听
   └─────────────┘            └────────┬─────────┘
          │                            │ 注册
          │  watch 线程                 ▼
          │                   ┌──────────────────────┐
          └──────────────────►│  Config（进程级注册表）│
                              │  Lookup / List       │
                              │  两阶段提交 + 通知     │
                              └──────────┬───────────┘
                                         │ 只读快照
                                         ▼
                              ┌──────────────────────┐
                              │  RuntimeConfig       │
                              │  scheduler.* / log.* │
                              └──────────────────────┘
                                         │
                     ┌───────────────────┼───────────────────┐
                     ▼                   ▼                   ▼
              Scheduler 启动参数     LoggerManager       hook.* 开关
```

**两阶段提交的意义**：热加载时先对所有待改项**预检**（类型/范围/校验器），
全部通过才写入，再统一通知监听者。任一项不合法时**保留旧快照**，不会出现
「改了一半」的中间态。

**线程模型**：`IniFile` 无锁、单线程使用；`ConfigVar`/`Config` 内部有锁，
可被多个线程读写；文件监听由独立线程驱动，通知在**监听线程**上回调。

## 最小示例

```cpp
#include "go2cpp/config.hpp"
#include <iostream>

int main() {
    using namespace go2cpp::config;

    IniFile ini;
    std::string error;
    if (!ini.Parse("[server]\nport = 8080\nhost = 127.0.0.1\n", &error)) {
        std::cerr << "解析失败: " << error << '\n';
        return 1;
    }

    const auto port = ini.Get("server", "port", "0");        // 缺失键返回默认值
    const bool has  = ini.Has("server", "port");

    ConfigVar<int> workers("workers", 4, "工作线程数");       // 名字 / 默认值 / 描述
    (void)workers.SetValue(8);
    const int current = workers.GetValue();
    (void)workers.FromString("16");                          // 文本往返，便于从 INI 赋值

    std::cout << port << ' ' << has << ' ' << current << ' ' << workers.GetValue() << '\n';
    return 0;
}
```

运行时快照（`runtime_config_demo.cpp` 的写法）：

```cpp
go2cpp::config::IniFile ini;
ini.Parse("[scheduler]\nprocessor_count = 4\n[log]\nlevel = info\n", &error);
go2cpp::config::RuntimeConfig runtime;
const bool ok = go2cpp::config::RuntimeConfig::FromIni(ini, &runtime, &error);  // 不需要文件
```

进程级配置中心（`config_center_demo.cpp` 的写法）：

```cpp
auto& center = go2cpp::config::Config::Instance();
auto port = center.Lookup<int>("demo.port", 8080, "监听端口");  // 注册默认值
// 同名再次 Lookup 返回同一实例，不会把值重置回默认值
(void)center.LoadFromIni(ini);                                  // 预检 → 写入 → 统一通知
```

## 3 个常见异常

### 1. 热加载后「配置改了一半」
- **现象**：批量加载失败，部分值已变、部分没变，运行行为前后不一致。
- **原因**：`Config::LoadFromIni` 走两阶段提交（预检 → 写入 → 统一通知），任一项校验失败整批回滚；但**已发出的通知不会撤回**。
- **处理**：监听者按「最终快照」重建状态，不要按增量事件累加；失败后重新读一次 `RuntimeConfig`。

### 2. 缓存了旧的 `RuntimeConfig` 引用
- **现象**：热加载返回成功，但调度器/日志阈值没变。
- **原因**：`RuntimeConfig` 是**构造时快照**，配置变更不会刷新已有对象。
- **处理**：每次重载后重新 `FromIni` 构造，不跨重载持有引用或指针。

### 3. 在监听回调里再次改配置
- **现象**：卡死、死锁或无限递归。
- **原因**：通知在**监听线程**上回调，回调内再触发变更会重入 `Config` 的锁。
- **处理**：回调只做投递（无锁队列 / 原子标志），真正的变更回到主循环执行。

## 示例清单

| 示例 | 演示什么 | ctest |
| --- | --- | --- |
| `config_demo` | INI 解析、默认值、`ConfigVar` 强类型读写与字符串往返 | ✅ |
| `config_full` | 7 个场景：分段解析/默认值/ConfigVar/校验/监听/编码往返 | ✅ |
| `runtime_config_demo` | 从内存 INI 构造 `RuntimeConfig` 并校验 | ✅ |
| `runtime_config_full` | 7 个场景：scheduler/log 组字段、边界校验、快照一致性 | ✅ |
| `config_center_demo` | 注册默认值，再用 INI 覆盖（`Config` 中心最小用法） | ✅ |
| `config_center_full` | 7 个场景：Lookup 幂等、枚举、两阶段提交、热重载通知 | ✅ |
| `config_app_ini` | 静态注册 → 加载 `app.ini` → 运行时调整 → 导出 `app.ini` | ❌（写文件，未注册） |

```sh
cmake --build /tmp/build-user --target go2cpp_config_demo -j && /tmp/build-user/go2cpp_config_demo
```

## 源码位置

| 能力 | 头文件 | 实现 |
| --- | --- | --- |
| INI 解析 / ConfigVar / Config / RuntimeConfig | `include/go2cpp/config/config.hpp` | `src/config/config.cpp` |
| 结构体配置编解码 | `include/go2cpp/config/config_codec.hpp` | `src/config/config_codec.cpp` |
| 文件监听 | `include/go2cpp/config/watch.hpp` | `src/config/config.cpp` |
| 动态库清单/重载 | `include/go2cpp/config/so_manifest.hpp`、`so_reloader.hpp` | `src/config/so_manifest.cpp`、`so_reloader.cpp` |

配置格式与服务端示例的用法见 `user/modules/app/web_server_full/`（`server.ini`）。

## 注意事项

- **`IniFile` 只解析、不校验**：类型转换、范围检查、同步都要调用方自己做；
  要强类型就用 `ConfigVar<T>`。
- **变更是两阶段的**：`SetValue` 失败时**旧值保持不变**；批量加载失败时整批回滚，
  但**已经发出的通知不会撤回** —— 监听者要按「最终快照」而不是「增量事件」写逻辑。
- **通知在监听线程上回调**：回调里不要做长阻塞，也不要再触发一次配置变更（会重入）。
- **`RuntimeConfig` 是快照**：构造后不再变化；改配置后要重新构造，不要缓存旧引用。
- **`ConfigVar` 的求值顺序坑**：`GetValue()` 与 `FromString()` 不要写在同一个
  `<<` 链里（示例首版踩过），先取值再赋值。

## 深入阅读

- [配置热更新与运行时快照](../../deep/10-observability.md)
- [存储侧配置](../../deep/04-storage.md)
- [交接摘要（docs）](../../docs/10-observability.md)
- 源码位置：`include/go2cpp/config/`、`src/config/`
- 示例目标名 = `go2cpp_<文件名>`；新增示例需同时登记 `cmake/Go2CppTests.cmake` 与本文清单。
