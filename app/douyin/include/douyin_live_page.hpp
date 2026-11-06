/**
 * @file douyin_live_page.hpp
 * @brief 抖音直播间单页（第 10 轮）：聊天 / 礼物 / 贡献榜。
 *
 * @details 页面由网关本地直出（GET /live），数据全部走 `/api/live/*`：
 *          - 聊天：首屏 `?after=0` 全量，之后每 2s 用 `after=<last_id>` 增量；
 *            自己发的消息先乐观上屏，服务端回包后按 id 去重；
 *          - 礼物：礼物面板（价目来自 `/api/live/gifts`）+ 余额（`/wallet`），
 *            送出后聊天区高亮「送出 火箭 ×1」并播放飘屏特效；
 *          - 贡献榜：`/ranking` 与聊天同一个轮询周期刷新，前三名奖牌。
 *
 * @note 页面无框架、无构建依赖；所有用户内容用 textContent 写入，防 XSS。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#pragma once

namespace douyin {

inline constexpr const char* kLivePageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>直播间 · 抖音 Demo</title>
<style>
  :root {
    --bg: #0b0d12;
    --panel: rgba(22, 25, 34, .96);
    --line: rgba(255, 255, 255, .1);
    --text: #f4f6fb;
    --muted: #9aa3b5;
    --pink: #fe2c55;
    --cyan: #25f4ee;
    --gold: #ffd166;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { height: 100%; margin: 0; }
  body {
    background: var(--bg); color: var(--text);
    font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
          "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
    overflow: hidden;
  }
  .live-stage {
    position: relative; display: flex; flex-direction: column;
    height: 100vh; height: 100dvh; max-width: 720px; margin: 0 auto;
    background: radial-gradient(120% 80% at 50% 0%, #1b2233 0%, #0b0d12 62%);
    overflow: hidden;
  }
  .live-top {
    display: flex; align-items: center; gap: 8px;
    padding: calc(10px + env(safe-area-inset-top)) 12px 10px;
    background: linear-gradient(180deg, rgba(0,0,0,.55), rgba(0,0,0,0));
    position: relative; z-index: 5;
  }
  .back {
    color: var(--text); text-decoration: none; font-size: 15px;
    padding: 4px 8px; border-radius: 999px; background: rgba(255,255,255,.08);
  }
  .host { display: flex; align-items: center; gap: 8px; min-width: 0; flex: 1; }
  .host-avatar {
    width: 34px; height: 34px; border-radius: 50%; flex: 0 0 34px;
    display: grid; place-items: center; font-weight: 700;
    background: linear-gradient(135deg, var(--pink), #7b3cff);
    border: 2px solid rgba(255,255,255,.6);
  }
  .host-meta { display: flex; flex-direction: column; min-width: 0; line-height: 1.25; }
  .host-meta strong { font-size: 14px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .host-meta span { color: var(--muted); font-size: 11px; }
  .live-badge {
    font-size: 10px; font-weight: 800; letter-spacing: .5px; padding: 2px 6px;
    color: #fff; background: var(--pink); border-radius: 4px;
  }
  .viewer { color: var(--muted); font-size: 11px; white-space: nowrap; }
  .rank-btn {
    border: 1px solid rgba(255,209,102,.5); color: var(--gold);
    background: rgba(255,209,102,.1); border-radius: 999px;
    padding: 5px 10px; font-size: 12px; cursor: pointer;
  }
  .stage-art {
    position: absolute; inset: 0; z-index: 0;
    background:
      radial-gradient(50% 38% at 50% 30%, rgba(254,44,85,.32), transparent 70%),
      radial-gradient(40% 30% at 68% 46%, rgba(37,244,238,.18), transparent 70%),
      linear-gradient(160deg, #171b26 0%, #0b0d12 70%);
  }
  .pulse {
    position: absolute; left: 50%; top: 26%; width: 190px; height: 190px;
    margin-left: -95px; border-radius: 50%;
    background: radial-gradient(circle, rgba(254,44,85,.38), rgba(254,44,85,0) 68%);
    animation: pulse 2.8s ease-in-out infinite;
  }
  @keyframes pulse { 0%,100% { transform: scale(.92); opacity: .8; } 50% { transform: scale(1.06); opacity: 1; } }
  .stage-hint {
    position: absolute; left: 0; right: 0; top: 40%; text-align: center;
    color: rgba(255,255,255,.42); letter-spacing: 8px; font-size: 13px;
  }
  .room-picker {
    position: absolute; left: 12px; right: 12px; top: 76px; z-index: 6;
    background: var(--panel); border: 1px solid var(--line); border-radius: 14px;
    padding: 10px; max-height: 52vh; overflow: auto; display: none;
  }
  .room-picker.open { display: block; }
  .room-picker h3 { margin: 2px 6px 8px; font-size: 13px; color: var(--muted); font-weight: 500; }
  .room-item {
    display: flex; align-items: center; gap: 10px; padding: 10px;
    border-radius: 10px; cursor: pointer;
  }
  .room-item:hover { background: rgba(255,255,255,.06); }
  .room-item .dot { width: 8px; height: 8px; border-radius: 50%; background: #5b6274; }
  .room-item.live .dot { background: var(--pink); box-shadow: 0 0 8px var(--pink); }
  .room-item b { font-weight: 600; font-size: 13px; flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .room-item small { color: var(--muted); }
  .chat {
    position: relative; z-index: 2; flex: 1; min-height: 0;
    overflow-y: auto; padding: 8px 12px 4px;
    display: flex; flex-direction: column; justify-content: flex-end; gap: 4px;
    background: linear-gradient(180deg, transparent, rgba(0,0,0,.55) 45%);
    scrollbar-width: thin;
  }
  .chat-empty {
    position: relative; z-index: 2; text-align: center; color: var(--muted);
    font-size: 12px; padding: 6px 0 10px;
  }
  .msg {
    align-self: flex-start; max-width: 88%;
    background: rgba(18,20,27,.72); border: 1px solid rgba(255,255,255,.07);
    border-radius: 12px; padding: 4px 9px; font-size: 13px;
    word-break: break-word; animation: msgIn .18s ease-out;
  }
  @keyframes msgIn { from { opacity: 0; transform: translateY(5px); } to { opacity: 1; transform: none; } }
  .msg-user { color: var(--cyan); margin-right: 4px; }
  .msg-self { background: rgba(254,44,85,.16); border-color: rgba(254,44,85,.4); }
  .msg-self .msg-user { color: #ff8fa8; }
  .msg-gift {
    align-self: stretch; max-width: 100%; text-align: center;
    background: linear-gradient(90deg, rgba(255,209,102,.16), rgba(254,44,85,.16));
    border: 1px solid rgba(255,209,102,.55); color: #ffe3a3;
    font-weight: 600; padding: 6px 10px;
    box-shadow: 0 0 18px rgba(255,209,102,.18);
    animation: giftIn .4s cubic-bezier(.2,.8,.3,1.2);
  }
  @keyframes giftIn {
    0% { transform: scale(.86); opacity: 0; }
    60% { transform: scale(1.03); }
    100% { transform: scale(1); opacity: 1; }
  }
  .live-bottom {
    position: relative; z-index: 4; display: flex; gap: 8px; align-items: center;
    padding: 8px 12px calc(10px + env(safe-area-inset-bottom));
    background: rgba(8,9,13,.86); border-top: 1px solid var(--line);
    backdrop-filter: blur(10px);
  }
  .live-bottom input {
    flex: 1; min-width: 0; height: 38px; border-radius: 999px;
    border: 1px solid var(--line); background: rgba(255,255,255,.07);
    color: var(--text); padding: 0 14px; font-size: 14px; outline: none;
  }
  .live-bottom input:focus { border-color: rgba(37,244,238,.6); }
  .live-bottom button {
    height: 38px; border: 0; border-radius: 999px; cursor: pointer;
    font-size: 13px; font-weight: 600; color: #fff; background: var(--pink);
    padding: 0 14px;
  }
  .live-bottom #giftBtn {
    background: rgba(255,255,255,.1); border: 1px solid var(--line);
    font-size: 17px; padding: 0 12px;
  }
  .panel {
    position: absolute; left: 0; right: 0; bottom: 0; z-index: 20;
    background: var(--panel); border-top: 1px solid var(--line);
    border-radius: 18px 18px 0 0; padding: 12px 14px calc(14px + env(safe-area-inset-bottom));
    transform: translateY(104%); transition: transform .22s ease;
    max-height: 68vh; display: flex; flex-direction: column;
  }
  .panel.open { transform: none; }
  .panel-head { display: flex; align-items: center; gap: 10px; padding-bottom: 8px; }
  .panel-head strong { font-size: 15px; flex: 1; }
  .panel-head .coins { color: var(--muted); font-size: 12px; }
  .panel-head .coins b { color: var(--gold); }
  .panel-head button {
    border: 0; background: rgba(255,255,255,.08); color: var(--text);
    width: 28px; height: 28px; border-radius: 50%; cursor: pointer; font-size: 16px;
  }
  .gift-grid {
    display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px;
    overflow-y: auto; padding: 4px 2px 10px;
  }
  .gift-item {
    border: 1px solid var(--line); border-radius: 12px; padding: 10px 4px;
    background: rgba(255,255,255,.04); text-align: center; cursor: pointer;
    transition: .15s;
  }
  .gift-item .icon { font-size: 26px; display: block; line-height: 1.2; }
  .gift-item .name { font-size: 12px; margin-top: 2px; }
  .gift-item .price { font-size: 11px; color: var(--gold); }
  .gift-item.selected { border-color: var(--gold); background: rgba(255,209,102,.13); }
  .gift-foot { display: flex; align-items: center; gap: 10px; }
  .stepper { display: flex; align-items: center; gap: 4px; }
  .stepper button {
    width: 30px; height: 30px; border-radius: 8px; border: 1px solid var(--line);
    background: rgba(255,255,255,.07); color: var(--text); cursor: pointer; font-size: 16px;
  }
  .stepper span { min-width: 26px; text-align: center; font-weight: 700; }
  .send-gift {
    margin-left: auto; height: 36px; padding: 0 22px; border: 0;
    border-radius: 999px; background: linear-gradient(90deg, #ff9a3d, var(--pink));
    color: #fff; font-weight: 700; cursor: pointer;
  }
  .rank-list {
    list-style: none; margin: 0; padding: 2px 0 4px; overflow-y: auto;
  }
  .rank-row {
    display: flex; align-items: center; gap: 10px; padding: 9px 4px;
    border-bottom: 1px solid rgba(255,255,255,.06);
  }
  .rank-row:last-child { border-bottom: 0; }
  .rank-no { width: 26px; text-align: center; color: var(--muted); font-weight: 700; }
  .rank-no.top { color: var(--gold); font-size: 17px; }
  .rank-avatar {
    width: 32px; height: 32px; border-radius: 50%; flex: 0 0 32px;
    display: grid; place-items: center; font-size: 13px; font-weight: 700;
    background: linear-gradient(135deg, #2c3550, #4a3a6b);
  }
  .rank-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .rank-coins { color: var(--gold); font-weight: 700; font-size: 13px; }
  .rank-empty { color: var(--muted); text-align: center; padding: 18px 0; font-size: 13px; }
  .fx-layer { position: absolute; inset: 0; z-index: 30; pointer-events: none; overflow: hidden; }
  .gift-fx {
    position: absolute; left: 50%; bottom: 12%;
    transform: translateX(-50%); white-space: nowrap;
    font-size: 15px; font-weight: 700; color: #ffe3a3;
    text-shadow: 0 2px 12px rgba(255,160,0,.65);
    animation: flyUp 1.8s ease-out forwards;
  }
  .gift-fx .big { font-size: 34px; margin-right: 8px; vertical-align: middle; }
  @keyframes flyUp {
    0% { opacity: 0; transform: translate(-50%, 20px) scale(.8); }
    18% { opacity: 1; transform: translate(-50%, 0) scale(1.04); }
    70% { opacity: 1; }
    100% { opacity: 0; transform: translate(-50%, -110px) scale(.96); }
  }
  .toast {
    position: fixed; left: 50%; bottom: 96px; transform: translate(-50%, 8px);
    background: rgba(30,33,44,.96); border: 1px solid var(--line); color: var(--text);
    padding: 8px 14px; border-radius: 999px; font-size: 13px; opacity: 0;
    pointer-events: none; transition: .2s; z-index: 50; max-width: 82vw;
  }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  @media (min-width: 721px) {
    body { display: grid; place-items: center; }
    .live-stage { border-left: 1px solid var(--line); border-right: 1px solid var(--line); }
  }
</style>
</head>
<body>
<div class="live-stage" id="stage">
  <div class="stage-art"><div class="pulse"></div><div class="stage-hint">现场画面</div></div>
  <header class="live-top">
    <a class="back" href="/">‹ 首页</a>
    <div class="host">
      <div class="host-avatar" id="hostAvatar">播</div>
      <div class="host-meta">
        <strong id="liveTitle">直播间</strong>
        <span id="hostName">主播</span>
      </div>
    </div>
    <span class="live-badge">LIVE</span>
    <span class="viewer" id="viewerCount">0 人在线</span>
    <button class="rank-btn" id="rankBtn" type="button">🏆 榜</button>
  </header>

  <div class="room-picker" id="roomPicker">
    <h3>选择一个直播间</h3>
    <div id="roomList"></div>
  </div>

  <div class="chat" id="chatList" aria-live="polite"></div>
  <div class="chat-empty" id="chatEmpty">还没有人说话，来抢沙发～</div>

  <footer class="live-bottom">
    <input id="chatInput" maxlength="200" placeholder="说点什么…" autocomplete="off">
    <button id="sendBtn" type="button">发送</button>
    <button id="giftBtn" type="button" aria-label="送礼物">🎁</button>
  </footer>

  <div class="fx-layer" id="fxLayer"></div>

  <section class="panel" id="giftPanel" aria-hidden="true">
    <div class="panel-head">
      <strong>送礼物</strong>
      <span class="coins">余额 <b id="coins">--</b> 金币</span>
      <button type="button" data-close="giftPanel" aria-label="关闭">×</button>
    </div>
    <div class="gift-grid" id="giftGrid"></div>
    <div class="gift-foot">
      <div class="stepper">
        <button id="countMinus" type="button" aria-label="减少">−</button>
        <span id="giftCount">1</span>
        <button id="countPlus" type="button" aria-label="增加">+</button>
      </div>
      <button class="send-gift" id="sendGift" type="button">送出</button>
    </div>
  </section>

  <section class="panel" id="rankingPanel" aria-hidden="true">
    <div class="panel-head">
      <strong>贡献榜</strong>
      <button type="button" data-close="rankingPanel" aria-label="关闭">×</button>
    </div>
    <ol class="rank-list" id="rankList"></ol>
  </section>

  <div class="toast" id="toast" role="status" aria-live="polite"></div>
</div>

<script>
(function () {
  'use strict';

  var TOKEN_KEY = 'douyin_token';
  var USER_KEY = 'douyin_user';
  var POLL_MS = 2000;            // 聊天 + 榜单的轮询周期
  var MAX_CHAT_NODES = 200;      // 聊天区 DOM 上限，防止长时间开播撑爆内存
  var MAX_GIFT_COUNT = 99;

  var $ = function (id) { return document.getElementById(id); };

  var state = {
    roomId: 0,
    lastId: 0,
    rendered: Object.create(null),
    gifts: [],
    selectedGift: 0,
    giftCount: 1,
    coins: 0,
    coinsLoaded: false,
    token: '',
    user: null,
    pollTicks: 0,
    pollTimer: 0,
    sending: false
  };

  try {
    state.token = localStorage.getItem(TOKEN_KEY) || '';
    state.user = JSON.parse(localStorage.getItem(USER_KEY) || 'null');
  } catch (e) { state.token = ''; }

  /* ---------- 基础工具 ---------- */

  function toast(text) {
    var el = $('toast');
    el.textContent = text;
    el.classList.add('show');
    clearTimeout(toast.timer);
    toast.timer = setTimeout(function () { el.classList.remove('show'); }, 2200);
  }

  function getJSON(url) {
    var headers = {};
    if (state.token) { headers['Authorization'] = 'Bearer ' + state.token; }
    return fetch(url, { headers: headers, credentials: 'same-origin' })
      .then(function (res) { return res.json().catch(function () { return {}; }); });
  }

  function postJSON(url, payload, token) {
    var headers = { 'Content-Type': 'application/json' };
    if (token) { headers['Authorization'] = 'Bearer ' + token; }
    return fetch(url, {
      method: 'POST',
      headers: headers,
      body: JSON.stringify(payload || {})
    }).then(function (res) { return res.json().catch(function () { return {}; }); });
  }

  function fmtCoins(n) {
    n = Number(n) || 0;
    if (n >= 10000) { return (n / 10000).toFixed(n % 10000 === 0 ? 0 : 1) + ' 万'; }
    return String(n);
  }

  /* ---------- 房间信息 ---------- */

  function roomFromQuery() {
    var match = /[?&]room=(\d+)/.exec(location.search || '');
    return match ? Number(match[1]) : 0;
  }

  function renderRoom(room) {
    if (!room) { return; }
    $('liveTitle').textContent = room.title || ('直播间 #' + room.id);
    $('hostName').textContent = '房主 UID ' + room.user_id;
    $('viewerCount').textContent = (room.viewer_count || 0) + ' 人在线';
    $('hostAvatar').textContent = '播';
    document.title = (room.title || '直播间') + ' · 抖音 Demo';
  }

  function renderPicker(rooms) {
    var picker = $('roomPicker');
    var list = $('roomList');
    list.textContent = '';
    if (!rooms.length) {
      var empty = document.createElement('div');
      empty.className = 'rank-empty';
      empty.textContent = '还没有直播间，先去创建一个吧';
      list.appendChild(empty);
    }
    rooms.forEach(function (room) {
      var item = document.createElement('div');
      item.className = 'room-item' + (room.status === 1 ? ' live' : '');
      var dot = document.createElement('span'); dot.className = 'dot';
      var name = document.createElement('b'); name.textContent = room.title || ('直播间 #' + room.id);
      var meta = document.createElement('small');
      meta.textContent = room.status === 1 ? (room.viewer_count || 0) + ' 人在线' : '未开播';
      item.appendChild(dot); item.appendChild(name); item.appendChild(meta);
      item.addEventListener('click', function () {
        picker.classList.remove('open');
        enterRoom(room.id);
      });
      list.appendChild(item);
    });
    picker.classList.toggle('open', !state.roomId || !rooms.some(function (r) {
      return r.id === state.roomId;
    }));
  }

  function loadRooms() {
    return getJSON('/api/live/rooms').then(function (data) {
      var rooms = ((data && data.items) || []).map(function (item) { return item.room; })
        .filter(Boolean);
      if (!state.roomId) {
        renderPicker(rooms);
        return;
      }
      var room = rooms.filter(function (r) { return r.id === state.roomId; })[0];
      if (room) {
        renderRoom(room);
        $('roomPicker').classList.remove('open');
      } else {
        toast('直播间不存在或已关闭');
        renderPicker(rooms);
      }
    }).catch(function () {});
  }

  /* ---------- 聊天 ---------- */

  function nearBottom() {
    var chat = $('chatList');
    return chat.scrollHeight - chat.scrollTop - chat.clientHeight < 90;
  }

  function scrollBottom(force) {
    var chat = $('chatList');
    if (force || nearBottom()) { chat.scrollTop = chat.scrollHeight; }
  }

  function trimChat() {
    var chat = $('chatList');
    while (chat.children.length > MAX_CHAT_NODES) { chat.removeChild(chat.firstChild); }
  }

  function chatNode(message) {
    var node = document.createElement('div');
    node.className = 'msg';
    if (message.kind === 'gift') {
      node.className = 'msg msg-gift';
      node.textContent = '🎁 ' + (message.user_name || ('用户' + message.user_id)) +
        ' ' + message.content;
      return node;
    }
    if (state.user && Number(message.user_id) === Number(state.user.id)) {
      node.className = 'msg msg-self';
    }
    var who = document.createElement('span');
    who.className = 'msg-user';
    who.textContent = (message.user_name || ('用户' + message.user_id)) + '：';
    var text = document.createElement('span');
    text.textContent = message.content;
    node.appendChild(who);
    node.appendChild(text);
    return node;
  }

  function spawnGiftFx(message) {
    var layer = $('fxLayer');
    var fx = document.createElement('div');
    fx.className = 'gift-fx';
    var big = document.createElement('span');
    big.className = 'big';
    big.textContent = '🎁';
    fx.appendChild(big);
    fx.appendChild(document.createTextNode(
      (message.user_name || '有人') + ' ' + message.content));
    layer.appendChild(fx);
    setTimeout(function () {
      if (fx.parentNode) { fx.parentNode.removeChild(fx); }
    }, 1900);
  }

  function removeTemps() {
    var chat = $('chatList');
    var temps = chat.querySelectorAll('[data-temp="1"]');
    for (var i = 0; i < temps.length; i++) {
      if (temps[i].parentNode) { temps[i].parentNode.removeChild(temps[i]); }
    }
  }

  function appendMessages(items) {
    if (!items || !items.length) { return; }
    var chat = $('chatList');
    var shouldScroll = nearBottom();
    var fresh = false;
    items.forEach(function (message) {
      if (!message || !message.id) { return; }
      if (state.rendered[message.id]) { return; }
      state.rendered[message.id] = true;
      if (Number(message.id) > state.lastId) { state.lastId = Number(message.id); }
      chat.appendChild(chatNode(message));
      fresh = true;
      if (message.kind === 'gift') { spawnGiftFx(message); }
    });
    if (!fresh) { return; }
    removeTemps();
    trimChat();
    $('chatEmpty').hidden = true;
    scrollBottom(shouldScroll);
  }

  function appendOptimistic(content) {
    var chat = $('chatList');
    var me = state.user || { id: 0, name: '我' };
    var node = chatNode({
      user_id: me.id,
      user_name: me.name || '我',
      content: content,
      kind: 'chat'
    });
    node.setAttribute('data-temp', '1');
    node.style.opacity = '.62';
    chat.appendChild(node);
    $('chatEmpty').hidden = true;
    scrollBottom(true);
  }

  function loadMessages(initial) {
    if (!state.roomId) { return Promise.resolve(); }
    var after = initial ? 0 : state.lastId;
    return getJSON('/api/live/' + state.roomId + '/messages?after=' + after + '&limit=50')
      .then(function (data) {
        if (!data || !data.ok) { return; }
        appendMessages(data.items || []);
      });
  }

  function sendMessage() {
    var input = $('chatInput');
    var text = (input.value || '').trim();
    if (!text) { return; }
    if (!state.token) { toast('请先在首页登录'); return; }
    if (!state.roomId) { toast('请先选择直播间'); return; }
    if (state.sending) { return; }
    state.sending = true;
    input.value = '';
    appendOptimistic(text);
    postJSON('/api/live/' + state.roomId + '/messages', { content: text }, state.token)
      .then(function (data) {
        if (data && data.ok && data.message) {
          appendMessages([data.message]);
        } else {
          removeTemps();
          toast((data && data.error) || '发送失败');
        }
      })
      .catch(function () { removeTemps(); toast('网络异常，发送失败'); })
      .then(function () { state.sending = false; });
  }

  /* ---------- 礼物 ---------- */

  function renderCoins() {
    $('coins').textContent = fmtCoins(state.coins);
  }

  function loadWallet() {
    if (!state.token) { $('coins').textContent = '未登录'; return Promise.resolve(); }
    return getJSON('/api/live/wallet').then(function (data) {
      if (data && data.ok) {
        state.coins = Number(data.coins) || 0;
        state.coinsLoaded = true;
        renderCoins();
      }
    }).catch(function () {});
  }

  function renderGifts() {
    var grid = $('giftGrid');
    grid.textContent = '';
    if (!state.gifts.length) {
      var empty = document.createElement('div');
      empty.className = 'rank-empty';
      empty.textContent = '礼物加载失败';
      grid.appendChild(empty);
      return;
    }
    state.gifts.forEach(function (gift) {
      var item = document.createElement('div');
      item.className = 'gift-item' + (gift.id === state.selectedGift ? ' selected' : '');
      var icon = document.createElement('span');
      icon.className = 'icon';
      icon.textContent = gift.icon;
      var name = document.createElement('div');
      name.className = 'name';
      name.textContent = gift.name;
      var price = document.createElement('div');
      price.className = 'price';
      price.textContent = gift.price_coins + ' 金币';
      item.appendChild(icon); item.appendChild(name); item.appendChild(price);
      item.addEventListener('click', function () {
        state.selectedGift = gift.id;
        renderGifts();
      });
      grid.appendChild(item);
    });
  }

  function loadGifts() {
    if (state.gifts.length) { return Promise.resolve(); }
    return getJSON('/api/live/gifts').then(function (data) {
      if (data && data.ok) {
        state.gifts = data.items || [];
        if (!state.selectedGift && state.gifts.length) {
          state.selectedGift = state.gifts[0].id;
        }
        renderGifts();
      }
    }).catch(function () {});
  }

  function openGiftPanel() {
    $('rankingPanel').classList.remove('open');
    $('giftPanel').classList.add('open');
    $('giftPanel').setAttribute('aria-hidden', 'false');
    loadGifts();
    loadWallet();
    renderCoins();
  }

  function closePanel(id) {
    var panel = $(id);
    if (!panel) { return; }
    panel.classList.remove('open');
    panel.setAttribute('aria-hidden', 'true');
  }

  function selectedGift() {
    for (var i = 0; i < state.gifts.length; i++) {
      if (state.gifts[i].id === state.selectedGift) { return state.gifts[i]; }
    }
    return null;
  }

  function sendGift() {
    if (!state.token) { toast('请先在首页登录'); return; }
    if (!state.roomId) { toast('请先选择直播间'); return; }
    var gift = selectedGift();
    if (!gift) { toast('请选择礼物'); return; }
    var total = gift.price_coins * state.giftCount;
    if (state.coinsLoaded && total > state.coins) { toast('余额不足，请减少数量'); return; }
    var button = $('sendGift');
    if (button.disabled) { return; }
    button.disabled = true;
    postJSON('/api/live/' + state.roomId + '/gift', {
      gift_id: gift.id,
      count: state.giftCount
    }, state.token).then(function (data) {
      if (data && data.ok) {
        state.coins = Number(data.coins) || state.coins;
        renderCoins();
        appendMessages([data.message]);
        toast('送出 ' + gift.icon + ' ' + gift.name + ' ×' + state.giftCount);
      } else {
        toast((data && data.error) || '送礼失败');
      }
    }).catch(function () { toast('网络异常，送礼失败'); })
      .then(function () { button.disabled = false; });
  }

  /* ---------- 贡献榜 ---------- */

  function renderRanking(items) {
    var list = $('rankList');
    list.textContent = '';
    if (!items || !items.length) {
      var empty = document.createElement('li');
      empty.className = 'rank-empty';
      empty.textContent = '还没有人送礼，快抢榜首';
      list.appendChild(empty);
      return;
    }
    items.forEach(function (rank, index) {
      var row = document.createElement('li');
      row.className = 'rank-row';

      var no = document.createElement('span');
      no.className = 'rank-no' + (index < 3 ? ' top' : '');
      no.textContent = index === 0 ? '🥇' : index === 1 ? '🥈' : index === 2 ? '🥉' : String(index + 1);

      var avatar = document.createElement('span');
      avatar.className = 'rank-avatar';
      avatar.textContent = (rank.user_name || '?').slice(0, 1);

      var name = document.createElement('span');
      name.className = 'rank-name';
      name.textContent = rank.user_name || ('用户' + rank.user_id);

      var coins = document.createElement('span');
      coins.className = 'rank-coins';
      coins.textContent = fmtCoins(rank.coins) + ' 金币';

      row.appendChild(no); row.appendChild(avatar);
      row.appendChild(name); row.appendChild(coins);
      list.appendChild(row);
    });
  }

  function loadRanking() {
    if (!state.roomId) { return Promise.resolve(); }
    return getJSON('/api/live/' + state.roomId + '/ranking?limit=10')
      .then(function (data) {
        if (data && data.ok) { renderRanking(data.items || []); }
      }).catch(function () {});
  }

  function openRanking() {
    $('giftPanel').classList.remove('open');
    $('rankingPanel').classList.add('open');
    $('rankingPanel').setAttribute('aria-hidden', 'false');
    loadRanking();
  }

  /* ---------- 轮询 ---------- */

  function schedulePoll() {
    clearTimeout(state.pollTimer);
    state.pollTimer = setTimeout(poll, POLL_MS);
  }

  function poll() {
    if (document.hidden) { schedulePoll(); return; }
    if (!state.roomId) { schedulePoll(); return; }
    var room = state.roomId;
    Promise.all([
      getJSON('/api/live/' + room + '/messages?after=' + state.lastId + '&limit=50'),
      getJSON('/api/live/' + room + '/ranking?limit=10')
    ]).then(function (results) {
      var messages = results[0];
      if (messages && messages.ok) { appendMessages(messages.items || []); }
      var ranking = results[1];
      if (ranking && ranking.ok) { renderRanking(ranking.items || []); }
      state.pollTicks += 1;
      if (state.pollTicks % 5 === 0) { loadRooms(); }
    }).catch(function () {}).then(schedulePoll);
  }

  /* ---------- 进入房间 ---------- */

  function enterRoom(id) {
    state.roomId = Number(id) || 0;
    state.lastId = 0;
    state.rendered = Object.create(null);
    state.pollTicks = 0;
    $('chatList').textContent = '';
    $('chatEmpty').hidden = false;
    $('roomPicker').classList.remove('open');
    if (history.replaceState) {
      history.replaceState(null, '', '/live?room=' + state.roomId);
    }
    loadRooms();
    loadMessages(true).then(function () { scrollBottom(true); });
    loadRanking();
    if (state.token) {
      postJSON('/api/live/room/' + state.roomId + '/enter', {}, state.token)
        .catch(function () {});
    }
  }

  /* ---------- 事件绑定 ---------- */

  $('sendBtn').addEventListener('click', sendMessage);
  $('chatInput').addEventListener('keydown', function (event) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      sendMessage();
    }
  });
  $('giftBtn').addEventListener('click', openGiftPanel);
  $('rankBtn').addEventListener('click', openRanking);
  $('sendGift').addEventListener('click', sendGift);
  $('countMinus').addEventListener('click', function () {
    state.giftCount = Math.max(1, state.giftCount - 1);
    $('giftCount').textContent = state.giftCount;
  });
  $('countPlus').addEventListener('click', function () {
    state.giftCount = Math.min(MAX_GIFT_COUNT, state.giftCount + 1);
    $('giftCount').textContent = state.giftCount;
  });
  Array.prototype.forEach.call(document.querySelectorAll('[data-close]'), function (button) {
    button.addEventListener('click', function () {
      closePanel(button.getAttribute('data-close'));
    });
  });
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) { poll(); }
  });

  /* ---------- 启动 ---------- */

  state.roomId = roomFromQuery();
  loadRooms();
  if (state.roomId) {
    loadMessages(true).then(function () { scrollBottom(true); });
    loadRanking();
  }
  loadWallet();
  loadGifts();
  schedulePoll();
})();
</script>
</body>
</html>)HTML";

}  // namespace douyin
