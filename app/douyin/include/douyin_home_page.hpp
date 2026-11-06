/**
 * @file douyin_home_page.hpp
 * @brief 抖音首页（全屏竖向 feed + 评论面板 + 用户主页）的内联 HTML 常量。
 *
 * @details 页面完全自包含：内联 CSS / JS，不引用任何外部 CDN、字体或图片。
 *          所有接口请求都用相对路径（`api/...`、`videos/...`），因此同一份页面
 *          在本机 `http://host:28080/` 与公网 `http://host:8443/douyin/` 前缀下
 *          都能工作。视频源按 `(id % 3) + 1` 映射到 `videos/1..3.mp4`，
 *          由网关的同源静态路由直接返回，不依赖数据库里的 url 字段。
 *
 *          交互模型（对齐抖音的「一屏一条 + 右侧操作栏」）：
 *          - `#feed` 是 `scroll-snap-type: y mandatory` 的全屏滚动容器，
 *            每个 `.slide` 宽 100%、高 `100dvh`（`100vh` 兜底）、
 *            `scroll-snap-align: start`；
 *          - 视频 `position:absolute; inset:0; width/height:100%` 铺满整屏，
 *            默认 `object-fit:contain`（完整显示留黑边），底部按钮可切
 *            `cover`（铺满裁切），选择写入 localStorage；
 *          - 底部信息与右侧操作栏均绝对定位叠加在视频上，`z-index` 更高，
 *            由半透明渐变遮罩保证可读性；
 *          - `IntersectionObserver`（阈值 0.6）只播放当前屏视频、暂停其它；
 *          - 底部进度条随 `timeupdate` 更新，pointer 拖动 seek 到 `pct * duration`；
 *          - 双击视频任意处点赞（`POST api/like`）+ 心形 keyframes 动画，
 *            已点赞只播动画不重复请求；单击挂起 240ms 以区分双击；
 *          - `preload="none"` + 同名 `videos/<name>.jpg` 封面：首屏只拉缩略图，
 *            视频字节等真正播放/拖动时才加载；
 *          - 评论图标打开右侧抽屉，`GET api/comment/<id>?limit=50&sort=` 拉列表
 *            （最新/最热切换、置顶标签与作者置顶按钮），登录后 `POST api/comment`
 *            发送并插入列表头、同步计数；输入 `@` 弹用户补全下拉；
 *          - 作者名/头像打开用户主页，`GET api/user/<id>` + `api/feed?limit=100`
 *            前端按 `user_id` 过滤出该用户视频（最多 30 条），可返回 feed。
 *
 * @note HTML 用 R"HTML(...)HTML" 原始字符串承载；页面内不得再出现 `)HTML`
 *       序列，否则会提前结束字面量。
 *
 * @author douyin demo
 * @date 2026-11-03
 * @copyright MIT License
 */

#ifndef DOUYIN_HOME_PAGE_HPP
#define DOUYIN_HOME_PAGE_HPP

