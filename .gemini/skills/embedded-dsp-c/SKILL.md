---
name: embedded-dsp-c
description: Pure C11 real-time DSP patterns for 48 kHz audio synthesis and FX on ARM Cortex-M4F (nRF52840 @ 64 MHz): uint32_t phase accumulators, interpolated Flash sine/wavefolder LUTs, stable TPT State-Variable Filters (SVF), and hard cycle budgeting.
---

# Embedded C11 DSP Skill (48 kHz Cortex-M4F / nRF52840)

Use this skill whenever writing or reviewing C11 synthesis, envelope, filter, or FX code under `src/dsp/` or `src/core/`.

## 1. Target CPU & Cycle Budget (nRF52840 @ 64 MHz, FPU Enabled)

- **CPU Clock:** 64,000,000 Hz (Cortex-M4F with single-precision FPv4-SP-D16 FPU; `CONFIG_FPU=y`, `CONFIG_FPU_SHARING=y`).
- **Audio Rate:** 48,000 Hz stereo (24-bit in 32-bit I2S frames or 16-bit PCM).
- **Hard Ceiling:** `64,000,000 / 48,000 = 1,333.3 cycles / stereo sample`.
  - For a **64-sample DMA block** (`1.333 ms`): `85,333` total CPU cycles.
  - For a **128-sample DMA block** (`2.667 ms`): `170,666` total CPU cycles.
- **Target DSP Budget (<= 55% CPU = ~730 cycles/sample):** Leave >= 45% headroom for Zephyr scheduler, I2S EasyDMA IRQ, USB-C MIDI/CDC traffic, SAADC polling, and PWM LED updates.

| Stage | Target Budget (cycles/sample) | Notes |
|---|---:|---|
| Voice 1: Kick (pitch-env sine + click + drive) | ~90 | 1 LUT osc + 2 exp envs + soft clip |
| Voice 2: Snare (body sine + LP/HP noise + snap) | ~120 | 1 LUT osc + xorshift32 noise + 1 SVF |
| Voice 3: Hat (metallic/noise + HP SVF + exp env) | ~110 | 4-6 square/XOR phase accs + 1 SVF |
| Voice 4: Perc (FM / wavefolder synth + SVF) | ~140 | 2 LUT oscs + wavefold LUT + 1 SVF |
| Master FX Bus (DJ SVF + Drive + Stutter/Delay) | ~180 | Stereo TPT SVF + circular buffer + soft limiter |
| Mix, Stereo Pan, Int24/Int32 Pack | ~50 | Branchless clamp & scale |
| **Total DSP Target** | **~690** | **~52% of 1,333 cycles/sample** |

### FPU Rules for Cortex-M4F
1. **Always suffix float literals with `f`** (`0.5f`, `1.0f`, `48000.0f`). Missing `f` promotes to IEEE-754 `double`, triggering slow software-emulated `__aeabi_dmul`/`__aeabi_dadd` (~50–100 cycles instead of 1–3 FPU cycles).
2. **Avoid `sinf`, `cosf`, `tanf`, `expf`, `powf` per-sample.** Compute coefficients at control/block rate (every 64 samples or on parameter change) or use Flash LUTs and fast rational approximations.
3. **Avoid per-sample division.** Float `vdiv.f32` costs 14 cycles; multiply by precomputed reciprocals (`1.0f / 48000.0f`) instead.
4. **Prevent subnormal stalls:** Enable Flush-To-Zero (`FPSCR.FZ = 1`, `FPSCR.DN = 1`) on the audio thread at startup and inject a tiny alternating anti-denormal offset (`1e-20f`) or clamp decay envelopes `< 1e-6f` to `0.0f`.

---

## 2. `uint32_t` Phase Accumulators

A 32-bit unsigned phase accumulator wraps modulo $2^{32}$ natively in 1 cycle with zero branches and sub-millihertz frequency resolution (`48000 / 2^32 = 0.0000112 Hz`).

```c
/* Precomputed constant: 2^32 / 48000.0f = 89478.485333f */
#define SP1_FS_HZ            48000.0f
#define SP1_PHASE_SCALE      89478.485333f

static inline uint32_t sp1_hz_to_phase_inc(float freq_hz) {
    if (freq_hz <= 0.0f) {
        return 0U;
    }
    if (freq_hz > (SP1_FS_HZ * 0.45f)) {
        freq_hz = SP1_FS_HZ * 0.45f;
    }
    return (uint32_t)(freq_hz * SP1_PHASE_SCALE);
}

/* Per-sample advance (1 cycle, branchless wrap): */
/* phase += phase_inc; */
```

- **Phase Modulation (2-op FM):** Add `(uint32_t)((int32_t)(mod_sample * mod_phase_scale))` (with `float mod_phase_scale` clamped to `<= 2.0e9f`, or cast via `int64_t`, so `mod_sample * mod_phase_scale` never exceeds `INT32_MAX` and avoids C11 float-to-int overflow UB or Cortex-M4F `vcvt.s32.f32` saturation) to the carrier read index without mutating the carrier's running accumulator (true phase modulation, zero DC pitch drift).

---

## 3. Interpolated Sine & Wavefolder Lookup Tables (Flash-Resident)

Store LUTs as `static const float` so the linker places them in `.rodata` (Flash, 892 KB available) rather than precious SRAM (256 KB).

### 256-Entry Sine LUT with Guard Sample (`1028 bytes` Flash)
Using 256 entries + 1 wrap guard entry (`table[256] == table[0]`) eliminates modulo/masking on the `idx + 1` read:

