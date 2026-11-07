#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Audi Étoffe
# See the LICENSE file in the top directory.
"""End-to-end check: python3 web/e2e.py /path/to/stock/XDJ1KMK2.UPD
Builds the expected firmware with the Python reference tool (tools/apply_spec.py), then drives index.html in headless
Chromium (pip install playwright; playwright install chromium) and checks the page produces the identical file."""
import asyncio, hashlib, os, subprocess, sys, tempfile
from playwright.async_api import async_playwright
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); stock = os.path.abspath(sys.argv[1])
def reference(extra):
    out = os.path.join(tempfile.mkdtemp(), 'ref.UPD')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'apply_spec.py'), stock, out, '--spec', os.path.join(ROOT, 'data', 'patchspec.json')] + extra, stdout=subprocess.DEVNULL)
    return hashlib.sha256(open(out, 'rb').read()).hexdigest()
async def check(page, label, djp, hires, want):
    await page.goto('file://' + os.path.join(ROOT, 'index.html'))
    await page.wait_for_function("document.getElementById('xdjp-status').hidden===true", timeout=30000)
    await page.locator('#xdjp-file').set_input_files(stock)
    await page.wait_for_function("document.getElementById('xdjp-fileNote').textContent.startsWith('Recognised')", timeout=60000)
    for sel, want_on in (('#xdjp-optDjp', djp), ('#xdjp-optHires', hires)):
        if await page.locator(sel).is_checked() != want_on: await page.locator(sel).click()
    await page.locator('#xdjp-go').click()
    await page.wait_for_function("!document.getElementById('xdjp-result').hidden || document.querySelector('#xdjp-msg .err')", timeout=280000)
    got = await page.evaluate("""async()=>{const b=new Uint8Array(await (await fetch(document.getElementById('xdjp-dl').href)).arrayBuffer());
      return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b))).map(x=>x.toString(16).padStart(2,'0')).join('');}""")
    print(label, '->', 'IDENTICAL to the reference' if got == want else 'MISMATCH'); return got == want
async def main():
    ok = True
    async with async_playwright() as p:
        b = await p.chromium.launch(); page = await b.new_page(); errs = []
        page.on('pageerror', lambda e: errs.append(str(e)))
        ok &= await check(page, 'exFAT only', False, False, reference([]))
        ok &= await check(page, 'exFAT + extra features', True, False, reference(['--djp']))
        print('page errors:', errs or 'none'); await b.close()
    sys.exit(0 if ok else 1)
asyncio.run(main())
