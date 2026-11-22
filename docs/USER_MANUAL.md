# SP-1 Euclidean Drum Machine — Musician's User Manual & Hardware Guide

`sp1-drum` transforms the **Teenage Engineering Stem Player (SP-1)** into a standalone 4-track Bjorklund Euclidean drum machine, 4-voice C11 drum synthesizer, and stereo performance effects processor running at 48 kHz on the Nordic nRF52840 (64 MHz ARM Cortex-M4F).

---

## 1. Hardware Layout & Screenless Design Philosophy

The SP-1 has **no screen**. Every parameter is controlled through the 4 analog fader sliders, 10 tactile buttons, and 8 monochrome 1024 µs PWM LEDs (`4 Track LEDs` at the corners + `4 Play LEDs` around the center cross):

- **4x Analog Fader Sliders (`Fader 1..4`):** Sampled at 12-bit resolution (`0..4080`) with an 8-LSB hardware noise deadband (`~0.2%`).
- **4x Track Buttons (`T1..T4`) + `Play` Button (`AIN0` Resistor Ladder):**
  - `T1` = **Track 1 (Kick / Sub)**
  - `T2` = **Track 2 (Snare / Rimshot Morph)**
  - `T3` = **Track 3 (6-Oscillator Metallic Hi-Hat)**
  - `T4` = **Track 4 (2-Op FM / West-Coast Wavefold Perc)**
  - `Play` = **Master Transport Start / Stop**
- **4x Transport & Volume Buttons (`AIN1` Resistor Ladder):**
  - `RWD (<<)` = **Fine Tempo Down (`-2 BPM` tap) / Euclidean Rotation Shift Modifier**
  - `FWD (>>)` = **Fine Tempo Up (`+2 BPM` tap) / Polymeter Step Length Shift Modifier**
  - `Vol- (-)` = **Master Volume Down (`-8` tap) / Performance FX Chord Modifier (`Vol- + T1`)**
  - `Vol+ (+)` = **Master Volume Up (`+8` tap)**
- **1x `Function (••)` Button (`GPIO P0.27`):**
  - Primary shift modifier for **J Dilla Micro-Timing Nudge (`±35 ms`)**, **Coarse Tempo (`Function + RWD/FWD`, `±10 BPM`)**, **Global Swing (`Function + Vol-/Vol+`, `50%..66%`)**, **Pocket-Safe Power-On (`1.5-second hold` on battery)**, and **Graceful Power-Off (`3-second hold`)**.

### Anti-Clobber Fader Latching
Because the 4 faders serve multiple pages (Euclidean Pulse Density, Dilla Micro-Timing, Rotation, Polymeter Step Length, Voice Timbre/Morph, and Master Performance FX), `sp1-drum` latches every fader's raw 12-bit SAADC position whenever you press or release a modifier key, hold/release `T1..T4` in Normal Mode, or toggle Performance FX Mode. A fader remains disengaged on the new page until you physically move it past the **`16-LSB` engagement threshold (`SP1_UI_FADER_ENGAGE_DELTA`)**, preventing parked faders from overwriting your rhythms when you release a shift or track button.

---

## 2. Complete Gesture & Control Reference

### 2.1 Normal Pattern Mode (Default Page)

| Gesture | Action | Range / Behavior |
|---|---|---|
| **`Play`** | Toggle Sequencer Transport | Starts or stops playback (`0xFA` Start / `0xFC` Stop emitted on USB-C MIDI) |
| **`Fader 1..4`** | Track 1..4 Euclidean Pulses (`k`) | `0..n` active hits distributed evenly across track step length `n` via Bjorklund's algorithm `E(k, n)` |
| **`Tap T1..T4`** | Toggle Track 1..4 Mute | Toggles mute on release when no fader moved while held (mutes internal synthesis & outbound MIDI notes) |
| **`Tap RWD / FWD`** | Fine Master Tempo | `-2 BPM` / `+2 BPM` per unshifted tap (clamped to `60.00..180.00 BPM`, default `120.00 BPM`; suppressed if a fader or track reset is used while held) |
| **`Vol-` / `Vol+`** | Master Output Volume | `-8` / `+8` per unshifted tap (clamped to `0..127`, default `64` / medium) across `CS42L42` headphone DAC & `TAS2505` speaker amp |

### 2.2 `Function (••)` Shift Mode — J Dilla Micro-Timing, Coarse Tempo, Swing & Power

