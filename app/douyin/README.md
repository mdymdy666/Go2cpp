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

## 完整验收（C10）

> 本节是 C10 补充的当前状态说明。上文「骨架现状」「当前唯一可用路由」「验收场景（当前仅第 0 项达成）」
> 是 C0 时点的描述；六服务与网关的业务接口已按 C2–C8 落地，实际行为以本节、`REQUIREMENTS.md`
> 与脚本实跑结果为准。

### 1. 启动全部 7 个进程（28080 / 29101–29106）

```bash
cd app/douyin
mkdir -p logs
./build.sh                    # 7 个二进制已存在时可跳过

# 六服务共享一个库，启动时都要执行 PRAGMA journal_mode=WAL；
# 同时拉起会偶发 SQLITE_BUSY，所以每条之间错开 0.4s（见「已知限制」）。
start() { # 服务名 端口
  nohup "./build/$1" "$2" > "logs/$1.log" 2>&1 &
  echo $! > "logs/$1.pid"
  sleep 0.4
}
start gateway             28080
start user_service        29101
start video_service       29102
start feed_service        29103
start interaction_service 29104
start live_service        29105
start search_service      29106

for port in 28080 29101 29102 29103 29104 29105 29106; do
  curl -fsS "http://127.0.0.1:$port/health" && echo
done
```

- 六个服务默认共享同一个 SQLite 库 `data/douyin.db`（WAL）；可用 `DOUYIN_DB=/path/to.db` 指定。
- 端口是各服务 `main` 的第 1 个位置参数：`./build/user_service 39101` 即可换端口。
- 停止时按上面记录的 PID，不用 `pkill -f`：

```bash
for pid_file in logs/*.pid; do
  kill "$(cat "$pid_file")" 2>/dev/null && echo "stopped $pid_file"
  rm -f "$pid_file"
done
```

### 2. 一键验收（C9 交付）

```bash
./scripts/accept_all.sh          # 功能验收
./scripts/accept_all.sh --bench  # 功能验收后追加两轮压测
```

`scripts/accept_all.sh` 会自己拉起 6 个服务 + 网关，经网关 28080 跑注册、发视频、
feed 分页、点赞评论、直播、搜索等整套 REST 接口，输出 PASS/FAIL 并以退出码表示结果。
**跑之前先停掉手动启动的 7 个进程**（同上按 `logs/*.pid` 停止），否则 28080/29101–29106
端口冲突、脚本内启动的服务会失败。单服务验收脚本仍可用（各自起进程、独立临时库）：
`scripts/accept_user.sh`、`scripts/accept_video_feed.sh`、
`scripts/accept_interaction_search.sh`、`scripts/accept_live.sh`。

### 3. 压测（C9 交付）

```bash
./build/loadgen --url "http://127.0.0.1:28080/api/feed?limit=20" \
                --concurrency 32 --requests 2000
```

`scripts/loadgen.cpp` 是 C9 自研压测工具（无第三方依赖，每请求独立建连，与网关
「一连接一请求」的模型一致），统计 QPS、p50/p99 与非 200 数。参数：`--url`、`--concurrency`、
`--requests`、`--method`、`--body`，`--help` 查看完整用法；`build.sh` 会顺带构建 `build/loadgen`。

### 4. 可靠性验收（C10 交付）

```bash
./scripts/reliability_test.sh
```

流程：起 `user_service`(29101) + `video_service`(29102) 共用一个库 → 注册 alice、发 2 条视频 →
用 python3 标准库记录 `users`/`videos` 行数与库文件（db + wal + shm）字节数 →
`kill -9` 两个进程、等 1 秒重启 → 再查 alice 资料与两条视频详情（校验 `author` 为 alice、标题不变）→
比对重启前后快照 → 打印 PASS/FAIL 并以 0/1 退出。默认端口被占用时自动换到 +10000 起的临时端口；
清理只用脚本记录的 PID，不用 `pkill -f`；不要求安装 `sqlite3` 客户端（用 python3 标准库读取）。

C10 实跑：19 PASS / 0 FAIL，重启前后均为 `users=1`、`videos=2`、库文件合计 214056 字节。

### 5. 已知限制

- **SQLite 单写者**：六个服务共享一个 SQLite 库，WAL 仍只有单写者，写并发靠
  `busy_timeout=5000` 串行重试；写压力大时会遇到 SQLITE_BUSY，仅适合 demo，不是生产存储。
- **自写 HTTP 每连接一线程**：`include/douyin_http.hpp` 是 demo 用的最小 HTTP/1.1 实现，
  每条连接一个线程、无 keep-alive，并发连接数受线程与 fd 约束；压测数字不代表生产框架上限。
- **demo 不做转码/推荐**：视频只保存 URL，不落盘、不转码、不生成封面；feed「推荐流」是
  `ORDER BY like_count*2 + comment_count*3 + 0.1*created_at/3600` 的固定 SQL 热度公式，
  不是推荐算法；token 为 HMAC 自签，无刷新/吊销。
- **启动竞态**：六服务共享一个库，启动时每个进程都要执行 `PRAGMA journal_mode=WAL`；
  同时拉起会偶发 `SQLITE_BUSY` 使后启动的服务直接退出（C10 自检时 video_service 实测命中过），
  按上面的 0.4s 间隔启动即可规避。
