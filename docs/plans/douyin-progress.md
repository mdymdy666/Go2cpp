# 抖音 Demo 进度看板

| 项 | 内容 |
|---|---|
| 盘点时间 | 2026-11-06（第 12 轮） |
| 盘点范围 | `app/douyin/` 全部源码（7 服务 + 网关 + 单页前端） |
| 盘点方式 | 读 REQUIREMENTS.md、逐服务路由表、前端函数/元素清单、关键词扫描、跑既有验收 |
| 基线 | `scripts/accept_all.sh` → **89 PASS / 0 FAIL**（第 12 轮回归）；`scripts/accept_profile_edit.sh` → **53 PASS / 0 FAIL**；`scripts/accept_delete.sh` → **45 PASS / 0 FAIL**；`scripts/accept_search_cluster.sh` → **71 PASS / 0 FAIL**；`scripts/accept_gzip.sh` → **30 PASS / 0 FAIL**；`scripts/accept_notifications.sh` → 92 PASS / 0 FAIL；`scripts/accept_report_block.sh` → 79 PASS / 0 FAIL；`scripts/accept_share_landing.sh` → 88 PASS / 0 FAIL；`scripts/accept_profile_tabs.sh` → **115 PASS / 0 FAIL**；`scripts/accept_player_cluster.sh` → **94 PASS / 0 FAIL**；`scripts/accept_notif_cluster.sh` → **111 PASS / 0 FAIL**；`scripts/accept_live_cluster.sh` → **128 PASS / 0 FAIL** |

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
- [x] **第 8 轮**：单击 250ms / 双击点赞 / 长按 2× / 上下滑 / 左右滑手势
- [x] **第 8 轮**：进度条悬浮时间预览 + buffered 高亮 + 60ms 节流 seek
- [x] **第 8 轮**：自动连播（默认开，到底刷新）+ 推荐/关注双 tab + 倍速
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
- [x] **消息三件套（第 9 轮新增）**：通知分类 tab（`?kind=like|comment|follow|
      mention|system`；`counts` 一次 GROUP BY 出 6 类未读，前端每 tab 角标）/
      系统通知（`POST /api/notifications/system`，内部 token 或 admin，
      `kind=system` 白名单放开、📢 展示、点击不跳转）/ 单类已读
      （`POST /api/notifications/read {"kind":"like"}`，每 tab「全部已读」
      只清当前分类）

- [x] **举报 / 拉黑（第 3 轮新增）**：`reports` / `blocks` 两表；举报去重 +
      拉黑/取消/状态/黑名单 5 个接口；feed / 视频详情 / 评论 / 搜索 / 通知 /
      收藏 / 话题 7 处读过滤；评论/点赞/关注写接口防骚扰（403）；前端「⋯」菜单、
      举报弹窗、黑名单抽屉
- [x] **分享落地页（第 4 轮新增）**：`/v/:id` 由 video 服务 SSR 直出独立页面
      （播放器 / 作者关注 / 点赞评论数 / 评论区翻页 / 下载分享 + OG 预览）；
      已删除、被拉黑作者一律 404，匿名按公开页处理
- [x] **个人主页三 tab（第 5 轮新增）**：`/api/user/:id/videos|liked|favorites`
      三个分页接口（作品公开 / 喜欢·收藏仅本人可见）；拉黑双向过滤；
      主页 tab 栏 + 滚动加载 + 私密锁 + 分 tab 空状态；
      作品数改用接口 `total`（不再从 feed 拉 100 条过滤）

### 4. 发现与话题

- [x] 搜索三分类：用户 / 视频 / 话题（`searchTabs` + 三套行渲染）
- [x] **搜索增强（第 11 轮新增）**：搜索历史（同词去重 / 单条删除 / 清空 /
      仅本人）、热搜（最近 7 天 `GROUP BY` 聚合）、输入 200ms 防抖建议
      （热搜词 + 用户昵称 + 话题并集去重）
- [x] 搜索空态、分页、标签页（`openTag` / `renderTagPage`）
- [x] 标题 `#话题` 解析（`topicHash` / `tagLink`）+ `/api/tag/list`
- [x] 推荐流随机洗牌 + 可续页游标（`EncodeFeedCursor`）
- [x] 关注流 `/api/feed/following`

### 5. 发布与上传

- [x] 分片上传 + 断点续传：`/api/video/upload/init|chunk|finish` + `upload_sessions` 表
- [x] 上传进度 UI（`buildProgress` / `insertUploaded`）
- [x] **草稿箱（第 7 轮新增）**：localStorage 自动保存（1s 防抖）+ 恢复提示 +
      我的草稿列表（继续编辑 / 删除）+ 发布成功自动清理
- [x] **视频编辑（第 7 轮新增）**：canvas 抽 3 帧选封面 + 裁剪起止；
      `videos` 表 `cover_url`/`clip_start_ms`/`clip_end_ms`；播放端按裁剪窗播放
- [x] **发布流程完善（第 7 轮新增）**：XHR 上传进度 + 失败重试 + 成功跳转 +
      防重复提交；`Idempotency-Key` 服务端回放（`Idempotency-Replayed`）
- [x] 网关 32 MiB 上传体上限前置拦截

### 6. 直播与其它

- [x] 直播间创建/开播/关播/进出房（viewer_count 幂等）
- [x] **直播三件套（第 10 轮新增）**：直播间聊天（1~200 字、2s 增量
      `after` 轮询、乐观上屏、双向拉黑过滤）/ 礼物（5 个价目、事务扣
      余额、聊天区高亮 + 飘屏特效）/ 贡献榜（单条 `GROUP BY` 聚合、
      头像 + 昵称 + 金额、前三奖牌、随聊天轮询刷新）；`GET /live` 直出
      直播页，首页顶栏有入口
- [x] 数据种子脚本（seed_demo / seed_comments / seed_cloud 等）
- [x] 压测工具 `loadgen`、 soak / 可靠性 / 上传超时脚本

---

## 二、待做（按「用户能感知」排序）

| # | 功能 | 用户价值 | 预估 | 依赖/备注 |
|---|---|---|---|---|
| 1 | ~~评论点赞~~ | 高 | — | **本轮已完成** |
| 2 | ~~消息通知中心~~ | 高 | — | **本轮已完成** |
| 3 | ~~举报 / 拉黑~~ | 高 | — | **本轮已完成** |
| 3.1 | 举报列表后台（运营端） | 中 | 1 h | 目前只落库，无管理接口；见 `reports.status` |
| 4 | ~~分享落地页 `/v/:id`~~ | 中高 | — | **本轮已完成**（独立 SSR 页 + OG 预览） |
| 5 | ~~个人主页三 tab（作品/喜欢/收藏）~~ | 中高 | — | **本轮已完成** |
| 6 | ~~评论 @mention~~ | 中 | — | **第 6 轮已完成** |
| 7 | ~~草稿箱~~ | 中 | — | **第 7 轮已完成**（自动保存 / 恢复提示 / 我的草稿） |
| 8 | 播放器手势增强（上下滑/长按倍速） | 中 | 1 h | 现有双击点赞/拖动，缺上下切换与长按 2x |
| 9 | ~~评论排序（最热/最新）~~ | 中低 | — | **第 6 轮已完成** |
| 10 | ~~消息未读红点~~ | 低 | — | **第 5 轮已完成**（并入 🔔 未读徽章） |
| 11 | ~~视频编辑（封面选帧 / 时长裁剪）~~ | 中高 | — | **第 7 轮已完成**（无转码，播放端按裁剪窗播放） |
| 12 | ~~发布流程完善（进度 / 重试 / 跳转 / 幂等）~~ | 中高 | — | **第 7 轮已完成** |