| Gesture | Action | Range / Behavior |
|---|---|---|
| **`Function + Fader 1..4`** | Per-Track Dilla Micro-Timing Nudge | `-35.0 ms` (early/rushed) .. `0.0 ms` (`[0.42..0.58]` center grid-lock deadzone) .. `+35.0 ms` (late/dragged) |
| **`Function + T1..T4`** | Reset Track 1..4 Dilla Nudge | Instantly snaps that track's micro-timing back to `0.0 ms` and re-latches its fader |
| **`Function + RWD / FWD`** | Coarse Master Tempo | `-10 BPM` / `+10 BPM` per press (`60.00..180.00 BPM`) |
| **`Function + Vol- / Vol+`** | Global 16th-Note Swing | `-2%` / `+2%` per press (`50%` straight 16ths .. `66%` triplet swing) |
| **`Hold Function (••) 1.5 s` (from Off)** | Pocket-Safe Battery Power-On | Animates a progressive 4-LED fill across `Play 1..4`; releasing before `1,500 ms` blanks LEDs and returns immediately to `SYSTEM_OFF` |
| **`Hold Function (••) 3 s` (while On)** | Graceful Hardware Shutdown | Fades LEDs, stops I2S0, mutes/resets `TAS2505` & `CS42L42`, disables `3.072 MHz` oscillator, enters `SYSTEM_OFF` |

### 2.3 `RWD (<<)` Shift Mode — Euclidean Pattern Rotation

| Gesture | Action | Range / Behavior |
|---|---|---|
| **`RWD + Fader 1..4`** | Track 1..4 Euclidean Rotation (`rot`) | Rotates pattern offset across `0 .. n-1` steps (suppresses the `-2 BPM` tap on release) |
| **`RWD + T1..T4`** | Reset Track 1..4 Rotation | Snaps track rotation back to `0` (downbeat aligned), re-latches fader, and suppresses `-2 BPM` tap |

### 2.4 `FWD (>>)` Shift Mode — Polymeter Step Length

| Gesture | Action | Range / Behavior |
|---|---|---|
| **`FWD + Fader 1..4`** | Track 1..4 Step Length (`n`) | Sets independent loop length `1..32` steps per track for polymetric phasing (suppresses the `+2 BPM` tap on release) |
| **`FWD + T1..T4`** | Reset Track 1..4 Step Length | Resets track loop length to `16` steps, re-latches fader, and suppresses `+2 BPM` tap |

### 2.5 `Hold T1..T4 + Fader 1..4` — Voice Timbre & Rimshot-Snare Morph

In Normal Pattern Mode (when `Play` is not held and Performance FX Mode is off), hold **`T1..T4`** and move its corresponding **`Fader 1..4`** to sculpt each drum synthesis voice in real time. Moving a fader while holding a track button automatically suppresses the release mute toggle and re-latches the fader on release so Euclidean pulse counts never jump:

| Gesture | Voice Target | Sound Design Range |
|---|---|---|
| **`Hold T1 + Fader 1`** | **Track 1: Kick / Sub** | **Decay & Drive (`0.0..1.0`):** `0.0` = tight punchy sub kick (`0.20` drive) → `1.0` = long booming saturated 808 sub (`0.95` cubic/wavefold drive) |
| **`Hold T2 + Fader 2`** | **Track 2: Snare / Rimshot** | **Continuous Rimshot ↔ Snare Morph (`0.0..1.0`):** `0.0` = tight, woody 808/909 **Rimshot** (`~185 Hz` + `~335–465 Hz` dual body/rim knock, minimal wires) → `1.0` = crisp **Snare** with bright TPT SVF snare wires (`default 0.65`) |
| **`Hold T3 + Fader 3`** | **Track 3: Metallic Hi-Hat** | **Closed ↔ Open Decay Morph (`0.0..1.0`):** `0.0` = tight **Closed Hat** tick → `1.0` = shimmering **Open Hat** decay (`default 0.20`) |
| **`Hold T4 + Fader 4`** | **Track 4: 2-Op FM Perc** | **FM Index & Wavefold Timbre (`0.0..1.0`):** `0.0` = hollow woody 2-Op FM conga/clave → `1.0` = aggressive West-Coast wavefolded metallic perc (`default 0.45`) |

### 2.6 Performance FX Mode (Latched Master FX Bus + Momentary Ratchets)

Toggle **Performance FX Mode** on or off at any time using either hardware-safe chord:
- **`Function (••) + T1 + T4`**, or
- **`Vol- (-) + T1`**

When Performance FX Mode is active, the faders and track buttons control the stereo master FX bus:

