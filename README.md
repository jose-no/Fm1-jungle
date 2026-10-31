# JUNGLE

A breakbeat slicer firmware for the M-VAVE FM-1. Load one break from a web page; the FM-1 cuts it into sixteenths and
rearranges it in time as it plays — shuffled beats, stutters, reverses, dropouts, fills — following a MIDI clock or
its own tempo, with the keys for live cue-juggling and beat repeats.

- **Site** (installer, break loader, manual): https://jose-no.github.io/fm1-jungle/
- [Manual](app/docs/MANUAL.md) · [Upload protocol](docs/PROTOCOL.md)

> **Untested on hardware so far.** It runs in fm1-base's host simulator and its tests pass, but no FM-1 has run it
> yet. The installer can put M-VAVE's firmware back; an FM-1 that does not start can be recovered with
> [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).

## Layout

| Path | What |
| --- | --- |
| `app/engine.c` | The engine: clock (internal or MIDI), the generative pattern, the slice voices, the FX. No platform calls |
| `app/brk.c` | The break in the flash and its upload over SysEx |
| `app/app.c` | The FM-1 app on fm1-base: the screen, the controls, MIDI |
| `web/loader/` | The break loader page: decode, trim, tempo and bars, slices on the hits, the snare, resampling, upload |
| `tests/` | The engine's test, the host simulator's script (`app/tests/basic.txt`), the loader page in headless Chromium against `brk.c` |
| `base/` | [fm1-base](https://github.com/charlesvestal/fm1-base) (git submodule) |

## Build

GitHub Actions (`.github/workflows/build.yml`) tests, builds `jungle-X.Y.fwsc` and the browser emulator, and publishes the
site to the `gh-pages` branch on every push to `main`. Locally:

    git clone --recursive https://github.com/jose-no/fm1-jungle && cd fm1-jungle
    sh tests/run_tests.sh                 # engine, host simulator, loader page (Node + Playwright)
    sh base/tools/get_toolchain.sh && sh base/bsp/tools/get_sdk.sh
    ./build.sh --release 0.1              # app/build/jungle-0.1.fwsc (Linux x86-64)

## Licence

GPL-3.0. fm1-base and felucca-bsp are MPL-2.0 (Charles Vestal; Leo Kuroshita, Hügelton Instruments).
