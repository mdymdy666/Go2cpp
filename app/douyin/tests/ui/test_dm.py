"""私信页 `/messages`：打开 / 会话列表或空态 / 发送消息持久化。"""
from playwright.sync_api import Page, expect

from conftest import BASE, api, login

DM_TEXT = "E2E 私信冒烟：你好"


def _ensure_conversation(page: Page) -> tuple[int, int]:
    """幂等保证 user0002 至少有一个会话（没有就发一条给 user0001）。

    返回 (conversation_id, peer_id)。
    """
    convs = api(page, "/api/dm/conversations?limit=50")
    assert convs["status"] == 200, f"会话接口异常: {convs}"
    items = convs["data"].get("items", [])
    if items:
        return int(items[0]["id"]), int(items[0]["peer_id"])
    sent = api(page, "/api/dm/send", "POST",
               {"to_user_id": 1, "content": DM_TEXT})
    assert sent["status"] == 200, f"建立会话失败: {sent}"
    return int(sent["data"]["conversation_id"]), 1


def test_messages_page_opens(page: Page):
    """/messages 能打开：左侧会话区 + 聊天区 + 输入框结构都在。"""
    page.goto(BASE + "/messages", wait_until="networkidle")
    page.wait_for_selector("#convList", timeout=15000)
    assert page.locator("#input").count() == 1
    assert page.locator("#sendBtn").count() == 1


def test_messages_conversation_list_or_empty(logged_in: Page):
    """会话列表要么有会话行，要么给空态文案。"""
    page = logged_in
    _ensure_conversation(page)
    page.goto(BASE + "/messages", wait_until="networkidle")
    page.wait_for_selector("#convList", timeout=15000)
    page.wait_for_timeout(1200)
    text = page.inner_text("#convList")
    rows = page.evaluate("() => document.querySelectorAll('#convList .conv').length")
    assert rows >= 1 or "还没有私信" in text, f"会话列表空白: {text!r}"


def test_messages_send_and_persist(logged_in: Page):
    """点开会话发消息 → 气泡可见 → 刷新后重新打开仍在（先查再发）。"""
    page = logged_in
    conv_id, peer_id = _ensure_conversation(page)
    history = api(page, f"/api/dm/{conv_id}/messages?after=0&limit=100")
    existed = any(m.get("content") == DM_TEXT
                  for m in history["data"].get("items", []))
    if not existed:
        api(page, "/api/dm/send", "POST",
            {"to_user_id": peer_id, "content": DM_TEXT})

    page.goto(f"{BASE}/messages?conversation={conv_id}", wait_until="networkidle")
    page.wait_for_selector("#convList", timeout=15000)
    page.wait_for_timeout(1200)
    assert page.locator("#composer").is_visible(), "点开会话后输入区没出现"
    expect(page.locator("#chatBody")).to_contain_text(DM_TEXT, timeout=15000)

    page.reload(wait_until="networkidle")
    page.wait_for_selector("#convList", timeout=15000)
    page.wait_for_timeout(1200)
    page.locator("#convList .conv").first.click()
    page.wait_for_timeout(1200)
    expect(page.locator("#chatBody")).to_contain_text(DM_TEXT, timeout=15000)


def test_messages_filter_empty_state(logged_in: Page):
    """搜索框输入不存在的词：显示「没有匹配的会话」。"""
    page = logged_in
    _ensure_conversation(page)
    page.goto(BASE + "/messages", wait_until="networkidle")
    page.wait_for_selector("#convFilter", timeout=15000)
    page.wait_for_timeout(800)
    page.fill("#convFilter", "zzz-no-such-conversation")
    page.wait_for_timeout(500)
    assert "没有匹配的会话" in page.inner_text("#convList")
