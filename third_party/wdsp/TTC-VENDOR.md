# WDSP in tentec-console

This directory is AetherSDR's vendored WDSP 2.10 (`third_party/wdsp` of
https://github.com/aethersdr/AetherSDR at main @ 86f592a, 2026-10-14), itself
TAPR's OpenHPSDR-wdsp `wdsp 2.10/Source` at `COMMIT` plus the fixes listed in
`AETHERSDR-PATCHES.md` and the Unix portability layer in `port/`.
WDSP: Copyright (C) Warren Pratt, NR0V, and contributors — GPL-2.0-or-later
(`LICENSE`), compatible with this project's GPL-3.

One change from AetherSDR's copy: `upstream/nnr_model_0.c` and
`upstream/nnr_model_1.c` (34.7 MB of generated model data) are not committed.
`CMakeLists.txt` downloads them from TAPR at the pinned revision into the build
directory and checks their SHA-256 (byte-identical to AetherSDR's copies,
verified 2026-10-14). An offline build needs them copied there by hand.

Used for SDR receive audio (the RX ▾ menu): `src/sdr/SdrRx.*`.
