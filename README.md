# XDJ-1000MK2 Firmware Patcher

**By: Audi Étoffe** · [SoundCloud](https://soundcloud.com/audietoffe) · [linktr.ee/etoffe](https://linktr.ee/etoffe)

A browser-based patcher that turns Pioneer's original **XDJ-1000MK2 1.45** update file into **firmware 2.0**. Everything runs
on your own computer: nothing is uploaded, and no Pioneer firmware is stored in this repository.
100% Free, but consider following my music pages @ linktr.ee/etoffe

**Use it online:** https://audietoffe.github.io/xdj-1000mk2-firmware-patcher/ &nbsp;(or download `index.html` and open it in a
browser: it also works offline).

## What firmware 2.0 adds
- **exFAT USB drives**: the player reads exFAT sticks and drives (including 64 GB and larger) as well as FAT32. Read-only.
- **Extra features** (optional): 3-band waveform colours, 32-beat loops, gated hot cues, extended beat jump.
- **88.2 / 96 kHz FLAC and WAV** (optional, **experimental and not tested on hardware**): the CDJ-2000NXS2's sample-rate checks.

## How to use it
1. Download the XDJ-1000MK2 **1.45** firmware from Pioneer DJ's support site, unzip it, and keep `XDJ1KMK2.UPD` safe.
2. Open the page, choose that file, pick your options, and press **Patch the firmware**. It takes about a minute.
3. Download the result (it must keep the name `XDJ1KMK2.UPD`) and copy it to the top level of a **FAT32** USB stick.
4. Switch the player off, insert the stick, switch on while holding **LOOP IN** and **RELOOP/EXIT**. Do not interrupt the update.
5. The version screen shows **2.00**. To go back, flash Pioneer's original 1.45 file the same way.

Flashing firmware always carries some risk. This is an unofficial modification: use it at your own risk, with the player on mains
power.

## Status
| | |
|---|---|
| exFAT support | Worked on a real XDJ-1000MK2 in development builds (stick mounts, library loads) |
| Release build "2.00" (quiet, no diagnostics) | Built and emulated; **awaiting confirmation on hardware** |
| Patcher output | Byte-identical to the Python reference tool, checked in Node and in Chromium; the page re-reads its own output before offering it |
| 88.2 / 96 kHz audio | Checked in emulation only; **untested on hardware** |
| Text on the player's own screen | Not implemented |

## How it works
The update file is a container of S-records. The MAIN section holds an LZSS-compressed firmware image. The page decompresses it,
applies a list of checked byte patches (`data/patchspec.json`; each patch refuses to apply unless the original bytes are exactly what
it expects), recompresses it, rebuilds the container with correct checksums, decompresses the result again to prove it round-trips,
and only then offers the download. Inside the firmware, a small read-only layer (`src/`) presents an exFAT volume to the player as a
virtual FAT32 volume, and three hooks make the player's media-identification routine accept it.

Accepted inputs: the original 1.45 file (verified by SHA-256), or djpatcher's output with all four options.

## Build and test
- **Rebuild the pages:** `python3 web/build_fragment.py` writes `index.html` and `embed/xdj-patcher-PASTE-INTO-BODY.html`
  (the latter is body-only, for site builders that want content between `<body>` tags).
- **Check the page end-to-end:** `python3 web/e2e.py /path/to/XDJ1KMK2.UPD` (needs `playwright` and Chromium).
- **Check the library in Node:** see `web/test_node.js`. **Python reference:** `tools/apply_spec.py`.
- **Firmware layer:** `python3 tools/build_fragments.py --quiet --src src <base.UPD> <outdir>` (needs `sh4-linux-gnu-gcc`);
  `tools/make_spec.py` regenerates the patch data. `tests/` holds the host tests; `emulation/` the qemu-sh4 harnesses used to run
  the real firmware routines.

## Layout
`index.html` page (GitHub Pages) · `embed/` body-only version · `web/` page source and library · `data/` patch data ·
`src/` exFAT layer (C) · `tools/` container, compression and build tools · `tests/` · `emulation/`

## Licence
GNU General Public License, version 2 only (`SPDX: GPL-2.0-only`). See `LICENSE`. Some material is not covered by it: see `NOTICE`.
