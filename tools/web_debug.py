#!/usr/bin/env python3
"""Debug the web text-cell pipeline: dump cell values + fillText call counts."""
import json
import pathlib
import sys
import time

from playwright.sync_api import sync_playwright

URL = (pathlib.Path(__file__).resolve().parent.parent / "web" / "index.html").as_uri()
errors = []

with sync_playwright() as pw:
    browser = pw.chromium.launch()
    page = browser.new_page(viewport={"width": 1280, "height": 720})
    page.on("console", lambda m: errors.append(f"{m.type}: {m.text}"))
    page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
    page.goto(URL)
    page.wait_for_function(
        "typeof Module !== 'undefined' && typeof Module._web_key === 'function'",
        timeout=20000,
    )
    time.sleep(1.0)

    # instrument fillText + fillRect on the display context
    page.evaluate(
        """() => {
        const c = document.getElementById('screen').getContext('2d');
        window.__ft = 0; window.__fr = 0;
        const ft = c.fillText.bind(c), fr = c.fillRect.bind(c);
        c.fillText = function () { window.__ft++; return ft.apply(null, arguments); };
        c.fillRect = function () { window.__fr++; return fr.apply(null, arguments); };
    }"""
    )
    time.sleep(0.8)

    dump1 = page.evaluate(
        """() => {
        const n = Module._web_text_count(), tp = Module._web_text_ptr();
        const U = Module.HEAPU32, base = tp >>> 2;
        const out = [];
        for (let i = 0; i < Math.min(n, 25); i++) {
            const o = base + i * 5;
            out.push({x: U[o], y: U[o+1], ch: U[o+2],
                      fg: U[o+3].toString(16), bg: U[o+4].toString(16)});
        }
        return {n, ft: window.__ft, fr: window.__fr,
                cols: Module._web_cols(), rows: Module._web_rows(), out};
    }"""
    )

    # open the help overlay, then sample cells again
    page.keyboard.press("h")
    time.sleep(0.6)
    dump2 = page.evaluate(
        """() => {
        const n = Module._web_text_count(), tp = Module._web_text_ptr();
        const U = Module.HEAPU32, base = tp >>> 2;
        const out = [];
        let glyphs = 0, sameFgBg = 0;
        for (let i = 0; i < n; i++) {
            const o = base + i * 5;
            const ch = U[o+2], fg = U[o+3], bg = U[o+4];
            if (ch > 32) glyphs++;
            if (fg === bg) sameFgBg++;
            if (out.length < 40 && ch > 32)
                out.push({x: U[o], y: U[o+1], ch, c: String.fromCharCode(ch),
                          fg: fg.toString(16), bg: bg.toString(16)});
        }
        return {n, glyphs, sameFgBg, ft: window.__ft, fr: window.__fr, out};
    }"""
    )
    page.screenshot(path="/tmp/web_debug_help.png")
    browser.close()

print("BEFORE HELP:", json.dumps(dump1, indent=1))
print("AFTER HELP :", json.dumps(dump2, indent=1))
print("console:", errors if errors else "none")
