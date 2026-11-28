# `sp1-drum` — SP-1 Standalone 4-Track Euclidean Drum Machine & C11 DSP Synth

> [!WARNING]
> **Work in Progress — Does Not Work Yet:** This project is an active work in progress and is **not** functional yet. Expect broken builds, incomplete features, and non-working hardware behavior.

`sp1-drum` is an open-source standalone firmware that transforms the **Teenage Engineering Stem Player (SP-1)** (Nordic nRF52840 @ 64 MHz ARM Cortex-M4F, 1 MB Flash, 256 KB SRAM) into a **4-mode Bjorklund Euclidean drum machine**, **4-voice C11 real-time drum synthesizer with 4 curated kits**, **live Jamaican Dub Siren**, **24 PPQN USB-C MIDI clock master/follower**, and **tape-gliding stereo performance FX unit** running at 48 kHz.

> **Full Musician's Guide & MIDI Chart:** See [`docs/USER_MANUAL.md`](docs/USER_MANUAL.md) for the complete 4-Mode gesture guide, Dual 4-LED meter reference, and hardware troubleshooting, and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the real-time system architecture.

---

## Key Features

- **4-Track Multi-Speed Bjorklund Euclidean Sequencer (`src/core/`):**
  - Independent `E(k, n, rot)` Euclidean generators on Tracks 1–4 with polymeter step lengths (`n = 1..32`, default `16`), pulse densities (`k = 0..n`), rotations (`rot = 0..n-1`), **per-track clock speeds (`0.25x`, `0.5x`, `1x`, `2x`)**, and **per-track hit probability (`0..100%`)**.
  - **Global 16th-Note Swing (`50%..66%`)** + **Per-Track J Dilla Micro-Timing Nudge (`-35.0 ms .. +35.0 ms`)** evaluated via sample-accurate 32nd-slot lookahead rings (`[0.42..0.58]` center grid-lock detent) + **momentary `±6%` DJ Tempo Bend**.
- **4-Voice Pure C11 Drum Synthesizer + 4 Kits + Channel Strips + Dub Siren @ 48 kHz (`src/dsp/drum_voices.c`):**
  - **Track 1 — Kick / Sub (GM `36`):** Flash LUT sine/triangle oscillator, exponential pitch sweep, phase-distortion punch, and soft-clip + West-Coast wavefold drive.
  - **Track 2 — Snare / Rimshot Morph (GM `38`):** Dual body/rim resonators + `xorshift32` noise through a Cytomic TPT SVF (`0.0` woody Rimshot ↔ `1.0` crisp Snare).
  - **Track 3 — 6-Oscillator Metallic Hi-Hat (GM `42`):** 6 inharmonic square oscillators + resonant HP Cytomic TPT SVF (`0.0` Closed ↔ `1.0` Open decay).
  - **Track 4 — 2-Op FM / West-Coast Wavefold Perc (GM `45`):** Phase-modulated sine pair with modulator feedback and `257`-entry Flash LUT wavefolding.
  - **4 Curated Synth Kits + 4-Ch Mixer Strips** (Level, Stereo Pan, Dub Delay Send, and zero-cost-detent 1-pole TPT LP/HP Tone Filter) + **Live Dub Siren** (`Function` hold during playback).
- **Stereo Master Performance FX Bus (`src/dsp/master_fx.c`):**
  - **Tape-Style Pitch-Gliding Dub Ping-Pong Delay (`16.0 KB` static SRAM):** `4,096`-sample/tap stereo ping-pong tape delay (`8 kHz` `6x` warm tape decimation = `512 ms`/tap, `1,024 ms` ping-pong) with **continuous fractional Doppler pitch-shift glide** while moving `F3` that magnetically locks into **`1/32`, `1/16`, `dotted 1/16`, `1/8`, `dotted 1/8`, or `1/4`** tempo divisions!
  - **Tempo-Synced Beat-Repeat / Stutter Looper (`32.0 KB` static SRAM):** `8,192`-frame circular stereo buffer supporting momentary `1/8`, `1/16`, `1/32`, and `1/64` ratchets with 64-frame clickless boundary crossfading.
  - **DJ Resonant Cytomic TPT SVF Isolator** + **ZOH Sample-Rate Decimator / Bitcrusher** + **Master Soft-Clipper**.

---

## 4-Mode Screenless Control & Dual 4-LED Cheat Sheet

- **`Vol-` / `Vol+`:** **100% Standalone Master Volume** (`±8` steps, `0..127`, `800 ms` vertical Side LED HUD, zero button combinations).
- **`Play`:** **Master Transport Start / Stop** (never used in chords due to `AIN0` resistor-ladder physics).
- **Short-Press `Function (••)` (`< 200 ms`):** Cycles **`Mode 1 → Mode 2 → Mode 3 → Mode 4 → Mode 1`** and shows a **Mode Toast** on the **4 Side LEDs** (`1.2 s` solid + `2x` blinks).
- **Hold `Function (••)` While Playing:** Suppresses power-off and triggers the **Dub Siren** (`Func + F1`=Pitch, `F2`=LFO Rate, `F3`=LFO Shape/Depth, `F4`=Dub Echo Throw). While **Stopped**, a `3.0 s` hold enters graceful `SYSTEM_OFF`.

