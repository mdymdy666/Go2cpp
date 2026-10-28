# bento-app 模块组：抓取、文件浏览、备份打包、能力探测、命令与文件传输

> **状态：现行**（2026-10-28）。基线 `HEAD afc135a`（`feat(bento): X25519 前向保密 + 弱网拥塞控制修复（netem 实测）`）。
> 源码：`include/go2cpp/bento/app/{fetch,file_browser,backup,zip,capability}.hpp` 与 `include/go2cpp/bento/net/{command,file_transfer}.hpp`，加对应 `src/bento/{app,net}/*.cpp`；7 个模块共 9182 行（头文件 1858 行 / 实现 7324 行，`wc -l` 实测）。
> **命名空间以头文件为准**：`fetch`/`file_browser`/`backup`/`zip`/`capability` 在 `go2cpp::bento::app`；
> `command`/`file_transfer` 在 **`go2cpp::bento` 根**（不在 `bento::net`，属 `AI_COLLAB.md` §二记录的历史债）。
> 引用约定：代码位置写 `file:line`，签名逐字摘自头文件；测试数字为本地 `grep` 实测，**本次未运行 ctest，不写“已验证通过”**。
> 测试目录：`tests/bento/`（`test_fetch.cpp`、`test_upload.cpp`、`test_file_browser.cpp`、`test_backup.cpp`、`test_zip.cpp`、`test_capability.cpp`、`test_command.cpp`、`test_file_transfer.cpp`）。
> 相关文档：[bento-foundation.md](bento-foundation.md)、[bento-session.md](bento-session.md)、[bento-transport.md](bento-transport.md)、[bento-scenarios.md](bento-scenarios.md)（§21）、[PROTOCOL.md](../../tests/bento/PROTOCOL.md)。

---

## 1. 模块清单

| 模块 | 命名空间 | 头文件 | 实现 | 测试 | 职责 |
|---|---|---|---|---|---|
| fetch（MOD-27） | `go2cpp::bento::app` | `app/fetch.hpp`（247 行） | `app/fetch.cpp`（1824 行） | `test_fetch.cpp` | HTTP(S) 单文件下发：URL/路径沙箱、流式 SHA-256、大小上限、重试退避、重定向、原子落盘 |
| file_browser（MOD-30） | `go2cpp::bento::app` | `app/file_browser.hpp`（245 行） | `app/file_browser.cpp`（656 行） | `test_file_browser.cpp` | 受限根目录的 List/Get/Put/Stat：路径沙箱、符号链接拒绝、临时文件发布 |
| backup（MOD-31） | `go2cpp::bento::app` | `app/backup.hpp`（211 行） | `app/backup.cpp`（1112 行） | `test_backup.cpp`、`test_upload.cpp` | 目录 → store-only zip 打包（`.part.<pid>.<seq>` → rename）与 curl 上传（token 走 stdin、重试、可选远端校验） |
| zip（MOD-31） | `go2cpp::bento::app` | `app/zip.hpp`（267 行） | `app/zip.cpp`（1202 行） | `test_zip.cpp` | store-only ZIP 写入/中央目录读取/解压：固定头、CRC-32、条目与大小上限、Zip Slip 防护 |
| capability（MOD-37） | `go2cpp::bento::app` | `app/capability.hpp`（165 行） | `app/capability.cpp`（718 行） | `test_capability.cpp` | 15 项只读环境探测（/proc、epoll、cgroup、rlimit、ifaddrs、statfs、uname）→ 能力矩阵 |
| command（MOD-26） | **`go2cpp::bento`** | `net/command.hpp`（323 行） | `net/command.cpp`（813 行） | `test_command.cpp` | 命令帧编解码、白名单/拒绝表校验、超时杀整个进程组、stdout/stderr 捕获 |
| file_transfer（MOD-28） | **`go2cpp::bento`** | `net/file_transfer.hpp`（400 行） | `net/file_transfer.cpp`（999 行） | `test_file_transfer.cpp` | 64KB 分块 + 块级/整文件 SHA-256、断点续传、路径沙箱与大小上限 |

## 2. 分层与调用关系

