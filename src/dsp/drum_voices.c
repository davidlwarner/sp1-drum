#include "dsp/drum_voices.h"

#include <stddef.h>
#include <string.h>

/* ── Flash-Resident 257-Entry Sine Lookup Table (1028 B in .rodata, 0 B SRAM) ─ */

const float k_sine_lut_256[SP1_DSP_LUT_TABLE_LEN] = {
     0.00000000f,  0.02454123f,  0.04906767f,  0.07356456f,  0.09801714f,  0.12241068f,  0.14673047f,  0.17096189f,
     0.19509032f,  0.21910124f,  0.24298018f,  0.26671276f,  0.29028468f,  0.31368174f,  0.33688985f,  0.35989504f,
     0.38268343f,  0.40524131f,  0.42755509f,  0.44961133f,  0.47139674f,  0.49289819f,  0.51410274f,  0.53499762f,
     0.55557023f,  0.57580819f,  0.59569930f,  0.61523159f,  0.63439328f,  0.65317284f,  0.67155895f,  0.68954054f,
     0.70710678f,  0.72424708f,  0.74095113f,  0.75720885f,  0.77301045f,  0.78834643f,  0.80320753f,  0.81758481f,
     0.83146961f,  0.84485357f,  0.85772861f,  0.87008699f,  0.88192126f,  0.89322430f,  0.90398929f,  0.91420976f,
     0.92387953f,  0.93299280f,  0.94154407f,  0.94952818f,  0.95694034f,  0.96377607f,  0.97003125f,  0.97570213f,
     0.98078528f,  0.98527764f,  0.98917651f,  0.99247953f,  0.99518473f,  0.99729046f,  0.99879546f,  0.99969882f,
     1.00000000f,  0.99969882f,  0.99879546f,  0.99729046f,  0.99518473f,  0.99247953f,  0.98917651f,  0.98527764f,
     0.98078528f,  0.97570213f,  0.97003125f,  0.96377607f,  0.95694034f,  0.94952818f,  0.94154407f,  0.93299280f,
     0.92387953f,  0.91420976f,  0.90398929f,  0.89322430f,  0.88192126f,  0.87008699f,  0.85772861f,  0.84485357f,
     0.83146961f,  0.81758481f,  0.80320753f,  0.78834643f,  0.77301045f,  0.75720885f,  0.74095113f,  0.72424708f,
     0.70710678f,  0.68954054f,  0.67155895f,  0.65317284f,  0.63439328f,  0.61523159f,  0.59569930f,  0.57580819f,
     0.55557023f,  0.53499762f,  0.51410274f,  0.49289819f,  0.47139674f,  0.44961133f,  0.42755509f,  0.40524131f,
     0.38268343f,  0.35989504f,  0.33688985f,  0.31368174f,  0.29028468f,  0.26671276f,  0.24298018f,  0.21910124f,
     0.19509032f,  0.17096189f,  0.14673047f,  0.12241068f,  0.09801714f,  0.07356456f,  0.04906767f,  0.02454123f,
     0.00000000f, -0.02454123f, -0.04906767f, -0.07356456f, -0.09801714f, -0.12241068f, -0.14673047f, -0.17096189f,
    -0.19509032f, -0.21910124f, -0.24298018f, -0.26671276f, -0.29028468f, -0.31368174f, -0.33688985f, -0.35989504f,
    -0.38268343f, -0.40524131f, -0.42755509f, -0.44961133f, -0.47139674f, -0.49289819f, -0.51410274f, -0.53499762f,
    -0.55557023f, -0.57580819f, -0.59569930f, -0.61523159f, -0.63439328f, -0.65317284f, -0.67155895f, -0.68954054f,
    -0.70710678f, -0.72424708f, -0.74095113f, -0.75720885f, -0.77301045f, -0.78834643f, -0.80320753f, -0.81758481f,
    -0.83146961f, -0.84485357f, -0.85772861f, -0.87008699f, -0.88192126f, -0.89322430f, -0.90398929f, -0.91420976f,
    -0.92387953f, -0.93299280f, -0.94154407f, -0.94952818f, -0.95694034f, -0.96377607f, -0.97003125f, -0.97570213f,
    -0.98078528f, -0.98527764f, -0.98917651f, -0.99247953f, -0.99518473f, -0.99729046f, -0.99879546f, -0.99969882f,
    -1.00000000f, -0.99969882f, -0.99879546f, -0.99729046f, -0.99518473f, -0.99247953f, -0.98917651f, -0.98527764f,
    -0.98078528f, -0.97570213f, -0.97003125f, -0.96377607f, -0.95694034f, -0.94952818f, -0.94154407f, -0.93299280f,
    -0.92387953f, -0.91420976f, -0.90398929f, -0.89322430f, -0.88192126f, -0.87008699f, -0.85772861f, -0.84485357f,
    -0.83146961f, -0.81758481f, -0.80320753f, -0.78834643f, -0.77301045f, -0.75720885f, -0.74095113f, -0.72424708f,
    -0.70710678f, -0.68954054f, -0.67155895f, -0.65317284f, -0.63439328f, -0.61523159f, -0.59569930f, -0.57580819f,
    -0.55557023f, -0.53499762f, -0.51410274f, -0.49289819f, -0.47139674f, -0.44961133f, -0.42755509f, -0.40524131f,
    -0.38268343f, -0.35989504f, -0.33688985f, -0.31368174f, -0.29028468f, -0.26671276f, -0.24298018f, -0.21910124f,
    -0.19509032f, -0.17096189f, -0.14673047f, -0.12241068f, -0.09801714f, -0.07356456f, -0.04906767f, -0.02454123f,
     0.00000000f
};

