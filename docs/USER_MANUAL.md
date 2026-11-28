# SP-1 Euclidean Drum Machine — Musician's User Manual & Hardware Guide

`sp1-drum` transforms the **Teenage Engineering Stem Player (SP-1)** into a standalone 4-track Bjorklund Euclidean drum machine, 4-voice C11 drum synthesizer with 4 curated kits, live Jamaican Dub Siren, and stereo tape-gliding performance effects processor running at 48 kHz on the Nordic nRF52840 (64 MHz ARM Cortex-M4F).

---

## 1. Hardware Layout & 4-Mode Screenless Philosophy

The SP-1 has **no screen**. Every musical gesture is organized around **4 dedicated Modes**, **standalone Master Volume buttons**, **mode-dependent `RWD / FWD` rocker actions**, and **two independent 4-LED displays** (`4 Side LEDs` + `4 Track LEDs`):

- **4x Analog Fader Sliders (`F1–F4`):** Sampled at 12-bit resolution (`0..4080`) with an 8-LSB hardware noise deadband (`~0.2%`).
- **4x Track Buttons (`T1–T4`) + `Play` Button (`AIN0` Resistor Ladder):**
  - `T1` = **Track 1 (Kick / Sub)**
  - `T2` = **Track 2 (Snare / Rimshot Morph)**
  - `T3` = **Track 3 (6-Oscillator Metallic Hi-Hat)**
  - `T4` = **Track 4 (2-Op FM / West-Coast Wavefold Perc)**
  - `Play` = **Master Transport Start / Stop** (strictly standalone due to `AIN0` resistor-ladder physics)
- **Standalone Volume Buttons (`Vol-` / `Vol+` on `AIN1`):**
  - **100% dedicated Master Output Volume (`-8` / `+8` steps across `0..127`, default `64`)** in every mode and state, displaying a vertical `800 ms` bar on the **4 Side LEDs**. `Vol-` and `Vol+` **never** form combinations with any other button or slider.
- **Mode-Dependent `RWD (<<)` / `FWD (>>)` Rocker (`AIN1`):**
  - **Mode 1 (Pattern):** Global 16th-Note Swing (`-2% / +2%`, `50%..66%`)
  - **Mode 2 (Track):** Master Tempo (`-2 / +2 BPM` tap; hold `>= 350 ms` auto-repeats `-5 / +5 BPM` every `200 ms`, `60..180 BPM`)
  - **Mode 3 (Sound Edit):** Cycle 4 Curated Synth Kits (`808/909`, `Dry Woody`, `Industrial FM`, `Lo-Fi Dub`)
  - **Mode 4 (Performance):** Momentary DJ Tempo Nudge / Bend (`-6%` / `+6%` while held)
- **Dual-Role `Function (••)` Button (`GPIO P0.27`):**
  - **Short Press (`< 200 ms`, anytime):** Cycles `Mode 1 → Mode 2 → Mode 3 → Mode 4 → Mode 1` and flashes a Mode Toast (`1.2 s` solid + `2x` blinks) on the **4 Side LEDs**.
  - **Hold While Playing (`>= 200 ms` or `Function + F1–F4`):** Suppresses power-off and fires the live **Dub Siren**, with `F1–F4` sculpting Siren Pitch, LFO Rate, LFO Shape/Depth, and Dub Delay Echo Throw!
  - **Hold While Stopped (`3.0 s`):** Graceful hardware power-off into nRF52840 `SYSTEM_OFF` (`1.5 s` pocket-safe hold wakes on battery).

### Anti-Clobber Fader Latching (`16-LSB` Pickup)
Whenever you switch modes via `Function`, press or release `T1–T4` in Modes 1–3, load a kit preset, or release the Dub Siren, `sp1-drum` latches all 4 faders (`SP1_UI_FADER_ENGAGE_DELTA = 16` raw counts). Parked sliders never overwrite parameters on the new layer until you deliberately move them. *(Exception: in **Mode 4 Performance**, holding `T1–T4` for momentary stutters keeps `F1–F4` live so you can sweep the DJ Filter and Tape Delay simultaneously over ratchets.)*

---

## 2. Complete 4-Mode Control Reference

