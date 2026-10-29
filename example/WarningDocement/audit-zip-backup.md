# zip / backup 模块审查报告（压测 / 随机 / 场景 / 解耦 / 增强 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/app/zip.hpp`、`src/bento/app/zip.cpp`、
  `include/go2cpp/bento/app/backup.hpp`、`src/bento/app/backup.cpp`、
  `tests/bento/test_zip.cpp`、`tests/bento/test_backup.cpp`、`tests/bento/test_upload.cpp`
- 审计程序（新增，不注册 CTest）：
  - `tests/bento/audit_zip_stress.cpp` —— 1 GiB 树 / 1 GiB 单文件压测 + zip 炸弹、
    Zip Slip、10 万小文件 3 个场景；
  - `tests/bento/audit_backup_upload_stress.cpp` —— 1/10/100/500/1000 并发上传 +
    重试、超时、断连、verify 临时文件 4 个场景；
  - `tests/bento/audit_zip_random.cpp` —— 1000 随机树、1000 随机归档、2 万次结构变异、
    500 随机炸弹、2000 非法名字；
  - `tests/bento/audit_zip_support.hpp` —— 资源观测（RSS/fd/进程数/磁盘）、
    可复现随机数、多线程 HTTP 测试服务器、zip 字节篡改辅助。
- 环境：WSL2 Ubuntu 24.04，内核 6.6.87.2，12 vCPU，19 GiB RAM，`/tmp` 位于
  `/dev/sdd` ext4（921 GiB 可用），`ulimit -n = 10240`，curl 8.5.0，unzip 6.00；
  构建目录 `.build-bento-wsl`（Ninja / GCC 13.3.0 / Release）。
- 结论：**无 P0**。修复 **3 个 P1**（ZIP32 条目上限自相矛盾、Backup 顶层目录/文件
  同名可生成不可解包的归档、curl 请求级超时被误分类）与 **2 个 P2 性能问题**
  （`fs::relative` 的 readlink 风暴、每文件重复 stat/tellp），小文件打包吞吐提升
  **1.7–2.2×**、目录遍历提升 **约 3×**。`bento_test_zip` 322 checks、`bento_test_backup`
  117 checks、`bento_test_upload` 169 checks 全过（用例数 24 / 14 / 19）；三个审计程序
  共 79 + 38 + 57 439 checks 全过。`ctest -R bento` **48 项 47 过 + 1 环境 SKIP**
  （`bento_netem` 需要 `CAP_NET_ADMIN`）；ASAN+UBSAN 版随机测试 57 439 checks 全过。
  解耦检查通过（只依赖标准库 + hash + os）。

## 1. 压测数据

### 1.1 约 1 GiB / 6 万文件树（打包 → 解包 → 全量逐字节比对）

| 阶段 | 优化前 | 优化后 | 峰值 RSS | 峰值 fd |
|---|---:|---:|---:|---:|
| 打包（60 000 文件 / 1.03 GiB） | 5.11 s / **205 MiB/s** | 3.14 s / **334 MiB/s** | 19.2 MiB | 8 |
| 解包 | 6.01 s / **175 MiB/s** | 4.87 s / **215 MiB/s** | 20.9 MiB | 7 |
| 全量比对（读 2×1 GiB） | 3.20 s | 3.11 s | — | — |
| 磁盘 | 源 1.03 GiB / 包 1.03 GiB / 解包 1.03 GiB | 同左 | — | — |

条目 60 000 + 目录 31，归档大小与 `stats.archive_bytes` 一致；`CompareTrees` 全量
逐字节一致。RSS 仅 ~20 MiB 说明读取/写入都是 64 KiB 流式，不随归档大小增长。

### 1.2 1 GiB 单文件（流式验证）

| 项目 | 3 次运行区间 |
|---|---:|
| 打包 | 1.38–1.84 s，**555–744 MiB/s** |
| 解包 | 1.72–1.80 s，**568–604 MiB/s** |
| 峰值 RSS | 20.9–22.3 MiB（远小于 1 GiB，流式成立） |
| SHA-256 一致性 | 全部一致 |

