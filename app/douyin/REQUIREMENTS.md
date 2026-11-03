# 抖音 Demo 需求文档（P0）

| 项 | 内容 |
|---|---|
| 文档编号 | APP-DOUYIN-REQ-01 |
| 版本 | v1.0 |
| 日期 | 2026-11-03 |
| 状态 | 生效，作为 C2–C10 的实现与验收依据 |
| 目录 | `app/douyin/`（原 `demos/douyin/` 骨架已迁入） |
| 定位 | 单机可跑的短视频/直播业务 demo，验证 Go2Cpp 在「HTTP + 并发 + 存储」上的组合能力 |

---

## 1. 目标与范围

### 1.1 总目标

单机启动 6 个业务服务 + 1 个网关，跑通一条完整业务闭环：

```
注册/登录 → 发布视频 → 刷 Feed → 点赞/评论 → 搜索 → 开播/进直播间
```

所有接口用 REST + JSON；数据落 SQLite；压测达标；5 个真实场景可用 curl 一条命令复现。

### 1.2 In Scope

| 项 | 说明 |
|---|---|
| 6 服务 + 网关 | user / video / feed / interaction / live / search + gateway |
| REST + JSON | 统一错误格式与状态码 |
| SQLite 持久化 | 复用 `go2cpp::storage::DB` + ORM，WAL 模式 |
| 鉴权 | 简单 Bearer token（登录发放，网关校验） |
| 压测 | 仓库内 `loadgen`（不用第三方压测工具） |
| 验收脚本 | `app/douyin/scripts/accept.sh` 一键跑 5 场景 |

### 1.3 Out of Scope（P0 不做）

- 视频转码/切片/对象存储（视频只存 URL 引用，不存字节）；
- 推荐算法（Feed 用「时间倒序 + 热度加权」的简单公式，不做 ML）；
- 直播流媒体（只做房间状态与在线数，不推流）；
- 分布式/多机部署、CDN、消息队列；
- 前端页面（只提供 REST，验收用 curl）。

---

## 2. 角色与用户故事

| 角色 | 用户故事 |
|---|---|
| 创作者 | 注册后能发布视频、看到自己视频的点赞/评论数、能开直播 |
| 观众 | 不登录也能刷 Feed；登录后能点赞、评论、关注、搜索、进直播间 |
| 游客 | 只能访问 `/health` 与只读 Feed（P0 允许匿名刷 Feed） |

---

## 3. 功能需求

### FR-01 用户（user_service，29101）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-01.1 | 注册 | `POST /api/user/register` | 返回 `User` + `token`；重复 email → 409 |
| FR-01.2 | 登录 | `POST /api/user/login` | 正确密码 → 200 + token；错误 → 401 |
| FR-01.3 | 资料 | `GET /api/user/:id` | 返回 `User`（不含 password_hash）；不存在 → 404 |
| FR-01.4 | 关注 | `POST /api/user/:id/follow` | 幂等；自己关注自己 → 400 |
| FR-01.5 | 粉丝/关注列表 | `GET /api/user/:id/following`、`/followers` | 分页返回 id 列表 |

请求示例：

```json
POST /api/user/register
{"name":"alice","email":"alice@example.com","password":"secret123"}
→ 200 {"ok":true,"user":{"id":1,"name":"alice","email":"alice@example.com","created_at":1762...},"token":"..."}
```

密码存储：`SHA-256(salt + password)`，salt 每用户随机 16B，hex 存 `password_hash`。

### FR-02 视频（video_service，29102）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-02.1 | 发布 | `POST /api/video`（需 token） | 返回 `Video.id`；缺 title/url → 400 |
| FR-02.2 | 详情 | `GET /api/video/:id` | 含 author 名、like_count、comment_count；不存在 → 404 |
| FR-02.3 | 我的视频 | `GET /api/video/mine`（需 token） | 按 created_at 倒序 |
| FR-02.4 | 删除 | `DELETE /api/video/:id`（需 token，仅作者） | 非作者 → 403；不存在 → 404 |

### FR-03 Feed（feed_service，29103）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-03.1 | 推荐流 | `GET /api/feed?cursor=&limit=` | 时间倒序 + 热度（like*2+comment*3）加权；`limit` 默认 20、上限 100 |
| FR-03.2 | 关注流 | `GET /api/feed/following`（需 token） | 只含已关注作者的视频 |
| FR-03.3 | 分页 | cursor 为上一页最后一条的 `(created_at,id)` 编码 | 不重不漏 |