### 2.1 Mode 1: Pattern Mode (Side LED 1) — Euclidean Rhythm & Polyrhythm

| Gesture | Target Parameter | Range / Behavior | Track LED Meter (`600 ms`) |
|---|---|---|---|
| **Unshifted `F1–F4`** | **Track 1–4 Euclidean Hits (`k`)** | `0 .. n` hits distributed via Bjorklund `E(k, n)` | `BASE2_COUNTER` (`k & 0x0F`) |
| **Tap `T1–T4`** | **Toggle Track 1–4 Mute** | Toggles mute on release when no fader moved while held | Idle Track LEDs (`8` muted) |
| **Tap `RWD / FWD`** | **Global 16th-Note Swing** | `-2% / +2%` per tap (`50%` straight .. `66%` triplet) | `UNBALANCED_BAR` (`900 ms`) |
| **`Hold T_x + F1`** | **Track `x` Clock Speed** | `0.25x` (`1/4`), `0.5x` (`1/8`), `1x` (`1/16` default), `2x` (`1/32` rolls) | `DISCRETE_SELECT` (LED `1..4`) |
| **`Hold T_x + F2`** | **Track `x` Start-Beat Rotation** | `0 .. n-1` step shift (`0..31`) | `BASE2_COUNTER` (`rot & 0x0F`) |
| **`Hold T_x + F3`** | **Track `x` Dilla Micro-Timing** | `-35 ms` (rushed) .. `0 ms` (`[0.42..0.58]` detent) .. `+35 ms` (dragged) | `MIDDLE_BALANCED` |
| **`Hold T_x + F4`** | **Track `x` Step Length (`n`)** | `1 .. 32` steps (default `16`; `16 & 32` display as `0000`, `1 & 17` as `0001`) | `BASE2_COUNTER` (`n & 0x0F`) |

### 2.2 Mode 2: Track Mode (Side LED 2) — 4-Channel Mixer, Sends, Probability & Tempo

| Gesture | Target Parameter | Range / Behavior | Track LED Meter (`600 ms`) |
|---|---|---|---|
| **Unshifted `F1–F4`** | **Track 1–4 Mix Level** | `0% .. 115%` voice channel level (`~87%` unity default) | `UNBALANCED_BAR` |
| **Tap `T1–T4`** | **Toggle Track 1–4 Mute** | Toggles mute on release when no fader moved while held | Idle Track LEDs |
| **Tap / Hold `RWD / FWD`** | **Master Tempo (`BPM`)** | Tap: `-2 / +2 BPM`; Hold `>= 350 ms`: `-5 / +5 BPM` every `200 ms` (`60..180 BPM`) | `MIDDLE_BALANCED` (`120 BPM` center) |
| **`Hold T_x + F1`** | **Track `x` DJ Tone Filter** | Warm Low-Pass (`< 0.45`) .. Flat Bypass (`[0.45..0.55]`) .. Crisp High-Pass (`> 0.55`) | `MIDDLE_BALANCED` |
| **`Hold T_x + F2`** | **Track `x` Stereo Pan** | Hard Left .. Center (`[0.45..0.55]` detent) .. Hard Right | `MIDDLE_BALANCED` |
| **`Hold T_x + F3`** | **Track `x` Dub Delay Send** | `0% .. 100%` send into the Tape Ping-Pong Delay (`Kick` defaults to `0%` dry) | `UNBALANCED_BAR` |
| **`Hold T_x + F4`** | **Track `x` Hit Probability** | `0% .. 100%` per-step trigger chance (default `100%`) | `UNBALANCED_BAR` |

### 2.3 Mode 3: Sound Edit Mode (Side LED 3) — Synth Macros, Live Pads, Kits & 4x4 Voices

