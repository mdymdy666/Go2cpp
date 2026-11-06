/**
 * @file douyin_dm_page.hpp
 * @brief 抖音私信单页（第 12 轮）：会话列表 + 聊天窗口 + 2s 增量轮询。
 *
 * @details 页面由网关本地直出（GET /messages），数据全部走 `/api/dm/*`：
 *          - 会话列表：`GET /api/dm/conversations?cursor=&limit=`，按最近
 *            消息倒序；每 4 个轮询周期刷新一次，避免每 2s 全量拉列表；
 *          - 聊天窗口：首屏 `?after=0` 全量，之后每 2s 用 `after=<last_id>`
 *            增量；自己发的消息先乐观上屏（临时负 id + pending 样式），
 *            服务端回包后按 id 去重替换；
 *          - 已读：进入会话 / 收到新消息后 `POST /api/dm/:id/read`，
 *            未读角标回会话列表同步清零。
 *
 * @note 页面无框架、无构建依赖；所有用户内容用 textContent 写入，防 XSS。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#pragma once

namespace douyin {

inline constexpr const char* kDmPageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>私信 · 抖音 Demo</title>
<style>
  :root {
    --bg: #0b0d12;
    --panel: #161922;
    --panel-2: #1d212c;
    --line: rgba(255, 255, 255, .09);
    --text: #f4f6fb;
    --muted: #9aa3b5;
    --pink: #fe2c55;
    --cyan: #25f4ee;
    --mine: linear-gradient(135deg, #fe2c55, #ff5f7e);
    --theirs: #262b38;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { height: 100%; margin: 0; }
  body {
    background: var(--bg); color: var(--text);
    font: 14px/1.55 -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
          "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
    overflow: hidden;
  }
  .app { display: flex; height: 100vh; height: 100dvh; max-width: 1080px; margin: 0 auto; }
  .side {
    width: 300px; min-width: 300px; border-right: 1px solid var(--line);
    display: flex; flex-direction: column; background: var(--panel);
  }
  .side-head {
    display: flex; align-items: center; gap: 10px; padding: 14px 14px 10px;
    border-bottom: 1px solid var(--line);
  }
  .side-head h1 { font-size: 17px; margin: 0; flex: 1; letter-spacing: .5px; }
  .icon-btn {
    width: 32px; height: 32px; border-radius: 10px; border: 1px solid var(--line);
    background: transparent; color: var(--text); cursor: pointer; font-size: 15px;
    display: inline-flex; align-items: center; justify-content: center; text-decoration: none;
  }
  .icon-btn:hover { background: var(--panel-2); }
  .search { padding: 10px 12px; border-bottom: 1px solid var(--line); }
  .search input {
    width: 100%; padding: 8px 12px; border-radius: 10px; border: 1px solid var(--line);
    background: var(--panel-2); color: var(--text); outline: none;
  }
  .conv-list { flex: 1; overflow-y: auto; }
  .conv {
    display: flex; gap: 10px; padding: 11px 14px; cursor: pointer;
    border-bottom: 1px solid rgba(255,255,255,.04);
  }
  .conv:hover { background: var(--panel-2); }
  .conv.active { background: rgba(254, 44, 85, .14); }
  .avatar {
    width: 42px; height: 42px; min-width: 42px; border-radius: 50%;
    background: linear-gradient(135deg, #2b3346, #3d4660); color: var(--cyan);
    display: flex; align-items: center; justify-content: center; font-weight: 700;
    font-size: 17px; overflow: hidden;
  }
  .conv-main { flex: 1; min-width: 0; }
  .conv-top { display: flex; align-items: baseline; gap: 8px; }
  .conv-top b {
    font-size: 13.5px; font-weight: 600; white-space: nowrap; overflow: hidden;
    text-overflow: ellipsis; flex: 1;
  }
  .conv-top time { font-size: 10.5px; color: var(--muted); }
  .conv-preview {
    font-size: 12px; color: var(--muted); white-space: nowrap; overflow: hidden;
    text-overflow: ellipsis; margin-top: 2px;
  }
  .badge {
    min-width: 18px; height: 18px; padding: 0 5px; border-radius: 9px;
    background: var(--pink); color: #fff; font-size: 10.5px; font-weight: 700;
    display: inline-flex; align-items: center; justify-content: center; margin-top: 4px;
  }
  .empty {
    padding: 44px 18px; text-align: center; color: var(--muted); font-size: 12.5px;
  }
  .chat { flex: 1; display: flex; flex-direction: column; min-width: 0; background: var(--bg); }
  .chat-head {
    display: flex; align-items: center; gap: 10px; padding: 12px 16px;
    border-bottom: 1px solid var(--line);
    background: rgba(22, 25, 34, .92);
  }
  .chat-head .name { font-size: 15px; font-weight: 600; flex: 1; }
  .chat-head .sub { font-size: 11px; color: var(--muted); }
  .back { display: none; }
  .chat-body { flex: 1; overflow-y: auto; padding: 18px 16px 8px; }
  .msg { display: flex; gap: 8px; margin-bottom: 14px; align-items: flex-end; }
  .msg .bubble {
    max-width: min(70%, 520px); padding: 9px 13px; border-radius: 14px;
    background: var(--theirs); color: var(--text); white-space: pre-wrap;
    word-break: break-word; font-size: 13.5px;
  }
  .msg .meta { font-size: 10px; color: var(--muted); margin-top: 3px; }
  .msg.mine { flex-direction: row-reverse; }
  .msg.mine .bubble { background: var(--mine); color: #fff; border-bottom-right-radius: 4px; }
  .msg.mine .meta { text-align: right; }
  .msg .bubble.pending { opacity: .65; }
  .msg .bubble.failed { outline: 1px dashed #ff8484; }
  .day-sep { text-align: center; color: var(--muted); font-size: 10.5px; margin: 14px 0; }
  .composer {
    display: flex; gap: 10px; padding: 10px 14px calc(12px + env(safe-area-inset-bottom));
    border-top: 1px solid var(--line); background: var(--panel);
  }
  .composer textarea {
    flex: 1; resize: none; height: 42px; max-height: 120px; padding: 10px 12px;
    border-radius: 12px; border: 1px solid var(--line); background: var(--panel-2);
    color: var(--text); font: inherit; outline: none;
  }
  .composer textarea:focus { border-color: rgba(254, 44, 85, .55); }
  .send-btn {
    padding: 0 20px; border: 0; border-radius: 12px; background: var(--pink);
    color: #fff; font-weight: 600; cursor: pointer; font-size: 13.5px;
  }
  .send-btn:disabled { opacity: .45; cursor: default; }
  .login-tip { padding: 10px 16px; font-size: 12px; color: var(--muted); text-align: center; }
  .login-tip a { color: var(--cyan); }
  .toast {
    position: fixed; left: 50%; bottom: 88px; transform: translate(-50%, 12px);
    background: rgba(20, 22, 30, .96); border: 1px solid var(--line); color: var(--text);
    padding: 9px 16px; border-radius: 10px; font-size: 12.5px; opacity: 0;
    pointer-events: none; transition: .18s ease; z-index: 40;
  }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  @media (max-width: 720px) {
    .side { position: absolute; inset: 0; width: 100%; min-width: 0; z-index: 20; }
    .chat { position: absolute; inset: 0; z-index: 10; display: none; }
    body.chat-open .chat { display: flex; }
    body.chat-open .side { display: none; }
    .back { display: inline-flex; }
    .msg .bubble { max-width: 78%; }
  }
</style>
</head>
<body>
<div class="app">
  <aside class="side" id="side">
    <div class="side-head">
      <a class="icon-btn" href="/" title="返回首页">←</a>
      <h1>私信</h1>
      <button class="icon-btn" id="refreshBtn" title="刷新">⟳</button>
    </div>
    <div class="search"><input id="convFilter" placeholder="搜索会话" autocomplete="off"></div>
    <div class="conv-list" id="convList"></div>
  </aside>

  <main class="chat" id="chat">
    <div class="chat-head">
      <button class="icon-btn back" id="backBtn" title="返回">←</button>
      <div class="avatar" id="peerAvatar">·</div>
      <div style="flex:1;min-width:0">
        <div class="name" id="peerName">选择一个会话</div>
        <div class="sub" id="peerSub"></div>
      </div>
    </div>
    <div class="chat-body" id="chatBody">
      <div class="empty" id="chatEmpty">从左侧选择会话开始聊天</div>
    </div>
    <div class="composer" id="composer" style="display:none">
      <textarea id="input" placeholder="发消息…（Enter 发送，Shift+Enter 换行）" maxlength="500"></textarea>
      <button class="send-btn" id="sendBtn" disabled>发送</button>
    </div>
    <div class="login-tip" id="loginTip" style="display:none">
      未登录，<a href="/">回首页登录</a> 后才能查看私信
    </div>
  </main>
</div>
<div class="toast" id="toast"></div>

<script>
(function () {
  'use strict';

  var TOKEN_KEY = 'douyin_token';
  var USER_KEY = 'douyin_user';
  var POLL_MS = 2000;
  var LIST_EVERY = 2;      // 每 2 个轮询周期刷新一次会话列表
  var MAX_MSGS = 300;      // DOM 上限

  var $ = function (id) { return document.getElementById(id); };
  var state = {
    token: '',
    me: 0,
    conversations: [],
    activeId: 0,
    activePeer: 0,
    activeName: '',
    lastId: 0,
    rendered: Object.create(null),
    pollTimer: 0,
    ticks: 0,
    sending: false,
    tempId: -1,
    filter: ''
  };

  try {
    state.token = localStorage.getItem(TOKEN_KEY) || '';
    var u = JSON.parse(localStorage.getItem(USER_KEY) || 'null');
    state.me = (u && u.id) || 0;
  } catch (e) { state.token = ''; }

  function toast(text) {
    var el = $('toast');
    el.textContent = text;
    el.classList.add('show');
    clearTimeout(toast.timer);
    toast.timer = setTimeout(function () { el.classList.remove('show'); }, 2200);
  }

  function api(url, options) {
    var opts = options || {};
    var headers = opts.headers || {};
    if (state.token) { headers['Authorization'] = 'Bearer ' + state.token; }
    if (opts.body) { headers['Content-Type'] = 'application/json'; }
    opts.headers = headers;
    return fetch(url, opts).then(function (res) {
      return res.json().catch(function () { return {}; }).then(function (data) {
        return { status: res.status, data: data };
      });
    });
  }

  function fmtTime(sec) {
    if (!sec) { return ''; }
    var d = new Date(sec * 1000);
    var now = new Date();
    var hm = ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
    if (d.toDateString() === now.toDateString()) { return hm; }
    return (d.getMonth() + 1) + '/' + d.getDate() + ' ' + hm;
  }

  function initial(name) {
    if (!name) { return '?'; }
    return name.slice(0, 1).toUpperCase();
  }

  /* ---------- 会话列表 ---------- */

  function renderConversations() {
    var list = $('convList');
    list.textContent = '';
    var filter = state.filter.toLowerCase();
    var shown = state.conversations.filter(function (c) {
      return !filter || (c.peer_name || '').toLowerCase().indexOf(filter) >= 0;
    });
    if (!shown.length) {
      var empty = document.createElement('div');
      empty.className = 'empty';
      empty.textContent = state.conversations.length ? '没有匹配的会话' : '还没有私信，去用户主页发起聊天吧';
      list.appendChild(empty);
      return;
    }
    shown.forEach(function (c) {
      var row = document.createElement('div');
      row.className = 'conv' + (c.id === state.activeId ? ' active' : '');
      row.dataset.id = String(c.id);
      var av = document.createElement('div');
      av.className = 'avatar';
      av.textContent = initial(c.peer_name);
      var main = document.createElement('div');
      main.className = 'conv-main';
      var top = document.createElement('div');
      top.className = 'conv-top';
      var name = document.createElement('b');
      name.textContent = c.peer_name || ('用户 #' + c.peer_id);
      var time = document.createElement('time');
      time.textContent = fmtTime(c.updated_at);
      top.appendChild(name); top.appendChild(time);
      var preview = document.createElement('div');
      preview.className = 'conv-preview';
      var prefix = (c.last_sender_id && c.last_sender_id !== c.peer_id) ? '我：' : '';
      preview.textContent = (prefix + (c.last_content || '')).slice(0, 60);
      main.appendChild(top); main.appendChild(preview);
      row.appendChild(av); row.appendChild(main);
      if (c.unread > 0) {
        var badge = document.createElement('span');
        badge.className = 'badge';
        badge.textContent = c.unread > 99 ? '99+' : String(c.unread);
        main.appendChild(badge);
      }
      row.addEventListener('click', function () { openConversation(c); });
      list.appendChild(row);
    });
  }

  function loadConversations() {
    return api('/api/dm/conversations?limit=50').then(function (res) {
      if (res.status === 401) { showLoginTip(); return; }
      if (res.status !== 200) { return; }
      state.conversations = res.data.items || [];
      renderConversations();
    });
  }

  function totalUnread() {
    return state.conversations.reduce(function (sum, c) { return sum + (c.unread || 0); }, 0);
  }

  /* ---------- 会话选择 ---------- */

  function openConversation(c) {
    state.activeId = c.id;
    state.activePeer = c.peer_id;
    state.activeName = c.peer_name || ('用户 #' + c.peer_id);
    state.lastId = 0;
    state.rendered = Object.create(null);
    state.sending = false;
    $('peerName').textContent = state.activeName;
    $('peerSub').textContent = '与 @' + (c.peer_name || c.peer_id) + ' 的私信';
    $('peerAvatar').textContent = initial(state.activeName);
    $('chatEmpty').style.display = 'none';
    $('composer').style.display = 'flex';
    $('sendBtn').disabled = false;
    $('chatBody').querySelectorAll('.msg,.day-sep').forEach(function (n) { n.remove(); });
    document.body.classList.add('chat-open');
    renderConversations();
    loadMessages(true);
    markRead();
  }

  function closeChat() {
    document.body.classList.remove('chat-open');
  }

  /* ---------- 消息渲染 ---------- */

  function appendMessage(msg, pending) {
    if (state.rendered[msg.id] && !pending) { return; }
    state.rendered[msg.id] = true;
    var wrap = document.createElement('div');
    wrap.className = 'msg' + (msg.sender_id === state.me ? ' mine' : '');
    var bubble = document.createElement('div');
    bubble.className = 'bubble' + (msg.pending ? ' pending' : '');
    bubble.textContent = msg.content;
    var meta = document.createElement('div');
    meta.className = 'meta';
    meta.textContent = (msg.pending ? '发送中…' : fmtTime(msg.created_at));
    wrap.appendChild(bubble);
    var holder = document.createElement('div');
    holder.appendChild(meta);
    wrap.appendChild(holder);
    var body = $('chatBody');
    body.appendChild(wrap);
    while (body.querySelectorAll('.msg').length > MAX_MSGS) {
      var first = body.querySelector('.msg');
      if (!first) { break; }
      first.remove();
    }
    return bubble;
  }

  function scrollBottom(force) {
    var body = $('chatBody');
    var near = body.scrollHeight - body.scrollTop - body.clientHeight < 140;
    if (force || near) { body.scrollTop = body.scrollHeight; }
  }

  function loadMessages(reset) {
    if (!state.activeId) { return Promise.resolve(); }
    var after = reset ? 0 : state.lastId;
    var url = '/api/dm/' + state.activeId + '/messages?after=' + after + '&limit=50';
    return api(url).then(function (res) {
      if (res.status === 403) { toast('你不是该会话成员'); return; }
      if (res.status !== 200) { return; }
      var items = res.data.items || [];
      if (!items.length) { return; }
      var added = false;
      items.forEach(function (m) {
        if (!state.rendered[m.id]) {
          appendMessage(m, false);
          state.lastId = Math.max(state.lastId, m.id);
          added = true;
        }
      });
      if (added) { scrollBottom(reset); }
      if (items.some(function (m) { return m.sender_id !== state.me && !m.read_at; })) {
        markRead();
      }
    });
  }

  function markRead() {
    if (!state.activeId) { return; }
    api('/api/dm/' + state.activeId + '/read', { method: 'POST', body: '{}' })
      .then(function () {
        state.conversations.forEach(function (c) {
          if (c.id === state.activeId) { c.unread = 0; }
        });
        renderConversations();
      });
  }

  /* ---------- 发送（乐观上屏） ---------- */

  function send() {
    var input = $('input');
    var text = input.value.trim();
    if (!text || state.sending || !state.activeId) { return; }
    if (text.length > 500) { toast('消息不能超过 500 字'); return; }
    var temp = {
      id: state.tempId--,
      sender_id: state.me,
      content: text,
      created_at: Math.floor(Date.now() / 1000),
      pending: true
    };
    var bubble = appendMessage(temp, true);
    input.value = '';
    state.sending = true;
    $('sendBtn').disabled = true;
    scrollBottom(true);
    api('/api/dm/send', {
      method: 'POST',
      body: JSON.stringify({ to_user_id: state.activePeer, content: text })
    }).then(function (res) {
      state.sending = false;
      $('sendBtn').disabled = false;
      if (res.status !== 200 || !res.data.message) {
        bubble.classList.remove('pending');
        bubble.classList.add('failed');
        toast(res.data.error || '发送失败');
        return;
      }
      var msg = res.data.message;
      bubble.classList.remove('pending');
      bubble.parentElement.querySelector('.meta').textContent = fmtTime(msg.created_at);
      state.rendered[msg.id] = true;
      if (res.data.conversation_id && res.data.conversation_id !== state.activeId) {
        state.activeId = res.data.conversation_id;
      }
      state.lastId = Math.max(state.lastId, msg.id);
      loadConversations();
    }).catch(function () {
      state.sending = false;
      $('sendBtn').disabled = false;
      bubble.classList.remove('pending');
      bubble.classList.add('failed');
      toast('网络错误');
    });
  }

  /* ---------- 轮询 ---------- */

  function schedulePoll() {
    clearInterval(state.pollTimer);
    state.pollTimer = setInterval(function () {
      if (document.hidden) { return; }
      state.ticks++;
      if (state.activeId) { loadMessages(false); }
      if (state.ticks % LIST_EVERY === 0 || !state.conversations.length) {
        loadConversations();
      }
    }, POLL_MS);
  }

  function showLoginTip() {
    $('loginTip').style.display = 'block';
    $('composer').style.display = 'none';
  }

  /* ---------- 事件绑定 ---------- */

  $('sendBtn').addEventListener('click', send);
  $('input').addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); }
  });
  $('backBtn').addEventListener('click', closeChat);
  $('refreshBtn').addEventListener('click', function () {
    loadConversations();
    if (state.activeId) { loadMessages(true); }
  });
  $('convFilter').addEventListener('input', function (e) {
    state.filter = e.target.value || '';
    renderConversations();
  });

  /* ---------- 启动 ---------- */

  if (!state.token) {
    showLoginTip();
  } else {
    loadConversations().then(function () {
      var peer = /[?&]peer=(\d+)/.exec(location.search || '');
      var conv = /[?&]conversation=(\d+)/.exec(location.search || '');
      if (conv) {
        var hit = state.conversations.filter(function (c) { return c.id === Number(conv[1]); })[0];
        if (hit) { openConversation(hit); }
      } else if (peer) {
        var target = state.conversations.filter(function (c) {
          return c.peer_id === Number(peer[1]);
        })[0];
        if (target) { openConversation(target); }
        else { toast('还没有与这位用户的会话，去主页发一条吧'); }
      }
    });
    schedulePoll();
  }

  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) {
      loadConversations();
      if (state.activeId) { loadMessages(false); }
    }
  });
})();
</script>
</body>
</html>)HTML";

}  // namespace douyin