```
backup ──► zip（ZipWriter / ListZip）        os::proc（curl PUT/POST + 回读 GET/HEAD）
   └────► app::detail::retry（指数退避，retry.hpp:31）
fetch ───► 自研 HTTP/1.1（POSIX socket，明文）+ os::proc（https → 系统 curl）
file_browser ─► 标准库 filesystem + POSIX（无网络、无进程）
capability ───► POSIX 只读探测（/proc、cgroup、rlimit、getifaddrs、statfs、uname）
command ──────► os::proc::Process（new_session=true，子进程即进程组组长）
file_transfer ► go2cpp::hash::Sha256 + std::fstream（自包含 BF 帧，不绑定链路）
zip ──────────► go2cpp::hash::Crc32
```

- **依赖方向**：上述模块只向下依赖 `os`/`hash` 与标准库；反向不存在依赖。`backup` 不调用 `fetch` 的下载逻辑（上传统一走 `os::proc` + curl），`zip` 不反向依赖 `backup`，`fetch` 在 `src/` 内无其它调用方。
- **禁止项**：app 层不得 include `bento/net/link.hpp`/`session.hpp`/`frame.hpp`（当前 0 处）；`command`/`file_transfer` 不得依赖 Bento 链路——两者的帧都自带 magic/长度/总长校验，可走任意可靠信道（`command.hpp:70-73`、`file_transfer.hpp:6-9`）。
- **command 与 sandbox 的边界**：两者没有代码级耦合。`CommandPolicy::allowed_cwd_roots` 只做 cwd 的绝对路径 + 根目录归属校验；进程启动后靠“argv 白名单 + 不经 shell + 杀进程组”约束，**不 chroot、不降权、不设 seccomp**。真正的文件沙箱在 `file_browser`/`file_transfer`/`fetch` 各自的路径解析层。

## 3. API 参考

### 3.1 fetch

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `FetchResult Fetch(std::string_view url, const std::string& dest_dir, const FetchOptions& options = FetchOptions{});` | 下载并校验，成功时 `path`/`bytes`/`sha256` 有效，落盘为 `rename` 原子替换 | 目标已存在且 `overwrite=false` 时直接 `kTargetExists`，不发请求 |
| `FetchStatus ResolveFetchPath(const std::string& dest_dir, const std::string& filename, std::size_t max_path_bytes, std::string* resolved);` | 单段文件名 + 沙箱校验，成功写规范化绝对路径 | 可单独测试 |
| `bool ParseSha256Hex(std::string_view text, std::string* canonical_lower_hex);` | 校验 64 位十六进制（大小写不敏感、允许首尾空白） | 摘要预检 |
| `const char* FetchStatusToString(FetchStatus status) noexcept;` | 转 snake_case 文本（`path_rejected`、`checksum_mismatch`…） | app 层状态名约定 |
| `inline constexpr std::uint64_t kFetchDefaultMaxBytes` / `kFetchMaxBytesCeiling` | 64 MiB / 4 GiB | 上限见 §5 |
| `FetchOptions{filename, expected_sha256, max_bytes, connect_timeout_ms, read_timeout_ms, total_timeout_ms, max_retries, retry_backoff_ms, overwrite, allow_redirects, max_redirects}` | 默认值即安全默认（不覆盖、64 MiB、3 次重试） | **头文件未提供进度回调**（无 callback 参数/字段）；`Fetch` 同步阻塞，调用方只能拿到最终 `attempts`/`message` |

可重试错误：DNS/连接失败、超时、截断、TLS/curl 临时失败、摘要不符、HTTP 408/425/429/5xx；不可重试：参数/沙箱、目标已存在、超上限、本地 IO、其余 4xx（`fetch.hpp:52-56`）。用尽重试返回 `kRetryExhausted`。

