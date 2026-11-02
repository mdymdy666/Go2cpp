# file_browser 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/app/file_browser.hpp`、`src/bento/app/file_browser.cpp`、
  `tests/bento/test_file_browser.cpp`
- 审计工具：`tests/bento/audit_file_browser_stress.cpp`（独立可执行，**不包含**
  `test_util.hpp`，因此不引入 `link.hpp`；直接与 `file_browser.cpp` 链接）
- 环境：WSL2 Ubuntu 24.04，GCC 13.3.0，12 vCPU / 19 GiB，`/tmp` ext4，uid=1000；
  构建目录 `.build-bento-wsl`（Release，`-Wall -Wextra -Wpedantic -Werror`）
- 结论：**无 P0**。修复 **2 个 P1** 与 **4 个 P2**，另做 3 项性能增强。HEAD 基线
  13 组 / 1890 checks（任务描述的 12 组 / 1881 checks 是旧版，见 §4）；增强后
  **19 组 / 5748 checks 全过**；独立审计 **10,856 checks 全过**；
  `ctest -R bento` **48 项 47 过 + 1 项无关 SKIP**（`bento_netem` 缺 `CAP_NET_ADMIN`）。

## 1. 问题清单（P0/P1/P2 + 证据 + 处理）

### P1-1 根目录构造后被替换为符号链接时，`List("")` 列出外部目录

- 证据（修复前审计）：构造 `FileBrowser(root)` 后删除 root、换成指向 `outside/`
  的符号链接，`List("")` 返回 `kOk` 并列出外部内容；`Stat("")` 也返回 `kOk`。
- 根因：`Resolve` 对空 `rel` 直接返回 `m_root`，绕过了所有规范化/逃逸检查；
  `List` 随后 `stat` + `directory_iterator` 跟随了根符号链接。
- 修复：新增 `CheckRootDirectory()`（`lstat` 根路径）：删除 → `kNotFound`，
  符号链接/非目录 → `kPathRejected`；`Resolve`/`Stat` 每次操作先复核；
  `List` 用 `open(O_RDONLY|O_DIRECTORY|O_NOFOLLOW)` 打开目录。
- 回归：`TestRootLifecycle`、审计场景 D/G；变异测试“禁用根复核”产生 10 项失败
  （见 §4.3）。

### P1-2 长文件名（约 ≥ 225 字节）无法 Get/Put

- 证据（修复前探针，HEAD 代码）：240 字节文件名 `Get`/`Put` 均返回
  `kPathRejected`，文件没有落盘；修复后同一探针均 `kOk`。
- 根因：临时名 = **目标名 + `.go2cpp-part.<pid>.<seq>`**，目标名接近
  `NAME_MAX=255` 时临时名超长，`open()` 返回 `ENAMETOOLONG`。
- 修复：`MakeSiblingTempPath()` 改为同目录固定前缀
  `.go2cpp-part.<pid>.<seq>`（< 48 B），不再拼接目标名；`O_EXCL` + 最多 16 次
  换序号重试。
- 回归：`TestEmptyUnicodeAndLimits` 的 240 B 往返；变异测试“恢复拼接目标名”
  产生 4 项失败。

### P2-1 `ENAMETOOLONG` / `ELOOP` 在 List/Get/Put 被误报 `kIoError`

- 证据（修复前）：256 字节组件 `List`/`Get`/`Put` = `kIoError`，而 `Stat` =
  `kPathRejected`；随机 2510 用例中 `io_error=228`。
- 修复：`MapError()` 补 `filename_too_long` / `too_many_symbolic_link_levels`
  → `kPathRejected`、`is_a_directory` → `kNotRegular`、`file_exists` →
  `kAlreadyExists`，与 `MapErrno()` 对齐。
- 修复后：随机用例 `io_error=0`，256 B 组件四个入口统一 `kPathRejected`。

### P2-2 `Put` 会把构造后删除的根目录重新创建出来

- 证据（修复前）：删除 root 后 `Put("resurrect.txt")` 返回 `kOk`，root 目录重新
  出现（`Resolve` 不要求根存在，随后 `create_directories` 把根建回来）。
- 修复：同 P1-1 的根复核；现在返回 `kNotFound` 且 `!exists(root)`。
- 回归：`TestRootLifecycle`、审计场景 G。

### P2-3 崩溃残留的临时名把首次 Get/Put 误报 `kAlreadyExists`

