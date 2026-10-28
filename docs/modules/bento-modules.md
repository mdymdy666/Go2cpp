# Bento 模块覆盖矩阵（JinShan 39 模块 → Go2Cpp 通用实现）

> 状态日期：2026-10-28。对应提交 `046d39f`；2026-10-28 终审补文档链接列，并按 `afc135a` 校正 X25519 接线状态。
> JinShanProject 为只读场景参考；本文件记录「可通用化部分」在 Go2Cpp 的落地状态。
> 不适用项给理由，不硬做 Windows/RDP 专属能力。
> 模块文档（2026-10-28 补）：`bento-transport.md`、`bento-security.md`、
> `bento-session.md`、`bento-app.md`、`bento-foundation.md`。

## 一、已实现（通用模块）

| JinShan 模块 | Bento 实现 | 测试 | 状态 | 模块文档 |
|---|---|---|---|---|
| MOD-07/08/09 传输抽象/TCP/内存 | `ftransport`（ITransport/TcpTransport/MemoryTransport） | 19 用例/348 断言 | ✅ | [bento-transport.md](bento-transport.md) |
| MOD-10 帧编解码 | `frame`（20B 头/CRC32/FrameDecoder） | 12/5511（含 200 轮 fuzz） | ✅ | [bento-transport.md](bento-transport.md) |
| MOD-11 心跳/超时 | `heartbeat`（30s/90s/假时钟） | 7/67 | ✅ | [bento-transport.md](bento-transport.md) |
| MOD-12 重连退避 | `backoff`（指数+抖动） | 7/565 | ✅ | [bento-transport.md](bento-transport.md) |
| MOD-13 配对协议 | `pairing`（32B 帧/`PairingAuthPolicy`/12 失败码） | 13/8239 | ✅ | [bento-session.md](bento-session.md) |
| MOD-14 会话表 | `session_table`（1:N/租约 90s） | 9/135 | ✅ | [bento-session.md](bento-session.md) |
| MOD-05 密钥管理 | `keystore`（加载/脱敏/常量时间/轮换） | 20/2719 | ✅ | [bento-security.md](bento-security.md) |
| MOD-15 字节转发/背压 | `relay`（64KB 有界/慢客户端/半关闭） | 13/80 | ✅ | [bento-transport.md](bento-transport.md) |
| MOD-25 虚拟通道 | `net::Channel`（STREAM/DATAGRAM/VIDEO） | 既有 | ✅ | [PROTOCOL §3-§5](../../tests/bento/PROTOCOL.md) |
| MOD-26 命令协议 | `command`（白名单/杀进程组/沙箱） | 14/93 | ✅ | [bento-app.md](bento-app.md) |
| MOD-27 文件下发 | `app::fetch`（HTTP/HTTPS/SHA256/重试） | 17/237 | ✅ | [bento-app.md](bento-app.md) |
| MOD-28 文件传输 | `file_transfer`（分块/SHA256/续传/沙箱） | 14/162 | ✅ | [bento-app.md](bento-app.md) |
| MOD-30 文件浏览 | `app::file_browser`（List/Get/Put/Stat） | 12/1890 | ✅ | [bento-app.md](bento-app.md) |
| MOD-31 备份打包上传 | `app::zip` + `app::backup`（store-only/CRC32/FR-18 远端校验） | 18+10+10/468 | ✅ | [bento-app.md](bento-app.md) |
| MOD-37 差环境检测 | `app::capability`（15 项探测→能力矩阵） | 6/125 | ✅ | [bento-app.md](bento-app.md) |
| MOD-39 会话编排 | `app::session`（状态机/回调/优雅关闭） | 13/200 | ✅ | [bento-session.md](bento-session.md) |
| — 控制权仲裁 | `session_control`（IControlPolicy/审计/SESS-01） | 18997 断言 | ✅ | [bento-session.md](bento-session.md) |
| — 多 peer 会话 | `peer_hub`（conn_id/加密/超时回收/HELLO 重放防护） | 825 断言 | ✅ | [bento-session.md](bento-session.md) |
| — 加密/认证 | `crypto`（ChaCha20-Poly1305/重放窗口）+ `x25519`（ECDH） | 1160+ | ✅ | [bento-security.md](bento-security.md) |
| — 错误/反射 | `bento::error` + `bento::reflect`（注册表/mapping/TypeTraits） | 58 用例/16657 | ✅ | [bento-foundation.md](bento-foundation.md) |
| — UDP 传输 | `udp_transport`（无连接 + Connect 语义） | 15/158 | ✅ | [bento-foundation.md](bento-foundation.md) |

## 二、不实现（Windows/RDP 专属或其他）

| JinShan 模块 | 不实现理由 |
|---|---|
| MOD-06 Win7 兼容 | Go2Cpp 是 Linux 运行时（epoll/syscall），不适用 |
| MOD-16 Go 中转 | Go 服务，非 C++ 通用模块 |
| MOD-17–24 媒体/输入/UI | IRDPSRAPI/Win32 专属；Go2Cpp 不做桌面采集 |
| MOD-32 会话录屏 | 依赖 RDP 帧，Go2Cpp 有 replay 但不做 AVI |
| MOD-33 提权 | Windows UAC 语义；Linux 用 sudo 另行处理 |
| MOD-34 AI 采集 | 无桌面上下文；可用 os 采集替代但不硬做 |
| MOD-35/36 UI/托盘 | Win32 原生 UI 专属 |

## 三、剩余增强（非阻塞）

| 项 | 说明 |
|---|---|
| FR-18 远端校验 | ✅ 已实现（`verify_remote`） |
| CON-06 降级策略 | capability 已探测定，应用层接入（文档示例已给） |
| X25519 集成到 crypto HELLO | ✅ 已接线（`afc135a`：HELLO/HELLO_ACK 携带 32B 临时公钥，`DeriveSessionKeyWithX25519` 派生会话密钥；见 `bento-security.md` §4） |
| 命名空间历史债 | net 的 link/peer_hub/session_control 仍在 `bento` 根（文档已注明） |

## 四、验证口径

- `ctest -R bento`：**48 项**（47 通过 + `bento_netem` 无 CAP_NET_ADMIN 时 SKIP 77，2026-10-28 实测，见 `codex.md` §28）；另有 app 层构建记录 59/59（§27，两次构建的测试注册数不同）；
- 构建：Bento 目标 `-Werror` 0 警告；
- 三遍审查：批 1（源码/测试/扩展性）+ 批 2（52/20/16 条）报告落盘 `docs/review/`，P0/P1 已修复并复审；
- 变异验证：11 组回退实验全部被测试捕获。