| Control (FX Mode Active) | Master FX Processor | Parameter Behavior |
|---|---|---|
| **`Fader 1`** | **DJ Resonant Cytomic TPT SVF Isolator** | `< 0.45`: Low-Pass sweep (`20 kHz → 60 Hz`)<br>`[0.45..0.55]`: Center bypass deadzone<br>`> 0.55`: High-Pass sweep (`30 Hz → 12 kHz`) |
| **`Fader 2`** | **Latched Beat-Repeat / Stutter Looper** | `< 0.15`: Off<br>`0.15..0.32`: `1/4` repeat<br>`0.32..0.50`: `1/8` repeat<br>`0.50..0.68`: `1/16` repeat<br>`0.68..0.84`: `1/32` ratchet<br>`>= 0.84`: `1/64` fast buzz ratchet |
| **`Fader 3`** | **Bitcrusher & ZOH Decimator** | `0.0`: Clean 48 kHz 16-bit audio → `1.0`: 16x zero-order-hold sample-rate decimation + bit crushing |
| **`Fader 4`** | **Selective Dub Ping-Pong Delay** | Controls stereo ping-pong wet level (`0.0..1.0`) and feedback (`0.25..0.80`). Fed selectively by **Track 2 (Snare/Rimshot)** and **Track 4 (FM Perc)** so **Track 1 (Kick)** stays 100% dry and punchy |
| **`Hold T1 / T2 / T3 / T4`** | **Momentary Stutter Punch-In** | Holds tempo-synced stutter loops (`T1 = 1/8`, `T2 = 1/16`, `T3 = 1/32`, `T4 = 1/64`) with a 64-frame clickless boundary crossfade; releasing returns to `Fader 2`'s setting |

---

## 3. 8-Channel PWM LED Visual Feedback Guide

### 3.1 Normal Mode LED Readout
- **Track LEDs (`1..4`):**
  - **Muted Track:** Dim steady glow (`8 / 255`).
  - **Active Idle Track (`k > 0`):** Low steady glow (`32 / 255`).
  - **Empty Track (`k == 0`, unmuted):** Off (`0 / 255`).
  - **Step Trigger Hit:** Flashes to **`255 / 255`** on downbeat accents (`200 / 255` on normal steps) with an `~85 ms` smooth PWM decay envelope.
- **Play / Center LEDs (`1..4`):**
  - **Internal Clock Running:** Quarter-note beat chaser rotating around `Play 1 → 2 → 3 → 4` (`255 / 255` on Beat 1 downbeat, `180 / 255` on Beats 2–4, background LEDs at `0`).
  - **External USB-C 24 PPQN MIDI Sync Locked:** Background floor across all 4 Play LEDs rises to `16 / 255` while running (`80 / 255` on `Play 1` when stopped vs. `40 / 255` in internal mode), giving instant visual confirmation of external clock lock.

### 3.2 Shift Overlay LED Meters (`Function`, `RWD`, `FWD`)
Whenever you hold `Function`, `RWD`, or `FWD`, the **Track LEDs** highlight the most recently edited track at full brightness (`255 / 255`, other 3 tracks at `40 / 255`), and the **4 Play LEDs** transform into a precision parameter meter:

1. **Bipolar Dilla Micro-Timing Polarity Meter (`Function` Held):**
   - **Exact `0.0 ms` Grid Lock (`[0.42..0.58]` Center Deadzone):** The symmetric inner pair **`Play 2 + Play 3`** illuminate together at `160 / 255` (`Play 1` and `Play 4` off).
   - **Rushed / Early (`-0.1 ms .. -35.0 ms`):** Left LEDs illuminate progressively from center outward (**`Play 2`** fills to `255`, then **`Play 1`** ramps `0 → 255` as nudge approaches `-35 ms`).
   - **Dragged / Late (`+0.1 ms .. +35.0 ms`):** Right LEDs illuminate progressively from center outward (**`Play 3`** fills to `255`, then **`Play 4`** ramps `0 → 255` as nudge approaches `+35 ms`).
2. **Polymeter Step Length & Rotation 4-LED Bar Meter (`FWD` or `RWD` Held):**
   - `Play 1 → Play 2 → Play 3 → Play 4` fill sequentially like a 4-segment VU bar (`0..1020` total brightness units, `32` units per step), giving an exact visual readout of `1..32` polymeter steps (`FWD`) or `0..n-1` rotation offset (`RWD`).

---

## 4. Step-by-Step Musical Workflows

