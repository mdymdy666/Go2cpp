"""验证：鼠标小幅度滚轮是否被强制吸附到下一整屏。"""
import time
import pytest
from playwright.sync_api import Page

BASE = "http://127.0.0.1:28080"


def test_small_wheel_does_not_jump_full_screen(page: Page):
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    page.wait_for_timeout(1500)
    before = page.evaluate("() => document.querySelector('.feed').scrollTop")
    # 鼠标移到页面中央，滚 60px（小幅度）
    page.mouse.move(640, 400)
    page.mouse.wheel(0, 60)
    page.wait_for_timeout(1200)
    after = page.evaluate("() => document.querySelector('.feed').scrollTop")
    h = page.evaluate("() => document.querySelector('.feed').clientHeight")
    print(f"before={before} after={after} screenHeight={h}")
    # 如果小滚动导致跳动 > 半屏，说明被强制吸附
    assert abs(after - before) < h * 0.5, f"小幅滚轮(60px)导致跳了 {after - before}px（屏高 {h}）"
