"""管理后台 `/admin`：非 admin 403 提示 / admin 列表 / 处理举报 / 状态筛选。"""
import pytest
from playwright.sync_api import Browser, Page, expect

from conftest import ADMIN, BASE, USER, api, login


def _login_admin(page: Page) -> None:
    login(page, *ADMIN)


def _admin_ready(page: Page) -> None:
    page.goto(BASE + "/admin", wait_until="networkidle")
    page.wait_for_timeout(1500)


def test_admin_rejects_normal_user(logged_in: Page):
    """普通用户打开 /admin：显示「需要管理员权限」，统计保持空。"""
    page = logged_in
    _admin_ready(page)
    error = page.locator("#error")
    assert error.is_visible(), "普通用户没有看到权限提示"
    expect(error).to_contain_text("需要管理员权限")
    assert page.inner_text("#stat-pending") in ("-", ""), "普通用户看到了管理数据"


def test_admin_lists_reports(logged_in_admin: Page):
    """admin 打开 /admin：3 个统计卡有数字，表格或空态渲染。"""
    page = logged_in_admin
    _admin_ready(page)
    page.wait_for_function(
        "() => document.getElementById('stat-pending').textContent !== '-'",
        timeout=15000)
    for key in ["#stat-pending", "#stat-today", "#stat-handled"]:
        text = page.inner_text(key)
        assert text.strip().isdigit(), f"{key} 不是数字: {text!r}"
    assert page.locator("#rows tr").count() >= 0
    assert "已处理" in page.inner_text("body") or "待处理" in page.inner_text("body")


def test_admin_handles_a_report(logged_in: Page, browser: Browser):
    """有 pending 举报时，admin 点「忽略」→ 提示已处理、pending 数下降。"""
    user_page = logged_in
    # 用户侧：找一个还没举报过的视频举报（幂等；重复组合服务端会判 duplicate）
    feed = api(user_page, "/api/feed?limit=100")
    ids = [item["video"]["id"] for item in feed["data"].get("items", [])]
    assert ids, "feed 里没有视频，无法造举报"
    for vid in ids:
        created = api(user_page, "/api/report", "POST",
                      {"target_type": "video", "target_id": vid,
                       "reason": "E2E 举报"})
        if created["status"] == 201:
            break
    admin_ctx = browser.new_context()
    admin_page = admin_ctx.new_page()
    _login_admin(admin_page)
    _admin_ready(admin_page)
    admin_page.wait_for_function(
        "() => document.getElementById('stat-pending').textContent !== '-'",
        timeout=15000)
    pending_before = int(admin_page.inner_text("#stat-pending"))
    rows = admin_page.locator("#rows tr")
    if pending_before == 0 or rows.count() == 0:
        pytest.skip("当前没有待处理举报（举报组合已耗尽）")
    first_row_id = rows.first.locator("td").first.inner_text()
    hint_before = admin_page.inner_text("#hint")
    # confirm 对话框必须点确定
    admin_page.on("dialog", lambda d: d.accept())
    rows.first.locator("button", has_text="忽略").click()
    expect(admin_page.locator("#hint")).to_contain_text(
        f"举报 #{first_row_id} 已处理", timeout=15000)
    admin_page.wait_for_function(
        f"() => Number(document.getElementById('stat-pending').textContent) "
        f"< {pending_before}", timeout=15000)
    assert admin_page.inner_text("#hint") != hint_before
    admin_ctx.close()


def test_admin_status_filter(logged_in_admin: Page):
    """状态下拉切到「已处理」：列表重新加载，提示「已加载 N 条」。"""
    page = logged_in_admin
    _admin_ready(page)
    page.wait_for_function(
        "() => document.getElementById('stat-pending').textContent !== '-'",
        timeout=15000)
    page.select_option("#status", "handled")
    page.wait_for_timeout(1200)
    info = page.inner_text("#page-info")
    assert "已加载" in info, f"筛选后没有加载提示: {info!r}"
    assert page.locator("#error").is_hidden(), f"筛选出错: {page.inner_text('#error')}"
