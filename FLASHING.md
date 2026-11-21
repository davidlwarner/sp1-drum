# SP-1 Euclidean Drum Machine — Flashable Firmware Binary (`zephyr.bin`)

- **Source Commit (`main`):** `6c91e15`
- **Target Board:** Teenage Engineering Stem Player SP-1 (`stem_player`, Nordic nRF52840 Cortex-M4F @ 64 MHz)
- **Toolchain:** Zephyr RTOS `v4.3.1` + Zephyr SDK `0.17.4` (`arm-zephyr-eabi-gcc 12.2.0`)
- **Binary File:** `zephyr.bin` (`217336` bytes)
- **SHA-256 Checksum:** `c65893282930b51d88bfb1832cde3b0751bb3074662813fa08e8e8d44233d3c8`
- **Load Address (`slot0_partition`):** `0x00020000` (`0x00000..0x1FFFF` Teenage Engineering `T1+T4` recovery bootloader protected)
- **Measured Memory Footprint:**
  - **FLASH:** `217,336 B / 892 KB (23.79%)` (`text = 213,592 B`, `data = 3,736 B`)
  - **SRAM:** `104,708 B / 256 KB (39.94% <= 128 KB gate)` (`data = 3,736 B`, `bss = 100,974 B`)
- **Measured Real-Time Cycle Load (96 frames / `2.0 ms` @ 48 kHz):**
  - `60,604` avg / `62,380` peak cycles per block (`47.3%` avg / `48.7%` peak of `128,000`-cycle ceiling @ 64 MHz, well inside `<= 55%` target)

## Web Serial Flashing Instructions

1. Power off your SP-1 (hold `Function ••` for 3 seconds until LEDs fade out).
2. Hold **`Track 1 (T1)` + `Track 4 (T4)`** simultaneously while plugging in the USB-C cable.
3. Confirm all 4 corner Track LEDs illuminate solid (bootloader mode active), then release `T1+T4`.
4. Download [`zephyr.bin`](https://github.com/davidlwarner/sp1-drum/raw/preview-firmware/zephyr.bin) and verify its SHA-256 digest (`c65893282930b51d88bfb1832cde3b0751bb3074662813fa08e8e8d44233d3c8`).
5. Open [`https://solderless.engineering`](https://solderless.engineering) in Chrome or Edge, connect over Web Serial, select `zephyr.bin`, and flash.
