"""douyin UI E2E 公共设施：登录、API 辅助、控制台错误收集。

约定：
- 所有断言看**用户可见内容**（文本/可见性/媒体加载状态），不看请求状态码；
  需要跨页面共享状态的用例才用 `api()` 做「先查再操作」，且必须幂等。
- 每个测试用独立的 browser context（pytest-playwright 默认），localStorage
  不共享，所以需要登录的用例都显式调用 `login()`。
"""
import pytest
from playwright.sync_api import Page

BASE = "http://127.0.0.1:28080"
USER = ("user0002@douyin.local", "user0002")
ADMIN = ("admin@douyin.local", "123456")


def login(page: Page, email: str = USER[0], pwd: str = USER[1]) -> None:
    """走真实 UI 登录（首页右上角表单），确保 localStorage 有 token。"""
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_selector("#email", timeout=15000)
    page.fill("#email", email)
    page.fill("#password", pwd)
    page.click("#loginBtn")
    page.wait_for_function(
        "() => !!localStorage.getItem('douyin_token')", timeout=15000)
    page.wait_for_timeout(300)


def api(page: Page, path: str, method: str = "GET", body=None) -> dict:
    """在页面上下文里调接口（同源、自动带当前 token）。

    返回 {status, data, text}。仅用于「先查再操作」与跨页面断言的数据准备，
    不能替代 UI 断言。
    """
    return page.evaluate(
        """async ([p, m, b]) => {
            const t = localStorage.getItem('douyin_token') || '';
            const headers = {'Content-Type': 'application/json'};
            if (t) headers.Authorization = 'Bearer ' + t;
            const r = await fetch(p, {
                method: m, headers,
                body: b === null ? undefined : JSON.stringify(b)
            });
            const text = await r.text();
            let data = null;
            try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
            return {status: r.status, data, text};
        }""",
        [path, method, body],
    )


@pytest.fixture
def console_errors(page: Page):
    """收集控制台 error 与 4xx/5xx 响应（含导航响应）。"""
    bucket = []

    def on_console(msg):
        if msg.type == "error":
            bucket.append(f"console: {msg.text}")

    def on_response(resp):
        if resp.status >= 400:
            bucket.append(f"{resp.status} {resp.url}")

    page.on("console", on_console)
    page.on("response", on_response)
    return bucket


@pytest.fixture
def logged_in(page: Page) -> Page:
    """已登录（user0002）的页面。"""
    login(page)
    return page


@pytest.fixture
def logged_in_admin(page: Page) -> Page:
    """已登录（admin 账号）的页面。"""
    login(page, *ADMIN)
    return page
