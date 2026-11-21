# SP-1 Euclidean Drum Machine — Flashable Firmware Binary (`zephyr.bin`)

- **Source Commit (`main`):** `e027427`
- **Target Board:** Teenage Engineering Stem Player SP-1 (`stem_player`, Nordic nRF52840 Cortex-M4F @ 64 MHz)
- **Toolchain:** Zephyr RTOS `v4.3.1` + Zephyr SDK `0.17.4` (`arm-zephyr-eabi-gcc 12.2.0`)
- **Build Command:** `west build -b stem_player . --build-dir build -p auto` *(do **not** pass `-DSNIPPET=cdc-acm-console` on Zephyr `v4.3.1`)*
- **Binary File:** `zephyr.bin` (`172100` bytes)
- **SHA-256 Checksum:** `55a291eeb59879cc37ec7031e9ba4598c2f980491e5fa0e4c8b5fe374527cbbf`
- **Image Format & Load Address (`slot0_partition`):** Raw ARM Cortex-M4 binary linked directly at `0x00020000` (NO MCUboot `0x96f3b83d` header — Teenage Engineering's SP-1 bootloader at `0x00000..0x1FFFF` jumps directly to the raw vector table at `0x20000`, matching `Worldwave/Wakes`, `chattock/sp1-tape-looper`, and `bnjreece/feldd-sp1-firmware`)
- **Verified Vector Table (first 2 words at `0x20000`):**
  - `MSP = 0x20012680` (valid nRF52840 SRAM `0x20000000..0x20040000`)
  - `Reset_Handler = 0x00031c11` (Thumb entry inside `slot0_partition`, zero load bytes below `0x20000` or at/above `0xFF000`)
- **Measured Memory Footprint:**
  - **FLASH:** `172,100 B / 892 KB (18.84%)` (`text = 169,076 B`, `data = 3,020 B`)
  - **SRAM:** `105,740 B / 256 KB (40.34% <= 128 KB gate)` (`data = 3,020 B`, `bss = 102,647 B`, `CONFIG_ARM_MPU=y`, `CONFIG_HW_STACK_PROTECTION=y`)
- **Measured Real-Time Cycle Load (96 frames / `2.0 ms` @ 48 kHz):**
  - `60,604` avg / `62,380` peak cycles per block (`47.3%` avg / `48.7%` peak of `128,000`-cycle ceiling @ 64 MHz, well inside `<= 55%` target)

## What Changed in This Build (`e027427`)

1. **Removed Zephyr `v4.3.1` USB Boot-Init Collision:** Built without `-DSNIPPET=cdc-acm-console` and removed `Kconfig.cdc_acm_serial.defconfig` from board defaults so `cdc_acm_serial_init_device` never claims `zephyr_udc0` before `main()`'s `init_usb()`. Single `cdc_acm_uart0` + `usb_midi` composite device.
2. **Fixed SP-1 I2S0 Slave Topology & CS42L42 PLL:** Switched `i2s0` pinctrl and driver config to slave clocks (`I2S_SCK_S` P0.12 from external 3.072 MHz osc, `I2S_LRCK_S` P0.11 mastered by CS42L42), fixed CS42L42 `0x1504` PLL fractional byte (`0x80`), and disabled unused `uart0` (BT pins).
3. **Fault-Tolerant Boot & Startup LED Sweep:** Added an immediate 8-LED boot sweep at the start of `main()` before USB/audio init, switched audio thread from `K_PRIO_COOP(2)` to preemptible `K_PRIO_PREEMPT(0)` with non-spinning I2S error recovery, reloaded all 8 WDT channels across codec I2C init, increased main/USBD stacks (`4096 B` / `2048 B`), and added a 50 Hz fallback UI/LED tick in `AppStateMachine` so all 8 LEDs and controls stay live even if audio init fails.

## Web Serial Flashing Instructions

1. Power off your SP-1 (hold `Function ••` for 3 seconds until LEDs fade out).
2. Hold **`Track 1 (T1)` + `Track 4 (T4)`** simultaneously while plugging in the USB-C cable.
3. Confirm all 4 corner Track LEDs illuminate solid (bootloader mode active), then release `T1+T4`.
4. Download [`zephyr.bin`](https://github.com/davidlwarner/sp1-drum/raw/preview-firmware/zephyr.bin) and verify its SHA-256 digest (`55a291eeb59879cc37ec7031e9ba4598c2f980491e5fa0e4c8b5fe374527cbbf`).
5. Open [`https://solderless.engineering`](https://solderless.engineering) in Chrome or Edge, connect over Web Serial, select `zephyr.bin`, and flash.
6. After flashing finishes and the unit reboots (or power-on), you should immediately see an **8-LED startup sweep** (Track 1→4, Play 1→4, brief all-8 pulse) followed by the live Euclidean drum sequencer LEDs.