### 3.2 file_browser

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `explicit FileBrowser(std::string root, FileBrowserOptions options = FileBrowserOptions{});` | 构造时 `weakly_canonical` 根目录；失败则对象无效，后续操作返回 `kPathRejected` | `valid()`/`root()`/`options()` 只读 |
| `FileBrowserStatus List(std::string_view rel, std::vector<FileEntry>* entries) const;` | 列目录，条目按 `name` 字节序升序；`FileEntry{name,size,mtime_ms,type}` | 空串表示根；`include_hidden` 控制 `.` 前缀 |
| `FileBrowserStatus Get(std::string_view rel, std::string_view local_path) const;` | 根内文件复制到本地，先写临时文件再发布 | `overwrite=false` 用 `link()` 发布，`kAlreadyExists` |
| `FileBrowserStatus Put(std::string_view rel, std::string_view local_path) const;` | 本地文件复制进根内，父目录自动创建 | 源必须是普通文件，否则 `kNotRegular` |
| `FileBrowserStatus Stat(std::string_view rel, FileEntry* entry) const;` | 元信息（不跟随符号链接） | 空串表示根 |
| `FileBrowserStatus List(const std::string& root, std::string_view rel, std::vector<FileEntry>* entries, const FileBrowserOptions& options = FileBrowserOptions{});` | 便捷自由函数（每次构造一次 `FileBrowser`） | `Get`/`Put` 同名重载同样存在 |
| `const char* FileEntryTypeToString(FileEntryType) noexcept;` / `FileBrowserStatusToString(FileBrowserStatus) noexcept;` | 类型/状态转文本 | `kFile`/`kDirectory`/`kSymlink`/`kOther` |

`FileBrowserStatus`：`kOk`、`kInvalidArgument`、`kPathRejected`、`kNotFound`、`kNotRegular`、`kTooLarge`、`kIoError`、`kPermissionDenied`、`kAlreadyExists`、`kTooManyEntries`。

### 3.3 backup

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `BackupResult Backup(const std::vector<std::string>& paths, const std::string& out_zip, const ZipLimits& limits = ZipLimits{});` | 多路径递归打包，条目名取输入 basename；`stats{files,directories,symlinks_skipped,special_skipped,input_bytes,archive_bytes}` | **没有 plan/run 两阶段 API**：一次调用完成，旧包在成功后原子替换、失败保持原样 |
| `UploadResult Upload(const std::string& zip_path, const UploadRequest& request, const UploadOptions& options = UploadOptions{});` | PUT/POST 上传，2xx 成功；重试耗尽返回最后一次失败分类，`attempts` 为总次数 | 只读本地包，**任何失败都不删除**；curl 缺失立即 `kCurlMissing` |
| `struct UploadRequest{endpoint, method, headers, token, content_type}` | endpoint 禁空白/控制字符/双引号；token 追加 `Authorization: Bearer` | token 经 stdin 配置传入，不进 argv |
| `struct UploadOptions{max_retries, retry_delay, connect_timeout, request_timeout, verify_remote, verify_mode, verify_checksum_header, verify_timeout}` | `verify_remote=true` 时 2xx 之后再 GET（`kGet`）或 HEAD（`kHead`），比较大小与 SHA-256 | **FR-18 远端校验已实现**（`backup.cpp:1077-1090`，提交 `046d39f`；[bento-scenarios.md](bento-scenarios.md) §21.2/§21.4 的“远端校验未做”写于该提交之前）；默认 `false`，不开不产生额外请求 |
| `RemoteVerifyMode` / `HttpMethod` / `UploadStatus` + `ToString` | 校验方式、方法、状态枚举 | `kRemoteVerifyFailed` 时本地包保留 |

`UploadStatus`：`kOk`、`kInvalidArgument`、`kZipMissing`、`kCurlMissing`、`kNetworkError`、`kHttpRejected`、`kTimeout`、`kLocalIoError`、`kRemoteVerifyFailed`（无 `kRetryExhausted`，重试耗尽回落到具体分类）。

