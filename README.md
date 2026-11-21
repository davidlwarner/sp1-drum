# `sp1-drum` — SP-1 Standalone 4-Track Euclidean Drum Machine & C11 DSP Synth

`sp1-drum` is an open-source standalone firmware that transforms the **Teenage Engineering Stem Player (SP-1)** (Nordic nRF52840 @ 64 MHz ARM Cortex-M4F, 1 MB Flash, 256 KB SRAM) into a **4-track Bjorklund Euclidean drum machine**, **4-voice C11 real-time drum synthesizer**, **24 PPQN USB-C MIDI clock master/follower**, and **stereo performance FX unit** running at 48 kHz.

> **Full Musician's Guide & MIDI Chart:** See [`docs/USER_MANUAL.md`](docs/USER_MANUAL.md) for in-depth performance workflows, J Dilla micro-timing tutorials, and hardware troubleshooting, and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the real-time system architecture.

---

## Key Features

- **4-Track Bjorklund Euclidean Rhythm Engine (`src/core/`):**
  - Independent `E(k, n, rot)` Euclidean generators on Tracks 1–4 with per-track polymeter step lengths (`n = 1..32`), pulse densities (`k = 0..n`), and pattern rotations (`rot = 0..n-1`).
  - **Global 16th-Note Swing (`50%..66%`)** + **Per-Track J Dilla Micro-Timing Nudge (`-35.0 ms .. +35.0 ms`, `±1,680` frames @ 48 kHz)** evaluated via a 1-step sample-accurate lookahead ring buffer with a generous `[0.42..0.58]` center grid-lock deadzone.
- **4-Voice Pure C11 Drum Synthesizer @ 48 kHz (`src/dsp/drum_voices.c`):**
  - **Track 1 — Kick / Sub (GM Note `36`):** `uint32_t` phase-accumulator sine/triangle core (`257`-entry Flash LUT), exponential pitch sweep (`200 Hz → 45 Hz`), transient phase-distortion punch, and cubic soft-clipper + West-Coast wavefolder drive (kept 100% dry on the Dub Delay send).
  - **Track 2 — Snare / Rimshot Morph (GM Note `38`):** Dual detuned shell/rim resonators (`~185 Hz` body + `~335–465 Hz` woody rim knock) mixed with `xorshift32` noise through a Cytomic Trapezoidal Integrated State-Variable Filter (TPT SVF)—continuously morphing from a tight, woody 808/909 **Rimshot (`0.0`)** to a crisp **Snare (`1.0`)**.
  - **Track 3 — 6-Oscillator Metallic Hi-Hat (GM Note `42`):** 6 inharmonic square-wave oscillators (`205, 304, 369, 522, 540, 800 Hz`) with upper-octave harmonic taps summed through a resonant `5.6 kHz` High-Pass Cytomic TPT SVF with continuous **Closed (`0.0`) ↔ Open (`1.0`)** decay morphing.
  - **Track 4 — 2-Op FM / West-Coast Wavefold Perc (GM Note `45`):** Phase-modulated sine pair with modulator self-feedback, fast pitch/index envelopes, and `257`-entry Flash LUT West-Coast wavefolding.
- **Stereo Master Performance FX Bus (`src/dsp/master_fx.c`):**
  - **Selective Dub Ping-Pong Delay (`8.0 KB` static SRAM):** Tempo-synced stereo ping-pong BBD delay (`16 kHz` 3x decimation with linear read interpolation and `~3.2 kHz` one-pole LP damping) fed selectively by Track 2 (Snare/Rimshot) and Track 4 (FM Perc).
  - **Tempo-Synced Beat-Repeat / Stutter Looper (`32.0 KB` static SRAM):** `8,192`-frame circular stereo buffer supporting quantized `1/4`, `1/8`, `1/16`, `1/32`, and `1/64` ratchets with a 64-frame clickless boundary crossfade.
  - **DJ-Style Resonant Cytomic TPT SVF Isolator:** One-knob Low-Pass sweep (`< 0.45`, `20 kHz → 60 Hz`), center bypass deadzone (`[0.45..0.55]`), and High-Pass sweep (`> 0.55`, `30 Hz → 12 kHz`).
  - **Zero-Order Hold Sample-Rate Decimator / Bitcrusher + Master Soft-Clipper.**
