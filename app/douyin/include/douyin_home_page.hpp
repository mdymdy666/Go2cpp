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
 *          - 评论图标打开右侧抽屉，`GET api/comment/<id>?limit=50` 拉列表，
 *            登录后 `POST api/comment` 发送并插入列表头、同步计数；
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
    display: flex; gap: 8px; padding: 12px 16px calc(12px + env(safe-area-inset-bottom));
    border-top: 1px solid var(--line); background: #171721;
  }
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

  /* ---------- 主页视频弹层 ---------- */
  .video-modal {
    position: fixed; inset: 0; z-index: 80;
    display: grid; place-items: center;
    background: rgba(0, 0, 0, .94);
  }
  .video-modal video {
    width: min(100vw, 920px); max-height: 86vh;
    background: #000; object-fit: contain;
  }
  .video-modal .vm-close {
    position: absolute; top: 16px; right: 18px;
    width: 38px; height: 38px; border-radius: 50%;
    background: rgba(255, 255, 255, .12); color: #fff; font-size: 17px;
  }
  .video-modal .vm-title {
    position: absolute; left: 20px; bottom: 20px; right: 20px;
    font-size: 14px; color: #e7e8ef; text-shadow: 0 1px 8px #000;
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
  <div class="auth">
    <span class="who" id="who"><span class="dot"></span><b id="whoName"></b></span>
    <input class="login-only" id="email" type="email" value="admin@demo.local"
           placeholder="邮箱" autocomplete="username" spellcheck="false">
    <input class="login-only" id="password" type="password" value="123456"
           placeholder="密码" autocomplete="current-password">
    <button class="btn-primary login-only" id="loginBtn" type="button">登录</button>
    <input type="file" id="uploadInput" accept="video/mp4" hidden>
    <button class="btn-ghost upload-btn" id="uploadBtn" type="button">上传</button>
    <button class="btn-ghost" id="logoutBtn" type="button" style="display:none">退出</button>
  </div>
</header>

<div class="toast" id="toast" role="status" aria-live="polite"></div>

<main class="feed" id="feed" aria-label="推荐视频流"></main>

<div class="backdrop" id="backdrop"></div>
<aside class="comment-panel" id="comment-panel" aria-label="评论面板" aria-hidden="true">
  <div class="cp-head">
    <div class="cp-titles">
      <b id="cpTitle">评论</b>
      <span id="cpCount">0 条评论</span>
    </div>
    <button class="cp-close" id="cpClose" type="button" aria-label="关闭评论">✕</button>
  </div>
  <div class="cp-list" id="cpList"><div class="cp-empty">加载中…</div></div>
  <form class="cp-form" id="cpForm">
    <input id="cpInput" type="text" maxlength="500" placeholder="说点什么…" autocomplete="off">
    <button id="cpSend" type="submit">发送</button>
  </form>
</aside>

<section class="profile-view" id="profileView" hidden aria-label="用户主页">
  <div class="profile-top">
    <button class="back" id="profileBack" type="button">← 返回</button>
    <h2>用户主页</h2>
  </div>
  <div class="profile-body" id="profileBody"></div>
</section>

<div class="video-modal" id="videoModal" hidden>
  <button class="vm-close" id="vmClose" type="button" aria-label="关闭">✕</button>
  <video id="modalVideo" muted loop playsinline webkit-playsinline></video>
  <div class="vm-title" id="vmTitle"></div>
</div>

<script>
(function () {
  'use strict';

  /* 公网前缀可能被访问成 /douyin（无尾斜杠），此时相对路径会解析到根路径；
     先补一个尾斜杠再继续，保证 api/ 与 videos/ 都落在同一前缀下。 */
  if (location.pathname.slice(-1) !== '/' &&
      !/\.[a-z0-9]+$/i.test(location.pathname)) {
    location.replace(location.pathname + '/' + location.search + location.hash);
  }

  var TOKEN_KEY = 'douyin_token';
  var USER_KEY = 'douyin_user';
  var LIKES_KEY = 'douyin_likes';
  var FIT_KEY = 'douyin_fit';

  var state = {
    token: localStorage.getItem(TOKEN_KEY) || '',
    user: null,
    likes: {},
    items: [],
    observer: null,
    activeVideo: null,
    panelVideoId: '',
    panelSlide: null,
    profileUserId: '',
    modalOpen: false,
    fit: 'contain'
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
  var profileEl = document.getElementById('profileView');
  var profileBody = document.getElementById('profileBody');
  var modalEl = document.getElementById('videoModal');
  var modalVideo = document.getElementById('modalVideo');
  var modalTitle = document.getElementById('vmTitle');
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
    return fetch(path, {
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
        if (state.profileUserId === String(state.user.id) && !profileEl.hidden) {
          renderProfile(state.user);
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

  function toggleLike(slide) {
    if (!state.token) { toast('请先在右上角登录，再点赞', 'err'); return; }
    var id = slide.dataset.id;
    var liked = !!state.likes[id];
    var btn = slide.querySelector('.like-btn');
    if (btn.dataset.busy === '1') return;
    btn.dataset.busy = '1';
    api('api/like', {
      method: liked ? 'DELETE' : 'POST',
      body: { video_id: Number(id) }
    }).then(function (data) {
      paintLike(slide, !liked, data.like_count);
      var stat = slide.querySelector('.stat-like');
      if (stat) stat.textContent = fmt(data.like_count);
      toast(liked ? '已取消点赞' : '点赞成功', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) toast('点赞失败：' + (err.message || '网络错误'), 'err');
    }).then(function () { btn.dataset.busy = ''; });
  }

  /* ---------- 评论面板 ---------- */

  function commentItem(c) {
    var row = document.createElement('div');
    row.className = 'cp-item';
    var who = document.createElement('div');
    who.className = 'cp-who';
    var uid = document.createElement('b');
    uid.textContent = '用户 #' + (c.user_id === undefined ? '—' : c.user_id);
    who.appendChild(uid);
    who.appendChild(document.createTextNode(' · ' + fmtTime(c.created_at)));
    var txt = document.createElement('div');
    txt.className = 'cp-txt';
    txt.textContent = c.content || '';
    row.appendChild(who);
    row.appendChild(txt);
    return row;
  }

  function renderComments(items) {
    cpList.innerHTML = '';
    var list = (items || []).map(function (it) { return it.comment || it; });
    if (!list.length) {
      var empty = document.createElement('div');
      empty.className = 'cp-empty';
      empty.textContent = '还没有评论，来抢沙发';
      cpList.appendChild(empty);
      return;
    }
    list.forEach(function (c) { cpList.appendChild(commentItem(c)); });
  }

  function openComments(slide) {
    var item = state.items[Number(slide.dataset.index)] || {};
    var v = item.video || {};
    state.panelVideoId = String(v.id || slide.dataset.id);
    state.panelSlide = slide;
    cpTitle.textContent = v.title || ('视频 #' + state.panelVideoId);
    cpCount.textContent = fmt(v.comment_count) + ' 条评论';
    cpList.innerHTML = '<div class="cp-empty">加载中…</div>';
    panelEl.classList.add('open');
    panelEl.setAttribute('aria-hidden', 'false');
    backdropEl.classList.add('show');
    api('api/comment/' + state.panelVideoId + '?limit=50')
      .then(function (data) { renderComments(data.items); })
      .catch(function (err) {
        cpList.innerHTML = '<div class="cp-empty">评论加载失败：' +
          (err.message || '网络错误') + '</div>';
      });
    setTimeout(function () { cpInput.focus(); }, 120);
  }

  function closeComments() {
    panelEl.classList.remove('open');
    panelEl.setAttribute('aria-hidden', 'true');
    backdropEl.classList.remove('show');
    state.panelSlide = null;
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
      var c = (data && data.comment) || {
        user_id: state.user ? state.user.id : 0,
        content: content,
        created_at: Math.floor(Date.now() / 1000)
      };
      var empty = cpList.querySelector('.cp-empty');
      if (empty) cpList.removeChild(empty);
      cpList.insertBefore(commentItem(c), cpList.firstChild);
      bumpCommentCount(state.panelVideoId);
      toast('评论已发送', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) toast('评论失败：' + (err.message || '网络错误'), 'err');
    }).then(function () { cpSend.disabled = false; });
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
      p.catch(function () { /* 浏览器策略拒绝时保持静音状态，不弹错 */ });
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
  }

  function uploadVideo(file) {
    var title = file.name || '未命名视频';
    var xhr = new XMLHttpRequest();
    xhr.open('POST', 'api/video/upload?title=' + encodeURIComponent(title));
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

  /* 上传的视频用服务端返回的 videos/upload_* 相对路径；其余沿用 1..3 演示源。 */
  function videoSrc(v) {
    v = v || {};
    var url = String(v.url || '');
    if (/^videos\/[A-Za-z0-9_.-]+$/.test(url)) return url;
    return 'videos/' + ((Number(v.id) % 3) + 1) + '.mp4';
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
    video.muted = true;
    video.loop = true;
    video.playsInline = true;
    video.preload = 'metadata';
    video.setAttribute('muted', '');
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
    title.textContent = v.title || ('视频 #' + id);

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

    meta.appendChild(title);
    meta.appendChild(authorRow);
    meta.appendChild(stats);
    meta.appendChild(fitBtn);

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

    rail.appendChild(railAvatar);
    rail.appendChild(likeBtn);
    rail.appendChild(cmtBtn);

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

    slide.addEventListener('click', function (e) {
      if (e.target.closest('.rail') || e.target.closest('.meta') ||
          e.target.closest('button') || e.target.closest('.author-link')) {
        return;
      }
      togglePlay(slide);
    });

    slide.appendChild(video);
    slide.appendChild(shade);
    slide.appendChild(hint);
    slide.appendChild(meta);
    slide.appendChild(rail);
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

  function loadFeed() {
    showSkeleton();
    api('api/feed?limit=20')
      .then(function (data) {
        var items = data.items || [];
        state.items = items;
        feedEl.innerHTML = '';
        if (!items.length) {
          showNotice('还没有视频，先跑 scripts/seed_bulk.sh 灌数据', true);
          return;
        }
        var frag = document.createDocumentFragment();
        items.forEach(function (item, i) { frag.appendChild(buildSlide(item, i)); });
        feedEl.appendChild(frag);
        feedEl.scrollTop = 0;
        setupObserver();
      })
      .catch(function (err) {
        showNotice('feed 加载失败：' + (err.message || '网络错误'), true);
      });
  }

  /* ---------- 用户主页 ---------- */

  function renderProfile(user) {
    var name = user.name || ('user' + (user.id || ''));
    var isVip = !!(user.vip || user.is_vip || Number(user.vip_level) > 0);
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
    var vip = document.createElement('span');
    vip.className = 'vip' + (isVip ? '' : ' off');
    vip.textContent = isVip ? 'VIP 会员' : '普通用户（非 VIP）';
    info.appendChild(h3);
    info.appendChild(email);
    info.appendChild(created);
    info.appendChild(vip);
    card.appendChild(bigAvatar);
    card.appendChild(info);
    profileBody.appendChild(card);

    var grid = document.createElement('div');
    grid.className = 'profile-grid';
    profileBody.appendChild(grid);
    var note = document.createElement('div');
    note.className = 'profile-note';
    note.textContent = '加载视频中…';
    profileBody.appendChild(note);

    var uid = String(user.id);
    api('api/feed?limit=100').then(function (data) {
      var all = data.items || [];
      var mine = all.filter(function (it) {
        return String((it.video || {}).user_id) === uid;
      }).slice(0, 30);
      grid.innerHTML = '';
      if (!mine.length) {
        note.textContent = '该用户暂时没有视频';
        return;
      }
      note.textContent = '共 ' + mine.length + ' 条视频 · 点击播放';
      mine.forEach(function (item) {
        var v = item.video || {};
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
        cardBtn.addEventListener('click', function () { openVideoModal(v, item); });
        grid.appendChild(cardBtn);
      });
    }).catch(function (err) {
      note.textContent = '视频列表加载失败：' + (err.message || '网络错误');
    });
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
      .then(function (data) { renderProfile(data.user || {}); })
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
    var active = feedEl.querySelector('.slide.active video') || state.activeVideo;
    if (active) playOnly(active);
  }

  /* ---------- 主页视频弹层 ---------- */

  function openVideoModal(v, item) {
    state.modalOpen = true;
    modalTitle.textContent = (v.title || ('视频 #' + v.id)) +
      ' · @' + authorName(item || { video: v });
    modalVideo.src = videoSrc(v);
    modalEl.hidden = false;
    var p = modalVideo.play();
    if (p && p.catch) p.catch(function () {});
  }

  function closeVideoModal() {
    state.modalOpen = false;
    modalEl.hidden = true;
    modalVideo.pause();
    modalVideo.removeAttribute('src');
    modalVideo.load();
  }

  /* ---------- 事件绑定与启动 ---------- */

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
  backdropEl.addEventListener('click', closeComments);
  document.getElementById('cpForm').addEventListener('submit', function (e) {
    e.preventDefault();
    sendComment();
  });
  document.getElementById('profileBack').addEventListener('click', hideProfile);
  document.getElementById('vmClose').addEventListener('click', closeVideoModal);
  modalEl.addEventListener('click', function (e) {
    if (e.target === modalEl) closeVideoModal();
  });
  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape') return;
    if (state.modalOpen) { closeVideoModal(); return; }
    if (!profileEl.hidden) { hideProfile(); return; }
    if (panelEl.classList.contains('open')) closeComments();
  });

  if (state.token && state.user) {
    setSignedIn(state.user);
  }
  loadFeed();
})();
</script>
</body>
</html>)HTML";

}  // namespace douyin

#endif  // DOUYIN_HOME_PAGE_HPP
