# Embedded Audio & SP-1 Firmware Engineering Rules

These rules govern all code written for the SP-1 4-track Euclidean drum machine firmware (Nordic nRF52840 @ 64 MHz Cortex-M4F, 1 MB Flash / 892 KB `slot0_partition`, 256 KB SRAM, Zephyr RTOS `v4.3.1`).

## 1. Graph-First Codebase Exploration
- Always check `graphify-out/GRAPH_REPORT.md` or query the Graphify MCP / CLI (`graphify query`, `graphify explain`, `graphify path`) **before** doing broad regex or grep searches across `reference/` or vendor SDK files.
- See `.gemini/rules/graphify.md` for refresh and query commands.

## 2. Clean Isolation from Upstream BSP (`ericlewis/sp1-midi`)
- Keep **all** new firmware logic strictly isolated inside self-contained directories:
  - `src/core/`: Pure C11 Euclidean Bjorklund sequencer, pattern state, and 24 PPQN clock sync.
  - `src/dsp/`: Pure C11 4-voice drum synthesizer (Kick, Snare, Hat, Perc) and master FX bus.
  - `src/hal/`: Thin SP-1 Zephyr hardware abstraction layer (I2S0 DMA audio pump, CS42L42/TAS2505 init, SAADC faders/buttons, PWM LEDs, USB-C MIDI UMP bridge).
- **Never** interleave drum-machine application logic into upstream `sp1-midi` files (`boards/`, `drivers/`, `subsys/`, `app/`) beyond minimal glue/dispatch hooks when Milestone 1+ integration begins. This preserves clean upstream merges and keeps future rebasing onto `Worldwave/Wakes` straightforward.
- `src/core/` and `src/dsp/` **must remain 100% portable C11** with zero `#include <zephyr/...>` or Nordic HAL headers so every module compiles and executes natively in the host `ctest` suite.
- Because the upstream Zephyr entrypoint (`app/main.cpp`) is compiled as C++17, **all public headers under `src/core/`, `src/dsp/`, and `src/hal/` must include `#ifdef __cplusplus extern "C" { #endif` guards**.

## 3. Zero Dynamic Memory Allocation After Startup
- **Forbidden** anywhere after startup initialization (and forbidden entirely inside `src/core/` and `src/dsp/`):
  - `malloc`, `calloc`, `realloc`, `free`
  - Zephyr `k_malloc`, `k_calloc`, `k_free`
  - C++ `new`, `delete`, dynamic `std::vector`/`std::string` growth
- All DSP voice states, delay/stutter lines, lookup tables, ring buffers, and sequencer bitmasks **must be statically allocated** with explicit RAM and Flash byte-budget comments next to their declarations (e.g., `/* Static RAM: 4096 B (1024 stereo int16/float samples) */`).

## 4. Hard Real-Time Safety (Audio & 24 PPQN Clock Paths)
- Inside the **I2S/USB audio ISR or high-priority audio render callback** and inside any **24 PPQN timer ISR/callback**:
  - **No blocking I/O**, no `k_sleep`, no `k_mutex_lock`, no `k_sem_take(..., K_FOREVER)`.
  - **No logging or formatting:** no `printf`, `snprintf`, `printk`, or `LOG_*` calls.
  - **No eMMC or Flash writes** (storage I/O belongs strictly on low-priority background threads and never touches flash below `0x20000`).
  - **No expensive runtime division** (`/` or `%` on non-power-of-two integers or floats inside per-sample inner loops); precompute reciprocals or use bit shifts.
  - **No implicit `double` promotion:** use single-precision `float` with explicit `f` suffixes (`0.5f`, `48000.0f`) and `float` math intrinsics (`sqrtf`, `fabsf`), avoiding `sinf`/`cosf`/`tanf`/`powf` inside per-sample loops in favor of Flash lookup tables or polynomial approximations.
- Communicate between UI/SAADC/USB-MIDI threads and the audio engine strictly via lock-free single-producer/single-consumer (SPSC) queues or atomic parameter snapshots.

## 5. SP-1 Hardware & Bootloader Safety ("Big Five")
- Never write internal Flash below `0x20000` (`boot_partition` holds the Teenage Engineering `T1+T4` recovery bootloader). Keep `CONFIG_USE_DT_CODE_PARTITION=y`.
- Never write `0xFF000`–`0xFFFFF` (`storage_partition`) until bootloader MBR reservation is verified; use eMMC for user presets if persistence is added.
- Feed the hardware watchdog across all potentially blocking loops (`< 5 s`), keep `CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y`, reboot rather than halt in fatal fault handlers, and power down external audio chips (`TAS2505`, `CS42L42`, `3.072 MHz osc`) and LEDs before `sys_poweroff()`.

## 6. Compiler Warning & Dual-Target Hygiene
- All new C modules under `src/` and all unit tests under `tests/` **must compile cleanly with `-std=c11 -Wall -Wextra -Werror`** on:
  1. Target ARM cross-compiler (`arm-zephyr-eabi-gcc`, Cortex-M4F with hard-float FPU; also compatible with `arm-none-eabi-gcc`).
  2. Host `gcc` (x86_64 CTest harness).
  3. Host `clang` (x86_64 CTest harness).

## 7. Open-Source Hygiene & Attribution
- **Repository-Relative Paths Only:** All tracked source files, CMake files, documentation, rules, and skills must use repository-relative paths (`src/...`, `boards/...`, `build-host/...`). Never commit machine- or container-specific absolute paths to `main`.
- **Zero Binary or Simulation Artifacts in Git:** Never commit compiled firmware binaries or host simulation outputs (`.bin`, `.elf`, `.hex`, `.uf2`, `.wav`, `.csv`, `build/`, `build-host/`, `reference/`, `graphify-out/`) to `main`.
- **Clean-Room `src/` Implementation & MIT Attribution:** All modules under `src/core/`, `src/dsp/`, `src/hal/`, `sim/`, and `tests/` must remain clean-room C11/C++17 implementations. Upstream BSP attribution (`ericlewis/sp1-midi`, `timknapen/SP-1-dev`, `chattock/sp1-tape-looper`, `Worldwave/Wakes`, and the Zephyr Project) must be maintained in `LICENSE` and `ATTRIBUTION.md`, and any code snippet ever adapted from `reference/` must carry an explicit MIT copyright and attribution header.