- **24 PPQN USB-C MIDI Clock Sync & Standalone Audio (`src/hal/`):**
  - Runs standalone on battery or USB-C via headphones (`CS42L42` 24-bit I2S0 EasyDMA) and the internal speaker (`TAS2505`).
  - Class-compliant USB-C MIDI 2.0/1.0 UMP endpoint: emits/locks to 24 PPQN `0xF8` Timing Clock, `0xFA` Start, `0xFB` Continue, and `0xFC` Stop (with automatic `500 ms` failover back to internal master clock), outputs General MIDI drum notes (`36, 38, 42, 45`), and transmits Fader CCs (`CC 1..4`).

---

## Screenless Hardware Gesture & LED Cheat Sheet

> **SP-1 Hardware Safety (`AIN0` Resistor Ladder):** On the SP-1 PCB, `Play` shares the `AIN0` resistor ladder with `T1..T4` and dominates ladder voltage (`1587–1592 mV`). `Play` is strictly Master Transport Start/Stop and is **never** used as a shift modifier. All track/fader shift gestures use **`Function (••)` (`GPIO P0.27`)** or **Transport/Volume buttons (`AIN1`: `RWD <<`, `FWD >>`, `Vol-`, `Vol+`)**. Mode switches automatically latch fader positions (`16-LSB` anti-clobber threshold) so releasing a shift key never overwrites your patterns.

### 1. Normal Pattern Mode (Default Page)

| Physical Control | Function | Range / Behavior |
|---|---|---|
| **`Play`** | Master Transport Start / Stop | Starts or stops sequencer playback (`0xFA` Start / `0xFC` Stop) |
| **`Fader 1..4`** | Track 1..4 Euclidean Pulses (`k`) | Distributes `0..n` hits evenly across track step length `n` (`E(k, n)`) |
| **`T1..T4`** | Track 1..4 Mute Toggle | Mutes or unmutes internal synth voice and outbound MIDI notes |
| **`Vol-` / `Vol+`** | Fine Master Tempo | `-2 BPM` / `+2 BPM` per tap (`60..180 BPM`, default `120 BPM`) |

### 2. `Function (••)` Shift Mode — J Dilla Micro-Timing, Coarse Tempo, Swing & Power

| Chord / Gesture | Function | Range / Behavior |
|---|---|---|
| **`Function + Fader 1..4`** | Per-Track Dilla Micro-Timing Nudge | `-35 ms` (rushed/early) .. `0 ms` (`[0.42..0.58]` center grid-lock deadzone) .. `+35 ms` (dragged/late) |
| **`Function + T1..T4`** | Reset Track 1..4 Dilla Nudge | Snaps track micro-timing to exact `0.0 ms` and re-latches fader |
| **`Function + Vol- / Vol+`** | Coarse Master Tempo | `-10 BPM` / `+10 BPM` per tap (`60..180 BPM`) |
| **`Function + RWD / FWD`** | Global 16th-Note Swing | `-2%` / `+2%` per tap (`50%` straight .. `66%` triplet swing) |
| **`Hold Function (••) 3 s`** | Graceful Hardware Power-Off | Fades LEDs, stops I2S0, mutes/resets `TAS2505` & `CS42L42`, disables `3.072 MHz` osc, enters `SYSTEM_OFF` |

### 3. `RWD (<<)` & `FWD (>>)` Shift Modes — Euclidean Rotation & Polymeter Length

| Chord / Gesture | Function | Range / Behavior |
|---|---|---|
| **`RWD + Fader 1..4`** | Track 1..4 Euclidean Rotation (`rot`) | Rotates pattern offset across `0 .. n-1` steps |
| **`RWD + T1..T4`** | Reset Track 1..4 Rotation | Resets track rotation to `0` (downbeat aligned) and re-latches fader |
| **`FWD + Fader 1..4`** | Track 1..4 Polymeter Step Length (`n`) | Sets independent track loop length from `1..32` steps |
| **`FWD + T1..T4`** | Reset Track 1..4 Step Length | Resets track loop length to `16` steps and re-latches fader |