### Workflow 1: Building a Polymetric Euclidean Groove
1. Press **`Play`** if the sequencer is stopped.
2. In Normal Mode, set **`Fader 1` (Kick)** to ~30% (`E(5, 16)`), **`Fader 2` (Snare/Rim)** to ~20% (`E(3, 16)`), and **`Fader 3` (Hi-Hat)** to ~70% (`E(11, 16)`).
3. Hold **`RWD (<<)`** and move **`Fader 2`** to ~25% (`rot = 4`) to rotate the Snare/Rimshot onto the backbeat.
4. Hold **`FWD (>>)`** and move **`Fader 4` (FM Perc)** to ~38% (`12` steps instead of `16`), then release `FWD` and raise **`Fader 4`** to ~40% (`E(5, 12)`). Track 4 now cycles every 12 sixteenth-notes against the 16-step main groove, creating an evolving 3-bar polymeter.

### Workflow 2: Dialing in a J Dilla "Drunk" Micro-Timing Pocket
1. Hold **`Function (••)`** and tap **`Vol+ (+)`** four times to set Global Swing to **`58%`**.
2. Still holding **`Function (••)`**, pull **`Fader 3` (Hi-Hat)** slightly below the center deadzone until **`Play 2`** glows on the left (`~ -12 ms` rushed hi-hats).
3. Move **`Fader 2` (Snare/Rimshot)** slightly above the center deadzone until **`Play 3`** and a hint of **`Play 4`** glow on the right (`~ +18 ms` laid-back snare pocket).
4. Keep **`Fader 1` (Kick)** inside the center deadzone (`Play 2 + Play 3` lit symmetrically) to anchor the downbeat. To snap any track back to the grid instantly during a live set, tap **`Function + T1..T4`**.

### Workflow 3: Live Rimshot-to-Snare & Voice Morphing
1. Hold **`T2`** and pull **`Fader 2`** all the way down (`0.0`): Track 2 becomes a dry, woody 808/909 **Rimshot** ideal for minimal intros and verses. Releasing `T2` keeps Track 2 unmuted and preserves its Euclidean pulse count.
2. As the arrangement builds, hold **`T2`** and sweep **`Fader 2`** upward toward `0.85..1.0`, hold **`T3`** to open **`Fader 3` (Hi-Hat decay)** from `0.15` (tight closed hat) to `0.70` (open hat), and hold **`T4`** while pushing **`Fader 4` (FM Perc)** into West-Coast wavefold territory.

### Workflow 4: Performance FX Transitions & Momentary Ratchet Fills
1. Press **`Vol- + T1`** (or **`Function + T1 + T4`**) to latch **Performance FX Mode**.
2. Raise **`Fader 4`** to ~45% to throw stereo Dub Ping-Pong Delay tails onto the Snare/Rimshot and FM Perc while the Kick stays solid and dry.
3. At the end of a 4-bar phrase, hold **`T2` (`1/16`)** then roll onto **`T3` (`1/32`)** and **`T4` (`1/64`)** for an instant tempo-locked beat-repeat build, while sweeping **`Fader 1` (DJ Resonant SVF)** above center (`> 0.55`) to high-pass the drop.
4. Release `T4`, return **`Fader 1`** to the center detent (`0.45..0.55` bypass), and tap **`Vol- + T1`** to return to Pattern Mode.

---

## 5. USB-C MIDI Implementation Chart

The SP-1 enumerates as a class-compliant USB-C Full-Speed Composite Device (`VID:PID = 0x1209:0x0001`, `"Teenage Engineering SP-1 MIDI Controller"` + CDC ACM debug console).

### 5.1 24 PPQN Clock Master & Automatic External Follower
- **Internal Master Mode (Default):** Emits sample-accurate `0xF8` Timing Clock ticks at 24 PPQN (`24` ticks per quarter note), `0xFA` Start, and `0xFC` Stop over USB-C MIDI.
- **External Follower Mode (Auto-Detect):** Receiving an incoming `0xFA` (Start), `0xFB` (Continue), or `0xF8` (Clock) message over USB-C automatically switches the sequencer to external sync with a jitter-smoothing IIR clock tracker (`7/8` history, `1/8` instantaneous period).
- **Failover Recovery:** If external `0xF8` clock ticks stop arriving for more than **`500 ms`** (`24,000` sample frames @ 48 kHz), the engine seamlessly fails over to its internal master clock at the last locked tempo without dropping transport.

### 5.2 Outbound General MIDI Drum Notes (Channel 1 / `0x90`, `0x80`)

| Track | Voice | GM MIDI Note | Note Name | Normal Velocity | Downbeat Accent Velocity | Max Gate Length |
|---:|---|---:|---|---:|---:|---:|
| **Track 1** | Kick / Sub | **`36`** | `C1` | `96` | `120` | `50 ms` (`2,400` frames) |
| **Track 2** | Snare / Rimshot | **`38`** | `D1` | `96` | `120` | `50 ms` (`2,400` frames) |
| **Track 3** | Metallic Hi-Hat | **`42`** | `F#1` | `96` | `120` | `50 ms` (`2,400` frames) |
| **Track 4** | 2-Op FM / Wavefold Perc | **`45`** | `A1` | `96` | `120` | `50 ms` (`2,400` frames) |

