# 用户视角全链路审查报告（2026-11-06）

审查方式：按真实用户顺序用 curl 模拟浏览器（带 Bearer / Cookie、跟随跳转、检查 HTML 正文与文件实体），
每项都验证「页面内容是否合理」，而不是只看状态码。服务：gateway 28080 + 9 后端（29101-29109）。

## 一、14 项逐项结果

| # | 用户动作 | 结果 | 证据 |
|---|---|---|---|
| 1 | 打开首页 | ✅ | `GET /` 200/233 539 B；`/api/feed` 22 条；22 条 `url` + 22 张封面**逐个落盘校验全部存在**（`check_media_links.sh` 0 缺失） |
| 2 | 登录 | ✅ | `POST /api/user/login` 200，token 81 字符；错误密码 401 |
| 3 | 刷 feed + 点赞 | ✅ | 点赞 10→11、重复点赞幂等（`inserted:false`）、取消赞回 10；「喜欢」tab 同步 19 条 |
| 4 | 评论 | ✅ | 发评论 200，列表回显含 `author_name:"user0001"`；@补全 `type=user` 返回 user0009/0008/0007 |
| 5 | 搜索 | ✅ | `history`/`hot`/`suggest`/`search` 全 200 且结构合理；搜「猫」命中 2 条视频；空 q 400、无结果 200 空数组 |
| 6 | 个人主页三 tab | ✅ | works 0（真实空态）、liked 19、favorites 0（真实空态）；匿名/他人看 liked → `private:true` 且 items 为空（隐私正确） |
| 7 | 设置页 | ✅ | `GET /settings` 200/12 778 B；`PATCH /api/user/me` 改简介 200 并回读一致 |
| 8 | 消息 | ✅ | `GET /messages` 200/18 808 B；`/api/dm/conversations` 200（0 会话，页面有「未登录/选择会话」空态文案） |
| 9 | 直播 | ✅ | `GET /live` 200/29 998 B；`/api/live/rooms` 200（0 间房，空态文案）；`/api/live/gifts` 5 个礼物 |
| 10 | 创作中心 | ✅ | `GET /creator` 200/14 762 B；`overview`/`trend`/`top-videos` 全 200（user0001 无作品 → 全 0，合理空态） |
| 11 | 举报后台（admin） | ✅ | `GET /admin` 200；admin token 调 `/api/admin/reports` 200；**user0001 调 → 403**（修复后边界正确） |
| 12 | 分享落地页 | ✅ | `/v/1` `/v/3` 200；OG title 正确、`og:image` 尺寸 480×270 = 真实封面像素；`/v/99999` 404 且是友好「视频不存在」页 |
| 13 | 静态资源 | ✅ | demo1-5.mp4、demo1-5.jpg、`/covers/*`、`/static/robots.txt` 全 200 且 MIME 正确；Range 请求 206 |
| 14 | gzip | ✅ | 带 `Accept-Encoding: gzip` → 233 539 B 压到 65 854 B，`Content-Encoding: gzip`；identity 时不压缩 |

## 二、修复清单（现象 → 根因 → 修法）

1. **首页 8 条视频「视频加载失败」** → 仓库 `.gitignore` 忽略 `*.mp4`，但**没有任何脚本负责 `orig → demo*.mp4` 转码**，demo2/demo3 从未生成；seed 按 `i%5` 轮换写入 8 条死链，之前只做了软删掩盖 → 新增 `scripts/gen_demo_videos.sh`（缺则从 orig 转码，`--force` 可重建），接到 `run_all.sh` 启动前；补齐文件后**解除 8 条软删**（22 条视频全部可见，8 个原本「还没有作品」的主页恢复内容）。
2. **验收查不出死链** → `accept_*.sh` 只校验 JSON/状态码，从不校验文件实体 → 新增 `scripts/check_media_links.sh`（逐条校验 `url`/`cover_url` 落盘存在）；`seed_cloud.sh` 发布前加 5 个素材存在性门禁。
3. **admin 登不进举报后台，普通用户反而有权限** → admin 白名单缺省写死 `user_id=1`，而 seed 先建 30 个普通用户、最后才建 admin（id=31） → `run_all.sh` 里按邮箱从 DB 解析 admin id 并 `export DOUYIN_ADMIN_USER_IDS`。
4. **分享页评论全是「用户 #1」** → SSR 侧 `ShareComment` 无昵称字段、从未填；JS 追加侧也硬编码 → 加 `author_name`，`video_service` 按 user_id 去重回填。
5. **首页/分享页评论区同样全是「用户 #id」** → 评论列表接口不返回昵称 → `Comment.author_name` + `ToJson` 输出 + interaction 服务去重回填 + 两处前端优先显示昵称。
6. **分享卡片图片变形** → `og:image:width/height` 写死 720×1280，实际封面 480×270 → 新增 `ReadJpegSize()` 读 JPEG SOF 段，按真实像素输出（读不到就不输出）。
7. **demo3 封面纯黑（968 B）且开头 3 秒全黑** → 原始素材带淡入 → 转码从第 4 秒起截 10 秒，封面随之重抽。
8. **播放失败提示是开发者文案** → toast 写「检查 /videos 静态路由」 → 改为「这条视频暂时无法播放，滑到下一条看看」，同一条只提示一次。

## 三、用户能看见但未修（时间/范围所限）

- **直播 0 间房、私信 0 会话**：页面有兜底空态，但演示站「点进去什么都没有」，需要种子数据（`gifts` 有 5 条，`rooms`/`dm_conversations` 为空）。
- **搜索热搜/历史初始为空**：`hot` 取近 7 天真实搜索聚合，新库必然空；页面显示「暂无热搜，搜点什么吧」。
- **举报列表 0 条**：`reports` 表为空，后台统计全 0（结构正确，缺演示数据）。
- **点赞状态只存 localStorage**：`/api/feed` 不返回 `liked`，换设备/清缓存后已赞视频显示未赞（服务端 `likes` 有记录却不回传）。
- **仓库根目录存在 `app/douyin\r/`**（目录名含回车）：某次脚本写入路径错误留下的垃圾目录，与用户功能无关。

## 四、复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp/app/douyin
bash scripts/check_media_links.sh          # 媒体实体自检（0 缺失）
bash scripts/gen_demo_videos.sh            # 缺则补 demo 视频
bash scripts/run_all.sh stop && bash scripts/run_all.sh
curl -s localhost:28080/api/feed?limit=50 | head -c 400
```