- 证据（预置 `.go2cpp-part.<pid>.0` 的探针，HEAD 代码）：目标不存在时
  `Get` 仍返回 `kAlreadyExists`，残留文件留在目录里；修复后换序号重试，
  `Get=kOk`，目标正确生成，旧残留不受影响。
- 修复：`CopyAtomically()` 对临时名 `EEXIST` 最多重试 16 次；仍失败返回
  `kIoError`（不能报“目标已存在”）。

### P2-4 `local_path` 含 NUL 被静默截断

- 证据：修复前 `"…/nul-target\0evil"` 的 `Get=kAlreadyExists`、`Put=kNotFound`
  （路径按 NUL 截断）；修复后均为 `kInvalidArgument`，且没有文件产生。
- 修复：`Get`/`Put` 入口检查 `local_path.find('\0') != npos`。
- 说明：`Get` 的临时名同样会在 NUL 处截断、与目标同名导致 `link` 自指失败，
  未实际写盘；但状态码具有误导性。

### 性能增强（非缺陷）

1. **路径解析快路径**：默认模式下根和所有已存在段都经 `lstat` 确认非符号链接，
   词法拼接即为真实路径，跳过 `weakly_canonical` 的第二次全量遍历；只有
   `follow_symlinks=true` 且真的遇到链接时才做真实路径判定。
2. **List 改用 `readdir` + `fstatat(dirfd, AT_SYMLINK_NOFOLLOW)`**：不再为每个
   条目构造 `fs::path`/`std::string` 全路径，也不再经 `std::filesystem` 错误码层。
3. **原子发布优先 `renameat2(RENAME_NOREPLACE)`**：一次系统调用完成“不覆盖发布”，
   不依赖硬链接支持（FAT/exFAT/部分网络盘），不可用时自动回退 `link()+unlink()`。

## 2. 压测数据

### 2.1 1 GiB 流式 Get/Put（本地 ext4，单线程）

| 操作 | 耗时 | 吞吐 | 峰值 RSS | fd | 临时残留 |
|---|---:|---:|---:|---:|---:|
| Get 1 GiB | 1289.8 ms | **794 MiB/s** | 6.2 MiB | 5→5 | 0 |
| Put 1 GiB | 1452.0 ms | **705 MiB/s** | 6.2 MiB | 5→5 | 0 |
| 超限（1 GiB+1 稀疏文件）Get | 0.006 ms | — | — | — | 0（`kTooLarge`，未复制） |

- 源文件生成（非模块）：1024 MiB / 1258 ms = 814 MiB/s，说明 Get/Put 已接近
  本机页缓存 + ext4 写回速度。
- 磁盘峰值：源 + 下载副本 + 上传副本 = 3 GiB；删除后逻辑占用归零。
- 内容：1 GiB 全量逐块校验一致；大小与 `st_size` 一致。

### 2.2 10 万次 List/Stat 与小文件吞吐（A/B 为同一 quick 负载）

| 用例 | 修复前 | 修复后（quick） | 修复后（full） |
|---|---:|---:|---:|
| List（根 33 项） | 12,913 op/s | 19,604 op/s（+52%） | 19,314 op/s（100k，5.18 s） |
| Stat（单段） | 152,532 op/s | 367,755 op/s（+141%） | 370,280 op/s（100k，270 ms） |
| Stat（4 段嵌套） | 63,888 op/s | 137,415 op/s（+115%） | 138,417 op/s（50k，361 ms） |
| Put 4 KiB（覆盖） | 5,546 op/s | 7,016 op/s | 6,402 op/s（3k，469 ms） |
| Get 4 KiB（覆盖） | 5,059 op/s | 6,319 op/s | 6,204 op/s（3k，484 ms） |

- fd 5→5；RSS 4.0→4.5 MiB（峰值 6.2 MiB）；临时文件残留 0。
- 4 KiB 小文件受 ext4 `open/rename` 元数据限制（~160 µs/次），路径解析优化
  对它帮助有限；这是环境上限，不是模块缺陷。

### 2.3 并发（8 线程，同目录）

| 场景 | 结果 |
|---|---|
| 8 线程 × 200 轮独立文件 Put+Get | 1600 次，166.9 ms，**9,589 op/s**，失败 0，临时残留 0，fd 稳定 |
| 8 线程 × 30 轮同一目标 `overwrite=false` | **ok=30，exists=210，其它=0**（每轮恰一个成功） |
| 目录被并发增删时 List | 300 次全部 `kOk`，无崩溃/异常状态 |

### 2.4 随机路径（2510 用例 × 4 个入口）