单文件吞吐波动较大（WSL 磁盘缓存/调度），取区间报告；解包侧未做代码改动，
差异属噪声。

### 1.3 小文件（元数据支配场景）

| 场景 | 优化前 | 优化后 | 提升 |
|---|---:|---:|---:|
| 20 000 个 16 B 文件打包（直接 ZipWriter） | 0.562–0.606 s（33–36 k files/s） | 0.271–0.335 s（**60–74 k files/s**） | **2.0–2.2×** |
| 60 000 文件经 Backup 打包（7.64 MiB） | 1.94 s | 1.04–1.15 s | 1.7–1.9× |
| 10 万文件目录遍历（失败前） | 1.46 s（68 k files/s） | 0.47–0.52 s（**192–214 k files/s**） | **≈3×** |

## 2. 随机测试（`audit_zip_random`，57 439 checks 全过）

| 项目 | 规模 | 结果 |
|---|---|---|
| 随机文件树 | 1000 轮 / 18 741 文件 / 140.25 MiB，深目录 ≤4 层、中文名、空格名、空文件、≤64 KiB 文件、空目录 | 打包→解包→逐字节比对 **全部一致** |
| 随机合法归档 | 1000 轮，随机条目数 0–20、`AddBytes`/`AddFile` 混合、大小 0–8 KiB | List 与 Extract 逐条目名字/大小/CRC/内容一致 |
| 随机结构变异 | **20 000 次**：字节翻转、截断、插入、EOCD 字段、中央目录字段、本地头字段 | 0 崩溃；Extract 成功 146 次、失败残留校验 1071 次；**失败时留在输出目录的文件全部是完整条目**（大小+CRC 与中央目录一致）；canary 文件从未被改写 |
| 随机声明炸弹 | 500 个：1–16 条目、声明 0–2 GiB、零数据 | 全部拒绝；RSS 增长 < 64 MiB（不按声明分配） |
| 随机非法名字 | 2000 次（`../`、绝对路径、反斜杠、`//`、`./`、`..`、冒号、CR、DEL、`/`） | **2000/2000 拒绝** |

变异状态分布（List / Extract）：`corrupt_archive` 15 832 / 16 903、`unsupported_compression`
1 995 / 1 995、`path_rejected` 530 / 530、`archive_too_large` 426 / 426、`ok` 1 217 / 146。
**Extract ok 必然 List ok**（两条解析路径一致），无一条不一致记录。

## 3. 复杂场景（≥5，覆盖 zip/backup/upload 全部功能点）

| # | 场景 | 输入 | 期望 | 实际 |
|---|---|---|---|---|
| 1 | zip 炸弹（声明式） | 伪造 10×1 GiB 声明但零数据；真实 3×800 KiB 数据 + 总量上限 2 MiB；真实 200 KiB + 单条上限 128 KiB；单条声明 2 GiB | 全部拒绝，不按声明分配 | `corrupt_archive`（数据越界）、`archive_too_large`、`entry_too_large`、`entry_too_large`；RSS 5336→5336 KiB |
| 2 | Zip Slip | 等长改名为 `../x.t` / `/tmp/x` / `a\b.tx`；输出目录预置 `out/link -> outside` 后写 `link/pwn.txt`；绝对路径 `/abs.x` | List/Extract 都 `path_rejected`，逃逸目录不落文件 | 5 项全部 `path_rejected`，`outside/pwn.txt` 不存在 |
| 3 | 10 万小文件 | 100 000 个 16 B 文件 | 默认 16 384 与硬上限 65 534 下都明确失败、无 `.part` 残留 | `too_large`（0.49 s / 0.48 s）；无残留；6 万文件可完整打包 |
| 4 | 100 并发上传 | 100 × 16 KiB zip → 本地 201 服务器 | 全部成功且请求数=100 | 100/100，0.20–0.26 s，496 req/s |
| 5 | 503×2→201 | 前两次 503、第三次 201 | 重试成功、本地包逐字节保留 | `attempts=3`、`http=201`、包字节不变、无 `.part.` 残留 |
| 6 | 上传超时 | 服务器每次延迟 2.5 s，`request_timeout=1s`，重试 1 次 | `kTimeout`、attempts=2、本地包保留 | 修复后 `timeout`，attempts=2，2.07 s |
| 7 | 连接断开 | accept 后立即 close，重试 2 次 | 失败分类为网络错误、attempts=3、本地包保留 | `network_error`，attempts=3（curl 55 broken pipe / 56 reset） |
| 8 | 远端校验清理 | `verify_remote=true` GET 成功 | `/tmp/bento_verify*` 前后一致 | verified=1，临时文件 0→0 |

