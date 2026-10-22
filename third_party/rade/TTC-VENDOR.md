# RADE (FreeDV Radio Autoencoder) — how it gets into this build

Nothing big is committed here. CMake fetches, SHA-256-checked, once per
build tree:

| What | From | Pin | License |
|---|---|---|---|
| `rade_c` (the pure-C RADE: OFDM modem + encoder/decoder with compiled-in weights, ~47 MB of C arrays) | `github.com/freedv/rade_c` | `c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9` | BSD-2 (Peter B Marks, David Rowe) |
| Opus with LPCNet feature extraction + FARGAN vocoder | `github.com/xiph/opus` | `940d4e5af64351ca8ba8390df3f555484c567fbb` (rade_c's own pin) | BSD-3 |
| Opus DNN model data | `media.xiph.org/opus/models/opus_data-4ed9445b….tar.gz` | fetched + sha256-checked by Opus's own `autogen.sh` | BSD-3 |

Opus is built exactly as rade_c's `cmake/BuildOpus.cmake` builds it
(`autogen.sh && configure --with-pic --enable-osce --enable-dred
--disable-shared`, rade_c's `opus-nnet.{h,c}.diff` applied), static.
Arch's system opus cannot be used: it exports none of the `lpcnet_*` /
`fargan_*` / `compute_generic_*` symbols RADE needs and ships no `dnn/`
headers. rade_c at this pin already carries the two phase-renormalization
fixes AetherSDR cherry-picks (`rade_bpf.c`, `rade_rx.c`).

`text/` IS committed (small): the end-of-over callsign codec RADE V1
stations actually use — 6-bit chars + CRC-8 + LDPC(112,56) + interleave —
copied from AetherSDR's `third_party/radae/src` (GPL-3, KK7GWY and
contributors; its header lists its include changes):

- `rade_text.{c,h}` — FreeDV-GUI `src/pipeline/rade_text.c`, BSD-2 (Mooneer Salem)
- `mpdecode_core.*`, `HRA_56_56.*`, `ldpc_codes.*`, `gp_interleaver.*`,
  `comp.h`, `debug_alloc.h` — codec2, LGPL-2.1 (David Rowe et al.)
- `ulog.h` — logging stub

Do NOT use rade_api.h's `rade_tx_set_eoo_callsign` helper: plain 7-bit
packing, not what freedv-gui sends, so nobody else decodes it.