| Gesture | Target Parameter | Range / Behavior | Track LED Meter (`600 ms`) |
|---|---|---|---|
| **Unshifted `F1–F4`** | **Voice 1–4 Character Macro** | Sweeps `Kick Decay+Drive`, `Rim↔Snare`, `Closed↔Open Hat`, `FM Perc Timbre` while preserving custom `T_x + F1` Pitch edits | `UNBALANCED_BAR` |
| **Tap `T1–T4`** | **Live Drum Audition** | Zero-latency velocity-accented finger-drum trigger on press (stopped or playing) | Trigger Flash (`255`) |
| **Tap `RWD / FWD`** | **Cycle 4 Synth Kit Presets** | `Kit 1` (`808/909`) ↔ `Kit 2` (`Dry Woody`) ↔ `Kit 3` (`Industrial FM`) ↔ `Kit 4` (`Lo-Fi Dub`) | `DISCRETE_SELECT` (LED `1..4`) |
| **`Hold T1 + F1..F4`** | **Kick / Sub Synthesis** | `F1`: Sub Tune (`32–72 Hz`) · `F2`: Sub Decay · `F3`: Pitch-Sweep Punch · `F4`: Wavefold Drive | `F1`: `MIDDLE_BALANCED`<br>`F2–F4`: `UNBALANCED` |
| **`Hold T2 + F1..F4`** | **Snare / Rim Synthesis** | `F1`: Shell Pitch · `F2`: Rim ↔ Snare Morph · `F3`: Wire Decay · `F4`: Wire TPT SVF Cutoff | `F1`: `MIDDLE_BALANCED`<br>`F2–F4`: `UNBALANCED` |
| **`Hold T3 + F1..F4`** | **Metallic Hat Synthesis** | `F1`: 6-Osc Cluster Tune · `F2`: Closed ↔ Open Decay · `F3`: HP SVF Cutoff · `F4`: Metal ↔ Noise | `F1`: `MIDDLE_BALANCED`<br>`F2–F4`: `UNBALANCED` |
| **`Hold T4 + F1..F4`** | **2-Op FM Perc Synthesis** | `F1`: Carrier Pitch · `F2`: Modulator Ratio · `F3`: Amp/Pitch Decay · `F4`: FM & Wavefold Depth | `F1`: `MIDDLE_BALANCED`<br>`F2–F4`: `UNBALANCED` |

### 2.4 Mode 4: Performance Mode (Side LED 4) — Master FX, Tape-Gliding Delay & Stutters

| Gesture | Master FX Processor | Live Performance Behavior | Track LED Meter |
|---|---|---|---|
| **`F1`** | **DJ Resonant TPT SVF Isolator** | `< 0.45`: Low-Pass (`20 kHz → 60 Hz`) · `[0.45..0.55]`: Center Bypass · `> 0.55`: High-Pass (`30 Hz → 12 kHz`) | `MIDDLE_BALANCED` |
| **`F2`** | **Bitcrusher & ZOH Decimator** | `0.0`: Clean 48 kHz 16-bit → `1.0`: gritty 16x ZOH sample-rate decimation + bit reduction | `UNBALANCED_BAR` |
| **`F3`** | **Tape-Gliding Dub Delay Time** | **Continuous Doppler Tape Pitch-Bend** while moving `F3`; `120 ms` after stopping, smoothly glides to lock into **`1/32`, `1/16`, `dotted 1/16`, `1/8`, `dotted 1/8`, or `1/4`**! | Drag: `UNBALANCED_BAR`<br>Lock: `DISCRETE` (6-div) |
| **`F4`** | **Dub Ping-Pong Wet & Feedback** | Sweeps stereo ping-pong echo level (`0..100%`) and analog-damped feedback (`25%..82%`) | `UNBALANCED_BAR` |
| **`Hold T1–T4`** | **Momentary Stutter Punch-In** | `T1 = 1/8`, `T2 = 1/16`, `T3 = 1/32`, `T4 = 1/64` beat-repeat (`F1–F4` remain live for FX sweeps!) | Rapid Stutter Pulse |
| **`Hold RWD / FWD`** | **Momentary DJ Tempo Bend** | Temporary `-6%` (`RWD`) or `+6%` (`FWD`) clock nudge while held; releases cleanly to nominal BPM | Beat Chaser |

### 2.5 Playback Dub Siren (`Hold Function ••` While Playing)

While the sequencer is **playing**, the `3.0 s` shutdown timer is disabled. Holding **`Function (••)`** (`>= 200 ms` or immediately upon moving `F1–F4`) triggers a classic Jamaican sound-system **Dub Siren** layered over the beat (`F1–F3` settings are remembered across blasts!):

