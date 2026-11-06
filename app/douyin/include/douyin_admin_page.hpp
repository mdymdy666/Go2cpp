/**
 * @file douyin_admin_page.hpp
 * @brief 运营端「举报后台」单页 HTML（`GET /admin`）。
 *
 * @details 页面只负责展示与触发操作，所有数据都走 `/api/admin` 下的接口：
 *          - 顶部统计卡：待处理 / 今日新增 / 已处理；
 *          - 状态筛选（pending / handled）与分页（cursor）；
 *          - 举报表格：对象类型、对象摘要、原因、举报人、时间；
 *          - 每行「忽略」「删除内容」两个操作（仅 pending 行显示）。
 *
 *          token 来源与主站一致：`localStorage['douyin_token']`；也接受
 *          `?token=...`，便于验收脚本 / 首次打开时注入。页面本身不校验
 *          admin——接口在服务内校验，非 admin 一律 403。
 *
 * @note 纯静态字符串，无模板引擎；`R"HTML(...)HTML"` 内的 JS 使用
 *       `textContent` 写入用户数据，避免把举报内容当 HTML 执行。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#pragma once

namespace douyin {

/// @brief 举报后台页面 HTML；由 admin 服务在 `GET /admin` 直出。
inline constexpr char kAdminPageHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>举报后台 · 抖音</title>
<style>
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  body { margin: 0; font: 14px/1.5 system-ui, "PingFang SC", "Microsoft YaHei", sans-serif;
         background: #f5f6f8; color: #1d2129; }
  header { display: flex; flex-wrap: wrap; gap: 12px; align-items: center;
           padding: 16px 24px; background: #161823; color: #fff; }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; }
  .token-row { display: flex; gap: 8px; align-items: center; margin-left: auto; }
  .token-row label { font-size: 12px; opacity: .8; }
  .token-row input { width: 240px; padding: 6px 8px; border-radius: 6px;
                     border: 1px solid #3a3d4a; background: #242632; color: #fff; }
  main { max-width: 1180px; margin: 20px auto; padding: 0 16px 48px; }
  .stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; margin-bottom: 16px; }
  .card { background: #fff; border-radius: 10px; padding: 14px 18px;
          box-shadow: 0 1px 2px rgba(0,0,0,.06); }
  .card span { display: block; font-size: 26px; font-weight: 700; }
  .card small { color: #6b7280; }
  .toolbar { display: flex; gap: 10px; align-items: center; margin-bottom: 10px; }
  select, button { padding: 6px 12px; border-radius: 6px; border: 1px solid #d0d3d9;
                   background: #fff; cursor: pointer; font: inherit; }
  button.primary { background: #fe2c55; border-color: #fe2c55; color: #fff; }
  button.danger { border-color: #e5484d; color: #e5484d; }
  button:disabled { opacity: .45; cursor: not-allowed; }
  .hint { color: #6b7280; font-size: 12px; }
  .error { padding: 8px 12px; border-radius: 6px; background: #fdecec;
           color: #b42318; margin: 0 0 10px; }
  table { width: 100%; border-collapse: collapse; background: #fff;
          border-radius: 10px; overflow: hidden; box-shadow: 0 1px 2px rgba(0,0,0,.06); }
  th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid #f0f1f3;
           vertical-align: top; }
  th { background: #fafbfc; color: #4b5563; font-size: 12px; font-weight: 600; }
  tr:last-child td { border-bottom: 0; }
  .summary { max-width: 320px; overflow: hidden; text-overflow: ellipsis;
             white-space: nowrap; }
  .tag { display: inline-block; padding: 1px 8px; border-radius: 999px;
         font-size: 12px; background: #eef2ff; color: #4338ca; }
  .tag.handled { background: #ecfdf3; color: #027a48; }
  .actions { display: flex; gap: 8px; white-space: nowrap; }
  .pager { display: flex; gap: 12px; align-items: center; margin-top: 14px; }
</style>
</head>
<body>
<header>
  <h1>举报后台</h1>
  <div class="token-row">
    <label for="token">管理员 Token</label>
    <input id="token" type="password" autocomplete="off" placeholder="粘贴管理员 Bearer token">
    <button id="save" type="button">保存</button>
  </div>
</header>
<main>
  <section class="stats">
    <div class="card"><span id="stat-pending">-</span><small>待处理</small></div>
    <div class="card"><span id="stat-today">-</span><small>今日新增</small></div>
    <div class="card"><span id="stat-handled">-</span><small>已处理</small></div>
  </section>
  <section class="toolbar">
    <label for="status">状态</label>
    <select id="status">
      <option value="pending" selected>待处理</option>
      <option value="handled">已处理</option>
    </select>
    <button id="reload" type="button">刷新</button>
    <span class="hint" id="hint"></span>
  </section>
  <p class="error" id="error" hidden></p>
  <table>
    <thead>
      <tr>
        <th style="width:64px">ID</th>
        <th style="width:88px">类型</th>
        <th>对象摘要</th>
        <th style="width:140px">原因</th>
        <th style="width:120px">举报人</th>
        <th style="width:160px">时间</th>
        <th style="width:160px">操作</th>
      </tr>
    </thead>
    <tbody id="rows"></tbody>
  </table>
  <div class="pager">
    <button id="more" type="button" disabled>加载更多</button>
    <span class="hint" id="page-info"></span>
  </div>
</main>
<script>
(function () {
  'use strict';
  var TOKEN_KEY = 'douyin_token';
  var TYPE_LABEL = { video: '视频', comment: '评论', user: '用户' };
  var state = { status: 'pending', cursor: '', items: [], loading: false };

  function byId(id) { return document.getElementById(id); }
  function token() { return byId('token').value.trim(); }
  function setError(message) {
    var box = byId('error');
    if (!message) { box.hidden = true; box.textContent = ''; return; }
    box.hidden = false;
    box.textContent = message;
  }

  function api(path, options) {
    options = options || {};
    var headers = options.headers || {};
    headers['Authorization'] = 'Bearer ' + token();
    options.headers = headers;
    return fetch(path, options).then(function (resp) {
      return resp.text().then(function (text) {
        var data = null;
        try { data = JSON.parse(text); } catch (err) { data = null; }
        if (!resp.ok) {
          var message = (data && data.error) ? data.error : ('HTTP ' + resp.status);
          throw new Error(message);
        }
        return data || {};
      });
    });
  }

  function cell(text, className) {
    var td = document.createElement('td');
    if (className) { td.className = className; }
    td.textContent = text == null ? '' : String(text);
    return td;
  }

  function actionButton(label, className, disabled, onClick) {
    var button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    if (className) { button.className = className; }
    button.disabled = !!disabled;
    button.addEventListener('click', onClick);
    return button;
  }

  function renderRows(items) {
    var tbody = byId('rows');
    items.forEach(function (item) {
      var tr = document.createElement('tr');
      tr.appendChild(cell(item.id));
      var typeTd = document.createElement('td');
      var tag = document.createElement('span');
      tag.className = 'tag' + (item.status === 'handled' ? ' handled' : '');
      tag.textContent = TYPE_LABEL[item.target_type] || item.target_type;
      typeTd.appendChild(tag);
      tr.appendChild(typeTd);
      tr.appendChild(cell(item.target_summary || ('#' + item.target_id), 'summary'));
      tr.appendChild(cell(item.reason));
      tr.appendChild(cell(item.reporter_name || ('#' + item.reporter_id)));
      tr.appendChild(cell(item.created_at_text));
      var actions = document.createElement('td');
      actions.className = 'actions';
      if (item.status === 'pending') {
        actions.appendChild(actionButton('忽略', '', false, function () {
          handle(item.id, 'dismiss', '忽略');
        }));
        actions.appendChild(actionButton('删除内容', 'danger',
          item.target_type === 'user', function () {
            handle(item.id, 'delete_target', '删除内容');
          }));
      } else {
        actions.appendChild(cell('已处理'));
      }
      tr.appendChild(actions);
      tbody.appendChild(tr);
    });
  }

  function loadStats() {
    return api('/api/admin/stats').then(function (data) {
      byId('stat-pending').textContent = data.pending;
      byId('stat-today').textContent = data.today;
      byId('stat-handled').textContent = data.handled;
    }).catch(function (err) { setError(err.message); });
  }

  function loadReports(reset) {
    if (state.loading) { return Promise.resolve(); }
    if (!token()) { setError('请先填写管理员 token（与服务端 DOUYIN_ADMIN_USER_IDS 对应）'); return Promise.resolve(); }
    state.loading = true;
    var query = '/api/admin/reports?status=' + encodeURIComponent(state.status) + '&limit=20';
    if (!reset && state.cursor) { query += '&cursor=' + encodeURIComponent(state.cursor); }
    var body = reset ? byId('rows') : null;
    return api(query).then(function (data) {
      var items = data.items || [];
      if (reset) { state.items = []; state.cursor = ''; if (body) { body.textContent = ''; } }
      state.items = state.items.concat(items);
      renderRows(items);
      state.cursor = data.next_cursor || '';
      byId('more').disabled = !state.cursor;
      byId('page-info').textContent = '已加载 ' + state.items.length + ' 条';
      setError('');
    }).catch(function (err) { setError(err.message); }).then(function () {
      state.loading = false;
    });
  }

  function loadAll() {
    return Promise.all([loadStats(), loadReports(true)]);
  }

  function handle(id, action, label) {
    if (!window.confirm('确定对举报 #' + id + ' 执行「' + label + '」？')) { return; }
    api('/api/admin/reports/' + id + '/handle', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: action })
    }).then(function (data) {
      setError('');
      byId('hint').textContent = '举报 #' + id + ' 已处理' + (data.deleted ? '（内容已删除）' : '');
      loadAll();
    }).catch(function (err) { setError(err.message); });
  }

  var saved = localStorage.getItem(TOKEN_KEY) || '';
  var fromUrl = new URLSearchParams(window.location.search).get('token');
  if (fromUrl) { saved = fromUrl; localStorage.setItem(TOKEN_KEY, fromUrl); }
  byId('token').value = saved;
  byId('save').addEventListener('click', function () {
    localStorage.setItem(TOKEN_KEY, token());
    byId('hint').textContent = 'token 已保存到本机';
    loadAll();
  });
  byId('reload').addEventListener('click', loadAll);
  byId('status').addEventListener('change', function () {
    state.status = this.value;
    loadReports(true);
  });
  byId('more').addEventListener('click', function () { loadReports(false); });
  loadAll();
})();
</script>
</body>
</html>)HTML";

}  // namespace douyin
