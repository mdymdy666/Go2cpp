"""复现「页面自动向下翻」——观察 scrollTop 随时间变化。"""
import time
import pytest
from playwright.sync_api import Page

BASE = "http://127.0.0.1:28080"


def test_scroll_stays_put(page: Page):
    """打开首页后 8 秒内，scrollTop 不应自己变化（除非用户操作/视频播完连播）。"""
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    page.wait_for_timeout(1000)
    samples = []
    for _ in range(8):
        st = page.evaluate("() => { const f = document.querySelector('.feed'); return f ? f.scrollTop : -1; }")
        samples.append(st)
        time.sleep(1)
    print("scrollTop samples:", samples)
    # 前后差 > 10px 视为自动滚动
    if len(samples) >= 2 and samples[0] >= 0:
        delta = abs(samples[-1] - samples[0])
        assert delta <= 10, f"页面在无操作下自动滚动了 {delta}px: {samples}"
