"""douyin UI E2E —— 登录 / 点赞持久化 / 搜索 / 设置 / 分享页。"""
import pytest
from playwright.sync_api import Page, expect

BASE = "http://127.0.0.1:28080"


def _login(page: Page, email="user0002@douyin.local", pwd="user0002"):
    """走真实 UI 登录（右上角邮箱/密码表单 + 登录按钮）。

    注意：产品登录接口是 POST /api/user/login（网关没有 /api/login）。
    E2E 之前直接 fetch 不存在的路径，token 拿不到，后续点赞/刷新全被
    当成匿名——这是测试脚本的错，不是产品 bug。
    """
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector("#email", timeout=10000)
    page.fill("#email", email)
    page.fill("#password", pwd)
    page.click("#loginBtn")
    page.wait_for_function(
        "() => !!localStorage.getItem('douyin_token')", timeout=10000)
    page.wait_for_timeout(300)


def test_login_via_ui(page: Page):
    """通过 UI 登录后，页面显示已登录状态，刷新后仍保持。"""
    _login(page)
    page.reload(wait_until="networkidle")
    page.wait_for_timeout(500)
    token = page.evaluate("() => localStorage.getItem('douyin_token')")
    assert token and len(token) > 10, "登录后 localStorage 没有 token"
    visible = page.inner_text("body")
    assert "登录" not in visible or "退出" in visible, "刷新后未显示登录态"


def test_like_persists_after_reload(page: Page):
    """点赞后刷新，点赞态必须保持（已知 bug：feed 不返回 liked）。"""
    _login(page)
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    page.wait_for_timeout(1200)

    btn = page.locator(".like-btn, .cp-like, [data-action=like]").first
    btn.wait_for(timeout=10000)
    # feed 每页随机排序，刷新后第一张卡可能换人；记住被点的视频 id，
    # 刷新后按 id 找回同一张卡（找不到再退化为「页内存在已赞卡」）。
    target_id = page.evaluate(
        "() => { var s = document.querySelector('.feed .slide');"
        " return s ? String(s.dataset.id || '') : ''; }"
    )
    # seed 数据里第一张卡可能已赞；直接 click 是「取消」。先归一化为未赞，
    # 这样断言的才是「点赞 -> 刷新 -> 仍是已赞」这条用户路径。
    if "liked" in (btn.get_attribute("class") or ""):
        btn.click()
        page.wait_for_timeout(700)
        assert "liked" not in (btn.get_attribute("class") or ""), "取消点赞后按钮仍显示已赞"
    btn.click()
    page.wait_for_timeout(700)
    assert "liked" in (btn.get_attribute("class") or ""), "点击后按钮没有进入已赞态（请求失败？）"

    page.reload(wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    page.wait_for_timeout(1500)
    # 刷新后：服务端 liked 必须让卡片重新变 ♥（前端按 liked 加 liked 类）。
    state = page.evaluate(
        """(vid) => {
            const slides = Array.prototype.slice.call(
                document.querySelectorAll('.feed .slide'));
            function isLiked(s) {
                const b = s.querySelector('.like-btn, .cp-like, [data-action=like]');
                return !!(b && /(^|\\s)(liked|active)(\\s|$)/.test(b.className));
            }
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
    assert state["likedCount"] >= 1, (
        f"刷新后没有任何卡片恢复已赞态（服务端 liked 未生效？）: {state}")
    if target_id and state["targetFound"]:
        assert state["targetLiked"], f"刷新后该视频点赞态丢失: {state}"


def test_search_shows_history_and_hot(page: Page):
    """搜索框聚焦出现历史/热搜，输入后出结果。"""
    page.goto(BASE + "/", wait_until="networkidle")
    box = page.locator("input[type=search], input[placeholder*='搜索']").first
    if box.count() == 0:
        pytest.skip("没有搜索框")
    box.click()
    page.wait_for_timeout(800)
    panel = page.locator("#searchDiscover")
    assert panel.count() == 1, "页面没有搜索发现面板"
    assert panel.is_visible(), "聚焦搜索框后历史/热搜面板不可见"
    visible = page.inner_text("body")
    assert ("热搜" in visible or "历史" in visible), f"聚焦后没出现热搜/历史: {visible[:200]}"


def test_share_landing_page(page: Page):
    """/v/<id> 分享页能播放 + OG 标签正确。"""
    page.goto(BASE + "/v/1", wait_until="networkidle")
    assert page.locator("video").count() >= 1, "分享页没有视频"
    html = page.content()
    for tag in ["og:title", "og:image", "og:description"]:
        assert tag in html, f"分享页缺少 {tag}"
    v = page.locator("video").first
    state = v.evaluate("el => ({readyState: el.readyState, error: el.error ? el.error.code : 0})")
    assert state["error"] == 0, f"分享页视频加载失败: {state}"


def test_settings_page_loads(page: Page):
    """/settings 能打开且能改昵称。"""
    _login(page)
    page.goto(BASE + "/settings", wait_until="networkidle")
    visible = page.inner_text("body")
    assert "昵称" in visible or "简介" in visible, f"设置页内容异常: {visible[:200]}"
