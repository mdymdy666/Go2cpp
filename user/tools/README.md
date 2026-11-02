# user/tools —— Go 标准库对应物（fmt / strings / strconv / streams / time / error / defer）

> 「Go 的常用工具包」在本项目里的对应实现：语义 1:1 对齐 Go 1.23，独立实现，
> 不依赖第三方运行时。

## 这个模块是什么

把 Go 日常最常用的几个标准库搬到 C++17，**语义对齐而不是「差不多的替代品」**：

| 本模块 | Go 对应 | 解决的问题 |
| --- | --- | --- |
| `go2cpp::fmt` | `fmt` | `%v`/`%q`/`%w` 指令集与 Go 一致的格式化 |
| `go2cpp::strings` | `strings` | `Split/Cut/Fields/TrimSpace/Builder` 等，含 `Count(s,"")==runes+1` 这类反直觉边界 |
| `go2cpp::strconv` | `strconv` | `base=0` 进制推断、下划线规则、`ErrRange` 钳制值 |
| `go2cpp::streams` | `io` | `Reader/Writer/Copy/CopyN/ReadFull/LimitReader/MultiReader/TeeReader` |
| `go2cpp::time` | `time` | 强类型 `Duration`（int64 ns）、`String()` 格式、单调时钟 `Time`、协作式 `Sleep` |
| `go2cpp::Error` / `errors::` | `errors` | 不可变错误链 + `Is`/`As`/`Unwrap`/`Join` |
| `defer` / `panic` / `recover` | 语言关键字 | 显式状态式控制流（**不伪造 C++ 栈跳转**） |

为什么自研而不是移植：每个头文件的文件级注释都列了候选库与取舍
（例如 `strings.hpp` 对比了 abseil/boost/fmt/strutil）。结论一致：
**现成库的语义是它们自己的，与 Go 1:1 对齐的成本高于自研**，而底层能力
（`std::from_chars`/`std::to_chars`）可以直接复用。

## 底层架构

```
        业务代码
           │
  ┌────────┴─────────────────────────────────────────────┐
  │ fmt（格式化）  ─── 复用 ───► strconv（数字/引号）      │
  │      │                        │                      │
  │      └────── 复用 ───► error（%w 包装链、ErrorPtr）    │
  └────────┬─────────────────────────────────────────────┘
           │
  ┌────────┴───────────────┐   ┌──────────────────────────┐
  │ strings / strconv      │   │ streams（Reader/Writer）  │
  │ （header-only，纯函数） │   │ （header-only，接口族）    │
  └────────────────────────┘   └──────────────────────────┘
           │
  ┌────────┴───────────────┐   ┌──────────────────────────┐
  │ time（Duration/Time）   │   │ control_flow（defer/...） │
  │ 复用 ::nanosleep→Hook   │   │ 状态对象，随 Fiber 迁移    │
  └────────────────────────┘   └──────────────────────────┘
```

**关键设计**：

- 绝大多数是 **header-only 纯函数**（`fmt`/`strings`/`strconv`/`streams`/`time`
  的模板与 inline 函数），只有 `error.cpp`、`control_flow.cpp` 需要编译单元；
- **错误用值不用异常**：所有失败都返回 `ErrorPtr`，判断用 `Is()` 做**身份比较**
  而不是文本比较（`ErrSyntax`/`ErrRange` 等锚点错误）；
- **`time::Sleep` 复用 Hook**：底层直接调 `::nanosleep`，被 Hook 拦截后在 Fiber
  里变成协作式等待（不占 M），与 Go「挂起 goroutine」的语义一致；
- **`defer/panic/recover` 是显式状态机**：`defer` 用 RAII，`panic` 只发布共享状态，
  `recover` 在 defer 回调里消费 —— 不做 `longjmp` 那种栈跳转，因此可与 C++ 异常共存。

## 最小用法

```cpp
#include "go2cpp/fmt.hpp"
#include "go2cpp/strings.hpp"
#include "go2cpp/strconv.hpp"

int main() {
    // 格式化：语义与 Go 的 fmt 一致（%v 浮点最短表示、%q 转义）
    // 注意：这些函数都在 go2cpp 命名空间下（不是 go2cpp::fmt）
    std::string line = go2cpp::Sprintf("user=%s n=%d ok=%v", "alice", 7, true);

    // 字符串：返回 std::vector<std::string>，Go 的边界也一并照搬
    auto parts = go2cpp::Split("a,b,c", ",");                // {"a","b","c"}
    bool starts = go2cpp::HasPrefix(line, "user=");

    // 数字：ParseInt 返回结构化结果（value + error），base=0 推断进制
    auto parsed = go2cpp::ParseInt("0x1f", 0, 64);           // → 31
    auto bad = go2cpp::Atoi("12x");                          // Atoi == ParseInt(s, 10, 0)
    bool syntax = go2cpp::Is(bad.error, go2cpp::StrconvErrSyntax());  // 身份比较

    return (parts.size() == 3 && starts && parsed.ok() && parsed.value == 31 && syntax) ? 0 : 1;
}
```