### 5.3 Outbound Fader Continuous Controllers (Channel 1 / `0xB0`)

| Physical Control | SAADC Channel / Pin | MIDI CC Number | Value Range |
|---|---|---|---|
| **Fader 1** | `AIN3` (`P0.05`) | **`CC 1`** | `0..127` (8-LSB hardware deadband) |
| **Fader 2** | `AIN6` (`P0.30`) | **`CC 2`** | `0..127` (8-LSB hardware deadband) |
| **Fader 3** | `AIN2` (`P0.04`) | **`CC 3`** | `0..127` (8-LSB hardware deadband) |
| **Fader 4** | `AIN7` (`P0.31`) | **`CC 4`** | `0..127` (8-LSB hardware deadband) |

---

## 6. Hardware Troubleshooting & Safety Notes

### 6.1 Standalone Battery vs. USB-C Operation & 1.5 s Power-On Delay
- **USB-C Power:** When USB-C VBUS is present, `sp1-drum` transitions immediately from `STATE_IDLE` to `STATE_RUNNING` and starts its 48 kHz I2S0 EasyDMA audio engine (playing via `CS42L42` headphones or `TAS2505` internal speaker).
- **Battery Power-On (`1.5 s` Pocket-Safe Hold):** When waking from `SYSTEM_OFF` on battery via `Function (••)` (`P0.27`), `STATE_IDLE` inspects and clears `NRF_POWER->RESETREAS` and requires `Function` to remain held continuously for **`1,500 ms`** while animating a progressive 4-LED fill across `Play 1..4`. If `Function` is released early, all LEDs blank and the unit immediately re-enters `SYSTEM_OFF` so accidental pocket presses never drain the battery or trigger audio.
- **Graceful Power-Off (`3 s` Hold):** To power down cleanly on battery, **hold `Function (••)` for 3 seconds**. The firmware fades out the LEDs, stops I2S0, mutes and asserts hardware reset on both `TAS2505` (`P0.09`) and `CS42L42` (`P0.15`), disables the external `3.072 MHz` audio oscillator (`P0.13`), blanks all 8 PWM LEDs, waits for button release, and enters nRF52840 `SYSTEM_OFF` deep sleep (`< 5 µA`).

### 6.2 Why `Play` Is Never Used as a Shift Modifier (`AIN0` Ladder Physics)
On the SP-1 PCB, `T1`, `T2`, `T3`, `T4`, and `Play` share a single analog resistor ladder connected to `AIN0` (`P0.02`). Pressing `Play` pulls the ladder to `1587–1592 mV` (nearly identical to `T1+T2+T3+T4 = 1584 mV`), physically masking individual `T1..T4` button presses. For 100% reliable hardware decoding, `sp1-drum` enforces `SP1_BTN_VALID_SHIFT_MASK` in [`src/hal/hal_controls.h`](../src/hal/hal_controls.h): `Play` only toggles transport when no shift key is held, and all track/fader chords use `Function` (`GPIO P0.27`) or Transport/Volume buttons (`AIN1` / `P0.03`).

### 6.3 `TAS2505` Speaker Amp & SWD Debug Cable Caveat
If you attach a full 10-pin SWD debug probe ribbon cable to the internal header, the `TAS2505` speaker amplifier (`I2C0` address `0x18`, reset on `P0.09` / `NFC1`) may NACK I2C configuration transactions. `sp1_hal_audio_init()` treats `TAS2505` I2C errors as non-fatal so headphone output via `CS42L42` (`0x48`) and sequencer operation continue normally while debugging over SWD.

### 6.4 `T1+T4` USB-C Emergency Bootloader Recovery
The Teenage Engineering factory bootloader resides in protected flash (`0x00000..0x1FFFF`), and `sp1-drum` always links into `slot0_partition` (`0x20000..0xFEFFF`). You can always recover or re-flash the unit without an SWD programmer:
1. Power off the SP-1 (hold `Function ••` for 3 seconds, or unplug USB-C if uncharged).
2. **Hold `Track 1 (T1)` + `Track 4 (T4)`** simultaneously.
3. While holding `T1+T4`, **plug in the USB-C cable** connected to your computer.
4. All 4 corner Track LEDs will light solid, confirming the hardware bootloader is active.
5. Open [`https://solderless.engineering`](https://solderless.engineering) in Chrome or Edge, connect via Web Serial, and flash `zephyr.bin` (or reinstall stock firmware).