### 3.4 zip

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `ZipStatus Open(const std::string& output_path, const ZipLimits& limits = ZipLimits{});` | 创建/截断输出、写本地头 | 已 Open 时先 `Abort()`（会删除上一份输出，审查 D-12） |
| `ZipStatus AddFile(const std::string& source_path, const std::string& entry_name);` | 普通文件条目（不跟随符号链接） | 失败即进入 poison 态 |
| `ZipStatus AddBytes(const std::string& entry_name, const std::uint8_t* data, std::size_t size);` | 内存条目 | `size==0` 允许 `data==nullptr` |
| `ZipStatus AddDirectory(const std::string& source_dir, const std::string& entry_prefix = {});` | 递归添加，按路径排序保证稳定顺序；跳过符号链接/特殊文件并计数 | 先写 `entry_prefix/` 目录条目（非空时） |
| `ZipStatus AddExclusion(const std::string& path);` | 登记不参与递归的绝对路径（输出包自身） | 防止归档把自己打进去 |
| `ZipStatus Close();` / `void Abort() noexcept;` | 写中央目录与 EOCD；失败删输出；`Abort` 幂等 | 析构未 Close 自动 `Abort` |
| `ZipListing ListZip(const std::string& zip_path, const ZipLimits& limits = ZipLimits{});` | 只读中央目录，返回 `entries{name,size,crc32,is_directory}` | 校验签名与偏移，不读数据 |
| `ZipResult ExtractZip(const std::string& zip_path, const std::string& output_dir, const ZipLimits& limits = ZipLimits{});` | 解压 store-only 条目，逐条重算 CRC-32 与声明值比对 | 名字过 Zip Slip 校验；CRC 不符删该文件并 `kCorruptArchive` |

格式：固定 30B 本地头 / 46B 中央目录项 / 22B EOCD，全部小端，无 extra/注释/数据描述符；只接受 method 0，压缩条目返回 `kUnsupportedCompression`。

### 3.5 capability

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `CapabilityMatrix Probe(const ProbeOptions& options = ProbeOptions{});` | 执行 15 项只读探测，固定顺序返回 | 不缓存、不抛异常；单项失败记为 `kDegraded`/`kUnavailable` |
| `const CapabilityItem* Find(std::string_view name) const noexcept;` | 按稳定名查找，缺失返回 `nullptr` | 调用方须处理缺失项 |
| `std::size_t Count(CapabilityStatus status) const noexcept;` | 统计某状态项数 | — |
| `bool IsDegraded() const noexcept;` / `std::vector<std::string> Reasons() const;` | 是否存在非可用项；全部原因（`"<name>: <reason>"`，原因为空退化为状态名） | 启动日志建议输出 `Summary()` |
| `std::string Summary() const;` | 单行摘要（项数/可用/受限/不可用） | — |
| `struct ProbeOptions{proc_root, cgroup_root, tmp_path, root_path, min_nofile, min_nproc, min_mem_available, check_epoll, check_interfaces, check_cgroup, check_rlimit}` | 全部路径可覆盖，测试注入假路径 | 默认探测真实系统 |

15 项名称：`proc.meminfo`、`proc.cpuinfo`、`proc.self.status`、`proc.self.fd`、`unix.privilege`、`ipc.epoll`、`cgroup.version`、`cgroup.memory`、`cgroup.cpu`、`net.interfaces`、`fs.tmpfs`、`fs.root_readonly`、`rlimit.nofile`、`rlimit.nproc`、`kernel.info`。`CapabilityStatusName` 返回 `"available"`/`"degraded"`/`"unavailable"`。

### 3.6 command

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `CommandStatus ValidateCommand(const Command& command, const CommandPolicy& policy, std::string* reason = nullptr);` | 只校验不执行；返回 `kOk`/`kInvalidArgument`/`kDenied` | 参数超限在帧解码前就拒绝 |
| `std::vector<std::uint8_t> EncodeCommand(const Command& command);` | 请求帧（16B 头，大端）；非法返回空 vector | magic `'B' 'C'` |
| `bool DecodeCommand(const std::uint8_t* data, std::size_t size, Command* out, std::string* error = nullptr);` | 对每段长度做上界与精确总长校验；失败不改 `*out` | 绝不信任对端 |
| `std::vector<std::uint8_t> EncodeCommandResult(const CommandResult& result);` / `bool DecodeCommandResult(...);` | 结果帧（32B 头）编解码 | 上限 16MB |
| `explicit CommandExecutor(CommandPolicy policy = CommandPolicy{});` | 缺省策略即**全拒绝**（`allow_all=false` 且白名单空） | 必须显式配置白名单 |
| `CommandResult Execute(const Command& command) const;` / `CommandResult Execute(const Command& command, const CommandCallbacks& callbacks) const;` | 校验 → spawn → 边读边等 → 超时杀进程组 → 汇总 | 回调：`on_spawn`/`on_stdout`/`on_stderr`/`on_exit`，异常被吞掉 |
| `const std::vector<std::string>& BuiltinDangerousCommands();` | 内置危险命令表（70+ 项：shell/解释器、rm/dd、chmod/chown、kill/systemctl、nc/ssh/curl…） | 即使 `allow_all=true` 也拒绝，除非 `allow_dangerous` |

