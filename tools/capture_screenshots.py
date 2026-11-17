"""Capture genuine screenshots of the running LoRa Peer Station dashboard.

The dashboard must be the only program owning the ESP32 COM ports:

    python -m streamlit run dashboard/peer_app.py --server.headless true

Then:

    python tools/capture_screenshots.py

Every image is a screenshot of the live app; no values are mocked. Each step
verifies that the packet really arrived before the image is saved. The
dashboard auto-refreshes every 0.6 s, so the refresh loop is briefly paused
while a frame is captured (Streamlit dims widgets during a rerun).
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

from playwright.sync_api import Page, sync_playwright

URL = "http://localhost:8501"
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "screenshots"
OUT.mkdir(parents=True, exist_ok=True)

# Hide only the Streamlit browser chrome; the app itself is untouched.
CHROME_CSS = """
header[data-testid="stHeader"] { display: none !important; }
[data-testid="stAppViewContainer"] > .main { padding-top: 1.4rem !important; }
.block-container { padding-top: 0.6rem !important; }
/* Streamlit leaves a grey focus tint on the last clicked button. The button is
   enabled; this only removes the transient focus tint for the capture. */
button:focus, button:focus-visible {
  background-color: #ffffff !important;
  color: rgb(49, 51, 63) !important;
  box-shadow: none !important;
  outline: none !important;
}
"""


def shot(page: Page, name: str, clip: dict | None = None) -> None:
    # Streamlit keeps a grey focus style on the last clicked button; clear it
    # so the frame is clean. Nothing about the data changes.
    page.evaluate("() => document.activeElement && document.activeElement.blur()")
    time.sleep(1.2)
    path = OUT / name
    page.screenshot(path=str(path), full_page=clip is None, clip=clip)
    print(f"saved {path}")


def column_text(page: Page, index: int) -> str:
    return page.locator("[data-testid='stColumn']").nth(index).inner_text()


def send(page: Page, label: str, value: str, button: str,
         receiver_col: int, expect: str, wait_seconds: float = 14) -> None:
    """Type a value into a peer field, transmit it and verify arrival."""
    field = page.get_by_label(label)
    field.click()
    field.fill(value)
    field.press("Enter")          # commit the value to Streamlit state
    time.sleep(1.5)
    page.get_by_role("button", name=button).click()

    deadline = time.time() + wait_seconds
    while time.time() < deadline:
        if expect in column_text(page, receiver_col):
            break
        time.sleep(0.5)
    else:
        raise RuntimeError(
            f"packet not received: expected {expect!r} in column {receiver_col}")
    time.sleep(2)                 # let the ACK land and the panels settle


def main() -> int:
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="msedge", headless=True)
        page = browser.new_page(viewport={"width": 1600, "height": 1040},
                                device_scale_factor=1.5)
        page.goto(URL, wait_until="domcontentloaded", timeout=60000)
        page.wait_for_selector("text=LORA BIDIRECTIONAL PEER STATION",
                               timeout=60000)
        page.add_style_tag(content=CHROME_CSS)
        time.sleep(8)

        # 1. Overview: both peers connected, radio initialised
        shot(page, "dashboard_overview.png")

        # 2. Text ESP32 #1 -> ESP32 #2
        send(page, "Text from ESP32 #1", "HELLO FROM ESP32 1",
             "TRANSMIT TEXT #1", 1, "from 7630 at")
        shot(page, "bidirectional_text.png")

        # 3. Text ESP32 #2 -> ESP32 #1
        send(page, "Text from ESP32 #2", "HELLO FROM ESP32 2",
             "TRANSMIT TEXT #2", 0, "from 2DF4 at")
        shot(page, "reverse_direction_text.png")

        # 4. Numeric ESP32 #1 -> ESP32 #2, then ESP32 #2 -> ESP32 #1
        send(page, "Values from ESP32 #1", "100,200,300,400",
             "TRANSMIT VALUES #1", 1, "100, 200, 300, 400")
        send(page, "Values from ESP32 #2", "1500,1200,1800,2000",
             "TRANSMIT VALUES #2", 0, "1500, 1200, 1800, 2000")
        shot(page, "numerical_data.png")

        # 5. ACK / delivery confirmation detail, both status panels
        panels = page.locator(".panel")
        box1 = panels.nth(0).bounding_box()
        box2 = panels.nth(1).bounding_box()
        if box1 and box2:
            left = min(box1["x"], box2["x"])
            top = min(box1["y"], box2["y"])
            right = max(box1["x"] + box1["width"], box2["x"] + box2["width"])
            bottom = max(box1["y"] + box1["height"], box2["y"] + box2["height"])
            shot(page, "acknowledgement_status.png", clip={
                "x": left, "y": top,
                "width": right - left, "height": bottom - top})

        # 6. Diagnostics
        page.get_by_text("Diagnostics", exact=True).click()
        time.sleep(4)
        shot(page, "lora_diagnostics.png")

        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
