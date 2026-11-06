# 抖音 Demo 进度看板

| 项 | 内容 |
|---|---|
| 盘点时间 | 2026-11-06（第 1 轮） |
| 盘点范围 | `app/douyin/` 全部源码（7 服务 + 网关 + 单页前端） |
| 盘点方式 | 读 REQUIREMENTS.md、逐服务路由表、前端函数/元素清单、关键词扫描、跑既有验收 |
| 基线 | `scripts/accept_all.sh` → 70 PASS / 0 FAIL |

> 说明：本文档只记录**代码里核实过**的能力。摘要里提到但代码未命中的项，
> 统一标「待核实」，不当作已完成。

---

## 一、已完成（代码核实 + 验收覆盖）

### 1. 基础链路

- [x] 6 业务服务 + 网关，端口 28080 / 29101–29106
- [x] SQLite WAL 持久化，自动建表 + 旧库 ALTER 迁移
- [x] Bearer token 鉴权（HMAC），网关统一校验 + 各服务二次校验
- [x] 统一错误体 `{ok:false,error,code}`，400/401/403/404/409/500/502/504
- [x] `/health`、`/health/live`、`/health/ready`、`/metrics`
- [x] 幂等中间件（`include/idempotency.hpp`，`Idempotency-Key` + TTL）

### 2. 播放体验

- [x] 全屏 Feed、单例播放（`playOnly`）、自动暂停其它视频
- [x] 点击暂停/播放、进度条拖动（`seekToRatio`）、时长显示
- [x] 双击点赞 + 心形动画（`doubleTapLike` / `spawnHeart`）
- [x] 音量：全局静音开关 + 音量滑块（`applySound` / `setMuted`）
- [x] 视频地址容错（`videoSrc`：绝对/相对/文件名/回退 demoN.mp4）
- [x] HTTP Range 支持（`Content-Range` 出现在 2 个源文件，视频可拖动）
- [x] 骨架屏（`showSkeleton`）、图片预览（`posterSrc`）

### 3. 内容互动

- [x] 点赞/取消赞（幂等，`POST/DELETE /api/like`）
- [x] 评论：列表 + 发评论 + 二级回复（`parent_id`，折叠展开）
- [x] **评论点赞（第 1 轮新增）**：`POST/DELETE /api/comment/:id/like`，
      列表回填 `like_count` + `liked`，前端评论/回复均可点赞
- [x] 收藏：`/api/video/favorites` + 收藏页（`showFavorites` / `paintFav`）
- [x] 关注/取关 + 关注数/粉丝数（`toggleFollow` / `FollowCounts`）
- [x] 个人主页（`showProfile` / `renderProfile`：作品、统计、关注按钮）
- [x] 视频链接复制、分享面板、二维码（`copyVideoLink` / `openShare` / `renderQr`）
- [x] 评论数本地同步（`bumpCommentCount`）、点赞数本地同步

### 4. 发现与话题

- [x] 搜索三分类：用户 / 视频 / 话题（`searchTabs` + 三套行渲染）
- [x] 搜索空态、分页、标签页（`openTag` / `renderTagPage`）
- [x] 标题 `#话题` 解析（`topicHash` / `tagLink`）+ `/api/tag/list`
- [x] 推荐流随机洗牌 + 可续页游标（`EncodeFeedCursor`）
- [x] 关注流 `/api/feed/following`

### 5. 发布与上传

- [x] 分片上传 + 断点续传：`/api/video/upload/init|chunk|finish` + `upload_sessions` 表
- [x] 上传进度 UI（`buildProgress` / `insertUploaded`）
- [x] 网关 32 MiB 上传体上限前置拦截

### 6. 直播与其它

- [x] 直播间创建/开播/关播/进出房（viewer_count 幂等）
- [x] 数据种子脚本（seed_demo / seed_comments / seed_cloud 等）
- [x] 压测工具 `loadgen`、 soak / 可靠性 / 上传超时脚本

---

## 二、待做（按「用户能感知」排序）