杀进程组：spawn 时 `new_session=true`（`command.cpp:672`），超时先整组 `SIGTERM`，`kill_grace_ms` 后整组 `SIGKILL`（`terminate_tree`）。上限：64 项 argv × 4KB、超时 30s 默认/10min 硬顶、每路输出 1MB 默认/4MB 硬顶。

### 3.7 file_transfer

| 签名 | 语义与返回值 | 备注 |
|---|---|---|
| `FileTransferStatus ResolveTransferPath(const std::string& root, const std::string& relative_path, std::size_t max_path_bytes, std::string* resolved);` | 沙箱解析：拒绝绝对路径/`..`/反斜杠/冒号/空/超长/控制字符，解析后须在 root 下 | 可单独测试 |
| `std::vector<std::uint8_t> EncodeFileHeader(const FileHeader&) / EncodeFileChunk(const FileChunk&) / EncodeFileAck(const FileAck&);` | HEADER(60B)/CHUNK(56B)/ACK(24B) 帧编码；非法返回空 vector | magic `'B' 'F'`，大端 |
| `bool DecodeFileHeader(...) / DecodeFileChunk(...) / DecodeFileAck(...);` | 校验固定头、字段上界与精确总长 | 截断/超长一律拒绝 |
| `FileTransferStatus Open(const std::string& source_path, const std::string& transfer_path, std::uint32_t transfer_id, const FileTransferConfig& config = FileTransferConfig{});` | 发送端：打开源文件并算整文件 SHA-256 | `header()`/`chunk_count()`/`next_index()`/`next_offset()` |
| `FileTransferStatus Seek(std::uint64_t offset);` / `void Rewind();` | 断点续传对齐到 `offset` 所在块；回到开头 | `offset <= size` |
| `FileTransferStatus BuildChunkFrame(std::uint32_t index, std::vector<std::uint8_t>* frame);` / `BuildNextChunkFrame(...)` | 读指定块/下一块并编码，块级 SHA-256 随帧 | 块发完返回 `kAlreadyCompleted` |
| `FileTransferStatus Open(const FileHeader& header, const std::string& root_dir, const FileTransferConfig& config = FileTransferConfig{});` | 接收端：校验 HEADER、解析目标、按 `.part` 块边界恢复 | `resume_offset()` 为 0 表示无续传 |
| `FileTransferStatus ApplyChunk(const FileChunk& chunk);` / `ApplyChunkFrame(...)` | 校验 index/offset/长度/块摘要后按序写入 | `kDuplicateChunk` 是幂等成功 |
| `FileTransferStatus Finish();` / `void Cancel();` / `FileAck MakeAck(FileAckReason reason) const;` | Finish 重算整文件 SHA-256 后 `rename`；Cancel 删除 `.part` | `kChecksumMismatch` 时 `.part` 被删 |

`FileTransferStatus`：`kOk`、`kInvalidArgument`、`kPathRejected`、`kTooLarge`、`kIoError`、`kChecksumMismatch`、`kSequenceError`、`kRetryExhausted`、`kDuplicateChunk`、`kNotReady`、`kAlreadyCompleted`、`kTargetExists`、`kCancelled`。重试语义：同块允许 3 次重发、第 4 次失败才 `kRetryExhausted`。

## 4. 设计要点