/* ── Flash-Resident 257-Entry West-Coast Wavefolder LUT (1028 B in .rodata) ── *
 * Precomputed transfer curve for x in [-4.0f, +4.0f]:
 *   y(x) = 0.72f * sin(0.5f * pi * 1.75f * x) + 0.28f * tanh(0.85f * x)
 * Odd-symmetric with exact 0.0f at center index 128. */

const float k_wavefold_lut_256[SP1_DSP_LUT_TABLE_LEN] = {
     0.43935567f,  0.48341686f,  0.52287185f,  0.55742943f,  0.58683461f,  0.61087024f,  0.62935907f,  0.64216560f,
     0.64919794f,  0.65040880f,  0.64579695f,  0.63540798f,  0.61933464f,  0.59771717f,  0.57074285f,  0.53864521f,
     0.50170255f,  0.46023634f,  0.41460919f,  0.36522245f,  0.31251344f,  0.25695240f,  0.19903925f,  0.13929999f,
     0.07828290f,  0.01655436f, -0.04531438f, -0.10674264f, -0.16714543f, -0.22593877f, -0.28254503f, -0.33639827f,
    -0.38694978f, -0.43367326f, -0.47606987f, -0.51367313f, -0.54605359f, -0.57282364f, -0.59364212f, -0.60821825f,
    -0.61631531f, -0.61775386f, -0.61241436f, -0.60023922f, -0.58123446f, -0.55547059f, -0.52308297f, -0.48427165f,
    -0.43929994f, -0.38849288f, -0.33223474f, -0.27096623f, -0.20518118f, -0.13542300f, -0.06228047f,  0.01361547f,
     0.09159607f,  0.17096011f,  0.25097942f,  0.33090448f,  0.40997028f,  0.48740238f,  0.56242287f,  0.63425654f,
     0.70213687f,  0.76531261f,  0.82305384f,  0.87465876f,  0.91946030f,  0.95683235f,  0.98619574f,  1.00000000f,
     1.00000000f,  1.00000000f,  1.00000000f,  0.99639434f,  0.97817266f,  0.95197952f,  0.91801590f,  0.87654293f,
     0.82788002f,  0.77240247f,  0.71053839f,  0.64276534f,  0.56960684f,  0.49162835f,  0.40943310f,  0.32365763f,
     0.23496747f,  0.14405259f,  0.05162289f, -0.04160725f, -0.13491443f, -0.22757201f, -0.31885615f, -0.40805173f,
    -0.49445826f, -0.57739615f, -0.65621281f, -0.73028874f, -0.79904336f, -0.86194122f, -0.91849744f, -0.96828318f,
    -1.00000000f, -1.00000000f, -1.00000000f, -1.00000000f, -1.00000000f, -1.00000000f, -1.00000000f, -1.00000000f,
    -1.00000000f, -0.97180200f, -0.92777228f, -0.87687898f, -0.81948513f, -0.75599927f, -0.68687266f, -0.61259615f,
    -0.53369737f, -0.45073742f, -0.36430728f, -0.27502418f, -0.18352754f, -0.09047470f,  0.00346367f,  0.09760920f,
     0.00000000f, -0.09760920f, -0.00346367f,  0.09047470f,  0.18352754f,  0.27502418f,  0.36430728f,  0.45073742f,
     0.53369737f,  0.61259615f,  0.68687266f,  0.75599927f,  0.81948513f,  0.87687898f,  0.92777228f,  0.97180200f,
     1.00000000f,  1.00000000f,  1.00000000f,  1.00000000f,  1.00000000f,  1.00000000f,  1.00000000f,  1.00000000f,
     1.00000000f,  0.96828318f,  0.91849744f,  0.86194122f,  0.79904336f,  0.73028874f,  0.65621281f,  0.57739615f,
     0.49445826f,  0.40805173f,  0.31885615f,  0.22757201f,  0.13491443f,  0.04160725f, -0.05162289f, -0.14405259f,
    -0.23496747f, -0.32365763f, -0.40943310f, -0.49162835f, -0.56960684f, -0.64276534f, -0.71053839f, -0.77240247f,
    -0.82788002f, -0.87654293f, -0.91801590f, -0.95197952f, -0.97817266f, -0.99639434f, -1.00000000f, -1.00000000f,
    -1.00000000f, -1.00000000f, -0.98619574f, -0.95683235f, -0.91946030f, -0.87465876f, -0.82305384f, -0.76531261f,
    -0.70213687f, -0.63425654f, -0.56242287f, -0.48740238f, -0.40997028f, -0.33090448f, -0.25097942f, -0.17096011f,
    -0.09159607f, -0.01361547f,  0.06228047f,  0.13542300f,  0.20518118f,  0.27096623f,  0.33223474f,  0.38849288f,
     0.43929994f,  0.48427165f,  0.52308297f,  0.55547059f,  0.58123446f,  0.60023922f,  0.61241436f,  0.61775386f,
     0.61631531f,  0.60821825f,  0.59364212f,  0.57282364f,  0.54605359f,  0.51367313f,  0.47606987f,  0.43367326f,
     0.38694978f,  0.33639827f,  0.28254503f,  0.22593877f,  0.16714543f,  0.10674264f,  0.04531438f, -0.01655436f,
    -0.07828290f, -0.13929999f, -0.19903925f, -0.25695240f, -0.31251344f, -0.36522245f, -0.41460919f, -0.46023634f,
    -0.50170255f, -0.53864521f, -0.57074285f, -0.59771717f, -0.61933464f, -0.63540798f, -0.64579695f, -0.65040880f,
    -0.64919794f, -0.64216560f, -0.62935907f, -0.61087024f, -0.58683461f, -0.55742943f, -0.52287185f, -0.48341686f,
    -0.43935567f
};

