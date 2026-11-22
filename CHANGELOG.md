# Changelog

All notable changes to `sp1-drum` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Hardened GitHub Actions CI (`.github/workflows/ci.yml`) running the 15-suite CTest harness across `gcc`, `clang`, and `gcc` with AddressSanitizer + UndefinedBehaviorSanitizer (`-Wall -Wextra -Werror`).
- Cached Zephyr `v4.3.1` + minimal Zephyr SDK `0.17.4` (`arm-zephyr-eabi`) ARM Cortex-M4F CI build enforcing zero project compiler warnings and a 4-stage Anti-Brick & Memory Safety Gate (SRAM/Flash limits, `slot0_partition` ELF `LOAD` bounds, `zephyr.bin` vector table MSP/Thumb `Reset_Handler` checks, zero `nrf_nvmc_*`/`nrfx_nvmc_*` symbols, and strong `k_sys_fatal_error_handler` verification).
- Automated SemVer GitHub Releases workflow (`.github/workflows/release.yml`) and categorized release-notes configuration (`.github/release.yml` grouping `feat`, `fix`, `perf`, and `docs`) attaching `zephyr.bin` and `zephyr.bin.sha256` on `v*` tags with automatic prerelease detection.
- Keep a Changelog hygiene (`CHANGELOG.md`).

### Changed
- Updated Web Serial flashing instructions in `README.md` and `docs/USER_MANUAL.md` to reference **GitHub Releases** (and GitHub Actions CI build artifacts for commit-by-commit development builds) instead of the retired `preview-firmware` branch.

## [0.1.0-alpha.1] - 2026-11-22

### Added
- **Milestone 1 — Portable C11 HAL & Host Offline Simulator (`src/hal/`, `sim/sp1_sim.c`):**
  - Defined pure C11 hardware abstraction interfaces (`sp1_hal.h`, `hal_audio.h`, `hal_controls.h`, `hal_midi.h`) with lock-free SPSC MIDI queues and double-buffered control snapshots (`sp1_hal_common.c`).
  - Implemented the Zephyr nRF52840 HAL (`sp1_hal_zephyr.c`) and desktop host simulator (`sp1_hal_host.c`, `sp1_sim`) rendering 48 kHz 16-bit stereo WAV files and timestamped MIDI CSV logs, backed by 15 host CTest unit suites.
- **Milestone 2 — 4-Track Bjorklund Euclidean Sequencer & 24 PPQN Sync (`src/core/`):**
  - Implemented deterministic integer Bjorklund Euclidean pattern generation (`euclid.c`, `E(k, n, rot)` for `1..32` steps per track), 24 PPQN USB-C MIDI clock master/follower with IIR jitter smoothing and `500 ms` automatic internal failover (`clock_sync.c`), and a 1-step lookahead sequencer (`sequencer.c`) supporting global 16th-note swing (`50%..66%`) and per-track J Dilla micro-timing (`±35.0 ms` with `[0.42..0.58]` grid-lock deadzone).
  - Implemented the screenless SP-1 gesture mapper and 8-channel PWM LED renderer (`ui_mapper.c`) with `16-LSB` anti-clobber fader latching, bipolar Dilla nudge meter, and 4-LED rotation/polymeter bar meters.
- **Milestone 3 — 4-Voice C11 Drum Synth, Master Performance FX & Standalone 48 kHz I2S0 Audio (`src/dsp/`):**
  - Implemented 4 statically allocated 48 kHz drum voices (`drum_voices.c`): Kick/Sub, continuous Rimshot↔Snare morph, 6-oscillator Metallic Hi-Hat (Closed↔Open decay morph), and 2-Op FM / West-Coast Wavefold Perc using Flash-resident LUTs and Cytomic TPT SVFs.
  - Implemented the stereo Master Performance FX bus (`master_fx.c`): selective Dub Ping-Pong Delay (`8 KB` SRAM), tempo-synced Beat-Repeat/Stutter looper (`32 KB` SRAM, 64-frame clickless crossfade), DJ resonant LP/HP TPT SVF isolator, and ZOH Bitcrusher + soft-limiter.
  - Wired standalone 48 kHz I2S0 EasyDMA audio streaming (`CS42L42` headphone DAC and `TAS2505` internal speaker amplifier) with Cortex-M4F FPU Flush-To-Zero (`FPSCR.FZ/DN`).
- **Milestone 4 — DWT Cycle Profiler, Safety Gates & Documentation:**
  - Added ARMv7E-M `DWT->CYCCNT` audio-block cycle profiling (`47.3%` avg / `48.7%` peak CPU @ 64 MHz), `k_sys_fatal_error_handler()` cold-reboot override, `slot0_partition` (`0x00020000`) bootloader-safe linking, `README.md`, `docs/USER_MANUAL.md`, `docs/ARCHITECTURE.md`, and `ATTRIBUTION.md`.
- **UX Remapping, Live Voice Morphing & Pocket-Safe Power-On:**
  - Remapped unshifted `RWD/FWD` taps to fine tempo (`±2 BPM`), `Function + RWD/FWD` to coarse tempo (`±10 BPM`), `Function + Vol-/Vol+` to global swing (`±2%`), and `Vol-/Vol+` to master volume (`±8`).
  - Added live `Hold T1..T4 + Fader 1..4` per-voice timbre/morph control in Normal Mode with automatic release-mute suppression and anti-clobber fader re-latching.
  - Added a `1.5 s` pocket-safe `Function` battery power-on hold with progressive 4-LED `Play 1..4` fill and a boot LED sweep.

### Fixed
- **Initial Physical SP-1 Hardware Boot Bring-Up:**
  - Resolved the Zephyr `v4.3.1` USB boot-init collision by disabling `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` so the application owns the single composite USB device (`zephyr_udc0`).
  - Corrected `I2S0` slave clock topology against the `CS42L42` master clock and hardened watchdog reloading during I2C codec bring-up.
- **Volume, Real-Time Concurrency & Sequencer Hardening:**
  - Set default boot master volume to medium (`64/127`) across `CS42L42` and `TAS2505`.
  - Eliminated cross-thread races in SPSC snapshot ownership and audio lifecycle, fixed codec attenuation polarity, clamped transient DSP envelopes, and hardened lookahead re-anchoring, clock failover continuity, and MIDI TX ordering.

[Unreleased]: https://github.com/davidlwarner/sp1-drum/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/davidlwarner/sp1-drum/releases/tag/v0.1.0-alpha.1