1. **为什么 store-only**：备份只需要“把目录树原样装进一个文件”。method 0 让格式、CRC 与偏移都能用标准库几十行完成，不引入 zlib/libzip，且写入端可在写完后 seek 回填 CRC/大小，不必整文件缓存（`zip.hpp:6-10`、`zip.cpp:4-12`）。确定性带来可校验性：`ExtractZip` 逐条重算 CRC 即可判定损坏。代价明确——**不压缩，上传字节数等于原始字节数**。
2. **为什么 SHA-256 全量校验**：fetch 边收边算、在同一个 `O_NOFOLLOW` fd 上校验后才 `rename`；file_transfer 同时做块级与整文件摘要，块摘要先于写盘，整文件摘要在 `Finish` 前重算，因此“续传接错内容”只能造成失败、不会静默落下错误文件；Upload 的 `verify_remote` 用 GET/HEAD 回读比较大小与摘要。
3. **重试退避统一**：fetch 与 backup 共用 `app::detail::SleepBeforeRetry(std::chrono::milliseconds base, int failed_attempt, std::chrono::milliseconds cap)`（`detail/retry.hpp:31`），`failed_attempt` 从 1 起、每次翻倍并受 `cap` 约束；下载封顶 2s（`kFetchMaxBackoffMs`），上传封顶 30s。
4. **沙箱路径规范化防穿越**：先做语法拒绝（单段/无 `..`/无反斜杠冒号/长度），再 `weakly_canonical` 后确认仍在根下，打开时加 `O_NOFOLLOW`。`FileBrowser` 的 `overwrite=false` 发布改用 `link()`（`file_browser.cpp:203-224`），使“检查后目标出现”由内核 `EEXIST` 拒绝，而不是被 `rename` 覆盖。
5. **命令白名单默认拒绝**：`CommandPolicy` 默认 `allow_all=false` + 空白名单，任何命令都是 `kDenied`；`denied_commands` 优先级最高，`allow_dangerous` 也绕不过；argv 直传 `execve`、不经 shell，`;`/`|`/`$()` 无注入面。
6. **续传 `.part` 与已知 TOCTOU（如实保留）**：`FileReceiver` 的临时文件固定为 `<target>.part`（`file_transfer.cpp:800-801`），用 `std::fstream` 按路径打开、**无 `O_NOFOLLOW`**；`Open` 时用 `exists`/`FileSize` 恢复偏移，`Open` 的目标存在性检查与 `Finish` 的 `rename` 之间也存在竞争窗口。审查记录 `docs/review/review-batch2-api.md:278-279` 的 D-05（file_browser 检查-发布 TOCTOU）与 D-06（backup 临时包固定名）针对同批 app 模块：当前源码里 D-06 已改为 `<out>.part.<pid>.<seq>`（`backup.cpp:901-905`）、D-05 对应路径已用 `link()`，但 `AI_COLLAB.md` §三与 `bento-scenarios.md` §21.4 仍写着“未修”。本文按源码现状陈述，**不据此宣称缺陷已消除**——没有构造并发复现用例，也未见测试覆盖该竞争。

## 5. 边界与已知限制

| 项 | fetch | file_browser | zip | file_transfer | command |
|---|---|---|---|---|---|
| 单文件上限 | 默认 64 MiB / 硬顶 4 GiB | 默认 1 GiB / 硬顶 1 TiB | 单条 1 GiB，总量 2 GiB（硬顶 4 GiB-1） | 默认 1 GiB / 硬顶 1 TiB | — |
| 条目数 | — | 默认 100000 / 硬顶 1000 万 | 默认 16384 / 硬顶 65535 | — | argv 64 项 × 4KB |
| 超时 | 连接 5s、读空闲 15s、总 10min（均硬顶 10min） | — | — | — | 默认 30s / 硬顶 10min；TERM→KILL 宽限 300ms |
| 并发 | 每次调用独立；同步阻塞 | 值对象；同目标并发 Put 由 `link()` 决定唯一成功者 | `ZipWriter` 非线程安全 | 非线程安全，同目标并发接收会互写同一 `.part` | `Execute` 可并发，每次独立子进程 |

