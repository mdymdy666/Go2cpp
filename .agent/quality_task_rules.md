# Go2Cpp 文档 / 测试 / 示例 质量任务执行规则

> 本文件是多轮「完善与修正文档、测试、示例」任务的**唯一执行规则**。
> 与项目既有 `codex.md`（协作记录）分工：本文件定规则，`codex.md` 记历史。
> 与 `docs/quality_backlog.md`（问题台账）分工：本文件不做问题登记。

---

## 1. 唯一事实来源（Single Source of Truth）

- **项目真身**：WSL 内 `/UserData/CodexWorkSpace/Go2Cpp`（ext4，`stat -c %d` = 2096）。
- **Windows 侧可用通道**：`\\wsl.localhost\Ubuntu\UserData\CodexWorkSpace\Go2Cpp\`
  （`\\wsl$\Ubuntu\...` 等价），实测与 WSL 侧内容一致。
- **禁止**在任何 `/mnt/e/...` 路径读写、构建、提交。该挂载下至少存在两份内容不同的副本：
  - `E:\UserData\CodexWorkSpace\Go2Cpp`（10-02 旧快照）
  - `E:\CodexWorkspace\Go2Cpp`（另一份，含 `coost/`、`NewGo2Cpp/` 等）
- **开工第一件事**：`pwd` 确认在 `/UserData/...`；`git rev-parse HEAD` 记录基线。

### 1.1 已知工具陷阱（必须规避，否则证据全不可信）

| 陷阱 | 现象 | 规避方式 |
| --- | --- | --- |
| **T1 内置 `read`/`grep` 工具路径错解析** | `/UserData/CodexWorkSpace/Go2Cpp/...` 被解析到 `E:\UserData\CodexWorkSpace\Go2Cpp`（旧副本），返回**陈旧内容**。实例：`runtime.hpp` 真身 36 行 vs 副本 26 行；`codex.md` 真身 1116 行 vs 副本 861 行；`cmake/Go2CppModules.cmake` 真身 177 行（含 `go2cpp_network`）vs 副本 165 行（不含） | **读取项目文件一律走** `wsl bash -lc "cd /UserData/... && cat -n / sed -n / grep -n"` **或 UNC 路径** |
| **T2 PowerShell 吃掉 `$`** | 经 `pwsh` 下发 `wsl bash -lc "..."` 时，双引号内 `$f`、`$?` 被 PowerShell 插值成空，导致 `ambiguous redirect`、退出码丢失、循环失效 | 命令含 shell 变量或 `$?` 时，改用 Python 临时脚本 + `subprocess.run(["wsl","bash","-lc",...])` |
| **T3 工具返回的"成功"不可当证据** | `read` 会成功返回**错误内容**而非报错 | 关键结论必须至少用两种通道（WSL 直读 + `git show`）交叉验证 |

---

## 2. 每轮标准工作流

1. **基线**：`git branch --show-current`、`git log --oneline -10`、`git status --short`、
   `git diff --stat`、`git diff | sha256sum`、`git status --porcelain | sha256sum`。
   指纹留档，轮末逐字节比对。
2. **取数据**：按 §1.1 的可靠通道读取，所有结论附 `路径:行号`。
3. **改动**：只动本批次声明的文件；每处改动可追溯到台账 ID。
4. **验证**：构建 + `ctest`；涉及超时/并发的用例须重复运行（≥5 次）以暴露间歇问题。
5. **记录**：向 `codex.md` 顶部区域追加一条 `## YYYY-MM-DD：标题`（详见 §5）。
6. **收尾**：清理本轮新增的构建目录、临时脚本、`log/` 新增日志行；
   确认 `git diff` 在途改动零丢失。

---

## 3. 与用户既有改动的边界（硬约束）

工作区存在**用户在途改动**，共 3 个文件（注意：任务书有时只提到 2 个，实际为 3 个）：

| 文件 | 变更 |
| --- | --- |
| `go2cpp.ini` | `:50` `stdout = false` → `true` |
| `include/go2cpp/logging/log.hpp` | `:226-228` 新增 3 个空行 |
| `include/go2cpp/tools/error.hpp` | `:204-246` 新增 Go 风格 `namespace errors` 薄封装 |

**保留规则**：
- 这 3 处**原样保留**，不得回退、格式化或"顺手整理"。
- 每轮开工与收尾都对 `git diff -- go2cpp.ini include/go2cpp/logging/log.hpp
  include/go2cpp/tools/error.hpp` 取 md5 指纹并逐字节比对，确保零丢失。
- 若某批次确实需要改这 3 个文件之一，先在 `codex.md` 写明理由，并**单独提交**。
- `codex.md` 只**追加**新段落，不改写历史条目。

### 3.1 git 操作策略（持续推进模式，与只读轮不同）

**允许**：
- `git add <逐个具体文件路径>`：只暂存本批次声明的文件。
- `git commit -m "<type>: <中文简述>"`：Conventional Commits 风格。
- `git diff` / `git diff --cached` / `git status` / `git log` / `git show`：只读检查。

**严格禁止**：
- `git add -A` / `git add .` / `git commit -a`——会把用户 3 处在途改动裹进提交。
- `git push` / force push——无人值守下不推送远端。
- `git checkout` / `git stash` / `git reset` / `git clean`。