namespace douyin {

/// @brief 首页 HTML（抖音风格全屏 feed 单页）；只读常量，进程生命周期内有效。
inline constexpr const char* kHomePageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>Go2Cpp 抖音 Demo</title>
<style>
  :root {
    --pink: #fe2c55;
    --cyan: #25f4ee;
    --bg: #08080d;
    --panel: #14141c;
    --line: rgba(255, 255, 255, .1);
    --text: #f4f5fa;
    --muted: #9a9dad;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; padding: 0; height: 100%; }
  body {
    background: #000;
    color: var(--text);
    font: 15px/1.6 -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
          "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
    -webkit-font-smoothing: antialiased;
    overflow: hidden;
  }
  [hidden] { display: none !important; }
  button { font: inherit; cursor: pointer; border: 0; }

  /* ---------- 顶部栏（悬浮在视频上） ---------- */
  .topbar {
    position: fixed; inset: 0 0 auto 0; z-index: 40;
    display: flex; align-items: center; gap: 12px; flex-wrap: wrap;
    padding: 10px 18px 22px;
    background: linear-gradient(to bottom, rgba(0, 0, 0, .78), rgba(0, 0, 0, 0));
    pointer-events: none;
  }
  .topbar > * { pointer-events: auto; }
  .brand {
    font-size: 18px; font-weight: 800; letter-spacing: .3px;
    background: linear-gradient(92deg, var(--cyan), #fff 45%, var(--pink));
    -webkit-background-clip: text; background-clip: text; color: transparent;
    white-space: nowrap;
  }
  .brand small {
    display: block; font-size: 10px; font-weight: 500; letter-spacing: 1.2px;
    color: var(--muted); -webkit-text-fill-color: var(--muted); margin-top: -2px;
  }
  .auth { display: flex; align-items: center; gap: 8px; margin-left: auto; flex-wrap: wrap; }
  .auth input {
    width: 168px; padding: 7px 11px;
    background: rgba(20, 20, 28, .82); color: var(--text);
    border: 1px solid var(--line); border-radius: 9px;
    font-size: 13px; outline: none; transition: border-color .16s, box-shadow .16s;
  }
  .auth input:focus { border-color: rgba(37, 244, 238, .55); box-shadow: 0 0 0 3px rgba(37, 244, 238, .12); }
  .auth input[type=password] { width: 118px; }
  .upload-btn { white-space: nowrap; }
  .btn-primary {
    padding: 7px 17px; font-size: 13px; font-weight: 700; color: #fff;
    border-radius: 9px; background: linear-gradient(92deg, var(--pink), #ff5c7a);
    transition: filter .16s, transform .12s;
  }
  .btn-primary:hover { filter: brightness(1.08); }
  .btn-primary:active { transform: scale(.97); }
  .btn-ghost {
    padding: 7px 13px; font-size: 13px; color: var(--muted);
    background: transparent; border: 1px solid var(--line); border-radius: 9px;
  }
  .btn-ghost:hover { color: var(--text); border-color: rgba(255, 255, 255, .24); }
  .who { display: none; align-items: center; gap: 8px; font-size: 13px; color: var(--muted); }
  .who .dot { width: 7px; height: 7px; border-radius: 50%; background: #3ddc97; box-shadow: 0 0 8px #3ddc97; }
  .who b { color: var(--text); font-weight: 600; }
  .signed-in .who { display: flex; }
  .signed-in .login-only { display: none; }

  /* ---------- 全屏竖向 feed ---------- */
  .feed {
    height: 100vh; height: 100dvh;
    overflow-y: scroll; overflow-x: hidden;
    scroll-snap-type: y mandatory;
    overscroll-behavior-y: contain;
    scrollbar-width: none;
    -webkit-overflow-scrolling: touch;
  }
  .feed::-webkit-scrollbar { display: none; }
  /* 每个 slide 就是一屏：尺寸跟随视口（100dvh 处理移动端地址栏伸缩，
     不支持的浏览器回退到 100vh）。不写死像素高度，窗口缩放由浏览器重排。 */
  .slide {
    position: relative;
    width: 100%;
    height: 100vh; height: 100dvh;
    overflow: hidden;
    background: #000;
    scroll-snap-align: start;
    scroll-snap-stop: always;
  }
  /* 视频绝对定位铺满 slide：inset:0 + 宽高 100% 让视频随窗口同步缩放。
     默认 object-fit:contain（完整显示，必要时留黑边）；加 .cover 后为
     object-fit:cover（铺满容器，超出部分裁切），两种模式由底部按钮切换。 */
  .slide .main-video {
    position: absolute; inset: 0; z-index: 0;
    width: 100%; height: 100%;
    display: block;
    object-fit: contain; background: #000;
    cursor: pointer;
  }
  .slide .main-video.cover { object-fit: cover; }

  /* ---------- 播放进度条（底部细条 + 可拖动 seek） ---------- */
  /* 命中区高 20px、可见轨道仅 3px：手指/鼠标都好按，视觉上仍是细条。
     hover / 拖动 / 键盘聚焦时才显示圆形拖柄与时间，避免遮挡视频。 */
  .progress {
    position: absolute; left: 0; right: 0; bottom: 0; z-index: 6;
    height: 20px; display: flex; align-items: flex-end;
    cursor: pointer; touch-action: none; outline: none;
  }
  .progress .progress-rail {
    position: absolute; left: 0; right: 0; bottom: 0;
    height: 3px; background: rgba(255, 255, 255, .24);
    transition: height .15s ease;
  }
  .progress .progress-fill {
    width: 0%; height: 100%;
    background: linear-gradient(90deg, var(--cyan), var(--pink));
  }
  .progress .progress-thumb {
    position: absolute; bottom: -4px; left: 0;
    width: 11px; height: 11px; border-radius: 50%;
    background: #fff; box-shadow: 0 1px 6px rgba(0, 0, 0, .6);
    transform: translateX(-50%); opacity: 0;
    transition: opacity .15s ease, transform .15s ease;
    pointer-events: none;
  }
  .progress:hover .progress-rail, .progress.dragging .progress-rail,
  .progress:focus-visible .progress-rail { height: 5px; }
  .progress:hover .progress-thumb, .progress.dragging .progress-thumb,
  .progress:focus-visible .progress-thumb { opacity: 1; }
  .progress.dragging .progress-thumb { transform: translateX(-50%) scale(1.25); }
  .progress .progress-time {
    position: absolute; right: 12px; bottom: 12px;
    padding: 2px 7px; border-radius: 6px;
    font-size: 11px; font-variant-numeric: tabular-nums; color: #fff;
    background: rgba(0, 0, 0, .62); white-space: nowrap;
    opacity: 0; pointer-events: none; transition: opacity .15s ease;
  }
  .progress:hover .progress-time, .progress.dragging .progress-time,
  .progress:focus-visible .progress-time { opacity: 1; }

  /* ---------- 双击点赞的心形动画 ---------- */
  .heart-burst {
    position: absolute; z-index: 7; left: 50%; top: 50%;
    font-size: 92px; line-height: 1; color: var(--pink);
    transform: translate(-50%, -50%) scale(.3);
    opacity: 0; pointer-events: none; user-select: none;
    filter: drop-shadow(0 8px 26px rgba(254, 44, 85, .55));
    animation: heart-pop .82s cubic-bezier(.2, .7, .3, 1) forwards;
  }
  @keyframes heart-pop {
    0%   { opacity: 0; transform: translate(-50%, -50%) scale(.25) rotate(-14deg); }
    18%  { opacity: 1; transform: translate(-50%, -50%) scale(1.18) rotate(6deg); }
    38%  { transform: translate(-50%, -50%) scale(.94) rotate(-2deg); }
    68%  { opacity: 1; transform: translate(-50%, -50%) scale(1) rotate(0deg); }
    100% { opacity: 0; transform: translate(-50%, -74%) scale(1.32) rotate(0deg); }
  }
  @media (prefers-reduced-motion: reduce) {
    .heart-burst { animation-duration: .01ms; }
  }

  /* 半透明渐变遮罩：底部保证信息区可读，右侧保证操作栏可读；z-index 高于视频 */
  .slide .shade {
    position: absolute; inset: 0; z-index: 2; pointer-events: none;
    background:
      linear-gradient(to top, rgba(0, 0, 0, .88) 0%, rgba(0, 0, 0, .22) 34%, rgba(0, 0, 0, 0) 56%),
      linear-gradient(to left, rgba(0, 0, 0, .42) 0%, rgba(0, 0, 0, 0) 26%);
  }
  .pause-hint {
    position: absolute; inset: 0; z-index: 3; display: grid; place-items: center;
    pointer-events: none; opacity: 0; transition: opacity .2s ease;
  }
  .slide:not(.playing) .pause-hint { opacity: 1; }
  .pause-hint i {
    width: 66px; height: 66px; border-radius: 50%;
    display: grid; place-items: center;
    background: rgba(255, 255, 255, .9); color: #111;
    font-size: 24px; font-style: normal; padding-left: 5px;
    box-shadow: 0 10px 30px rgba(0, 0, 0, .55);
  }

  /* 底部信息：absolute 叠加在视频上，z-index 高于视频与遮罩 */
  .meta {
    position: absolute; left: 18px; right: 96px; bottom: 26px; z-index: 4;
    padding-bottom: env(safe-area-inset-bottom);
    text-shadow: 0 1px 8px rgba(0, 0, 0, .8);
  }
  .meta .title {
    font-size: 18px; font-weight: 700; line-height: 1.4;
    margin: 0 0 9px; max-width: 100%;
    display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical;
    overflow: hidden;
  }
  .author-row { display: flex; align-items: center; gap: 9px; }
  .avatar {
    flex: 0 0 auto;
    width: 38px; height: 38px; border-radius: 50%;
    display: grid; place-items: center;
    background: linear-gradient(135deg, var(--cyan), var(--pink));
    color: #08080d; font-size: 16px; font-weight: 800;
    user-select: none;
  }
  .author-name {
    font-size: 14px; font-weight: 700; color: #fff;
    cursor: pointer; padding: 0; background: transparent;
  }
  .author-name:hover { text-decoration: underline; }
  .author-meta { font-size: 12px; color: rgba(255, 255, 255, .58); }
  .stats { margin-top: 7px; display: flex; gap: 16px; font-size: 12.5px; color: rgba(255, 255, 255, .72); }
  .stats b { color: #fff; font-weight: 600; }
  /* 画幅切换：完整（contain，留黑边）/ 铺满（cover，裁切） */
  .fit-btn {
    margin-top: 10px;
    display: inline-flex; align-items: center;
    padding: 4px 11px; border-radius: 999px;
    font-size: 12px; font-weight: 600; line-height: 1.5;
    color: rgba(255, 255, 255, .86);
    background: rgba(0, 0, 0, .42);
    border: 1px solid rgba(255, 255, 255, .26);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
    text-shadow: none;
    transition: background .16s, border-color .16s;
  }
  .fit-btn:hover { background: rgba(0, 0, 0, .64); border-color: rgba(255, 255, 255, .46); }
  .fit-btn[aria-pressed="true"] { border-color: rgba(37, 244, 238, .6); color: #d9fffd; }

  /* 右侧操作栏：absolute 叠加在视频上，z-index 高于视频与遮罩 */
  .rail {
    position: absolute; right: 12px; bottom: calc(128px + env(safe-area-inset-bottom));
    z-index: 5;
    display: flex; flex-direction: column; align-items: center; gap: 20px;
  }
  .rail .avatar { width: 46px; height: 46px; font-size: 19px; border: 2px solid #fff; }
  .rail button {
    display: flex; flex-direction: column; align-items: center; gap: 2px;
    background: transparent; color: #fff; font-size: 11.5px; padding: 0;
  }
  .rail .ico {
    font-size: 30px; line-height: 1; font-style: normal;
    filter: drop-shadow(0 2px 7px rgba(0, 0, 0, .65));
    transition: transform .14s ease;
  }
  .rail button:active .ico { transform: scale(.88); }
  .rail .like-btn.liked { color: var(--pink); }
  .rail .like-btn.liked .ico { color: var(--pink); }
  .rail .num { font-variant-numeric: tabular-nums; }
  .rail .fav-btn.faved { color: #ffd479; }
  .rail .fav-btn.faved .ico { color: #ffd479; }

  /* ---------- 分享面板（底部弹出） ---------- */
  .share-mask {
    position: fixed; inset: 0; z-index: 80;
    background: rgba(0, 0, 0, .55);
  }
  .share-sheet {
    position: fixed; left: 50%; bottom: 0; z-index: 81;
    width: min(520px, 100%); transform: translateX(-50%);
    padding: 16px 18px calc(16px + env(safe-area-inset-bottom));
    background: var(--panel); border: 1px solid var(--line);
    border-radius: 18px 18px 0 0;
    box-shadow: 0 -20px 60px -24px rgba(0, 0, 0, .95);
  }
  .ss-head { display: flex; align-items: center; gap: 10px; margin-bottom: 14px; }
  .ss-head b {
    flex: 1; font-size: 14px;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .ss-close {
    width: 30px; height: 30px; border-radius: 50%;
    background: #23232e; color: var(--text); font-size: 14px; line-height: 1;
  }
  .ss-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }
  .ss-item {
    display: flex; flex-direction: column; align-items: center; gap: 6px;
    padding: 12px 4px; color: var(--text); font-size: 12px;
    background: #1b1b25; border: 1px solid var(--line); border-radius: 12px;
  }
  .ss-item:hover { border-color: rgba(255, 255, 255, .24); }
  .ss-ico { font-size: 22px; line-height: 1; }
  .ss-qr { margin-top: 14px; text-align: center; }
  .ss-qr p { margin: 8px 0 0; font-size: 11.5px; color: var(--muted); }
  .qr-box {
    display: grid; width: 132px; height: 132px; margin: 0 auto;
    grid-template-columns: repeat(21, 1fr); grid-template-rows: repeat(21, 1fr);
    background: #fff; padding: 5px; border-radius: 6px;
  }
  .qr-box i { display: block; }
  .qr-box i.on { background: #0a0a10; }
  .ss-cancel {
    width: 100%; margin-top: 14px; padding: 11px; font-size: 13.5px;
    color: var(--text); background: #23232e; border-radius: 12px;
  }

  /* ---------- 举报 / 拉黑 / 黑名单（第 3 轮） ---------- */
  .menu-list { display: flex; flex-direction: column; gap: 8px; }
  .menu-item {
    padding: 12px 14px; font-size: 13.5px; color: var(--text);
    background: #1b1b25; border: 1px solid var(--line); border-radius: 12px;
    text-align: left;
  }
  .menu-item:hover { border-color: rgba(255, 255, 255, .24); }
  .menu-item.danger { color: #ff8fa3; border-color: rgba(254, 44, 85, .35); }
  .report-reasons { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 12px; }
  .reason {
    padding: 7px 12px; font-size: 12.5px; color: var(--muted);
    background: #1b1b25; border: 1px solid var(--line); border-radius: 999px;
  }
  .reason.on {
    color: #fff; border-color: rgba(254, 44, 85, .7);
    background: rgba(254, 44, 85, .16);
  }
  .report-detail {
    width: 100%; min-height: 76px; resize: vertical; padding: 10px 12px;
    background: #1e1e29; color: var(--text); border: 1px solid var(--line);
    border-radius: 10px; font: inherit; font-size: 13px; outline: none;
  }
  .report-detail:focus { border-color: rgba(254, 44, 85, .55); }
  .report-submit {
    width: 100%; margin-top: 12px; padding: 11px; font-size: 13.5px;
    border-radius: 12px;
  }
  .block-row {
    display: flex; align-items: center; gap: 11px;
    padding: 12px 4px; border-bottom: 1px solid var(--line);
  }
  .block-row .avatar { width: 38px; height: 38px; font-size: 16px; }
  .block-info { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .block-info b {
    font-size: 13.5px; overflow: hidden; text-overflow: ellipsis;
    white-space: nowrap;
  }
  .block-info span { font-size: 11.5px; color: var(--muted); }
  .block-unblock {
    padding: 6px 12px; font-size: 12px; color: var(--text);
    background: #23232e; border: 1px solid var(--line); border-radius: 999px;
    white-space: nowrap;
  }
  .block-unblock:hover { border-color: rgba(254, 44, 85, .5); color: #ff8fa3; }
  .cp-more-btn {
    margin-left: 8px; padding: 0 6px; font-size: 13px; line-height: 1;
    color: var(--muted); background: transparent; border-radius: 6px;
  }
  .cp-more-btn:hover { color: var(--text); background: rgba(255, 255, 255, .08); }
  .p-more-btn { padding: 7px 12px; font-size: 13px; }

  /* ---------- toast ---------- */
  .toast {
    position: fixed; left: 50%; top: 74px; z-index: 90;
    transform: translate(-50%, -14px);
    max-width: min(90vw, 460px);
    padding: 10px 18px; border-radius: 10px;
    font-size: 13.5px; color: #fff;
    background: rgba(28, 28, 38, .96);
    border: 1px solid rgba(255, 255, 255, .1);
    box-shadow: 0 16px 40px -12px rgba(0, 0, 0, .8);
    opacity: 0; pointer-events: none;
    transition: opacity .2s ease, transform .2s ease;
  }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  .toast.err { border-color: rgba(254, 44, 85, .6); }
  .toast.ok { border-color: rgba(61, 220, 151, .55); }

  /* ---------- 评论面板（右侧抽屉） ---------- */
  .backdrop {
    position: fixed; inset: 0; z-index: 55;
    background: rgba(0, 0, 0, .48);
    opacity: 0; pointer-events: none; transition: opacity .24s ease;
  }
  .backdrop.show { opacity: 1; pointer-events: auto; }
  .comment-panel {
    position: fixed; top: 0; right: 0; bottom: 0; z-index: 60;
    width: min(400px, 100%);
    display: flex; flex-direction: column;
    background: var(--panel);
    border-left: 1px solid var(--line);
    box-shadow: -24px 0 60px -30px rgba(0, 0, 0, .95);
    transform: translateX(102%);
    transition: transform .26s cubic-bezier(.4, 0, .2, 1);
  }
  .comment-panel.open { transform: translateX(0); }
  .cp-head {
    display: flex; align-items: center; gap: 10px;
    padding: 14px 16px; border-bottom: 1px solid var(--line);
  }
  .cp-head .cp-titles { min-width: 0; flex: 1; }
  .cp-head b { display: block; font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .cp-head span { font-size: 12px; color: var(--muted); }
  .cp-sort {
    display: inline-flex; flex: 0 0 auto; padding: 2px;
    border-radius: 8px; background: #23232e;
  }
  .cp-sort button {
    padding: 4px 9px; border-radius: 6px;
    font-size: 12px; color: var(--muted);
  }
  .cp-sort button.on { color: #fff; background: rgba(254, 44, 85, .92); }
  .cp-pin {
    display: inline-block; margin-left: 6px; padding: 0 5px;
    border-radius: 5px; font-size: 10.5px; font-weight: 700; color: #fff;
    background: linear-gradient(92deg, var(--pink), #ff5c7a);
    vertical-align: 1px;
  }
  .cp-pin-btn { margin-left: 6px; font-size: 11.5px; color: var(--muted); }
  .cp-pin-btn:hover { color: var(--pink); }
  .cp-close {
    width: 32px; height: 32px; border-radius: 50%;
    background: #23232e; color: var(--text); font-size: 15px; line-height: 1;
  }
  .cp-list { flex: 1; overflow-y: auto; padding: 4px 16px 16px; }
  .cp-item { padding: 11px 0; border-bottom: 1px solid var(--line); }
  .cp-item:last-child { border-bottom: 0; }
  .cp-who { font-size: 12px; color: var(--muted); margin-bottom: 3px; }
  .cp-who b { color: #cfd2de; font-weight: 600; }
  .cp-txt { font-size: 14px; line-height: 1.55; word-break: break-word; }
  .cp-empty { padding: 48px 16px; text-align: center; color: var(--muted); font-size: 13.5px; }
  .cp-form {
    position: relative;
    display: flex; gap: 8px; padding: 12px 16px calc(12px + env(safe-area-inset-bottom));
    border-top: 1px solid var(--line); background: #171721;
  }
  .mention-menu {
    position: absolute; left: 16px; right: 16px; bottom: calc(100% - 2px);
    z-index: 5; overflow: hidden;
    background: #23232e; border: 1px solid var(--line); border-radius: 10px;
    box-shadow: 0 -14px 34px -14px rgba(0, 0, 0, .9);
  }
  .mention-item {
    display: block; width: 100%; padding: 9px 12px;
    text-align: left; font-size: 13px; color: var(--text);
  }
  .mention-item:hover, .mention-item.active { background: rgba(254, 44, 85, .16); }
  .mention-empty { padding: 10px 12px; font-size: 12.5px; color: var(--muted); }
  .cp-form input {
    flex: 1; min-width: 0; padding: 10px 12px;
    background: #1e1e29; color: var(--text);
    border: 1px solid var(--line); border-radius: 10px;
    font-size: 13.5px; outline: none;
  }
  .cp-form input:focus { border-color: rgba(254, 44, 85, .55); }
  .cp-form button {
    padding: 0 18px; font-size: 13px; font-weight: 700; color: #fff;
    border-radius: 10px; background: linear-gradient(92deg, var(--pink), #ff5c7a);
  }
  .cp-form button:disabled { opacity: .5; cursor: default; }

  /* ---------- 消息通知（顶部铃铛 + 右侧抽屉） ---------- */
  .bell {
    position: relative; padding: 6px 10px; font-size: 16px; line-height: 1;
    color: var(--text); background: rgba(20, 20, 28, .82);
    border: 1px solid var(--line); border-radius: 9px;
    transition: border-color .16s;
  }
  .bell:hover { border-color: rgba(255, 255, 255, .24); }
  .bell-badge {
    position: absolute; top: -6px; right: -6px;
    min-width: 17px; height: 17px; padding: 0 4px;
    font-size: 10px; font-weight: 700; line-height: 17px; text-align: center;
    color: #fff; background: var(--pink); border-radius: 999px;
    box-shadow: 0 0 0 2px rgba(0, 0, 0, .5);
  }
  .notif-panel {
    position: fixed; top: 0; right: 0; bottom: 0; z-index: 65;
    width: min(420px, 100%);
    display: flex; flex-direction: column;
    background: var(--panel);
    border-left: 1px solid var(--line);
    box-shadow: -24px 0 60px -30px rgba(0, 0, 0, .95);
    transform: translateX(102%);
    transition: transform .26s cubic-bezier(.4, 0, .2, 1);
  }
  .notif-panel.open { transform: translateX(0); }
  .notif-item {
    display: flex; gap: 11px; align-items: flex-start;
    padding: 12px 4px; border-bottom: 1px solid var(--line);
    cursor: pointer; border-radius: 10px;
    transition: background .15s;
  }
  .notif-item:hover { background: rgba(255, 255, 255, .04); }
  .notif-item.unread .notif-line { color: #fff; font-weight: 600; }
  .notif-item.unread::before {
    content: ''; width: 7px; height: 7px; margin-top: 7px; flex: none;
    border-radius: 50%; background: var(--pink);
    box-shadow: 0 0 8px rgba(254, 44, 85, .8);
  }
  .notif-ico {
    width: 30px; height: 30px; flex: none; display: grid; place-items: center;
    font-size: 15px; background: #23232e; border-radius: 50%;
  }
  .notif-body { min-width: 0; flex: 1; }
  .notif-line { font-size: 13.5px; line-height: 1.5; color: #d5d7e2; word-break: break-word; }
  .notif-meta {
    margin-top: 3px; font-size: 11.5px; color: var(--muted);
    display: flex; gap: 8px; flex-wrap: wrap;
  }
  .notif-quote {
    max-width: 100%; overflow: hidden; text-overflow: ellipsis;
    white-space: nowrap;
  }
  .notif-more {
    margin: 10px 16px calc(14px + env(safe-area-inset-bottom));
    padding: 9px; font-size: 13px; color: var(--muted);
    background: #23232e; border: 1px solid var(--line); border-radius: 10px;
  }
  .notif-more:hover { color: var(--text); }

  /* ---------- 顶部搜索框与搜索结果页（用户 / 视频 / 话题） ---------- */
  .nav-search { display: flex; align-items: center; gap: 6px; }
  .nav-search input {
    width: 200px; padding: 7px 11px;
    background: rgba(20, 20, 28, .82); color: var(--text);
    border: 1px solid var(--line); border-radius: 9px;
    font-size: 13px; outline: none;
  }
  .nav-search input:focus {
    border-color: rgba(254, 44, 85, .55);
    box-shadow: 0 0 0 3px rgba(254, 44, 85, .14);
  }
  #searchView { z-index: 68; }
  #searchView .profile-top { gap: 10px; }
  #searchView .profile-top input {
    flex: 1; min-width: 0; padding: 9px 12px;
    background: #1e1e29; color: var(--text);
    border: 1px solid var(--line); border-radius: 10px;
    font-size: 14px; outline: none;
  }
  #searchView .profile-top input:focus { border-color: rgba(37, 244, 238, .5); }
  .search-tabs {
    display: flex; gap: 8px; padding: 12px 18px 0;
    max-width: 820px; margin: 0 auto;
  }
  .search-tab {
    padding: 7px 16px; font-size: 13.5px; color: var(--muted);
    background: transparent; border: 1px solid var(--line);
    border-radius: 999px;
  }
  .search-tab.active {
    color: #fff; border-color: transparent; font-weight: 700;
    background: linear-gradient(92deg, var(--pink), #ff5c7a);
  }
  .search-row {
    display: flex; align-items: center; gap: 12px; width: 100%;
    padding: 12px 0; text-align: left; color: var(--text);
    background: transparent; border: 0; border-bottom: 1px solid var(--line);
  }
  .search-row:hover { background: rgba(255, 255, 255, .03); }
  .search-avatar {
    flex: 0 0 44px; width: 44px; height: 44px; border-radius: 50%;
    display: flex; align-items: center; justify-content: center;
    font-weight: 700; color: #fff;
    background: linear-gradient(135deg, #2f2f3d, #4a4a5e);
  }
  .search-avatar.tag { background: linear-gradient(135deg, var(--cyan), var(--pink)); }
  .search-cover {
    flex: 0 0 64px; width: 64px; height: 44px; border-radius: 8px;
    display: flex; align-items: center; justify-content: center;
    background: #232330; color: var(--muted); font-size: 15px;
  }
  .search-main { min-width: 0; flex: 1; display: block; }
  .search-main b {
    display: block; font-size: 14px; font-weight: 600;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .search-main span { display: block; font-size: 12px; color: var(--muted); }
  .search-empty {
    padding: 40px 0; text-align: center;
    color: var(--muted); font-size: 13.5px;
  }

  /* ---------- 用户主页 ---------- */
  .profile-view {
    position: fixed; inset: 0; z-index: 70;
    overflow-y: auto;
    background: radial-gradient(1100px 560px at 50% -12%, #1b1430 0%, var(--bg) 58%) fixed;
  }
  .profile-top {
    position: sticky; top: 0; z-index: 3;
    display: flex; align-items: center; gap: 12px;
    padding: 13px 18px;
    background: rgba(10, 10, 16, .9);
    backdrop-filter: blur(14px);
    border-bottom: 1px solid var(--line);
  }
  .profile-top h2 { margin: 0; font-size: 15px; font-weight: 700; }
  .profile-top .back {
    padding: 7px 14px; font-size: 13px; color: var(--text);
    background: #1e1e29; border: 1px solid var(--line); border-radius: 9px;
  }
  .profile-top .back:hover { background: #262633; }
  .profile-body { max-width: 820px; margin: 0 auto; padding: 20px 16px 70px; }
  .profile-card {
    display: flex; align-items: center; gap: 16px;
    padding: 20px; background: var(--panel);
    border: 1px solid var(--line); border-radius: 18px;
  }
  .profile-card .avatar { width: 66px; height: 66px; font-size: 28px; }
  .profile-card .p-info { min-width: 0; }
  .profile-card h3 { margin: 0 0 4px; font-size: 20px; font-weight: 800; }
  .profile-card .p-line { font-size: 13px; color: var(--muted); word-break: break-all; }
  .vip {
    display: inline-block; margin-top: 7px; padding: 2px 9px;
    font-size: 11.5px; border-radius: 999px;
    color: #ffd479; background: rgba(255, 196, 79, .12);
    border: 1px solid rgba(255, 196, 79, .35);
  }
  .vip.off { color: var(--muted); background: rgba(255, 255, 255, .05); border-color: var(--line); }
  .profile-grid {
    display: grid; grid-template-columns: repeat(auto-fill, minmax(152px, 1fr));
    gap: 12px; margin-top: 18px;
  }
  .pv-card {
    padding: 0; text-align: left; color: var(--text);
    background: var(--panel); border: 1px solid var(--line);
    border-radius: 14px; overflow: hidden;
    transition: transform .16s ease, border-color .16s ease;
  }
  .pv-card:hover { transform: translateY(-2px); border-color: rgba(255, 255, 255, .2); }
  .pv-card video {
    display: block; width: 100%; aspect-ratio: 9 / 16;
    object-fit: cover; background: #000; pointer-events: none;
  }
  .pv-title {
    display: block; padding: 8px 10px; font-size: 12.5px;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .profile-note { margin-top: 16px; color: var(--muted); font-size: 13.5px; }
  /* ---------- 主页三 tab：作品 / 喜欢 / 收藏 ---------- */
  .p-tabs {
    display: flex; gap: 4px; margin-top: 18px;
    border-bottom: 1px solid var(--line);
  }
  .p-tab {
    flex: 1 1 0; padding: 10px 6px; font-size: 14px; font-weight: 700;
    color: var(--muted); background: transparent; border: 0;
    border-bottom: 2px solid transparent; cursor: pointer;
  }
  .p-tab:hover { color: var(--text); }
  .p-tab.on { color: var(--text); border-bottom-color: #fe2c55; }
  .p-tab b { margin-left: 5px; font-size: 12px; font-weight: 600; color: var(--muted); }
  .p-panel { margin-top: 16px; }
  .p-hint {
    padding: 30px 18px; text-align: center; color: var(--muted);
    font-size: 14px; line-height: 1.8;
    background: rgba(255, 255, 255, .03);
    border: 1px dashed var(--line); border-radius: 14px;
  }
  .p-hint .lock { display: block; font-size: 26px; margin-bottom: 6px; }
  .p-load-more {
    display: block; width: 100%; margin-top: 14px; padding: 11px;
    font-size: 13.5px; color: var(--text); background: #1e1e29;
    border: 1px solid var(--line); border-radius: 11px; cursor: pointer;
  }
  .p-load-more:hover { background: #262633; }

  /* ---------- 话题页 ---------- */
  .topic-link { color: #7fd6ff; text-decoration: none; }
  .topic-link:hover { text-decoration: underline; }
  .tag-chips { display: flex; flex-wrap: wrap; gap: 8px; margin: 0 0 10px; }
  .tag-chip {
    display: inline-block; padding: 3px 10px; border-radius: 999px;
    font-size: 12px; font-weight: 600; color: #fff; text-decoration: none;
    background: rgba(255, 255, 255, .14); border: 1px solid rgba(255, 255, 255, .2);
  }
  .tag-chip:hover { background: rgba(255, 255, 255, .28); }
  .tag-hero {
    padding: 22px; margin-bottom: 18px; border-radius: 18px;
    background: linear-gradient(120deg, rgba(254, 44, 85, .18), rgba(37, 244, 238, .14));
    border: 1px solid var(--line);
  }
  .tag-hero h3 { margin: 0 0 8px; font-size: 26px; font-weight: 800; }
  .tag-hero .tag-meta { font-size: 13px; color: var(--muted); }

  /* ---------- 视频工具行：画幅 + 声音 ---------- */
  .tools { margin-top: 10px; display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
  .tools .fit-btn { margin-top: 0; }
  .sound-btn {
    display: inline-flex; align-items: center; gap: 4px;
    padding: 4px 11px; border-radius: 999px;
    font-size: 12px; font-weight: 600; line-height: 1.5;
    color: rgba(255, 255, 255, .86);
    background: rgba(0, 0, 0, .42);
    border: 1px solid rgba(255, 255, 255, .26);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
    text-shadow: none;
  }
  .sound-btn[aria-pressed="true"] {
    border-color: rgba(37, 244, 238, .6); color: #d9fffd;
  }
  .vol { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--muted); }
  .vol input { width: 72px; accent-color: var(--pink); }
  @media (max-width: 640px) { .vol input { width: 52px; } }

  /* ---------- 评论二级回复 ---------- */
  .cp-replies {
    margin: 7px 0 0 10px; padding-left: 10px;
    border-left: 2px solid rgba(255, 255, 255, .12);
  }
  .cp-replies[hidden] { display: none; }
  .cp-reply { padding: 5px 0; }
  .cp-reply .cp-who { margin-bottom: 1px; font-size: 11.5px; }
  .cp-reply .cp-txt { font-size: 13px; }
  .cp-more {
    margin-top: 6px; padding: 4px 10px; border-radius: 8px;
    font-size: 12px; color: #9fe9e5; background: rgba(37, 244, 238, .1);
    border: 1px solid rgba(37, 244, 238, .25);
  }
  .cp-reply-btn {
    margin-left: 8px; padding: 0; font-size: 12px; color: var(--muted);
    background: transparent;
  }
  .cp-reply-btn:hover { color: var(--text); }
  /* 评论点赞：默认灰，已赞变粉；与视频点赞同一套视觉语言。 */
  .cp-like {
    margin-left: 8px; padding: 0; font-size: 12px; line-height: 1;
    color: var(--muted); background: transparent;
  }
  .cp-like:hover { color: var(--text); }
  .cp-like.on { color: var(--pink); }
  .cp-like .cl-heart { display: inline-block; transition: transform .15s ease; }
  .cp-like.on .cl-heart { transform: scale(1.15); }
  .cp-reply-form { display: flex; gap: 6px; margin: 8px 0 2px 10px; }
  .cp-reply-form input {
    flex: 1; min-width: 0; padding: 6px 10px; font-size: 13px;
    background: #1e1e29; color: var(--text);
    border: 1px solid var(--line); border-radius: 8px; outline: none;
  }
  .cp-reply-form input:focus { border-color: rgba(254, 44, 85, .55); }
  .cp-reply-form button { padding: 0 12px; font-size: 12px; border-radius: 8px; }
  .cp-reply-form .send { color: #fff; background: linear-gradient(92deg, var(--pink), #ff5c7a); }
  .cp-reply-form .cancel { color: var(--muted); background: #23232e; }
  .cp-reply-form button:disabled { opacity: .5; cursor: default; }

  /* ---------- 主页关注按钮与数据 ---------- */
  .p-stats { display: flex; gap: 22px; margin: 10px 0 2px; font-size: 12.5px; color: var(--muted); }
  .p-stats b { display: block; font-size: 17px; color: var(--text); font-weight: 700; }
  .p-actions { display: flex; gap: 8px; margin-top: 12px; }
  .follow-btn {
    padding: 8px 22px; border-radius: 999px;
    font-size: 13.5px; font-weight: 700; color: #fff;
    background: linear-gradient(92deg, var(--pink), #ff5c7a);
  }
  .follow-btn.following {
    color: var(--text); background: #23232e; border: 1px solid var(--line);
  }
  .follow-btn:disabled { opacity: .6; cursor: default; }
  .p-self-tag {
    display: inline-block; margin-top: 8px; padding: 3px 10px;
    font-size: 12px; color: var(--muted);
    background: rgba(255, 255, 255, .06); border: 1px solid var(--line);
    border-radius: 999px;
  }

  /* 骨架屏 / 空态 */
  .skeleton {
    height: 100vh; height: 100dvh;
    background: linear-gradient(100deg, #101017 20%, #1c1c26 40%, #101017 60%);
    background-size: 220% 100%; animation: shimmer 1.3s infinite linear;
  }
  @keyframes shimmer { to { background-position: -220% 0; } }
  .notice {
    margin: 18px; padding: 44px 20px; text-align: center;
    color: var(--muted); font-size: 14px;
    background: var(--panel); border: 1px dashed var(--line); border-radius: 16px;
  }
  .notice button { margin-top: 14px; }

  @media (max-width: 640px) {
    .topbar { padding: 8px 12px 18px; gap: 8px; }
    .brand { font-size: 15px; }
    .brand small { display: none; }
    .auth input { width: 122px; padding: 6px 9px; }
    .auth input[type=password] { width: 88px; }
    .meta { left: 14px; right: 86px; bottom: 22px; }
    .meta .title { font-size: 16px; }
    .fit-btn { margin-top: 8px; padding: 4px 10px; font-size: 11.5px; }
    .rail { right: 8px; bottom: calc(118px + env(safe-area-inset-bottom)); }
    .comment-panel { width: 100%; }
    .profile-card { flex-direction: column; text-align: center; }
  }
</style>
</head>
<body>
<header class="topbar" id="topbar">
  <div class="brand">Go2Cpp 抖音<small>FEED · PLAY · LIKE · COMMENT</small></div>
  <div class="nav-search">
    <input id="navSearch" type="search" placeholder="搜索用户 / 视频 / 话题"
           autocomplete="off" spellcheck="false">
    <button class="btn-primary" id="navSearchBtn" type="button">搜索</button>
  </div>
  <div class="auth">
    <button class="bell" id="notifBtn" type="button" title="消息通知"
            aria-label="消息通知" style="display:none">🔔<span class="bell-badge"
            id="notifBadge" hidden>0</span></button>
    <button class="btn-ghost" id="blacklistBtn" type="button" title="黑名单管理"
            style="display:none">🚫 黑名单</button>
    <span class="who" id="who" title="点击进入我的主页"><span class="dot"></span><b id="whoName"></b></span>
    <input class="login-only" id="email" type="email" value="admin@demo.local"
           placeholder="邮箱" autocomplete="username" spellcheck="false">
    <input class="login-only" id="password" type="password" value="123456"
           placeholder="密码" autocomplete="current-password">
    <button class="btn-primary login-only" id="loginBtn" type="button">登录</button>
    <span class="vol" title="视频音量">
      <span id="volIcon">🔊</span>
      <input id="volumeSlider" type="range" min="0" max="100" value="100" aria-label="音量">
    </span>
    <input type="file" id="uploadInput" accept="video/mp4" hidden>
    <button class="btn-ghost upload-btn" id="uploadBtn" type="button">上传</button>
    <button class="btn-ghost mine-btn" id="mineBtn" type="button" style="display:none">我的主页</button>
    <button class="btn-ghost fav-btn-top" id="favBtn" type="button" style="display:none">⭐ 我的收藏</button>
    <button class="btn-ghost" id="logoutBtn" type="button" style="display:none">退出</button>
  </div>
</header>

<div class="toast" id="toast" role="status" aria-live="polite"></div>

<main class="feed" id="feed" aria-label="推荐视频流"></main>

<div class="backdrop" id="backdrop"></div>

<div class="share-mask" id="shareMask" hidden></div>
<div class="share-sheet" id="shareSheet" hidden role="dialog" aria-modal="true" aria-label="分享">
  <div class="ss-head">
    <b id="ssTitle">分享</b>
    <button class="ss-close" id="ssClose" type="button" aria-label="关闭分享">✕</button>
  </div>
  <div class="ss-grid" id="ssGrid">
    <button class="ss-item" data-act="copy" type="button"><span class="ss-ico">🔗</span><span>复制链接</span></button>
    <button class="ss-item" data-act="qr" type="button"><span class="ss-ico">📱</span><span>二维码</span></button>
    <button class="ss-item" data-act="wx" type="button"><span class="ss-ico">💬</span><span>微信好友</span></button>
    <button class="ss-item" data-act="wxline" type="button"><span class="ss-ico">🧭</span><span>朋友圈</span></button>
  </div>
  <div class="ss-qr" id="ssQr" hidden>
    <div class="qr-box" id="qrBox" aria-hidden="true"></div>
    <p>扫码打开这条视频（演示占位，未接入真正二维码编码）</p>
  </div>
  <button class="ss-cancel" id="ssCancel" type="button">关闭</button>
</div>
<aside class="comment-panel" id="comment-panel" aria-label="评论面板" aria-hidden="true">
  <div class="cp-head">
    <div class="cp-titles">
      <b id="cpTitle">评论</b>
      <span id="cpCount">0 条评论</span>
    </div>
    <div class="cp-sort" id="cpSort" role="tablist" aria-label="评论排序">
      <button type="button" data-sort="new" class="on" role="tab" aria-selected="true">最新</button>
      <button type="button" data-sort="hot" role="tab" aria-selected="false">最热</button>
    </div>
    <button class="cp-close" id="cpClose" type="button" aria-label="关闭评论">✕</button>
  </div>
  <div class="cp-list" id="cpList"><div class="cp-empty">加载中…</div></div>
  <form class="cp-form" id="cpForm">
    <div class="mention-menu" id="mentionMenu" hidden></div>
    <input id="cpInput" type="text" maxlength="500" placeholder="说点什么…" autocomplete="off">
    <button id="cpSend" type="submit">发送</button>
  </form>
</aside>

<aside class="notif-panel" id="notifPanel" aria-label="消息通知" aria-hidden="true">
  <div class="cp-head">
    <div class="cp-titles">
      <b>消息通知</b>
      <span id="notifSub">全部已读</span>
    </div>
    <button class="btn-ghost" id="notifAllRead" type="button">全部已读</button>
    <button class="cp-close" id="notifClose" type="button" aria-label="关闭通知">✕</button>
  </div>
  <div class="cp-list" id="notifList"><div class="cp-empty">加载中…</div></div>
  <button class="notif-more" id="notifMore" type="button" hidden>加载更多</button>
</aside>

<div class="share-mask" id="menuMask" hidden></div>
<div class="share-sheet" id="menuSheet" hidden role="dialog" aria-modal="true" aria-label="更多操作">
  <div class="ss-head">
    <b id="menuTitle">更多操作</b>
    <button class="ss-close" id="menuClose" type="button" aria-label="关闭菜单">✕</button>
  </div>
  <div class="menu-list" id="menuList"></div>
  <button class="ss-cancel" id="menuCancel" type="button">取消</button>
</div>

<div class="share-mask" id="reportMask" hidden></div>
<div class="share-sheet" id="reportModal" hidden role="dialog" aria-modal="true" aria-label="举报">
  <div class="ss-head">
    <b id="reportTitle">举报</b>
    <button class="ss-close" id="reportClose" type="button" aria-label="关闭举报">✕</button>
  </div>
  <div class="report-reasons" id="reportReasons">
    <button class="reason on" data-reason="广告营销" type="button">广告营销</button>
    <button class="reason" data-reason="色情低俗" type="button">色情低俗</button>
    <button class="reason" data-reason="人身攻击" type="button">人身攻击</button>
    <button class="reason" data-reason="谣言不实" type="button">谣言不实</button>
    <button class="reason" data-reason="违法违规" type="button">违法违规</button>
    <button class="reason" data-reason="其它" type="button">其它</button>
  </div>
  <textarea class="report-detail" id="reportDetail" maxlength="500"
            placeholder="补充说明（选填，最多 500 字）"></textarea>
  <button class="btn-primary report-submit" id="reportSubmit" type="button">提交举报</button>
  <button class="ss-cancel" id="reportCancel" type="button">取消</button>
</div>

<aside class="notif-panel" id="blockPanel" aria-label="黑名单管理" aria-hidden="true">
  <div class="cp-head">
    <div class="cp-titles">
      <b>黑名单管理</b>
      <span id="blockSub">0 人</span>
    </div>
    <button class="cp-close" id="blockClose" type="button" aria-label="关闭黑名单">✕</button>
  </div>
  <div class="cp-list" id="blockList"><div class="cp-empty">加载中…</div></div>
</aside>

<section class="profile-view" id="profileView" hidden aria-label="用户主页">
  <div class="profile-top">
    <button class="back" id="profileBack" type="button">← 返回</button>
    <h2>用户主页</h2>
  </div>
  <div class="profile-body" id="profileBody"></div>
</section>

<section class="profile-view" id="favView" hidden aria-label="我的收藏">
  <div class="profile-top">
    <button class="back" id="favBack" type="button">← 返回</button>
    <h2>⭐ 我的收藏</h2>
  </div>
  <div class="profile-body" id="favBody"></div>
</section>

<section class="profile-view" id="tagView" hidden aria-label="话题页">
  <div class="profile-top">
    <button class="back" id="tagBack" type="button">← 返回</button>
    <h2 id="tagHead">话题</h2>
  </div>
  <div class="profile-body" id="tagBody"></div>
</section>
<section class="profile-view" id="searchView" hidden aria-label="搜索结果">
  <div class="profile-top">
    <button class="back" id="searchBack" type="button">← 返回</button>
    <input id="searchInput" type="search" placeholder="搜索用户 / 视频 / 话题"
           autocomplete="off" spellcheck="false">
    <button class="btn-primary" id="searchSubmit" type="button">搜索</button>
  </div>
  <div class="search-tabs" id="searchTabs">
    <button class="search-tab active" data-tab="user" type="button">用户</button>
    <button class="search-tab" data-tab="video" type="button">视频</button>
    <button class="search-tab" data-tab="tag" type="button">话题</button>
  </div>
  <div class="profile-body" id="searchBody"></div>
</section>


<script>
(function () {
  'use strict';

  /* 计算页面根前缀：本机 `/`、公网 `/douyin/`、直达链接 `/v/12` 或
     `/douyin/v/12` 都归一到页面所在目录。所有 api/videos 请求都拼这个前缀，
     因此 `/v/<id>` 直达页里的相对路径不会错位（不再靠重定向补尾斜杠）。 */
  var BASE = (function () {
    var p = location.pathname;
    var m = p.match(/^(.*)\/v\/\d+\/?$/);
    if (m) return m[1] + '/';
    if (p.slice(-1) !== '/' && !/\.[a-z0-9]+$/i.test(p)) return p + '/';
    return p;
  })();

  var TOKEN_KEY = 'douyin_token';
  var USER_KEY = 'douyin_user';
  var LIKES_KEY = 'douyin_likes';
  var FIT_KEY = 'douyin_fit';
  var MUTE_KEY = 'douyin_muted';
  var VOL_KEY = 'douyin_volume';

  var state = {
    token: localStorage.getItem(TOKEN_KEY) || '',
    user: null,
    likes: {},
    favs: {},
    items: [],
    observer: null,
    activeVideo: null,
    panelVideoId: '',
    panelSlide: null,
    panelOwnerId: '',
    commentSort: 'new',
    profileUserId: '',
    profileView: null,
    fit: 'contain',
    muted: true,
    volume: 1,
    deepLink: ''
  };
  try {
    state.user = JSON.parse(localStorage.getItem(USER_KEY) || 'null');
    state.likes = JSON.parse(localStorage.getItem(LIKES_KEY) || '{}') || {};
  } catch (e) {
    state.user = null;
    state.likes = {};
  }
  /* 画幅持久化：cover=铺满裁切，其余一律按默认 contain（完整显示） */
  state.fit = localStorage.getItem(FIT_KEY) === 'cover' ? 'cover' : 'contain';
  /* 声音偏好：默认静音（浏览器自动播放策略要求），点过「开声音」后记住。 */
  state.muted = localStorage.getItem(MUTE_KEY) !== '0';
  var savedVol = Number(localStorage.getItem(VOL_KEY));
  state.volume = isFinite(savedVol) && savedVol >= 0 ? Math.min(savedVol, 1) : 1;
  /* 直达链接：/v/<id> 或 ?video=<id> */
  var deepPath = location.pathname.match(/\/v\/(\d+)\/?$/);
  var deepQuery = '';
  try { deepQuery = new URLSearchParams(location.search).get('video') || ''; } catch (e) {}
  state.deepLink = deepPath ? deepPath[1] : (/^\d+$/.test(deepQuery) ? deepQuery : '');

  var feedEl = document.getElementById('feed');
  var toastEl = document.getElementById('toast');
  var toastTimer = 0;
  var panelEl = document.getElementById('comment-panel');
  var backdropEl = document.getElementById('backdrop');
  var cpList = document.getElementById('cpList');
  var cpTitle = document.getElementById('cpTitle');
  var cpCount = document.getElementById('cpCount');
  var cpInput = document.getElementById('cpInput');
  var cpSend = document.getElementById('cpSend');
  var cpSort = document.getElementById('cpSort');
  var mentionMenu = document.getElementById('mentionMenu');
  var profileEl = document.getElementById('profileView');
  var profileBody = document.getElementById('profileBody');
  var tagEl = document.getElementById('tagView');
  var searchEl = document.getElementById('searchView');
  var searchBody = document.getElementById('searchBody');
  var searchInput = document.getElementById('searchInput');
  var navSearch = document.getElementById('navSearch');
  var searchTabs = document.getElementById('searchTabs');
  var searchState = { q: '', tab: 'user', data: null };

  var tagBody = document.getElementById('tagBody');
  var tagHead = document.getElementById('tagHead');
  var favViewEl = document.getElementById('favView');
  var favBody = document.getElementById('favBody');
  var shareMask = document.getElementById('shareMask');
  var shareSheet = document.getElementById('shareSheet');
  var ssTitle = document.getElementById('ssTitle');
  var ssQr = document.getElementById('ssQr');
  var qrBox = document.getElementById('qrBox');
  var ssGrid = document.getElementById('ssGrid');
  var uploadBtn = document.getElementById('uploadBtn');
  var uploadInput = document.getElementById('uploadInput');

  /* ---------- 基础工具 ---------- */

  function toast(message, kind) {
    toastEl.textContent = message;
    toastEl.className = 'toast show' + (kind ? ' ' + kind : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.className = 'toast'; }, 2600);
  }

  function fmt(n) {
    n = Number(n) || 0;
    if (n >= 10000) return (n / 10000).toFixed(1).replace(/\.0$/, '') + 'w';
    if (n >= 1000) return (n / 1000).toFixed(1).replace(/\.0$/, '') + 'k';
    return String(n);
  }

  function fmtTime(ts) {
    var n = Number(ts);
    if (!n) return '刚刚';
    var d = new Date(n * 1000);
    var p = function (x) { return x < 10 ? '0' + x : '' + x; };
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
           ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
  }

  function initial(name) {
    var s = String(name || '?').trim();
    return s ? s.charAt(0).toUpperCase() : '?';
  }

  /* 画幅按钮文案：显示当前模式（完整 / 铺满），点击切换 */
  function fitLabel() {
    return '画幅：' + (state.fit === 'cover' ? '铺满' : '完整');
  }

  /* 切换所有 feed 视频的画幅模式并持久化；新插入的 slide 在 buildSlide 里套用 */
  function setFit(mode) {
    state.fit = mode === 'cover' ? 'cover' : 'contain';
    try { localStorage.setItem(FIT_KEY, state.fit); } catch (e) {}
    feedEl.querySelectorAll('.main-video').forEach(function (v) {
      v.classList.toggle('cover', state.fit === 'cover');
    });
    feedEl.querySelectorAll('.fit-btn').forEach(function (btn) {
      btn.textContent = fitLabel();
      btn.setAttribute('aria-pressed', state.fit === 'cover' ? 'true' : 'false');
    });
    toast(state.fit === 'cover' ? '画幅：铺满（可能裁切）' : '画幅：完整（保留黑边）');
  }

  /* 相对路径：/douyin/ 前缀下解析为 /douyin/api/...，本机解析为 /api/... */
  function api(path, options) {
    options = options || {};
    var headers = { 'Accept': 'application/json' };
    if (options.body !== undefined) headers['Content-Type'] = 'application/json';
    if (state.token) headers['Authorization'] = 'Bearer ' + state.token;
    return fetch(BASE + path, {
      method: options.method || 'GET',
      headers: headers,
      body: options.body === undefined ? undefined : JSON.stringify(options.body)
    }).then(function (res) {
      return res.text().then(function (text) {
        var data = null;
        try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
        if (!res.ok || !data || data.ok === false) {
          var err = new Error((data && data.error) || ('HTTP ' + res.status));
          err.status = res.status;
          err.data = data;
          throw err;
        }
        return data;
      });
    });
  }

  function setSignedIn(user) {
    var topbar = document.getElementById('topbar');
    var whoName = document.getElementById('whoName');
    var logoutBtn = document.getElementById('logoutBtn');
    var isIn = !!(state.token && user);
    topbar.classList.toggle('signed-in', isIn);
    logoutBtn.style.display = isIn ? '' : 'none';
    whoName.textContent = isIn ? (user.name || user.email || '已登录') : '';
    cpInput.placeholder = isIn ? '说点什么…' : '登录后参与评论';
    var mineBtn = document.getElementById('mineBtn');
    if (mineBtn) mineBtn.style.display = isIn ? '' : 'none';
    var favBtn = document.getElementById('favBtn');
    if (favBtn) favBtn.style.display = isIn ? '' : 'none';
    var notifBtn = document.getElementById('notifBtn');
    if (notifBtn) notifBtn.style.display = isIn ? '' : 'none';
    var blacklistBtn = document.getElementById('blacklistBtn');
    if (blacklistBtn) blacklistBtn.style.display = isIn ? '' : 'none';
    if (isIn) {
      refreshUnreadNotifications();
    } else {
      notifState.unread = 0;
      paintNotifBadge();
      closeNotifications();
      closeBlacklist();
    }
    loadFavorites();
  }

  function clearAuth() {
    state.token = '';
    state.user = null;
    localStorage.removeItem(TOKEN_KEY);
    localStorage.removeItem(USER_KEY);
    setSignedIn(null);
  }

  function handleAuthError(err) {
    if (err && (err.status === 401 || err.status === 403)) {
      clearAuth();
      toast('登录已失效，请重新登录', 'err');
      return true;
    }
    return false;
  }

  /* ---------- 登录 / 退出 ---------- */

  function login() {
    var email = document.getElementById('email').value.trim();
    var password = document.getElementById('password').value;
    var btn = document.getElementById('loginBtn');
    if (!email || !password) { toast('请输入邮箱和密码', 'err'); return; }
    btn.disabled = true;
    btn.textContent = '登录中…';
    api('api/user/login', { method: 'POST', body: { email: email, password: password } })
      .then(function (data) {
        state.token = data.token || '';
        state.user = data.user || { name: email };
        localStorage.setItem(TOKEN_KEY, state.token);
        localStorage.setItem(USER_KEY, JSON.stringify(state.user));
        setSignedIn(state.user);
        toast('登录成功，欢迎 ' + (state.user.name || email), 'ok');
        if (!profileEl.hidden && state.profileUserId) {
          showProfile(state.profileUserId);
        }
      })
      .catch(function (err) { toast('登录失败：' + (err.message || '网络错误'), 'err'); })
      .then(function () {
        btn.disabled = false;
        btn.textContent = '登录';
      });
  }

  function logout() {
    clearAuth();
    toast('已退出登录');
  }

  /* ---------- 点赞 ---------- */

  function paintLike(slide, liked, count) {
    var btn = slide.querySelector('.like-btn');
    if (!btn) return;
    btn.classList.toggle('liked', liked);
    btn.querySelector('.ico').textContent = liked ? '♥' : '♡';
    if (count !== undefined && count !== null) {
      btn.querySelector('.like-count').textContent = fmt(count);
    }
    var id = slide.dataset.id;
    if (liked) state.likes[id] = 1; else delete state.likes[id];
    try { localStorage.setItem(LIKES_KEY, JSON.stringify(state.likes)); } catch (e) {}
  }

  /* 点赞/取消赞的唯一提交点：按钮与双击都走这里，busy 标记挡住并发重复请求。
     silent=true 时由调用方负责提示（双击只播动画，不弹「点赞成功」）。 */
  function setLike(slide, liked, silent) {
    var id = slide.dataset.id;
    var btn = slide.querySelector('.like-btn');
    if (btn && btn.dataset.busy === '1') return;
    if (btn) btn.dataset.busy = '1';
    api('api/like', {
      method: liked ? 'POST' : 'DELETE',
      body: { video_id: Number(id) }
    }).then(function (data) {
      paintLike(slide, liked, data.like_count);
      var stat = slide.querySelector('.stat-like');
      if (stat) stat.textContent = fmt(data.like_count);
      if (!silent) toast(liked ? '点赞成功' : '已取消点赞', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) toast('点赞失败：' + (err.message || '网络错误'), 'err');
    }).then(function () { if (btn) btn.dataset.busy = ''; });
  }

  function toggleLike(slide) {
    if (!state.token) { toast('请先在右上角登录，再点赞', 'err'); return; }
    setLike(slide, !state.likes[slide.dataset.id]);
  }

  /* 双击点赞：先播心形动画（未登录也播，符合抖音观感），再决定是否发请求。
     已点赞时只动画、不重复 POST，保证 /api/like 幂等语义不被滥用。 */
  function doubleTapLike(slide, event) {
    spawnHeart(slide, event);
    var id = slide.dataset.id;
    if (!state.token) { toast('请先在右上角登录，再点赞', 'err'); return; }
    if (state.likes[id]) { toast('已点赞', 'ok'); return; }
    setLike(slide, true, true);
  }

  /* 在双击坐标处放一颗心；同屏最多 6 颗，动画结束自动移除（防 DOM 泄漏）。 */
  function spawnHeart(slide, event) {
    var rect = slide.getBoundingClientRect();
    var x = event && typeof event.clientX === 'number' ? event.clientX - rect.left : rect.width / 2;
    var y = event && typeof event.clientY === 'number' ? event.clientY - rect.top : rect.height / 2;
    var hearts = slide.querySelectorAll('.heart-burst');
    if (hearts.length >= 6) slide.removeChild(hearts[0]);
    var heart = document.createElement('span');
    heart.className = 'heart-burst';
    heart.textContent = '♥';
    heart.style.left = x + 'px';
    heart.style.top = y + 'px';
    heart.setAttribute('aria-hidden', 'true');
    slide.appendChild(heart);
    setTimeout(function () {
      if (heart.parentNode) heart.parentNode.removeChild(heart);
    }, 900);
  }

  /* 点击落在操作栏/信息区/按钮/进度条上时不抢 slide 的单击、双击语义。 */
  function isUiTarget(target) {
    if (!target || !target.closest) return false;
    return !!(target.closest('.rail') || target.closest('.meta') ||
              target.closest('button') || target.closest('.author-link') ||
              target.closest('.progress'));
  }

  /* ---------- 评论面板 ---------- */

  var REPLY_FOLD = 2;  /* 二级回复默认显示条数，超出折叠 */

  function commentItem(c, replies, expandAll) {
    var row = document.createElement('div');
    row.className = 'cp-item';
    row.dataset.cid = String(c.id === undefined ? '' : c.id);
    var who = document.createElement('div');
    who.className = 'cp-who';
    var uid = document.createElement('b');
    uid.textContent = '用户 #' + (c.user_id === undefined ? '—' : c.user_id);
    who.appendChild(uid);
    if (Number(c.pinned_at) > 0) {
      var pinTag = document.createElement('span');
      pinTag.className = 'cp-pin';
      pinTag.textContent = '置顶';
      who.appendChild(pinTag);
    }
    who.appendChild(document.createTextNode(' · ' + fmtTime(c.created_at)));
    var replyBtn = document.createElement('button');
    replyBtn.type = 'button';
    replyBtn.className = 'cp-reply-btn';
    replyBtn.textContent = '回复';
    replyBtn.addEventListener('click', function () { openReplyForm(row, c); });
    who.appendChild(replyBtn);
    who.appendChild(commentLikeButton(c));
    var cmtMore = document.createElement('button');
    cmtMore.type = 'button';
    cmtMore.className = 'cp-more-btn';
    cmtMore.textContent = '⋯';
    cmtMore.title = '举报 / 拉黑';
    cmtMore.addEventListener('click', function () { openCommentMenu(c); });
    who.appendChild(cmtMore);
    /* 置顶按钮只对视频作者显示；服务端仍会再校验一次权限（非作者 403）。 */
    if (state.user && state.panelOwnerId &&
        String(state.user.id) === state.panelOwnerId) {
      var pinBtn = document.createElement('button');
      pinBtn.type = 'button';
      pinBtn.className = 'cp-pin-btn';
      pinBtn.textContent = Number(c.pinned_at) > 0 ? '取消置顶' : '置顶';
      pinBtn.addEventListener('click', function () {
        toggleCommentPin(pinBtn, c);
      });
      who.appendChild(pinBtn);
    }

    var txt = document.createElement('div');
    txt.className = 'cp-txt';
    txt.textContent = c.content || '';
    row.appendChild(who);
    row.appendChild(txt);

    var box = document.createElement('div');
    box.className = 'cp-replies';
    row.appendChild(box);
    renderReplies(row, replies || [], !!expandAll);
    return row;
  }

  /* 评论点赞按钮：点击后由服务端返回最新计数与状态，前端不自行加减。 */
  function commentLikeButton(c) {
    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'cp-like' + (c.liked ? ' on' : '');
    btn.setAttribute('aria-pressed', c.liked ? 'true' : 'false');
    btn.title = '点赞这条评论';
    var heart = document.createElement('span');
    heart.className = 'cl-heart';
    heart.textContent = '♥';
    var num = document.createElement('span');
    num.className = 'cl-num';
    num.textContent = fmt(Number(c.like_count) || 0);
    btn.appendChild(heart);
    btn.appendChild(document.createTextNode(' '));
    btn.appendChild(num);
    btn.addEventListener('click', function () { toggleCommentLike(btn, c); });
    return btn;
  }

  function paintCommentLike(btn, c) {
    btn.classList.toggle('on', !!c.liked);
    btn.setAttribute('aria-pressed', c.liked ? 'true' : 'false');
    var num = btn.querySelector('.cl-num');
    if (num) num.textContent = fmt(Number(c.like_count) || 0);
  }

  function toggleCommentLike(btn, c) {
    if (!state.token) { toast('请先登录再点赞', 'err'); return; }
    if (btn._busy) return;
    btn._busy = true;
    var wanted = !c.liked;
    api('api/comment/' + c.id + '/like', { method: wanted ? 'POST' : 'DELETE' })
      .then(function (data) {
        c.liked = !!data.liked;
        c.like_count = Number(data.like_count) || 0;
        paintCommentLike(btn, c);
      })
      .catch(function (err) {
        if (!handleAuthError(err)) {
          toast('点赞失败：' + (err.message || '网络错误'), 'err');
        }
      })
      .then(function () { btn._busy = false; });
  }

  /* 渲染某条顶层评论下的回复；超过 REPLY_FOLD 条折叠成「展开更多 N 条」。 */
  function renderReplies(row, replies, expandAll) {
    var box = row.querySelector('.cp-replies');
    if (!box) return;
    row._replies = replies;
    box.innerHTML = '';
    if (!replies.length) {
      box.hidden = true;
      return;
    }
    box.hidden = false;
    var show = expandAll ? replies.length : Math.min(replies.length, REPLY_FOLD);
    for (var i = 0; i < show; i++) {
      var r = replies[i] || {};
      var item = document.createElement('div');
      item.className = 'cp-reply';
      var who = document.createElement('div');
      who.className = 'cp-who';
      var b = document.createElement('b');
      b.textContent = '用户 #' + (r.user_id === undefined ? '—' : r.user_id);
      who.appendChild(b);
      who.appendChild(document.createTextNode(' · ' + fmtTime(r.created_at)));
      who.appendChild(commentLikeButton(r));
      var replyMore = document.createElement('button');
      replyMore.type = 'button';
      replyMore.className = 'cp-more-btn';
      replyMore.textContent = '⋯';
      replyMore.title = '举报 / 拉黑';
      replyMore.addEventListener('click', function () { openCommentMenu(r); });
      who.appendChild(replyMore);
      var t = document.createElement('div');
      t.className = 'cp-txt';
      t.textContent = r.content || '';
      item.appendChild(who);
      item.appendChild(t);
      box.appendChild(item);
    }
    if (show < replies.length) {
      var more = document.createElement('button');
      more.type = 'button';
      more.className = 'cp-more';
      more.textContent = '展开更多 ' + (replies.length - show) + ' 条回复';
      more.addEventListener('click', function () {
        renderReplies(row, row._replies || [], true);
      });
      box.appendChild(more);
    }
  }

  function renderComments(items) {
    cpList.innerHTML = '';
    var list = items || [];
    if (!list.length) {
      var empty = document.createElement('div');
      empty.className = 'cp-empty';
      empty.textContent = '还没有评论，来抢沙发';
      cpList.appendChild(empty);
      return;
    }
    list.forEach(function (it) {
      var c = it.comment || it;
      cpList.appendChild(commentItem(c, it.replies || [], false));
    });
  }

  /* 在指定评论下展开回复输入框（同一时间只保留一个）。 */
  function openReplyForm(row, c) {
    var old = cpList.querySelector('.cp-reply-form');
    if (old) old.remove();
    if (!state.token) { toast('请先登录再回复', 'err'); return; }
    var form = document.createElement('div');
    form.className = 'cp-reply-form';
    var input = document.createElement('input');
    input.type = 'text';
    input.maxLength = 500;
    input.placeholder = '回复 用户 #' + (c.user_id === undefined ? '—' : c.user_id) + '…';
    var send = document.createElement('button');
    send.type = 'button';
    send.className = 'send';
    send.textContent = '发送';
    var cancel = document.createElement('button');
    cancel.type = 'button';
    cancel.className = 'cancel';
    cancel.textContent = '取消';
    cancel.addEventListener('click', function () { form.remove(); });
    send.addEventListener('click', function () {
      var content = (input.value || '').trim();
      if (!content) { toast('回复内容不能为空', 'err'); return; }
      send.disabled = true;
      api('api/comment', {
        method: 'POST',
        body: {
          video_id: Number(state.panelVideoId),
          content: content,
          parent_id: Number(c.id)
        }
      }).then(function (data) {
        var reply = (data && data.comment) || {
          user_id: state.user ? state.user.id : 0,
          content: content,
          created_at: Math.floor(Date.now() / 1000),
          parent_id: c.id
        };
        var list = (row._replies || []).slice();
        list.push(reply);
        renderReplies(row, list, true);
        form.remove();
        bumpCommentCount(state.panelVideoId);
        toast('回复已发送', 'ok');
      }).catch(function (err) {
        if (!handleAuthError(err)) toast('回复失败：' + (err.message || '网络错误'), 'err');
      }).then(function () { send.disabled = false; });
    });
    form.appendChild(input);
    form.appendChild(send);
    form.appendChild(cancel);
    row.appendChild(form);
    input.focus();
  }

  function openComments(slide) {
    var item = state.items[Number(slide.dataset.index)] || {};
    var v = item.video || {};
    state.panelVideoId = String(v.id || slide.dataset.id);
    state.panelSlide = slide;
    /* 记录视频作者：决定是否显示「置顶」按钮（服务端仍会二次校验）。 */
    state.panelOwnerId = v.user_id === undefined ? '' : String(v.user_id);
    cpTitle.textContent = v.title || ('视频 #' + state.panelVideoId);
    cpCount.textContent = fmt(v.comment_count) + ' 条评论';
    panelEl.classList.add('open');
    panelEl.setAttribute('aria-hidden', 'false');
    backdropEl.classList.add('show');
    syncCommentSort();
    loadComments();
    setTimeout(function () { cpInput.focus(); }, 120);
  }

  /* 评论列表重新拉取：切换排序、置顶/取消置顶后都走这里。
     排序与置顶都由服务端决定；第一页固定把置顶评论放在最前。 */
  function loadComments() {
    cpList.innerHTML = '<div class="cp-empty">加载中…</div>';
    api('api/comment/' + state.panelVideoId + '?limit=50&sort=' +
        encodeURIComponent(state.commentSort))
      .then(function (data) { renderComments(data.items); })
      .catch(function (err) {
        cpList.innerHTML = '<div class="cp-empty">评论加载失败：' +
          (err.message || '网络错误') + '</div>';
      });
  }

  /* 排序按钮高亮：state 是唯一真源，按钮只跟着它画。 */
  function syncCommentSort() {
    if (!cpSort) return;
    cpSort.querySelectorAll('button').forEach(function (btn) {
      var on = btn.getAttribute('data-sort') === state.commentSort;
      btn.classList.toggle('on', on);
      btn.setAttribute('aria-selected', on ? 'true' : 'false');
    });
  }

  if (cpSort) {
    cpSort.addEventListener('click', function (ev) {
      var btn = ev.target.closest('button');
      if (!btn) return;
      var sort = btn.getAttribute('data-sort');
      if (!sort || sort === state.commentSort) return;
      state.commentSort = sort;
      syncCommentSort();
      loadComments();
    });
  }

  /* 置顶 / 取消置顶：成功后重拉列表，让顺序与标签以服务端为准。 */
  function toggleCommentPin(btn, c) {
    if (btn._busy) return;
    if (!state.token) { toast('请先登录', 'err'); return; }
    btn._busy = true;
    var wanted = !(Number(c.pinned_at) > 0);
    api('api/comment/' + c.id + '/pin', { method: wanted ? 'POST' : 'DELETE' })
      .then(function (data) {
        var comment = (data && data.comment) || {};
        c.pinned_at = Number(comment.pinned_at) || 0;
        toast(wanted ? '已置顶这条评论' : '已取消置顶', 'ok');
        loadComments();
      })
      .catch(function (err) {
        if (!handleAuthError(err)) {
          toast((wanted ? '置顶' : '取消置顶') + '失败：' +
                (err.message || '网络错误'), 'err');
        }
      })
      .then(function () { btn._busy = false; });
  }

  function closeComments() {
    panelEl.classList.remove('open');
    panelEl.setAttribute('aria-hidden', 'true');
    backdropEl.classList.remove('show');
    state.panelSlide = null;
  }

  /* ---------- @mention 补全 ---------- */

  /* 下拉的当前状态：items 是搜索到的用户（最多 5 个），index 是键盘高亮项，
     seq 用来丢弃「已过时」的搜索响应（快速输入时后发先至）。 */
  var mentionState = { open: false, items: [], index: -1, seq: 0 };
  var mentionTimer = 0;

  function closeMention() {
    mentionTimer = 0;
    mentionState.open = false;
    mentionState.items = [];
    mentionState.index = -1;
    mentionState.seq += 1;
    if (!mentionMenu) return;
    mentionMenu.hidden = true;
    mentionMenu.innerHTML = '';
  }

  /* 光标前是否正处于一个 `@昵称` 输入中；返回 @ 的位置与已输入片段。
     `a@b` 这类邮箱不算提及，与后端解析规则保持一致。 */
  function mentionQuery() {
    var pos = cpInput.selectionStart;
    if (pos === null || pos === undefined) pos = cpInput.value.length;
    var text = cpInput.value.slice(0, pos);
    var at = text.lastIndexOf('@');
    if (at < 0) return null;
    if (at > 0) {
      var prev = text.charAt(at - 1);
      if (/[0-9A-Za-z_]/.test(prev)) return null;
    }
    var word = text.slice(at + 1);
    if (/[\s@]/.test(word)) return null;
    if (word.length > 20) return null;
    return { at: at, word: word };
  }

  function paintMention() {
    if (!mentionMenu) return;
    mentionMenu.innerHTML = '';
    if (!mentionState.items.length) {
      var empty = document.createElement('div');
      empty.className = 'mention-empty';
      empty.textContent = '没有匹配的用户';
      mentionMenu.appendChild(empty);
      mentionMenu.hidden = false;
      return;
    }
    mentionState.items.forEach(function (u, i) {
      var item = document.createElement('button');
      item.type = 'button';
      item.className = 'mention-item' + (i === mentionState.index ? ' active' : '');
      item.textContent = u.name;
      /* mousedown 阻止默认行为：不让输入框先失焦，否则光标位置丢失。 */
      item.addEventListener('mousedown', function (ev) {
        ev.preventDefault();
        pickMention(i);
      });
      mentionMenu.appendChild(item);
    });
    mentionMenu.hidden = false;
  }

  /* 把光标前的 `@片段` 替换成 `@昵称 `，并把光标放到空格之后。 */
  function pickMention(index) {
    var u = mentionState.items[index];
    if (!u) return;
    var pos = cpInput.selectionStart;
    if (pos === null || pos === undefined) pos = cpInput.value.length;
    var text = cpInput.value;
    var at = text.lastIndexOf('@', pos - 1);
    if (at < 0) return;
    var before = text.slice(0, at);
    var after = text.slice(pos);
    var insert = '@' + u.name + ' ';
    cpInput.value = before + insert + after;
    var caret = (before + insert).length;
    cpInput.selectionStart = caret;
    cpInput.selectionEnd = caret;
    closeMention();
    cpInput.focus();
  }

  function searchMention(word) {
    var seq = ++mentionState.seq;
    api('api/search?type=user&limit=5&q=' + encodeURIComponent(word))
      .then(function (data) {
        if (seq !== mentionState.seq) return;
        var users = (data.items || []).filter(function (it) {
          return it.type === 'user';
        });
        if (!users.length && data.users) users = data.users.slice(0, 5);
        mentionState.items = users.slice(0, 5).map(function (u) {
          return { id: u.id, name: u.name || u.title || ('用户#' + u.id) };
        });
        mentionState.index = mentionState.items.length ? 0 : -1;
        paintMention();
      })
      .catch(function () {
        if (seq !== mentionState.seq) return;
        mentionState.items = [];
        mentionState.index = -1;
        paintMention();
      });
  }

  cpInput.addEventListener('input', function () {
    var q = mentionQuery();
    if (!q) { closeMention(); return; }
    mentionState.open = true;
    mentionState.index = -1;
    mentionState.items = [];
    clearTimeout(mentionTimer);
    if (!q.word) {
      /* 刚敲下 @：先提示，不发请求，避免每个 @ 都打一次搜索。 */
      if (mentionMenu) {
        mentionMenu.innerHTML =
          '<div class="mention-empty">继续输入昵称，选择要 @ 的人</div>';
        mentionMenu.hidden = false;
      }
      return;
    }
    mentionTimer = setTimeout(function () { searchMention(q.word); }, 160);
  });

  cpInput.addEventListener('keydown', function (ev) {
    if (!mentionState.open) return;
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      if (!mentionState.items.length) return;
      ev.preventDefault();
      var n = mentionState.items.length;
      var step = ev.key === 'ArrowDown' ? 1 : n - 1;
      mentionState.index = (mentionState.index + step) % n;
      paintMention();
      return;
    }
    if (ev.key === 'Enter') {
      if (!mentionState.items.length) return;
      ev.preventDefault();
      pickMention(mentionState.index < 0 ? 0 : mentionState.index);
      return;
    }
    if (ev.key === 'Escape') {
      closeMention();
    }
  });

  cpInput.addEventListener('blur', function () {
    /* 延时关闭：mousedown 选词发生在 blur 之前，直接关会点不到。 */
    setTimeout(closeMention, 160);
  });

  /* ---------- 消息通知中心 ---------- */

  var notifPanelEl = document.getElementById('notifPanel');
  var notifListEl = document.getElementById('notifList');
  var notifSubEl = document.getElementById('notifSub');
  var notifBadgeEl = document.getElementById('notifBadge');
  var notifMoreBtn = document.getElementById('notifMore');
  var notifState = { items: [], cursor: '', unread: 0, loading: false };

  /* 类型 → 图标；后端已限制 kind 枚举，未知识别成 @mention 兜底。 */
  function notifIcon(kind) {
    if (kind === 'like') return '❤️';
    if (kind === 'comment') return '💬';
    if (kind === 'follow') return '➕';
    return '@';
  }

  /* 相对时间：<1 分钟「刚刚」，<1 小时「N 分钟前」，<1 天「N 小时前」，
     7 天内「N 天前」，更早显示日期。 */
  function fmtAgo(ts) {
    var n = Number(ts) || 0;
    if (!n) return '';
    var diff = Math.max(0, Math.floor(Date.now() / 1000) - n);
    if (diff < 60) return '刚刚';
    if (diff < 3600) return Math.floor(diff / 60) + ' 分钟前';
    if (diff < 86400) return Math.floor(diff / 3600) + ' 小时前';
    if (diff < 7 * 86400) return Math.floor(diff / 86400) + ' 天前';
    return fmtTime(n).slice(0, 10);
  }

  /* 文案：谁 + 做了什么；content 是后端截断后的摘要。 */
  function notifLine(n) {
    var who = n.actor_name || ('用户#' + n.actor_id);
    if (n.kind === 'like') {
      return who + (n.target_type === 'comment' ? ' 赞了你的评论' : ' 赞了你的视频');
    }
    if (n.kind === 'comment') {
      return who + (n.target_type === 'comment' ? ' 回复了你的评论' : ' 评论了你的视频');
    }
    if (n.kind === 'follow') return who + ' 关注了你';
    return who + ' 在评论中提到了你';
  }

  function paintNotifBadge() {
    if (!notifBadgeEl) return;
    var n = Number(notifState.unread) || 0;
    if (n <= 0) {
      notifBadgeEl.hidden = true;
      notifBadgeEl.textContent = '';
      return;
    }
    notifBadgeEl.hidden = false;
    notifBadgeEl.textContent = n > 99 ? '99+' : String(n);
  }

  /* 未读徽章：登录后拉一次，之后每 30 秒轻量轮询；失败静默不打扰用户。 */
  function refreshUnreadNotifications() {
    if (!state.token) { notifState.unread = 0; paintNotifBadge(); return; }
    api('api/notifications/unread_count')
      .then(function (data) {
        notifState.unread = Number(data.unread_count) || 0;
        paintNotifBadge();
      })
      .catch(function () { /* 徽章失败保持旧值 */ });
  }

  function notifEmpty(text) {
    notifListEl.innerHTML = '<div class="cp-empty">' + text + '</div>';
  }

  /* 单条通知：未读行加左侧粉点；点击标已读并按目标跳转。 */
  function notifItemEl(n) {
    var row = document.createElement('div');
    row.className = 'notif-item' + (n.read_at ? '' : ' unread');
    row.dataset.id = String(n.id);
    var icon = document.createElement('span');
    icon.className = 'notif-ico';
    icon.textContent = notifIcon(n.kind);
    var body = document.createElement('div');
    body.className = 'notif-body';
    var line = document.createElement('div');
    line.className = 'notif-line';
    line.textContent = notifLine(n);
    var meta = document.createElement('div');
    meta.className = 'notif-meta';
    var time = document.createElement('span');
    time.textContent = fmtAgo(n.created_at);
    meta.appendChild(time);
    if (n.content) {
      var quote = document.createElement('span');
      quote.className = 'notif-quote';
      quote.textContent = '「' + n.content + '」';
      meta.appendChild(quote);
    }
    body.appendChild(line);
    body.appendChild(meta);
    row.appendChild(icon);
    row.appendChild(body);
    row.addEventListener('click', function () { openNotification(n, row); });
    return row;
  }

  function renderNotifications() {
    notifListEl.innerHTML = '';
    if (!notifState.items.length) {
      notifEmpty(state.token ? '还没有新消息，去发点视频吧' : '登录后查看消息');
      notifMoreBtn.hidden = true;
      notifSubEl.textContent = '全部已读';
      return;
    }
    notifState.items.forEach(function (n) {
      notifListEl.appendChild(notifItemEl(n));
    });
    notifMoreBtn.hidden = !notifState.cursor;
    notifSubEl.textContent = notifState.unread > 0
      ? (notifState.unread + ' 条未读') : '全部已读';
  }

  /* 拉列表：reset=true 重拉第一页，否则按游标续页；固定 limit=20。 */
  function loadNotifications(reset) {
    if (!state.token) { notifState.items = []; renderNotifications(); return; }
    if (notifState.loading) return;
    if (reset) { notifState.items = []; notifState.cursor = ''; }
    if (!reset && !notifState.cursor) return;
    notifState.loading = true;
    var path = 'api/notifications?limit=20';
    if (!reset && notifState.cursor) {
      path += '&cursor=' + encodeURIComponent(notifState.cursor);
    }
    api(path)
      .then(function (data) {
        var items = data.items || [];
        notifState.items = reset ? items : notifState.items.concat(items);
        notifState.cursor = data.next_cursor || '';
        notifState.unread = Number(data.unread_count) || 0;
        paintNotifBadge();
        renderNotifications();
      })
      .catch(function (err) {
        notifEmpty('消息加载失败：' + (err.message || '网络错误'));
        handleAuthError(err);
      })
      .then(function () { notifState.loading = false; });
  }

  function openNotifications() {
    if (!state.token) { toast('请先登录', 'err'); return; }
    closeComments();
    notifPanelEl.classList.add('open');
    notifPanelEl.setAttribute('aria-hidden', 'false');
    backdropEl.classList.add('show');
    renderNotifications();
    loadNotifications(true);
  }

  function closeNotifications() {
    notifPanelEl.classList.remove('open');
    notifPanelEl.setAttribute('aria-hidden', 'true');
    if (!panelEl.classList.contains('open')) {
      backdropEl.classList.remove('show');
    }
  }

  /* 标已读：本地先改（立即反馈），接口失败再回退并提示。 */
  function markNotificationRead(n, row) {
    if (!state.token || n.read_at) return;
    n.read_at = Math.floor(Date.now() / 1000);
    if (row) row.classList.remove('unread');
    notifState.unread = Math.max(0, notifState.unread - 1);
    paintNotifBadge();
    notifSubEl.textContent = notifState.unread > 0
      ? (notifState.unread + ' 条未读') : '全部已读';
    api('api/notifications/read', { method: 'POST', body: { ids: [n.id] } })
      .then(function (data) {
        notifState.unread = Number(data.unread_count) || 0;
        paintNotifBadge();
      })
      .catch(function (err) {
        n.read_at = 0;
        if (row) row.classList.add('unread');
        notifState.unread += 1;
        paintNotifBadge();
        handleAuthError(err);
      });
  }

  /* 点击通知：先标已读，再跳目标——关注通知看关注者主页，视频类通知
     直接定位到那条视频；评论点赞没有视频 id，只标已读。 */
  function openNotification(n, row) {
    markNotificationRead(n, row);
    if (n.kind === 'follow' && n.actor_id) {
      closeNotifications();
      showProfile(n.actor_id);
      return;
    }
    if (n.target_type === 'video' && n.target_id) {
      closeNotifications();
      jumpToVideo(n.target_id);
      return;
    }
    if (n.kind === 'like' && n.target_type === 'comment') {
      toast('有人赞了你的评论', 'ok');
    }
  }

  function markAllNotificationsRead() {
    if (!state.token || notifState.unread <= 0) return;
    api('api/notifications/read', { method: 'POST', body: { all: true } })
      .then(function () {
        var now = Math.floor(Date.now() / 1000);
        notifState.items.forEach(function (n) { n.read_at = now; });
        notifState.unread = 0;
        paintNotifBadge();
        renderNotifications();
        toast('已全部标记为已读', 'ok');
      })
      .catch(function (err) {
        toast('操作失败：' + (err.message || '网络错误'), 'err');
        handleAuthError(err);
      });
  }

  function slideById(videoId) {
    var found = null;
    feedEl.querySelectorAll('.slide').forEach(function (s) {
      if (s.dataset.id === String(videoId)) found = s;
    });
    return found;
  }

  function bumpCommentCount(videoId) {
    var current = 0;
    state.items.forEach(function (item) {
      var v = item.video || {};
      if (String(v.id) === String(videoId)) {
        v.comment_count = (Number(v.comment_count) || 0) + 1;
        current = v.comment_count;
      }
    });
    var slide = slideById(videoId);
    if (slide) {
      var num = slide.querySelector('.cmt-num');
      if (num) {
        num.dataset.n = String(current);
        num.textContent = fmt(current);
      }
      var stat = slide.querySelector('.stat-comment');
      if (stat) stat.textContent = fmt(current);
    }
    if (String(state.panelVideoId) === String(videoId)) {
      cpCount.textContent = fmt(current) + ' 条评论';
    }
  }

  function sendComment() {
    if (!state.token) { toast('请先在右上角登录，再评论', 'err'); return; }
    var content = (cpInput.value || '').trim();
    if (!content) { toast('评论内容不能为空', 'err'); return; }
    if (content.length > 500) { toast('评论最多 500 字', 'err'); return; }
    if (!state.panelVideoId) return;
    cpSend.disabled = true;
    api('api/comment', {
      method: 'POST',
      body: { video_id: Number(state.panelVideoId), content: content }
    }).then(function (data) {
      cpInput.value = '';
      closeMention();
      var c = (data && data.comment) || {
        user_id: state.user ? state.user.id : 0,
        content: content,
        created_at: Math.floor(Date.now() / 1000)
      };
      var empty = cpList.querySelector('.cp-empty');
      if (empty) cpList.removeChild(empty);
      cpList.insertBefore(commentItem(c, [], false), cpList.firstChild);
      bumpCommentCount(state.panelVideoId);
      toast('评论已发送', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) toast('评论失败：' + (err.message || '网络错误'), 'err');
    }).then(function () { cpSend.disabled = false; });
  }

  /* ---------- 声音控制 ---------- */

  /* 把全局静音/音量状态套到所有视频与按钮上（新插入的 slide 也要调用）。 */
  function applySound() {
    feedEl.querySelectorAll('video').forEach(function (v) {
      v.muted = state.muted;
      v.volume = state.volume;
    });
    document.querySelectorAll('.sound-btn').forEach(function (b) {
      b.textContent = state.muted ? '🔇 开声音' : '🔊 声音开';
      b.setAttribute('aria-pressed', state.muted ? 'false' : 'true');
    });
    var slider = document.getElementById('volumeSlider');
    if (slider) slider.value = String(Math.round(state.volume * 100));
    var icon = document.getElementById('volIcon');
    if (icon) {
      icon.textContent = (state.muted || state.volume === 0)
        ? '🔇' : (state.volume < 0.5 ? '🔈' : '🔊');
    }
  }

  /* 切换静音；silent=true 时不弹 toast（拖动音量条时用）。 */
  function setMuted(muted, silent) {
    state.muted = !!muted;
    try { localStorage.setItem(MUTE_KEY, state.muted ? '1' : '0'); } catch (e) {}
    applySound();
    if (!state.muted) {
      var active = state.activeVideo || feedEl.querySelector('.slide.active video');
      if (active) {
        var p = active.play();
        if (p && p.catch) p.catch(function () {});
      }
    }
    if (!silent) toast(state.muted ? '已静音' : '已开启声音');
  }

  /* ---------- 收藏（⭐） ---------- */

  /* 收藏状态以服务端为准：登录后拉一次全量 id 集合，按钮按集合渲染。
     不在前端做 localStorage 缓存——换设备登录要能看到同一份收藏。 */
  function isFaved(id) {
    return !!state.favs[String(id)];
  }

  function paintFav(btn, id) {
    if (!btn) return;
    var on = isFaved(id);
    var ico = btn.querySelector('.ico');
    if (ico) ico.textContent = on ? '⭐' : '☆';
    var label = btn.querySelector('.fav-label');
    if (label) label.textContent = on ? '已收藏' : '收藏';
    btn.classList.toggle('faved', on);
    btn.setAttribute('aria-pressed', on ? 'true' : 'false');
  }

  /* buildSlide 可能早于收藏集合返回，所以集合更新后统一重刷所有按钮 */
  function syncFavButtons() {
    feedEl.querySelectorAll('.fav-btn').forEach(function (btn) {
      paintFav(btn, btn.dataset.id);
    });
  }

  function loadFavorites() {
    if (!state.token) {
      state.favs = {};
      syncFavButtons();
      return;
    }
    api('api/video/favorites?limit=100').then(function (data) {
      var next = {};
      (data.items || []).forEach(function (item) {
        var v = (item && item.video) || item || {};
        if (v.id) next[String(v.id)] = true;
      });
      state.favs = next;
      syncFavButtons();
    }).catch(function (err) {
      /* 拉收藏状态失败不打断浏览：按钮保持默认 ☆，用户点一下会重新提交 */
      handleAuthError(err);
    });
  }

  function toggleFavorite(id, btn) {
    if (!state.token) { toast('登录后才能收藏', 'err'); return; }
    var key = String(id);
    if (btn) btn.disabled = true;
    api('api/video/' + encodeURIComponent(key) + '/favorite', { method: 'POST' })
      .then(function (data) {
        if (data.favorited) state.favs[key] = true; else delete state.favs[key];
        syncFavButtons();
        toast(data.favorited ? '已加入我的收藏' : '已取消收藏', 'ok');
        if (!favViewEl.hidden) renderFavorites();
      })
      .catch(function (err) {
        if (!handleAuthError(err)) {
          toast('收藏失败：' + (err.message || '网络错误'), 'err');
        }
      })
      .then(function () { if (btn) btn.disabled = false; });
  }

  /* ---------- 「我的收藏」页 ---------- */

  function renderFavorites() {
    favBody.innerHTML = '';
    var loading = document.createElement('div');
    loading.className = 'notice';
    loading.textContent = '加载收藏…';
    favBody.appendChild(loading);
    api('api/video/favorites?limit=100').then(function (data) {
      var items = data.items || [];
      var next = {};
      items.forEach(function (item) {
        var v = (item && item.video) || item || {};
        if (v.id) next[String(v.id)] = true;
      });
      state.favs = next;
      syncFavButtons();
      favBody.innerHTML = '';
      if (!items.length) {
        var empty = document.createElement('div');
        empty.className = 'notice';
        empty.textContent = '还没有收藏。点视频右侧的 ☆ 收藏喜欢的视频。';
        favBody.appendChild(empty);
        return;
      }
      var note = document.createElement('div');
      note.className = 'profile-note';
      note.textContent = '共 ' + items.length + ' 条收藏 · 点击播放';
      favBody.appendChild(note);
      var grid = document.createElement('div');
      grid.className = 'profile-grid';
      items.forEach(function (item) {
        var v = (item && item.video) || item || {};
        var cardBtn = document.createElement('button');
        cardBtn.type = 'button';
        cardBtn.className = 'pv-card';
        cardBtn.dataset.id = String(v.id || 0);
        var pv = document.createElement('video');
        pv.src = videoSrc(v);
        pv.muted = true;
        pv.playsInline = true;
        pv.preload = 'metadata';
        pv.setAttribute('muted', '');
        pv.setAttribute('playsinline', '');
        var pt = document.createElement('span');
        pt.className = 'pv-title';
        pt.textContent = v.title || ('视频 #' + (v.id || ''));
        cardBtn.appendChild(pv);
        cardBtn.appendChild(pt);
        cardBtn.addEventListener('click', function () { playInFeed(item, v); });
        grid.appendChild(cardBtn);
      });
      favBody.appendChild(grid);
    }).catch(function (err) {
      if (handleAuthError(err)) return;
      favBody.innerHTML = '';
      var box = document.createElement('div');
      box.className = 'notice';
      box.textContent = '收藏列表加载失败：' + (err.message || '网络错误');
      favBody.appendChild(box);
    });
  }

  function showFavorites() {
    if (!state.user) { toast('请先登录', 'err'); return; }
    closeComments();
    pauseAllVideos();
    profileEl.hidden = true;
    favViewEl.hidden = false;
    favViewEl.scrollTop = 0;
    renderFavorites();
  }

  /* 只负责隐藏；播放由调用方（playInFeed / 返回按钮）决定，避免重复 play */
  function hideFavorites() {
    favViewEl.hidden = true;
  }

  /* ---------- 分享链接 / 直达 ---------- */

  function videoLink(id) {
    return location.origin + BASE + 'v/' + id;
  }

  function fallbackCopy(text, done) {
    var area = document.createElement('textarea');
    area.value = text;
    area.style.position = 'fixed';
    area.style.top = '-1000px';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
    document.body.removeChild(area);
    if (ok) { done(); } else { toast('复制失败，链接：' + text, 'err'); }
  }

  function copyVideoLink(id) {
    var url = videoLink(id);
    var done = function () { toast('链接已复制：' + url, 'ok'); };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(url).then(done, function () {
        fallbackCopy(url, done);
      });
    } else {
      fallbackCopy(url, done);
    }
  }

  /* ---------- 分享面板（复链接 / 二维码占位 / 第三方占位） ---------- */

  var shareVideoId = '';

  function renderQr(seedText) {
    var n = 21;
    var seed = 0;
    String(seedText).split('').forEach(function (ch) {
      seed = (seed * 31 + ch.charCodeAt(0)) >>> 0;
    });
    var html = '';
    for (var i = 0; i < n * n; i++) {
      seed = (seed * 1103515245 + 12345) >>> 0;
      var row = Math.floor(i / n);
      var col = i % n;
      /* 三个 7x7 定位角 + 伪随机点阵，只求「看起来像二维码」；不能真扫 */
      var corner = (row < 7 && col < 7) || (row < 7 && col >= n - 7) ||
                   (row >= n - 7 && col < 7);
      var on = corner || (((seed >>> 16) & 1) === 1);
      html += on ? '<i class="on"></i>' : '<i></i>';
    }
    qrBox.innerHTML = html;
  }

  function openShare(id, title) {
    shareVideoId = String(id || '');
    ssTitle.textContent = '分享「' + (title || ('视频 #' + shareVideoId)) + '」';
    ssQr.hidden = true;
    renderQr(shareVideoId);
    shareMask.hidden = false;
    shareSheet.hidden = false;
  }

  function closeShare() {
    shareMask.hidden = true;
    shareSheet.hidden = true;
  }

  /* 打开分享链接后滚到对应视频并播放；不在当前 feed 时提示。 */
  function jumpToVideo(id) {
    var slide = slideById(id);
    if (!slide) {
      toast('视频 #' + id + ' 不在当前推荐流中', 'err');
      return;
    }
    /* 用 scrollIntoView 交给浏览器处理滚动容器，避免 offsetParent 差异 */
    if (slide.scrollIntoView) {
      slide.scrollIntoView({ block: 'start' });
    } else {
      feedEl.scrollTop = slide.offsetTop;
    }
    var video = slide.querySelector('video');
    if (video) playOnly(video);
  }

  /* ---------- 关注 / 取关 ---------- */

  function toggleFollow(user, btn, stats) {
    if (!state.token) { toast('请先登录再关注', 'err'); return; }
    var following = btn.classList.contains('following');
    btn.disabled = true;
    api('api/user/' + user.id + '/follow', { method: following ? 'DELETE' : 'POST' })
      .then(function (data) {
        var now = !following;
        btn.classList.toggle('following', now);
        btn.textContent = now ? '已关注' : '+ 关注';
        if (data && typeof data.followers === 'number') {
          user.followers = data.followers;
          var el = stats.querySelector('.p-followers');
          if (el) el.textContent = fmt(data.followers);
        }
        toast(now ? '已关注 @' + (user.name || user.id) : '已取消关注');
      })
      .catch(function (err) {
        if (!handleAuthError(err)) {
          toast('关注失败：' + (err.message || '网络错误'), 'err');
        }
      })
      .then(function () { btn.disabled = false; });
  }

  /* ---------- 播放控制 ---------- */

  function pauseAllVideos() {
    feedEl.querySelectorAll('video').forEach(function (v) { v.pause(); });
  }

  function playOnly(video) {
    feedEl.querySelectorAll('video').forEach(function (v) {
      if (v !== video) v.pause();
    });
    var p = video.play();
    if (p && p.catch) {
      p.catch(function () {
        /* 未静音时自动播放会被策略拒绝：回退静音并提示一次。 */
        if (!state.muted) {
          state.muted = true;
          try { localStorage.setItem(MUTE_KEY, '1'); } catch (e) {}
          applySound();
          var retry = video.play();
          if (retry && retry.catch) retry.catch(function () {});
          toast('浏览器限制自动播放，已暂时静音；点「开声音」恢复', 'err');
        }
      });
    }
  }

  function setupObserver() {
    if (state.observer) state.observer.disconnect();
    if (typeof IntersectionObserver !== 'function') {
      var first = feedEl.querySelector('video');
      if (first) playOnly(first);
      return;
    }
    /* 阈值 0.6：滑到哪一屏就播哪一屏，其余暂停 */
    state.observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        var video = entry.target.querySelector('video');
        if (!video) return;
        if (entry.isIntersecting && entry.intersectionRatio >= 0.6) {
          state.activeVideo = video;
          entry.target.classList.add('active');
          playOnly(video);
        } else if (!entry.isIntersecting || entry.intersectionRatio < 0.6) {
          video.pause();
          if (!entry.isIntersecting) entry.target.classList.remove('active');
        }
      });
    }, { root: feedEl, threshold: [0.6] });
    feedEl.querySelectorAll('.slide').forEach(function (s) {
      state.observer.observe(s);
    });
  }

  function togglePlay(slide) {
    var video = slide.querySelector('video');
    if (!video) return;
    if (video.paused) {
      playOnly(video);
    } else {
      video.pause();
    }
  }

  /* ---------- 上传自己的视频 ---------- */

  /* 上传成功后把新视频插到 feed 首位：服务端 feed 按热度排序，新视频
     没有互动分，不插入的话会被种子数据的高评论视频挤到后面。 */
  function insertUploaded(video) {
    var item = {
      video: video,
      author: (state.user && (state.user.name || state.user.email)) ||
              ('user' + (video.user_id || ''))
    };
    state.items.unshift(item);
    feedEl.innerHTML = '';
    var frag = document.createDocumentFragment();
    state.items.forEach(function (it, i) { frag.appendChild(buildSlide(it, i)); });
    feedEl.appendChild(frag);
    feedEl.scrollTop = 0;
    setupObserver();
    applySound();
  }

  function uploadVideo(file) {
    var title = file.name || '未命名视频';
    var xhr = new XMLHttpRequest();
    xhr.open('POST', BASE + 'api/video/upload?title=' + encodeURIComponent(title));
    xhr.setRequestHeader('Authorization', 'Bearer ' + state.token);
    xhr.setRequestHeader('Content-Type', 'video/mp4');
    uploadBtn.disabled = true;
    uploadBtn.textContent = '上传 0%';
    var reset = function () {
      uploadBtn.disabled = false;
      uploadBtn.textContent = '上传';
    };
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable && e.total > 0) {
        uploadBtn.textContent =
          '上传 ' + Math.round(e.loaded / e.total * 100) + '%';
      }
    };
    xhr.onload = function () {
      reset();
      var data = null;
      try { data = JSON.parse(xhr.responseText || '{}'); } catch (e) { data = null; }
      if (xhr.status === 200 && data && data.ok && data.video) {
        toast('上传成功', 'ok');
        insertUploaded(data.video);
        return;
      }
      var err = new Error((data && data.error) || ('HTTP ' + xhr.status));
      err.status = xhr.status;
      if (!handleAuthError(err)) toast('上传失败：' + err.message, 'err');
    };
    xhr.onerror = function () {
      reset();
      toast('上传失败：网络错误', 'err');
    };
    xhr.send(file);
  }

  /* ---------- feed 渲染 ---------- */

  /* 视频地址解析：DB 里存的是 `videos/<文件名>`，但历史数据也可能是
     `/videos/x.mp4`、纯文件名或外部 http(s) 地址；四种都归一，避免「同一个
     视频在 feed 能播、在主页播不了」这类前缀不一致。全都不匹配才退回 demo。 */
  function videoSrc(v) {
    v = v || {};
    var url = String(v.url || '');
    if (/^https?:\/\//i.test(url)) return url;
    if (/^\/?videos\/[A-Za-z0-9_.-]+$/.test(url)) {
      return BASE + url.replace(/^\//, '');
    }
    if (/^[A-Za-z0-9_.-]+\.(mp4|webm|m4v)$/i.test(url)) return BASE + 'videos/' + url;
    return BASE + 'videos/demo' + ((Number(v.id) % 5) + 1) + '.mp4';
  }

  /* 封面地址：与视频同名的 .jpg（构建时用 ffmpeg 抽首帧）。
     没有对应封面时返回空串，video 退回黑底，不影响播放。 */
  function posterSrc(v) {
    var src = String(videoSrc(v) || '');
    var m = /^(.+\/videos\/[A-Za-z0-9_.-]+)\.(mp4|webm|m4v)$/i.exec(src);
    if (!m) return '';
    return m[1] + '.jpg';
  }

  function fmtClock(seconds) {
    if (!isFinite(seconds) || seconds < 0) seconds = 0;
    var total = Math.floor(seconds);
    var m = Math.floor(total / 60);
    var s = total % 60;
    return m + ':' + (s < 10 ? '0' + s : s);
  }

  /* 进度条：timeupdate 驱动填充；pointer 拖动 seek 到 pct*duration。
     preload=none 时首次拖动会临时升级到 metadata，拿到 duration 后补一次 seek。 */
  function buildProgress(slide, video) {
    var bar = document.createElement('div');
    bar.className = 'progress';
    bar.tabIndex = 0;
    bar.setAttribute('role', 'slider');
    bar.setAttribute('aria-label', '播放进度');
    bar.setAttribute('aria-valuemin', '0');
    bar.setAttribute('aria-valuemax', '100');
    bar.setAttribute('aria-valuenow', '0');
    bar.innerHTML = '<div class="progress-rail"><div class="progress-fill"></div></div>' +
                    '<div class="progress-thumb"></div>' +
                    '<span class="progress-time">0:00 / 0:00</span>';
    var fill = bar.querySelector('.progress-fill');
    var thumb = bar.querySelector('.progress-thumb');
    var time = bar.querySelector('.progress-time');
    var dragging = false;
    var pendingRatio = null;

    function duration() {
      var d = video.duration;
      return isFinite(d) && d > 0 ? d : 0;
    }

    function paint() {
      var d = duration();
      var pct = d > 0 ? Math.min(1, Math.max(0, video.currentTime / d)) : 0;
      fill.style.width = (pct * 100).toFixed(2) + '%';
      thumb.style.left = (pct * 100).toFixed(2) + '%';
      time.textContent = fmtClock(video.currentTime) + ' / ' + fmtClock(d);
      bar.setAttribute('aria-valuenow', String(Math.round(pct * 100)));
      bar.setAttribute('aria-valuetext', fmtClock(video.currentTime) + ' / ' + fmtClock(d));
    }

    function ratioOf(event) {
      var rect = bar.getBoundingClientRect();
      if (rect.width <= 0) return 0;
      return Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
    }

    function seekToRatio(ratio) {
      var d = duration();
      if (d <= 0) { pendingRatio = ratio; paintPreview(ratio); return; }
      pendingRatio = null;
      video.currentTime = ratio * d;
      paintPreview(ratio);
    }

    function paintPreview(ratio) {
      fill.style.width = (ratio * 100).toFixed(2) + '%';
      thumb.style.left = (ratio * 100).toFixed(2) + '%';
      var d = duration();
      time.textContent = fmtClock(d > 0 ? ratio * d : 0) + ' / ' + fmtClock(d);
    }

    function endDrag(event) {
      if (!dragging) return;
      dragging = false;
      bar.classList.remove('dragging');
      if (event && bar.hasPointerCapture && bar.hasPointerCapture(event.pointerId)) {
        bar.releasePointerCapture(event.pointerId);
      }
      paint();
    }

    bar.addEventListener('pointerdown', function (event) {
      event.preventDefault();
      event.stopPropagation();
      dragging = true;
      bar.classList.add('dragging');
      if (bar.setPointerCapture) {
        try { bar.setPointerCapture(event.pointerId); } catch (e) {}
      }
      if (video.readyState === 0) {
        video.preload = 'metadata';
        video.load();
      }
      seekToRatio(ratioOf(event));
    });
    bar.addEventListener('pointermove', function (event) {
      if (!dragging) return;
      event.preventDefault();
      seekToRatio(ratioOf(event));
    });
    bar.addEventListener('pointerup', endDrag);
    bar.addEventListener('pointercancel', endDrag);
    bar.addEventListener('click', function (event) { event.stopPropagation(); });
    bar.addEventListener('dblclick', function (event) { event.stopPropagation(); });
    bar.addEventListener('keydown', function (event) {
      var d = duration();
      if (!d) return;
      if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
        event.preventDefault();
        event.stopPropagation();
        var next = video.currentTime + (event.key === 'ArrowRight' ? 5 : -5);
        video.currentTime = Math.min(d, Math.max(0, next));
        paint();
      }
    });
    video.addEventListener('loadedmetadata', function () {
      if (pendingRatio !== null) seekToRatio(pendingRatio);
      paint();
    });
    video.addEventListener('durationchange', paint);
    video.addEventListener('timeupdate', function () { if (!dragging) paint(); });
    video.addEventListener('seeked', function () { if (!dragging) paint(); });
    video.addEventListener('emptied', paint);
    paint();
    return bar;
  }

  function authorName(item) {
    return item.author || ('user' + ((item.video || {}).user_id || ''));
  }

  function buildSlide(item, index) {
    var v = item.video || {};
    var id = String(v.id || 0);
    var name = authorName(item);
    var slide = document.createElement('article');
    slide.className = 'slide';
    slide.dataset.id = id;
    slide.dataset.index = String(index);
    slide.setAttribute('data-user-id', String(v.user_id || ''));

    var video = document.createElement('video');
    video.className = 'main-video' + (state.fit === 'cover' ? ' cover' : '');
    video.src = videoSrc(v);
    video.muted = state.muted;
    video.volume = state.volume;
    video.loop = true;
    video.playsInline = true;
    /* 首屏不预加载视频本体：只拉同名封面 jpg，真正字节等 play()/拖动时才请求。 */
    video.preload = 'none';
    var poster = posterSrc(v);
    if (poster) {
      video.poster = poster;
      video.setAttribute('poster', poster);
    }
    video.setAttribute('playsinline', '');
    video.setAttribute('webkit-playsinline', '');

    var shade = document.createElement('div');
    shade.className = 'shade';

    var hint = document.createElement('div');
    hint.className = 'pause-hint';
    hint.innerHTML = '<i>▶</i>';

    var meta = document.createElement('div');
    meta.className = 'meta';

    var title = document.createElement('h1');
    title.className = 'title';
    renderTitleInto(title, v);

    var authorRow = document.createElement('div');
    authorRow.className = 'author-row';
    var avatar = document.createElement('span');
    avatar.className = 'avatar author-link';
    avatar.textContent = initial(name);
    avatar.title = '查看 ' + name + ' 的主页';
    var authorBtn = document.createElement('button');
    authorBtn.type = 'button';
    authorBtn.className = 'author-name';
    authorBtn.textContent = '@' + name;
    var authorMeta = document.createElement('span');
    authorMeta.className = 'author-meta';
    authorMeta.textContent = '· 视频 ID ' + id;
    authorRow.appendChild(avatar);
    authorRow.appendChild(authorBtn);
    authorRow.appendChild(authorMeta);

    var stats = document.createElement('div');
    stats.className = 'stats';
    stats.innerHTML = '<span>♥ <b class="stat-like"></b> 点赞</span>' +
                      '<span>💬 <b class="stat-comment"></b> 评论</span>';
    stats.querySelector('.stat-like').textContent = fmt(v.like_count);
    stats.querySelector('.stat-comment').textContent = fmt(v.comment_count);

    var fitBtn = document.createElement('button');
    fitBtn.type = 'button';
    fitBtn.className = 'fit-btn';
    fitBtn.textContent = fitLabel();
    fitBtn.title = '切换 铺满/完整 画幅（记住选择）';
    fitBtn.setAttribute('aria-pressed', state.fit === 'cover' ? 'true' : 'false');
    fitBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      setFit(state.fit === 'cover' ? 'contain' : 'cover');
    });

    var soundBtn = document.createElement('button');
    soundBtn.type = 'button';
    soundBtn.className = 'sound-btn';
    soundBtn.textContent = state.muted ? '🔇 开声音' : '🔊 声音开';
    soundBtn.setAttribute('aria-pressed', state.muted ? 'false' : 'true');
    soundBtn.title = '切换静音/取消静音';
    soundBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      setMuted(!state.muted);
    });

    var tools = document.createElement('div');
    tools.className = 'tools';
    tools.appendChild(fitBtn);
    tools.appendChild(soundBtn);

    meta.appendChild(title);
    var tagChips = document.createElement('div');
    tagChips.className = 'tag-chips';
    renderChips(tagChips, v);
    if (tagChips.childNodes.length) meta.appendChild(tagChips);
    meta.appendChild(authorRow);
    meta.appendChild(stats);
    meta.appendChild(tools);

    var rail = document.createElement('div');
    rail.className = 'rail';
    var railAvatar = document.createElement('span');
    railAvatar.className = 'avatar author-link';
    railAvatar.textContent = initial(name);
    railAvatar.title = '查看 ' + name + ' 的主页';

    var likeBtn = document.createElement('button');
    likeBtn.type = 'button';
    likeBtn.className = 'like-btn';
    likeBtn.innerHTML = '<span class="ico">♡</span><span class="like-count num">0</span>';
    likeBtn.querySelector('.like-count').textContent = fmt(v.like_count);
    likeBtn.addEventListener('click', function (e) { e.stopPropagation(); toggleLike(slide); });

    var favBtn = document.createElement('button');
    favBtn.type = 'button';
    favBtn.className = 'fav-btn';
    favBtn.dataset.id = id;
    favBtn.innerHTML = '<span class="ico">☆</span><span class="fav-label">收藏</span>';
    favBtn.title = '收藏 / 取消收藏';
    favBtn.setAttribute('aria-pressed', 'false');
    favBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      toggleFavorite(id, favBtn);
    });
    paintFav(favBtn, id);

    var cmtBtn = document.createElement('button');
    cmtBtn.type = 'button';
    cmtBtn.className = 'comment-btn';
    cmtBtn.innerHTML = '<span class="ico">💬</span><span class="cmt-num num">' +
      fmt(v.comment_count) + '</span>';
    cmtBtn.querySelector('.cmt-num').dataset.n = String(v.comment_count || 0);
    cmtBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      openComments(slide);
    });

    var shareBtn = document.createElement('button');
    shareBtn.type = 'button';
    shareBtn.className = 'share-btn';
    shareBtn.innerHTML = '<span class="ico">🔗</span><span>分享</span>';
    shareBtn.title = '复制这条视频的分享链接';
    shareBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      openShare(id, v.title);
    });

    var moreBtn = document.createElement('button');
    moreBtn.type = 'button';
    moreBtn.className = 'more-btn';
    moreBtn.innerHTML = '<span class="ico">⋯</span><span>更多</span>';
    moreBtn.title = '举报 / 拉黑';
    moreBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      openVideoMenu(v, name);
    });

    rail.appendChild(railAvatar);
    rail.appendChild(likeBtn);
    rail.appendChild(favBtn);
    rail.appendChild(cmtBtn);
    rail.appendChild(shareBtn);
    rail.appendChild(moreBtn);

    var openProfile = function (e) {
      e.stopPropagation();
      showProfile(v.user_id, name);
    };
    avatar.addEventListener('click', openProfile);
    railAvatar.addEventListener('click', openProfile);
    authorBtn.addEventListener('click', openProfile);

    video.addEventListener('play', function () {
      slide.classList.add('playing');
      state.activeVideo = video;
      feedEl.querySelectorAll('video').forEach(function (other) {
        if (other !== video) other.pause();
      });
    });
    video.addEventListener('pause', function () {
      slide.classList.remove('playing');
      if (state.activeVideo === video) state.activeVideo = null;
    });
    video.addEventListener('ended', function () { slide.classList.remove('playing'); });
    video.addEventListener('error', function () {
      slide.classList.remove('playing');
      toast('视频加载失败，检查 /videos 静态路由', 'err');
    });

    /* 单击 = 暂停/播放，双击 = 点赞：单击先挂起 240ms，等第二下不来再执行，
       否则双击会先暂停再播放（画面闪一下）并多走一次播放切换。 */
    var tapTimer = 0;
    slide.addEventListener('click', function (e) {
      if (isUiTarget(e.target)) return;
      clearTimeout(tapTimer);
      tapTimer = setTimeout(function () { tapTimer = 0; togglePlay(slide); }, 240);
    });
    slide.addEventListener('dblclick', function (e) {
      if (isUiTarget(e.target)) return;
      clearTimeout(tapTimer);
      tapTimer = 0;
      doubleTapLike(slide, e);
    });

    slide.appendChild(video);
    slide.appendChild(shade);
    slide.appendChild(hint);
    slide.appendChild(meta);
    slide.appendChild(rail);
    slide.appendChild(buildProgress(slide, video));
    return slide;
  }

  function showNotice(text, withRetry) {
    feedEl.innerHTML = '';
    var box = document.createElement('div');
    box.className = 'notice';
    box.textContent = text;
    if (withRetry) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'btn-primary';
      btn.textContent = '重新加载';
      btn.addEventListener('click', loadFeed);
      box.appendChild(document.createElement('br'));
      box.appendChild(btn);
    }
    feedEl.appendChild(box);
  }

  function showSkeleton() {
    feedEl.innerHTML = '';
    for (var i = 0; i < 3; i++) {
      var sk = document.createElement('div');
      sk.className = 'skeleton';
      feedEl.appendChild(sk);
    }
  }

  /* 按 state.items 重建全部 slide：feed 首次加载与「主页点视频补进 feed」
     共用这一处，保证两边永远同一套播放器与同一套 observer。 */
  function renderFeed() {
    feedEl.innerHTML = '';
    var frag = document.createDocumentFragment();
    state.items.forEach(function (item, i) { frag.appendChild(buildSlide(item, i)); });
    feedEl.appendChild(frag);
    feedEl.scrollTop = 0;
    setupObserver();
    applySound();
  }

  function loadFeed() {
    showSkeleton();
    api('api/feed?limit=20')
      .then(function (data) {
        var items = data.items || [];
        state.items = items;
        if (!items.length) {
          feedEl.innerHTML = '';
          showNotice('还没有视频，先跑 scripts/seed_bulk.sh 灌数据', true);
          return;
        }
        renderFeed();
        if (state.deepLink) {
          jumpToVideo(state.deepLink);
          toast('已定位到分享的视频 #' + state.deepLink, 'ok');
        }
      })
      .catch(function (err) {
        showNotice('feed 加载失败：' + (err.message || '网络错误'), true);
      });
  }

  /* ---------- 搜索结果页：用户 / 视频 / 话题三组结果 ---------- */

  function searchTabName(tab) {
    return tab === 'user' ? '用户' : (tab === 'video' ? '视频' : '话题');
  }

  function paintSearchTabs() {
    var tabs = searchTabs.querySelectorAll('.search-tab');
    for (var i = 0; i < tabs.length; ++i) {
      var on = tabs[i].dataset.tab === searchState.tab;
      tabs[i].classList.toggle('active', on);
    }
  }

  function searchEmpty(text) {
    var div = document.createElement('div');
    div.className = 'search-empty';
    div.textContent = text;
    return div;
  }

  function searchRow(avatarClass, avatarText, title, subtitle, onClick) {
    var row = document.createElement('button');
    row.type = 'button';
    row.className = 'search-row';
    var av = document.createElement('span');
    av.className = 'search-avatar' + (avatarClass ? ' ' + avatarClass : '');
    av.textContent = avatarText;
    var box = document.createElement('span');
    box.className = 'search-main';
    var b = document.createElement('b');
    b.textContent = title;
    var s = document.createElement('span');
    s.textContent = subtitle;
    box.appendChild(b);
    box.appendChild(s);
    row.appendChild(av);
    row.appendChild(box);
    row.addEventListener('click', onClick);
    return row;
  }

  function searchUserRow(u) {
    return searchRow('', initial(u.name || '?'),
      u.name || ('用户 #' + u.id), '用户 · id ' + u.id, function () {
        showProfile(u.id, u.name);
      });
  }

  function searchVideoRow(v) {
    var row = searchRow('', '▶', v.title || ('视频 #' + v.id),
      '❤ ' + fmt(v.like_count) + ' · 💬 ' + fmt(v.comment_count), function () {
        var video = {
          id: v.id, user_id: v.user_id, title: v.title, url: v.url,
          like_count: v.like_count, comment_count: v.comment_count
        };
        playInFeed({ video: video, author: '用户' + v.user_id });
        searchEl.hidden = true;
      });
    row.firstChild.className = 'search-cover';
    return row;
  }

  function searchTagRow(t) {
    return searchRow('tag', '#', '#' + (t.name || ''),
      (t.count || 0) + ' 条视频参与', function () { openTag(t.name); });
  }

  function renderSearchResults() {
    var data = searchState.data || {};
    searchBody.innerHTML = '';
    var list = data[searchState.tab === 'user' ? 'users'
                 : (searchState.tab === 'video' ? 'videos' : 'tags')] || [];
    if (!list.length) {
      searchBody.appendChild(searchEmpty('没有找到相关' +
        searchTabName(searchState.tab) + '，换个关键词试试'));
      return;
    }
    for (var i = 0; i < list.length; ++i) {
      var item = list[i];
      searchBody.appendChild(
        searchState.tab === 'user' ? searchUserRow(item)
          : (searchState.tab === 'video' ? searchVideoRow(item)
            : searchTagRow(item)));
    }
  }

  function runSearch(q, tab) {
    q = String(q || '').trim();
    if (!q) { toast('请输入搜索词', 'err'); return; }
    searchState.q = q;
    if (tab) { searchState.tab = tab; }
    searchEl.hidden = false;
    searchInput.value = q;
    navSearch.value = q;
    pauseAllVideos();
    paintSearchTabs();
    searchBody.innerHTML = '';
    searchBody.appendChild(searchEmpty('搜索「' + q + '」…'));
    api('api/search?q=' + encodeURIComponent(q) + '&type=all')
      .then(function (data) {
        if (searchState.q !== q) { return; }
        searchState.data = data;
        renderSearchResults();
      })
      .catch(function (err) {
        searchBody.innerHTML = '';
        searchBody.appendChild(searchEmpty('搜索失败：' + (err.message || '网络错误')));
      });
  }

  function hideSearch() {
    searchEl.hidden = true;
    var active = feedEl.querySelector('.slide.active video') || state.activeVideo;
    if (active) { playOnly(active); }
  }

  /* ---------- 话题页 ---------- */

  function topicHash(tag) {
    return '#tag=' + encodeURIComponent(tag);
  }

  function tagLink(tag, chip) {
    var a = document.createElement('a');
    a.className = chip ? 'tag-chip' : 'topic-link';
    a.href = topicHash(tag);
    a.textContent = (chip ? '' : '#') + tag;
    a.title = '查看话题 #' + tag;
    a.addEventListener('click', function (e) {
      e.preventDefault();
      e.stopPropagation();
      openTag(tag);
    });
    return a;
  }

  var TAG_RE = /#([^\s#，。！？、；：,.!?;:()（）【】\[\]]{1,20})/g;

  /* 把标题拆成「文本 + #话题链接」；标题没有 # 时保持纯文本，
     话题由后端 tags 走下方的 chips 展示。 */
  function renderTitleInto(el, v) {
    var raw = String(v.title || '');
    if (!raw) {
      el.textContent = '视频 #' + (v.id || '');
      return;
    }
    var m;
    var last = 0;
    var matched = false;
    TAG_RE.lastIndex = 0;
    while ((m = TAG_RE.exec(raw)) !== null) {
      matched = true;
      if (m.index > last) {
        el.appendChild(document.createTextNode(raw.slice(last, m.index)));
      }
      el.appendChild(tagLink(m[1], false));
      last = TAG_RE.lastIndex;
    }
    if (!matched) {
      el.textContent = raw;
      return;
    }
    if (last < raw.length) {
      el.appendChild(document.createTextNode(raw.slice(last)));
    }
  }

  /* 没有显式 #tag 的历史视频，用后端提取的话题渲染成可点击 chips。 */
  function renderChips(box, v) {
    if (/#[^\s#]/.test(String(v.title || ''))) return;
    (v.tags || []).forEach(function (t) { box.appendChild(tagLink(t, true)); });
  }

  function openTag(tag) {
    tag = String(tag || '').replace(/^#/, '').trim();
    if (!tag) { toast('话题为空', 'err'); return; }
    closeComments();
    if (!profileEl.hidden) hideProfile();
    pauseAllVideos();
    tagEl.hidden = false;
    tagHead.textContent = '#' + tag;
    tagBody.scrollTop = 0;
    tagBody.innerHTML = '';
    var loading = document.createElement('div');
    loading.className = 'notice';
    loading.textContent = '加载话题 #' + tag + ' …';
    tagBody.appendChild(loading);
    if (location.hash !== topicHash(tag)) {
      try {
        history.replaceState(null, '', location.pathname + location.search + topicHash(tag));
      } catch (e) { /* 某些 file:// 环境会抛，忽略 */ }
    }
    api('api/tag/' + encodeURIComponent(tag) + '?page=1&page_size=50')
      .then(function (data) { renderTagPage(data); })
      .catch(function (err) {
        tagBody.innerHTML = '';
        var box = document.createElement('div');
        box.className = 'notice';
        box.textContent = '话题加载失败：' + (err.message || '网络错误');
        tagBody.appendChild(box);
      });
  }

  function renderTagPage(data) {
    tagBody.innerHTML = '';
    var hero = document.createElement('div');
    hero.className = 'tag-hero';
    var h3 = document.createElement('h3');
    h3.textContent = '#' + (data.tag || '');
    var meta = document.createElement('div');
    meta.className = 'tag-meta';
    meta.textContent = (data.participants || 0) + ' 人参与 · ' +
                       (data.total || data.video_count || 0) + ' 条视频';
    hero.appendChild(h3);
    hero.appendChild(meta);
    tagBody.appendChild(hero);

    var items = data.items || [];
    if (!items.length) {
      var empty = document.createElement('div');
      empty.className = 'notice';
      empty.textContent = '这个话题下还没有视频';
      tagBody.appendChild(empty);
      return;
    }
    var grid = document.createElement('div');
    grid.className = 'profile-grid';
    items.forEach(function (item) {
      var v = item.video || item;
      var card = document.createElement('button');
      card.type = 'button';
      card.className = 'pv-card';
      var thumb = document.createElement('video');
      thumb.src = videoSrc(v);
      thumb.muted = true;
      thumb.playsInline = true;
      thumb.preload = 'metadata';
      thumb.setAttribute('muted', '');
      thumb.setAttribute('playsinline', '');
      var title = document.createElement('span');
      title.className = 'pv-title';
      title.textContent = v.title || ('视频 #' + (v.id || ''));
      card.appendChild(thumb);
      card.appendChild(title);
      card.addEventListener('click', function () { playInFeed(item, v); });
      grid.appendChild(card);
    });
    tagBody.appendChild(grid);
  }

  function hideTagView() {
    if (tagEl.hidden) return;
    tagEl.hidden = true;
    tagHead.textContent = '话题';
    if ((location.hash || '').indexOf('#tag=') === 0) {
      try {
        history.replaceState(null, '', location.pathname + location.search);
      } catch (e) { /* 忽略 */ }
    }
  }

  /* ---------- 举报 / 拉黑 / 黑名单（第 3 轮） ---------- */

  var menuMaskEl = document.getElementById('menuMask');
  var menuSheetEl = document.getElementById('menuSheet');
  var menuTitleEl = document.getElementById('menuTitle');
  var menuListEl = document.getElementById('menuList');
  var reportMaskEl = document.getElementById('reportMask');
  var reportModalEl = document.getElementById('reportModal');
  var reportTitleEl = document.getElementById('reportTitle');
  var reportReasonsEl = document.getElementById('reportReasons');
  var reportDetailEl = document.getElementById('reportDetail');
  var reportSubmitEl = document.getElementById('reportSubmit');
  var blockPanelEl = document.getElementById('blockPanel');
  var blockListEl = document.getElementById('blockList');
  var blockSubEl = document.getElementById('blockSub');
  var reportState = { type: '', id: '', reason: '广告营销', busy: false };

  /* 底部动作菜单：item = {label, danger, fn}。同一时间只开一个。 */
  function openMenu(title, items) {
    menuTitleEl.textContent = title || '更多操作';
    menuListEl.innerHTML = '';
    (items || []).forEach(function (it) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'menu-item' + (it.danger ? ' danger' : '');
      btn.textContent = it.label;
      btn.addEventListener('click', function () {
        closeMenu();
        if (typeof it.fn === 'function') it.fn();
      });
      menuListEl.appendChild(btn);
    });
    menuMaskEl.hidden = false;
    menuSheetEl.hidden = false;
  }

  function closeMenu() {
    menuMaskEl.hidden = true;
    menuSheetEl.hidden = true;
  }

  /* 举报弹窗：默认选中「广告营销」，切换原因即更新状态。 */
  function openReport(targetType, targetId, label) {
    if (!state.token) { toast('请先登录再举报', 'err'); return; }
    reportState.type = targetType;
    reportState.id = String(targetId || '');
    reportState.reason = '广告营销';
    reportState.busy = false;
    reportTitleEl.textContent = '举报' + (label ? '「' + label + '」' : '');
    reportDetailEl.value = '';
    var btns = reportReasonsEl.querySelectorAll('.reason');
    for (var i = 0; i < btns.length; i++) {
      btns[i].classList.toggle('on', btns[i].dataset.reason === reportState.reason);
    }
    reportMaskEl.hidden = false;
    reportModalEl.hidden = false;
  }

  function closeReport() {
    reportMaskEl.hidden = true;
    reportModalEl.hidden = true;
    reportState.busy = false;
    reportSubmitEl.disabled = false;
  }

  function submitReport() {
    if (reportState.busy || !reportState.type || !reportState.id) return;
    reportState.busy = true;
    reportSubmitEl.disabled = true;
    api('api/report', {
      method: 'POST',
      body: {
        target_type: reportState.type,
        target_id: Number(reportState.id),
        reason: reportState.reason,
        detail: (reportDetailEl.value || '').trim()
      }
    }).then(function (data) {
      closeReport();
      toast(data && data.duplicate
        ? '你已经举报过这条内容，无需重复提交'
        : '举报已提交，我们会尽快处理', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) {
        toast('举报失败：' + (err.message || '网络错误'), 'err');
      }
    }).then(function () {
      reportState.busy = false;
      reportSubmitEl.disabled = false;
    });
  }

  /* 拉黑后立刻从本地列表剔除该作者内容；服务端过滤是下一屏的兜底。 */
  function purgeUserContent(userId) {
    var uid = String(userId);
    if (state.panelSlide) {
      var current = state.items[Number(state.panelSlide.dataset.index)] || {};
      if (String((current.video || {}).user_id) === uid) closeComments();
    }
    var before = state.items.length;
    state.items = state.items.filter(function (item) {
      return String((item.video || {}).user_id) !== uid;
    });
    if (state.items.length !== before) renderFeed();
    if (state.profileUserId === uid) hideProfile();
    if (searchState.data && searchState.data.users) {
      searchState.data.users = searchState.data.users.filter(function (u) {
        return String(u.id) !== uid;
      });
      renderSearchResults();
    }
  }

  /* 拉黑 / 取消拉黑：成功后刷新 feed 与黑名单面板。 */
  function toggleBlock(userId, name, block) {
    if (!state.token) {
      toast('请先登录再拉黑', 'err');
      return Promise.reject(new Error('未登录'));
    }
    var uid = String(userId || '');
    if (!uid || uid === '0') {
      toast('用户信息缺失', 'err');
      return Promise.reject(new Error('用户信息缺失'));
    }
    return api('api/user/' + uid + '/block', { method: block ? 'POST' : 'DELETE' })
      .then(function (data) {
        if (block) {
          toast('已拉黑 @' + (name || uid) + '，相关内容已隐藏', 'ok');
          purgeUserContent(uid);
        } else {
          toast('已取消拉黑 @' + (name || uid), 'ok');
          loadFeed();
        }
        loadBlacklist();
        return data;
      })
      .catch(function (err) {
        if (!handleAuthError(err)) {
          toast((block ? '拉黑' : '取消拉黑') + '失败：' +
            (err.message || '网络错误'), 'err');
        }
        throw err;
      });
  }

  function openBlacklist() {
    if (!state.token) { toast('请先登录', 'err'); return; }
    closeNotifications();
    blockPanelEl.classList.add('open');
    blockPanelEl.setAttribute('aria-hidden', 'false');
    loadBlacklist();
  }

  function closeBlacklist() {
    blockPanelEl.classList.remove('open');
    blockPanelEl.setAttribute('aria-hidden', 'true');
  }

  function loadBlacklist() {
    if (!state.token) return;
    api('api/user/blocks').then(function (data) {
      renderBlacklist(data.items || []);
    }).catch(function (err) {
      if (!handleAuthError(err)) {
        blockListEl.innerHTML = '<div class="cp-empty">黑名单加载失败：' +
          (err.message || '网络错误') + '</div>';
      }
    });
  }

  function renderBlacklist(items) {
    blockSubEl.textContent = items.length + ' 人';
    blockListEl.innerHTML = '';
    if (!items.length) {
      blockListEl.innerHTML = '<div class="cp-empty">还没有拉黑任何人</div>';
      return;
    }
    items.forEach(function (it) {
      var row = document.createElement('div');
      row.className = 'block-row';
      var avatar = document.createElement('span');
      avatar.className = 'avatar';
      avatar.textContent = initial(it.blocked_name || ('用户' + it.blocked_id));
      var info = document.createElement('div');
      info.className = 'block-info';
      var b = document.createElement('b');
      b.textContent = '@' + (it.blocked_name || ('用户 #' + it.blocked_id));
      var t = document.createElement('span');
      t.textContent = '拉黑于 ' + fmtTime(it.created_at);
      info.appendChild(b);
      info.appendChild(t);
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'block-unblock';
      btn.textContent = '取消拉黑';
      btn.addEventListener('click', function () {
        toggleBlock(it.blocked_id, it.blocked_name, false);
      });
      row.appendChild(avatar);
      row.appendChild(info);
      row.appendChild(btn);
      blockListEl.appendChild(row);
    });
  }

  /* 视频操作栏「⋯」：举报视频 / 拉黑作者。 */
  function openVideoMenu(v, name) {
    var uid = String(v.user_id || '');
    var items = [{
      label: '举报视频',
      fn: function () { openReport('video', v.id, v.title || ('视频 #' + v.id)); }
    }];
    if (uid && (!state.user || String(state.user.id) !== uid)) {
      items.push({
        label: '拉黑作者 @' + (name || ('用户 #' + uid)),
        danger: true,
        fn: function () { toggleBlock(uid, name || ('用户 #' + uid), true); }
      });
    }
    openMenu('视频操作', items);
  }

  /* 评论「⋯」：举报评论 / 拉黑评论作者。 */
  function openCommentMenu(c) {
    var uid = String(c.user_id || '');
    var slide = state.panelSlide;
    var items = [{
      label: '举报评论',
      fn: function () { openReport('comment', c.id, '评论'); }
    }];
    if (uid && (!state.user || String(state.user.id) !== uid)) {
      items.push({
        label: '拉黑 @用户 #' + uid,
        danger: true,
        fn: function () {
          toggleBlock(uid, '用户 #' + uid, true).then(function () {
            // 拉黑可能触发 feed 重渲染（评论者也有作品时），旧 slide 会脱离
            // DOM；此时不再重开评论区，避免索引错位打开错误视频。
            if (slide && slide.parentNode) openComments(slide);
          }).catch(function () { /* 失败已 toast */ });
        }
      });
    }
    openMenu('评论操作', items);
  }

  /* ---------- 用户主页三 tab：作品 / 喜欢 / 收藏 ---------- */

  /** 三个 tab 的固定顺序：作品 → 喜欢 → 收藏（与抖音一致）。 */
  function profileTabKeys() {
    return ['works', 'liked', 'favorites'];
  }

  /** 单个 tab 的独立分页状态：游标/已加载项/是否到底都各存一份。 */
  function profileTabState(key, path) {
    return {
      key: key,
      path: path,
      cursor: '',
      items: [],
      total: 0,
      loaded: false,
      loading: false,
      done: false,
      private: false,
      blocked: false,
      error: ''
    };
  }

  /** 空状态文案：本人/他人、三个 tab 各不相同。 */
  function profileEmptyText(key, self) {
    if (key === 'works') {
      return self ? '你还没有发过视频，点右上角「上传」发一条'
                  : '该用户暂时没有视频';
    }
    if (key === 'liked') {
      return '你还没有点赞过视频，在 feed 里双击红心试试';
    }
    return '你还没有收藏视频，点视频右侧的 ⭐ 收藏';
  }

  /** 居中提示块（锁 / 拉黑 / 空状态共用）。 */
  function profileHint(icon, text) {
    var box = document.createElement('div');
    box.className = 'p-hint';
    if (icon) {
      var span = document.createElement('span');
      span.className = 'lock';
      span.textContent = icon;
      box.appendChild(span);
    }
    var line = document.createElement('div');
    line.textContent = text;
    box.appendChild(line);
    return box;
  }

  /** 主页网格里的一张视频卡（与原作品卡片一致，点击走 feed 播放）。 */
  function profileVideoCard(item) {
    var v = item.video || item;
    var cardBtn = document.createElement('button');
    cardBtn.type = 'button';
    cardBtn.className = 'pv-card';
    cardBtn.dataset.id = String(v.id || 0);
    var pv = document.createElement('video');
    pv.src = videoSrc(v);
    pv.muted = true;
    pv.playsInline = true;
    pv.preload = 'metadata';
    pv.setAttribute('muted', '');
    pv.setAttribute('playsinline', '');
    var pt = document.createElement('span');
    pt.className = 'pv-title';
    pt.textContent = v.title || ('视频 #' + (v.id || ''));
    cardBtn.appendChild(pv);
    cardBtn.appendChild(pt);
    cardBtn.addEventListener('click', function () { playInFeed(item, v); });
    return cardBtn;
  }

  /** 重绘 tab 栏：选中态 + 计数（私密 tab 显示锁，未加载不显示数字）。 */
  function paintProfileTabs(view) {
    var keys = profileTabKeys();
    for (var i = 0; i < keys.length; i++) {
      var key = keys[i];
      var tab = view.tabs[key];
      var btn = view.tabBar.querySelector('[data-tab="' + key + '"]');
      if (!btn) { continue; }
      btn.classList.toggle('on', key === view.active);
      btn.setAttribute('aria-selected', key === view.active ? 'true' : 'false');
      var count = btn.querySelector('b');
      if (!count) { continue; }
      if (tab.private) {
        count.textContent = '🔒';
      } else if (tab.loaded && !tab.blocked) {
        count.textContent = String(tab.total || 0);
      } else {
        count.textContent = '';
      }
    }
    if (view.statsVideos && view.tabs.works.loaded && !view.tabs.works.blocked) {
      view.statsVideos.textContent = fmt(view.tabs.works.total);
    }
  }

  /** 重绘当前 tab 的面板：加载中 / 错误 / 私密 / 拉黑 / 空 / 网格 + 加载更多。 */
  function paintProfilePanel(view) {
    var tab = view.tabs[view.active];
    var panel = view.panel;
    panel.innerHTML = '';
    paintProfileTabs(view);
    if (!tab) { return; }
    if (!tab.loaded && !tab.error) {
      var loading = document.createElement('div');
      loading.className = 'profile-note';
      loading.textContent = '加载中…';
      panel.appendChild(loading);
      return;
    }
    if (tab.error) {
      var failed = profileHint('⚠️', '加载失败：' + tab.error);
      var retry = document.createElement('button');
      retry.type = 'button';
      retry.className = 'p-load-more';
      retry.textContent = '重试';
      retry.addEventListener('click', function () {
        loadProfileTab(view, view.active, true);
      });
      failed.appendChild(retry);
      panel.appendChild(failed);
      return;
    }
    if (tab.private) {
      panel.appendChild(profileHint(
        '🔒',
        view.active === 'liked' ? '喜欢列表仅本人可见' : '收藏列表仅本人可见'));
      return;
    }
    if (tab.blocked) {
      panel.appendChild(profileHint('🚫', '由于拉黑设置，TA 的内容不会展示'));
      return;
    }
    if (!tab.items.length) {
      panel.appendChild(profileHint('', profileEmptyText(view.active, view.self)));
      return;
    }
    var grid = document.createElement('div');
    grid.className = 'profile-grid';
    tab.items.forEach(function (item) { grid.appendChild(profileVideoCard(item)); });
    panel.appendChild(grid);
    var note = document.createElement('div');
    note.className = 'profile-note';
    note.textContent = '共 ' + (tab.total || tab.items.length) + ' 条 · 点击播放' +
                       (tab.done ? '' : ' · 滚动到底加载更多');
    panel.appendChild(note);
    if (!tab.done) {
      var more = document.createElement('button');
      more.type = 'button';
      more.className = 'p-load-more';
      more.textContent = '加载更多';
      more.addEventListener('click', function () {
        loadProfileTab(view, view.active, false);
      });
      panel.appendChild(more);
    }
  }

  /**
   * 拉取某个 tab 的一页。
   * @param view 主页视图对象（闭包持有 DOM 引用）。
   * @param key works / liked / favorites。
   * @param reset true=从第一页重来（首次打开或重试）。
   */
  function loadProfileTab(view, key, reset) {
    var tab = view.tabs[key];
    if (!tab || tab.loading) { return; }
    if (!reset && tab.done) { return; }
    tab.loading = true;
    tab.error = '';
    if (reset) {
      tab.cursor = '';
      tab.items = [];
      tab.loaded = false;
      tab.done = false;
      tab.private = false;
      tab.blocked = false;
    }
    var url = tab.path + '?limit=20';
    if (tab.cursor) { url += '&cursor=' + encodeURIComponent(tab.cursor); }
    api(url).then(function (data) {
      tab.loading = false;
      tab.loaded = true;
      tab.private = !!data.private;
      tab.blocked = !!data.blocked;
      tab.total = Number(data.total || 0);
      tab.items = reset ? (data.items || []) : tab.items.concat(data.items || []);
      tab.cursor = data.next_cursor || '';
      tab.done = !tab.cursor;
      /* 主页已被关闭/替换：只保留数据，不碰已经脱离 DOM 的节点。 */
      if (state.profileView !== view) { return; }
      if (view.active === key) { paintProfilePanel(view); }
      paintProfileTabs(view);
    }).catch(function (err) {
      tab.loading = false;
      tab.error = err.message || '网络错误';
      if (state.profileView !== view) { return; }
      if (view.active === key) { paintProfilePanel(view); }
    });
  }

  function renderProfile(user, isFollowing) {
    var name = user.name || ('user' + (user.id || ''));
    var isVip = !!(user.vip || user.is_vip || Number(user.vip_level) > 0);
    var self = !!(state.user && String(state.user.id) === String(user.id));
    profileBody.innerHTML = '';

    var card = document.createElement('div');
    card.className = 'profile-card';
    var bigAvatar = document.createElement('span');
    bigAvatar.className = 'avatar';
    bigAvatar.textContent = initial(name);
    var info = document.createElement('div');
    info.className = 'p-info';
    var h3 = document.createElement('h3');
    h3.textContent = name;
    var email = document.createElement('div');
    email.className = 'p-line';
    email.textContent = '邮箱：' + (user.email || '—');
    var created = document.createElement('div');
    created.className = 'p-line';
    created.textContent = '注册时间：' + fmtTime(user.created_at);

    /* 数据行：作品 / 关注 / 粉丝（作品数等 works tab 加载完再填） */
    var stats = document.createElement('div');
    stats.className = 'p-stats';
    stats.innerHTML = '<span><b class="p-videos">—</b>作品</span>' +
                      '<span><b class="p-following"></b>关注</span>' +
                      '<span><b class="p-followers"></b>粉丝</span>';
    stats.querySelector('.p-following').textContent = fmt(user.following);
    stats.querySelector('.p-followers').textContent = fmt(user.followers);

    var vip = document.createElement('span');
    vip.className = 'vip' + (isVip ? '' : ' off');
    vip.textContent = isVip ? 'VIP 会员' : '普通用户（非 VIP）';

    info.appendChild(h3);
    info.appendChild(email);
    info.appendChild(created);
    info.appendChild(stats);
    info.appendChild(vip);

    if (self) {
      var tag = document.createElement('span');
      tag.className = 'p-self-tag';
      tag.textContent = '这是你的主页';
      info.appendChild(tag);
    } else {
      var actions = document.createElement('div');
      actions.className = 'p-actions';
      var followBtn = document.createElement('button');
      followBtn.type = 'button';
      followBtn.className = 'follow-btn' + (isFollowing ? ' following' : '');
      followBtn.textContent = isFollowing ? '已关注' : '+ 关注';
      followBtn.addEventListener('click', function () {
        toggleFollow(user, followBtn, stats);
      });
      actions.appendChild(followBtn);
      var profileMore = document.createElement('button');
      profileMore.type = 'button';
      profileMore.className = 'btn-ghost p-more-btn';
      profileMore.textContent = '⋯';
      profileMore.title = '举报 / 拉黑';
      profileMore.addEventListener('click', function () {
        openMenu('用户操作', [{
          label: '举报该用户',
          fn: function () { openReport('user', user.id, name); }
        }, {
          label: user.is_blocked ? '取消拉黑' : '拉黑该用户',
          danger: !user.is_blocked,
          fn: function () {
            toggleBlock(user.id, name, !user.is_blocked).then(function () {
              showProfile(user.id, name);
            }).catch(function () { /* 失败已 toast */ });
          }
        }]);
      });
      actions.appendChild(profileMore);
      info.appendChild(actions);
    }

    card.appendChild(bigAvatar);
    card.appendChild(info);
    profileBody.appendChild(card);

    /* 三 tab：作品 / 喜欢 / 收藏。每个 tab 各自游标与列表，切换互不影响；
       「喜欢」「收藏」的可见性由后端判定（他人拿到的就是空列表 + private），
       前端只按标记渲染锁，不自行决定隐私。 */
    var uid = String(user.id);
    var view = {
      user: user,
      self: self,
      active: 'works',
      statsVideos: stats.querySelector('.p-videos'),
      tabs: {
        works: profileTabState('works', 'api/user/' + uid + '/videos'),
        liked: profileTabState('liked', 'api/user/' + uid + '/liked'),
        favorites: profileTabState('favorites', 'api/user/' + uid + '/favorites')
      }
    };

    var labels = { works: '作品', liked: '喜欢', favorites: '收藏' };
    var bar = document.createElement('div');
    bar.className = 'p-tabs';
    bar.setAttribute('role', 'tablist');
    profileTabKeys().forEach(function (key) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'p-tab';
      btn.setAttribute('role', 'tab');
      btn.dataset.tab = key;
      btn.textContent = labels[key];
      btn.appendChild(document.createElement('b'));
      btn.addEventListener('click', function () {
        if (view.active !== key) {
          view.active = key;
          paintProfilePanel(view);
        }
        var tab = view.tabs[key];
        if (!tab.loaded && !tab.loading) { loadProfileTab(view, key, true); }
      });
      bar.appendChild(btn);
    });
    view.tabBar = bar;
    profileBody.appendChild(bar);

    var panel = document.createElement('div');
    panel.className = 'p-panel';
    view.panel = panel;
    profileBody.appendChild(panel);
    state.profileView = view;

    /* 拉黑后主页只显示「已拉黑」和取消入口，不再请求视频列表——否则
       拉黑等于只挡住 feed，从主页还能绕进去。 */
    if (!self && user.is_blocked) {
      view.tabs.works.loaded = true;
      view.tabs.works.blocked = true;
      paintProfileTabs(view);
      paintProfilePanel(view);
      var unblockBtn = document.createElement('button');
      unblockBtn.type = 'button';
      unblockBtn.className = 'btn-primary';
      unblockBtn.textContent = '取消拉黑';
      unblockBtn.addEventListener('click', function () {
        toggleBlock(uid, name, false).then(function () {
          showProfile(uid, name);
        }).catch(function () { /* 失败已 toast */ });
      });
      profileBody.appendChild(unblockBtn);
      return;
    }

    paintProfileTabs(view);
    paintProfilePanel(view);
    loadProfileTab(view, 'works', true);
  }

  function showProfile(userId, fallbackName) {
    if (!userId) { toast('作者信息缺失', 'err'); return; }
    state.profileUserId = String(userId);
    closeComments();
    pauseAllVideos();
    profileEl.hidden = false;
    profileEl.scrollTop = 0;
    profileBody.innerHTML = '';
    var loading = document.createElement('div');
    loading.className = 'notice';
    loading.textContent = '加载 ' + (fallbackName ? '@' + fallbackName : '用户') + ' 的主页…';
    profileBody.appendChild(loading);
    api('api/user/' + state.profileUserId)
      .then(function (data) {
        renderProfile(data.user || {}, !!data.is_following);
      })
      .catch(function (err) {
        profileBody.innerHTML = '';
        var box = document.createElement('div');
        box.className = 'notice';
        box.textContent = '用户信息加载失败：' + (err.message || '网络错误');
        var back = document.createElement('button');
        back.type = 'button';
        back.className = 'btn-primary';
        back.textContent = '返回 feed';
        back.addEventListener('click', hideProfile);
        box.appendChild(document.createElement('br'));
        box.appendChild(back);
        profileBody.appendChild(box);
      });
  }

  function hideProfile() {
    profileEl.hidden = true;
    state.profileUserId = '';
    state.profileView = null;
    var active = feedEl.querySelector('.slide.active video') || state.activeVideo;
    if (active) playOnly(active);
  }

  /* ---------- 主页点视频：切回 feed，复用同一个播放器 ---------- */

  /* 以前这里弹独立 <video> 播放器，和 feed 是两条路径：一旦那条路径出问题
     （地址前缀、解码、自动播放策略）就只剩黑屏，而 feed 明明能播。现在统一
     走 feed：视频不在当前 feed 里就先补进去，滚动到该屏再用 playOnly 播放。 */
  function playInFeed(item, v) {
    v = v || (item && item.video) || {};
    var id = String(v.id || 0);
    if (!id || id === '0') { toast('视频信息缺失', 'err'); return; }
    var known = false;
    for (var i = 0; i < state.items.length; i++) {
      if (String((state.items[i].video || {}).id) === id) { known = true; break; }
    }
    if (!known) {
      state.items.unshift(item && item.video
        ? item
        : { video: v, author: authorName(item || { video: v }) });
      renderFeed();
    }
    hideProfile();
    hideTagView();
    hideFavorites();
    var slide = feedEl.querySelector('.slide[data-id="' + id + '"]');
    if (!slide) { toast('视频未找到', 'err'); return; }
    slide.classList.add('active');
    slide.scrollIntoView({ block: 'start' });
    var video = slide.querySelector('video');
    if (!video) { toast('播放器初始化失败', 'err'); return; }
    video.currentTime = 0;
    playOnly(video);
    toast('正在播放：' + (v.title || ('视频 #' + id)), 'ok');
  }

  /* ---------- 事件绑定与启动 ---------- */

  document.getElementById('blacklistBtn').addEventListener('click', openBlacklist);
  document.getElementById('blockClose').addEventListener('click', closeBlacklist);
  document.getElementById('menuClose').addEventListener('click', closeMenu);
  document.getElementById('menuCancel').addEventListener('click', closeMenu);
  document.getElementById('reportClose').addEventListener('click', closeReport);
  document.getElementById('reportCancel').addEventListener('click', closeReport);
  document.getElementById('reportSubmit').addEventListener('click', submitReport);
  menuMaskEl.addEventListener('click', closeMenu);
  reportMaskEl.addEventListener('click', closeReport);
  reportReasonsEl.addEventListener('click', function (e) {
    var btn = e.target && e.target.closest ? e.target.closest('.reason') : null;
    if (!btn) return;
    reportState.reason = btn.dataset.reason || '其它';
    var all = reportReasonsEl.querySelectorAll('.reason');
    for (var i = 0; i < all.length; i++) {
      all[i].classList.toggle('on', all[i] === btn);
    }
  });

  document.getElementById('loginBtn').addEventListener('click', login);
  document.getElementById('uploadBtn').addEventListener('click', function () {
    if (!state.token) { toast('请先登录', 'err'); return; }
    uploadInput.click();
  });
  uploadInput.addEventListener('change', function () {
    var file = uploadInput.files && uploadInput.files[0];
    uploadInput.value = '';
    if (file) uploadVideo(file);
  });
  document.getElementById('logoutBtn').addEventListener('click', logout);
  document.getElementById('password').addEventListener('keydown', function (e) {
    if (e.key === 'Enter') login();
  });
  document.getElementById('cpClose').addEventListener('click', closeComments);
  backdropEl.addEventListener('click', function () {
    closeNotifications();
    closeComments();
  });
  document.getElementById('cpForm').addEventListener('submit', function (e) {
    e.preventDefault();
    sendComment();
  });
  document.getElementById('profileBack').addEventListener('click', hideProfile);
  /* 主页 tab 的「滚动到底加载更多」：只在当前 tab 还有下一页时触发；
     滚动事件里不做 DOM 重建，交给 loadProfileTab 的响应回调统一重绘。 */
  profileEl.addEventListener('scroll', function () {
    var view = state.profileView;
    if (!view) { return; }
    var tab = view.tabs[view.active];
    if (!tab || !tab.loaded || tab.loading || tab.done ||
        tab.private || tab.blocked) {
      return;
    }
    if (profileEl.scrollTop + profileEl.clientHeight + 260 >= profileEl.scrollHeight) {
      loadProfileTab(view, view.active, false);
    }
  });
  document.getElementById('tagBack').addEventListener('click', hideTagView);
  document.getElementById('searchBack').addEventListener('click', hideSearch);
  document.getElementById('searchSubmit').addEventListener('click', function () {
    runSearch(searchInput.value, searchState.tab);
  });
  searchInput.addEventListener('keydown', function (e) {
    if (e.key !== 'Enter') { return; }
    e.preventDefault();
    runSearch(searchInput.value, searchState.tab);
  });
  document.getElementById('navSearchBtn').addEventListener('click', function () {
    runSearch(navSearch.value, 'user');
  });
  navSearch.addEventListener('keydown', function (e) {
    if (e.key !== 'Enter') { return; }
    e.preventDefault();
    runSearch(navSearch.value, 'user');
  });
  searchTabs.addEventListener('click', function (e) {
    var btn = e.target.closest('.search-tab');
    if (!btn) { return; }
    searchState.tab = btn.dataset.tab;
    paintSearchTabs();
    renderSearchResults();
  });

  document.getElementById('mineBtn').addEventListener('click', function () {
    if (!state.user) { toast('请先登录', 'err'); return; }
    showProfile(state.user.id, state.user.name);
  });
  document.getElementById('favBtn').addEventListener('click', showFavorites);
  document.getElementById('notifBtn').addEventListener('click', openNotifications);
  document.getElementById('notifClose').addEventListener('click', closeNotifications);
  document.getElementById('notifAllRead').addEventListener('click', markAllNotificationsRead);
  notifMoreBtn.addEventListener('click', function () { loadNotifications(false); });
  setInterval(function () {
    if (state.token) refreshUnreadNotifications();
  }, 30000);
  document.getElementById('favBack').addEventListener('click', function () {
    hideFavorites();
    var active = feedEl.querySelector('.slide.active video') || state.activeVideo;
    if (active) playOnly(active);
  });
  document.getElementById('ssClose').addEventListener('click', closeShare);
  document.getElementById('ssCancel').addEventListener('click', closeShare);
  shareMask.addEventListener('click', closeShare);
  ssGrid.addEventListener('click', function (e) {
    var btn = e.target.closest('[data-act]');
    if (!btn) return;
    var act = btn.getAttribute('data-act');
    if (act === 'copy') { copyVideoLink(shareVideoId); return; }
    if (act === 'qr') { ssQr.hidden = !ssQr.hidden; return; }
    toast('分享到' + (act === 'wx' ? '微信好友' : '朋友圈') +
          '：演示占位，未接入第三方', 'ok');
  });
  document.getElementById('who').addEventListener('click', function () {
    if (!state.user) return;
    showProfile(state.user.id, state.user.name);
  });
  var volSlider = document.getElementById('volumeSlider');
  volSlider.addEventListener('input', function () {
    state.volume = Math.max(0, Math.min(1, Number(volSlider.value) / 100));
    try { localStorage.setItem(VOL_KEY, String(state.volume)); } catch (e) {}
    if (state.volume > 0 && state.muted) {
      setMuted(false, true);
    } else {
      applySound();
    }
  });
  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape') return;
    if (!reportModalEl.hidden) { closeReport(); return; }
    if (!menuSheetEl.hidden) { closeMenu(); return; }
    if (blockPanelEl.classList.contains('open')) { closeBlacklist(); return; }
    if (!shareSheet.hidden) { closeShare(); return; }
    if (!favViewEl.hidden) { hideFavorites(); return; }
    if (!tagEl.hidden) { hideTagView(); return; }
    if (!profileEl.hidden) { hideProfile(); return; }
    if (notifPanelEl.classList.contains('open')) { closeNotifications(); return; }
    if (panelEl.classList.contains('open')) closeComments();
  });

  if (state.token && state.user) {
    setSignedIn(state.user);
  }
  applySound();
  loadFeed();
  (function () {
    var m = /^#tag=(.+)$/.exec(location.hash || '');
    if (m) {
      try { openTag(decodeURIComponent(m[1])); } catch (e) { /* 忽略非法 hash */ }
    }
  })();
})();
</script>
</body>
</html>)HTML";

}  // namespace douyin

#endif  // DOUYIN_HOME_PAGE_HPP