| Gesture (While Playing) | Siren Parameter | Range / Behavior |
|---|---|---|
| **`Hold Function (••)`** | **Siren Gate On** | Smooth clickless `2.5 ms` attack / `8.3 ms` release oscillator blast |
| **`Function + F1`** | **Siren Base Pitch** | `140 Hz` low horn .. `1,120 Hz` piercing dub siren |
| **`Function + F2`** | **Siren LFO Rate** | `0.4 Hz` slow rise/fall .. `18.4 Hz` rapid warble |
| **`Function + F3`** | **Siren LFO Shape & Depth** | Left (`< 0.42`): Smooth Wail · Center (`[0.42..0.58]`): Steady Horn · Right (`> 0.58`): 2-Tone Square Siren |
| **`Function + F4`** | **Siren Dub Echo Throw** | Feeds the siren into the Tape Ping-Pong Delay with a temporary feedback swell that decays smoothly over `~1.5 s` after release |

---

## 3. Dual 4-LED Visual Display System

### 3.1 The 4 Side LEDs (`Play 1..4`) — System HUD, Mode Toast, Beat & Battery
The 4 Side LEDs never lock onto a static mode indicator—they follow a clear 5-level priority stack:
1. **Master Volume HUD (`800 ms` on `Vol-` / `Vol+`):** Progressive vertical bar (`Side LED 1 → 4`) showing master output volume.
2. **Mode Change Toast (`1.68 s` on `Function` tap):** Lights the newly selected Mode's Side LED (`1..4`) solid (`255`) for `1.2 s`, then **blinks twice** (`2x` `120 ms` pulses) before returning to the beat chaser or stopped display.
3. **Playback 4-Beat Chaser (While Playing):** Steps cleanly across `Side LED 1 → 2 → 3 → 4` on quarter notes (`255` on Beat 1 downbeat, `180` on Beats 2–4; background floor rises to `16` when locked to external USB-C 24 PPQN MIDI clock).
4. **Stopped Battery Level Bar (`3.0 s` on Boot, Stop, or USB Plug/Unplug):** Displays LiPo battery charge (`AIN4`) as a 4-LED bar, gently pulsing the top active LED while charging over USB-C.
5. **Stopped Resting State:** Quiet dim glow (`48 / 255`) on the active Mode's Side LED (`1..4`).

### 3.2 The 4 Track LEDs (`Track 1..4`) — 4 Standard Parameter Meters & Live Triggers
Whenever you move a slider (`600 ms` hold-off) or tap `RWD / FWD` (`900 ms` hold-off), the **4 Track LEDs** render one of 4 standardized parameter widgets before returning to live drum trigger flashes (`255` accent / `200` normal, `32` idle, `8` muted):
1. **`UNBALANCED_BAR`:** Smooth `0..1020` brightness fill from `Track LED 1 → 4` (Levels, Swing, Sends, Probability, Voice Macros, Bitcrusher, Delay Wet).
2. **`MIDDLE_BALANCED`:** Center pair **`Track LED 2 + 3`** glow symmetrically (`160`) at exact neutral/detent, leaning left (`LED 2 → 1`) for negative/LP/Left or right (`LED 3 → 4`) for positive/HP/Right (Dilla Offset, BPM around `120`, Tone, Pan, Voice Pitch, DJ Filter).
3. **`BASE2_COUNTER` (Mod-16 4-Bit Binary `MSB..LSB`):** Displays integer counts (`0..32`) using `value & 0x0F`:
   - `0`, `16`, and `32` → `0000` (all 4 Track LEDs off)
   - `1` and `17` → `0001` · `2` and `18` → `0010` · `4` and `20` → `0100` · `8` and `24` → `1000` · `15` and `31` → `1111`
4. **`DISCRETE_SELECT`:** Crisp stepped indicator for Track Clock Speed (`1..4`), Synth Kit (`1..4`), and the 6 locked Tape Delay Tempo Divisions (`1/32`, `1/16`, `dot 1/16`, `1/8`, `dot 1/8`, `1/4`).

---

## 4. Step-by-Step Musical Workflows

