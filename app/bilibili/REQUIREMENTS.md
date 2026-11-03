# B 站核心复刻（弹幕 + 播放 + 会员）需求文档

| 项 | 内容 |
|---|---|
| 文档编号 | APP-BILIBILI-REQ-01 |
| 版本 | v1.0 |
| 日期 | 2026-11-03 |
| 状态 | 生效，作为 B1–B4 实现与验收依据 |
| 目录 | `app/bilibili/` |
| 定位 | 复刻 B 站核心商业闭环（弹幕/播放/大会员），验证 Go2Cpp 的 WebSocket、广播、存储、并发能力 |

---

## 1. 目标与范围

### 1.1 总目标

单机启动「用户 + 视频 + 弹幕 + 会员」四个服务（可合并为 2–3 个进程），跑通：

```
登录 → 看视频 → 发弹幕（实时广播）→ 拉历史弹幕 → 开通大会员 → 解锁 1080P
```

### 1.2 In Scope

| 项 | 说明 |
|---|---|
| 用户 | 登录/资料/大会员状态 |
| 视频 | 投稿（标题/封面/分P）、详情、播放地址 |
| 弹幕 | WebSocket 接入、按视频分房间、实时广播、历史弹幕分片、限流/屏蔽词 |
| 会员 | 模拟订阅（订单 + 状态），权益：1080P 清晰度 |
| 统计 | 播放数、弹幕数、在线观看数 |

### 1.3 Out of Scope（明确不做）

- 视频转码/转码集群（用现成 mp4 模拟，播放地址返回文件路径或分片清单）；
- 推荐算法（首页按投稿时间倒序）；
- 直播/电商/游戏联运/动态；
- 前端页面（只提供 REST + WS，验收用脚本）。

---

## 2. 角色

| 角色 | 能力 |
|---|---|
| 游客 | 看视频、看弹幕、拉历史弹幕 |
| 登录用户 | 发弹幕、投稿、看自己的视频 |
| 大会员 | 1080P 清晰度、弹幕特效（模拟字段） |

---

## 3. 功能需求

### FR-01 用户（user_service，端口 29201）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-01.1 | 注册/登录 | `POST /api/user/register`、`/api/user/login` | 返回 token；重复 409、错密码 401 |
| FR-01.2 | 资料 | `GET /api/user/:id` | 含 `vip` 状态（0/1）与到期时间 |
| FR-01.3 | 大会员开通 | `POST /api/user/:id/vip`（需 token，模拟支付） | 返回 vip=1 与到期时间；幂等 |

### FR-02 视频（video_service，端口 29202）

| 编号 | 需求 | 接口 | 验收 |
|---|---|---|---|
| FR-02.1 | 投稿 | `POST /api/video`（需 token） | 标题/封面/分P 列表；返回 id |
| FR-02.2 | 详情 | `GET /api/video/:id` | 含分P、播放数、弹幕数 |
| FR-02.3 | 我的视频 | `GET /api/video/mine`（需 token） | 倒序 |
| FR-02.4 | 播放地址 | `GET /api/video/:id/play?quality=` | 游客 720P；大会员可 1080P；非会员请求 1080P → 403 |

### FR-03 弹幕（danmaku_service，端口 29203）

| 编号 | 需求 | 接口/协议 | 验收 |
|---|---|---|---|
| FR-03.1 | 历史弹幕 | `GET /api/danmaku/:video_id?from=&to=` | 按时间片返回；空视频返回空列表 |
| FR-03.2 | 实时广播 | WS `/ws/danmaku?video_id=&token=` | 加入房间；新弹幕广播给同房间所有连接 |
| FR-03.3 | 发送弹幕 | WS 文本帧 `{"text":"...","time_ms":1234,"color":16777215}` | 长度 1–100、频率 ≤5 条/10s、屏蔽词过滤 |
| FR-03.4 | 房间统计 | WS 消息 `{"type":"online","count":N}` 或 REST | 在线数准确 |
| FR-03.5 | 断线 | WS 关闭后从房间移除，在线数 -1 | 不泄漏连接 |

### FR-04 会员（复用 user_service）

- 订阅记录 `memberships(user_id, expires_at, created_at)`；
- 权益：`GET /api/video/:id/play?quality=1080p` 时校验 `vip` 且未过期。

### FR-05 统计

- `GET /api/video/:id/stats` → `{views, danmaku, online}`；
- 播放数在 `GET /api/video/:id/play` 时 +1。

---

## 4. 数据模型（SQLite）

```sql
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  email TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  salt TEXT NOT NULL,
  vip_expires_at INTEGER NOT NULL DEFAULT 0,   -- 0=非会员
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS videos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  title TEXT NOT NULL,
  cover TEXT NOT NULL,
  views INTEGER NOT NULL DEFAULT 0,
  danmaku_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS video_parts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  video_id INTEGER NOT NULL,
  part_no INTEGER NOT NULL,
  title TEXT NOT NULL,
  url TEXT NOT NULL,            -- 复用现成 mp4 路径
  duration_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS danmaku (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  video_id INTEGER NOT NULL,
  user_id INTEGER NOT NULL,
  text TEXT NOT NULL,
  time_ms INTEGER NOT NULL,     -- 弹幕出现时间
  color INTEGER NOT NULL DEFAULT 16777215,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_danmaku_video_time ON danmaku(video_id, time_ms);

CREATE TABLE IF NOT EXISTS memberships (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
```

