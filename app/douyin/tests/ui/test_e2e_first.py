"""douyin UI E2E —— 首页用户可见性验证（真实浏览器）。"""
import re
import pytest
from playwright.sync_api import Page, expect

BASE = "http://127.0.0.1:28080"


def test_home_renders_feed(page: Page):
    """首页能渲染出 feed，且没有用户可见的错误文案。"""
    page.goto(BASE, wait_until="networkidle")
    expect(page.locator(".feed")).to_be_visible(timeout=10000)
    # 只看**可见文本**（不看 HTML 源码里的模板字符串）
    visible = page.inner_text("body")
    for bad in ["加载失败", "检查 /videos", "undefined", "null"]:
        assert bad not in visible, f"页面出现用户可见错误文案: {bad}"


def test_home_videos_actually_load(page: Page):
    """每条 feed 视频必须真的加载成功（不是 404 占位）。"""
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    videos = page.locator("video")
    n = videos.count()
    assert n >= 1, "首页没有任何 video 元素"

    bad = []
    for i in range(n):
        v = videos.nth(i)
        if not v.is_visible():
            continue
        state = v.evaluate(
            """el => ({
                readyState: el.readyState,
                networkState: el.networkState,
                src: el.currentSrc || el.src,
                error: el.error ? el.error.code : 0,
            })"""
        )
        if state["src"] and (state["error"] != 0 or state["networkState"] == 3):
            bad.append(state)
    assert not bad, f"有视频加载失败: {bad}"


def test_home_console_clean(page: Page):
    """控制台无 error、无 4xx/5xx 请求。"""
    errors = []
    page.on("console", lambda m: errors.append(f"console: {m.text}") if m.type == "error" else None)
    page.on("response", lambda r: errors.append(f"{r.status} {r.url}") if r.status >= 400 else None)
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_timeout(2000)
    assert not errors, f"控制台/网络错误: {errors}"
