# douyin UI E2E 报告（清单 A ④⑤）

日期：2026-11-07 ｜ 环境：WSL Ubuntu 24.04，Chromium（Playwright Python）
入口：`bash app/douyin/scripts/run_ui_tests.sh`（起服务 → 跑 E2E → 停服务 → 摘要）

## 结果

- 用例：**42 个**，**42 passed / 0 failed**（连续跑 2 次结果一致）
- 覆盖页面：首页、/v/:id、/settings、/live、/creator、/messages、/admin、/douyin/ 前缀、
  通知面板、个人主页（10 个功能区，9 个独立页面）
- 断言口径：用户可见内容（文本/可见性/`readyState>=2`/`naturalWidth>0`）、控制台无
  error、无 4xx/5xx、刷新后持久化、先查再操作（幂等）

## 用例清单（按文件）

| 文件 | 数量 | 覆盖 |
|---|---|---|
| `test_home.py` | 8 | feed 渲染/首屏视频真加载/点赞刷新持久化/无自动滚动/移动端视口/小幅滚轮/搜索下拉/关注 tab 空态 |
| `test_share.py` | 4 | 视频可播/OG 标签且 og:image 真加载/不存在 id 404/评论区有数据或空态 |
| `test_settings.py` | 5 | 打开并回填资料/改名成功并恢复/重名 409 提示/简介超长拒绝/未登录跳回首页 |
| `test_live.py` | 4 | 打开/房间列表或空态/聊天发送并刷新持久化/礼物面板有礼物 |
| `test_creator.py` | 4 | 未登录提示/4 卡片数字与 API 一致/趋势图 7 天 21 柱与峰值/TOP 列表或空态 |
| `test_dm.py` | 4 | 打开/会话列表或空态/发消息刷新持久化/筛选空态 |
| `test_admin.py` | 4 | 非 admin 403 提示/admin 列表有数字/处理举报（confirm）/状态筛选 |
| `test_public.py` | 3 | `/douyin/` 首页 200 且首屏视频可播/`/douyin/v/1` OG/`/douyin/api/feed` 有数据 |
| `test_profile.py` | 6 | 通知面板 6 tab/通知列表空态/全部已读刷新后仍读/主页三 tab 切换/关注粉丝抽屉（含关闭）/收藏空态 |

## 发现的 bug（4 个，全部已修）

1. **创作者中心 4 个卡片全 0、作品数 0**（`douyin_creator_page.hpp`）
   现象：接口 `overview` 返回 14 播放/10 赞/4 评论/1 作品，页面却显示 0/0/0/0、作品 0 个。
   根因：`api('overview').then(renderOverview)` 把 `{ok, overview:{...}}` 整包当 overview，
   字段全 undefined→0。修复：先取 `data.overview` 再渲染。

2. **个人主页的「关注/粉丝」抽屉关不掉**（`douyin_home_page.hpp`）
   现象：从主页点关注/粉丝打开抽屉后，✕ 按钮可见但点不动（被主页拦截）。
   根因：`.notif-panel` z-index 65 < `.profile-view` 70，抽屉被主页盖住。
   修复：`#followPanel { z-index: 76; }`。

3. **`/favicon.ico` 404 导致所有页面控制台报错**（`gateway.cpp`）
   现象：分享页等页面控制台出现 404，违反「控制台无 error」。
   根因：浏览器自动请求站点图标，网关无此路由。修复：新增 `FaviconMatcher` 返回 204。

4. **公网前缀 `/douyin/` 全部 404**（`gateway.cpp`）
   现象：`/douyin/`、`/douyin/v/1`、`/douyin/api/feed` 均 404（本地与未剥前缀部署）。
   根因：基座 Dispatch 先匹配路由再跑中间件，前缀无法在中间件层剥离；且隧道
   `--no-strip` / 反向代理会保留前缀。修复：路由注册时补 `/douyin` 前缀别名 +
   自定义 Matcher 前缀感知 + 中间件归一化 target（保留 query）。

## 已知限制（未修）

- 举报接口的 JSON 解析不支持 `\uXXXX` 转义：外部客户端用转义中文提交会入库成
  `u5783...`。浏览器 `JSON.stringify` 发 UTF-8 原始中文，UI 路径不受影响。
- `reports` 有 `UNIQUE(reporter,target)`：同一用户重复举报同一对象不会重置为
  pending，admin「处理举报」用例每次消耗一个新的 (reporter,target) 组合。
- 公网隧道客户端未常驻，`run_ui_tests.sh` 用本地 `/douyin/` 前缀验证等价行为。

**基线修复**：原 `test_fav_consistency.py`（1 failed）依赖对收藏切换接口的状态假设，已重写为
「先查再操作 + UI 断言」并并入 `test_profile.py`/`test_home.py`；旧零散用例文件已删除。