## 4. 解耦检查

| 检查 | 结果 |
|---|---|
| `zip.cpp` 依赖 | `zip.hpp` + `hash/crc32.hpp` + 标准库（无 os/net/link/peer_hub） |
| `backup.cpp` 依赖 | `backup.hpp` + `detail/retry.hpp` + `hash/sha256.hpp` + `os/proc.hpp` + `<unistd.h>` + 标准库 |
| 头文件自包含 | `#include "go2cpp/bento/app/zip.hpp"` 与 `backup.hpp` 各自单独编译通过（`-Wall -Wextra -Wpedantic -Werror`） |
| 头文件传递依赖 | `zip.hpp` 只有 5 个标准库头；`backup.hpp` 多一个 `zip.hpp`，无其它项目头 |
| 反向依赖 | 无 `net/`、`link.hpp`、`peer_hub` 引用（grep 全空） |

结论：**通过**。backup 的 os 依赖仅用于 `os::proc::Run`（启动 curl），与「只依赖
标准库 + hash + os」的边界一致。

## 5. 实现增强：瓶颈、修复与记录

### 5.1 已修（P2 性能）

1. **`fs::relative` 的 readlink 风暴**：`AddDirectory` 对每个条目调用
   `fs::relative`，其内部 `weakly_canonical` 会对路径每一段调用 `readlink`。
   `strace -c` 实测 2 万个小文件触发 **160 280 次失败的 readlink**（占打包 CPU
   时间 26%）与 12 万次 `newfstatat`。改为先把根目录绝对化、再用纯字符串的
   `lexically_relative` 剥离前缀。
2. **每文件 3 次 `lstat`**：遍历阶段 `symlink_status` 一次，写入阶段又
   `symlink_status` + `file_size` 一次，`AddFile` 内部再 `symlink_status` +
   `file_size` 一次。改为遍历时把 `(relative, type, size)` 缓存进
   `PendingEntry`，写入阶段直接 `AddFileFromPath`，不再 stat。
3. **每文件 3 次 `tellp`（=3 次 lseek）**：数据起点可由「条目偏移 + 30 + 名字长」
   直接算出，改为计算值 + 写入后用一次 `tellp` 校验实际字节数。
4. **每文件多一次 EOF `read`**：`AddFile` 的旧循环 `while (in)` 对已知大小的文件
   会多读一次 0 字节；改为按 size 精确读满。
5. 遍历阶段对同一个条目调用两次 `IsExcluded`（一次收集、一次写入），删除冗余的
   第二次调用。

小文件打包 33–36 k → 60–74 k files/s（2.0–2.2×），10 万文件遍历 68 k → 192–214 k
files/s（≈3×），1 GiB/6 万文件树打包 205 → 334 MiB/s。

### 5.2 记录（未改，属设计权衡或超出本次边界）

1. **Upload 每请求 fork+exec 一次 `curl`**：1000 并发吞吐 115–158 req/s
   （16 KiB/请求，1.8–2.5 MiB/s），500 并发开始超线性退化（进程创建/调度成为
   瓶颈）。换实现需要自己处理 TLS/代理/重定向，与「backup 不重复实现 TLS」的
   既有取舍冲突；若要提升，应在上层做请求合并/批量上传。
