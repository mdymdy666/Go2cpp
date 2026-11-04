# 多 Agent 并行隔离规范

> 起因：2026-11-04 下午，主控同时起了 8 个 agent 改基座（Parser/Socket/Channel/栈/TLS/tunnel/bufio/HTTP），**没有做任何隔离**，直接后果：
>
> - 同一文件被 3 个 agent 并发写（`http.cpp`、`socket.cpp`、`douyin_home_page.hpp`）；
> - 共用 `.build-bento-wsl` → **旧 exe + 新 so（ABI 不匹配）→ 13 个假 SEGFAULT**；
> - 共用 28080 / 29101–29106（生产端口）→ 端口冲突假失败、验收被迫改用隔离端口；
> - 共用 `douyin.db` / `accept_all.db` → 数据互踩（`alice 409`）。
>
> 本文是强制规范，**任何并行 agent 任务都必须遵守**。

## 一、硬规则（违反即返工）

| # | 规则 | 具体要求 |
|---|---|---|
| 1 | **独立 worktree** | 每个 agent 一个 `git worktree add .agent-work/<name> <base>`，**绝不共用主工作树** |
| 2 | **独立构建目录** | worktree 内 `.build-<name>/`；**exe 与 .so 必须同批重建**（禁止只 ninja 一个 target 就跑测试） |
| 3 | **独立端口段** | agent N 用 `29N00–29N99`（例：agent 3 用 `29300-29399`）；**禁止碰 28080 与 29101–29106**（生产 demo） |
| 4 | **独立 DB** | `data/<agent>.db`；**禁止直接写 `data/douyin.db`**（生产库） |
| 5 | **文件所有权** | 一个文件同一时刻**只允许一个 agent 改**；按模块划分所有权（见下表），不按功能划分 |

## 二、所有权划分（示例）

| 模块/文件 | 归属 |
|---|---|
| `src/network/http.cpp`、`include/go2cpp/network/http.hpp` | HTTP agent |
| `src/network/socket.cpp`、`address.cpp` | Socket agent |
| `src/concurrency/**` | Concurrency agent |
| `app/douyin/**` | Douyin agent |
| `docs/reports/**` | 文档 agent（只写报告，不碰代码） |
| `cmake/**` | 主控独占（避免多 agent 同时改构建） |

若两个 agent 必须改同一文件：**串行**（A 完成 → 提交 → B 基于新 HEAD 开工）。

## 三、给 agent 的 prompt 模板（必须包含）

```
## 隔离环境（强制）
- 工作树：/UserData/CodexWorkSpace/Go2cpp/.agent-work/<your-name>（独立 worktree，禁改主树）
- 构建目录：该 worktree 内 .build-<your-name>/
- 端口：29<你的编号>00–29<你的编号>99（禁碰 28080/29101-29106）
- DB：data/<your-name>.db（禁碰 data/douyin.db）
- 只改你所有权范围内的文件；改完给出 git diff --stat 与本 worktree 的相对路径
```

## 四、验收（主控侧）

1. agent 交付后，主控在**主工作树**跑：`git -C <worktree> diff --stat` 核对改动面；
2. **全量重建**（`cmake --build .build-bento-wsl -j4`）后再跑 ctest——禁止用旧 exe + 新 so 的结论；
3. 每个 agent 的结论必须注明「构建目录 + exe/so 是否同批」；
4. 串行合并：A 提交 → 主控 verify → B 才 rebase。

## 五、事故复盘（2026-11-04）

| 现象 | 真实原因 | 正确做法 |
|---|---|---|
| 13 个 SEGFAULT | 旧 exe + 新 so（ABI 不匹配） | 同批重建 |
| `locks` 一会过一会崩 | 高负载下 60s watchdog + 旧二进制 | 独立构建目录 + 独占 CPU |
| `alice 409` | 共用 `accept_all.db` 遗留数据 | 独立 DB |
| hpp 被改 2692 行 | 3 个 agent 同时改前端 | 文件所有权 |

## 六、执行检查清单（每个并行任务开工前）

- [ ] 每个 agent 有独立 worktree
- [ ] 每个 agent 有独立构建目录
- [ ] 端口段不重叠、不碰生产端口
- [ ] DB 文件独立
- [ ] 文件所有权无冲突（或已串行）
- [ ] 交付后主控全量重建 + 复跑