- 强制拒绝用例 **1997**；四个入口（List/Stat/Get/Put）全部断言 `kPathRejected`。
- 覆盖：`..` 各位置、绝对路径、`//`、尾斜杠、`.`、反斜杠、盘符、UNC、
  NUL、控制字符、1024/1025 字节、300 字节组件、255/256 字节名、Unicode、
  符号链接（内/外/链/环）、FIFO、百分号编码、通配符、`$HOME`、空白等。
- 状态分布：`ok=178 rejected=8473 not_found=1271 not_regular=60 exists=58`，
  **`io_error=0`**（修复前 228）。
- 副作用：外部目录快照前后完全一致；根内无临时文件；失败 Get 不留下目标；
  成功 Put 的目标均在根内且内容/大小正确。

## 3. 场景数据（≥5 个复杂场景，全部实跑）

| 场景 | 实测 |
|---|---|
| **路径逃逸**（`../`、绝对、外部链接、链接链、链接环） | 全部拒绝；`follow=true` 只放行根内链接；`Stat` 能报告单段符号链接本身；外部文件快照不变 |
| **1 GiB 大文件流式传输** | Get 794 MiB/s、Put 705 MiB/s；RSS 6.2 MiB；fd 不增长；无残留 |
| **8 线程并发 Get/Put** | 见 §2.3；同一目标恰好一个 `kOk`，其余 `kAlreadyExists` |
| **权限拒绝**（`chmod 000`、只读根、只读本地目录） | 13 项检查全过：List/Get/Put/本地目标 EACCES 均 `kPermissionDenied`，只读根读取仍可用 |
| **沙箱根不存在 / 只读 / 被删除 / 被替换** | 构造时无效 → `kPathRejected`；构造后删除 → `kNotFound` 且 Put 不重建；替换为符号链接 → `kPathRejected`，外部不可写 |
| 覆盖语义（额外） | 覆盖符号链接目标被拒绝，链接本身不被写穿；本地链接被替换而非写穿 |
| 边界（额外） | 0 字节、Unicode、240 B 长名、NAME_MAX 255/256、1024/1025 字节路径、FIFO 均符合预期 |

## 4. 测试复查

### 4.1 规模与 SKIP 逃逸

- 任务描述的“12 组 / 1881 checks”与 HEAD 不符：HEAD 实测 **13 个测试函数 /
  1890 checks**（`TestGetNoOverwriteRace`/D-05 未计入头注释，差 9 项断言）。
- **SKIP 逃逸（已修）**：旧 `TestPermissionDenied` 在 `geteuid()==0` 时调用
  `bento_test::Skip`；`test_util.hpp` 的 `Finish()` 只要 `g_skips != 0` 就返回 77，
  而 CMake 把 77 配置为 `SKIP_RETURN_CODE`。也就是说 **root/容器 CI 下整套 1890
  checks 会被记为 Skipped**，其余断言全部“看不见”。
- 修复：权限用例在 root 下临时 `setegid/seteuid(65534)`，执行完后恢复；不再使用
  `Skip`。本环境 uid=1000，root 分支经代码审查/编译，但未以真实 root 实跑
  （审计程序另含 `fork + setuid(65534)` 的 root 路径）。

### 4.2 断言有效性（变异测试）

| 变异 | 结果 |
|---|---|
| 删除 `List` 的 `std::sort` | HEAD 测试 **10 项失败**（捕获） |
| `Resolve` 不再拒绝符号链接 | **1 项失败**（捕获） |
| `SplitRelativePath` 允许 `..` | **331 项失败**（捕获） |
| 临时名恢复为“目标名 + 后缀” | 增强后测试 **4 项失败**（新增长名用例捕获） |
| 禁用 `CheckRootDirectory` 复核 | 增强后测试 **10 项失败**（新增根生命周期/逃逸用例捕获） |

### 4.3 覆盖缺口（已补）

- 排序：旧夹具 6 项可能恰好按 readdir 顺序通过；新增“逆序创建 25 项 + Unicode”
  的确定性排序用例。
- 逃逸：旧用例只断言状态码，不检查“拒绝后是否产生副作用”；新用例检查外部快照、
  目标不存在、链接不被写穿。
- 缺失场景：根目录删除/替换、240 B 长名、NUL 参数、NAME_MAX、1024/1025 路径、
  0 字节、Unicode、FIFO、8 线程并发与同目标竞争——已全部补入（19 组）。
