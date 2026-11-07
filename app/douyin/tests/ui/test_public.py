"""公网前缀 `/douyin/`：首页 / 视频页 / API 都必须在带前缀时可用。"""
from playwright.sync_api import Page

from conftest import BASE


def _media_ok(page: Page) -> dict:
    """只检查首屏第一张卡的视频——其余卡片是懒加载，readyState=0 正常。"""
    return page.evaluate(
        """() => {
            const out = {videos: 0, bad: []};
            const v = document.querySelector('.feed .slide video');
            if (!v || (!v.currentSrc && !v.src)) return out;
            out.videos = 1;
            if ((v.error && v.error.code) || v.readyState < 2) {
                out.bad.push({rs: v.readyState, err: v.error ? v.error.code : 0});
            }
            return out;
        }"""
    )


def test_public_prefix_home(page: Page, console_errors):
    """/douyin/ 首页：HTTP 200、feed 渲染、首屏视频可播、无 4xx/5xx。"""
    resp = page.goto(BASE + "/douyin/", wait_until="networkidle")
    assert resp is not None and resp.status == 200, f"/douyin/ 状态 {resp and resp.status}"
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(1200)
    assert page.locator(".feed .slide").count() >= 1, "前缀首页没有 feed"
    media = _media_ok(page)
    assert media["videos"] >= 1, "前缀首页没有视频"
    assert not media["bad"], f"前缀首页有视频加载失败: {media['bad']}"
    assert not console_errors, f"前缀首页控制台/网络错误: {console_errors[:3]}"


def test_public_prefix_share(page: Page):
    """/douyin/v/1 分享页：200、OG 完整、视频可播。"""
    resp = page.goto(BASE + "/douyin/v/1", wait_until="networkidle")
    assert resp is not None and resp.status == 200, f"状态 {resp and resp.status}"
    meta = page.evaluate(
        "() => Array.from(document.querySelectorAll('meta[property^=\"og:\"]'))"
        ".map(m => m.getAttribute('property'))")
    for key in ["og:title", "og:image", "og:video"]:
        assert key in meta, f"前缀分享页缺少 {key}"
    page.wait_for_timeout(800)
    state = page.evaluate(
        "() => { const v = document.querySelector('video');"
        " return v ? {rs: v.readyState, err: v.error ? v.error.code : 0} : null; }")
    assert state and state["err"] == 0 and state["rs"] >= 2, f"视频不可播: {state}"


def test_public_prefix_api(page: Page):
    """/douyin/api/feed：200 且返回真实卡片数据（不是 404/空壳）。"""
    page.goto(BASE + "/douyin/", wait_until="networkidle")
    out = page.evaluate(
        """async () => {
            const r = await fetch('/douyin/api/feed?limit=3');
            const text = await r.text();
            let data = null;
            try { data = JSON.parse(text); } catch (e) { data = null; }
            return {status: r.status, count: data ? (data.items || []).length : -1,
                    text: text.slice(0, 120)};
        }"""
    )
    assert out["status"] == 200, f"/douyin/api/feed 状态 {out['status']}: {out['text']}"
    assert out["count"] >= 1, f"API 没有数据: {out}"