| Mode (Side LED) | Unshifted `F1–F4` | Tap `T1–T4` | `RWD (<<)` / `FWD (>>)` | `Hold T_x + F1 .. F4` |
|---|---|---|---|---|
| **1. Pattern** | Track 1–4 Euclidean Hits (`0..n`) | Mute Toggle | Global Swing (`±2%`, `50–66%`) | `F1`: Speed (`0.25x..2x`) · `F2`: Rotation · `F3`: Dilla Offset (`±35 ms`) · `F4`: Length (`1..32`) |
| **2. Track** | Track 1–4 Mix Level | Mute Toggle | Master Tempo (`±2 BPM` tap / `±5` hold) | `F1`: Tone (`LP↔Flat↔HP`) · `F2`: Stereo Pan · `F3`: Delay Send · `F4`: Hit Probability |
| **3. Sound Edit** | Voice 1–4 Character Macro | Live Pad Audition | Cycle 4 Synth Kits | Deep 4-Parameter Voice Synthesis (`Tune/Pitch`, `Decay/Morph/Ratio`, `Punch/Cutoff`, `Drive/Fold`) |
| **4. Performance** | `F1`: DJ Filter · `F2`: Bitcrusher · `F3`: Tape Delay Time · `F4`: Delay Wet/FB | *(Hold)* `1/8 .. 1/64` Stutter (`F1–F4` stay live!) | *(Hold)* `±6%` DJ Tempo Bend | `F1–F4` sweep Master FX live over held `T1–T4` stutter ratchets |

### Dual 4-LED Display Summary
- **4 Side LEDs (`Play 1..4`):** `800 ms` Volume HUD → `1.68 s` Mode Toast (`1.2 s` solid + `2x` blink) → Quarter-Note Beat Chaser (playing) → `3.0 s` Battery Level Bar (boot/stop/USB, breathing while charging) → Dim Active-Mode Glow (`48/255`).
- **4 Track LEDs (`Track 1..4`):** Temporary parameter meters (`UNBALANCED_BAR`, `MIDDLE_BALANCED`, `BASE2_COUNTER` mod-16 where `0/16/32 = 0000` & `1/17 = 0001`, `DISCRETE_SELECT`) → Dub Siren LFO Wave → Stutter Pulse → Drum Trigger Flashes (`255` accent / `200` normal, `32` idle, `8` muted).

---

## Web Serial Flashing & `T1+T4` Bootloader Recovery Guide

The SP-1 includes a factory hardware bootloader in protected flash (`0x00000..0x1FFFF`). `sp1-drum` always links into `slot0_partition` at **`0x00020000`** (`CONFIG_USE_DT_CODE_PARTITION=y`), so the factory bootloader is never touched:

1. **Power off your SP-1:** Stop playback (`Play`), then hold **`Function (••)`** for 3 seconds until the LEDs fade out into `SYSTEM_OFF`.
2. **Enter `T1+T4` Bootloader Mode:** Hold **`Track 1 (T1)` + `Track 4 (T4)`** while plugging in USB-C until all 4 corner Track LEDs illuminate solid.
3. **Download `zephyr.bin`:** Grab `zephyr.bin` from [**GitHub Releases**](https://github.com/davidlwarner/sp1-drum/releases) or download the `sp1-drum-firmware` artifact from the latest passing [**GitHub Actions CI**](https://github.com/davidlwarner/sp1-drum/actions/workflows/ci.yml) run.
4. **Flash via Web Serial:** Open [`https://solderless.engineering`](https://solderless.engineering) in Chrome or Edge, connect over Web Serial, and flash `zephyr.bin`.

---

## Offline WAV/MIDI Simulator (`sp1_sim`) & Zephyr ARM Build

```bash
# Host C11/C++17 15-suite CTest harness + offline WAV/MIDI simulator
CC=gcc CXX=g++ cmake -S tests -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure

./build-host/sp1_sim --wav build-host/showcase.wav --midi-csv build-host/showcase.csv \
    --duration-ms 8000 --bpm 124 --swing 58 --fx-showcase

# Zephyr v4.3.1 + Zephyr SDK 0.17.4 ARM firmware build (do NOT pass -DSNIPPET=cdc-acm-console)
west build -b stem_player . --build-dir build -p auto
```

---

## Cortex-M4F Memory & Real-Time Cycle Footprint

Built with **Zephyr `v4.3.1`** + **Zephyr SDK `0.17.4`** (`arm-zephyr-eabi-gcc 12.2.0`, `-O2`, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, `CONFIG_NRF_ENABLE_ICACHE=y`):

| Metric | Measured Value | Hardware Capacity / Budget | Utilization | Headroom Remaining |
|---|---:|---:|---:|---:|
| **Engine Static Structures (`synth + fx + seq + ui`)** | **`51,004 B`** (`49.81 KB`) | `32 KB` stutter + `16 KB` `8 kHz` tape delay | **`38.91%` of `128 KB` gate** | Zero heap allocation |
| **Total Firmware Static `SRAM` (`data + bss`)** | **`~114 KB`** (`< 117 KB`) | `256 KB` (`262,144 B`, CI gate `<= 128 KB`) | **`~44.5%` of `256 KB`** | `> 140 KB` SRAM free |
| **Worst-Case Audio Cycle Load (96 frames / `2.0 ms`)** | **`64,850` avg / `65,784` peak cyc/block** | `128,000` cyc/block @ 64 MHz (`<= 55%` / `70,400` target) | **`50.7%` avg / `51.4%` peak** | `> 48%` CPU headroom |

---

## License & Attribution

Released under the **MIT License** — see [`LICENSE`](LICENSE). Built upon the work of [`ericlewis/sp1-midi`](https://github.com/ericlewis/sp1-midi), [`timknapen/SP-1-dev`](https://github.com/timknapen/SP-1-dev), [`chattock/sp1-tape-looper`](https://github.com/chattock/sp1-tape-looper), [`Worldwave/Wakes`](https://github.com/Worldwave/Wakes), and The Zephyr Project (see [`ATTRIBUTION.md`](ATTRIBUTION.md)).