---

## 5. 接口与 WS 协议

### 5.1 REST（网关 28200 聚合，或直连各服务）

| 方法 | 路径 | 服务 | 鉴权 |
|---|---|---|---|
| POST | `/api/user/register` | user | 否 |
| POST | `/api/user/login` | user | 否 |
| GET | `/api/user/:id` | user | 否 |
| POST | `/api/user/:id/vip` | user | 是 |
| POST | `/api/video` | video | 是 |
| GET | `/api/video/:id` | video | 否 |
| GET | `/api/video/:id/play?quality=` | video | 否（1080P 需会员） |
| GET | `/api/video/mine` | video | 是 |
| GET | `/api/danmaku/:video_id?from=&to=` | danmaku | 否 |
| GET | `/api/video/:id/stats` | video | 否 |

### 5.2 WebSocket（`/ws/danmaku?video_id=&token=`）

客户端 → 服务端：

```json
{"type":"send","text":"前方高能","time_ms":1234,"color":16777215}
```

服务端 → 客户端：

```json
{"type":"danmaku","id":1,"user_id":3,"text":"...","time_ms":1234,"color":16777215,"created_at":...}
{"type":"online","count":12}
{"type":"error","code":429,"message":"发送过于频繁"}
```

约束：文本 1–100 字符；10 秒内最多 5 条；屏蔽词（可配置列表，默认 `["广告","加群"]`）；非法 JSON 回 error 不断连。

---

## 6. 非功能需求

| 类别 | 指标 | 验收 |
|---|---|---|
| 弹幕并发 | 单机 1000 条 WS 连接、10 房间 | WS 压测脚本 |
| 广播延迟 | 同房间 p99 < 50 ms | 时间戳统计 |
| 历史拉取 | 1 万条弹幕分片查询 p99 < 20 ms | 压测 |
| 播放 API | 1000 QPS、p99 < 50 ms | loadgen |
| 可靠性 | kill -9 danmaku_service 重启后历史弹幕仍在 | 手动验收 |
| 安全 | token 校验；屏蔽词；频率限制 | 用例 |

---

## 7. 验收场景（脚本 `scripts/accept_bili.sh`）

1. 注册 alice → 登录拿 token → 资料 vip=0；
2. alice 投稿 2 个视频（各 2 分P）→ 详情返回 2 分P；
3. 游客播放 720P → 200；请求 1080P → 403；
4. 开通大会员 → 再请求 1080P → 200；
5. 两条 WS 连接进同一房间 → A 发弹幕 → B 收到（广播）；
6. 拉历史弹幕 → 包含刚发的弹幕；
7. 频率限制：10 秒内发 6 条 → 第 6 条回 429。

---

## 8. 目录与技术选型

```
app/bilibili/
├── REQUIREMENTS.md
├── README.md
├── build.sh
├── include/
│   ├── bili_proto.hpp       # IDL
│   ├── bili_store.hpp       # SQLite（复用 storage::DB）
│   ├── bili_auth.hpp        # token（复用 douyin 方案）
│   └── bili_ws.hpp          # WS 房间管理（基于 network/websocket.hpp）
├── src/
│   ├── bili_store.cpp  bili_auth.cpp  bili_ws.cpp  bili_proto.cpp
│   ├── user_service.cpp  video_service.cpp  danmaku_service.cpp
│   └── gateway.cpp          # 复用 app::gateway 组件（可选）
├── scripts/
│   ├── accept_bili.sh       # REST 场景
│   └── ws_loadgen.cpp       # 弹幕并发压测
└── data/                    # SQLite（ignore）
```

端口：gateway 28200、user 29201、video 29202、danmaku 29203（避开抖音 28080/29101-29106）。

技术选型：WebSocket 用 `network/websocket.hpp`；存储复用 `storage::DB`；token 复用 HMAC-SHA256 方案；压测自研。

---

## 9. 里程碑

| 批次 | 内容 | 完成判据 |
|---|---|---|
| B1 | 存储层 + 用户（注册/登录/资料/大会员）+ 验收脚本骨架 | 场景 1、4 通过 |
| B2 | 视频（投稿/详情/分P/播放地址 + 会员清晰度） | 场景 2、3 通过 |
| B3 | 弹幕（WS 房间/广播/历史/限流/屏蔽词） | 场景 5、6、7 通过 |
| B4 | 压测（1000 WS 连接、广播延迟、历史 p99）+ 可靠性 + 文档 | §6 指标达标 |

---

## 10. 风险

| 风险 | 缓解 |
|---|---|
| WebSocket 实现细节（握手/帧） | 复用 `network/websocket.hpp`，先写 ping/pong 最小闭环 |
| 1000 连接线程模型 | 先每连接一线程验证功能，B4 再评估 EventLoop 化 |
| 广播风暴（同房间 1000 连接） | 有界发送队列 + 慢连接踢出 |
| SQLite 写入（弹幕高频） | WAL + 批量提交（每 10 条/100ms） |
