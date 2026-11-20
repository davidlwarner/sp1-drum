---
name: sp1-hardware-map
description: Verified hardware map for the Teenage Engineering Stem Player SP-1 (Nordic nRF52840): SAADC fader sliders, ADC resistor-ladder buttons, GPIO function button, PWM LED array, I2C0/I2S0 CS42L42 + TAS2505 audio codecs, USB-C MIDI/Audio endpoints, eMMC, and T1+T4 USB-C bootloader recovery sequence.
---

# Teenage Engineering SP-1 Hardware Map

Use this skill whenever writing or debugging SP-1 hardware abstraction code (`src/hal/`), Devicetree overlays, Kconfig options, or flashing procedures. All pinouts and register facts below are verified against `boards/teenageengineering/stem_player/stem_player.dts`, `stem_player-pinctrl.dtsi`, `reference/SP-1-dev/src/stemplayer_pins.h`, `reference/Wakes/`, and `reference/sp1-tape-looper/`.

## 1. SoC & Memory Map (Nordic nRF52840 QIAA)

- **CPU:** ARM Cortex-M4F @ 64 MHz with single-precision FPU (`CONFIG_FPU=y`, `CONFIG_FPU_SHARING=y`, `CONFIG_NRF_ENABLE_ICACHE=y`).
- **LFCLK:** Synthesized (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y`) — **do not change**; no 32.768 kHz crystal is driven for the application.
- **SRAM:** 256 KB (`sram0`).
- **Internal Flash (1024 KB, `flash0`):**

| Partition | Address Range | Size | Owner / Safety Rule |
|---|---|---|---|
| `boot_partition` | `0x00000`–`0x1FFFF` | 128 KB | **TE Bootloader (`T1+T4` recovery). NEVER erase or write below `0x20000`.** |
| `slot0_partition` | `0x20000`–`0xFEFFF` | 892 KB | Application image (`CONFIG_USE_DT_CODE_PARTITION=y` required). |
| `storage_partition` | `0xFF000`–`0xFFFFF` | 4 KB | Potentially MBR/bootloader-reserved; **do not write** (use eMMC for presets). |

---

## 2. Faders (4x Analog Sliders via 12-bit nRF52840 SAADC)

> **Important hardware correction:** The SP-1's four touch/slider strips are **analog potentiometer/resistive tracks sampled by the nRF52840 SAADC** (not I2C capacitive touch sensors). They share the `btn_com_reg` (`P1.10`) common rail regulator (always-on at boot).

All SAADC channels are configured in `stem_player.dts` with 12-bit resolution (`0`–`4080`), `ADC_GAIN_1_4`, `ADC_REF_VDD_1_4`, `3 µs` acquisition time.

| Control | DT `zephyr,user` Name | SAADC Channel | Pin | `analog-axis` Axis |
|---|---|---|---|---|
| Fader 1 (Track 1) | `fader1` | `AIN3` (`&adc 3`) | `P0.05` | `INPUT_ABS_X` |
| Fader 2 (Track 2) | `fader2` | `AIN6` (`&adc 6`) | `P0.30` | `INPUT_ABS_Y` |
| Fader 3 (Track 3) | `fader3` | `AIN2` (`&adc 2`) | `P0.04` | `INPUT_ABS_Z` |
| Fader 4 (Track 4) | `fader4` | `AIN7` (`&adc 7`) | `P0.31` | `INPUT_ABS_RX` |
| Battery Sense | `battery` | `AIN4` (`&adc 4`) | `P0.28` | `INPUT_ABS_RY` |

- **SAADC Ordering Caveat:** If using Zephyr's `analog-axis` batched `adc_read()`, children in DTS **must** be ordered by ascending SAADC channel number (`2, 3, 4, 6, 7`), because nRF SAADC writes DMA results in hardware channel order.
- **Deadband & Scaling:** Stock `sp1-midi` clamps samples to `[0, 0x0FF0]`, applies an 8-LSB deadband (`~0.2%`), and shifts `sample >> 5` for 7-bit MIDI CC (`0`–`127`).

---

## 3. Buttons (2x ADC Resistor Ladders + 1x GPIO)

- **Button Common Rail:** `P1.10` (`btn_com_reg`, active-high fixed regulator, enabled at boot).
- **Function Button (`••`):** GPIO `P0.27` (`GPIO_PULL_UP | GPIO_ACTIVE_LOW`), `INPUT_KEY_MENU`. Hold `>= 3000 ms` triggers graceful `sys_poweroff()` (`SYSTEM_OFF`).
- **Track + Play Ladder (`AIN0` / `P0.02`, `adc_keys_track`, `vref-mv = 825`, 20 ms poll):**

| Button | Zephyr Keycode | Single-Press / Chord Thresholds (mV) |
|---|---|---|
| Track 1 (`T1`) | `INPUT_KEY_1` | `179, 517, 779, 996, 1178, 1334, 1467, 1584` |
| Track 2 (`T2`) | `INPUT_KEY_2` | `364, 517, 893, 996, 1259, 1334, 1528, 1584` |
| Track 3 (`T3`) | `INPUT_KEY_3` | `655, 779, 893, 996, 1403, 1467, 1528, 1584` |
| Track 4 (`T4`) | `INPUT_KEY_4` | `1091, 1178, 1259, 1334, 1403, 1467, 1528, 1584` |
| Play | `INPUT_KEY_PLAY` | `1587, 1589, 1591, 1592` |

- **Transport + Volume Ladder (`AIN1` / `P0.03`, `adc_keys_transport`, `vref-mv = 825`, 20 ms poll):**

| Button | Zephyr Keycode | Thresholds (mV) |
|---|---|---|
| Rewind / Back (`<<`) | `INPUT_KEY_BACK` | `355, 441` |
| Volume Down (`-`) | `INPUT_KEY_VOLUMEDOWN` | `638, 793` |
| Fast Forward (`>>`) | `INPUT_KEY_FASTFORWARD` | `1064, 1321` |
| Volume Up (`+`) | `INPUT_KEY_VOLUMEUP` | `1598, 1984` |

---

## 4. PWM LED Array (8x Monochrome LEDs)

All 8 LEDs use `PWM_USEC(1024)` period (`~976.6 Hz`). Clear all LEDs before `sys_poweroff()` because `SYSTEM_OFF` latches GPIO output states.

| Group | Peripheral | Channel 0 | Channel 1 | Channel 2 | Channel 3 |
|---|---|---|---|---|---|
| **Track LEDs (1–4)** | `PWM2` | `P0.29` (`led_track_1`) | `P0.26` (`led_track_2`) | `P1.15` (`led_track_3`) | `P1.14` (`led_track_4`) |
| **Play / Center LEDs (1–4)** | `PWM3` | `P0.01` (`led_play_1`) | `P1.12` (`led_play_2`) | `P0.00` (`led_play_3`) | `P1.13` (`led_play_4`) |

> Note: `reference/SP-1-dev/src/stemplayer_pins.h` numbers the 4 playback LEDs in reverse physical order (`PIN_LED_1=P1.13` .. `PIN_LED_4=P0.01`) relative to `stem_player.dts` (`led_play_1=P0.01` .. `led_play_4=P1.13`). Verify physical orientation on hardware during Milestone 4 HIL testing.

---

## 5. Audio Subsystem (I2C0 Codecs + I2S0 Master @ 48 kHz)

### Clocks & Control Bus
- **External 3.072 MHz Oscillator Enable:** `P0.13` (`osc-en-hog`, active-high GPIO hog). **Must be enabled and CS42L42 initialized before starting I2S0.**
- **I2C0 (`TWIM`, Fast 400 kHz):** `SDA = P1.07`, `SCL = P1.11`.
  - **CS42L42 (Headphone DAC/ADC Codec):** I2C address `0x48`, reset `P0.15` (active low). Driver in `drivers/audio/cs42l42_codec.c`; reference init sequence also in `reference/Wakes/firmware/src/sp1_audio.c`.
  - **TAS2505 (Internal Speaker Amp):** I2C address `0x18`, reset `P0.09` (active low, `NFC1` — `nfct-pins-as-gpios` enabled in DTS). Driver in `drivers/audio/tas2505_codec.c`.
    - **SWD Caveat:** TAS2505 may NACK I2C transactions when a full SWD debug cable is attached.
- **Shutdown Hygiene:** Mute and reset both TAS2505 (`P0.09`) and CS42L42 (`P0.15`) and disable the 3.072 MHz oscillator (`P0.13`) before `sys_poweroff()` to prevent battery drain and speaker pop/hum.

### I2S0 Audio Streaming (`&i2s0`, Master Mode)
- **Pins:** `BCLK (SCK_M) = P0.12`, `LRCK_M = P0.11`, `SDOUT = P1.09`.
- **Format:** 48,000 Hz stereo, 16-bit or 24-bit frames (`Wakes` uses 96-frame / 2 ms stereo `int16_t` EasyDMA blocks via `K_MEM_SLAB_DEFINE_STATIC` + `i2s_write()` with `CONFIG_I2S_NRFX_TX_BLOCK_COUNT` tuned for ~8 ms end-to-end latency).

---

## 6. USB-C Composite Endpoints (MIDI 2.0/1.0, CDC ACM, UAC2 Reference)

- **Controller:** nRF52840 Full-Speed USBD (`zephyr_udc0`, `CONFIG_USB_DEVICE_STACK_NEXT=y`).
- **Stock `sp1-midi` Composite Device:**
  - `VID:PID = 0x1209:0x0001` (IAD Misc `0xEF / 0x02 / 0x01`).
  - **USB MIDI (`usb_midi`):** `CONFIG_USBD_MIDI2_CLASS=y`, `CONFIG_MIDI2_UMP_STREAM_RESPONDER=y`, protocol `midi1-up-to-128b` (group 0). RX packets arrive via `usbd_midi_ops.rx_packet_cb`; TX uses `usbd_midi_send(midi_dev, ump)`.
  - **CDC ACM Console (`cdc_acm_uart0`):** Zephyr shell (`stem> `) + log backend (`CONFIG_SHELL_BACKEND_SERIAL=y`).
- **Low-Latency USB Audio / MIDI Fast-Path Reference:**
  - Stock Zephyr USBD thread dispatch costs ~110 µs per USB-MIDI message on nRF52840.
  - `reference/Wakes/zephyr-patches/udc_nrf-fast-paths.patch` and `reference/Wakes/firmware/src/sp1_uac.c` + `reference/sp1-tape-looper/patches/zephyr-uac2.patch` document interrupt-level bulk OUT (`CONFIG_UDC_NRF_OUT_FAST`) and ISO IN UAC2 (`CONFIG_UDC_NRF_ISO_IN_FAST`) patterns if USB-C Audio output is added later.

---

## 7. Auxiliary Peripherals (eMMC, Charger, Bluetooth)

- **Toshiba 4 GB eMMC (1-bit custom interface):** `CLK = P0.06`, `DAT0 = P0.07`, `CMD = P0.08`, `VccQ En = P0.14`, `RST = P1.08` (`subsys/storage/emmc/`).
- **BQ24232 Battery Charger:** `nPGOOD = P0.24`, `nCHG = P0.22`, `nCE = P0.21`, `ISET override = P1.00` (note: `README.md` lists P0.19, while both `stem_player.dts` and `stemplayer_pins.h` confirm `P1.00`!).
- **CYBT-353027-02 Bluetooth Module (`&uart0`, 115200 + HW flow control):** `TX = P1.02`, `RX = P1.04`, `RTS = P1.01`, `CTS = P1.03`, `RST = P0.10`.

---

## 8. `T1+T4` USB-C Bootloader & Flashing Sequence

1. Ensure the SP-1 is powered off (hold Function `••` for 3 seconds to enter `SYSTEM_OFF` if currently running).
2. **Hold Track 1 (`T1`) + Track 4 (`T4`)** simultaneously.
3. While holding `T1+T4`, **connect the SP-1 to the host via USB-C**.
4. All four track LEDs illuminate solid, confirming the TE bootloader (`0x00000`–`0x1FFFF`) is active in firmware-loading mode.
5. Open the Solderless Engineering Web Serial firmware utility (`https://solderless.engineering` in Chrome/Edge), connect over Web Serial, select `build/zephyr/zephyr.bin`, flash, and unplug.
6. **Alternative (SWD):** `board.cmake` also defines `jlink` (`--device=nRF52840_xxAA`), `nrfjprog`, and `nrfutil` runners (`ninja -C build flash`), noting the TAS2505 I2C NACK caveat while SWD is attached.

### Mandatory Pre-Flash Safety Checklist
- [ ] `CONFIG_USE_DT_CODE_PARTITION=y` enabled (image linked at `0x20000`, never `0x0`).
- [ ] Watchdog fed `< 5 s` in every blocking loop (`subsys/system/Watchdog.cpp`).
- [ ] `k_sys_fatal_error_handler()` reboots rather than halting.
- [ ] Function 3-second hold cleanly shuts down codecs, oscillator, LEDs, and enters `sys_poweroff()`.