完整场景见 `strings_demo.cpp`（切分/清洗/Builder）与 `strings_full.cpp`（六场景 + 十万字段往返）。

## 示例列表

全部注册进 ctest，目标名 = `go2cpp_<文件名>`：

```sh
cmake --build /tmp/build-user --target go2cpp_fmt_demo -j && /tmp/build-user/go2cpp_fmt_demo
```

| 示例 | 演示什么 |
| --- | --- |
| `strings_demo` / `strings_full` | 切分与字段清洗、Builder 拼接与脱敏、`Cut` 族、CSV/URL/头名比较、十万字段往返 |
| `strconv_demo` / `strconv_full` | 配置解析、指标格式化、`base=0` 推断、`ErrSyntax`/`ErrRange`、下划线规则、hex float |
| `fmt_demo` / `fmt_full` | 日志行构造、`%q` 转义、`%w` 错误链、表格对齐、`%!d(string=abc)` 标记、十万次格式化 |
| `streams_demo` / `streams_full` | 1 MiB 复制、定长帧解析、`LimitReader`、`MultiReader`/`TeeReader`/`MultiWriter`、`ErrUnexpectedEOF` 边界 |
| `time_demo` / `time_full` | 超时预算、指数退避、耗时统计、`Duration.String` 的 15 条对照、千拍零漂移 |
| `errors_demo` / `error_full` | 构造/包装/聚合、`Unwrap` 逐层下降、`Is` 穿透包装、错误图去重 |
| `control_flow_demo` / `control_flow_full` | defer 的 LIFO 与求值时机、`dismiss`/`run_now`、panic/recover 状态机、SelectCaster |

`demo` = 3 个左右主干场景，`full` = 6–8 个场景（含错误/边界路径与规模测试）。

## 源码位置

| 能力 | 头文件 | 实现 |
| --- | --- | --- |
| fmt | `include/go2cpp/tools/fmt.hpp` | header-only |
| strings | `include/go2cpp/tools/strings.hpp` | header-only |
| strconv | `include/go2cpp/tools/strconv.hpp` | header-only |
| streams | `include/go2cpp/tools/streams.hpp` | header-only |
| time | `include/go2cpp/tools/time.hpp` | header-only |
| error | `include/go2cpp/tools/error.hpp` | `src/tools/error.cpp` |
| defer/panic/recover | `include/go2cpp/tools/control_flow.hpp` | `src/tools/control_flow.cpp` |
| json / sort / bytes | `include/go2cpp/tools/json.hpp`、`sort.hpp`、`bytes.hpp` | header-only |

`include/go2cpp/{fmt,strings,strconv,streams,time,error,control_flow}.hpp` 是顶层
转发头，兼容 `#include "go2cpp/fmt.hpp"` 的写法。

指南见 `docs/error_guide.md`、`docs/control_flow_guide.md`、`docs/timer_usage.md`；
各模块的语义对照表在 `docs/modules/`。

## 注意事项

- **`Go` 的 `io` ≠ 本项目 `go2cpp::io`**：前者对应本模块的 `go2cpp::streams`
  （流式读写接口族），后者是 `IOManager`（epoll 调度层）。命名偏差是有意的，
  见 `streams.hpp` 文件头。
- **错误用身份比较**：`Is(err, ErrSyntax())` 而不是比较消息文本；同一个文本的
  两个错误在 Go 语义里是不同的。
- **`strconv` 的越界不抛异常**：返回 `ErrRange` 并**同时给出钳制值**
  （`Atoi("999", 8bit)` → 127 + ErrRange），调用方必须自己决定是否接受钳制值。
- **`time::Sleep` 在非 Hook/非 Fiber 上下文是真睡眠**：会阻塞当前线程，
  与 Go 的 goroutine 挂起不同，别在调度器 worker 上裸调。
- **`defer` 不是语言级 finally**：它按 RAII 执行，异常仍在正常传播；
  `panic::call()` 只发布状态，**不会中断当前 C++ 栈**，要靠 defer 链收尾。
- 这些模块是 header-only，**改一行会触发大量重编译**；调试时优先用示例验证语义。
