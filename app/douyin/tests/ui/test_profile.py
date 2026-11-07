"""通知面板 + 个人主页：6 tab / 全部已读 / 三 tab / 关注粉丝抽屉。"""
from playwright.sync_api import Page, expect

from conftest import BASE, api, login


def _open_notif(page: Page) -> None:
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(800)
    page.click("#notifBtn")
    page.wait_for_timeout(1000)


def test_notification_panel_opens_with_six_tabs(logged_in: Page):
    """🔔 打开通知面板：6 个分类 tab（全部/赞/评论/关注/@我/系统）。"""
    page = logged_in
    _open_notif(page)
    panel = page.locator("#notifPanel")
    assert "open" in (panel.get_attribute("class") or ""), "通知面板没打开"
    tabs = page.evaluate(
        "() => Array.from(document.querySelectorAll('#notifTabs .notif-tab'))"
        ".map(t => t.textContent.trim())")
    assert len(tabs) == 6, f"通知 tab 不是 6 个: {tabs}"
    for label in ["全部", "赞", "评论", "关注", "@我", "系统"]:
        assert any(label in t for t in tabs), f"缺少 tab: {label} ({tabs})"
    page.click("#notifClose")
    page.wait_for_timeout(300)


def test_notification_list_or_empty(logged_in: Page):
    """通知列表：有内容就渲染条目，没有就显示空态文案。"""
    page = logged_in
    _open_notif(page)
    text = page.inner_text("#notifList")
    items = page.evaluate("() => document.querySelectorAll('#notifList .notif-item').length")
    assert items >= 1 or "暂无" in text or "没有" in text, f"列表空白: {text!r}"


def test_notification_mark_all_read_persists(logged_in: Page):
    """点「全部已读」→ 未读变 0，刷新后仍是全部已读。"""
    page = logged_in
    _open_notif(page)
    page.click("#notifAllRead")
    page.wait_for_timeout(1200)
    expect(page.locator("#notifSub")).to_have_text("全部已读", timeout=8000)
    unread = api(page, "/api/notifications/unread_count")["data"]["unread_count"]
    assert unread == 0, f"服务端仍有未读: {unread}"

    page.reload(wait_until="networkidle")
    page.wait_for_timeout(1000)
    page.click("#notifBtn")
    page.wait_for_timeout(800)
    expect(page.locator("#notifSub")).to_have_text("全部已读", timeout=8000)


def _open_profile(page: Page) -> None:
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector(".feed .slide", timeout=15000)
    page.wait_for_timeout(600)
    page.click("#who")
    page.wait_for_selector("#profileView .p-tabs", timeout=15000)
    page.wait_for_timeout(800)


def test_profile_three_tabs(logged_in: Page):
    """个人主页有「作品/喜欢/收藏」三个 tab，切换后各自渲染。"""
    page = logged_in
    _open_profile(page)
    tabs = page.evaluate(
        "() => Array.from(document.querySelectorAll('#profileBody .p-tab'))"
        ".map(t => t.textContent.trim())")
    assert len(tabs) == 3, f"主页 tab 不是 3 个: {tabs}"
    for label in ["作品", "喜欢", "收藏"]:
        assert any(label in t for t in tabs), f"缺少 tab {label}: {tabs}"

    page.locator('#profileBody .p-tab[data-tab="liked"]').click()
    page.wait_for_timeout(800)
    panel = page.inner_text("#profileBody .p-panel")
    assert panel.strip(), "切到「喜欢」后面板是空白"
    page.locator('#profileBody .p-tab[data-tab="favorites"]').click()
    page.wait_for_timeout(800)
    assert page.inner_text("#profileBody .p-panel").strip(), "切到「收藏」后面板是空白"


def test_profile_follow_and_fans_drawer(logged_in: Page):
    """点主页「关注」「粉丝」统计：抽屉打开、标题正确、关闭按钮可点（层级 bug 回归）。"""
    page = logged_in
    _open_profile(page)
    for kind, title in [("following", "关注"), ("followers", "粉丝")]:
        page.locator(f'#profileBody .p-stat-link[data-kind="{kind}"]').click()
        page.wait_for_timeout(1000)
        drawer = page.locator("#followPanel")
        assert "open" in (drawer.get_attribute("class") or ""), f"{title}抽屉没打开"
        expect(page.locator("#followPanelTitle")).to_have_text(title)
        text = page.inner_text("#followPanelList")
        assert text.strip(), f"{title}抽屉没有内容/空态"
        page.click("#followPanelClose")
        page.wait_for_timeout(500)
        assert "open" not in (drawer.get_attribute("class") or ""), (
            f"{title}抽屉关不掉（关闭按钮被主页遮挡？）")


def test_profile_tabs_empty_state(logged_in: Page):
    """「喜欢」「收藏」为空时给出用户可读的空态，而不是白屏。"""
    page = logged_in
    _open_profile(page)
    page.locator('#profileBody .p-tab[data-tab="favorites"]').click()
    page.wait_for_timeout(1000)
    panel = page.inner_text("#profileBody .p-panel")
    favs = api(page, "/api/user/2/favorites?limit=50")["data"]
    if int(favs.get("total", 0)) == 0:
        assert ("还没有收藏" in panel or "暂无" in panel or "收藏" in panel), (
            f"收藏空态文案缺失: {panel[:150]}")
    else:
        assert panel.strip(), "有收藏却渲染为空"