/* 6 Inharmonic Metallic Square Ratios for Track 3 Hi-Hat (205..800 Hz base) */
static const float k_hat_metallic_hz[SP1_DSP_HAT_NUM_OSC] = {
    205.0f, 304.0f, 369.0f, 522.0f, 540.0f, 800.0f
};

static inline float clamp01(float x) {
    if (x < 0.0f) {
        return 0.0f;
    }
    if (x > 1.0f) {
        return 1.0f;
    }
    return x;
}

/* ── Cytomic TPT SVF Coefficient Update (Control/Block Rate Only) ────────── */

static inline float fast_tan_pi_control(float w) {
    const float x = 3.14159265f * w;
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - (6.0f * x2));
}

void sp1_svf_set_params(sp1_svf_t *f, float cutoff_hz, float q) {
    if (f == NULL) {
        return;
    }

    float w = cutoff_hz * SP1_DSP_INV_FS_HZ;
    if (w < 0.0004f) {
        w = 0.0004f; /* ~20 Hz floor */
    }
    if (w > 0.42f) {
        w = 0.42f;   /* 20.16 kHz ceiling (< Nyquist) */
    }

    if (q < 0.5f) {
        q = 0.5f;
    }
    if (q > 12.0f) {
        q = 12.0f;
    }

    const float g = fast_tan_pi_control(w);
    const float k = 1.0f / q;
    f->k = k;
    f->a1 = 1.0f / (1.0f + (g * (g + k)));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

/* ── Control-Rate Parameter Helpers ──────────────────────────────────────── */

static void update_kick_coeffs(sp1_kick_voice_t *k) {
    /* Morph amp decay from ~110 ms (0.99935f) to ~290 ms (0.99975f) */
    k->amp_decay_coeff = 0.99935f + (0.00040f * k->decay_param);
    /* Exponential pitch sweep (200 Hz -> 45 Hz) with tau ~ 14 ms */
    k->pitch_decay_coeff = 0.99820f;
    /* Fast transient phase-distortion punch with tau ~ 2.2 ms */
    k->click_decay_coeff = 0.99050f;
    k->base_freq_hz = 45.0f;
}

static void update_snare_coeffs(sp1_snare_voice_t *s) {
    const float m = s->tone_morph;
    /* Rimshot (m = 0.0f): tight woody dual-resonator decay (~22 ms -> 0.99885f),
     * high rim overtone (~465 Hz), and short/minimal wire noise (~14 ms -> 0.99820f).
     * Snare (m = 1.0f): fuller drum body (~55 ms -> 0.99945f), classic ~335 Hz overtone,
     * and extended snappy wire tail (~125 ms -> 0.99968f). */
    s->body_decay_coeff = 0.99885f + (0.00060f * m);
    s->noise_decay_coeff = 0.99820f + (0.00148f * m);
    s->knock_decay_coeff = 0.99350f; /* ~3.2 ms woody transient pitch knock */
    s->rim_freq_hz = 465.0f - (130.0f * m);

    const float svf_cutoff = 1900.0f + (950.0f * m);
    const float svf_q = 2.4f - (0.8f * m);
    sp1_svf_set_params(&s->wire_svf, svf_cutoff, svf_q);
}

static void update_hat_coeffs(sp1_hat_voice_t *h) {
    const float m = h->decay_morph;
    /* Closed Hat (m = 0.0f): ~24 ms decay (0.99895f).
     * Open Hat   (m = 1.0f): ~260 ms decay (0.99988f). */
    h->amp_decay_coeff = 0.99895f + (0.00093f * m);
    sp1_svf_set_params(&h->hp_svf, 5600.0f, 2.25f);
}

static void update_perc_coeffs(sp1_perc_voice_t *p) {
    const float m = p->timbre_morph;
    p->amp_decay_coeff = 0.99915f + (0.00035f * m);
    p->pitch_decay_coeff = 0.99680f;
    p->index_decay_coeff = 0.99840f;
    p->carrier_base_hz = 290.0f + (190.0f * m);
    p->pitch_span_hz = 240.0f + (220.0f * m);
    /* Inharmonic metallic FM ratio (1.414 .. 2.718) */
    p->mod_ratio = 1.4142135f + (1.304f * m);

    float mod_scale = (3.5e8f + (1.35e9f * m));
    if (mod_scale > SP1_DSP_MAX_MOD_PHASE_SCALE) {
        mod_scale = SP1_DSP_MAX_MOD_PHASE_SCALE;
    }
    p->mod_phase_scale = mod_scale;

    float fb_scale = (1.2e8f + (3.2e8f * m));
    if (fb_scale > SP1_DSP_MAX_FB_PHASE_SCALE) {
        fb_scale = SP1_DSP_MAX_FB_PHASE_SCALE;
    }
    p->fb_phase_scale = fb_scale;
}

/* ── Division-Free Register-Resident Voice Segment Renderers ─────────────── *
 * Rendering one voice at a time across a contiguous [start, end) segment keeps
 * each voice's inner loop well inside the nRF52840 2 KB Flash ICACHE and keeps
 * hot phase/envelope/SVF state in Cortex-M4F registers with zero stack spills. */

#if defined(__GNUC__) || defined(__clang__)
#define SP1_DSP_NOINLINE __attribute__((noinline))
#else
#define SP1_DSP_NOINLINE
#endif

static SP1_DSP_NOINLINE void render_kick_segment(sp1_kick_voice_t *v,
                                                 float *out_main_l,
                                                 float *out_main_r,
                                                 uint32_t start,
                                                 uint32_t end) {
    if (!v->active || start >= end) {
        return;
    }

    uint32_t phase = v->phase;
    float amp_env = v->amp_env;
    float pitch_env = v->pitch_env;
    float click_env = v->click_env;

    const float amp_decay = v->amp_decay_coeff;
    const float pitch_decay = v->pitch_decay_coeff;
    const float click_decay = v->click_decay_coeff;
    const uint32_t base_inc = sp1_hz_to_phase_inc(v->base_freq_hz);
    const float pitch_span_inc_f = v->pitch_span_hz * SP1_DSP_PHASE_SCALE;
    const float pd_depth_scale = v->pd_depth_scale;
    const float drive_param = v->drive_param;
    const float drive_gain = 1.15f + (drive_param * 1.15f);
    const float fold_mix = drive_param * 0.30f;
    const bool use_fold = (drive_param > 0.01f);
    const float vel_gain = v->velocity_gain;
    bool active = true;

    for (uint32_t i = start; i < end; ++i) {
        if (pitch_env > 0.0f) {
            phase += base_inc + (uint32_t)(pitch_span_inc_f * pitch_env);
            pitch_env *= pitch_decay;
            if (pitch_env < SP1_DSP_ENV_FLOOR) {
                pitch_env = 0.0f;
            }
        } else {
            phase += base_inc;
        }

        uint32_t pd_offset = 0U;
        float click_chirp = 0.0f;
        if (click_env > 0.0f) {
            pd_offset = (uint32_t)((int32_t)(click_env * pd_depth_scale));
            click_chirp = sp1_sine_lut((phase << 2U) + pd_offset) * click_env * 0.26f;
            click_env *= click_decay;
            if (click_env < SP1_DSP_ENV_FLOOR) {
                click_env = 0.0f;
            }
        }

        const float sub_sine = sp1_sine_lut(phase + pd_offset);
        const float sub_tri = sp1_triangle_osc(phase);
        const float core = (sub_sine * 0.85f) + (sub_tri * 0.15f) + click_chirp;
        const float pre_drive = core * drive_gain;

        const float sat = sp1_soft_clip(pre_drive);
        float shaped = sat;
        if (use_fold) {
            const float folded = sp1_wavefold_lut(pre_drive * 0.75f);
            shaped = sat + ((folded - sat) * fold_mix);
        }

        const float out = shaped * amp_env * vel_gain;
        out_main_l[i] += out;
        out_main_r[i] += out;

        amp_env *= amp_decay;
        if (amp_env < SP1_DSP_ENV_FLOOR) {
            amp_env = 0.0f;
            pitch_env = 0.0f;
            click_env = 0.0f;
            active = false;
            break;
        }
    }

    v->phase = phase;
    v->amp_env = amp_env;
    v->pitch_env = pitch_env;
    v->click_env = click_env;
    v->active = active;
}

static SP1_DSP_NOINLINE void render_snare_segment(sp1_snare_voice_t *v,
                                                  float *out_main_l,
                                                  float *out_main_r,
                                                  float *out_send_l,
                                                  float *out_send_r,
                                                  uint32_t start,
                                                  uint32_t end) {
    if (!v->active || start >= end) {
        return;
    }

    uint32_t phase_body = v->phase_body;
    uint32_t phase_rim = v->phase_rim;
    uint32_t rng = v->rng;
    float ic1 = v->wire_svf.ic1eq;
    float ic2 = v->wire_svf.ic2eq;
    const float a1 = v->wire_svf.a1;
    const float a2 = v->wire_svf.a2;
    const float a3 = v->wire_svf.a3;
    const float svf_k = v->wire_svf.k;

    float body_env = v->body_env;
    float noise_env = v->noise_env;
    float knock_env = v->knock_env;

    const float body_decay = v->body_decay_coeff;
    const float noise_decay = v->noise_decay_coeff;
    const float knock_decay = v->knock_decay_coeff;
    const float m = v->tone_morph;
    const float body_weight = 0.45f + (0.30f * m);
    const float rim_weight = 0.85f - (0.50f * m);
    const float knock_m = (1.0f - m) * 0.85f;
    const float rim_leak = 0.12f * (1.0f - m);
    const float wire_gain = 0.05f + (0.75f * m);
    const float vel_gain = v->velocity_gain;

    const uint32_t body_base_inc = sp1_hz_to_phase_inc(185.0f);
    const uint32_t rim_base_inc = sp1_hz_to_phase_inc(v->rim_freq_hz);
    const float body_knock_inc_f = 135.0f * SP1_DSP_PHASE_SCALE;
    const float rim_knock_inc_f = 245.0f * SP1_DSP_PHASE_SCALE;

    bool active = true;
    uint32_t i = start;

    /* Phase 1: Dual-resonator body + wire noise (active only during initial 22..55 ms) */
    while (i < end && body_env > 0.0f) {
        if (knock_env > 0.0f) {
            phase_body += body_base_inc + (uint32_t)(body_knock_inc_f * knock_env);
            phase_rim += rim_base_inc + (uint32_t)(rim_knock_inc_f * knock_env);
        } else {
            phase_body += body_base_inc;
            phase_rim += rim_base_inc;
        }

        const float osc_body =
            (0.65f * sp1_sine_lut(phase_body)) + (0.35f * sp1_triangle_osc(phase_body));
        const float osc_rim =
            (0.55f * sp1_triangle_osc(phase_rim)) + (0.45f * sp1_sine_lut(phase_rim));

        const float knock_boost = 1.0f + (knock_m * knock_env);
        const float resonator_raw =
            ((osc_body * body_weight) + (osc_rim * rim_weight)) * knock_boost;
        const float resonator = sp1_soft_clip(resonator_raw * 1.25f) * body_env;

        const float v0 = sp1_xorshift32_noise(&rng) + (osc_rim * rim_leak);
        const float v3 = v0 - ic2;
        const float v1 = (a1 * ic1) + (a2 * v3);
        const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
        ic1 = (2.0f * v1) - ic1;
        ic2 = (2.0f * v2) - ic2;
        const float svf_hp = v0 - (svf_k * v1) - v2;

        const float wire_sig = ((v1 * 0.62f) + (svf_hp * 0.52f)) * noise_env * wire_gain;
        const float out = sp1_soft_clip(resonator + wire_sig) * vel_gain;

        out_main_l[i] += out * 0.96f;
        out_main_r[i] += out * 1.02f;
        if (out_send_l != NULL) {
            out_send_l[i] += out * 0.55f;
        }
        if (out_send_r != NULL) {
            out_send_r[i] += out * 0.55f;
        }

        body_env *= body_decay;
        if (body_env < SP1_DSP_ENV_FLOOR) {
            body_env = 0.0f;
        }
        noise_env *= noise_decay;
        if (knock_env > 0.0f) {
            knock_env *= knock_decay;
            if (knock_env < SP1_DSP_ENV_FLOOR) {
                knock_env = 0.0f;
            }
        }

        ++i;
        if (body_env == 0.0f && noise_env < SP1_DSP_ENV_FLOOR) {
            noise_env = 0.0f;
            knock_env = 0.0f;
            active = false;
            break;
        }
    }

    /* Phase 2: Noise-only wire tail (tight register-only loop, zero oscillator work) */
    if (active) {
        const float wire_scale = wire_gain;
        for (; i < end; ++i) {
            const float v0 = sp1_xorshift32_noise(&rng);
            const float v3 = v0 - ic2;
            const float v1 = (a1 * ic1) + (a2 * v3);
            const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
            ic1 = (2.0f * v1) - ic1;
            ic2 = (2.0f * v2) - ic2;
            const float svf_hp = v0 - (svf_k * v1) - v2;

            const float wire_sig = ((v1 * 0.62f) + (svf_hp * 0.52f)) * noise_env * wire_scale;
            const float out = sp1_soft_clip(wire_sig) * vel_gain;

            out_main_l[i] += out * 0.96f;
            out_main_r[i] += out * 1.02f;
            if (out_send_l != NULL) {
                out_send_l[i] += out * 0.55f;
            }
            if (out_send_r != NULL) {
                out_send_r[i] += out * 0.55f;
            }

            noise_env *= noise_decay;
            if (noise_env < SP1_DSP_ENV_FLOOR) {
                noise_env = 0.0f;
                knock_env = 0.0f;
                active = false;
                break;
            }
        }
    }

#if !defined(__ARM_FP) || (__ARM_FP == 0)
    if (fabsf(ic1) < SP1_DSP_DENORMAL_FLOOR) {
        ic1 = 0.0f;
    }
    if (fabsf(ic2) < SP1_DSP_DENORMAL_FLOOR) {
        ic2 = 0.0f;
    }
#endif

    v->phase_body = phase_body;
    v->phase_rim = phase_rim;
    v->rng = rng;
    v->wire_svf.ic1eq = ic1;
    v->wire_svf.ic2eq = ic2;
    v->body_env = body_env;
    v->noise_env = noise_env;
    v->knock_env = knock_env;
    v->active = active;
}

static SP1_DSP_NOINLINE void render_hat_segment(sp1_hat_voice_t *v,
                                                float *out_main_l,
                                                float *out_main_r,
                                                uint32_t start,
                                                uint32_t end) {
    if (!v->active || start >= end) {
        return;
    }

    uint32_t p0 = v->phase[0];
    uint32_t p1 = v->phase[3];
    uint32_t p2 = v->phase[5];
    const uint32_t inc0 = v->phase_inc[0];
    const uint32_t inc1 = v->phase_inc[3];
    const uint32_t inc2 = v->phase_inc[5];

    uint32_t rng = v->rng;
    float ic1 = v->hp_svf.ic1eq;
    float ic2 = v->hp_svf.ic2eq;
    const float a1 = v->hp_svf.a1;
    const float a2 = v->hp_svf.a2;
    const float a3 = v->hp_svf.a3;
    const float svf_k = v->hp_svf.k;

    float amp_env = v->amp_env;
    const float amp_decay = v->amp_decay_coeff;
    const float vel_gain = v->velocity_gain;
    bool active = true;

    for (uint32_t i = start; i < end; ++i) {
        /* 3 inharmonic metallic square oscillators (205, 522, 800 Hz) + 8x harmonic tap */
        p0 += inc0;
        p1 += inc1;
        p2 += inc2;

        int32_t metal_i = (((int32_t)(p0 >> 31U) * 2) - 1) * 4 +
                          (((int32_t)((p0 >> 28U) & 1U) * 2) - 1) * 6;
        metal_i += (((int32_t)(p1 >> 31U) * 2) - 1) * 4 +
                   (((int32_t)((p1 >> 28U) & 1U) * 2) - 1) * 6;
        metal_i += (((int32_t)(p2 >> 31U) * 2) - 1) * 4 +
                   (((int32_t)((p2 >> 28U) & 1U) * 2) - 1) * 6;

        /* 0.20f * 0.18f = 0.036f combined metallic mix gain */
        const float v0 = ((float)metal_i * 0.036f) + (sp1_xorshift32_noise(&rng) * 0.35f);

        const float v3 = v0 - ic2;
        const float v1 = (a1 * ic1) + (a2 * v3);
        const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
        ic1 = (2.0f * v1) - ic1;
        ic2 = (2.0f * v2) - ic2;
        const float hp_out = v0 - (svf_k * v1) - v2;

        const float out = sp1_soft_clip((hp_out * 0.82f) + (v1 * 0.32f)) * amp_env * vel_gain;
        out_main_l[i] += out * 0.76f;
        out_main_r[i] += out * 1.06f;

        amp_env *= amp_decay;
        if (amp_env < SP1_DSP_ENV_FLOOR) {
            amp_env = 0.0f;
            active = false;
            break;
        }
    }

#if !defined(__ARM_FP) || (__ARM_FP == 0)
    if (fabsf(ic1) < SP1_DSP_DENORMAL_FLOOR) {
        ic1 = 0.0f;
    }
    if (fabsf(ic2) < SP1_DSP_DENORMAL_FLOOR) {
        ic2 = 0.0f;
    }
#endif

    v->phase[0] = p0;
    v->phase[3] = p1;
    v->phase[5] = p2;
    v->rng = rng;
    v->hp_svf.ic1eq = ic1;
    v->hp_svf.ic2eq = ic2;
    v->amp_env = amp_env;
    v->active = active;
}

static SP1_DSP_NOINLINE void render_perc_segment(sp1_perc_voice_t *v,
                                                 float *out_main_l,
                                                 float *out_main_r,
                                                 float *out_send_l,
                                                 float *out_send_r,
                                                 uint32_t start,
                                                 uint32_t end) {
    if (!v->active || start >= end) {
        return;
    }

    uint32_t carrier_phase = v->carrier_phase;
    uint32_t mod_phase = v->mod_phase;
    float prev_mod = v->prev_mod;
    float amp_env = v->amp_env;
    float pitch_env = v->pitch_env;
    float index_env = v->index_env;

    const float amp_decay = v->amp_decay_coeff;
    const float pitch_decay = v->pitch_decay_coeff;
    const float index_decay = v->index_decay_coeff;
    const uint32_t carrier_base_inc = sp1_hz_to_phase_inc(v->carrier_base_hz);
    const float carrier_span_inc_f = v->pitch_span_hz * SP1_DSP_PHASE_SCALE;
    const uint32_t mod_base_inc = sp1_hz_to_phase_inc(v->carrier_base_hz * v->mod_ratio);
    const float mod_span_inc_f = (v->pitch_span_hz * v->mod_ratio) * SP1_DSP_PHASE_SCALE;

    const float fb_scale = v->fb_phase_scale;
    const float mod_scale = v->mod_phase_scale;
    const float timbre_scale = v->timbre_morph * 2.10f;
    const float vel_gain = v->velocity_gain;
    bool active = true;

    for (uint32_t i = start; i < end; ++i) {
        if (pitch_env > 0.0f) {
            carrier_phase += carrier_base_inc + (uint32_t)(carrier_span_inc_f * pitch_env);
            mod_phase += mod_base_inc + (uint32_t)(mod_span_inc_f * pitch_env);
            pitch_env *= pitch_decay;
            if (pitch_env < SP1_DSP_ENV_FLOOR) {
                pitch_env = 0.0f;
            }
        } else {
            carrier_phase += carrier_base_inc;
            mod_phase += mod_base_inc;
        }

        /* |prev_mod| <= 1.0f, 0 <= index_env <= 1.0f, fb_scale <= 5e8f (< INT32_MAX) */
        const uint32_t fb_offset = (uint32_t)((int32_t)(prev_mod * fb_scale * index_env));
        const float mod_sample = sp1_sine_lut(mod_phase + fb_offset);
        prev_mod = mod_sample;

        /* |mod_sample| <= 1.0f, 0 <= index_env <= 1.0f, mod_scale <= 2e9f (< INT32_MAX) */
        const uint32_t pm_offset = (uint32_t)((int32_t)(mod_sample * mod_scale * index_env));
        const float carrier = sp1_sine_lut(carrier_phase + pm_offset);

        const float fold_drive = 1.10f + (timbre_scale * index_env);
        const float out = sp1_wavefold_lut(carrier * fold_drive) * amp_env * vel_gain;

        out_main_l[i] += out * 1.05f;
        out_main_r[i] += out * 0.78f;
        if (out_send_l != NULL) {
            out_send_l[i] += out * 0.70f;
        }
        if (out_send_r != NULL) {
            out_send_r[i] += out * 0.50f;
        }

        amp_env *= amp_decay;
        index_env *= index_decay;
        if (amp_env < SP1_DSP_ENV_FLOOR) {
            amp_env = 0.0f;
            pitch_env = 0.0f;
            index_env = 0.0f;
            active = false;
            break;
        }
    }

    v->carrier_phase = carrier_phase;
    v->mod_phase = mod_phase;
    v->prev_mod = prev_mod;
    v->amp_env = amp_env;
    v->pitch_env = pitch_env;
    v->index_env = index_env;
    v->active = active;
}

/* ── Public API Implementation ───────────────────────────────────────────── */

void sp1_drum_voices_init(sp1_drum_synth_t *synth) {
    if (synth == NULL) {
        return;
    }
    memset(synth, 0, sizeof(*synth));

    /* Track 1: Kick defaults */
    synth->kick.decay_param = 0.45f;
    synth->kick.drive_param = 0.35f;
    synth->kick.pitch_span_hz = 155.0f; /* 200 Hz -> 45 Hz */
    synth->kick.pd_depth_scale = 4.2e8f;
    update_kick_coeffs(&synth->kick);

    /* Track 2: Snare / Rimshot defaults (0.65 = snappy snare with woody rim transient) */
    synth->snare.rng = 0x2468ACE1U;
    synth->snare.tone_morph = 0.65f;
    sp1_svf_reset(&synth->snare.wire_svf);
    update_snare_coeffs(&synth->snare);

    /* Track 3: 6-Oscillator Inharmonic Metallic Hi-Hat defaults */
    synth->hat.rng = 0xA5A55A5AU;
    synth->hat.decay_morph = 0.20f;
    for (uint32_t i = 0U; i < SP1_DSP_HAT_NUM_OSC; ++i) {
        synth->hat.phase_inc[i] = sp1_hz_to_phase_inc(k_hat_metallic_hz[i]);
    }
    sp1_svf_reset(&synth->hat.hp_svf);
    update_hat_coeffs(&synth->hat);

    /* Track 4: 2-Op FM / Wavefold Perc defaults */
    synth->perc.timbre_morph = 0.45f;
    update_perc_coeffs(&synth->perc);
}

void sp1_drum_voices_set_kick_params(sp1_drum_synth_t *synth,
                                     float decay_0_to_1,
                                     float drive_0_to_1) {
    if (synth == NULL) {
        return;
    }
    synth->kick.decay_param = clamp01(decay_0_to_1);
    synth->kick.drive_param = clamp01(drive_0_to_1);
    update_kick_coeffs(&synth->kick);
}

void sp1_drum_voices_set_snare_morph(sp1_drum_synth_t *synth,
                                     float morph_0_rim_to_1_snare) {
    if (synth == NULL) {
        return;
    }
    synth->snare.tone_morph = clamp01(morph_0_rim_to_1_snare);
    update_snare_coeffs(&synth->snare);
}

void sp1_drum_voices_set_hat_decay(sp1_drum_synth_t *synth,
                                   float decay_0_closed_to_1_open) {
    if (synth == NULL) {
        return;
    }
    synth->hat.decay_morph = clamp01(decay_0_closed_to_1_open);
    update_hat_coeffs(&synth->hat);
}

void sp1_drum_voices_set_perc_timbre(sp1_drum_synth_t *synth,
                                     float timbre_0_to_1) {
    if (synth == NULL) {
        return;
    }
    synth->perc.timbre_morph = clamp01(timbre_0_to_1);
    update_perc_coeffs(&synth->perc);
}

void sp1_drum_voices_trigger(sp1_drum_synth_t *synth,
                             uint8_t track,
                             uint8_t velocity,
                             bool accent) {
    if (synth == NULL || velocity == 0U) {
        return;
    }

    const float vel_norm = (float)(velocity & 0x7FU) * SP1_DSP_INV_127;
    const float accent_boost = accent ? 1.15f : 1.0f;
    const float gain = vel_norm * accent_boost;

    switch (track) {
    case 0U: { /* Track 1: Kick / Sub (200 Hz -> 45 Hz) */
        sp1_kick_voice_t *k = &synth->kick;
        k->phase = 0U;
        k->amp_env = 1.0f;
        k->pitch_env = 1.0f;
        k->click_env = 1.0f;
        k->pitch_span_hz = accent ? 175.0f : 155.0f;
        k->velocity_gain = gain * 0.72f;
        k->active = true;
        break;
    }
    case 1U: { /* Track 2: Snare / Rimshot */
        sp1_snare_voice_t *s = &synth->snare;
        s->phase_body = 0U;
        s->phase_rim = 0x20000000U; /* 45-degree initial phase offset for crisp rim knock */
        s->body_env = 1.0f;
        s->noise_env = 1.0f;
        s->knock_env = 1.0f;
        s->velocity_gain = gain * 0.58f;
        s->active = true;
        break;
    }
    case 2U: { /* Track 3: 6-Oscillator Inharmonic Metallic Hi-Hat */
        sp1_hat_voice_t *h = &synth->hat;
        h->amp_env = 1.0f;
        h->velocity_gain = gain * 0.42f;
        h->active = true;
        break;
    }
    case 3U: { /* Track 4: 2-Op FM / Wavefold Perc */
        sp1_perc_voice_t *p = &synth->perc;
        p->carrier_phase = 0U;
        p->mod_phase = 0U;
        p->prev_mod = 0.0f;
        p->amp_env = 1.0f;
        p->pitch_env = 1.0f;
        p->index_env = 1.0f;
        p->velocity_gain = gain * 0.48f;
        p->active = true;
        break;
    }
    default:
        break;
    }
}

void sp1_drum_voices_render_block(sp1_drum_synth_t *synth,
                                  const sp1_seq_trigger_event_t *triggers,
                                  uint32_t num_triggers,
                                  float *out_main_l,
                                  float *out_main_r,
                                  float *out_send_l,
                                  float *out_send_r,
                                  uint32_t num_frames) {
    if (synth == NULL || out_main_l == NULL || out_main_r == NULL || num_frames == 0U) {
        return;
    }

    const size_t bytes = (size_t)num_frames * sizeof(float);
    memset(out_main_l, 0, bytes);
    memset(out_main_r, 0, bytes);
    if (out_send_l != NULL) {
        memset(out_send_l, 0, bytes);
    }
    if (out_send_r != NULL) {
        memset(out_send_r, 0, bytes);
    }

    if (num_triggers == 0U &&
        !synth->kick.active && !synth->snare.active &&
        !synth->hat.active && !synth->perc.active) {
        return;
    }

    uint32_t next_trig = 0U;
    uint32_t cursor = 0U;

    while (cursor < num_frames) {
        /* Dispatch all triggers scheduled at or before cursor */
        while (triggers != NULL && next_trig < num_triggers &&
               triggers[next_trig].block_offset <= cursor) {
            const sp1_seq_trigger_event_t *ev = &triggers[next_trig];
            sp1_drum_voices_trigger(synth, ev->track, ev->velocity, ev->accent);
            next_trig++;
        }

        uint32_t seg_end = num_frames;
        if (triggers != NULL && next_trig < num_triggers) {
            const uint32_t next_off = (uint32_t)triggers[next_trig].block_offset;
            if (next_off < seg_end) {
                seg_end = next_off;
            }
        }

        if (seg_end > cursor) {
            /* Render each active voice across [cursor, seg_end) with full ICACHE residency */
            if (synth->kick.active) {
                render_kick_segment(&synth->kick, out_main_l, out_main_r, cursor, seg_end);
            }
            if (synth->snare.active) {
                render_snare_segment(&synth->snare, out_main_l, out_main_r,
                                     out_send_l, out_send_r, cursor, seg_end);
            }
            if (synth->hat.active) {
                render_hat_segment(&synth->hat, out_main_l, out_main_r, cursor, seg_end);
            }
            if (synth->perc.active) {
                render_perc_segment(&synth->perc, out_main_l, out_main_r,
                                    out_send_l, out_send_r, cursor, seg_end);
            }
            cursor = seg_end;
        }
    }

    /* Fire any trailing triggers clamped past num_frames - 1 */
    while (triggers != NULL && next_trig < num_triggers) {
        const sp1_seq_trigger_event_t *ev = &triggers[next_trig];
        sp1_drum_voices_trigger(synth, ev->track, ev->velocity, ev->accent);
        next_trig++;
    }
}
