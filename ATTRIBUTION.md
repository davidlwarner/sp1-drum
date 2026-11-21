# Attribution & Open-Source Acknowledgments

`sp1-drum` builds upon the work of the Teenage Engineering Stem Player (SP-1) open-source hardware and firmware community. We gratefully acknowledge the following projects and authors:

## 1. [`ericlewis/sp1-midi`](https://github.com/ericlewis/sp1-midi) — Eric Lewis
- **Role in `sp1-drum`:** Upstream Zephyr RTOS Board Support Package (BSP) foundation (`boards/teenageengineering/stem_player/`, `drivers/audio/cs42l42_codec.c`, `drivers/audio/tas2505_codec.c`, `drivers/charger/charger_bq24232.c`, `subsys/power/PowerManager.*`, `subsys/system/Watchdog.*`, `subsys/system/ResetBreadcrumbs.*`, `include/stem_player/Smf.hpp`, and `app/` skeleton).
- **License:** MIT License (`Copyright (c) 2026 Eric Lewis`).

## 2. [`timknapen/SP-1-dev`](https://github.com/timknapen/SP-1-dev) — Tim Knapen
- **Role in `sp1-drum`:** Reverse-engineered Teenage Engineering SP-1 nRF52840 pin mappings, SAADC fader and resistor-ladder button thresholds (`AIN0` Track/Play ladder, `AIN1` Transport/Volume ladder), PWM2/PWM3 LED layout, and `T1+T4` USB-C bootloader recovery documentation.
- **License:** Community open-source hardware documentation & reference firmware.

## 3. [`chattock/sp1-tape-looper`](https://github.com/chattock/sp1-tape-looper) — Charlie Shattock
- **Role in `sp1-drum`:** Hardware findings and Zephyr audio/USB experimental reference for the SP-1 platform.
- **License:** MIT License.

## 4. [`Worldwave/Wakes`](https://github.com/Worldwave/Wakes) — Worldwave / Eric Lewis
- **Role in `sp1-drum`:** Architectural reference for 48 kHz I2S0 EasyDMA block streaming (`K_MEM_SLAB_DEFINE_STATIC` + 96-frame / 2.0 ms stereo buffers), CS42L42/TAS2505 lifecycle management, and nRF52840 USBD performance characteristics.
- **License:** MIT License.

## 5. [The Zephyr Project](https://www.zephyrproject.org/) & [Cytomic (Andrew Simper)](https://cytomic.com/)
- **Zephyr RTOS (`v4.3.1`):** Real-time operating system, Devicetree infrastructure, nRF52840 HAL, I2S/SAADC/PWM drivers, and USB MIDI 2.0 Universal MIDI Packet (UMP) stack (Apache-2.0 License).
- **DSP Theory References:** Clean-room C11 implementations in `src/core/` and `src/dsp/` draw on Godfried Toussaint's Euclidean rhythm algorithm (*"The Euclidean Algorithm Generates Traditional Musical Rhythms"*, 2005) and Andrew Simper's linear Trapezoidal Integrated State-Variable Filter (TPT SVF) nodal equations (*Cytomic Technical Papers*, 2013).
