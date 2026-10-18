# File 状态机（`go2cpp::os::File`）

- 源文件：`src/os/file.cpp`（类定义在 `include/go2cpp/os/file.hpp`）
- 关键函数：`File::Open()`、`File::Create()`、`File::Adopt()`、移动构造/移动赋值、`File::Close()`

## 状态

| 状态 | 含义 |
|---|---|
| Closed | `m_fd == -1`，不持有任何 fd |
| OpenOwned | `m_fd >= 0` 且 `m_owned == true`，析构/`Close()` 负责关闭 |
| OpenBorrowed | `m_fd >= 0` 且 `m_owned == false`，只借用调用方的 fd |

## 转化表

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Closed | `Open()` | OpenOwned | `openat + fstat`，只接受 `S_ISREG` |
| Closed | `Create()` | OpenOwned | `O_RDWR|O_CREAT|O_TRUNC`，创建后即可读回 |
| Closed | `Adopt(fd, true)` | OpenOwned | 校验通过后接管所有权 |
| Closed | `Adopt(fd, false)` | OpenBorrowed | 校验通过但不负责关闭 |
| Closed | `Adopt(socket)` | Closed | 返回 `kIsSocket`，不修改状态、不关闭 fd |
| Closed | `Adopt(FIFO/eventfd/...)` | Closed | 返回 `kNotRegularFile`，原因见 `file.hpp` 1.4 |
| OpenOwned | 移动构造 / 移动赋值 | Closed（源）→ OpenOwned（目标） | fd 与 `m_owned` 一起转移，源置 -1 |
| OpenBorrowed | 移动构造 / 移动赋值 | Closed（源）→ OpenBorrowed（目标） | 源不关闭 fd |
| OpenOwned | `Close()` / 析构 | Closed | `close(fd)` 恰好一次，忽略 close 错误 |
| OpenBorrowed | `Close()` / 析构 | Closed | 只放弃引用，不关闭调用方 fd |
| OpenOwned | `Read`/`ReadAt`/`Write`/`WriteAt`/`WriteAll`/`Seek`/`Stat`/`Sync`/`Truncate` | OpenOwned | 失败只写 `m_last_error`，状态不变 |
| OpenOwned / OpenBorrowed | `Open`/`Create`/`Adopt` 写入同一对象 | OpenOwned / OpenBorrowed | 移动赋值会先 `Close()` 旧 fd，避免泄漏 |

## 不变量

- `m_fd == -1` 时 `m_owned` 恒为 false；`m_owned == true` 时析构/`Close()` 恰好关闭一次。
- 非 Closed 状态下 fd 必为普通文件（`S_ISREG`）—— `Open`/`Adopt` 都做了白名单校验。
- `Adopt` 不修改 fd 的任何标志位（尤其不碰 `O_NONBLOCK`），避免与 `FdManager` 的还原逻辑冲突。
- `Open` 失败时不把新 fd 写入 `*out`；失败路径会 `RawClose`，不泄漏 fd。

## 非法转化

- Closed -> 读写：`Read`/`Write` 返回 -1 并置 `m_last_error = EBADF`，状态保持 Closed。
- Closed -> OpenOwned（socket/FIFO）：被 `kIsSocket` / `kNotRegularFile` 拒绝，且**不会**关闭调用方的 fd。
- OpenOwned -> OpenBorrowed：不提供该转化；所有权只能随移动转移，或先 `Close()` 再 `Adopt`。