热度公式（P0 固定）：`score = like_count*2 + comment_count*3 - age_hours*0.1`，同分按 id 倒序。

### FR-04 互动（interaction_service，29104）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-04.1 | 点赞 | `POST /api/like`（需 token） | 幂等（唯一键）；返回最新 like_count |
| FR-04.2 | 取消赞 | `DELETE /api/like`（需 token） | 幂等 |
| FR-04.3 | 发评论 | `POST /api/comment`（需 token） | 返回 `Comment`；内容空 → 400；超 500 字 → 400 |
| FR-04.4 | 评论列表 | `GET /api/comment/:video_id?cursor=&limit=` | 时间倒序；不存在视频 → 404 |
| FR-04.5 | 计数一致性 | 点赞/评论后 video 的计数同步 | 由 service 内事务更新 `videos.like_count/comment_count` |

### FR-05 直播（live_service，29105）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-05.1 | 创建房间 | `POST /api/live/room`（需 token） | 返回 `Room`，status=0（关播） |
| FR-05.2 | 开播/关播 | `POST /api/live/room/:id/start`、`/stop`（仅房主） | 非房主 → 403 |
| FR-05.3 | 房间列表 | `GET /api/live/rooms?status=` | 只列 status=1（直播中）可选 |
| FR-05.4 | 进/出房间 | `POST /api/live/room/:id/enter`、`/leave` | viewer_count 增减，幂等 |

### FR-06 搜索（search_service，29106）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-06.1 | 综合搜索 | `GET /api/search?q=&type=&limit=` | type ∈ user/video/live/all；按 title/name 前缀+包含匹配 |
| FR-06.2 | 空查询 | `q` 为空 | 返回 400 或空列表（二选一，**定为 400**） |
| FR-06.3 | 分页 | cursor | 与 Feed 同规则 |

### FR-07 网关（gateway，28080）

| 编号 | 需求 | 验收 |
|---|---|---|
| FR-07.1 | 唯一入口 | 全部 `/api/**` 经网关转发到对应服务；未知路径 → 404 |
| FR-07.2 | 鉴权 | 需 token 的接口在网关校验 Bearer；无效/缺失 → 401 |
| FR-07.3 | 透传 | 请求体、query、状态码、响应体原样透传（不解析业务字段） |
| FR-07.4 | 健康 | `GET /health` 返回 gateway 自身；`GET /api/health/all` 聚合 6 服务健康 |
| FR-07.5 | 超时 | 上游 500ms 未响应 → 504；上游不可达 → 502 |

### FR-08 可观测

- 每个服务 `GET /health` → `{"ok":true,"service":"<name>"}`；
- 每个服务 `GET /metrics` → 简单计数（请求数/错误数/延迟 p50/p99，JSON）；
- 网关 `GET /api/metrics` 聚合。

### FR-09 持久化

- 单文件 `app/douyin/data/douyin.db`，SQLite WAL；
- 启动时自动建表（`CREATE TABLE IF NOT EXISTS`）+ 索引；
- 服务崩溃重启后数据不丢（验收：kill 后重启查数据）。

### FR-10 压测与验收脚本

- `app/douyin/scripts/accept.sh`：启动 7 进程 → 5 场景 curl → 输出 PASS/FAIL；
- `app/douyin/scripts/loadgen.cpp`：自研压测（N 并发 × M 请求，报 QPS/p50/p99）；
- 压测目标见 §7。

---

## 4. 数据模型

```sql
-- users
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  email TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  salt TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);

-- videos
CREATE TABLE IF NOT EXISTS videos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  title TEXT NOT NULL,
  url TEXT NOT NULL,
  like_count INTEGER NOT NULL DEFAULT 0,
  comment_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_videos_user ON videos(user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_videos_created ON videos(created_at DESC);

-- likes（幂等：主键）
CREATE TABLE IF NOT EXISTS likes (
  video_id INTEGER NOT NULL,
  user_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY (video_id, user_id)
);

-- comments
CREATE TABLE IF NOT EXISTS comments (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  video_id INTEGER NOT NULL,
  user_id INTEGER NOT NULL,
  content TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_comments_video ON comments(video_id, created_at DESC);

-- follows（幂等：主键）
CREATE TABLE IF NOT EXISTS follows (
  follower_id INTEGER NOT NULL,
  followee_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY (follower_id, followee_id)
);

-- rooms
CREATE TABLE IF NOT EXISTS rooms (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  title TEXT NOT NULL,
  status INTEGER NOT NULL DEFAULT 0,   -- 0=关播 1=直播中
  viewer_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_rooms_status ON rooms(status, created_at DESC);
```