- 随机路径从 500+200 扩到 **1200+300**，并加入 NUL/尾斜杠/300 B 段。
- 仍无法在单测覆盖：NFS/FAT 的 `renameat2` 回退、EIO/ENOSPC 故障注入、掉电一致性、
  中间段 TOCTOU 竞态、真实 root 运行。

## 5. 解耦与自包含

- 头文件只包含 5 个标准库头：`<cstddef> <cstdint> <string> <string_view> <vector>`。
- 实现只包含标准库 + POSIX（`fcntl/dirent/stat/unistd/syscall`），**没有**
  `link.hpp`、`peer_hub` 或任何网络模块引用。
- `nm -u file_browser.o`：未定义符号只有 libc/libstdc++/`std::filesystem`
  （`open/read/write/rename/link/lstat/fstatat/readdir/...`），无
  `go2cpp::bento::net` 符号。
- 头文件单独 `-Werror` 编译通过；只链接 `file_browser.cpp` 的最小程序可运行；
  审计程序本身就是“无 go2cpp 网络库”链接的证明。

## 6. 修复/改动文件

| 文件 | 内容 |
|---|---|
| `include/go2cpp/bento/app/file_browser.hpp` | 文档：根生命周期 fail-closed、`renameat2`/临时名重试、错误分类、NUL 参数、线程安全、List 部分结果、TOCTOU 残余风险 |
| `src/bento/app/file_browser.cpp` | 根复核；`PathSegments`（内联 16 段免堆分配）；`lstat` 快路径；`Stat` 直接 `lstat`；`List` 改 `readdir+fstatat`；短临时名 + 重试；`renameat2` 发布；`MapError` 对齐；NUL 校验；`Stat("")` 对 `/` 的名字回退 |
| `tests/bento/test_file_browser.cpp` | 19 组 / 5748 checks；root 降权权限用例；排序、长名、NUL、根生命周期、follow/覆盖、8 线程并发、FIFO；随机 fuzz 扩到 1200+300 |
| `tests/bento/audit_file_browser_stress.cpp` | 新增独立审计程序（压测/随机/场景/权限/解耦） |

仅改上述文件（不含 base）；未 commit。

## 7. 记录的设计风险（未修改）

1. **检查-打开之间的 TOCTOU**：默认模式只对最后一段 `O_NOFOLLOW`；中间段在
   `lstat` 之后仍可能被替换为符号链接。彻底修复需 `openat2(RESOLVE_BENEATH|
   RESOLVE_NO_SYMLINKS)` 或逐段 `openat`（本模块未引入 Linux 5.6+ 专属 API）。
   头文件已明确说明。
2. **无 fsync**：`rename/link` 保证原子可见性，不保证掉电持久性；崩溃可能丢数据或
   留下临时文件（下次重试可绕过）。如需持久性应增加可配置 fsync。
3. **根目录父链被替换**（bind mount/rename 父目录）不在路径字符串防护范围内。
4. **Put 自动建父目录**与解析之间存在竞态；根内攻击者可抢先插入符号链接。
5. **未采用 `copy_file_range`**：本机实测 753 MiB/s，慢于 `read/write` 的
   1053 MiB/s（同文件、1 GiB），故保留 128 KiB 栈缓冲的 read/write。
6. 临时文件以 0644 创建，最终权限受 umask 影响；不保留源文件 mode/mtime。
7. `List` 触发 `kTooManyEntries` 时返回未排序的前 `max_entries` 项（已文档化）。
8. 128 KiB 栈缓冲默认线程栈安全；极小栈调用方需注意。
9. `Stat("")` 对根 `/` 的 `filename()` 为空，已回退为绝对路径作为 name。

## 8. 验证命令与结果

```bash
ninja -C .build-bento-wsl                      # 零警告（-Werror）
.build-bento-wsl/bento_test_file_browser       # PASS, 5748 checks（0.14 s）
ctest --test-dir .build-bento-wsl -R bento     # 48 项：47 过 + 1 无关 SKIP(netem)
.build-bento-wsl/audit_file_browser_stress     # 10,856 checks，0 failures（1 GiB）
.build-bento-wsl/audit_file_browser_stress --quick   # 10,856 checks，0 failures
```

- ASan+UBSan（`detect_leaks=1`）：单测 5748 checks、审计 quick 10,856 checks，
  均无报告。
- TSAN：WSL 默认 ASLR 下报 `unexpected memory mapping`；`setarch $(uname -m) -R`
  重跑单测 5748 checks，无 data race 报告。
- 变异测试与前后对照数据见 §2、§4。
