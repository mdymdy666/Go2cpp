/**
 * @file douyin_home_page.hpp
 * @brief 抖音首页（feed 播放页）的内联 HTML 常量。
 *
 * @details 页面完全自包含：内联 CSS / JS，不引用任何外部 CDN、字体或图片。
 *          所有接口请求都用相对路径（`api/...`、`videos/...`），因此同一份页面
 *          在本机 `http://host:28080/` 与公网 `http://host:8443/douyin/` 前缀下
 *          都能工作。视频源按 `(id % 3) + 1` 映射到 `videos/1..3.mp4`，
 *          由网关的同源静态路由直接返回，不依赖数据库里的 url 字段。
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

/// @brief 首页 HTML（抖音风格 feed 单页）；只读常量，进程生命周期内有效。
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
    --bg: #0a0a0f;
    --card: #15151c;
    --line: rgba(255, 255, 255, .07);
    --text: #f2f3f7;
    --muted: #8d90a1;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; padding: 0; }
  body {
    min-height: 100vh;
    background: radial-gradient(1200px 600px at 50% -10%, #1b1430 0%, var(--bg) 55%) fixed;
    color: var(--text);
    font: 15px/1.6 -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
          "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
    -webkit-font-smoothing: antialiased;
  }

  /* 顶部栏：品牌 + 登录态 */
  .topbar {
    position: sticky; top: 0; z-index: 20;
    display: flex; align-items: center; gap: 14px; flex-wrap: wrap;
    padding: 12px 22px;
    background: rgba(10, 10, 15, .78);
    backdrop-filter: blur(14px);
    border-bottom: 1px solid var(--line);
  }
  .brand {
    font-size: 19px; font-weight: 800; letter-spacing: .3px;
    background: linear-gradient(92deg, var(--cyan), #fff 45%, var(--pink));
    -webkit-background-clip: text; background-clip: text; color: transparent;
    white-space: nowrap;
  }
  .brand small {
    display: block; font-size: 11px; font-weight: 500; letter-spacing: 1.2px;
    color: var(--muted); -webkit-text-fill-color: var(--muted); margin-top: -2px;
  }
  .auth { display: flex; align-items: center; gap: 8px; margin-left: auto; flex-wrap: wrap; }
  .auth input {
    width: 176px; padding: 8px 11px;
    background: #1c1c25; color: var(--text);
    border: 1px solid var(--line); border-radius: 9px;
    font-size: 13px; outline: none; transition: border-color .16s, box-shadow .16s;
  }
  .auth input:focus { border-color: rgba(37, 244, 238, .55); box-shadow: 0 0 0 3px rgba(37, 244, 238, .12); }
  .auth input[type=password] { width: 128px; }

  button {
    font: inherit; cursor: pointer; border: 0; border-radius: 9px;
    transition: transform .12s ease, filter .16s ease, background .16s ease;
  }
  button:active { transform: scale(.97); }
  .btn-primary {
    padding: 8px 18px; font-size: 13px; font-weight: 700; color: #fff;
    background: linear-gradient(92deg, var(--pink), #ff5c7a);
  }
  .btn-primary:hover { filter: brightness(1.08); }
  .btn-ghost {
    padding: 8px 14px; font-size: 13px; color: var(--muted);
    background: transparent; border: 1px solid var(--line);
  }
  .btn-ghost:hover { color: var(--text); border-color: rgba(255, 255, 255, .2); }
  .who {
    display: none; align-items: center; gap: 8px; font-size: 13px; color: var(--muted);
  }
  .who .dot { width: 7px; height: 7px; border-radius: 50%; background: #3ddc97; box-shadow: 0 0 8px #3ddc97; }
  .who b { color: var(--text); font-weight: 600; }
  .signed-in .who { display: flex; }
  .signed-in .login-only { display: none; }

  /* feed 主区 */
  main { max-width: 560px; margin: 0 auto; padding: 22px 16px 80px; }
  .feed-head {
    display: flex; align-items: baseline; gap: 10px; margin: 4px 2px 16px;
  }
  .feed-head h1 { font-size: 15px; margin: 0; font-weight: 700; letter-spacing: .4px; }
  .feed-head span { font-size: 12px; color: var(--muted); }

  .card {
    position: relative;
    margin: 0 0 22px;
    background: var(--card);
    border: 1px solid var(--line);
    border-radius: 18px;
    overflow: hidden;
    box-shadow: 0 18px 44px -28px rgba(0, 0, 0, .95);
    transition: transform .18s ease, border-color .18s ease;
  }
  .card:hover { transform: translateY(-2px); border-color: rgba(255, 255, 255, .14); }

  /* 视频舞台：16:9，点击播放/暂停，hover 出播放键 */
  .stage {
    position: relative; width: 100%; aspect-ratio: 16 / 9;
    background: #000; cursor: pointer; overflow: hidden;
  }
  .stage video {
    display: block; width: 100%; height: 100%; object-fit: cover;
    background: #000; pointer-events: none;
  }
  .stage::after {
    content: ""; position: absolute; inset: 0; pointer-events: none;
    background: linear-gradient(to top, rgba(0, 0, 0, .55), rgba(0, 0, 0, 0) 42%);
    opacity: .9; transition: opacity .2s;
  }
  .play-hint {
    position: absolute; inset: 0; z-index: 2;
    display: grid; place-items: center;
    opacity: 0; transition: opacity .2s ease;
    background: rgba(0, 0, 0, .18);
  }
  .stage:hover .play-hint { opacity: 1; }
  .stage.playing .play-hint { opacity: 0; }
  .stage.playing:hover .play-hint { opacity: 1; }
  .play-hint i {
    width: 60px; height: 60px; border-radius: 50%;
    display: grid; place-items: center;
    background: rgba(255, 255, 255, .92);
    color: #111; font-size: 22px; font-style: normal; padding-left: 4px;
    box-shadow: 0 8px 26px rgba(0, 0, 0, .5);
    transform: scale(.92); transition: transform .2s ease;
  }
  .stage:hover .play-hint i { transform: scale(1); }
  .stage.playing .play-hint i::before { content: "❚❚"; font-size: 17px; padding-left: 0; }
  .stage.playing .play-hint i { }
  .duration {
    position: absolute; right: 10px; bottom: 10px; z-index: 3;
    padding: 2px 7px; border-radius: 5px;
    font-size: 11px; color: #e9eaf0; background: rgba(0, 0, 0, .55);
    backdrop-filter: blur(4px);
  }

  /* 卡片正文 */
  .body { padding: 13px 15px 15px; }
  .title { font-size: 15px; font-weight: 650; margin: 0 0 3px; line-height: 1.45; }
  .author { font-size: 12.5px; color: var(--muted); margin: 0 0 11px; }
  .author b { color: #cfd2de; font-weight: 600; }

  .actions { display: flex; align-items: center; gap: 8px; margin-bottom: 11px; }
  .pill {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 6px 12px; font-size: 13px; color: #d7d9e3;
    background: #1e1e28; border: 1px solid var(--line); border-radius: 999px;
  }
  .pill:hover { background: #262633; }
  .pill.liked { color: var(--pink); border-color: rgba(254, 44, 85, .45); background: rgba(254, 44, 85, .1); }
  .pill .ico { font-size: 14px; line-height: 1; }
  .cmt-count { cursor: default; }
  .cmt-count:hover { background: #1e1e28; }

  .comment-row { display: flex; gap: 8px; }
  .comment-row input {
    flex: 1; min-width: 0; padding: 9px 12px;
    background: #1c1c25; color: var(--text);
    border: 1px solid var(--line); border-radius: 10px;
    font-size: 13px; outline: none; transition: border-color .16s;
  }
  .comment-row input:focus { border-color: rgba(254, 44, 85, .55); }
  .comment-row button {
    padding: 0 16px; font-size: 13px; font-weight: 700; color: #fff;
    background: linear-gradient(92deg, #2a2a36, #33333f);
    border: 1px solid var(--line);
  }
  .comment-row button:hover { filter: brightness(1.15); }
  .comment-row button:disabled { opacity: .45; cursor: default; }

  /* 空态 / 错误态 / 骨架 */
  .notice {
    padding: 40px 20px; text-align: center; color: var(--muted); font-size: 14px;
    background: var(--card); border: 1px dashed var(--line); border-radius: 16px;
  }
  .notice button { margin-top: 14px; }
  .skeleton { height: 260px; margin-bottom: 22px; border-radius: 18px;
    background: linear-gradient(100deg, #15151c 20%, #1e1e28 40%, #15151c 60%);
    background-size: 220% 100%; animation: shimmer 1.3s infinite linear; }
  @keyframes shimmer { to { background-position: -220% 0; } }

  /* toast */
  .toast {
    position: fixed; left: 50%; top: 74px; z-index: 60;
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

  @media (max-width: 560px) {
    .topbar { padding: 10px 14px; }
    .auth input { width: 140px; }
    .auth input[type=password] { width: 104px; }
    main { padding: 16px 10px 70px; }
  }
</style>
</head>
<body>
<header class="topbar" id="topbar">
  <div class="brand">Go2Cpp 抖音 Demo<small>FEED · PLAY · LIKE · COMMENT</small></div>
  <div class="auth">
    <span class="who" id="who"><span class="dot"></span><b id="whoName"></b></span>
    <input class="login-only" id="email" type="email" value="admin@demo.local"
           placeholder="邮箱" autocomplete="username" spellcheck="false">
    <input class="login-only" id="password" type="password" value="123456"
           placeholder="密码" autocomplete="current-password">
    <button class="btn-primary login-only" id="loginBtn" type="button">登录</button>
    <button class="btn-ghost" id="logoutBtn" type="button" style="display:none">退出</button>
  </div>
</header>

<div class="toast" id="toast" role="status" aria-live="polite"></div>

<main>
  <div class="feed-head">
    <h1>推荐 feed</h1><span id="feedMeta">加载中…</span>
  </div>
  <section id="feed"></section>
</main>

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

  var state = {
    token: localStorage.getItem(TOKEN_KEY) || '',
    user: null,
    likes: {}
  };
  try {
    state.user = JSON.parse(localStorage.getItem(USER_KEY) || 'null');
    state.likes = JSON.parse(localStorage.getItem(LIKES_KEY) || '{}') || {};
  } catch (e) {
    state.user = null;
    state.likes = {};
  }

  var feedEl = document.getElementById('feed');
  var feedMeta = document.getElementById('feedMeta');
  var toastEl = document.getElementById('toast');
  var toastTimer = 0;

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
    feedMeta.textContent = '';
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

  /* ---------- 登录 ---------- */

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

  function paintLike(card, liked, count) {
    var btn = card.querySelector('.like-btn');
    var label = btn.querySelector('.like-count');
    btn.classList.toggle('liked', liked);
    btn.querySelector('.ico').textContent = liked ? '♥' : '♡';
    if (count !== undefined && count !== null) label.textContent = fmt(count);
    var id = card.dataset.id;
    if (liked) state.likes[id] = 1; else delete state.likes[id];
    try { localStorage.setItem(LIKES_KEY, JSON.stringify(state.likes)); } catch (e) {}
  }

  function toggleLike(card) {
    if (!state.token) { toast('请先在右上角登录，再点赞', 'err'); return; }
    var id = card.dataset.id;
    var liked = !!state.likes[id];
    var btn = card.querySelector('.like-btn');
    if (btn.dataset.busy === '1') return;
    btn.dataset.busy = '1';
    api('api/like', {
      method: liked ? 'DELETE' : 'POST',
      body: { video_id: Number(id) }
    }).then(function (data) {
      paintLike(card, !liked, data.like_count);
      toast(liked ? '已取消点赞' : '点赞成功', 'ok');
    }).catch(function (err) {
      if (!handleAuthError(err)) toast('点赞失败：' + (err.message || '网络错误'), 'err');
    }).then(function () { btn.dataset.busy = ''; });
  }

  /* ---------- 评论 ---------- */

  function sendComment(card) {
    if (!state.token) { toast('请先在右上角登录，再评论', 'err'); return; }
    var input = card.querySelector('.comment-input');
    var btn = card.querySelector('.send-btn');
    var content = (input.value || '').trim();
    if (!content) { toast('评论内容不能为空', 'err'); return; }
    if (content.length > 500) { toast('评论最多 500 字', 'err'); return; }
    btn.disabled = true;
    api('api/comment', { method: 'POST', body: { video_id: Number(card.dataset.id), content: content } })
      .then(function () {
        input.value = '';
        var countEl = card.querySelector('.cmt-count .num');
        var n = Number(countEl.dataset.n || 0) + 1;
        countEl.dataset.n = n;
        countEl.textContent = fmt(n);
        toast('评论已发送', 'ok');
      })
      .catch(function (err) {
        if (!handleAuthError(err)) toast('评论失败：' + (err.message || '网络错误'), 'err');
      })
      .then(function () { btn.disabled = false; });
  }

  /* ---------- 视频播放 ---------- */

  var playingVideo = null;

  function pauseAll(except) {
    if (playingVideo && playingVideo !== except) {
      playingVideo.pause();
    }
  }

  function bindPlayer(card, video) {
    var stage = card.querySelector('.stage');

    video.addEventListener('play', function () {
      pauseAll(video);
      playingVideo = video;
      stage.classList.add('playing');
    });
    video.addEventListener('pause', function () {
      stage.classList.remove('playing');
      if (playingVideo === video) playingVideo = null;
    });
    video.addEventListener('ended', function () { stage.classList.remove('playing'); });
    video.addEventListener('error', function () {
      stage.classList.remove('playing');
      toast('视频加载失败，检查 /videos 静态路由', 'err');
    });

    stage.addEventListener('click', function () {
      if (video.paused) {
        var p = video.play();
        if (p && p.catch) {
          p.catch(function () { toast('浏览器阻止了自动播放，请再点一次', 'err'); });
        }
      } else {
        video.pause();
      }
    });
  }

  /* ---------- feed 渲染 ---------- */

  function videoSrc(id) {
    return 'videos/' + ((Number(id) % 3) + 1) + '.mp4';
  }

  function buildCard(item) {
    var v = item.video || {};
    var id = String(v.id || 0);
    var card = document.createElement('article');
    card.className = 'card';
    card.dataset.id = id;

    var stage = document.createElement('div');
    stage.className = 'stage';
    stage.setAttribute('role', 'button');
    stage.setAttribute('aria-label', '播放/暂停视频');

    var video = document.createElement('video');
    video.src = videoSrc(id);
    video.muted = true;
    video.loop = true;
    video.playsInline = true;
    video.preload = 'metadata';
    video.setAttribute('webkit-playsinline', '');

    var hint = document.createElement('div');
    hint.className = 'play-hint';
    hint.innerHTML = '<i>▶</i>';

    var duration = document.createElement('span');
    duration.className = 'duration';
    duration.textContent = '00:20';

    stage.appendChild(video);
    stage.appendChild(hint);
    stage.appendChild(duration);

    var body = document.createElement('div');
    body.className = 'body';

    var title = document.createElement('h2');
    title.className = 'title';
    title.textContent = v.title || ('视频 #' + id);

    var author = document.createElement('p');
    author.className = 'author';
    var b = document.createElement('b');
    b.textContent = '@' + (item.author || ('user' + (v.user_id || '')));
    author.appendChild(b);
    author.appendChild(document.createTextNode(' · 视频 ID ' + id));

    var actions = document.createElement('div');
    actions.className = 'actions';

    var likeBtn = document.createElement('button');
    likeBtn.type = 'button';
    likeBtn.className = 'pill like-btn';
    likeBtn.innerHTML = '<span class="ico">♡</span><span class="like-count">0</span>';
    likeBtn.addEventListener('click', function () { toggleLike(card); });

    var likeCount = likeBtn.querySelector('.like-count');
    likeCount.textContent = fmt(v.like_count);

    var cmt = document.createElement('span');
    cmt.className = 'pill cmt-count';
    cmt.innerHTML = '<span class="ico">💬</span>';
    var cmtNum = document.createElement('span');
    cmtNum.className = 'num';
    cmtNum.dataset.n = String(v.comment_count || 0);
    cmtNum.textContent = fmt(v.comment_count);
    cmt.appendChild(cmtNum);

    actions.appendChild(likeBtn);
    actions.appendChild(cmt);

    var row = document.createElement('div');
    row.className = 'comment-row';
    var input = document.createElement('input');
    input.className = 'comment-input';
    input.type = 'text';
    input.placeholder = state.token ? '说点什么…' : '登录后参与评论';
    input.maxLength = 500;
    var sendBtn = document.createElement('button');
    sendBtn.type = 'button';
    sendBtn.className = 'send-btn';
    sendBtn.textContent = '发送';
    sendBtn.addEventListener('click', function () { sendComment(card); });
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); sendComment(card); }
    });
    row.appendChild(input);
    row.appendChild(sendBtn);

    body.appendChild(title);
    body.appendChild(author);
    body.appendChild(actions);
    body.appendChild(row);

    card.appendChild(stage);
    card.appendChild(body);

    var liked = !!state.likes[id];
    paintLike(card, liked, liked ? null : v.like_count);
    if (liked) likeCount.textContent = fmt(v.like_count);

    bindPlayer(card, video);
    return card;
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
    feedMeta.textContent = '加载中…';
    api('api/feed?limit=20')
      .then(function (data) {
        var items = data.items || [];
        feedEl.innerHTML = '';
        if (!items.length) {
          showNotice('还没有视频，先跑 scripts/seed_bulk.sh 灌数据', true);
          feedMeta.textContent = '0 条';
          return;
        }
        var frag = document.createDocumentFragment();
        items.forEach(function (item) { frag.appendChild(buildCard(item)); });
        feedEl.appendChild(frag);
        feedMeta.textContent = '共 ' + items.length + ' 条 · 点封面播放';
      })
      .catch(function (err) {
        showNotice('feed 加载失败：' + (err.message || '网络错误'), true);
        feedMeta.textContent = '加载失败';
      });
  }

  /* ---------- 事件绑定与启动 ---------- */

  document.getElementById('loginBtn').addEventListener('click', login);
  document.getElementById('logoutBtn').addEventListener('click', logout);
  document.getElementById('password').addEventListener('keydown', function (e) {
    if (e.key === 'Enter') login();
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