存储选型：**复用 `go2cpp::storage::DB` + `storage::orm`**（`include/go2cpp/storage/db.hpp`、`orm.hpp`），底层系统 `libsqlite3`；不引入第三方库。

---

## 5. 接口规范

### 5.1 通用约定

- 请求/响应 `Content-Type: application/json`；
- 鉴权：`Authorization: Bearer <token>`；
- 成功：`200` + `{"ok":true,...}`；创建：`201`（P0 统一 200，可接受）；
- 错误：`{"ok":false,"error":"<message>","code":<int>}`；
- 错误码：400 参数错、401 未登录、403 无权限、404 不存在、409 冲突、500 内部错、502 上游不可达、504 上游超时。

### 5.2 全量端点表

| 方法 | 路径 | 服务 | 鉴权 | 说明 |
|---|---|---|---|---|
| GET | `/health` | 各 | 否 | 健康 |
| GET | `/metrics` | 各 | 否 | 计数 |
| POST | `/api/user/register` | user | 否 | 注册 |
| POST | `/api/user/login` | user | 否 | 登录 |
| GET | `/api/user/:id` | user | 否 | 资料 |
| POST | `/api/user/:id/follow` | user | 是 | 关注 |
| DELETE | `/api/user/:id/follow` | user | 是 | 取关 |
| GET | `/api/user/:id/following` | user | 否 | 关注列表 |
| GET | `/api/user/:id/followers` | user | 否 | 粉丝列表 |
| POST | `/api/video` | video | 是 | 发布 |
| GET | `/api/video/:id` | video | 否 | 详情 |
| GET | `/api/video/mine` | video | 是 | 我的视频 |
| DELETE | `/api/video/:id` | video | 是 | 删除 |
| GET | `/api/feed` | feed | 否 | 推荐流 |
| GET | `/api/feed/following` | feed | 是 | 关注流 |
| POST | `/api/like` | interaction | 是 | 点赞 |
| DELETE | `/api/like` | interaction | 是 | 取消赞 |
| POST | `/api/comment` | interaction | 是 | 评论 |
| GET | `/api/comment/:video_id` | interaction | 否 | 评论列表 |
| POST | `/api/live/room` | live | 是 | 创建房间 |
| POST | `/api/live/room/:id/start` | live | 是 | 开播 |
| POST | `/api/live/room/:id/stop` | live | 是 | 关播 |
| GET | `/api/live/rooms` | live | 否 | 房间列表 |
| POST | `/api/live/room/:id/enter` | live | 是 | 进房 |
| POST | `/api/live/room/:id/leave` | live | 是 | 出房 |
| GET | `/api/search` | search | 否 | 搜索 |
| GET | `/api/health/all` | gateway | 否 | 聚合健康 |

---

## 6. 非功能需求

| 类别 | 指标 | 验收方式 |
|---|---|---|
| 吞吐 | 单机 ≥2000 QPS（gateway 转发 `/api/feed`） | `loadgen 200 30s` |
| 延迟 | p99 < 50 ms、p50 < 5 ms（页缓存热库） | 同上 |
| 并发 | 1000 并发无 5xx、无丢数据 | `loadgen 1000 10s` |
| 存储 | 10 万视频 + 100 万点赞可查询 | 造数脚本 + 查询延迟 |
| 可靠性 | kill -9 任一服务后重启，数据不丢 | 手动验收 |
| 安全 | 密码不落明文；token 不可伪造（HMAC-SHA256） | 代码审查 + 401 用例 |
| 可维护 | 单服务 ≤800 行；公共逻辑抽到 `include/` | 代码审查 |

---

## 7. 验收标准（P0 完成定义）

### 7.1 五个真实场景（`scripts/accept.sh` 一键）

1. **注册→登录**：注册 alice → 登录拿 token → `GET /api/user/1` 返回 alice；
2. **发布→详情**：用 token 发布 2 条视频 → `GET /api/video/:id` 返回 title/author；
3. **Feed**：`GET /api/feed` 中出现刚发的视频，按时间倒序；`limit=1` 只返回 1 条；
4. **点赞幂等**：`POST /api/like` 两次 → like_count 只 +1；`DELETE` 后 -1；
5. **评论→列表**：发 2 条评论 → `GET /api/comment/:video_id` 返回 2 条且顺序正确。

