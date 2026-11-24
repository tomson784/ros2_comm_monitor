"""Capture Web UI screenshots with Playwright (used by demo_screenshots.sh).

    python3 capture_ui.py URL OUT_DIR PHASE      # PHASE: online | offline
"""
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

url, out, phase = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
out.mkdir(parents=True, exist_ok=True)


def shot(page, name, selector=None, full_page=False):
    path = out / name
    if selector:
        page.locator(selector).first.screenshot(path=str(path))
    else:
        page.screenshot(path=str(path), full_page=full_page)
    print("  saved", path)


def fit_graph(page, clicks=3):
    # Zoom out so the whole multi-host graph fits in the canvas.
    for _ in range(clicks):
        page.click("#graphZoomOutBtn")
    time.sleep(0.5)


def click_node(page, node_id):
    page.locator(f'[data-node-id="{node_id}"]').first.click()
    page.mouse.move(5, 5)  # hide the hover tooltip before the screenshot
    time.sleep(1)


def open_tab(page, tab):
    page.click(f'button.tab[data-tab="{tab}"]')
    time.sleep(1.5)


with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page(viewport={"width": 1600, "height": 1000}, device_scale_factor=1)
    page.goto(url)
    page.wait_for_selector("#topicsBody tr td .badge", timeout=30000)

    if phase == "online":
        # Let the 1 s polling build up chart history.
        time.sleep(25)
        shot(page, "ui_topics.png", full_page=True)
        shot(page, "ui_host_bar.png", selector="#hostBar")

        open_tab(page, "charts")
        shot(page, "ui_charts.png", selector="#charts")

        open_tab(page, "graph")
        page.wait_for_selector(".graph-svg", timeout=30000)
        time.sleep(2)
        fit_graph(page)
        shot(page, "ui_graph.png", selector=".graph-card")

        click_node(page, "/big")
        shot(page, "ui_graph_topic_detail.png", selector=".graph-card")

        click_node(page, "/shared_status")
        shot(page, "ui_graph_mixed_detail.png", selector=".graph-detail")
    else:
        time.sleep(4)
        shot(page, "ui_topics_offline.png", full_page=True)
        open_tab(page, "graph")
        page.wait_for_selector(".graph-svg", timeout=30000)
        time.sleep(2)
        fit_graph(page)
        shot(page, "ui_graph_offline.png", selector=".graph-card")
    browser.close()
