# 明日计划（2026-11-07）

> 用户指定两个焦点：**① 跨平台（只要 Windows）**、**② 完整 UI 测试 + 修 douyin 现存 bug**。
> 定时任务已取消，聚焦人工推进。

---

## 零、先做（否则后面全是假的）

| # | 事项 | 为什么必须先做 |
|---|---|---|
| **0.1** | **修 `run_all.sh stop`（假重启）** | 现在 stop 读 `run_all.pids`，每次启动会覆盖该文件 → **旧进程杀不掉**；导致「改了代码、重启了，用户访问的还是旧二进制」。今天「收藏看不到」就是这个原因。**改法：按端口查 pid（`ss -ltnp`）+ 启动前比对二进制 mtime 与进程启动时间**。 |
| **0.2** | **删除自动连播功能**（用户：「纯没用」） | 今天已把默认改关（`autoplay: false`、`=== '1'`），明天**彻底删除**：删 `onSlideEnded`/`autoplay` 状态/按钮/`AUTOPLAY_KEY`/`loop=false` 相关分支，代码与文档同步清理。 |
| **0.3** | 清理 `app/douyin\r/` 垃圾目录 | 历史遗留 |

---

## 一、跨平台（Windows 优先，只要 Windows）

| # | 事项 | 验收 |
|---|---|---|
| 1.1 | **MSVC 全量编译**（最新 HEAD） | 0 error / 0 warning（`/W4`） |
| 1.2 | **核心测试跑通**（子集：concurrency/network/storage/rpc） | 全 PASS |
| 1.3 | **douyin 最小链路**（登录 → feed → 发布 → 播放） | 端到端跑通 |
| 1.4 | 平台差异点核对 | 路径分隔符、`CRLF`、`fork`/`pthread` 缺失、`epoll` → IOCP/select、`/proc` 依赖 |
| 1.5 | JinShanProject Windows 交付 | REQ-010/011/012 在 Windows 上复验 |

---

## 二、完整 UI 测试 + 修 douyin bug

| # | 事项 | 验收 |
|---|---|---|
| 2.1 | **扩 E2E 到全部页面** | 9 页面 ×（主路径 + 空态 + 错误态）→ **≥30 用例** |
| 2.2 | E2E 接入启动脚本 | `scripts/run_ui_tests.sh`：起服务 → 跑 E2E → 停服务 → 输出报告 |
| 2.3 | **按失败项修 bug** | 每个失败 = 真 bug，修完 E2E 全绿 |
| 2.4 | 补演示数据 | 直播房间 / 私信会话 / 举报记录（点进去不是空的） |
| 2.5 | 封面纯黑重抽 | demo3 等 |

### 已知 bug 清单（明天直接查）

| # | bug | 状态 |
|---|---|---|
| B1 | `run_all.sh` 假重启 | 🔴 待修（0.1） |
| B2 | 自动连播「页面自己往下翻」 | 🔴 待删（0.2） |
| B3 | 直播 0 房间 / 私信 0 会话 / 举报 0 条 | 待补数据 |
| B4 | `app/douyin\r/` 垃圾目录 | 待清理 |
| B5 | 部分视频封面纯黑 | 待重抽 |
| B6 | 点赞态、收藏态在部分页面不一致（今天已修 `/api/user/:id/favorites`，需全链路复验） | 待复验 |

---

## 三、今天的现场（明天接着用）

| 项 | 状态 |
|---|---|
| 服务 | 10 个（gateway 28080 + 29101–29109），`scripts/run_all.sh` |
| 公网 | `http://82.157.147.127:8443/douyin/`（tunnel 已连） |
| 账号 | `user0001@douyin.local/ user0001`、`user0002@douyin.local/ user0002`、`admin@douyin.local/ 123456` |
| E2E | `app/douyin/tests/ui/`（Playwright + Chromium，8 用例） |
| 测试标准 | `C:\Users\m\.dsh\UI_TESTING_STANDARDS.md`（UI E2E 是最高级别验收，不可绕过） |
| 性能工具 | `perf` / `llvm-mca` / `valgrind` / `hyperfine` / FlameGraph 全部可用 |

---

## 四、今天已完成的（背景）

- douyin 功能：12 轮开发，60+ 功能点（社交/内容/播放/消息/直播/搜索/创作/举报/私信）
- 用户视角审查：14/14 项全绿 + 修 8 个用户可见 bug
- E2E：8 用例，修 3 个「curl 查不出」的 bug（点赞渲染 / 搜索面板 / 登录路径）
- 性能：基准改造（bufio/tunnel CV<5%）、Parser 24→12ms、gzip 首页 218KB→62KB