### 4. `Vol+ (+)` Shift Mode — Voice Timbre & Rimshot-Snare Morph

| Chord / Gesture | Target Voice | Sound Design Parameter |
|---|---|---|
| **`Vol+ + Fader 1`** | **Track 1 (Kick / Sub)** | Sub Decay & Saturation Drive (`0.0` tight punch → `1.0` booming saturated 808 sub) |
| **`Vol+ + Fader 2`** | **Track 2 (Snare / Rimshot)** | Continuous **Rimshot (`0.0`) ↔ Snare (`1.0`)** morph (`default 0.65`) |
| **`Vol+ + Fader 3`** | **Track 3 (Metallic Hi-Hat)** | Continuous **Closed Hat (`0.0`) ↔ Open Hat (`1.0`)** decay morph (`default 0.20`) |
| **`Vol+ + Fader 4`** | **Track 4 (2-Op FM Perc)** | 2-Op FM Index & West-Coast Wavefold Timbre (`0.0` woody FM → `1.0` metallic wavefold) |

### 5. Performance FX Mode — Toggle via `Function + T1 + T4` or `Vol- + T1`

| Control (FX Mode Active) | Master FX Processor | Behavior |
|---|---|---|
| **`Fader 1`** | **DJ Resonant TPT SVF Isolator** | `< 0.45`: Low-Pass (`20 kHz → 60 Hz`) · `[0.45..0.55]`: Center Bypass · `> 0.55`: High-Pass (`30 Hz → 12 kHz`) |
| **`Fader 2`** | **Latched Beat-Repeat / Stutter** | `< 0.15`: Off · `0.15..0.32`: `1/4` · `0.32..0.50`: `1/8` · `0.50..0.68`: `1/16` · `0.68..0.84`: `1/32` · `>= 0.84`: `1/64` |
| **`Fader 3`** | **Bitcrusher & ZOH Decimator** | `0.0`: Clean 48 kHz 16-bit → `1.0`: 16x ZOH sample-rate decimation + bit reduction |
| **`Fader 4`** | **Selective Dub Ping-Pong Delay** | Stereo ping-pong wet level (`0.0..1.0`) & feedback (`0.25..0.80`) on Track 2 (Snare/Rim) + Track 4 (Perc) |
| **`Hold T1 / T2 / T3 / T4`** | **Momentary Stutter Punch-In** | Momentary `1/8` (`T1`), `1/16` (`T2`), `1/32` (`T3`), or `1/64` (`T4`) ratchet override while held |

### 6. 8-Channel PWM LED Visual Feedback System

| Mode | Corner Track LEDs (`Track 1..4`) | Center Cross LEDs (`Play 1..4`) |
|---|---|---|
| **Normal Pattern Mode** | Dim (`8`) when muted · Idle (`32`) when active · Flashes `255` (accent) / `200` (normal) on trigger with `~85 ms` decay | Quarter-note beat chaser (`255` on Beat 1, `180` on Beats 2–4); background floor rises to `16` when locked to external USB-C 24 PPQN MIDI clock |
| **`Function` Held (Dilla Nudge Meter)** | Focused track at `255` (others at `40`) | **Bipolar Polarity Meter:** Center pair **`Play 2 + Play 3`** (`160`) = exact `0.0 ms` grid lock · Left **`Play 2 → Play 1`** = rushed (`-0.1..-35 ms`) · Right **`Play 3 → Play 4`** = dragged (`+0.1..+35 ms`) |
| **`RWD` / `FWD` Held (Bar Meter)** | Focused track at `255` (others at `40`) | **4-Segment Bar Meter (`Play 1 → 4`):** Continuous `0..1020` scale displaying Euclidean rotation (`0..n-1`) or polymeter step count (`1..32`) |

---

## Web Serial Flashing & `T1+T4` Bootloader Recovery Guide