2. **`verify_remote` GET 模式把整个 zip 复制到 `/tmp` 再算 SHA-256**：大包时临时
   磁盘占用翻倍，且每个并发校验产生 `body`+`headers` 两个文件（已验证用后即删）。
   默认 `verify_remote=false`，风险可控。
3. **`ZipWriter::Open` 立即 truncate 目标路径**：直接调用者会丢旧包；`Backup` 已用
   `<out>.part.<pid>.<seq>` + rename 规避。头文件已注明「旧包永远可用请自行写临时名」。
4. **分卷 zip 返回 `kUnsupportedCompression`**：语义上更接近「不支持的格式」，
   但错误码集合里没有专门的 `kUnsupportedFeature`，保留现状并记录。
5. **ZIP32 不支持 ZIP64**：条目 >65 534、归档 >4 GiB 无法打包（10 万文件场景
   实测被拒）。头文件已声明该约束；若产品需要 10 万+ 文件，必须引入 ZIP64 或分卷。
6. **`AddFile` 的 TOCTOU**：`file_size` 与读取之间文件变化会导致「写入字节数与声明
   不符」而失败（不会产生损坏归档），返回 `kIoError` 并 poison。审计未做并发改写
   文件的专项测试。

## 6. 问题清单

### P0

无。

### P1（已修）

1. **ZIP32 条目上限自相矛盾（写得出、读不回）**
   - 证据：修复前写 65 535 个条目 → `Close()` 返回 ok、生成 5 613 812 B 归档；同一
     文件 `ListZip`/`ExtractZip` 均返回 `archive_too_large`（"不支持 ZIP64"）。
     `0xFFFF` 是 ZIP64 哨兵，EOCD 计数字段写 0xFFFF 必然被读端按 ZIP64 拒绝。
   - 修复：`kZipMaxEntriesCeiling` 65 535 → **65 534**，头文件说明原因；写端
     `Open` 直接拒绝 `max_entries=65535`。
   - 复验：65 534 条目写→读→解包全部成功；65 535 在 `Open` 处 `invalid_argument`。

2. **Backup 顶层目录/文件同名会生成不可解包的归档**
   - 证据：`Backup({"/x/a", "/y/a"})`（前者是目录、后者是文件）旧逻辑分别以
     `"a/"` 与 `"a"` 入表，打包成功，但归档同时含目录条目 `a/` 与文件条目 `a`；
     解包时目录与文件冲突。
   - 修复：顶层名字统一按 **basename（不带尾斜杠）** 去重，冲突返回
     `kInvalidArgument` 且不产生输出文件。
   - 复验：`TestBackupDirectoryFileCollision` 通过；同时存在 `a/` 与 `a` 的归档
     解包被 `path_rejected` 拒绝（由 Zip Slip/ResolveOutputPath 用例覆盖）。

3. **curl 请求级超时被误分类为 `kNetworkError`**
   - 证据：服务器延迟 2.5 s、`request_timeout=1s` 时，`Upload` 返回
     `network_error`，`error` 是 `curl: (28) Operation timed out`。`os::proc` 只在
     进程超时（`request_timeout+5s`）时置 `timed_out`，而 curl 的 `--max-time`
     会以退出码 28 正常退出，因此文档承诺的 `kTimeout` 永远不会出现。
   - 修复：`PerformCurl` 显式识别 `exit_code == 28`（`CURLE_OPERATION_TIMEDOUT`）
     并置 `timed_out`。
   - 复验：同一场景返回 `timeout`，新增 `TestUploadTimeoutClassified`。

### P2（记录 / 已修见 §5）

1. 小文件打包吞吐受元数据路径支配（已修，2.0–2.2×）。
2. Upload 进程模型决定了 1000 并发只能到百级 req/s（记录）。
3. `verify_remote` 的 /tmp 全量副本（记录）。
4. `ZipWriter::Open` 直接截断目标（记录，Backup 已规避）。
5. 分卷 zip 的错误码语义（记录）。
6. ZIP32 无 ZIP64，10 万文件不可打包（记录，产品约束）。