| # | 功能 | 用户价值 | 预估 | 依赖/备注 |
|---|---|---|---|---|
| 1 | ~~评论点赞~~ | 高 | — | **本轮已完成** |
| 2 | 消息通知中心 | 高 | 2–3 h | 新表 notifications + 未读徽章 + 通知面板；点赞/评论/关注触发 |
| 3 | 举报 / 拉黑 | 高 | 1.5 h | 举报表 + 拉黑过滤 Feed/评论；入口放操作栏「…」 |
| 4 | 分享落地页 `/v/:id` | 中高 | 1 h | 分享出去的链接直接定位到该视频（当前只有首页 hash 路由） |
| 5 | 个人主页三 tab（作品/喜欢/收藏） | 中高 | 1.5 h | 需要 `/api/video/liked`；主页目前只有作品 |
| 6 | 评论 @mention | 中 | 2 h | 输入 @ 弹候选 + 评论渲染高亮 + 通知联动 |
| 7 | 草稿箱 | 中 | 1 h | 前端已有 localStorage 使用；需明确草稿持久化与恢复入口 |
| 8 | 播放器手势增强（上下滑/长按倍速） | 中 | 1 h | 现有双击点赞/拖动，缺上下切换与长按 2x |
| 9 | 评论排序（最热/最新） | 中低 | 40 min | `ORDER BY like_count` 分支 + 前端切换 |
| 10 | 消息未读红点 | 低 | 30 min | 依赖 #2 |

### 待核实（摘要声称已完成，但代码关键词未命中）

| 项 | 扫描结果 | 结论 |
|---|---|---|
| @mention | `mention` 在 src/include/前端均 0 命中 | 未实现（或用了完全不同的命名） |
| 草稿 | `draft/草稿` 0 命中；`localStorage` 前端 17 处 | 草稿箱未实现；localStorage 用于登录态/音量等 |
| WS Resume | `WebSocket / Sec-WebSocket / resume` 均 0 命中 | 未实现（当前是纯 HTTP + fetch） |
| 压缩 | `gzip / deflate / compress / zstd` 均 0 命中 | 未实现（`douyin_http.hpp` 无压缩） |

> 以上四项若要保留在「已完成」里，需要给出具体文件与验收用例；否则按未做处理。

---

## 三、本轮（第 1 轮）交付：评论点赞

### 改动

| 文件 | 改动 |
|---|---|
| `include/douyin_store.hpp` | `StoredComment` 增 `like_count`/`liked`；新增 `LikeComment`/`UnlikeComment`/`FillCommentLikes` |
| `src/douyin_store.cpp` | 建 `comment_likes` 表（主键即幂等键）；批量回填固定 2 条 SQL（GROUP BY + 已赞集合） |
| `include/douyin_proto.hpp`、`src/douyin_proto.cpp` | `Comment` 增 `like_count`/`liked` 并序列化 |
| `src/interaction_service.cpp` | `POST/DELETE /api/comment/:id/like`；评论列表批量回填；`ViewerId` 可选鉴权 |
| `include/douyin_home_page.hpp` | 评论/回复加 `♥ N` 按钮、`.cp-like` 样式、`toggleCommentLike` |
| `scripts/accept_comment_like.sh` | 新增 33 项验收（幂等/视角/鉴权/404/回复） |

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror` 零告警
- `bash scripts/accept_comment_like.sh` → **33 PASS / 0 FAIL**
- `bash scripts/accept_all.sh` → **70 PASS / 0 FAIL**（回归）
- `GET /` → 200，104695 字节，页面含 `cp-like` / `commentLikeButton` / `toggleCommentLike`
- 接口语义：重复点赞计数不变；匿名 `liked=false`、本人 `liked=true`；无 token 401；不存在评论 404；`GET /like` 405

### 踩坑记录

- 症状：点赞接口返回正确计数，但评论列表里 `like_count` 恒 0。
- 原因：`PublicComment()`（StoredComment → Comment）漏拷贝 `like_count`/`liked`，
  存储层已算对，序列化时又被打回默认值。
- 教训：新增结构字段时，转换函数与序列化函数要同步检查。

---

## 四、下一轮建议

1. 消息通知中心（#2）：与评论点赞联动——被点赞时写入 `notifications`，
   顶部加未读徽章；可复用本轮 `ViewerId` 与批量查询模式。
2. 顺手把「待核实」四项逐一确认：要么补实现，要么从已完成清单里移除。
3. 前端 JS 无本地语法检查工具（无 node/qjs）；下一轮可考虑用
   Python 做一次轻量括号/引号检查，或接入 `quickjs`。
