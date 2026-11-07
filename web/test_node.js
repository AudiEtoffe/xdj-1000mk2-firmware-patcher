/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
const fs = require('fs'); const X = require('./xdjpatch.js');
const spec = JSON.parse(fs.readFileSync('../data/patchspec.json', 'utf8'));
const cases = JSON.parse(process.argv[2]);   // [[input, ref, {opts}], ...]
(async () => {
  let allOk = true;
  for (const [inp, ref, opts] of cases) {
    const t0 = Date.now(); let last = -1;
    const r = await X.patchFirmware(new Uint8Array(fs.readFileSync(inp)), spec, opts, (f, t) => { const p = Math.floor(f * 10); if (p !== last) { last = p; process.stdout.write('.'); } });
    const want = fs.readFileSync(ref); const got = Buffer.from(r.bytes);
    let same = got.length === want.length && got.equals(want);
    console.log(`\n${inp} ${JSON.stringify(opts)} -> ${r.kind} / ${r.layout}  ${((Date.now() - t0) / 1000).toFixed(1)}s  ${same ? 'IDENTICAL to Python reference' : 'DIFFERENT'} (${got.length} vs ${want.length})`);
    if (!same) { let d = 0, first = -1; for (let i = 0; i < Math.min(got.length, want.length); i++) if (got[i] !== want[i]) { d++; if (first < 0) first = i; } console.log('  differing bytes', d, 'first at', first); allOk = false; }
  }
  process.exit(allOk ? 0 : 1);
})().catch((e) => { console.error('ERROR:', e.userMessage || e.stack); process.exit(2); });