## 7. 测试复查

### 7.1 断言有效性

- 断言数：`test_zip` **291 → 322**（用例 20 → 24）、`test_backup` **111 → 117**
  （13 → 14）、`test_upload` **161 → 169**（18 → 19）。
  （任务描述中的「zip 18 / backup 10 / upload 10」与实际用例数不符：修复前实际为
  20 / 13 / 18；本报告按实测计数。）
- 抽查未发现「空转断言」；唯一已知的弱断言是 `test_zip::TestReadLimits` 用 0 字节
  条目验证 `max_entry_bytes=1`，源码注释已说明由 `TestReadLimitsNonZero` 覆盖非零
  场景。
- 新增断言全部是行为断言（状态码 + 残留文件 + 字节一致性），没有「恒真」条件。

### 7.2 覆盖缺口（本次补齐 6 个）

| 缺口 | 新增用例 |
|---|---|
| 65 535 条目边界（写得出读不回） | `test_zip::TestEntryCeiling` |
| 输出目录预置符号链接的 Zip Slip（`ResolveOutputPath` 路径，旧测试只覆盖解析阶段） | `test_zip::TestExtractSymlinkEscape` |
| FIFO 等特殊文件跳过 | `test_zip::TestSpecialFileSkipped` |
| 条目数据区被破坏后的拒绝与无残留 | `test_zip::TestTruncatedEntryData` |
| Backup 顶层目录/文件同名 | `test_backup::TestBackupDirectoryFileCollision` |
| curl `--max-time` 超时分类 | `test_upload::TestUploadTimeoutClassified` |

仍未覆盖（已由审计程序覆盖，未进单测）：Upload 连接断开/网络错误、ZipWriter 的
TOCTOU（打包过程中源文件变化）、同一目标目录并发 Extract。

### 7.3 SKIP 逃逸

- `test_zip` 3 处 `Skip`（unzip 缺失、symlink 不支持、mkfifo 不支持），
  `test_backup` 1 处（symlink），`test_upload` 0 处。
- `bento_test::Finish` 在 `g_skips != 0` 时返回 **77**，CTest 配置
  `SKIP_RETURN_CODE 77` → 记为 **Skipped 而非 Passed**，不会静默通过。
- 风险：CI 若只检查「退出码 0」会漏掉被 Skip 的测试；建议 CI 统计 `ctest` 输出中的
  Skipped 数并在其为 0 时才判定通过。当前环境 unzip/symlink/mkfifo 全部可用，
  实测无 SKIP。

## 8. 修复与验证

### 8.1 改动文件

| 文件 | 改动 |
|---|---|
| `include/go2cpp/bento/app/zip.hpp` | `kZipMaxEntriesCeiling` 65 535→65 534；文档说明 ZIP64 哨兵 |
| `src/bento/app/zip.cpp` | `AddEntry` 用计算偏移省 2 次 tellp；`AddFileFromPath`（已知大小、精确读）；`AddDirectory` 绝对化根 + `lexically_relative` + `PendingEntry` 缓存 + 删冗余排除检查 |
| `src/bento/app/backup.cpp` | curl 退出码 28 → `kTimeout`；顶层名字统一 basename 去重 |
| `tests/bento/test_zip.cpp` | +4 用例（条目上限、symlink 逃逸、FIFO、截断数据） |
| `tests/bento/test_backup.cpp` | +1 用例（目录/文件同名） |
| `tests/bento/test_upload.cpp` | +1 用例（超时分类） |
| `tests/bento/audit_zip_support.hpp` | 新增：资源观测 / 随机数 / 多线程 HTTP 服务器 / 字节篡改 |
| `tests/bento/audit_zip_stress.cpp` | 新增：1 GiB 压测 + 3 场景 |
| `tests/bento/audit_backup_upload_stress.cpp` | 新增：并发上传压测 + 5 场景 |
| `tests/bento/audit_zip_random.cpp` | 新增：随机树 / 随机归档 / 2 万变异 / 炸弹 / 非法名字 |

