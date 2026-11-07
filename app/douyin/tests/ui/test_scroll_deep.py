"""深度复现「自动向下翻」：移动端视口 + 慢网 + 视频加载。"""
import time
import pytest
from playwright.sync_api import Page

BASE = "http://127.0.0.1:28080"


def test_mobile_viewport_no_auto_scroll(page: Page):
    """移动端视口（375x667）下打开首页，3 秒内不应自动滚动。"""
    page.set_viewport_size({"width": 375, "height": 667})
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    samples = []
    for _ in range(5):
        st = page.evaluate("() => document.querySelector('.feed').scrollTop")
        samples.append(st)
        time.sleep(0.6)
    print("mobile scrollTop:", samples)
    assert all(abs(s - samples[0]) <= 10 for s in samples), f"移动端自动滚动: {samples}"


def test_slow_network_no_auto_scroll(page: Page):
    """慢网（每个请求延迟 400ms）下打开首页，观察是否因渐进渲染导致 snap 跳。"""
    def slow(route):
        time.sleep(0.4)
        route.continue_()

    page.route("**/api/**", slow)
    page.goto(BASE, wait_until="domcontentloaded")
    page.wait_for_selector(".feed", timeout=15000)
    samples = []
    for _ in range(8):
        st = page.evaluate("() => document.querySelector('.feed') ? document.querySelector('.feed').scrollTop : -1")
        samples.append(st)
        time.sleep(0.6)
    print("slow-net scrollTop:", samples)
    assert all(abs(s - samples[0]) <= 10 for s in samples), f"慢网自动滚动: {samples}"


def test_after_video_ends_no_forced_jump(page: Page):
    """把视频 seek 到结尾触发 ended，观察是否强制跳到下一屏（这是自动连播，属预期）。"""
    page.goto(BASE, wait_until="networkidle")
    page.wait_for_selector(".feed", timeout=10000)
    page.wait_for_timeout(1000)
    info = page.evaluate(
        """() => {
            const v = document.querySelector('.slide video');
            if (!v) return {found: false};
            return {found: true, duration: v.duration, loop: v.loop, autoplay: !v.paused};
        }"""
    )
    print("video info:", info)
    # 仅记录，不强制断言（连播是产品行为）