### 待核实（摘要声称已完成，但代码关键词未命中）

| 项 | 扫描结果 | 结论 |
|---|---|---|
| @mention | 第 6 轮已实现（`interaction_service.cpp` 的 mention 解析 + 前端 @ 补全） | 已完成 |
| 草稿 | 第 7 轮已实现（`douyin_home_page.hpp` 的 DRAFT_KEY / saveDraft / resumeDraft） | 已完成 |
| WS Resume | `WebSocket / Sec-WebSocket / resume` 均 0 命中 | 未实现（当前是纯 HTTP + fetch） |
| 压缩 | ~~`gzip / deflate / compress / zstd` 均 0 命中~~ | **第 11 轮已实现**：网关 `DecorateGzip` 用基座 `go2cpp::compress` gzip |

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

## 五、第 3 轮交付：举报 + 拉黑

### 存储（第 3 轮新增 2 张表）

| 表 | 列 | 约束 / 索引 |
|---|---|---|
| `reports` | `id, reporter_id, target_type, target_id, reason, detail, status, created_at` | `UNIQUE(reporter_id, target_type, target_id)` 防重复举报；`idx_reports_status(status, created_at DESC)` 供运营后台 |
| `blocks` | `id, user_id（拉黑者）, blocked_id（被拉黑）, created_at` | `UNIQUE(user_id, blocked_id)` 幂等键；`idx_blocks_user(user_id, created_at DESC)` 让「我的黑名单」与 `NOT IN` 子查询只扫本人几行 |

- `target_type` 只接受 `video` / `comment` / `user`，由存储层白名单 + 服务层校验双重拦截；
- `reason` 1~50 字、`detail` 可空且 <=500 字（UTF-8 码点计数）；
- 重复举报**不新增行**：`INSERT` 命中唯一键后回查已有记录，接口返回 `200 + duplicate:true`。

### 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/report` | 举报；首次 **201**，重复 **200 + duplicate:true**，自己举报自己 400，目标不存在 404，匿名 401 |
| POST | `/api/user/:id/block` | 拉黑；幂等，重复时 `already:true`；不能拉黑自己（400），目标不存在 404 |
| DELETE | `/api/user/:id/block` | 取消拉黑；未拉黑也成功（幂等） |
| GET | `/api/user/:id/block` | 当前登录者是否已拉黑该用户（匿名 401） |
| GET | `/api/user/blocks` | 黑名单列表（JOIN 回填昵称，按拉黑时间倒序） |

- 网关 `/api/user/**` 已存在；新增 `/api/report` 全方法转发（POST 鉴权在上游，GET 405、匿名 401）；
- 写前统一 `ResetStatements()`，避免读游标残留触发 `SQLITE_BUSY_SNAPSHOT`。

### 拉黑过滤（覆盖 6 处读 + 4 处写）

| 场景 | 行为 | 实现 |
|---|---|---|
| Feed 推荐流 / 关注流 | 不返回被拉黑作者的视频 | `ListFeed` / `ListFollowingFeed` SQL `NOT IN (SELECT blocked_id ...)`；匿名无身份不过滤 |
| 视频详情 | 404（不暴露「被屏蔽」） | video 服务双向检查：viewer 拉黑作者 **或** 作者拉黑 viewer |
| 评论列表 / 回复 | 不返回被拉黑用户的评论（含回复） | `ListComments` / `ListReplies` 按 viewer 过滤；视频作者被拉黑时整页 404 |
| 搜索 | 用户、视频（含 facet）都不返回被拉黑者 | `SearchUsers` / `SearchVideos` 增加 `viewer_id` 过滤；匿名不过滤 |
| 通知列表 / 未读数 | 被拉黑者触发的通知不展示、不计未读 | `ListNotifications` / `CountUnreadNotifications` 用接收者自身作 `blocks.user_id` 过滤，两处条件一致 |
| 收藏列表 | 被拉黑作者的收藏视频不展示 | `ListFavorites` SQL 过滤 |
| 话题页 | 被拉黑作者视频不参与聚合 | video 服务按 `ListBlocks` 集合过滤 |
| 写接口（防骚扰） | 被拉黑后不能评论 / 点赞 / 关注，返回 403 | interaction 的评论、点赞；user 的关注，均双向检查 |

> 说明：匿名请求没有身份，无法应用个人黑名单，保持全量（与既有行为一致）。

### 前端