```c
/* Flash budget: 257 * 4 = 1028 bytes in .rodata; 0 bytes SRAM */
extern const float k_sine_lut_256[257];

static inline float sp1_sine_lut(uint32_t phase) {
    const uint32_t idx  = phase >> 24;                    /* top 8 bits: 0..255 */
    const uint32_t frac = (phase >> 8) & 0xFFFFU;         /* next 16 bits */
    const float t       = (float)frac * (1.0f / 65536.0f);
    const float a       = k_sine_lut_256[idx];
    const float b       = k_sine_lut_256[idx + 1U];
    return a + (b - a) * t;
}
```

### Wavefolder / Soft-Clip LUT (`1028 bytes` Flash)
Map an input `x` clamped to `[-4.0f, +4.0f]` into `[0.0f, 256.0f]` to look up a pre-computed West-Coast sine-fold curve (`sinf(x * pi * 0.5f)` with soft saturation at extremes) or use a fast branchless soft-clipper for drum bus saturation:

```c
/* Fast Padé-style tanh soft clipper (~6 cycles on M4F, exact 0 at 0, unit slope) */
static inline float sp1_fast_tanh(float x) {
    if (x > 3.0f)  return  1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Simple symmetric triangle wavefolder (zero division, branchless-friendly) */
static inline float sp1_wavefold(float x, float drive) {
    float v = x * drive;
    /* Fold into [-1, 1] using 2 iterations (covers drive up to 5x) */
    if (v >  1.0f) v =  2.0f - v;
    if (v < -1.0f) v = -2.0f - v;
    if (v >  1.0f) v =  2.0f - v;
    if (v < -1.0f) v = -2.0f - v;
    return v;
}
```

---

## 4. Cytomic TPT State-Variable Filter (SVF) with Stability Clamping

Prefer the **Cytomic Trapezoidal Integrated (TPT) SVF** over the naive Chamberlin SVF: it remains unconditionally stable up to Nyquist and decouples cutoff from resonance.

```c
typedef struct {
    float ic1eq;  /* integrator 1 state */
    float ic2eq;  /* integrator 2 state */
    float a1;
    float a2;
    float a3;
    float k;      /* damping = 1 / Q */
} sp1_svf_t;      /* Static RAM: 24 bytes per instance */

/* Fast tan(pi * fc / fs) approximation accurate to <0.2% for fc <= 0.30 * fs (<1.6% at 0.42 * fs).
 * Call at block rate or on parameter change — NOT per sample. */
static inline float sp1_fast_tan_pi(float w) {
    /* w = fc / fs clamped to [0.0004f, 0.42f] => pi*w in [0.00125, 1.32] */
    const float x  = 3.14159265f * w;
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

static inline void sp1_svf_set_params(sp1_svf_t *f, float cutoff_hz, float q) {
    /* Strict stability clamping */
    float w = cutoff_hz * (1.0f / SP1_FS_HZ);
    if (w < 0.0004f) w = 0.0004f;  /* ~20 Hz floor */
    if (w > 0.42f)   w = 0.42f;    /* ~20.16 kHz ceiling (< Nyquist) */

    if (q < 0.5f)  q = 0.5f;
    if (q > 12.0f) q = 12.0f;      /* k = 1/Q >= 0.0833 prevents self-oscillation blowup */

    const float g = sp1_fast_tan_pi(w);
    const float k = 1.0f / q;
    f->k  = k;
    f->a1 = 1.0f / (1.0f + g * (g + k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

/* Per-sample tick (~12 FPU ops, zero divisions) */
static inline void sp1_svf_process(sp1_svf_t *f, float v0,
                                   float *lp, float *bp, float *hp) {
    const float v3 = v0 - f->ic2eq;
    const float v1 = f->a1 * f->ic1eq + f->a2 * v3;
    const float v2 = f->ic2eq + f->a2 * f->ic1eq + f->a3 * v3;
    f->ic1eq = 2.0f * v1 - f->ic1eq;
    f->ic2eq = 2.0f * v2 - f->ic2eq;

    /* Flush tiny denormals to zero if audio thread FTZ is not active */
    if (fabsf(f->ic1eq) < 1e-20f) f->ic1eq = 0.0f;
    if (fabsf(f->ic2eq) < 1e-20f) f->ic2eq = 0.0f;

    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = v0 - f->k * v1 - v2;
}
```

---

## 5. Fast Noise, Envelopes & Static Delay Buffers

- **PRNG Noise (Snare/Hat):** 32-bit `xorshift32` (~4 cycles/sample) cast to signed `int32_t` and scaled via `(float)(int32_t)state * (1.0f / 2147483648.0f)` into `[-1.0f, +1.0f)`. Seed with non-zero constant per voice.
- **Exponential Decay Envelopes:** `env *= coeff` where `coeff = expf(-1.0f / (tau_sec * 48000.0f))` computed once on trigger/param change; snap to `0.0f` when `env < 1e-5f` so idle voices short-circuit and save CPU.
- **Delay / Stutter Buffer:** Size to a power of two (e.g., `16384` `int16_t` mono samples = `32 KB` SRAM = `341 ms` @ 48 kHz, or `32768` `int16_t` = `64 KB` SRAM = `682 ms`) so index wrapping uses `idx & (BUF_LEN - 1U)` (1 cycle, zero modulo).
- **One-Pole Parameter Smoothing:** Smooth fader-driven parameters (cutoff, pitch, drive, wet/dry) inside the audio callback via `cur += 0.002f * (target - cur)` to eliminate zipper noise without per-sample divisions.
