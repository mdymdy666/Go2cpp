"""首页 `/`：feed 渲染 / 视频真加载 / 点赞持久化 / 滚动 / 搜索 / 关注流。"""
import time

import pytest
from playwright.sync_api import Page, expect

from conftest import BASE, USER, api, login


def _feed_ready(page: Page) -> None:
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(800)


def test_home_renders_feed(logged_in: Page, console_errors):
    """首页 feed 有卡片、无用户可见错误文案、控制台干净。"""
    _feed_ready(logged_in)
    slides = logged_in.locator(".feed .slide")
    assert slides.count() >= 1, "首页没有渲染任何 feed 卡片"
    expect(slides.first).to_be_visible()
    visible = logged_in.inner_text("body")
    for bad in ["加载失败", "视频加载失败", "undefined", "null"]:
        assert bad not in visible, f"页面出现用户可见错误文案: {bad}"
    assert not console_errors, f"控制台/网络有错误: {console_errors[:3]}"


def test_home_first_video_actually_loads(logged_in: Page):
    """首屏视频必须真的解码成功（readyState >= 2、无 error）。"""
    _feed_ready(logged_in)
    state = logged_in.evaluate(
        """() => {
            const v = document.querySelector('.feed .slide video');
            if (!v) return {found: false};
            return {
                found: true,
                readyState: v.readyState,
                error: v.error ? v.error.code : 0,
                paused: v.paused,
                src: (v.currentSrc || v.src || '').slice(-40),
            };
        }"""
    )
    assert state["found"], "首页没有 video 元素"
    assert state["error"] == 0, f"首屏视频错误: {state}"
    assert state["readyState"] >= 2, f"首屏视频没有加载到可播放: {state}"


def test_home_like_persists_after_reload(logged_in: Page):
    """点赞 → 刷新 → 同一视频仍是已赞（服务端 liked 生效）。"""
    _feed_ready(logged_in)
    page = logged_in
    btn = page.locator(".feed .slide .like-btn").first
    btn.wait_for(timeout=10000)
    target_id = page.evaluate(
        """() => {
            const s = document.querySelector('.feed .slide');
            return s ? String(s.dataset.id || '') : '';
        }"""
    )
    # 幂等：先归一化成「未赞」，断言才是 点赞→刷新→已赞。
    if "liked" in (btn.get_attribute("class") or ""):
        btn.click()
        page.wait_for_timeout(600)
        assert "liked" not in (btn.get_attribute("class") or ""), "取消点赞失败"
    btn.click()
    page.wait_for_timeout(700)
    assert "liked" in (btn.get_attribute("class") or ""), "点击后按钮没进入已赞态"

    page.reload(wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(1200)
    state = page.evaluate(
        """(vid) => {
            const slides = Array.prototype.slice.call(
                document.querySelectorAll('.feed .slide'));
            const isLiked = (s) => {
                const b = s.querySelector('.like-btn');
                return !!(b && /(^|\\s)(liked|active)(\\s|$)/.test(b.className));
            };
            const target = slides.find(s => String(s.dataset.id) === String(vid));
            return {
                total: slides.length,
                likedCount: slides.filter(isLiked).length,
                targetFound: !!target,
                targetLiked: target ? isLiked(target) : null,
            };
        }""",
        target_id,
    )
    assert state["likedCount"] >= 1, f"刷新后没有任何已赞卡片（服务端 liked 丢了?) {state}"
    if target_id and state["targetFound"]:
        assert state["targetLiked"], f"刷新后该视频点赞态丢失: {state}"


def test_home_scroll_stays_put(logged_in: Page):
    """无操作 6 秒，feed 不应自己滚动。"""
    _feed_ready(logged_in)
    samples = []
    for _ in range(6):
        samples.append(logged_in.evaluate(
            "() => { const f = document.querySelector('.feed'); return f ? f.scrollTop : -1; }"))
        time.sleep(1)
    assert all(abs(s - samples[0]) <= 10 for s in samples), f"页面自动滚动: {samples}"


def test_home_mobile_viewport_no_auto_scroll(page: Page):
    """移动端视口（375x667）下打开首页，3 秒内不应自动跳。"""
    page.set_viewport_size({"width": 375, "height": 667})
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    samples = [page.evaluate("() => document.querySelector('.feed').scrollTop")]
    for _ in range(5):
        time.sleep(0.6)
        samples.append(page.evaluate("() => document.querySelector('.feed').scrollTop"))
    assert all(abs(s - samples[0]) <= 10 for s in samples), f"移动端自动滚动: {samples}"


def test_home_small_wheel_does_not_snap(page: Page):
    """小幅滚轮（60px）不应被强制吸附到下一整屏。"""
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(1000)
    before = page.evaluate("() => document.querySelector('.feed').scrollTop")
    height = page.evaluate("() => document.querySelector('.feed').clientHeight")
    page.mouse.move(640, 400)
    page.mouse.wheel(0, 60)
    page.wait_for_timeout(1000)
    after = page.evaluate("() => document.querySelector('.feed').scrollTop")
    assert abs(after - before) < height * 0.5, (
        f"小幅滚轮跳了 {after - before}px（屏高 {height}）")


def test_home_search_discovery(page: Page):
    """搜索框聚焦出现「历史/热搜」下拉。"""
    _feed_ready(page)
    box = page.locator("#navSearch")
    box.click()
    page.wait_for_timeout(800)
    panel = page.locator("#searchDiscover")
    assert panel.count() == 1, "没有搜索发现面板"
    assert panel.is_visible(), "聚焦后搜索面板不可见"
    text = panel.inner_text()
    assert "热搜" in text or "历史" in text, f"面板内容异常: {text[:120]}"


def test_home_following_tab_shows_state(logged_in: Page):
    """切到「关注」tab：无关注时给出空态提示，且 feed 不崩。"""
    _feed_ready(logged_in)
    page = logged_in
    page.click("#tabFollowing")
    page.wait_for_timeout(1200)
    body = page.inner_text("body")
    assert ("关注的人还没有作品" in body or "还没有作品" in body), (
        f"关注 tab 没有空态提示: {body[:200]}")
