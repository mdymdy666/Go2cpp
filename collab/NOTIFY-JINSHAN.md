# 通知提示词（发给 JinShanProject 的 agent）

> 把下面 **【通知正文】** 整段复制给 JinShanProject 的 agent。

---

## 【通知正文】

### 致 JinShanProject 的 agent

Go2Cpp（基础库项目）已建立**协作区**，位置：

```
Go2Cpp 仓库：/UserData/CodexWorkSpace/Go2cpp/collab/
```

### 一、规则（最重要）

**Go2Cpp 只做「通用层」——拒绝不通用的要求。**

| 类别 | 谁做 | 例子 |
|---|---|---|
| **通用** | Go2Cpp | 并发/网络/缓存/错误/序列化/Go 风格 API/有界队列/背压/超时/连接池/限流 |
| **专属** | **你自己（JinShan）** | RDRL 帧语义（type 1-9）、IRDPSRAPI、invitation 改写、命令白名单、业务状态机 |
| **可抽象** | Go2Cpp 提供机制，你填语义 | FrameCodec（length-prefix+CRC32）/ Hub（配对注册）/ RelayPipe（背压+慢 peer） |

**判定方法**：问「别的项目（游戏/AI 推理/IM）会用吗？」
- 会 → 通用（向 Go2Cpp 提）；
- 不会 → 专属（你自己实现）；
- 机制会、语义不会 → 抽象（让 Go2Cpp 提供机制，你填协议）。

### 二、你的任务（三步）

**第一步：读**

1. `collab/BRIDGE-PROMPT.md`（完整协作说明）；
2. `collab/go2cpp-offerings.md`（Go2Cpp 能提供什么——**不要重复造**）；
3. 你自己的 `docs/05-detailed/protocol.md`（RDRL 协议）。

**第二步：提需求**

在 `collab/jinshan-requests.md` 里写你需要什么（按格式）：

```markdown
## REQ-xxx: <需求名>

- **需要的接口**：<Go 风格签名>
- **通用性判定**：通用 / 专属（你自己做）/ 可抽象
- **用途**：<哪个模块（MOD-xx）/什么场景>
- **约束**：语义/性能/平台/依赖
- **优先级**：P0/P1/P2
- **验收**：<怎么算完成>
```

**第三步：验收**

Go2Cpp 实现后，在 `collab/acceptance.md` 记录（用例/数据/结论）。

### 三、已有能力（直接用，不用提）

| 能力 | 状态 |
|---|---|
| 并发（Fiber/Scheduler/Channel/Mutex/CV/WG/RW/Select/Context/Future） | ✅ 9 原语，1e9×3 压测通过 |
| 网络（TCP/UDP/HTTP/WS/MQTT/CoAP/SOCKS5） | ✅ 14/14 ≤1.1× Go |
| **缓存（7 策略：LRU/LRU-K/2Q/TinyLFU/GDSF/ARC/CLOCK）** | ✅ **Go 标准库没有**，几何 1.91× |
| P2P（STUN/打洞/TURN） | ✅（你文档说不做，可选） |
| 应用层（流表/资源传输/KV） | ✅ 62 场景 |
| Go 风格 API（net/sync/chan/context/error/time/io） | 🔄 进行中 |
| 通用 FrameCodec / Hub / RelayPipe | ❌ 待做（你提需求，Go2Cpp 提供机制） |

### 四、明确拒绝的（不要提）

```
❌ 实现 RDRL 协议本身（type 1-9 的语义）
❌ 封装 IRDPSRAPI / Win32 UI
❌ 实现 JinShan 的错误码表（只提供通用 Error 体系）
❌ 「按 JinShan 业务流程定制」的接口
```

### 五、你会得到什么

- **通用能力**（已有：并发/网络/缓存/错误/Go API）；
- **通用抽象**（新增：FrameCodec / Hub / RelayPipe——机制通用，语义你填）；
- **示例**（`collab/examples/`，可编译运行）。

### 六、立刻开始

1. 读 `collab/BRIDGE-PROMPT.md` + `collab/go2cpp-offerings.md`；
2. 读你的 `docs/05-detailed/protocol.md`；
3. 在 `collab/jinshan-requests.md` 写需求（**标注通用/专属/可抽象**）；
4. 每次写 3-5 条，标优先级，等 Go2Cpp 实现。

---

## 【一句话】

> **通用能力（并发/网络/缓存/错误/Go API）Go2Cpp 做；JinShan 专属（RDRL/IRDPSRAPI/业务）你自己做；机制通用语义专属的，让 Go2Cpp 提供机制、你填协议。**
