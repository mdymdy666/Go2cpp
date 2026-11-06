/**
 * @file douyin_settings_page.hpp
 * @brief 资料编辑页（第 12 轮）：网关直出 `GET /settings`。
 *
 * @details 页面只做三件事：
 *          - 打开时 `GET /api/user/me` 拉当前资料（localStorage 只用来带 token）；
 *          - 提交时 `PATCH /api/user/me`，前端做与后端一致的校验
 *            （昵称 2~20 码点、无 @/空白；简介 ≤80；头像 http(s)/videos 白名单）；
 *          - 保存成功后把最新 user 写回 localStorage 再回首页，顶栏头像/昵称
 *            立即刷新（首页启动时从 localStorage 读 user）。
 *
 * @note 无框架、无构建依赖；用户内容全部用 textContent/value 写入，防 XSS。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#pragma once

namespace douyin {

inline constexpr const char* kSettingsPageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>编辑资料 · 抖音 Demo</title>
<style>
  :root {
    --bg: #08080d;
    --card: #16161f;
    --line: rgba(255, 255, 255, .1);
    --text: #f4f4f7;
    --muted: rgba(255, 255, 255, .55);
    --pink: #fe2c55;
    --cyan: #25f4ee;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; padding: 0; background: var(--bg); color: var(--text); }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
      "Microsoft YaHei", sans-serif;
    min-height: 100vh;
    background:
      radial-gradient(900px 500px at 15% -10%, rgba(254, 44, 85, .18), transparent 60%),
      radial-gradient(700px 420px at 95% 0%, rgba(37, 244, 238, .12), transparent 60%),
      var(--bg);
  }
  header.top {
    display: flex; align-items: center; gap: 12px;
    padding: 16px 22px; border-bottom: 1px solid var(--line);
    position: sticky; top: 0; z-index: 2;
    background: rgba(8, 8, 13, .82); backdrop-filter: blur(10px);
  }
  header.top b { font-size: 16px; }
  header.top .spacer { flex: 1; }
  a.back, button.ghost {
    color: var(--text); text-decoration: none; font-size: 13px;
    padding: 8px 12px; border-radius: 10px; border: 1px solid var(--line);
    background: rgba(255, 255, 255, .04); cursor: pointer;
  }
  a.back:hover, button.ghost:hover { border-color: rgba(255, 255, 255, .28); }
  main { max-width: 620px; margin: 28px auto 60px; padding: 0 18px; }
  .card {
    background: var(--card); border: 1px solid var(--line);
    border-radius: 18px; padding: 24px;
    box-shadow: 0 24px 60px rgba(0, 0, 0, .35);
  }
  .avatar-row { display: flex; align-items: center; gap: 16px; margin-bottom: 18px; }
  .avatar {
    width: 68px; height: 68px; border-radius: 50%;
    display: grid; place-items: center; overflow: hidden;
    background: linear-gradient(135deg, var(--cyan), var(--pink));
    color: #08080d; font-size: 30px; font-weight: 800; flex: 0 0 auto;
  }
  .avatar img { width: 100%; height: 100%; object-fit: cover; }
  .avatar-row .hint { font-size: 12px; color: var(--muted); line-height: 1.6; }
  label { display: flex; align-items: baseline; gap: 8px; font-size: 13px;
    color: var(--muted); margin: 18px 0 7px; }
  label .count, label .tip { margin-left: auto; font-size: 12px; color: var(--muted); }
  input, textarea {
    width: 100%; padding: 11px 13px; border-radius: 12px;
    border: 1px solid var(--line); background: rgba(255, 255, 255, .05);
    color: var(--text); font-size: 14px; font-family: inherit;
    outline: none; resize: vertical;
  }
  input:focus, textarea:focus { border-color: rgba(254, 44, 85, .65); }
  .err { min-height: 18px; margin: 5px 0 0; font-size: 12px; color: #ff8fa3; }
  button.primary {
    width: 100%; margin-top: 14px; padding: 12px;
    border: 0; border-radius: 12px; cursor: pointer;
    background: linear-gradient(90deg, var(--pink), #ff6b81);
    color: #fff; font-size: 15px; font-weight: 700;
  }
  button.primary:disabled { opacity: .6; cursor: not-allowed; }
  .ok { min-height: 20px; margin: 10px 0 0; font-size: 13px; color: #57e6a9; }
  .toast {
    position: fixed; left: 50%; bottom: 36px; transform: translateX(-50%);
    background: rgba(20, 20, 28, .95); border: 1px solid var(--line);
    padding: 10px 16px; border-radius: 12px; font-size: 13px;
    opacity: 0; pointer-events: none; transition: opacity .18s ease;
    max-width: 86vw;
  }
  .toast.show { opacity: 1; }
  .toast.err { color: #ff8fa3; }
  .toast.ok { color: #57e6a9; }
  @media (max-width: 520px) {
    main { margin-top: 16px; }
    .card { padding: 18px; }
  }
</style>
</head>
<body>
<header class="top">
  <a class="back" id="backLink" href="./">← 返回</a>
  <b>编辑资料</b>
  <span class="spacer"></span>
  <button class="ghost" id="logoutBtn" type="button">退出登录</button>
</header>
<main>
  <div class="card">
    <div class="avatar-row">
      <span class="avatar" id="avatarPreview">?</span>
      <div class="hint">
        头像地址可填 http(s) 图片，或本站 <code>videos/</code> 下的 jpg/png。<br>
        留空则用昵称首字母。
      </div>
    </div>
    <form id="form" novalidate>
      <label for="name">昵称 <span class="count" id="nameCount">0/20</span></label>
      <input id="name" maxlength="60" autocomplete="nickname" placeholder="2~20 个字，不含 @ 与空格">
      <p class="err" id="nameErr"></p>

      <label for="bio">简介 <span class="count" id="bioCount">0/80</span></label>
      <textarea id="bio" rows="3" maxlength="240" placeholder="介绍一下自己（最多 80 字）"></textarea>
      <p class="err" id="bioErr"></p>

      <label for="avatar">头像地址 <span class="tip">可留空</span></label>
      <input id="avatar" maxlength="512" placeholder="https://example.com/a.jpg 或 videos/cover_1.jpg">
      <p class="err" id="avatarErr"></p>

      <button class="primary" id="saveBtn" type="submit">保存</button>
      <p class="ok" id="okMsg"></p>
    </form>
  </div>
</main>
<div class="toast" id="toast" role="status" aria-live="polite"></div>
<script>
(function () {
  'use strict';
  var BASE = location.pathname.replace(/[^/]*$/, '');
  var TOKEN_KEY = 'douyin_token';
  var USER_KEY = 'douyin_user';
  var token = localStorage.getItem(TOKEN_KEY) || '';
  var state = { user: null, busy: false };
  var nameEl = document.getElementById('name');
  var bioEl = document.getElementById('bio');
  var avatarEl = document.getElementById('avatar');
  var nameErr = document.getElementById('nameErr');
  var bioErr = document.getElementById('bioErr');
  var avatarErr = document.getElementById('avatarErr');
  var okMsg = document.getElementById('okMsg');
  var preview = document.getElementById('avatarPreview');
  var saveBtn = document.getElementById('saveBtn');

  function toast(message, kind) {
    var el = document.getElementById('toast');
    el.textContent = message;
    el.className = 'toast show' + (kind ? ' ' + kind : '');
    clearTimeout(toast._timer);
    toast._timer = setTimeout(function () { el.className = 'toast'; }, 2200);
  }

  function chars(text) {
    // 按 Unicode 码点计数（与后端 Utf8Length 一致），不用 length（UTF-16 单元）。
    var count = 0;
    for (var i = 0; i < text.length; i++) {
      var code = text.charCodeAt(i);
      if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) {
        var next = text.charCodeAt(i + 1);
        if (next >= 0xDC00 && next <= 0xDFFF) i++;
      }
      count++;
    }
    return count;
  }

  function avatarSrc(url) {
    if (!url) return '';
    if (/^https?:\/\//i.test(url)) return url;
    if (url.indexOf('videos/') === 0) return BASE + url;
    return '';
  }

  function paintPreview() {
    var src = avatarSrc(avatarEl.value.trim());
    preview.innerHTML = '';
    if (src) {
      var img = document.createElement('img');
      img.alt = '头像预览';
      img.onerror = function () {
        preview.textContent = initial(nameEl.value.trim() || '?');
      };
      img.src = src;
      preview.appendChild(img);
    } else {
      preview.textContent = initial(nameEl.value.trim() || '?');
    }
  }

  function initial(text) {
    var trimmed = (text || '').trim();
    if (!trimmed) return '?';
    var first = trimmed.charAt(0);
    return first.toUpperCase();
  }

  function setError(el, message) {
    el.textContent = message || '';
    return !message;
  }

  function validateName(show) {
    var value = nameEl.value.trim();
    var n = chars(value);
    document.getElementById('nameCount').textContent = n + '/20';
    var message = '';
    if (n < 2 || n > 20) message = '昵称需要 2~20 个字';
    else if (/[@\s\u3000]/.test(value)) message = '昵称不能包含 @ 或空白';
    if (!show) message = message ? message : '';
    return setError(nameErr, show ? message : '') && !message;
  }

  function validateBio(show) {
    var value = bioEl.value.trim();
    var n = chars(value);
    document.getElementById('bioCount').textContent = n + '/80';
    var message = n > 80 ? '简介不能超过 80 个字' : '';
    return setError(bioErr, show ? message : '') && !message;
  }

  function validateAvatar(show) {
    var value = avatarEl.value.trim();
    var message = '';
    if (value) {
      if (/^https?:\/\//i.test(value)) {
        if (value.length > 512) message = '头像地址过长';
      } else {
        var m = /^videos\/([A-Za-z0-9_.-]{1,128})$/.exec(value);
        if (!m || !/\.(jpg|png)$/i.test(value)) {
          message = '只支持 http(s) 链接或 videos/ 下的 jpg/png';
        }
      }
    }
    return setError(avatarErr, show ? message : '') && !message;
  }

  function api(path, options) {
    options = options || {};
    var headers = { 'Content-Type': 'application/json' };
    if (token) headers['Authorization'] = 'Bearer ' + token;
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
          throw err;
        }
        return data;
      });
    });
  }

  function fill(user) {
    state.user = user || {};
    nameEl.value = state.user.name || '';
    bioEl.value = state.user.bio || '';
    avatarEl.value = state.user.avatar_url || '';
    validateName(false);
    validateBio(false);
    validateAvatar(false);
    paintPreview();
  }

  function save(event) {
    event.preventDefault();
    if (state.busy) return;
    var okName = validateName(true);
    var okBio = validateBio(true);
    var okAvatar = validateAvatar(true);
    if (!okName || !okBio || !okAvatar) {
      toast('请先修正表单里的问题', 'err');
      return;
    }
    state.busy = true;
    saveBtn.disabled = true;
    saveBtn.textContent = '保存中…';
    okMsg.textContent = '';
    api('api/user/me', {
      method: 'PATCH',
      body: {
        name: nameEl.value.trim(),
        bio: bioEl.value.trim(),
        avatar_url: avatarEl.value.trim()
      }
    }).then(function (data) {
      var user = data.user || {};
      state.user = user;
      try { localStorage.setItem(USER_KEY, JSON.stringify(user)); } catch (e) {}
      toast('资料已保存', 'ok');
      okMsg.textContent = '保存成功，正在返回首页…';
      setTimeout(function () { location.href = BASE; }, 700);
    }).catch(function (err) {
      if (err.status === 409) {
        setError(nameErr, '昵称已被占用');
        toast('昵称已被占用', 'err');
      } else if (err.status === 401) {
        localStorage.removeItem(TOKEN_KEY);
        localStorage.removeItem(USER_KEY);
        toast('登录已失效，请重新登录', 'err');
        setTimeout(function () { location.href = BASE; }, 900);
      } else {
        toast('保存失败：' + (err.message || '网络错误'), 'err');
      }
    }).then(function () {
      state.busy = false;
      saveBtn.disabled = false;
      saveBtn.textContent = '保存';
    });
  }

  nameEl.addEventListener('input', function () { validateName(false); paintPreview(); });
  bioEl.addEventListener('input', function () { validateBio(false); });
  avatarEl.addEventListener('input', function () { validateAvatar(false); paintPreview(); });
  document.getElementById('form').addEventListener('submit', save);
  document.getElementById('backLink').addEventListener('click', function (e) {
    e.preventDefault();
    location.href = BASE;
  });
  document.getElementById('logoutBtn').addEventListener('click', function () {
    localStorage.removeItem(TOKEN_KEY);
    localStorage.removeItem(USER_KEY);
    location.href = BASE;
  });

  if (!token) {
    toast('请先在首页登录', 'err');
    setTimeout(function () { location.href = BASE; }, 1000);
    return;
  }
  // 首屏以服务端为准：localStorage 里的 user 可能已被其它设备改过。
  api('api/user/me').then(function (data) {
    fill(data.user);
  }).catch(function (err) {
    if (err.status === 401) {
      localStorage.removeItem(TOKEN_KEY);
      localStorage.removeItem(USER_KEY);
      toast('登录已失效，请重新登录', 'err');
      setTimeout(function () { location.href = BASE; }, 1000);
      return;
    }
    toast('加载资料失败：' + (err.message || '网络错误'), 'err');
  });
})();
</script>
</body>
</html>))HTML";

}  // namespace douyin
