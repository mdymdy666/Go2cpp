# B 站弹幕 Web Demo（可视化验收）

单页演示：40 秒测试视频 + 按播放时间对齐的滚动弹幕 + 双窗口实时互发。

- 静态页：`web/index.html`（单文件，内联 CSS/JS，无外部 CDN）
- 视频：`web/demo.mp4`（40s / 1280x720 / H.264 + AAC，由 ffmpeg 生成，**不入库**）
- 后端：`build/danmaku_service`（`app/bilibili/`，端口 29203）
- 一键脚本：`scripts/demo_web.sh`

---

## 1. 一键启动

```bash
cd app/bilibili
bash scripts/demo_web.sh
```

脚本会：

1. 若 `web/demo.mp4` 不存在，用 ffmpeg 现场生成（40s，含 440Hz 音轨）；
2. 启动 `build/danmaku_service --port 29203 --db /tmp/bili_demo.db --rate-limit 100`；
3. 启动 `python3 -m http.server 8000 --directory web`；
4. 打印访问地址，`Ctrl+C` 同时停止两个进程（pid 文件 `logs/demo_web.*.pid`，**不使用 `pkill -f`**）。

启动输出示例：

```
[demo_web] 启动 danmaku_service :29203（db=/tmp/bili_demo.db rate-limit=100）
[demo_web] 启动静态站点 :8000（web/）
[demo_web] 就绪

============================================================
 B 站弹幕 Web Demo 已就绪
------------------------------------------------------------
 页面（推荐） : http://localhost:8000/
 页面（备用） : http://127.0.0.1:8000/
 WS 服务      : ws://localhost:29203/ws/danmaku?video_id=1
 ...
```

浏览器打开 **http://localhost:8000/** 即可（Windows 浏览器直接访问 WSL 的 localhost，无需端口转发）。

---

## 2. 验收步骤（两个窗口 A / B）

| 步骤 | 操作 | 预期 |
|---|---|---|
| 1 | 窗口 A 打开 `http://localhost:8000/` | 视频自动播放（静音循环），右上角状态 `已连接`，在线 `1` |
| 2 | 窗口 B 打开同一地址 | A、B 的在线数都变 `2` |
| 3 | A 输入「A 的第一条弹幕」→ 回车 | B 立刻在视频当前时间看到白色弹幕从右向左滚过；A 也收到回显 |
| 4 | B 把大小切「大」、速度切「快」、点粉色、发「B 的大号粉色弹幕」 | A 端出现大号、较快的粉色弹幕 |
| 5 | A 点「显示弹幕」取消勾选 | A 端立刻清屏且不再上屏；B 不受影响 |
| 6 | A 拖进度条到 20s | 弹幕按各自的 `time_ms` 重新对齐，不一次性刷屏 |
| 7 | 关闭 B | A 的在线数回落到 `1` |
| 8 | 视频循环回放（40s 后） | 弹幕重新按时间轴播放 |

> 弹幕的 `time_ms` 由发送端取 `video.currentTime * 1000`，接收端按自己的 `video.currentTime` 决定上屏时刻；若 `time_ms` 已过则立即上屏，因此「实时互发」永远可见。

---

## 3. 页面功能对照

| 需求 | 实现 |
|---|---|
| 视频 | `<video src="demo.mp4" controls autoplay muted loop>`，40s |
| 弹幕层 | 绝对定位覆盖视频区，`pointer-events:none`，不挡播放控件 |
| 时间对齐 | `requestAnimationFrame` 每帧读 `video.currentTime`，与 `time_ms` 比对 |
| 滚动 | 每帧按像素速度推进 `translateX`，随视频暂停而暂停 |
| 轨道 | 按容器高度分 8 条轨道，自动选空闲轨道，避免重叠 |
| 显示/隐藏 | 复选框，关闭时清屏且丢弃该时段弹幕 |
| 大小 | 小 16px / 中 22px / 大 30px |
| 速度 | 慢 13s / 中 8s / 快 5s 横穿屏幕（对已上屏弹幕同样即时生效） |
| 颜色 | 6 个预设色 + 系统取色器，发送时写进 `color` 字段 |
| 发送 | 输入框 + `发送` 按钮 + Enter 快捷键 |
| 连接状态 | `连接中… / 已连接 / 已断开，重连中…`，断线指数退避自动重连 |
| 在线数 | 服务端 `{"type":"online","count":N}` |
| 历史弹幕 | 打开页面 `GET /api/danmaku/1?limit=2000` 预加载，失败自动降级 |

---

## 4. 已知限制：历史弹幕的 CORS

`danmaku_service` 的 REST 响应**没有 `Access-Control-Allow-Origin` 头**，而页面在 `:8000`、API 在 `:29203`，属于跨域。浏览器会拦截该 `fetch`。

页面已做容错：捕获失败后左下角显示「历史弹幕不可用（服务端无 CORS 头）→ 仅显示实时弹幕」，**不阻塞**视频、WS 连接与收发弹幕。

需要历史弹幕时的三种办法（按推荐度）：

1. 给 `danmaku_service` 的 REST 响应加 CORS 头（改服务端，本 demo 未改）；
2. 在 `:8000` 前面加同源反向代理（如 nginx/caddy），把 `/api/` 转发到 `:29203`；
3. 仅本机演示：以 `--disable-web-security --user-data-dir=/tmp/chrome-demo` 启动 Chrome（有安全风险，别在日常浏览器上用）。

> WS 不受同源策略限制，且服务端未校验 `Origin`，所以 `ws://localhost:29203` 从 `:8000` 页面连接是可用的——这正是实时互发能工作的原因。

---

## 5. 手工启动（不用脚本）

```bash
cd app/bilibili
./build.sh                                              # 若尚未构建
./build/danmaku_service --port 29203 --db /tmp/bili_demo.db --rate-limit 100 &
python3 -m http.server 8000 --directory web
```

验收接口（在另一个终端）：

```bash
ffprobe -v error -show_entries format=duration -of csv=p=0 web/demo.mp4   # ≥30
curl -sI http://127.0.0.1:8000/                                           # 200
curl -sI http://127.0.0.1:8000/demo.mp4                                   # 200 + Content-Length
curl -s  http://127.0.0.1:29203/api/danmaku/1                             # JSON
```

---

## 6. 重置与排错

- 清空历史弹幕：`rm -f /tmp/bili_demo.db*` 后重启脚本；
- 端口被占用：`DANMAKU_PORT=29203 WEB_PORT=8000` 可换端口，但页面里 `SERVICE_PORT` 写死 29203，换端口需同步改 `web/index.html`；
- 页面连不上 WS：确认 `danmaku_service` 在跑（`curl -s http://127.0.0.1:29203/api/danmaku/1/online`），以及浏览器地址栏用的是 `localhost` 而非 `file://`；
- 自动播放被拦截：点一下播放键即可（弹幕时钟随 `currentTime` 走，不影响）。
