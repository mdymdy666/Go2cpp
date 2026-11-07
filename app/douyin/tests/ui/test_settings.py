"""设置页 `/settings`：打开 / 改名 / 重名 409 / 简介超长 / 未登录。"""
from playwright.sync_api import Page, expect

from conftest import BASE, api, login

ORIGINAL_NAME = "user0002"


def _open_settings(page: Page):
    page.goto(BASE + "/settings", wait_until="networkidle")
    page.wait_for_selector("#name", timeout=15000)
    page.wait_for_function(
        "() => document.getElementById('name').value.length > 0", timeout=15000)


def test_settings_loads_with_server_data(logged_in: Page):
    """设置页打开后显示服务端资料（昵称非空、简介/头像字段存在）。"""
    page = logged_in
    _open_settings(page)
    name = page.input_value("#name")
    assert name, "昵称输入框为空（资料没加载出来）"
    assert page.locator("#bio").count() == 1
    assert page.locator("#avatar").count() == 1
    assert "保存" in page.inner_text("#saveBtn")


def test_settings_rename_success_and_restore(logged_in: Page):
    """改名成功 → 跳回首页；再改回原名（保证幂等）。"""
    page = logged_in
    _open_settings(page)
    tmp = "user0002_e2e"
    page.fill("#name", tmp)
    page.click("#saveBtn")
    page.wait_for_function("() => location.pathname === '/'", timeout=8000)
    # 服务端确实改了名
    me = api(page, "/api/user/me")
    assert me["data"]["user"]["name"] == tmp, f"改名没生效: {me['data']}"

    try:
        page.goto(BASE + "/settings", wait_until="networkidle")
        page.wait_for_function(
            "() => document.getElementById('name').value.length > 0", timeout=15000)
        page.fill("#name", ORIGINAL_NAME)
        page.click("#saveBtn")
        page.wait_for_function("() => location.pathname === '/'", timeout=8000)
        me = api(page, "/api/user/me")
        assert me["data"]["user"]["name"] == ORIGINAL_NAME, "恢复原名失败"
    finally:
        # 兜底恢复，避免中途异常留下脏名字
        if api(page, "/api/user/me")["data"]["user"]["name"] != ORIGINAL_NAME:
            api(page, "/api/user/me", "PATCH", {"name": ORIGINAL_NAME, "bio": "",
                                                 "avatar_url": ""})


def test_settings_duplicate_name_shows_409(logged_in: Page):
    """用别人已占用的昵称保存：原地提示「昵称已被占用」，不跳走。"""
    page = logged_in
    _open_settings(page)
    page.fill("#name", "user0001")
    page.click("#saveBtn")
    expect(page.locator("#nameErr")).to_have_text("昵称已被占用", timeout=8000)
    assert page.url.endswith("/settings"), "保存失败却跳走了"
    # 服务端名字没变
    me = api(page, "/api/user/me")
    assert me["data"]["user"]["name"] == ORIGINAL_NAME, "冲突保存却改了名字"


def test_settings_bio_too_long_rejected(logged_in: Page):
    """简介超过 80 字：前端直接拒绝并提示，不提交。"""
    page = logged_in
    _open_settings(page)
    page.fill("#bio", "长" * 90)
    page.click("#saveBtn")
    expect(page.locator("#bioErr")).to_have_text("简介不能超过 80 个字", timeout=5000)
    assert page.url.endswith("/settings"), "校验失败却跳走了"
    assert "简介不能超过 80 个字" in page.inner_text("body")


def test_settings_requires_login(page: Page):
    """未登录访问设置页：提示先登录并跳回首页。"""
    page.goto(BASE + "/settings", wait_until="networkidle")
    page.wait_for_timeout(300)
    body = page.inner_text("body")
    assert "请先在首页登录" in body or "登录" in body, f"未登录没有提示: {body[:150]}"
    page.wait_for_function("() => location.pathname === '/'", timeout=5000)
