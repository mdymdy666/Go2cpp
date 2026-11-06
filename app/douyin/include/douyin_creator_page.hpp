/**
 * @file douyin_creator_page.hpp
 * @brief 创作者中心页面（第 13 轮）：`GET /creator` 直出。
 *
 * @details 页面只读自己（Bearer token 从 localStorage `douyin_token` 取）：
 *          - 4 个数字卡片：总播放量（按互动量估算）、总点赞、总评论、粉丝数；
 *          - 互动趋势：7 天 / 30 天切换，CSS 柱状图（点赞 / 评论 / 粉丝三色），
 *            不引入任何第三方图表库；
 *          - TOP5 作品：按互动量（赞×2 + 评论×3）排序，点击进入 `/v/<id>`。
 *
 *          数据全部来自 `/api/creator/overview|trend|top-videos`，页面不缓存。
 *          用户内容一律用 textContent 写入，防 XSS。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#pragma once

namespace douyin {

inline constexpr const char* kCreatorPageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>创作者中心 · 抖音 Demo</title>
<style>
  :root {
    --bg: #08080d;
    --card: #16161f;
    --line: rgba(255, 255, 255, .1);
    --text: #f4f4f7;
    --muted: rgba(255, 255, 255, .55);
    --pink: #fe2c55;
    --cyan: #25f4ee;
    --gold: #ffc44d;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; padding: 0; background: var(--bg); color: var(--text); }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
      "Microsoft YaHei", sans-serif;
    min-height: 100vh;
    background:
      radial-gradient(900px 500px at 10% -10%, rgba(254, 44, 85, .16), transparent 60%),
      radial-gradient(700px 420px at 95% 0%, rgba(37, 244, 238, .12), transparent 60%),
      var(--bg);
  }
  header.top {
    display: flex; align-items: center; gap: 12px;
    padding: 14px 22px; border-bottom: 1px solid var(--line);
    position: sticky; top: 0; z-index: 2;
    background: rgba(8, 8, 13, .82); backdrop-filter: blur(10px);
  }
  header.top b { font-size: 16px; }
  header.top .sub { font-size: 12px; color: var(--muted); }
  header.top .spacer { flex: 1; }
  a.back, button.ghost {
    color: var(--text); text-decoration: none; font-size: 13px;
    padding: 8px 12px; border-radius: 10px; border: 1px solid var(--line);
    background: rgba(255, 255, 255, .04); cursor: pointer;
  }
  a.back:hover, button.ghost:hover { border-color: rgba(255, 255, 255, .28); }
  main { max-width: 980px; margin: 24px auto 64px; padding: 0 18px; }
  .gate {
    display: flex; align-items: center; gap: 14px; flex-wrap: wrap;
    padding: 18px 20px; margin-bottom: 20px; border-radius: 16px;
    border: 1px solid rgba(254, 44, 85, .35); background: rgba(254, 44, 85, .08);
    font-size: 14px; line-height: 1.6;
  }
  .gate a { color: var(--cyan); }
  .cards {
    display: grid; gap: 14px;
    grid-template-columns: repeat(4, minmax(0, 1fr));
  }
  @media (max-width: 720px) { .cards { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
  .card {
    background: var(--card); border: 1px solid var(--line); border-radius: 16px;
    padding: 18px 20px; box-shadow: 0 18px 40px rgba(0, 0, 0, .28);
  }
  .card .label { font-size: 12px; color: var(--muted); letter-spacing: .04em; }
  .card .value {
    margin-top: 10px; font-size: 30px; font-weight: 800; line-height: 1.1;
    font-variant-numeric: tabular-nums;
  }
  .card .hint { margin-top: 6px; font-size: 11px; color: var(--muted); }
  .card.plays .value { color: var(--cyan); }
  .card.likes .value { color: var(--pink); }
  .card.comments .value { color: var(--gold); }
  .card.followers .value { color: #9d8cff; }
  .panel {
    margin-top: 18px; background: var(--card); border: 1px solid var(--line);
    border-radius: 18px; padding: 20px 22px;
  }
  .panel-head {
    display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap;
    margin-bottom: 6px;
  }
  .panel-head h2 { font-size: 15px; margin: 0; }
  .panel-head .hint { font-size: 12px; color: var(--muted); }
  .panel-head .spacer { flex: 1; }
  .ranges { display: flex; gap: 8px; }
  .ranges button {
    font-size: 12px; color: var(--muted); padding: 6px 12px; cursor: pointer;
    background: rgba(255, 255, 255, .04); border: 1px solid var(--line);
    border-radius: 999px;
  }
  .ranges button.on {
    color: #08080d; background: var(--cyan); border-color: var(--cyan);
    font-weight: 700;
  }
  .legend { display: flex; gap: 16px; margin: 10px 0 4px; font-size: 12px; color: var(--muted); }
  .legend i { display: inline-block; width: 9px; height: 9px; border-radius: 2px; margin-right: 6px; }
  .legend .l-like i { background: var(--pink); }
  .legend .l-comment i { background: var(--cyan); }
  .legend .l-follow i { background: var(--gold); }
  .chart {
    display: flex; align-items: flex-end; gap: 8px;
    height: 190px; margin-top: 16px; padding-top: 12px;
    border-bottom: 1px solid var(--line);
  }
  .day { flex: 1 1 0; min-width: 0; display: flex; flex-direction: column; height: 100%; }
  .bars {
    flex: 1; display: flex; align-items: flex-end; justify-content: center; gap: 2px;
    min-height: 0;
  }
  .bar {
    flex: 1 1 0; max-width: 9px; min-height: 2px; border-radius: 3px 3px 0 0;
    transition: height .25s ease;
  }
  .bar.like { background: linear-gradient(180deg, #ff6684, var(--pink)); }
  .bar.comment { background: linear-gradient(180deg, #8ffcf8, var(--cyan)); }
  .bar.follow { background: linear-gradient(180deg, #ffd98a, var(--gold)); }
  .dlabel {
    height: 20px; margin-top: 6px; text-align: center;
    font-size: 10px; color: var(--muted); white-space: nowrap;
  }
  .dlabel.off { visibility: hidden; }
  .empty { margin: 14px 0 2px; font-size: 13px; color: var(--muted); }
  .top-list { list-style: none; margin: 8px 0 0; padding: 0; }
  .top-item {
    display: flex; align-items: center; gap: 14px;
    padding: 12px 4px; border-top: 1px solid var(--line);
  }
  .top-item:first-child { border-top: 0; }
  .rank {
    flex: 0 0 28px; height: 28px; border-radius: 9px; display: grid;
    place-items: center; font-size: 13px; font-weight: 800;
    background: rgba(255, 255, 255, .06); color: var(--muted);
  }
  .top-item:nth-child(1) .rank { background: rgba(254, 44, 85, .18); color: var(--pink); }
  .top-item:nth-child(2) .rank { background: rgba(255, 196, 77, .16); color: var(--gold); }
  .top-item:nth-child(3) .rank { background: rgba(37, 244, 238, .14); color: var(--cyan); }
  .video { flex: 1; min-width: 0; }
  .video a {
    color: var(--text); text-decoration: none; font-size: 14px; font-weight: 600;
    display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  .video a:hover { color: var(--cyan); }
  .video .meta { margin-top: 4px; font-size: 12px; color: var(--muted); }
  .score {
    flex: 0 0 auto; font-size: 13px; color: var(--muted);
    font-variant-numeric: tabular-nums;
  }
  .toast {
    position: fixed; left: 50%; bottom: 28px; transform: translateX(-50%);
    background: rgba(22, 22, 31, .96); border: 1px solid var(--line);
    color: var(--text); font-size: 13px; padding: 10px 16px; border-radius: 12px;
    box-shadow: 0 18px 40px rgba(0, 0, 0, .45); z-index: 9;
  }
</style>
</head>
<body>
<header class="top">
  <a class="back" href="/">← 返回首页</a>
  <b>创作者中心</b>
  <span class="sub" id="videoCount">作品 —</span>
  <span class="spacer"></span>
  <button class="ghost" id="refresh" type="button">刷新</button>
</header>
<main>
  <div class="gate" id="gate" hidden>
    <span id="gateText">登录后查看你的创作者数据。</span>
    <a href="/">去登录 / 注册</a>
  </div>

  <section class="cards" aria-label="核心指标">
    <div class="card plays">
      <div class="label">总播放量</div>
      <div class="value" id="statPlays">—</div>
      <div class="hint">按点赞 + 评论估算</div>
    </div>
    <div class="card likes">
      <div class="label">总点赞</div>
      <div class="value" id="statLikes">—</div>
      <div class="hint">作品收到的全部点赞</div>
    </div>
    <div class="card comments">
      <div class="label">总评论</div>
      <div class="value" id="statComments">—</div>
      <div class="hint">未删除评论</div>
    </div>
    <div class="card followers">
      <div class="label">粉丝数</div>
      <div class="value" id="statFollowers">—</div>
      <div class="hint">关注你的人</div>
    </div>
  </section>

  <section class="panel">
    <div class="panel-head">
      <h2>互动趋势</h2>
      <span class="hint" id="trendPeak"></span>
      <span class="spacer"></span>
      <div class="ranges">
        <button type="button" data-days="7" class="on">近 7 天</button>
        <button type="button" data-days="30">近 30 天</button>
      </div>
    </div>
    <div class="legend">
      <span class="l-like"><i></i>新增点赞</span>
      <span class="l-comment"><i></i>新增评论</span>
      <span class="l-follow"><i></i>新增粉丝</span>
    </div>
    <div class="chart" id="trendChart" role="img" aria-label="按天的点赞、评论、粉丝柱状图"></div>
    <p class="empty" id="trendEmpty" hidden>这段时间还没有新增互动。</p>
  </section>

  <section class="panel">
    <div class="panel-head">
      <h2>TOP 5 作品</h2>
      <span class="hint">互动量 = 点赞 × 2 + 评论 × 3</span>
    </div>
    <ol class="top-list" id="topVideos"></ol>
    <p class="empty" id="topEmpty" hidden>还没有发布作品。</p>
  </section>
</main>
<div class="toast" id="toast" hidden></div>
<script>
(function () {
  'use strict';
  var TOKEN_KEY = 'douyin_token';
  var MAX_BAR_PX = 132;
  var state = { days: 7, loading: false };

  function token() {
    try { return localStorage.getItem(TOKEN_KEY) || ''; } catch (e) { return ''; }
  }
  function showToast(text) {
    var toast = document.getElementById('toast');
    toast.textContent = text;
    toast.hidden = false;
    window.clearTimeout(showToast.timer);
    showToast.timer = window.setTimeout(function () { toast.hidden = true; }, 2600);
  }
  function setText(id, value) {
    document.getElementById(id).textContent = value;
  }
  function fmt(value) {
    var n = Number(value) || 0;
    try { return n.toLocaleString('zh-CN'); } catch (e) { return String(n); }
  }
  function api(path) {
    var headers = {};
    var tok = token();
    if (tok) { headers.Authorization = 'Bearer ' + tok; }
    return fetch('/api/creator/' + path, { headers: headers, cache: 'no-store' })
      .then(function (response) {
        if (!response.ok) {
          var error = new Error('HTTP ' + response.status);
          error.status = response.status;
          throw error;
        }
        return response.json();
      });
  }
  function onError(error) {
    if (error && error.status === 401) {
      showToast('登录已过期，请回首页重新登录');
    } else if (error && error.status === 403) {
      showToast('只能查看自己的数据');
    } else {
      showToast('数据加载失败，请稍后重试');
    }
  }

  function renderOverview(overview) {
    setText('statPlays', fmt(overview.total_plays));
    setText('statLikes', fmt(overview.total_likes));
    setText('statComments', fmt(overview.total_comments));
    setText('statFollowers', fmt(overview.followers));
    setText('videoCount', '作品 ' + fmt(overview.video_count) + ' 个');
  }

  function bar(value, maxValue, cls) {
    var height = maxValue > 0 ? Math.round((value / maxValue) * MAX_BAR_PX) : 0;
    if (value > 0 && height < 3) { height = 3; }
    var el = document.createElement('div');
    el.className = 'bar ' + cls;
    el.style.height = height + 'px';
    el.title = value + ' 个';
    return el;
  }

  function renderTrend(payload) {
    var chart = document.getElementById('trendChart');
    chart.textContent = '';
    var points = payload.points || [];
    var maxValue = 0;
    var total = 0;
    for (var i = 0; i < points.length; i++) {
      maxValue = Math.max(maxValue, points[i].likes, points[i].comments, points[i].followers);
      total += points[i].likes + points[i].comments + points[i].followers;
    }
    var peak = document.getElementById('trendPeak');
    peak.textContent = maxValue > 0 ? '单日峰值 ' + fmt(maxValue) : '';
    document.getElementById('trendEmpty').hidden = total > 0;

    var labelEvery = points.length > 10 ? 5 : 1;
    for (var j = 0; j < points.length; j++) {
      var point = points[j];
      var day = document.createElement('div');
      day.className = 'day';

      var bars = document.createElement('div');
      bars.className = 'bars';
      bars.appendChild(bar(point.likes, maxValue, 'like'));
      bars.appendChild(bar(point.comments, maxValue, 'comment'));
      bars.appendChild(bar(point.followers, maxValue, 'follow'));
      day.appendChild(bars);

      var label = document.createElement('div');
      label.className = 'dlabel';
      var showLabel = points.length <= 7 || j % labelEvery === 0;
      if (!showLabel) { label.className += ' off'; }
      label.textContent = String(point.date || '').slice(5);
      label.title = point.date || '';
      day.appendChild(label);
      chart.appendChild(day);
    }
  }

  function renderTopVideos(payload) {
    var list = document.getElementById('topVideos');
    list.textContent = '';
    var videos = payload.videos || [];
    document.getElementById('topEmpty').hidden = videos.length > 0;
    for (var i = 0; i < videos.length; i++) {
      var video = videos[i];
      var item = document.createElement('li');
      item.className = 'top-item';

      var rank = document.createElement('span');
      rank.className = 'rank';
      rank.textContent = String(i + 1);
      item.appendChild(rank);

      var box = document.createElement('div');
      box.className = 'video';
      var link = document.createElement('a');
      link.href = '/v/' + encodeURIComponent(video.id);
      link.textContent = video.title || ('作品 #' + video.id);
      box.appendChild(link);
      var meta = document.createElement('div');
      meta.className = 'meta';
      meta.textContent = '点赞 ' + fmt(video.like_count) + ' · 评论 ' +
        fmt(video.comment_count);
      box.appendChild(meta);
      item.appendChild(box);

      var score = document.createElement('span');
      score.className = 'score';
      score.textContent = '互动分 ' + fmt(video.score);
      item.appendChild(score);

      list.appendChild(item);
    }
  }

  function load() {
    if (!token()) {
      document.getElementById('gate').hidden = false;
      return;
    }
    if (state.loading) { return; }
    state.loading = true;
    document.getElementById('gate').hidden = true;
    Promise.all([
      api('overview').then(renderOverview),
      api('trend?days=' + state.days).then(renderTrend),
      api('top-videos?limit=5').then(renderTopVideos)
    ]).catch(onError).then(function () {
      state.loading = false;
    });
  }

  var buttons = document.querySelectorAll('.ranges button');
  for (var i = 0; i < buttons.length; i++) {
    buttons[i].addEventListener('click', function () {
      for (var k = 0; k < buttons.length; k++) {
        buttons[k].classList.remove('on');
      }
      this.classList.add('on');
      state.days = Number(this.getAttribute('data-days')) === 30 ? 30 : 7;
      api('trend?days=' + state.days).then(renderTrend).catch(onError);
    });
  }
  document.getElementById('refresh').addEventListener('click', load);

  if (!token()) {
    document.getElementById('gate').hidden = false;
  } else {
    load();
  }
})();
</script>
</body>
</html>
)HTML";

}  // namespace douyin
