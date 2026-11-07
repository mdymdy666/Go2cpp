"""创作中心 `/creator`：未登录提示 / 4 卡片数字 / 趋势图 / TOP 作品。"""
from playwright.sync_api import Page, expect

from conftest import BASE, api, login


def _open(page: Page) -> None:
    page.goto(BASE + "/creator", wait_until="networkidle")
    page.wait_for_timeout(1500)


def test_creator_requires_login(page: Page):
    """未登录：显示「登录后查看你的创作者数据」，不显示假数据。"""
    _open(page)
    gate = page.locator("#gate")
    assert gate.is_visible(), "未登录没有显示登录提示"
    assert "登录" in gate.inner_text(), f"提示文案异常: {gate.inner_text()[:80]}"


def test_creator_four_cards_show_numbers(logged_in: Page):
    """登录后 4 个卡片显示数字，且与 /api/creator/overview 一致。"""
    page = logged_in
    _open(page)
    expect(page.locator("#statPlays")).not_to_have_text("—", timeout=10000)
    stats = {
        "plays": page.inner_text("#statPlays"),
        "likes": page.inner_text("#statLikes"),
        "comments": page.inner_text("#statComments"),
        "followers": page.inner_text("#statFollowers"),
    }
    overview = api(page, "/api/creator/overview")["data"]["overview"]

    def num(text):
        return int(text.replace(",", "").replace("万", "0000") or 0)

    assert num(stats["plays"]) == overview["total_plays"], f"播放卡片不一致: {stats} vs {overview}"
    assert num(stats["likes"]) == overview["total_likes"], f"点赞卡片不一致: {stats}"
    assert num(stats["comments"]) == overview["total_comments"], f"评论卡片不一致: {stats}"
    assert num(stats["followers"]) == overview["followers"], f"粉丝卡片不一致: {stats}"
    assert str(overview["video_count"]) in page.inner_text("#videoCount"), (
        f"作品数文案不一致: {page.inner_text('#videoCount')}")


def test_creator_trend_chart_has_data(logged_in: Page):
    """趋势图：7 天柱子渲染；有互动时显示峰值、隐藏空态。"""
    page = logged_in
    _open(page)
    bars = page.evaluate("() => document.querySelectorAll('#trendChart .bar').length")
    days = page.evaluate("() => document.querySelectorAll('#trendChart .day').length")
    assert days == 7, f"趋势图不是 7 天: {days}"
    assert bars == 21, f"趋势图柱子数量异常（7 天 × 3 指标）: {bars}"
    trend = api(page, "/api/creator/trend?days=7")["data"]
    total = sum(p["likes"] + p["comments"] + p["followers"] for p in trend["points"])
    peak = page.inner_text("#trendPeak")
    empty_visible = page.locator("#trendEmpty").is_visible()
    if total > 0:
        assert not empty_visible, "有互动却显示空态"
        assert "单日峰值" in peak, f"缺少峰值文案: {peak!r}"
    else:
        assert empty_visible, "没有互动却隐藏了空态"


def test_creator_top_videos_or_empty(logged_in: Page):
    """TOP5 作品：有作品时列出标题，无作品显示空态。"""
    page = logged_in
    _open(page)
    items = page.evaluate("() => document.querySelectorAll('#topVideos .top-item').length")
    empty_visible = page.locator("#topEmpty").is_visible()
    payload = api(page, "/api/creator/top-videos?limit=5")["data"]
    count = int(payload.get("count", 0))
    assert items == count, f"TOP 列表数量 {items} 与接口 {count} 不一致"
    if count == 0:
        assert empty_visible, "没有作品却隐藏空态"
    else:
        assert not empty_visible, "有作品却显示空态"
        expect(page.locator("#topVideos .top-item").first).to_be_visible()