1. **Polymetric & Multi-Speed Grooves (Mode 1 — Pattern):** Press `Play`. Set `F1–F4` to dial Euclidean hit counts. Hold `T3` (Hi-Hat) + raise `F1` to the top (`2x` = 32nd-note trap hat rolls) and `T1` (Kick) + `F4` to `32` steps for a 2-bar evolving kick phrase. Hold `T2` + move `F3` slightly right of center for a laid-back `+18 ms` J Dilla snare pocket, and tap `FWD` to add Global Swing.
2. **Mixing, Tone Sculpting & Generative Probability (Mode 2 — Track):** Short-press `Function` once (`Side LED 2` lights + blinks twice). Balance voice levels on `F1–F4`, tap/hold `RWD / FWD` to dial Master BPM, hold `T3 + F2` to pan Hi-Hats right, and hold `T4 + F4` (~60%) so FM Perc hits evolve organically.
3. **Finger-Drumming, Kit Swapping & Deep Sound Design (Mode 3 — Sound Edit):** Short-press `Function` again (`Side LED 3`). Tap `RWD / FWD` to audition the 4 Synth Kits, tap `T1–T4` like live drum pads, use `F1–F4` for instant character morphs, or hold `T1..T4 + F1..F4` to edit all 16 synth parameters.
4. **Tape Delay Pitch-Dives, Stutters & Dub Siren Drops (Mode 4 — Performance):** Short-press `Function` (`Side LED 4`). Raise `F4` for Dub Ping-Pong repeats, then sweep `F3` rapidly down and up—hear the active echoes bend in pitch like a physical tape machine before gliding into tempo-locked sync when you let go! Hold `T3` (`1/32` stutter) while sweeping `F1` (DJ Filter), or hold `Function` during playback and work `F1–F4` to launch a wailing Dub Siren echo throw over the drop!

---

## 5. USB-C MIDI Implementation Chart

The SP-1 enumerates as a class-compliant USB-C Full-Speed Composite Device (`VID:PID = 0x1209:0x0001`, `"Teenage Engineering SP-1 MIDI Controller"` + CDC ACM debug console).

- **24 PPQN Clock Master & Automatic External Follower:** Emits `0xF8` Timing Clock, `0xFA` Start, and `0xFC` Stop in Internal mode; automatically locks to incoming USB-C `0xFA`/`0xFB`/`0xF8` clock with IIR jitter smoothing and seamless `500 ms` failover back to internal master clock.
- **Outbound General MIDI Drum Notes (`0x90` / `0x80`, Channel 1):** `Track 1 = 36 (C1 Kick)`, `Track 2 = 38 (D1 Snare)`, `Track 3 = 42 (F#1 Hat)`, `Track 4 = 45 (A1 Perc)` (`96` normal / `120` accent / `110` live pad audition, `<= 50 ms` gate).
- **Outbound Fader CCs (`0xB0`, Channel 1):** `Fader 1..4` emit `CC 1..4` (`0..127`, 8-LSB hardware deadband).

---

## 6. Hardware Troubleshooting & Safety Notes

- **Battery Power-On (`1.5 s`) & Stopped Power-Off (`3.0 s`):** Waking from `SYSTEM_OFF` on battery requires holding `Function (••)` for `1,500 ms` (`Side LEDs 1..4` fill progressively). To power off cleanly (`< 5 µA` `SYSTEM_OFF`), **press `Play` to stop playback first**, then hold `Function (••)` for **3 seconds** (holding `Function` while playing triggers the Dub Siren instead of powering off!).
- **`AIN0` Resistor Ladder Safety:** `Play` shares `AIN0` (`P0.02`) with `T1..T4` (`1587–1592 mV` masks individual track buttons), so `Play` is strictly standalone Transport Start/Stop and never participates in button or fader chords.
- **`T1+T4` USB-C Bootloader Recovery:** Power off, hold **`T1 + T4`** while plugging in USB-C (all 4 Track LEDs light solid), download `zephyr.bin` from [**GitHub Releases**](https://github.com/davidlwarner/sp1-drum/releases) or the latest `sp1-drum-firmware` CI artifact on [**GitHub Actions**](https://github.com/davidlwarner/sp1-drum/actions/workflows/ci.yml), and flash via Web Serial at [`https://solderless.engineering`](https://solderless.engineering).