- **沙箱边界**：所有路径校验都**不防 root**（能读任意路径的进程不受 `dest_dir`/`root` 约束），也不防符号链接竞争——`zip` 解压直接用 `ofstream` 打开目标、`file_transfer` 的 `.part` 用 `fstream` 打开，若目标已被替换为链接，写入会跟随；`fetch`/`file_browser` 的关键打开点带 `O_NOFOLLOW`，但父目录仍可能被并发替换。`command` 的 cwd 沙箱只是字符串级归属校验，不 chroot、不降权。
- **HTTP/TLS**：明文分支只支持 HTTP/1.1 `identity` 编码，声明 `gzip`/`br` 返回 `kProtocolError`；无代理支持。HTTPS 分支完全交给系统 `curl`，配置只加 `proto`/`proto-redir = "=https"`（`fetch.cpp:1316-1318`），**没有** `--insecure` 或自定义 CA 选项，证书校验即 curl 默认策略；`curl` 缺失时 `Fetch` 返回 `kBackendUnavailable`、`Upload` 返回 `kCurlMissing`（明文不受影响）。
- **未实现 / 未接入**：`fetch` 无进度回调、无代理；`Upload::verify_remote` 默认关闭，GET 模式会把远端对象完整下载一遍；`Probe()` 在 `src/` 下**没有任何生产消费者**（`docs/review/review-batch2-api.md:134`），CON-06 的“探测 → 降级策略”仍由调用方实现；app 层无统一 `LogHook` 注入点。
- **协作约束**：`ZipWriter`/`FileSender`/`FileReceiver` 一个实例只允许单线程；`Fetch` 在 Fiber 中调用会阻塞当前 M，需要并发请交给线程池；`Backup`/`Upload` 每次调用内部独立建对象，非线程安全。

## 6. 测试数据

| 测试文件 | 用例/断言（HEAD `afc135a` 实测） | 覆盖点 |
|---|---|---|
| `test_fetch.cpp` | 25 / 192 | 进程内真实 HTTP server：1MB 下载与 SHA-256、404 不重试、读超时、503 重试/耗尽、`Content-Length` 截断、chunked、重定向与上限、路径逃逸、并发下载、HTTPS/curl 分支与 symlink/超限拒绝 |
| `test_upload.cpp` | 18 / 102 | 进程内 HTTP server：PUT/POST、token 与自定义 header、重试后成功、重试耗尽保留本地包、连接失败、参数校验、远端校验 |
| `test_file_browser.cpp` | 13 / 160 | 构造/List 排序与隐藏项、Stat、Get/Put 与覆盖语义、1MB 往返、`..`/绝对路径/反斜杠/盘符/超长、符号链接逃逸、大小与条目上限、权限（root 跳过 EACCES） |
| `test_backup.cpp` | 13 / 100 | 1MiB 目录打包/解包比对、多路径、输出自排除、空目录、失败保留旧包、重名、符号链接、上限、16MiB 吞吐（`[perf]`） |
| `test_zip.cpp` | 20 / 181 | CRC 向量、单文件/目录往返、1MB、上限、非法名字、损坏归档、随机树、系统 `unzip` 交叉验证 |
| `test_capability.cpp` | 6 / 65 | 正常环境、矩阵 API、缺失 `/proc`、低 rlimit、缺失 cgroup、关闭探测开关（全部注入假路径） |
| `test_command.cpp` | 14 / 86 | 帧边界、白名单/拒绝表/内置危险命令优先级、参数边界、cwd 沙箱、真实执行、stderr/非零退出、四个回调、超时杀进程树、输出截断、spawn 失败、不经 shell 的注入面 |
| `test_file_transfer.cpp` | 14 / 145 | SHA-256 向量、三帧往返与非法帧、路径沙箱与符号链接逃逸、1MB/16 块、断点续传、块重试 3 次/第 4 次耗尽、空文件、大小上限、整文件摘要不符与原子落盘 |

运行命令：`ctest -R '^bento_' --output-on-failure`（CTest 名称注册见 `cmake/Go2CppBento.cmake:365-371`）。

> 数字口径：用例数 = 各 `main()` 中 `Test*` 调用数；断言数 = `grep -c 'BENTO_CHECK('` + `grep -c 'BENTO_CHECK_EQ('`（本仓库没有 `CHECK(` 宏，模板里的 `grep -c 'CHECK('` 在此仓库恒为 0，故用真实宏名）。[bento-modules.md](bento-modules.md)（对应 `046d39f`）与 [bento-scenarios.md](bento-scenarios.md) §21.5 给出了另一口径的“用例/断言”数字（含循环内重复执行的 checks），**两者不可混用**；本次文档编写未构建、未运行测试。

## 7. 示例

① zip 打包目录 → 列中央目录（含 CRC-32）→ 解压并核对内容：

