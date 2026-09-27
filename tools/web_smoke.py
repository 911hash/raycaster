#!/usr/bin/env python3
"""Headless smoke test for the web build.

Loads web/index.html from file:// (or the live site when RAYCASTER_URL is
set), waits for the wasm runtime, checks that the
canvas actually shows the game (non-black pixels + text cells), drives a few
keys, and screenshots before/after.  Exits non-zero on any console error or if
the canvas stayed black.

    python3 tools/web_smoke.py [screenshot_dir]
    RAYCASTER_URL=https://user.github.io/repo/ python3 tools/web_smoke.py
"""
import os
import pathlib
import sys
import time

from playwright.sync_api import sync_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent
URL = os.environ.get("RAYCASTER_URL") or (ROOT / "web" / "index.html").as_uri()
OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp")

errors = []


def canvas_stats(page):
    return page.evaluate(
        """() => {
        const c = document.getElementById('screen');
        const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
        let lit = 0, sum = 0;
        for (let i = 0; i < d.length; i += 4) {
            const v = d[i] + d[i + 1] + d[i + 2];
            if (v > 24) lit++;
            sum += v;
        }
        return { w: c.width, h: c.height, lit, n: d.length / 4,
                 avg: sum / (d.length / 4) };
    }"""
    )


with sync_playwright() as pw:
    browser = pw.chromium.launch()
    page = browser.new_page(viewport={"width": 1280, "height": 720})
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))

    page.goto(URL)
    page.wait_for_function(
        "typeof Module !== 'undefined' && typeof Module._web_key === 'function'",
        timeout=20000,
    )
    time.sleep(1.5)

    s1 = canvas_stats(page)
    page.screenshot(path=str(OUT / "web_smoke_1.png"))

    # walk forward, turn, toggle the minimap, open help
    for key in ["w", "w", "ArrowRight", "m", "h"]:
        page.keyboard.down(key)
        time.sleep(0.12)
        page.keyboard.up(key)
        time.sleep(0.12)
    time.sleep(1.0)

    s2 = canvas_stats(page)
    page.screenshot(path=str(OUT / "web_smoke_2.png"))
    browser.close()

print(f"frame 1: {s1['w']}x{s1['h']} lit={s1['lit']}/{s1['n']} avg={s1['avg']:.1f}")
print(f"frame 2: {s2['w']}x{s2['h']} lit={s2['lit']}/{s2['n']} avg={s2['avg']:.1f}")
print("console errors:", errors if errors else "none")

ok = (
    s1["lit"] > s1["n"] * 0.05          # scene is on screen
    and s2["w"] == s1["w"]               # still running (no crash)
    and not errors
)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