**唯一例外**：
- `git restore --staged <文件>`：撤销误暂存。
- `git restore <本轮新改且尚未提交的文件>`：回滚本轮失败改动。

**提交门禁**：每次提交前必须满足 `ctest` 全绿、零新增编译警告、
`git diff --cached --stat` 不含上述 3 个在途文件。

---

## 4. 每批次完成定义（Definition of Done）

一批次只有同时满足以下 5 条才算完成：

1. **范围**：只改本批次声明的文件，改动量与台账条目一一对应。
2. **证据**：每处改动有 `路径:行号` 证据；新增/修改的文档链接经自动扫描无失效。
3. **可运行**：
   - 文档批次：所有相对链接 target 存在；命令与 target 名经实际执行验证。
   - 测试批次：`ctest` 全绿，且新增用例在 `GO2CPP_TEST_FILTER` 下可单独运行。
   - 示例批次：示例退出码为 0，且不残留自己创建的 Scheduler/线程/fd。
4. **零回归**：`git diff | sha256sum` 中，用户 3 处在途改动与轮初一致。
5. **记录**：`codex.md` 已追加本批次的条目，且不含任何凭据。

---

## 5. 编码与文档规范

- 遵循项目既有风格：中文 Doxygen 注释、`-Wall -Wextra -Wpedantic` 零警告。
  **注意：项目当前没有 `-Werror`，也没有开启它的开关**；新增代码必须自行保证零警告。
- 测试：沿用自研框架（`tests/test_support.hpp`）。断言宏选择：
  - `GO2CPP_CHECK`：纯断言，失败计数，**不中止**；
  - `GO2CPP_REQUIRE`：失败即 `watchdog_abort`；
  - `GO2CPP_REQUIRE_EVENTUALLY`：带超时的轮询等待（并发场景首选）；
  - `GO2CPP_JOIN_WITH_WATCHDOG`：带看门狗的线程 join。
  **涉及等待/并发的用例必须使用 `REQUIRE_EVENTUALLY` 或 `JOIN_WITH_WATCHDOG`**，
  避免挂起式失败（挂起会拖到 CTest 的 30s TIMEOUT 才报错）。
- 示例：必须同时包含**成功路径 + 等待/取消或生命周期路径 + 可观察的错误或边界路径**
  （项目自定标准，见 `example/README.md:30-34`）；创建的 Scheduler 必须在返回前
  `shutdown()`；默认调度器须 `shutdown_default_scheduler()`。
- 文档：相对链接必须能被自动扫描验证；构建命令与 target 名必须实际存在；
  新增文件要在 `README.md` / `docs/modules.md` / `example/README.md` 相应索引中登记。

---

## 6. 安全与凭据

- root 密码仅在确有必要时使用（如 `build/` 属主为 root 导致写入失败），
  传递限于进程内管道：`printf '%s\n' "$PW" | sudo -S <cmd>`。
- **严禁**把密码写入任何文件、脚本、日志、`codex.md`、git 提交信息或持久化命令历史。
- **严禁**用 root 修改项目文件的属主、权限或内容。

---

## 7. 不可触碰项

- 禁止删除或"清理" `build/`、`log/`、`third_party/` 下任何内容。
  - 例外：`log/` 下**由本轮验证产生的**新增日志行，必须在轮末移除，
    并校验文件字节数与 md5 回到轮初基线。
- 禁止修改、删除、移动既有源码/文档/测试/示例文件——除非该文件属于本批次声明范围。
- 禁止捏造模块、文件、函数、测试或结论。不确定的一律写入台账并标注「未确认」。
- 禁止用文件名或目录名推测内容；**必须打开文件核对**。

---

## 8. 批次划分原则

- 每批是一个**可独立闭环**的模块/主题单元：改动范围可独立验证，不与其他批次产生依赖。
- 依赖关系必须显式声明；存在依赖时，被依赖批次先执行。
- 批次清单与执行顺序见 `docs/quality_backlog.md` 的「批次划分」一节，
  以及 `codex.md` 中第 0 轮记录。


---

## 9. 定时任务（持续推进）模式

本规则服务于**定时任务周期性投递**的单轮执行：

- **节奏**：每天 8:00–24:00 每 2 小时一次（含 0:00），单轮预算 60 分钟，
  轮次间隔余量约 60 分钟。
- **粒度**：每轮只做一个批次、≤5 个台账条目；宁可少做，不可超时留半成品。
- **互斥**：`.agent/guard.sh acquire` / `release`；陈旧锁阈值 90 分钟
  （大于 60 分钟预算，保留容差）。上一轮未结束时本轮只输出 `[skip]` 即退出。
- **状态**：`.agent/state.json` 记录 `round`、`current_batch`、`current_items`、
  `last_result`、`consecutive_failures`、`blocked_items`。
- **事实来源**：台账 `docs/quality_backlog.md` 是唯一任务来源，**不依赖对话记忆**。
- **无人值守**：不提问、不等待；待决策项有「默认建议」则按默认执行并在
  `codex.md` 留痕，无默认建议则标 `阻塞` 跳过。
- **熔断**：同一台账条目连续 3 轮未完成 → 标 `阻塞`，后续轮次不再尝试。
- **收尾**：每轮结束必须同步**台账 + `state.json` + `codex.md`** 三处，并释放锁。