### 8.2 验证命令

```bash
# 单元测试（-Werror 下零警告）
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl
ninja bento_test_zip bento_test_backup bento_test_upload
./bento_test_zip && ./bento_test_backup && ./bento_test_upload
ctest -R bento --output-on-failure

# 审计程序（手动编译，不注册 CTest）
cd /UserData/CodexWorkSpace/Go2cpp
g++ -std=c++17 -O2 -g -Iinclude -Itests/bento -Wall -Wextra -Wpedantic -Werror \
    tests/bento/audit_zip_stress.cpp \
    -L.build-bento-wsl -lgo2cpp_bento_app -lgo2cpp_bento -lgo2cpp_os -lgo2cpp_hash \
    -lgo2cpp_config -lgo2cpp_scheduler -lgo2cpp_log -lgo2cpp_fiber -lpthread \
    -Wl,-rpath,$PWD/.build-bento-wsl -o .build-bento-wsl/audit_zip_stress
./.build-bento-wsl/audit_zip_stress
./.build-bento-wsl/audit_backup_upload_stress
./.build-bento-wsl/audit_zip_random
```

### 8.3 结果

| 项 | 结果 |
|---|---|
| 编译警告 | 0（`-Wall -Wextra -Wpedantic -Werror`） |
| `bento_test_zip` | PASS，322 checks / 24 用例 |
| `bento_test_backup` | PASS，117 checks / 14 用例 |
| `bento_test_upload` | PASS，169 checks / 19 用例 |
| `ctest -R bento` | **100% tests passed（48 项：47 Passed + 1 Skipped）** |
| `audit_zip_stress` | PASS，79 checks |
| `audit_backup_upload_stress` | PASS，38 checks |
| `audit_zip_random` | PASS，57 439 checks |
| ASAN + UBSAN 随机测试 | PASS，57 439 checks，无 ASAN/UBSAN 报错、无 LeakSanitizer 泄漏 |

## 9. 未覆盖与已知风险

1. **ASAN/UBSAN 覆盖范围**：随机测试以 `-fsanitize=address,undefined` 重编
   `zip.cpp`/`backup.cpp` 后运行（1000 随机树 + 1000 随机归档 + 2 万变异 + 500 炸弹
   + 2000 非法名字 = 57 439 checks）全部通过，无越界、无未定义行为、无泄漏。
   1 GiB 压测未跑 ASAN（时间成本过高），大文件路径依赖 `audit_zip_stress` 的
   SHA-256 一致性校验。
2. **Upload 的 HTTPS 路径**：本地只有 HTTP 测试服务器；HTTPS/代理/重定向依赖
   curl 自身，未在本次审计中专项压测。
3. **并发备份同一目标**：`test_backup::TestBackupConcurrentSameTarget` 覆盖 4 线程；
   审计未做 100+ 并发 Backup 压测（上传侧已覆盖 1000 并发）。
4. **文件系统相关假设**：Zip Slip 防护依赖 `weakly_canonical` 与 POSIX 语义，未在
   大小写不敏感/网络文件系统上验证。
5. **10 万文件**：受 ZIP32 限制不可打包（已记录为产品约束）；60000 文件是本次
   实测上限内的最大规模。
6. **格式**：仓库没有 `.clang-format`，未做自动化格式检查（避免用 LLVM 默认风格
   制造无关 diff）；新增代码与原文件风格一致（4 空格、100 列）。

---

### 附：ASAN 运行记录

```
== ASAN 编译完成
asan 退出码=0
[PASS] audit_zip_random (57439 checks)
```

编译命令见 §8.2 的扩展：在随机测试命令上追加
`-fsanitize=address,undefined -fno-omit-frame-pointer`，并把
`src/bento/app/zip.cpp src/bento/app/backup.cpp` 直接编入（不链接
`libgo2cpp_bento_app.a`），以保证被测代码本身被插桩。