- 视频操作栏新增「⋯」→ 底部菜单：**举报视频 / 拉黑作者**；
- 评论与回复行新增「⋯」→ **举报评论 / 拉黑评论作者**；拉黑后自动关闭并重载评论区；
- 用户主页新增「⋯」→ **举报该用户 / 拉黑该用户**；已拉黑的用户主页显示「已拉黑」+ 取消入口，不请求、不展示作品；
- 举报弹窗：6 个原因单选（广告营销/色情低俗/人身攻击/谣言不实/违法违规/其它）+ 500 字补充说明；
- 顶部栏新增「🚫 黑名单」→ 右侧抽屉列出被拉黑用户（昵称/时间），可逐条取消拉黑；
- 拉黑成功立即从本地 feed / 搜索结果中剔除该作者内容，并异步刷新黑名单；服务端过滤是下一次请求的兜底；
- Esc / 遮罩均可关闭菜单、举报弹窗与黑名单抽屉。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror` 零告警
- `bash scripts/accept_report_block.sh` → **79 PASS / 0 FAIL**（9 组：举报去重/拉黑接口/feed/详情/评论/搜索/通知/防骚扰/前端 + 唯一约束）
- `bash scripts/accept_all.sh` → **70 PASS / 0 FAIL**（回归）
- `bash scripts/accept_notifications.sh` → **92 PASS / 0 FAIL**（回归）
- `bash scripts/accept_comment_like.sh` → **33 PASS / 0 FAIL**（回归）
- 前端静态检查：JS 88 KB，括号/引号闭合，77 个 `getElementById` 全部有对应 id，13 个新函数各定义一次
- 性能（loopback，20 次串行）：`GET /` avg 0.6 ms / max 1.1 ms；`GET /api/feed` avg 0.9 ms / max 1.2 ms；
  `GET /api/notifications` avg 0.9 ms / max 1.2 ms；`GET /api/search` avg 1.0 ms / max 1.5 ms；`GET /` 134 KB
- `GET /` → 200，页面含 `reportModal` / `blockPanel` / `menuSheet` / `openReport` / `toggleBlock` / `blacklistBtn`

### 踩坑记录

- **症状**：拉黑后 interaction 服务的通知列表与评论列表仍返回被拉黑者的数据，而点赞接口又能正确 403。
- **原因**：interaction 的连接被上一请求残留的 ORM 读游标钉在旧 WAL 快照上；写请求提交后才刷新，
  所以「评论」本身（写）先读到旧状态，其后的「点赞」（写）才看到新拉黑。
- **修复**：每个请求入口调用 `ResetStatements()` 清 ORM 语句缓存；同类问题可通过「先写一次再读」自证。
- **教训**：WAL 多进程读的「读不到刚写入的数据」不一定是没提交，先看读连接有没有未结束的语句。
- **方向坑**：`IsBlocked(user_id 拉黑者, blocked_id 被拉黑者)` 的参数顺序在「viewer 拉黑作者」与
  「作者拉黑 viewer」两个场景里相反；视频详情最初写反，导致只挡了一个方向。现在详情/评论/写接口都做双向检查。

## 六、第 4 轮交付：分享落地页 `/v/:id`

### 架构

| 层 | 改动 |
|---|---|
| 网关 `src/gateway.cpp` | 首页 Matcher 不再接管 `/v/<id>`（原来直接回首页 SPA）；新增 `GET/HEAD /v/**` → video 服务的公开读路由（`share-landing`）；对 `/v/` 前缀注入 `X-Forwarded-Host` / `X-Forwarded-Proto` |
| video 服务 `src/video_service.cpp` | 新增 `HandleSharePage`：解析 id + 站点前缀 → 查视频 → 拉黑双向过滤 → 作者/计数/播放/封面 → 首屏评论（20 条 + 游标）→ 输出 HTML；`Handle()` 里按 `/v/` 前缀分流 |
| 模板 `include/douyin_share_page.hpp` | 新文件：`BuildSharePage` / `BuildShareNotFound` / `EscapeHtml` / `TruncateUtf8` / `TimeAgo`；内联 CSS+JS，自包含无外部依赖 |

> 为什么不在网关拼页面：网关没有业务库句柄，SSR 需要视频、作者、评论、点赞、关注五份数据，
> 在网关侧要 3~4 次内部 HTTP + JSON 解析；放到 video 服务后是进程内一次查询，
> 且「已删除 404 / 被拉黑 404」与 `/api/video/:id` 的语义天然一致。

### OG / 预览标签（服务端写死，爬虫无需执行 JS）

| 标签 | 内容 |
|---|---|
| `og:type` | `video.other` |
| `og:site_name` / `og:title` / `og:description` | 站点名 / 标题 / 「标题 · @作者 · ♥ N · 💬 M」 |
| `og:url` | 落地页绝对地址（`http://host/v/<id>`，前缀随部署） |
| `og:image` + `og:image:width/height` | 同名封面 `videos/<name>.jpg`（`std::filesystem` 确认存在，否则 demo 封面兜底），绝对地址 |
| `og:video` + `og:video:type` | 绝对播放地址 + `video/mp4` |
| `twitter:card` / `twitter:title` / `twitter:description` / `twitter:image` | `summary_large_image` + 同上内容 |
| `link rel="canonical"` | 落地页地址 |

### 页面能力（移动端优先单列）

- 播放器 `<video controls playsinline preload=metadata>` + 封面，`max-height:72vh`；
- 标题 + 点赞数 / 评论数 / 发布日期；
- 作者行：首字母头像 + 昵称 + 关注按钮（登录后 `POST|DELETE /api/user/:id/follow`，
  SSR 直出「已关注」态，匿名显示「关注」）；
- 操作栏：点赞（`POST|DELETE /api/like`，乐观更新计数）、下载（`<a download>`）、
  分享（`navigator.share`，不可用时复制链接）；
- 评论区：SSR 首屏 20 条（`max-height:62vh` 可滚动）+「加载更多」
  （`/api/comment/:id?cursor=`）+ 发评论（`POST /api/comment`）+
  单条点赞（`/api/comment/:id/like`）；未登录操作提示「回首页登录」；
- 404 页：独立 HTML + `noindex`，含「回首页看更多」，不加载首页 SPA。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror -Wpedantic` 零告警
- `bash scripts/accept_share_landing.sh` → **88 PASS / 0 FAIL**（9 组）
- `bash scripts/accept_all.sh` → **70 PASS / 0 FAIL**（回归）
- `bash scripts/accept_comment_like.sh` / `accept_notifications.sh` /
  `accept_report_block.sh` → 33 / 92 / 79 PASS（回归）
- 覆盖：页面元素（video/作者/关注/点赞/评论/下载/分享）、6 个 OG + 4 个 Twitter 标签、
  XSS 转义（标题含 `<script>`）、25 条评论 SSR 20 条 + 第二页 5 条、
  不存在 / 非数字 / 0 / 已删除 404、viewer 拉黑作者 404、作者拉黑 viewer 404、
  匿名不受个人黑名单影响、关注/点赞态只对本人渲染、HEAD、尾斜杠、首页与 API 回归
- 性能（loopback，20 次串行）：`GET /v/<id>` TTFB **avg 0.77 ms / max 1.02 ms**，
  页面 16.8 KB

### 踩坑记录

- **症状**：`edit` / `write` 工具对 `\\wsl.localhost\...` 路径报 `ENOTSUP`
  （原子 rename 在 9P 上不支持）。
- **绕过**：文件先写 Windows 侧 `.dsh_tmp/`，再 `wsl bash -lc "cp /mnt/e/... /UserData/..."`；
  WSL 内批量替换用 `python3` 脚本（读-改-写 + 命中次数断言）。
- **路径**：本机存在 `E:\UserData\CodexWorkSpace\Go2cpp` 影子目录（历史误建），
  真树只在 WSL `/UserData/CodexWorkSpace/Go2cpp`；本轮曾误写一次，已搬回并删除误建的那一棵。

- **症状**：落地页在评论头像处出现非法 UTF-8（浏览器乱码；Python 读取直接抛
  `UnicodeDecodeError`，`wc` 之前完全看不出来）。
- **原因**：`std::string::substr(0, 1)` / `who[3]` 是按**字节**取「首字符」，
  「用户 #1」的第 4 个字节正好是「户」的中间字节，页面里留下半个码点。
- **修复**：作者头像用 `FirstUtf8Char` 取完整码点；评论头像固定 `#`
  （与 JS 追加的评论行一致）。
- **教训**：C++ 里凡是「取首字符」的地方都要确认按码点还是按字节；
  验收脚本要加一次 `decode('utf-8')`，否则非法字节在终端里看不出来。

## 七、第 5 轮交付：个人主页三 tab（作品 / 喜欢 / 收藏）

### 接口（user 服务 29101，网关 `/api/user/**`）

| 方法 | 路径 | 隐私策略 | 说明 |
|---|---|---|---|
| GET | `/api/user/:id/videos?cursor=&limit=` | 公开 | 作者作品，按发布时间倒序 keyset 分页 |
| GET | `/api/user/:id/liked?cursor=&limit=` | **仅本人** | 该用户点赞过的视频，按点赞时间倒序 |
| GET | `/api/user/:id/favorites?cursor=&limit=` | **仅本人** | 该用户收藏的视频，按收藏时间倒序 |

- 响应统一：`{ok,tab,private,blocked,owner_id,viewer_id,total,next_cursor,items:[{video,author}]}`；
  `items` 与 feed 同构，前端卡片直接复用；
- 隐私：`liked` / `favorites` 对他人与匿名一律 `200 + private:true + items:[] + total:0 + hint`，
  连 COUNT 都不执行，不泄露「有没有内容」；本人（Bearer 且 id 相同）才返回数据；
- 拉黑：**双向**过滤（viewer 拉黑作者、作者拉黑 viewer）；works 命中时额外回
  `blocked:true` + 空列表，前端显示「已拉黑」；
- 游标带类型前缀 `w` / `l` / `f`，三个 tab 的游标不能串用（串用回 400）；
- limit 缺省 20、夹到 [1,100]；非法游标 400、用户不存在 404、非 GET 405。

### 存储层

| 改动 | 说明 |
|---|---|
| `ListUserVideos` / `ListLikedVideos` / `ListFavoriteVideos` | 三个 tab 共用 `QueryTimeline`：来源表 + 时间列是唯一差异 |
| `likes` 新索引 `idx_likes_user(user_id, created_at DESC)` | 主键前缀是 video_id，按用户查点赞原本要全表扫 |
| 游标 `w/l/f + <created_at>-<video_id>` | 时间取「动作时间」（点赞/收藏），不是视频发布时间；同秒按 video.id 倒序 |

### 前端

- 主页 tab 栏：作品 / 喜欢 / 收藏，各自独立游标、独立列表、独立「加载更多」；
- 滚动到底自动加载下一页（阈值 260px），底部另有「加载更多」按钮兜底；
- 本人视角：三个 tab 都有内容；他人/匿名点「喜欢」「收藏」→ 锁图标 +「仅本人可见」；
- 空状态按 tab 且分本人/他人：作品（本人/他人）、喜欢、收藏各一条文案；
- 作品数统计改用 `works.total`（旧实现从 `/api/feed?limit=100` 拉全量再过滤，
  视频超过 100 条会漏）；
- 拉黑用户的主页不再请求列表，只显示「已拉黑 + 取消拉黑」。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror` 零告警
- `bash scripts/accept_profile_tabs.sh` → **115 PASS / 0 FAIL**（7 组：作品 / 喜欢 / 收藏 / 拉黑 / 前端 / 性能 / 回归）
- `bash scripts/accept_all.sh` → **70 PASS / 0 FAIL**
- `bash scripts/accept_comment_like.sh` / `accept_notifications.sh` / `accept_report_block.sh` / `accept_share_landing.sh` → 33 / 92 / 79 / 88 PASS（回归）
- 性能（loopback，20 次串行）：`GET /api/user/:id/liked` TTFB avg **1.0 ms**、
  `favorites` avg **1.0 ms**（阈值 200 ms）；`EXPLAIN QUERY PLAN` 命中 `idx_likes_user`
- 隐私用例：他人/匿名看 liked、favorites 均 `private:true`、`items` 为空、
  响应不含任何 `"video"` 对象与标题
- 拉黑用例：alice 拉黑 carol 后 liked 3→2、favorites 2→1，取消后恢复；
  bob 拉黑 alice 后 alice 看 bob 主页 `blocked:true`，匿名不受影响
- 前端：`GET /` 200、页面 > 100 KB，含 `p-tabs` / `profileTabState` / `loadProfileTab` /
  `paintProfilePanel` / 两条锁文案 / 两条空状态文案 / `加载更多`

### 踩坑记录

- **症状**：`ToJson(StoredVideo)` 编译失败（只有 `Video` 有序列化）。
- **原因**：主页 tab handler 在 user 服务里，直接拿了存储行去序列化。
- **修复**：user 服务补一个 `PublicVideo(StoredVideo)`，字段与 video 服务的同名函数一致。
- **同秒排序**：点赞/收藏时间只到秒，多条操作落在同一秒时必须用
  `(created_at DESC, video.id DESC)` 的全序，游标也要携带动作时间；
  想验证「后点的在前」得让两次动作落在不同秒（验收脚本里 sleep 1.1s）。

## 八、第 6 轮交付：社交三件套（@mention / 评论排序 / 评论置顶）

### 接口（interaction 服务 29104）

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/comment/:video_id?sort=hot\|new&cursor=&limit=` | 公开 | 评论列表；`sort` 缺省 `new` |
| GET | `/api/video/:id/comments?sort=&cursor=&limit=` | 公开 | 同一 handler 的路径别名（网关参数路由 → interaction） |
| POST | `/api/comment/:id/pin` | 作者 | 置顶；非作者 403、不存在 404、回复 400 |
| DELETE | `/api/comment/:id/pin` | 作者 | 取消置顶（幂等） |

- **排序**：`new` = `created_at DESC, id DESC`；`hot` = 评论点赞数 DESC、
  `created_at DESC, id DESC`；非法 sort 400。`new` 游标 `<created_at>-<id>`、
  `hot` 游标 `h<赞>-<created_at>-<id>`，两种游标串用一律 400（不静默跳页）。
- **置顶**：`comments` 新列 `pinned_at`（0=未置顶）；第一页固定把置顶评论排最前
  （不分 sort），置顶评论**不占**分页 limit、翻页不重复返回；最多 20 条。
- **@mention**：`POST /api/comment` 正文解析 `@昵称`（中文/英文/数字，1~20 码点），
  精确匹配 `users.name`（重名取最早注册者），写 `mention` 通知：
  `target_type=video`、`target_id=video_id`，点击跳视频；自己 @ 自己、同一条评论
  重复 @ 同一人都不重复写；`a@b` 这类邮箱不误判；不存在的昵称静默跳过。

### 存储层

| 改动 | 说明 |
|---|---|
| `comments.pinned_at` + `idx_comments_pinned(video_id, pinned_at DESC, id DESC)` | 旧库 ALTER 迁移，新库建表即含 |
| `idx_users_name(name)` | @mention 按昵称精确查人，避免每条评论全表扫 |
| `ListPinnedComments` | 置顶区单独查询（上限 20），与普通列表解耦 |
| `ListComments(..., sort, ...)` | hot 用相关子查询回算点赞数（评论点赞数不落列），new 走原索引 |
| `SetCommentPinned` | `UPDATE ... WHERE id`；changes=0 表示评论不存在 |
| `FindUserByName` | `WHERE name=? ORDER BY id ASC LIMIT 1` |

### 前端

- 评论面板头部「最新 / 最热」分段按钮，切换后按 `sort=` 重拉；
- 置顶评论显示「置顶」标签；视频作者额外看到「置顶 / 取消置顶」按钮；
- 评论输入框输入 `@` 弹用户补全下拉：160ms 防抖调
  `GET /api/search?type=user&limit=5`，取前 5，支持 ↑↓ 选择、Enter/点击插入
  `@昵称 `，Esc/失焦关闭；搜索响应按序号丢弃过期结果；
- 通知中心 mention 文案改为「X 在评论中提到了你」，点击跳视频（沿用 target=video）；
- 分享落地页 `/v/:id` 首屏同样把置顶评论排最前并显示「置顶」标签。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制全 ok，`-Werror -Wpedantic` 零告警
- `bash app/douyin/scripts/accept_social_cluster.sh` → **89 PASS / 0 FAIL**（9 组）
- `bash app/douyin/scripts/accept_all.sh` → **70 PASS / 0 FAIL**
- 历史回归：comment_like 33、notifications 92、report_block 79、
  share_landing 88、profile_tabs 115，全部 0 FAIL
- 性能（loopback，20 次串行）：`GET /api/comment/:id?sort=hot&limit=50`
  TTFB avg **1.18 ms** / max **1.54 ms**（阈值 200 ms）
- 覆盖：mention 解析/通知字段/自己@自己/重复@/邮箱/不存在昵称、new 与 hot 顺序、
  hot limit=1 三页游标、非法 sort、游标串用、带 token 视角、置顶权限
  （403/401/404/400）、置顶优先于 sort、置顶不占 limit、翻页不重复、
  重复置顶/取消幂等、别名路径 200/400/405/404、前端元素、TTFB
- 前端 JS 校验（无 node 环境）：抓 `GET /` 内联 JS，跳过字符串/注释/正则后
  括号严格配平，本轮函数与页面元素齐全

### 踩坑记录

- **hot 游标被判 400**：`ReadCursor` 原用 new 游标解码器预校验，`h...` 开头直接
  失败。修复：读取阶段只取值，格式校验下沉 `Store::ListComments` 按 sort 选解码器。
- **SQL 参数顺序**：hot/new 的 `? = 0` 开关与 viewer_id 曾写反（匿名时恰好都为 0，
  带 token 才会暴露）。修复：显式传 `(viewer_id > 0 ? 1 : 0)`。
- **别名路径 POST 变 404**：网关只注册 `kReadMask` 时，POST 落进
  `/api/video/**` 通配被 video 服务 404。修复：别名路由注册 `method_mask::kAll`，
  由 interaction 统一回 405。


## 九、第 7 轮交付：内容三件套（草稿箱 / 视频编辑 / 发布流程）

### 草稿箱（前端 localStorage）

- 键位：`douyin_draft_autosave`（当前编辑）、`douyin_drafts`（列表，最多 10 条）；
- 标题/描述/话题/文件名/选中封面 dataURL/裁剪区间在 `input` 后 **1s 防抖**自动保存，
  另在 `beforeunload` 与「返回 / 关闭」时同步保存一次；
- 重新打开发布面板：本地有自动保存副本 → 顶部提示「有未发布的草稿，是否恢复？」，
  「恢复」填充表单并进入编辑态，「忽略」删除该副本与列表项；
- 「我的草稿」列表：标题 + 更新时间 + 「继续编辑」「删除」；
  浏览器不能保存 File 本体，继续编辑会提示重新选择同名文件；
- 发布成功：`clearCurrentDraft()` 删除列表项与自动保存副本（按 draftId 精确匹配）。

### 视频编辑（封面 + 裁剪，无转码）

- 封面：选文件后 `<video>` 定位 0.5s / 2s / 末帧，`<canvas>` 抽帧
  `toDataURL('image/jpeg', 0.7)` 生成 3 张候选；点选一张 → 发布前先调
  `POST /api/video/cover`（新接口）落盘为 `videos/cover_<uid>_<ms>.jpg`，
  返回短路径作为 `cover_url`；
- 裁剪：两条滑杆记录起止（0.1s 精度），发布时带 `clip_start_ms`/`clip_end_ms`；
- 播放端：`buildSlide` 把区间写进 `video.dataset.clipStart/clipEnd`，
  `buildProgress` 的填充/拖动/时间文本按裁剪窗计算，`timeupdate` 到
  `clip_end` 暂停并停在终点，`playOnly` 起播前拉回 `clip_start`；
- 分享页 `og:image` 优先用 `cover_url`（文件存在才用，否则回退同名 jpg/demo）。

### 发布流程完善

- 进度条：`XMLHttpRequest.upload.onprogress` → 百分比 + 填充条；
- 失败重试：网络错误 / 5xx / 4xx 时按钮变「重试」，表单与选中文件全部保留，
  重试复用同一 `Idempotency-Key`；
- 成功：toast「发布成功 🎉」→ 关闭面板 → 新视频插到 feed 首位 →
  跳转「我的主页」（无登录态时 `jumpToVideo`）；
- 防重复：`pub.uploading` 标志 + 发布/保存/返回/选文件按钮同时 disabled；
- 服务端幂等：`/api/video/upload` 识别 `Idempotency-Key`（内存表，24h TTL，
  上限 4096 条），同 key 二次请求回放同一条视频并加
  `Idempotency-Replayed: true`。

### 后端字段与接口

| 项 | 改动 |
|---|---|
| `videos` 表 | 新增 `cover_url TEXT NOT NULL DEFAULT ''`、`clip_start_ms INTEGER NOT NULL DEFAULT 0`、`clip_end_ms INTEGER NOT NULL DEFAULT 0`；`EnsureSchema` 三条 ALTER 兼容旧库 |
| `StoredVideo` / `douyin::Video` | 同名三字段；`ToJson(Video)` 在 video / feed / user 三个服务的转换处统一输出 |
| `CreateVideo` | 新增带封面/裁剪的重载，旧签名转发（0/0） |
| `POST /api/video/cover` | **新增**：`{"data_url":"data:image/jpeg;base64,..."}` → 落盘 + 返回 `cover_url`；只收 jpeg/png，≤300 KiB，校验魔数；401/400/500 |
| `POST /api/video`、`POST /api/video/upload`、`POST /api/video/upload/finish` | 接受可选 `cover_url`/`clip_start_ms`/`clip_end_ms`，非法 400；响应回传三字段 |
| 校验 | 封面只允许 http(s) 或 `videos/<安全文件名>.(jpg\|png)`；裁剪 `0<=start<end<=24h`（end=0 到片尾） |

### 验收

- `bash app/douyin/build.sh` → 8 个二进制 ok，`-Werror -Wpedantic` 零告警
- `bash app/douyin/scripts/accept_content_cluster.sh` → **79 PASS / 0 FAIL**（9 组）
- `bash app/douyin/scripts/accept_all.sh` → **70 PASS / 0 FAIL**
- 历史回归：profile_tabs 115、upload_resume 50、social_cluster 89，全部 0 FAIL
- 性能（loopback，20 次串行）：视频详情 avg **0.7 ms**、feed avg **0.7 ms**、
  封面接口 avg **0.7 ms**（阈值 200 ms）
- 覆盖：封面上传（PNG/JPEG/落盘字节/401/格式/坏 base64/空 body）、发布三字段
  往返（响应/详情/feed/mine）、非法裁剪与封面、整包上传、分片 finish、
  同 key 幂等（同 id + Replayed + mine 数量不变）、旧库 ALTER 迁移、
  前端实现关键字、TTFB、分享页回归

### 踩坑记录

- **WSL 树不能直接 write/edit**：`\\wsl.localhost` 写文件报 ENOTSUP/EIO，
  继续用「Windows `.dsh_tmp` 落盘 + `wsl cp`」；
- **feed/user 服务漏字段**：两个服务各有自己的视频转换函数，只改 video 服务
  会导致 feed 里没有封面/裁剪；三处统一补齐；
- **幂等基数取错**：验收先取「上传前」数量，第一次上传本身 +1 会误判；
  改为「第一次上传后」取基数；
- **迁移测试停掉全部服务**：gateway 一起被杀导致后续请求全部 000；改为只重启
  video_service，并按 pid 文件精确删除条目。

## 十、第 8 轮交付：播放体验三件套（手势增强 / 进度条预览 / 自动连播）

本轮只改前端 `include/douyin_home_page.hpp`（内联 HTML/CSS/JS），后端零改动；
「推荐 / 关注」复用已有的 `GET /api/feed` 与 `GET /api/feed/following`。

### 手势增强

- **单击播放/暂停**：延迟 250ms 判定；双击到来时 `clearTapTimer` 取消单击，
  不会出现「先暂停再播放」的闪屏；
- **双击点赞**：保留 `doubleTapLike` + 心形动画；鼠标与触摸统一走 `pointerup`
  的 280ms 双轻点判定（原来只有 `dblclick`，触屏不生效）；
- **长按 2×**：500ms 未移动 → `playbackRate = 2` + 「2× 倍速」角标
  （`.speed-badge`）；松手恢复所选倍速；长按期间若视频暂停先 `playOnly`；
- **上下滑切换**：`goSlide(±1)` 按屏 `scrollIntoView({behavior:'smooth'})`，
  `scroll-snap` 保证严格对齐；鼠标纵向拖 ≥70px 触发，触摸交给原生滚动；
- **左右滑切 tab**：topbar 新增「推荐 / 关注」双 tab（`.feed-tabs`）；
  横向拖 ≥60px 或点 tab 切换；未登录切关注只提示不切；
- 统一手势状态机：`pointerdown/move/up/cancel` + `setPointerCapture`；
  长按/双击/滑动后 `suppressUntil` 吃掉随后 click，避免误暂停；
- 兼容兜底：`!window.PointerEvent` 时保留 click 单击（`detail>=2` 双击）。

### 进度条预览

- 拖动 / 悬停时进度条上方悬浮 `.progress-tip`：`mm:ss / mm:ss`，左右各留 6% 防出屏；
- 拖动时 `video.pause()`，松手按 `wasPlaying` 恢复播放（原来拖动不暂停）；
- `video.currentTime = start + ratio * span` 节流 **60ms**（`SEEK_THROTTLE_MS`），
  松手补一次最终 seek；
- `.progress-buffer` 按 `video.buffered` 高亮已缓冲区间（优先含当前时间的区间）；
- 保留裁剪窗（`clip_start/clip_end`）语义；键盘 ←/→ 仍可 5s 微调；
- `preload=none` 首次拖动仍临时升级 `metadata`，拿到 duration 后补 seek。

### 自动连播

- `state.autoplay` 默认开（`localStorage['douyin_autoplay']`，`'0'` 关），
  topbar「连播：开/关」按钮切换；
- 开连播时 `video.loop=false`（否则 `ended` 永不触发），关掉则单条循环；
- `ended` / 裁剪到点 → `onSlideEnded`：1.2s 去重 → `goSlide(1)`；
  已在最后一条 → toast「已到底，上滑刷新推荐流」+ 900ms 后 `loadFeed()`；
- 连播切下一条保留音量/静音/倍速（`playOnly` + `renderFeed` 统一
  `applySound` / `applyRate` / `applyAutoplay`）；
- 新增「倍速：1× / 1.25× / 1.5× / 2×」按钮（`localStorage['douyin_rate']`），
  长按只做临时 2×，松手回到该设置。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制 ok，`-Werror -Wpedantic` 零告警；
- `bash app/douyin/scripts/accept_player_cluster.sh` → **94 PASS / 0 FAIL**（8 组）：
  关注流数据面（401 / 空 / 关注 3 条 / 取关）、手势绑定、进度预览、自动连播、
  空白边界、内联 JS 括号平衡、TTFB、回归；
- `accept_all.sh` → **70 PASS / 0 FAIL**；历史回归：内容三件套 79、
  主页三 tab 115、社交三件套 89，全部 0 FAIL；
- TTFB（20 次均值）：`GET /` **0.68 ms**、`/api/feed` **0.79 ms**、
  `/api/feed/following` **1.04 ms**（阈值 200 ms），页面 203 KB；
- 环境无 node/chromium：JS 语法用「抽取 `<script>` + 朴素 tokenizer 括号平衡」
  校验（跳过字符串/注释/正则），已内置到验收脚本。

### 踩坑记录

- **`video.loop=true` 让 `ended` 永不触发**：自动连播必须先关 loop；
- **长按后浏览器仍派发 click**：用 `suppressUntil` 吃掉，否则松手会误暂停；
- **触屏没有 `dblclick`**：双击判定必须走 pointerup 时间差；
- **进度条拖动 seek 风暴**：pointermove 每像素都 seek 会让解码器抖动，60ms 节流；
- **括号检查的正则误判**：`/^https?:\/\//` 里的 `//` 会被当行注释，tokenizer
  必须先判正则字面量，否则整页括号「不平衡」。

## 十一、第 10 轮交付：直播三件套（聊天 / 礼物 / 贡献榜）

### 存储（`live_messages` / `gifts` / `gift_records`）

- `live_messages(id, room_id, user_id, content, kind, created_at)`，索引
  `idx_live_messages_room(room_id, id DESC)`；`kind` 区分 `chat` / `gift`，
  礼物消息与聊天共用同一个增量游标；
- `gifts(id, name, price_coins, icon)` 预置 5 个（1 / 10 / 30 / 100 / 500 金币）；
- `gift_records(id, room_id, sender_id, gift_id, count, created_at)`，索引
  `idx_gift_records_room(room_id, sender_id)`；
- `users` 增 `coins`（默认 1000；旧库 `ALTER TABLE` 迁移）。

### 接口（live 29105，网关 `/api/live/**`）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/live/:id/messages` | 发言（1~200 字；需登录；双向拉黑 403） |
| GET | `/api/live/:id/messages?after=&limit=` | 增量拉取（`id > after`，升序，1~100） |
| GET | `/api/live/gifts` | 礼物价目（公开） |
| POST | `/api/live/:id/gift` | 送礼（事务扣余额 + 写记录 + gift 消息；余额不足 400） |
| GET | `/api/live/:id/ranking?limit=` | 贡献榜（单条 GROUP BY，1~50） |
| GET | `/api/live/wallet` | 当前用户余额（需登录） |

`/api/live/room/:id/messages|gift|ranking` 与短路径等价（兼容旧调用）。

### 前端（`include/douyin_live_page.hpp`，网关 `GET /live`）

- 直播间聊天：底部列表 + 输入框；2s 轮询 `after=last_id` 增量；自己发言
  乐观上屏（`data-temp`），服务端回包按 id 去重并替换临时节点；
- 礼物面板：5 个礼物（图标 / 名称 / 价格）+ 数量步进 + 余额；送出后聊天区
  高亮「🎁 X 送出 火箭 ×1」并飘屏特效；余额不足前端提示 + 服务端 400 兜底；
- 贡献榜：🏆 按钮弹层，头像（昵称首字）+ 昵称 + 金额，前三名奖牌；
  与聊天同一轮询周期刷新；
- 无 `room` 参数显示房间选择器；`document.hidden` 时暂停轮询，恢复立即拉取；
- 首页顶栏新增「🔴 直播」入口。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制 ok，`-Werror -Wpedantic` 零告警；
- 新增 `scripts/accept_live_cluster.sh` → **128 PASS / 0 FAIL**（10 组）：
  发送 / 长度 / 增量 / 分页 / 双向拉黑 / 礼物扣款 / 余额不足回滚 / 榜单排序 /
  单条 SQL 索引命中 / 20 并发送礼恰好扣完 / 100 条并发消息无遗漏 / 前端元素 /
  2s 轮询 / TTFB；
- 回归：`accept_all.sh` **89 PASS / 0 FAIL**（原 70 + 新增 19）、
  `accept_live.sh` **66 PASS / 0 FAIL**；
- TTFB（20 次均值）：聊天增量 **0.52 ms**、榜单 **0.48 ms**、直播页
  **0.46 ms**（阈值 200 ms）；聊天/榜单查询计划分别命中
  `idx_live_messages_room` / `idx_gift_records_room`；
- 内联 JS 括号平衡（环境无 node）通过。

### 踩坑记录

- **多进程共库 + ORM 游标 = 旧快照**：`live_service` 发言后调 `FindUserById`
  （ORM）留下未 finalize 的读游标，连接被钉在旧快照；之后 `user_service`
  写入的拉黑/取消拉黑延后一拍，出现「拉黑后能送礼、取消后仍 403」；写操作还
  报 `SQLITE_BUSY_SNAPSHOT`。修复：每个请求入口 `store.ResetStatements()`，
  写路径再清一次；
- **WAL 并发启动**：`PRAGMA journal_mode=WAL` 不经过 busy handler，两进程同时
  初始化必现 `SQLITE_BUSY`；`Store::Open` 改为最多 40×50 ms 重试；
- **Windows Python 写 WSL 文件默认 CRLF**：`Path.write_text` 会把 `\n` 变成
  `\r\n`，bash 脚本立刻 `$'\r'` 报错；统一 `newline="\n"` + `sed -i
  's/\r$//'` 兜底；
- **验收脚本裸 `wait`**：会等待 `start_service` 启动的服务子进程，脚本永久
  卡死；改为 `wait "${PIDS[@]}"` 只等并发请求；
- **`json_int id` 撞礼物 id**：送礼响应里 `gift.id` 在 `message.id` 前面，
  取消息 id 必须匹配 `"message":{"id":`。

## 十二、第 11 轮交付：搜索增强（历史 / 热搜 / 建议）+ gzip

### 存储（`search_history` 一张表 + 两个索引）

| 项 | 内容 |
|---|---|
| 表 | `search_history(id, user_id, keyword, created_at)`，`UNIQUE(user_id, keyword)` |
| 索引 | `idx_search_history_user(user_id, created_at DESC)`（我的历史）/ `idx_search_history_created(created_at DESC)`（7 天热搜） |
| 去重 | 写入用 `INSERT OR REPLACE`：同用户同词只留一行；REPLACE 会生成更大的 id，`created_at` 只有秒级精度时靠 `ORDER BY created_at DESC, id DESC` 仍能体现「最近搜索」 |
| 方法 | `RecordSearchHistory` / `ListSearchHistory` / `DeleteSearchHistory` / `SearchHotKeywords`（单条 `GROUP BY`）/ `SearchSuggest`（三源并集去重） |

### 接口（search 29106，网关 `/api/search/**` 全方法转发）

| 方法 | 路径 | 语义 |
|---|---|---|
| GET | `/api/search/history?limit=10` | 本人历史（**需登录**，401；去重、按最近倒序、limit 夹紧 1~50） |
| DELETE | `/api/search/history?keyword=` | 带 keyword 删单条；不带清空；返回 `deleted`；需登录 |
| GET | `/api/search/hot?limit=10` | 热搜（公开；最近 7 天 `GROUP BY keyword`，count 降序、同次数按最近；limit 1~20） |
| GET | `/api/search/suggest?q=&limit=8` | 建议（公开；`LIKE 'q%'` 前缀，热搜 + 用户昵称 + 话题取并集去重；limit 1~20） |
| GET | `/api/search` | 原有综合搜索；**成功后自动记录历史**（匿名不记，记录失败只打日志） |

- 建议的话题来源复用唯一提取点 `ExtractTags`（`douyin_tag.hpp`），先用
  `title LIKE '%q%'` 让 SQLite 过滤再在内存做前缀判定；
- 每个请求入口 `store.ResetStatements()`（本服务新增写路径，避免 ORM 游标旧快照）。

### 前端（`include/douyin_home_page.hpp`）

- 搜索页新增「🕘 搜索历史」面板：单条 ✕ 删除、「清空」；匿名隐藏历史、只显示热搜；
- 「🔥 热搜」：序号（前三红色）+ 关键词 + 搜索次数，点击直接搜；
- 搜索框聚焦且为空时展开「历史 + 热搜」；输入时 200ms 防抖请求建议，
  下拉点击直接搜；点搜索/清空输入后收起；
- 搜索完成后刷新历史（服务端自动记录的权威结果），失败不影响结果渲染。

### gzip 压缩（网关统一装饰器）

- `config.decorate = DecorateGzip`：**所有响应**（首页/直播页本地直出、上游转发、
  错误体）离开网关前统一判断；
- 生效条件：`Accept-Encoding` 含 gzip（`gzip;q=0` 排除）、`body > 1 KiB`、
  `Content-Type` 为 `text/*` / `application/json` / `application/javascript`、
  响应未带 `Content-Encoding`、非文件体；
- 压缩用基座 `go2cpp::compress::Gzip::Writer<StringSink>`（`Close()` 校验完整流）；
  压缩报错或压缩后反而更大时**回退原 body**；
- 补 `Content-Encoding: gzip` + `Vary: Accept-Encoding`；`Content-Length` 由
  现有序列化器按新 body 重算，HEAD 与 GET 一致；已压缩的响应不二次压缩；
- 构建脚本链接新增 `-lgo2cpp_compress`（基座该库已存在）。

### 验收

- `bash app/douyin/build.sh` → 8 个二进制 ok，`-Werror -Wpedantic` 零告警；
- 新增 `scripts/accept_search_cluster.sh` → **71 PASS / 0 FAIL**（6 组：准备 /
  历史 19 / 热搜 11 / 建议 11 / 前端 17 / 性能 4）；
- 新增 `scripts/accept_gzip.sh` → **30 PASS / 0 FAIL**（基线 / 压缩 / 不压缩场景 /
  HEAD / 上游路径 / TTFB）；
- 回归 `scripts/accept_all.sh` → **89 PASS / 0 FAIL**；
- 首页 gzip：**218669 → 61993 字节（28.4%）**；TTFB（taskset 绑核，20 次均值）
  **0.69 ms → 3.81 ms**（本机 loopback 无链路时延，压缩成本约 3.1 ms；
  真实网络下少传 72% 字节）；
- 搜索接口 TTFB：热搜 0.93 ms / 历史 0.79 ms / 建议 0.82 ms（阈值 200 ms）；
- 查询计划：热搜命中 `idx_search_history_created`（`created_at>?`）。

### 踩坑记录

- **秒级时间戳 + `ON CONFLICT DO UPDATE` 无法体现「最近」**：同一秒内先搜 A 再搜 B
  再搜 A，A 的 `created_at` 不变、id 不变，列表仍把 B 排前面。改用
  `INSERT OR REPLACE`（删旧插新、id 递增），排序键 `created_at DESC, id DESC`；
- **验收脚本的简化 JS tokenizer 误报**：`/^https?:\/\//` 里的 `//` 被当行注释，
  整页括号「不平衡」；复用 `accept_player_cluster.sh` 的完整 tokenizer（识别
  正则字面量）后通过；
- **PowerShell 双引号会解析 `$` 与反引号**：`wsl bash -lc "... $VAR ..."` 里的
  变量被本机展开，补丁全部改为「Python 脚本落盘 → `wsl python3` 执行」；
- **热搜 count 的测试数据编排**：清空 alice 历史后忘记补一次搜索，导致
  「3 个用户搜同一词」实际只有 2 行——不是代码 bug，是脚本前置数据缺口。

## 十三、第 12 轮交付：资料编辑 / 视频与评论软删除 / 静态缓存

### 存储（旧库 ALTER 兼容）

| 变更 | 内容 |
|---|---|
| `users.bio` / `users.avatar_url` | `TEXT NOT NULL DEFAULT ''`；ORM 映射同步，`CreateUser` 默认空 |
| `videos.deleted_at` / `comments.deleted_at` | `INTEGER`，NULL=未删，非 NULL=删除时刻 |
| 读路径统一过滤 | videos：`FindVideo` / `ListByUser` / `ListFeed` / `ListFollowingFeed` / `ListAllVideos` / `SearchVideos` / `ListFavorites` / `QueryTimeline`（作品/喜欢/收藏）/ `SearchSuggest` 话题源；comments：`ListComments`（new+hot）/ `ListPinnedComments` / `ListReplies` / `FindComment` / `UpdateVideoCounts` |
| 新方法 | `NameTaken(name, except_id)`（排除自己，改名幂等）；`UpdateUserProfile(id, name, bio, avatar_url)` |
| 删除语义 | `DeleteVideo` / `DeleteComment` 从 DELETE 改为 `UPDATE ... SET deleted_at`；`changes=0` 表示不存在/已删（service 回 404），不产生副作用 |

### 接口

| 方法 | 路径 | 语义 |
|---|---|---|
| GET | `/api/user/me` | 本人资料（Bearer；401）；设置页首屏数据源 |
| PATCH | `/api/user/me` | 部分更新 name/bio/avatar_url；昵称 2~20 码点、不能含 `@`/空白、全局唯一（重复 409）；简介 ≤80；头像 http(s) 或 `videos/` 下 `.jpg/.png` 单段白名单；成功回读权威 user |
| DELETE | `/api/video/:id` | 仅作者（403）；软删后详情/feed/主页/搜索/话题/分享页全部 404；**保留 mp4/封面文件** |
| DELETE | `/api/comment/:id` | 评论作者**或视频作者**（403）；软删后列表不返回，`UpdateVideoCounts` 按未删行重算 comment_count；重复删除 404 |

- 昵称唯一性用「先查后写」（`idx_users_name` 精确匹配）；极端并发下两请求可能同时通过，
  演示规模可接受；真正强一致需唯一索引兜底（旧库可能有重名，留待数据清洗后再加）。
- 通知/评论昵称不冗余存储：通知列表 `LEFT JOIN users` 回填 `actor_name`，改名后自动生效
  （验收里 bob 改名后 alice 的通知立刻变成 bob_new）。

### 前端

- 新增 `include/douyin_settings_page.hpp`，网关直出 `GET /settings`：
  昵称/简介/头像三字段 + 实时字数与逐项校验 + 头像预览 + 409 提示；
  保存成功后把最新 user 写回 localStorage 再回首页（顶栏立即刷新）。
- 首页顶栏新增头像按钮 `#whoAvatar`：有 `avatar_url` 显示图片，否则昵称首字母；
  点击进入 `/settings`。
- 视频「⋯」菜单：作者本人多一项「删除视频」（`window.confirm` 二次确认 → DELETE → 从 feed 移除）。
- 评论「⋯」菜单：评论作者或视频作者多一项「删除评论」（二次确认 → DELETE →
  刷新列表、`comment_count` 与 `cpCount` 同步）。

### 静态缓存头

| 资源 | 头 |
|---|---|
| `/`、`/live`、`/settings`（HTML） | `Cache-Control: no-cache`（每次回源验证） |
| `/v/:id` 分享落地页（HTML） | `Cache-Control: no-store`（沿用第 5 轮语义，比 no-cache 更严格） |
| `/videos/**`、`/covers/**`、`/static/**` | `Cache-Control: public, max-age=86400` |
| 静态 404 / 416 | `no-store`（不把「暂时缺失」固化到浏览器） |

- 新增 `/covers/<name>` 别名（封面与视频同在 `web/videos/`）与 `/static/<name>`
  （`web/static/`，含 `robots.txt`）；单段文件名白名单沿用 `IsSafeVideoName`。

### 验收

- 新增 `scripts/accept_profile_edit.sh` → **53 PASS / 0 FAIL**（改名字段/重名/非法字符/
  简介 80/81 边界/头像白名单/未登录/通知昵称同步/设置页/缓存头/TTFB）；
- 新增 `scripts/accept_delete.sh` → **45 PASS / 0 FAIL**（评论删除权限/计数一致/视频删除权限/
  软删后 6 个读入口不可见/DB 行保留/重复删除不改 deleted_at/已删内容不可互动）；
- 回归 `scripts/accept_all.sh` → **89 PASS / 0 FAIL**；相关回归（评论点赞/通知/主页 tab/
  搜索/分享页/gzip）全部 0 FAIL；
- 构建 `bash app/douyin/build.sh` → 8 个二进制 ok，`-Werror -Wpedantic` 零告警；
- 缓存头 `curl -I`：首页/设置页 `no-cache`，`/videos/1.mp4`、`/covers/1.jpg`、
  `/static/robots.txt` 均 `public, max-age=86400`（`/static` 的 Content-Type 为
  `text/plain; charset=utf-8`）；
- TTFB（5 次均值，阈值 200 ms）：`/` 0.8 ms、`/settings` 0.4 ms、`/api/feed` 0.9 ms、
  `/api/user/me` 0.7 ms —— 无退化。

### 踩坑记录

- **软删必须全链路过滤**：只改 `FindVideo` 时 feed/主页/搜索仍会漏出已删视频；
  逐个 SQL 补 `deleted_at IS NULL` 后，用「6 个入口都查一遍」的验收兜底；
- **comment_count 用重算而不是 `-1`**：并发/重复删除/删除二级回复时自减会漂移，
  `UpdateVideoCounts` 按未删行 COUNT 一次，天然幂等；
- **软删不要顺手删文件**：旧实现删 DB 后删 mp4，改软删后必须移除文件清理，
  否则「删除」变「销毁」，恢复与分享兜底全失效；
- **头像白名单与封面白名单要同一套**：两处各写一份迟早漂移，本轮的
  `ValidAvatarUrl` 是 `ValidCoverUrl` 的同规则副本，注释里互相指向；
- **PATCH 空 body**：`JsonStringField` 只判断字段是否存在，三个字段都缺时必须回 400，
  否则会静默写回原值让前端以为保存成功。

## 十四、下一轮建议

1. 举报运营后台：`reports.status` 只有 pending，缺列表 / 处理接口与页面。
2. 服务端草稿：当前草稿只在 localStorage（换设备/清缓存即丢）；可加
   `drafts` 表 + 多端同步，File 本体仍由用户重选。
3. 「待核实」剩余：WS Resume 仍未实现；压缩已于第 11 轮落地
   （gzip + `Vary` + 回退策略）。搜索侧下一步可做：历史/热搜的个人化
   （按关注关系加权）、搜索词高亮、联想词点击率埋点。
4. 落地页增强：评论回复折叠、作者其它作品、播放进度记忆；
   `og:image` 已支持 `cover_url`，可再加服务端自动截帧。
5. 播放侧下一批：单条播放进度记忆（localStorage `douyin_pos_<id>`）、
   关注流分页 / 下拉刷新、字幕开关与倍速音调补偿。