The SP-1 includes a factory hardware bootloader in protected flash (`0x00000..0x1FFFF`). `sp1-drum` always links into `slot0_partition` at **`0x00020000`** (`CONFIG_USE_DT_CODE_PARTITION=y`), so the factory bootloader is never touched and you can flash or recover the device over a standard USB-C cable without an SWD debugger:

1. **Power off your SP-1:** If running, hold **`Function (••)`** for 3 seconds until the LEDs fade out and the unit enters `SYSTEM_OFF` (or unplug USB-C if the battery is empty).
2. **Enter `T1+T4` Bootloader Mode:** Press and hold **`Track 1 (T1)` + `Track 4 (T4)`** simultaneously, then plug the USB-C cable into your computer while keeping `T1+T4` held.
3. **Confirm 4 Solid Track LEDs:** All four corner Track LEDs will illuminate solid, indicating the Teenage Engineering bootloader is active. You may now release `T1+T4`.
4. **Flash via Web Serial:**
   - Download `zephyr.bin` from the [`preview-firmware`](https://github.com/davidlwarner/sp1-drum/raw/preview-firmware/zephyr.bin) branch (or build `build/zephyr/zephyr.bin` locally).
   - Open the Solderless Engineering Web Serial utility ([`https://solderless.engineering`](https://solderless.engineering)) in Google Chrome or Microsoft Edge.
   - Connect to the Stem Player bootloader over Web Serial, select `zephyr.bin`, and wait for the flash progress bar to complete.
5. **Reboot & Play:** Unplug and reconnect USB-C (or press `Function ••` to wake on battery). `sp1-drum` immediately boots into standalone 48 kHz drum machine mode.

---

## Building & Running the Offline WAV/MIDI Simulator (`sp1_sim`)

All portable sequencer (`src/core/`), synthesis/FX (`src/dsp/`), and shared HAL (`src/hal/sp1_hal_common.c`) modules are pure ISO C11 (`-std=c11 -Wall -Wextra -Werror`) with zero Zephyr dependencies. You can run the 15-suite CTest harness and render 48 kHz 16-bit stereo WAV files + timestamped MIDI CSV logs on any Linux/macOS workstation:

```bash
# 1. Configure and build the host C11/C++17 test & simulator suite
CC=gcc CXX=g++ cmake -S tests -B build-host
cmake --build build-host -j

# 2. Run all 15 unit & cycle-budget benchmark suites
ctest --test-dir build-host --output-on-failure

# 3. Render an 8-second 48 kHz 16-bit stereo WAV + MIDI CSV showcase
./build-host/sp1_sim \
    --wav build-host/showcase.wav \
    --midi-csv build-host/showcase_midi.csv \
    --duration-ms 8000 \
    --bpm 124 \
    --swing 58 \
    --dilla-snare-ms 18 \
    --dilla-hat-ms -12 \
    --fx-showcase
```

---

## Building the Zephyr ARM Cortex-M4F Firmware (`stem_player`)

### Prerequisites
- **Zephyr RTOS `v4.3.1`** (minimal module allowlist: `cmsis`, `cmsis_6`, `hal_nordic` with `select CMSIS_CORE_SYSTEM_CORE_CLOCK if ARM` on `SOC_FAMILY_NORDIC_NRF`)
- **Zephyr SDK `0.17.4`** (`arm-zephyr-eabi-gcc` + `hosttools`)
- **`west`**, **`cmake` >= 3.20**, **`ninja`**

### Build Command
```bash
west build -b stem_player . --build-dir build -p auto
```

> **Note (Zephyr `v4.3.1`):** Do **not** pass `-DSNIPPET=cdc-acm-console` — `app.overlay` and `prj.conf` already configure the single composite USB CDC ACM console + MIDI 2.0/1.0 device. Passing `-DSNIPPET=cdc-acm-console` on Zephyr `v4.3.1` forces `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y` and injects a duplicate `cdc-acm-uart` node that collides with `init_usb()`.

The flashable raw binary is emitted at `build/zephyr/zephyr.bin` (linked directly at `0x00020000` with a raw ARM Cortex-M4 vector table for the Teenage Engineering SP-1 bootloader and `https://solderless.engineering` — **not** MCUboot-signed).

---

## Measured Cortex-M4F Memory & Real-Time Cycle Footprint

Built with **Zephyr `v4.3.1`** + **Zephyr SDK `0.17.4`** (`arm-zephyr-eabi-gcc 12.2.0`, `-O2`, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, `CONFIG_NRF_ENABLE_ICACHE=y`, `CONFIG_ARM_MPU=y`, `CONFIG_HW_STACK_PROTECTION=y`):

| Metric | Measured Value | Hardware Capacity / Budget | Utilization | Headroom Remaining |
|---|---:|---:|---:|---:|
| **`FLASH` (`slot0_partition` @ `0x20000`)** | **`172,100 B`** (`168.07 KB`) | `892 KB` (`913,408 B`) | **`18.84%`** | `741,308 B` (`81.16%`) |
| **`SRAM` (`sram0`, `data + bss`)** | **`105,740 B`** (`103.26 KB`) | `256 KB` (`262,144 B`, gate `<= 128 KB`) | **`40.34%`** | `156,404 B` (`59.66%`) |
| **ELF Sections (`arm-zephyr-eabi-size`)** | `text=169,076` · `data=3,020` · `bss=102,647` | `slot0` load addr `0x00020000` | Bootloader `0x0..0x1FFFF` protected | Zero dynamic allocation |
| **Audio Block Cycle Load (96 frames / `2.0 ms`)** | **`60,604` avg / `62,380` peak cyc/block** | `128,000` cyc/block @ 64 MHz (`<= 55%` / `70,400` target) | **`47.3%` avg / `48.7%` peak** | `> 51%` CPU headroom |

---

## Repository Structure

| Directory / File | Description |
|---|---|
| [`src/core/`](src/core/) | Pure C11 Bjorklund Euclidean generator (`euclid.c`), 24 PPQN master/follower clock (`clock_sync.c`), 4-track 1-step lookahead sequencer (`sequencer.c`), and screenless gesture/LED mapper (`ui_mapper.c`) |
| [`src/dsp/`](src/dsp/) | Pure C11 4-voice drum synthesizer (`drum_voices.c`, Flash sine/wavefold LUTs, Cytomic TPT SVFs) and stereo master performance FX bus (`master_fx.c`) |
| [`src/hal/`](src/hal/) | Portable C11 HAL interfaces, lock-free SPSC MIDI/control buses & DWT `CYCCNT` profiler (`sp1_hal_common.c`), Zephyr nRF52840 backend (`sp1_hal_zephyr.c`), and host WAV/CSV backend (`sp1_hal_host.c`) |
| [`sim/sp1_sim.c`](sim/sp1_sim.c) | Standalone host CLI rendering 48 kHz 16-bit stereo WAV files and MIDI CSV logs |
| [`tests/`](tests/) | 15 zero-dependency C11/C++17 host CTest unit and cycle-budget benchmark suites |
| [`docs/USER_MANUAL.md`](docs/USER_MANUAL.md) | Complete musician's performance guide, MIDI implementation chart, and hardware troubleshooting |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | System architecture, hard-RT concurrency contract, and memory/cycle budgets |
| [`boards/`](boards/), [`drivers/`](drivers/), [`subsys/`](subsys/), [`app/`](app/) | Upstream SP-1 Zephyr BSP (`ericlewis/sp1-midi`) with thin delegation hooks in `app/` |

---

## License & Attribution

Released under the **MIT License** — see [`LICENSE`](LICENSE) for full terms.

`sp1-drum` builds upon the work of the Teenage Engineering Stem Player open-source community, including [`ericlewis/sp1-midi`](https://github.com/ericlewis/sp1-midi), [`timknapen/SP-1-dev`](https://github.com/timknapen/SP-1-dev), [`chattock/sp1-tape-looper`](https://github.com/chattock/sp1-tape-looper), [`Worldwave/Wakes`](https://github.com/Worldwave/Wakes), and The Zephyr Project. See [`ATTRIBUTION.md`](ATTRIBUTION.md) for complete credits and references.