```cpp
// zip_demo.cpp
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "go2cpp/bento/app/zip.hpp"

namespace fs = std::filesystem;
namespace app = go2cpp::bento::app;

int main() {
    const fs::path work = fs::temp_directory_path() / "bento-zip-demo";
    fs::remove_all(work);
    fs::create_directories(work / "src");
    { std::ofstream out(work / "src" / "a.txt"); out << "hello bento"; }
    const std::string archive = (work / "demo.zip").string();

    app::ZipWriter writer;
    if (writer.Open(archive) != app::ZipStatus::kOk ||
        writer.AddDirectory((work / "src").string(), "src") != app::ZipStatus::kOk ||
        writer.Close() != app::ZipStatus::kOk) {
        writer.Abort();
        std::cerr << "打包失败\n";
        return 1;
    }
    const app::ZipListing listing = app::ListZip(archive);
    if (!listing.ok()) return 1;
    for (const app::ZipEntryInfo& entry : listing.entries) {
        std::cout << entry.name << " size=" << entry.size
                  << " crc32=0x" << std::hex << entry.crc32 << std::dec << '\n';
    }
    const std::string out_dir = (work / "out").string();
    if (!app::ExtractZip(archive, out_dir).ok()) {  // 逐条重算 CRC-32 并比对
        std::cerr << "解压/CRC 校验失败\n";
        return 1;
    }
    std::ifstream restored(out_dir + "/src/a.txt");
    std::string line;
    std::getline(restored, line);
    std::cout << "解压内容: " << line << '\n';
    return line == "hello bento" ? 0 : 1;
}
```

② capability 探测 + fetch 下载校验（明文 HTTP，不依赖 curl）：

```cpp
// probe_fetch_demo.cpp
// 前置：在提供文件的目录执行 `python3 -m http.server 8000`，
//       URL 形如 http://127.0.0.1:8000/a.bin；HTTPS 才需要系统 curl。
//       dest-dir 必须已存在（Fetch 不会自动创建目标目录）。
#include <iostream>
#include <string>

#include "go2cpp/bento/app/capability.hpp"
#include "go2cpp/bento/app/fetch.hpp"

int main(int argc, char** argv) {
    namespace app = go2cpp::bento::app;
    const app::CapabilityMatrix matrix = app::Probe();
    std::cout << matrix.Summary() << '\n';
    for (const std::string& reason : matrix.Reasons()) std::cout << reason << '\n';
    if (argc < 3) {
        std::cerr << "usage: probe_fetch_demo <http-url> <dest-dir> [sha256]\n";
        return 0;
    }
    app::FetchOptions options;
    options.max_bytes = 8ull * 1024 * 1024;
    options.max_retries = 2;
    if (argc >= 4) {
        std::string hex;
        if (!app::ParseSha256Hex(argv[3], &hex)) return 2;
        options.expected_sha256 = hex;
    }
    const app::FetchResult result = app::Fetch(argv[1], argv[2], options);
    if (!result.ok()) {
        std::cerr << app::FetchStatusToString(result.status) << ": " << result.message << '\n';
        return 1;
    }
    std::cout << result.path << " bytes=" << result.bytes << " sha256=" << result.sha256 << '\n';
    return 0;
}
```

编译（库名见 `cmake/Go2CppBento.cmake:81,128`，构建目录按实际替换 `build`）：

```sh
g++ -std=c++17 -I include zip_demo.cpp -L build \
    -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os -lgo2cpp_hash -lpthread -o zip_demo
g++ -std=c++17 -I include probe_fetch_demo.cpp -L build \
    -lgo2cpp_bento_capability -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os \
    -lgo2cpp_hash -lpthread -o probe_fetch_demo
```

## 8. 相关文档

- [bento-foundation.md](bento-foundation.md)：`bento::os`/`hash`/`frame` 等底座，本组模块只向下依赖它们。
- [bento-session.md](bento-session.md)：`app::Session` 编排层（不在本文范围）。
- [bento-transport.md](bento-transport.md)：`ITransport`/TCP/内存/UDP 传输，`command`/`file_transfer` 的帧可跑在其上但不是依赖。
- [bento-scenarios.md](bento-scenarios.md) §21：app 层落地记录与 FR-09/FR-15/FR-18/CON-06 映射。
- [bento-modules.md](bento-modules.md)：MOD 编号与覆盖矩阵（对应 `046d39f`，数字口径见 §6 注）。
- [PROTOCOL.md](../../tests/bento/PROTOCOL.md)：Bento 测试协议的帧格式与约定。
