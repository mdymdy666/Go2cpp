"""直播页 `/live`：打开 / 房间列表或空态 / 聊天发送并持久化 / 礼物面板。"""
from playwright.sync_api import Page, expect

from conftest import BASE, api, login

CHAT_TEXT = "E2E 冒烟：主播晚上好"


def _ensure_room(page: Page) -> int:
    """幂等保证 user0002 有一个直播间并处于开播状态。"""
    rooms = api(page, "/api/live/rooms")
    assert rooms["status"] == 200, f"房间列表接口异常: {rooms}"
    mine = [item["room"] for item in rooms["data"].get("items", [])
            if item["room"].get("user_id") == 2]
    if mine:
        room = mine[0]
        room_id = int(room["id"])
        if int(room.get("status", 0)) != 1:
            api(page, f"/api/live/room/{room_id}/start", "POST", {})
        return room_id
    created = api(page, "/api/live/room", "POST", {"title": "E2E 直播间"})
    assert created["status"] == 200, f"创建直播间失败: {created}"
    room_id = int(created["data"]["room"]["id"])
    api(page, f"/api/live/room/{room_id}/start", "POST", {})
    return room_id


def test_live_page_opens(page: Page):
    """/live 能打开：舞台、聊天输入框、发送按钮都在。"""
    page.goto(BASE + "/live", wait_until="networkidle")
    page.wait_for_selector("#stage", timeout=15000)
    assert page.locator("#chatInput").count() == 1
    assert page.locator("#sendBtn").count() == 1
    body = page.inner_text("body")
    assert "直播间" in body or "LIVE" in body, f"页面内容异常: {body[:150]}"


def test_live_room_list_or_empty(logged_in: Page):
    """房间列表要么有房间项，要么给空态文案（不能空白）。"""
    page = logged_in
    page.goto(BASE + "/live", wait_until="networkidle")
    page.wait_for_selector("#roomList", timeout=15000)
    page.wait_for_timeout(1200)
    text = page.inner_text("#roomList")
    items = page.evaluate(
        "() => document.querySelectorAll('#roomList .room-item, #roomList button').length")
    assert items >= 1 or "还没有直播间" in text, f"房间列表空白: {text!r}"


def test_live_chat_send_and_persist(logged_in: Page):
    """进房发聊天 → 聊天区出现 → 刷新后仍在（先查再发，幂等）。"""
    page = logged_in
    room_id = _ensure_room(page)

    msgs = api(page, f"/api/live/{room_id}/messages?after=0&limit=100")
    assert msgs["status"] == 200, f"消息接口异常: {msgs}"
    existed = any(m.get("content") == CHAT_TEXT
                  for m in msgs["data"].get("items", []))
    if not existed:
        api(page, f"/api/live/{room_id}/messages", "POST",
            {"content": CHAT_TEXT})

    page.goto(f"{BASE}/live?room={room_id}", wait_until="networkidle")
    page.wait_for_selector("#chatList", timeout=15000)
    page.wait_for_timeout(1500)
    expect(page.locator("#chatList")).to_contain_text(CHAT_TEXT, timeout=10000)
    assert page.locator("#chatEmpty").is_hidden(), "有消息时空态还显示"

    page.reload(wait_until="networkidle")
    page.wait_for_selector("#chatList", timeout=15000)
    page.wait_for_timeout(1500)
    expect(page.locator("#chatList")).to_contain_text(CHAT_TEXT, timeout=10000)


def test_live_gift_panel_shows_gifts(logged_in: Page):
    """礼物面板打开后列出礼物（不是「礼物加载失败」）。"""
    page = logged_in
    room_id = _ensure_room(page)
    page.goto(f"{BASE}/live?room={room_id}", wait_until="networkidle")
    page.wait_for_timeout(1000)
    page.click("#giftBtn")
    page.wait_for_timeout(1000)
    panel = page.locator("#giftPanel")
    assert panel.get_attribute("aria-hidden") == "false", "礼物面板没打开"
    text = page.inner_text("#giftPanel")
    assert "礼物加载失败" not in text, f"礼物加载失败: {text[:120]}"
    assert page.evaluate("() => document.querySelectorAll('#giftGrid > *').length") >= 1
