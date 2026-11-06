# 抖音 Demo 进度看板

| 项 | 内容 |
|---|---|
| 盘点时间 | 2026-11-06（第 4 轮） |
| 盘点范围 | `app/douyin/` 全部源码（7 服务 + 网关 + 单页前端） |
| 盘点方式 | 读 REQUIREMENTS.md、逐服务路由表、前端函数/元素清单、关键词扫描、跑既有验收 |
| 基线 | `scripts/accept_all.sh` → 70 PASS / 0 FAIL；`scripts/accept_notifications.sh` → 92 PASS / 0 FAIL；`scripts/accept_report_block.sh` → 79 PASS / 0 FAIL；`scripts/accept_share_landing.sh` → **88 PASS / 0 FAIL** |

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

- [x] **举报 / 拉黑（第 3 轮新增）**：`reports` / `blocks` 两表；举报去重 +
      拉黑/取消/状态/黑名单 5 个接口；feed / 视频详情 / 评论 / 搜索 / 通知 /
      收藏 / 话题 7 处读过滤；评论/点赞/关注写接口防骚扰（403）；前端「⋯」菜单、
      举报弹窗、黑名单抽屉
- [x] **分享落地页（第 4 轮新增）**：`/v/:id` 由 video 服务 SSR 直出独立页面
      （播放器 / 作者关注 / 点赞评论数 / 评论区翻页 / 下载分享 + OG 预览）；
      已删除、被拉黑作者一律 404，匿名按公开页处理

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
| 3 | ~~举报 / 拉黑~~ | 高 | — | **本轮已完成** |
| 3.1 | 举报列表后台（运营端） | 中 | 1 h | 目前只落库，无管理接口；见 `reports.status` |
| 4 | ~~分享落地页 `/v/:id`~~ | 中高 | — | **本轮已完成**（独立 SSR 页 + OG 预览） |
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

## 七、下一轮建议

1. 个人主页三 tab（#5）：作品 / 喜欢 / 收藏，需要 `/api/video/liked`。
2. 举报运营后台：`reports.status` 只有 pending，缺列表 / 处理接口与页面。
3. 「待核实」三项：@mention / 草稿 / WS Resume / 压缩——要么补实现，要么从
   已完成清单里移除；@mention 可直接复用通知中心的 `mention` 类型。
4. 落地页增强：评论回复折叠、作者其它作品、播放进度记忆；分享卡片的
   `og:image` 可选自动截帧（当前复用同名 jpg / demo 封面）。
