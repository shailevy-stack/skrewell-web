# SKREWELL — web

A browser version of SKREWELL, the M-VAVE FM-1 firmware: a self-modulating drone and texture instrument rebuilt from **Skrewell** for Native Instruments Reaktor 5, designed by **Alexander Potekhin aka Lazyfish**. The instrument's design and its 89 Bank 1 presets are his.

`index.html` is the whole app: one page, the engine inside it as WebAssembly, running in an AudioWorklet. Serve it over http(s) (opened as a file, browsers refuse the audio worklet).

- A full-screen scope behind everything; 8 functions × 8 channels with DRAW / WRAP / RAND, the three modes, OSC / FILTER / DELAY / FLOW.
- Reverb as a send or an insert, with a low cut / high cut on its return; glue compressor.
- 128 presets in 8 octaves (1–89: the original's Bank 1), each with 8 scenes. A preset is the instrument (its hidden ranges and FLOW settings, locked); a scene is everything on the panel: mode, faders, knobs, FX + GLO (or keep those global) and its loop. Made-up names; hold a preset 4 s for the original.
- REC: record moves (only the controls you touch), loop, overdub; hold to clear.
- Tap the scope for a full-screen scope, tap again for the controls. Tap the SKREWELL title for settings (snapshot MORPH time) and info.

## Build

`./build.sh` compiles `engine/` with clang (target wasm32, no libc) and writes `index.html` from `src/app.html`.

- `engine/` is the FM-1 firmware's engine (`app/dsp/`) and fm1-base's `fastmath.h` (MPL-2.0).
- `src/wasm_glue.c` is the API the page calls.

Port by Shai Levy. Published with the permission of Alexander Potekhin aka Lazyfish.
