# 上游与依赖清单

## Go 上游参考

本项目按 Go `go1.23.0` 设计，仓库为
`https://go.googlesource.com/go`，固定 commit：
`6885bad7dd86880be6929c02085e5c7a67ff2887`。源码归档地址为
`https://go.dev/dl/go1.23.0.src.tar.gz`，SHA-256 为
`42b7a8e80d805daa03022ed3fde4321d4c3bf2c990a144165d01eeecd6f699c6`。
上游许可证为 BSD-3-Clause，原始 `LICENSE` 保留在
`third_party/go1.23.0/LICENSE`。参考文件只用于研究，不加入任何 CMake target，
第三方源码与许可证原文不翻译、不修改。

归档文件因体积较大被 Git 忽略；可在仓库根目录执行：

```sh
cd third_party/go-reference && sha256sum -c SHA256SUMS
```

## Go 能力的 C++ 替代

| Go 机制 | 本项目替代 | 明确边界 |
|---|---|---|
| GC、栈图和 Go heap ABI | `shared_ptr`、RAII、显式所有权、弱子节点 | 没有移动 GC、写屏障或 Go heap ABI |
| `morestack`、分段栈 | Boost.Context 保护栈与 Task/Fiber 所有权 | 栈大小固定，不提供编译器生成的栈图和异步增长 |
| `mcall`/`gogo`/`gopark` | 合作式 C++ worker 与显式 G/M/P 状态 | 没有汇编 ABI 和任意指令点抢占 |
| `sudog`、futex、netpoller | 堆等待节点、ParkingCondition、TimerService、Linux epoll | 不复刻 Go netpoller 内部 ABI |
| runtime 原子操作 | `std::atomic` 加状态转换互斥量 | 遵循 C++ 内存模型，不承诺 Go 内部顺序 |
| cgo 与内部 ABI | 无依赖，只有公共 C++ 头文件 | cgo 互操作不在范围内 |
| 编译器生成 defer/panic | 显式 `panic_defer::Frame` 与 unwind 协议 | 转译器必须生成边界，普通 C++ 函数不会自动获得 Go 语义 |
| Go 定时器/取消 | 可取消 `TimerService`、Context deadline、等待节点 gate | 回调线程不会直接恢复 Fiber 栈 |

## 构建依赖

运行时链接 `Threads::Threads` 和 Boost.Context（最低 1.70；当前工作区验证为
1.83.0）。Linux Hook 额外使用系统 `dl`。Boost.Context 使用 Boost Software
License 1.0；Go2Cpp 不携带或修改它，系统包许可证由部署者负责。IO 模块使用
Linux epoll/eventfd/syscall。检测到 Valgrind 头文件时会注册保护 Fiber 栈；ASan、
UBSan、TSan 仅作为独立验证配置，不能混用互相冲突的 sanitizer。

## 可替换边界

Scheduler、Fiber backend、TimerService、Channel 等待后端、IOManager 和 Hook
均通过公共状态/结果契约隔离；0.x 没有稳定插件 ABI。替换实现必须保持 G/M/P
状态转换、一次性 wake claim、对象所有权、取消和 shutdown 规则。项目不依赖 Go
运行时、cgo、垃圾回收器、汇编或第三方 Sylar 代码。
