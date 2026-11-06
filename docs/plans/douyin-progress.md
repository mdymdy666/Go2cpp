# 抖音 Demo 进度看板

| 项 | 内容 |
|---|---|
| 盘点时间 | 2026-11-06（第 2 轮） |
| 盘点范围 | `app/douyin/` 全部源码（7 服务 + 网关 + 单页前端） |
| 盘点方式 | 读 REQUIREMENTS.md、逐服务路由表、前端函数/元素清单、关键词扫描、跑既有验收 |
| 基线 | `scripts/accept_all.sh` → 70 PASS / 0 FAIL；`scripts/accept_notifications.sh` → 92 PASS / 0 FAIL |

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
- [x] **消息通知中心（第 2 轮新增）**：`notifications` 表 + 顶部 🔔 未读徽章 +
      右侧通知面板；被赞（视频/评论）/被评论/被回复/被关注写通知，自己触发
      自己不写，重复点赞不重复写；列表固定 2 条 SQL、未读数单条索引 COUNT

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
| 2 | ~~消息通知中心~~ | 高 | — | **本轮已完成** |
| 3 | 举报 / 拉黑 | 高 | 1.5 h | 举报表 + 拉黑过滤 Feed/评论；入口放操作栏「…」 |
| 4 | 分享落地页 `/v/:id` | 中高 | 1 h | 分享出去的链接直接定位到该视频（当前只有首页 hash 路由） |
| 5 | 个人主页三 tab（作品/喜欢/收藏） | 中高 | 1.5 h | 需要 `/api/video/liked`；主页目前只有作品 |
| 6 | 评论 @mention | 中 | 2 h | 输入 @ 弹候选 + 评论渲染高亮 + 通知联动 |
| 7 | 草稿箱 | 中 | 1 h | 前端已有 localStorage 使用；需明确草稿持久化与恢复入口 |
| 8 | 播放器手势增强（上下滑/长按倍速） | 中 | 1 h | 现有双击点赞/拖动，缺上下切换与长按 2x |
| 9 | 评论排序（最热/最新） | 中低 | 40 min | `ORDER BY like_count` 分支 + 前端切换 |
| 10 | ~~消息未读红点~~ | 低 | — | **本轮已完成**（并入 🔔 未读徽章） |

### 待核实（摘要声称已完成，但代码关键词未命中）

| 项 | 扫描结果 | 结论 |
|---|---|---|
| @mention | `mention` 在 src/include/前端均 0 命中 | 未实现（或用了完全不同的命名） |
| 草稿 | `draft/草稿` 0 命中；`localStorage` 前端 17 处 | 草稿箱未实现；localStorage 用于登录态/音量等 |
| WS Resume | `WebSocket / Sec-WebSocket / resume` 均 0 命中 | 未实现（当前是纯 HTTP + fetch） |
| 压缩 | `gzip / deflate / compress / zstd` 均 0 命中 | 未实现（`douyin_http.hpp` 无压缩） |

> 以上四项若要保留在「已完成」里，需要给出具体文件与验收用例；否则按未做处理。

---

## 三、第 1 轮交付：评论点赞

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

## 四、第 2 轮交付：消息通知中心

### 存储

| 项 | 内容 |
|---|---|
| 表 | `notifications(id, user_id 接收者, actor_id 触发者, kind, target_type, target_id, content, created_at, read_at)` |
| kind | `like` / `comment` / `follow` / `mention`（存储层白名单校验，脏数据拒绝） |
| target_type | `video` / `comment` / `user` |
| 索引 | `idx_notifications_unread(user_id, read_at, created_at)`；`idx_notifications_timeline(user_id, created_at DESC, id DESC)` |
| 幂等 | 只有 `Like()==1`（本次新点赞）与「从未关注 → 关注」才写；重复点赞/关注不重复写 |
| 昵称 | 不冗余存表，列表查询 `LEFT JOIN users` 一次回填 `actor_name` |

### 写入点（3 类事件、4 个接口联动）

| 触发 | 接口 | 接收者 | kind / target_type |
|---|---|---|---|
| 视频被赞 | `POST /api/like` | 视频作者（≠ 点赞者） | `like` / `video` |
| 评论被赞 | `POST /api/comment/:id/like` | 评论作者（≠ 点赞者） | `like` / `comment` |
| 视频被评论 | `POST /api/comment` | 视频作者 + 父评论作者（去重、均 ≠ 评论者） | `comment` / `video`、`comment` / `comment` |
| 被关注 | `POST /api/user/:id/follow` | 被关注者 | `follow` / `user` |

### 接口（interaction 29104，网关 `/api/notifications/**`）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/notifications?cursor=&limit=20` | 列表（时间倒序 keyset）+ `unread_count` + `next_cursor` |
| POST | `/api/notifications/read` | `{"ids":[...]}` 或 `{"all":true}` → `updated` + 最新 `unread_count` |
| GET | `/api/notifications/unread_count` | 单条 COUNT |

- 鉴权：三个接口都必须 Bearer（网关 `require_auth=true` + 上游二次校验），匿名 401；
- 越权：`UPDATE ... WHERE user_id = ?`，ids 混入别人的通知时 `updated=0`；
- 列表 2 条 SQL（列表 JOIN + 未读 COUNT），不随页大小增长，无 N+1。

### 前端

- 顶部栏 🔔 按钮 + 未读徽章（`notifBtn` / `notifBadge`），登录后显示、30 s 轮询；
- 右侧抽屉 `notifPanel`：按 kind 显示 ❤️ / 💬 / ➕ / @、文案、相对时间、摘要；
- 点击单条 → 标已读（本地先改、失败回退）；视频类跳该视频、关注类进对方主页；
- 「全部已读」+「加载更多」分页；Esc / 遮罩关闭。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror` 零告警
- `bash scripts/accept_notifications.sh` → **92 PASS / 0 FAIL**（10 组功能 + 性能）
- `bash scripts/accept_all.sh` → **70 PASS / 0 FAIL**（回归）
- 性能：`EXPLAIN QUERY PLAN` 命中上述两个索引、列表无 TEMP B-TREE；
  TTFB 平均 0 ms / 最大 1 ms（20 次串行，本机 loopback）
- 前端：`GET /` 200、页面 > 100 KB，含 `notifBtn` / `notifBadge` / `notifPanel` /
  `openNotifications` / `renderNotifications` / `notif-line`

### 踩坑记录

- 症状：批量补丁脚本一次改同一文件多处，最后只有最后一处生效。
- 原因：每处替换都基于「原始文本」重新读盘，未在内存里累积，写盘时互相覆盖。
- 教训：多补丁必须用「内存文本累积 + 全部成功后单次落盘」模型，或逐处读-改-写。

## 五、下一轮建议

1. 举报 / 拉黑（#3）：举报表 + 拉黑过滤 Feed/评论，入口放操作栏「…」。
2. 分享落地页 `/v/:id`（#4）：目前只有 hash 路由与 feed 内 `jumpToVideo`。
3. 「待核实」四项：@mention / 草稿 / WS Resume / 压缩——要么补实现，要么从
   已完成清单里移除；@mention 可直接复用通知中心的 `mention` 类型。
4. 前端 JS 无 node/qjs：本轮用 Python 做「括号/字符串闭合 + 关键片段」检查
   （只覆盖新增通知块），后续可接 quickjs 对整页做 `--check`。
