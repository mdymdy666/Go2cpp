# 抖音 P0 骨架 + 共享 IDL

> **完整需求文档见 [REQUIREMENTS.md](REQUIREMENTS.md)**（功能需求 FR-01..FR-10、数据模型、接口规范、
> 非功能指标、5 个验收场景、C2–C10 里程碑）。本 README 只描述骨架现状与构建/启动命令。

单机 6 服务 + 1 网关的最小骨架。**当前只实现 `GET /health`**，业务、存储、RPC 均未实现。
构建产物在 `build/`（已被 `.gitignore` 忽略），不提交。

## 端口表

| 进程 | 端口 | 职责 |
| --- | --- | --- |
| `gateway` | **28080** | 唯一对外入口，REST 聚合与鉴权 |
| `user_service` | **29101** | 注册、登录、用户资料 |
| `video_service` | **29102** | 视频发布、详情、我的视频 |
| `feed_service` | **29103** | 关注流 / 推荐流（首页刷视频） |
| `interaction_service` | **29104** | 点赞、评论、计数 |
| `live_service` | **29105** | 直播间创建、开播、关播 |
| `search_service` | **29106** | 用户 / 视频 / 直播搜索 |

已占用端口（本项目**未使用**）：8080、9101-9103（电商 demo）、18080/18081（nginx）、18099（av_demo）。

## 目录

```
app/douyin/
├── README.md
├── build.sh                 # 编译 7 个二进制
├── include/douyin_proto.hpp # 共享 IDL（结构 + ToJson/FromJson 声明）
├── include/douyin_http.hpp  # 骨架用最小 HTTP/1.1（基于 go2cpp::net::Socket）
└── src/
    ├── douyin_proto.cpp     # 手写最小 JSON 实现
    ├── user_service.cpp  video_service.cpp  feed_service.cpp
    ├── interaction_service.cpp  live_service.cpp  search_service.cpp
    └── gateway.cpp
```

## IDL 字段

- `User{int64 id; string name, email; int64 created_at;}`
- `Video{int64 id, user_id; string title, url; int64 created_at;}`
- `FeedItem{Video video; string author;}`
- `Comment{int64 id, video_id, user_id; string content; int64 created_at;}`
- `Like{int64 video_id, user_id;}`
- `Room{int64 id, user_id; string title; int status;}`（status 0=关播 1=直播中）
- `SearchResult{string type; int64 id; string title;}`（type ∈ user/video/live）

`douyin::ToJson(...)` / `douyin::FromJson(text, ...)` 已对 7 个结构各提供重载；
JSON 解析为手写最小实现（缺字段保留默认值），不引第三方库。

## 接口草案（REST，尚未实现）

| 方法 | 路径 | 归属 | 说明 |
| --- | --- | --- | --- |
| POST | `/api/user/register` | user | 注册，返回 User |
| GET | `/api/user/:id` | user | 用户资料 |
| POST | `/api/video` | video | 发布视频（title/url） |
| GET | `/api/video/:id` | video | 视频详情 |
| GET | `/api/feed?cursor=&limit=` | feed | 刷 feed，返回 FeedItem 列表 |
| POST | `/api/like` | interaction | 点赞（video_id/user_id） |
| POST | `/api/comment` | interaction | 发评论 |
| GET | `/api/comment/:video_id` | interaction | 评论列表 |
| POST | `/api/live/room` | live | 创建直播间 |
| POST | `/api/live/room/:id/start` | live | 开播 / 关播 |
| GET | `/api/search?q=&type=` | search | 搜索，返回 SearchResult 列表 |

**当前唯一可用路由**：`GET /health` → `200 {"ok":true,"service":"<name>"}`；
其余路径一律 `404 {"ok":false,"error":"not found"}`。

## 验收场景（P0 全量目标，当前仅第 0 项达成）

0. `GET /health` 7 个进程全部 200，body 含各自 service 名。
1. 注册：`POST /api/user/register` 拿到 `User.id`。
2. 发视频：带该 `user_id` 调 `POST /api/video`，返回 `Video.id`。
3. 刷 feed：`GET /api/feed` 中出现刚发布的视频，`author` 与注册名一致。
4. 点赞：`POST /api/like` 后视频点赞数 +1，重复点赞幂等。
5. 评论：`POST /api/comment` 后 `GET /api/comment/:video_id` 能查到该评论。

## 构建

```bash
cd app/douyin
./build.sh          # 默认读 $GO2CPP_ROOT(.build-bento-wsl)，可用环境变量覆盖
```

产物：`build/{gateway,user_service,video_service,feed_service,interaction_service,live_service,search_service}`。

## 启动

```bash
cd app/douyin
setsid nohup ./build/gateway 28080 > logs/gateway.log 2>&1 &
setsid nohup ./build/user_service 29101 > logs/user_service.log 2>&1 &
# video 29102 / feed 29103 / interaction 29104 / live 29105 / search 29106 同理

curl 127.0.0.1:28080/health   # {"ok":true,"service":"gateway"}
pkill -x user_service        # interaction_service 的 comm 被截断为 interaction_ser
# gateway 与 8080 电商 demo 同名，勿直接 pkill -x gateway；按 PID 或会话杀：
# pkill -x -s <gateway的会话ID> gateway
```
