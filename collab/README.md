# Go2Cpp × JinShanProject 协作目录

> 本目录是 **Go2Cpp（基础库）** 与 **JinShanProject（远程桌面）** 两个项目之间的协作区。
> 双方 agent 在这里交换：需求、接口规范、示例、验收结果。

## 一、协作模型

| 角色 | 职责 |
|---|---|
| **Go2Cpp**（本仓库） | 提供**通用基础能力**（并发/网络/缓存/错误/Go 风格 API/P2P/Relay 协议）；给出**接口 + 示例 + 测试** |
| **JinShanProject** | 消费这些能力；在 `jinshan-requests.md` 提**需求**（要什么接口/什么语义/什么约束） |

## 二、目录结构

```
collab/
├── README.md                  # 本文件（协作说明）
├── go2cpp-offerings.md        # Go2Cpp 能提供什么（通用能力清单 + 接口签名）
├── jinshan-requests.md        # JinShan 的需求（由 JinShan agent 填写）
├── acceptance.md              # 验收记录（双方确认）
├── examples/                  # 接口示例（可直接编译运行）
│   ├── cache-example.md       # 缓存（7 策略，Go 标准库没有）
│   ├── go-api-example.md      # Go 风格 API（net/sync/chan/context/error/time/io）
│   ├── relay-example.md       # RDRL relay 接口（JinShan 专用）
│   └── concurrency-example.md # Fiber/Channel/Context 用法
└── BRIDGE-PROMPT.md           # 给 JinShan agent 的提示词（桥梁）
```

## 三、流程

```
① JinShan agent 读 BRIDGE-PROMPT.md + go2cpp-offerings.md
② JinShan agent 在 jinshan-requests.md 填需求（要什么/为什么/约束/优先级）
③ Go2Cpp agent 实现（或在 offerings 里标记「已有」）+ 更新 examples/
④ 双方在 acceptance.md 记录验收（测试/数据）
```

## 四、原则

- **Go 标准库没有的，Go2Cpp 补**（如缓存 7 策略、P2P 穿透、Relay 协议、Fiber 调度）；
- **接口保持 Go 风格**（`net.Conn`/`sync.Mutex`/`chan`/`context`/`error`），底层不暴露；
- **示例必须可编译运行**（不只是文档）；
- **不引入第三方库**（Go2Cpp 的约束，JinShan 也要遵守）。

## 五、当前状态（2026-10-31）

| 能力 | 状态 |
|---|---|
| 并发（Fiber/Scheduler/Channel/Mutex/CV/WG/RW/Select/Context/Future） | ✅ 已完成（9 原语 + 1e9×3 压测） |
| 网络（6 协议 + TCP/UDP 批量 + SOCKS5） | ✅ 已完成（14/14 ≤1.1× Go） |
| 缓存（7 策略：LRU/LRU-K/2Q/TinyLFU/GDSF/ARC/CLOCK） | ✅ 已完成（几何 1.91×，堆分配→0） |
| P2P（STUN/打洞/TURN） | ✅ 已完成（4/4 测试） |
| 应用层（流表/资源传输/KV） | ✅ 已完成（62 场景） |
| Go 风格 API（`include/go2cpp/go/**`） | 🔄 进行中 |
| RDRL relay 接口 | 🔄 待做（JinShan 专用） |
| UDP 弱网（BBR/FEC） | ⚠️ 实现丢失（报告在） |