### 7.2 正确性用例

- 重复 email 注册 → 409；
- 错误密码 → 401；
- 缺 title 发布 → 400；
- 不存在的 video → 404；
- 非作者删除 → 403；
- 无 token 调用需鉴权接口 → 401；
- 空 `q` 搜索 → 400。

### 7.3 压测

`loadgen --concurrency 200 --duration 30 --target feed` → 输出 QPS/p50/p99，写入本文件 §6 的实测行。

### 7.4 可靠性

`kill -9 user_service` → 重启 → 再查 alice 资料仍 200。

---

## 8. 目录结构与技术选型

```
app/douyin/
├── REQUIREMENTS.md          # 本文档
├── README.md                # 构建/启动/验收命令
├── build.sh                 # 编译 7 个二进制
├── include/
│   ├── douyin_proto.hpp     # 共享 IDL（结构 + ToJson/FromJson）
│   ├── douyin_http.hpp      # 最小 HTTP/1.1（基于 go2cpp::net::Socket）
│   ├── douyin_store.hpp     # SQLite 连接 + 建表 + 访问器（复用 storage::DB/orm）
│   └── douyin_auth.hpp      # token 签发/校验（HMAC-SHA256）
├── src/
│   ├── douyin_proto.cpp  douyin_store.cpp  douyin_auth.cpp
│   ├── user_service.cpp  video_service.cpp  feed_service.cpp
│   ├── interaction_service.cpp  live_service.cpp  search_service.cpp
│   └── gateway.cpp
├── scripts/
│   ├── accept.sh            # 5 场景验收
│   └── loadgen.cpp          # 自研压测
└── data/                    # SQLite 文件（.gitignore）
```

技术选型：

| 层 | 选型 | 理由 |
|---|---|---|
| HTTP | `douyin_http.hpp`（自写最小 HTTP/1.1） | 骨架已有；不引第三方 |
| JSON | `douyin_proto.cpp`（手写） | 结构固定，够用 |
| 存储 | `go2cpp::storage::DB` + ORM + 系统 libsqlite3 | 项目自带，零新增依赖 |
| 并发 | `go2cpp::net::Socket` + 每连接一线程（P0）或 Scheduler（P1） | 先跑通，再优化 |
| 压测 | 自研 `loadgen` | 不引第三方 |

端口：gateway 28080、user 29101、video 29102、feed 29103、interaction 29104、live 29105、search 29106（避开电商 8080/9101-9103 与 nginx 18080）。

---

## 9. 里程碑

| 里程碑 | 内容 | 完成判据 |
|---|---|---|
| C2 | user 服务（注册/登录/资料/关注）+ 存储层 | 5 场景中 1、2 通过；409/401 正确 |
| C3 | video 服务（发布/详情/我的/删除） | 场景 2 全过；403/404 正确 |
| C4 | feed 服务（推荐/关注/分页） | 场景 3 通过；limit/cursor 正确 |
| C5 | interaction（点赞/评论/计数） | 场景 4、5 通过；幂等 |
| C6 | search（综合/分页） | 搜索命中；空 q 400 |
| C7 | live（房间/开播/进出） | viewer_count 正确 |
| C8 | gateway（路由/鉴权/超时/聚合健康） | 全部接口经网关可访问；401/502/504 |
| C9 | 验收脚本 + loadgen + 压测报告 | §7 全过；QPS/p99 达标 |
| C10 | 可靠性 + 文档收尾 | kill 重启不丢；README 完整 |

---

## 10. 风险与开放问题

| 风险 | 影响 | 缓解 |
|---|---|---|
| 自写 HTTP 的并发模型（每连接一线程）在 1000 并发下线程爆炸 | 压测不达标 | C9 时切到 `Scheduler`/`EventLoop`，或限制线程池 |
| SQLite 单写者锁 | 写 QPS 上不去 | WAL + 短事务 + 写合并；写目标 500 QPS |
| 自研 JSON 解析边界（转义/嵌套） | 数据错误 | 只支持本项目固定结构 + 单测 |
| 网关透传与鉴权耦合 | 401 误判 | token 校验只查签名与过期，不查业务 |
| 端口冲突（电商 demo） | 启动失败 | 固定 28080/29101-29106，README 写明 |
