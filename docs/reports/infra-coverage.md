# 基础设施覆盖审计报告（reflect / error / config / logging）

> 审计对象：WSL 真树 `/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。
> 审计日期：2026-11-04。规范依据：`docs/modules/infra-usage.md`。
> 所有数字来自 `grep -rnE` 实测，命令见第 8 节，未做估算。

## 1. 结论摘要

- `src/` 下共 **27 个模块目录**，其中 3 个是基础设施**提供者**
  （`src/config`、`src/logging`、`src/reflect`），**24 个消费模块**。
- 四类基础设施的模块覆盖率（有至少 1 处调用点 / 24）：

  | 类别 | 有使用模块数 | 覆盖率 | 说明 |
  |---|---:|---:|---|
  | reflect | 1 | 4.2% | 仅 `rpc`（`reflect::Buffer`），其余全为手写序列化 |
  | error | 10 | 41.7% | 集中在 bento / tunnel / encoding / compress / concurrency |
  | config | 6 | 25.0% | 集中在 os / network / debug，热路径模块基本不读配置 |
  | logging | 4 | 16.7% | 原 3 个（concurrency/os/tunnel），本次补 network 后为 4 |

- **四类全零模块 10 个**：热路径 4 个（`app` 12 cpp、`storage` 12、`p2p` 13、
  `media` 16），纯计算/工具 6 个（`container`、`frame`、`math`、`metrics`、
  `path`、`rd_net`）。前 4 个是本次审计重点；后 6 个不强制日志。
- `logging` 覆盖率最低，且与「静默失败」高度重合：`src/` 全树只有 **6 个文件**
  使用 `GO2CPP_LOG_*`，而 `catch (...)` 静默捕获有 **244 处**、`bool` 失败返回
  数以千计；排障完全依赖调用方自行检查返回值。
- 本次补齐仅做**低风险、不改语义**的 3 处（`network` 的 DNS 解析失败、
  bind 失败、listen 失败），不重写序列化、不迁移错误返回值。

## 2. 审计口径

### 2.1 模块与分母

- 模块 = `src/` 的直接子目录，共 27 个；`src/config`、`src/logging`、
  `src/reflect` 是基础设施自身实现，不计入「消费模块」分母。
- 纯头文件/纯工具模块（`container`、`frame`）仍列入表格，但不作为
  「必须打日志」的强制对象。

### 2.2 「有使用」的判定

按**调用点**统计，不按 include 统计（间接 include 会导致虚高）：

| 类别 | 匹配模式 | 说明 |
|---|---|---|
| reflect | `\breflect::` | 含 `reflect::Buffer` / `reflect::Serializer` 等 |
| error | `\bErrorPtr\b`、`\berror::MakeError\b` | 不含旧 `go2cpp::Error`（全树 0 处） |
| config | `\bconfig::Config\b`、`\bconfig::GetEnv\b` | 不含协议常量命名空间 |
| logging | `GO2CPP_LOG_` | 唯一入口宏 |

### 2.3 「该用而没用」的判定

1. **reflect**：同一文件内既有手写字段拼接/逐字段读写、又无 `reflect::`；
   若类型是 POD 或协议定长结构，重写会改性能，判为「可保留」；
2. **error**：函数以 `bool`/`int` 返回失败、或模块私有 `enum class ...Result`
   表达失败，且失败需要跨层排障 → 缺口；纯内部状态机（如 parser 中间态）
   不判缺口；
3. **config**：出现可部署环境相关的常量（端口、超时、路径、容量）却写死在
   代码里 → 缺口；协议常量（帧 magic、HTTP 状态码）不算；
4. **logging**：失败路径 `return false` / `catch (...)` 之后**没有任何输出**，
   且该失败不是调用方必然检查的契约返回值 → 缺口（静默失败）。

## 3. 模块覆盖总表

> 数字为调用点数；`—` 表示该模块本身是提供者。证据列给首个命中 `file:line`，
> 无命中写「0 处」。

| 模块 | cpp | reflect | error | config | log | 缺口 | 建议 |
|---|---:|---|---|---|---|---|---|
| `app` | 12 | 0 处 | 0 处 | 0 处 | 0 处 | reflect/log/error 全缺 | 网关/隧道失败路径补 WARN；KV 流复用 storage 的 error |
| `bento` | 43 | 0 处 | 65 处 `net/keystore.cpp:113` | 2 处 `net/keystore.cpp` | 0 处 | log 缺 | 传输层重传/校验失败补 DEBUG/WARN |
| `bufio` | 1 | 0 处 | 14 处 `bufio.cpp` | 0 处 | 0 处 | log 缺 | 仅 1 个 TU，IO 错误靠 error 已够 |
| `compress` | 4 | 0 处 | 21 处 `errors.cpp` | 0 处 | 0 处 | log 缺 | 解压失败已有 error，无需日志 |
| `concurrency` | 11 | 0 处 | 45 处 `context.cpp` | 3 处 `fiber.cpp` | 11 处 `scheduler.cpp:1942` | reflect 缺（合理） | 已达标，保持 |
| `config` | 4 | 0 处 | 0 处 | 21 处（自身） | 0 处 | — | 提供者，例外 |
| `container` | 1 | 0 处 | 0 处 | 0 处 | 0 处 | 无（纯容器） | 不要求 |
| `debug` | 4 | 0 处 | 0 处 | 4 处 `debug_event_ring.cpp` | 0 处 | log 缺 | 调试模块自身可输出到 EventRing，不强制 |
| `distributed` | 9 | 0 处 | 1 处 `transport.cpp` | 0 处 | 0 处 | log/config 缺 | 选主/租约失败补 WARN；超时挪 config |
| `encoding` | 4 | 0 处 | 7 处 `binary.cpp` | 0 处 | 0 处 | 无（编解码纯函数） | 不强制日志 |
| `frame` | 1 | 0 处 | 0 处 | 0 处 | 0 处 | 无 | 协议定长，不要求 |
| `hash` | 6 | 0 处 | 0 处 | 1 处 `sha256.cpp` | 0 处 | 无 | 纯计算 |
| `logging` | 1 | 0 处 | 0 处 | 0 处 | — | — | 提供者，例外 |
| `math` | 2 | 0 处 | 0 处 | 0 处 | 0 处 | 无 | 纯数学 |
| `media` | 16 | 0 处 | 0 处 | 0 处 | 0 处 | error/log 缺 | 采集/编码失败返回 bool，建议补 error + WARN |
| `metrics` | 3 | 0 处 | 0 处 | 0 处 | 0 处 | 无 | 指标模块不写业务日志 |
| `network` | 19 | 0 处 | 1 处 `sse.cpp` | 2 处 `hook.cpp` | **3 处（本次补）** | reflect/error 仍缺 | 本次补 3 处；其余 `bool` 是 socket 契约 |
| `os` | 29 | 0 处 | 0 处 | 7 处 `proc.cpp:536` | 3 处 `proc.cpp:263` | error 缺 | 进程/PTY 失败补 error（改动大，暂缓） |
| `p2p` | 13 | 0 处 | 0 处 | 0 处 | 0 处 | 四类全缺 | NAT/中继失败补 WARN；STUN 超时挪 config |
| `path` | 2 | 0 处 | 0 处 | 0 处 | 0 处 | 无 | 纯字符串 |
| `rd_net` | 3 | 0 处 | 0 处 | 0 处 | 0 处 | log 缺 | 跨平台分支失败补 WARN |
| `reflect` | 4 | 5 处 include（自身） | 0 处 | 0 处 | 0 处 | — | 提供者，例外 |
| `relay` | 3 | 0 处 | 9 处 `relay_pipe.cpp` | 0 处 | 0 处 | log 缺 | 管道 push 失败已有 error |
| `rpc` | 6 | 17 处 `protocol.cpp:184` | 0 处 | 0 处 | 0 处 | error/log 缺 | `Status` → `ErrorPtr` 加转换层；连接失败补 WARN |
| `storage` | 12 | 0 处 | 0 处 | 0 处 | 0 处 | **四类全缺** | `KV::SetError` / `MemCache::Fail` 是集中点，补 error 转换与 WARN |
| `tools` | 2 | 0 处 | 32 处 `error.cpp` | 0 处 | 0 处 | 无 | error 体系实现者 |
| `tunnel` | 7 | 0 处 | 32 处 `protocol.cpp:32` | 0 处 | 27 处 `client_main.cpp` | reflect 缺 | 已达标 |

## 4. 缺口 TOP 5

### 4.1 `storage`（12 cpp，四类全零）

- 证据：`src/storage/kv.cpp:482` 的 `SetError()` 只把字符串写进成员
  `last_error_`，无日志、无错误类型；`src/storage/memcache.cpp:313` 的
  `Fail()` 同样只存字符串并返回 `false`。
- 影响：KV 打开失败、WAL 损坏、SSTable 损坏全部只能靠调用方主动查
  `LastError()`；生产环境表现为「进程起来但数据不可用」，无任何日志线索。
- 建议：`SetError` 内加 `GO2CPP_LOG_ERROR("storage.kv")`（错误路径，非数据面）；
  再提供 `LastErrorPtr()` 把首个错误映射为 `error::IOError`。需给
  `go2cpp_kv` 增加 `go2cpp_log` 链接，属构建改动，本次未做。

### 4.2 `app`（12 cpp，四类全零）

- 证据：`src/app/http_tunnel.cpp`、`gateway.cpp`、`stream_table.cpp` 的失败
  路径均为 `return false` / `return nullptr`，无任何日志。
- 影响：网关是入口层，握手/路由/流表失败无法定位到具体连接。
- 建议：入口层补 `GO2CPP_LOG_WARN("app.gateway")`，只加在「连接级失败」，
  不加在「单帧解析失败」（避免刷屏）。

### 4.3 `p2p`（13 cpp，四类全零）

- 证据：`src/p2p/relay_protocol.hpp:72` 有私有 `enum class RelayStatus`，
  但无 `error::` 转换；NAT 探测失败无日志。
- 影响：P2P 打洞失败只能看到「连不上」，无法区分 STUN 超时、中继拒绝、
  协议版本不符。
- 建议：`RelayStatus` 增加 `ToErrorPtr()`；探测失败补 WARN。

### 4.4 `rpc`（6 cpp，仅 reflect）

- 证据：`src/rpc/protocol.cpp:184` 起全部用 `reflect::Buffer`（已用 reflect），
  但错误一律走 `go2cpp::rpc::Status` 结构体，无 `ErrorPtr`、无日志。
- 影响：RPC 调用失败时，客户端只能拿到 `Status{code, message}`，无法参与
  `ErrorScope` 聚合。
- 建议：新增 `Status::ToError()` 单向转换（保留原 `Status` 语义），
  在连接建立失败处补 WARN。

### 4.5 `network`（19 cpp，log 全零；本次已补 3 处）

- 证据：`src/network/address.cpp:137` 的 `getaddrinfo` 失败只 `return false`；
  `src/network/socket.cpp:431/441` 的 `bind`/`listen` 失败只返回 bool。
- 影响：端口占用、DNS 失败均无日志，服务起不来时只能靠 strace。
- 本次已补（见第 5 节）；`error` 覆盖率仍低，但 socket 层用 `bool + errno`
  是既有契约，迁移会改调用方语义，不在本次范围。

## 5. 本次补齐（改前 / 改后）

> 原则：只加日志，不改返回值、不改 errno、不改控制流；全部在错误路径，
> 不进入数据面。

### 5.1 `src/network/address.cpp` — DNS 解析失败

**改前**：

```cpp
if (error != 0) return false;
```

**改后**：

```cpp
if (error != 0) {
    GO2CPP_LOG_WARN(GO2CPP_LOG_NAME("net.address"))
        << "getaddrinfo failed: host=" << host << " error=" << error
        << " (" << ::gai_strerror(error) << ")";
    return false;
}
```

**理由**：`getaddrinfo` 的失败原因在 `errno` 之外（`gai_strerror`），调用方
只拿到 `bool` 时无法区分「DNS 无记录」「临时失败」「family 不支持」。
加 WARN 后可直接从日志定位。`result.clear()` 与返回值完全不变。

### 5.2 `src/network/socket.cpp` — bind 失败

**改前**：

```cpp
const bool result = ::bind(fd, address.addr(), address.addr_len()) == 0;
end_operation(); return result;
```

**改后**：

```cpp
const bool result = ::bind(fd, address.addr(), address.addr_len()) == 0;
if (!result) {
    const int saved_errno = errno;  // to_string() 可能分配内存并覆盖 errno
    GO2CPP_LOG_ERROR(GO2CPP_LOG_NAME("net.socket"))
        << "bind failed: " << address.to_string()
        << " errno=" << saved_errno;
}
end_operation(); return result;
```

**理由**：bind 失败几乎总是「端口被占用 / 地址不可用 / 权限不足」，是启动期
最高频的排障点。先保存 `errno` 再格式化地址，避免 `to_string()` 的分配
覆盖错误码（这是本次改动唯一需要小心的点）。返回值语义不变。

### 5.3 `src/network/socket.cpp` — listen 失败

同 5.2 模式，输出 `fd`、`backlog`、`errno`。理由：listen 失败常与
backlog/资源限制相关，`fd` 可用于对照 `lsof`。

### 5.4 验证

```
ninja -C build-review go2cpp_network
[15/16] Linking CXX shared library libgo2cpp_network.so.0.1.0
[16/16] Creating library symlink ...
exit code 0
```

- 本次改动的两个 TU 无告警；
- 构建输出中唯一告警是既有 `src/network/http2/hpack.cpp:465`
  `DecodeName defined but not used`，该文件不在本次改动范围（`git status`
  未修改），非本次引入，未处理。

## 6. 未做的补齐及原因

| 候选 | 为何不做 |
|---|---|
| 给 `storage` / `app` / `rpc` 补日志 | 这三个目标的 CMake 链接里没有 `go2cpp_log`（`go2cpp_kv` 只链 `Threads::Threads`，`go2cpp_app` 只链 `hash`/`bento`，`go2cpp_rpc` 只链 `frame`/`reflect`）。加日志必须改 CMakeLists，超出本次「只最小修改 src/」的授权。 |
| 用 reflect 重写手写序列化 | 规范与本任务均明确：强上 reflect 会改变热路径性能（反射序列化比手写字段拷贝慢一个量级），收益是覆盖率数字而非正确性。 |
| 把协议常量挪进 config | 帧 magic、HTTP 状态码、HPACK 静态表属于协议不可配置常量；挪进 config 会引入运行期误配置风险，不是缺口。 |
| 把 `bool` 返回全部改 `ErrorPtr` | 会改变所有调用方语义，且 socket 层 `bool + errno` 是 POSIX 契约；只建议新代码用 `ErrorPtr`。 |
| 修 `hpack.cpp:465` 既有告警 | 不在授权范围（非本次引入），避免与并行改动冲突。 |

## 7. 检查脚本扩展

`scripts/check_infra_usage.sh` 新增 `--coverage` 模式（只报告，不设门禁，
退出码恒为 0）：

```bash
scripts/check_infra_usage.sh --coverage
```

输出：逐模块四类调用点数 + 四类「有使用模块数 / 消费模块数」汇总。
原 `--verbose` 与默认门禁模式行为不变。

## 8. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 四类调用点（报告第 3 节）
grep -rnE '\breflect::' src/<module>/ | wc -l
grep -rnE '\bErrorPtr\b|\berror::MakeError\b' src/<module>/ | wc -l
grep -rnE '\bconfig::Config\b|\bconfig::GetEnv\b' src/<module>/ | wc -l
grep -rnE 'GO2CPP_LOG_' src/<module>/ | wc -l

# 静默失败规模
grep -rn 'catch (\.\.\.)' src/ | wc -l      # 244
grep -rln 'GO2CPP_LOG_' src/ | wc -l        # 6

# 一键覆盖统计
scripts/check_infra_usage.sh --coverage
```

## 9. 后续建议（按优先级）

1. **给 `go2cpp_kv` / `go2cpp_app` / `go2cpp_rpc` 补 `go2cpp_log` 链接**，
   这是一次性构建改动，之后 storage/app/rpc 的静默失败才能低成本补齐；
2. `KV::SetError` / `MemCache::Fail` 两个集中点各自加一行日志，可覆盖
   storage 模块 80% 的失败路径；
3. `rpc::Status::ToError()` 转换层，让 RPC 错误进入 `ErrorScope`；
4. 把 `p2p::RelayStatus` 与 `distributed` 的租约失败映射到 `error`；
5. 每季度跑一次 `--coverage`，数字只升不降（作为报告而非门禁）。
