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

/* ── Division-Free Per-Sample Voice Renderers ────────────────────────────── */

static inline float render_kick_sample(sp1_kick_voice_t *v) {
    if (!v->active) {
        return 0.0f;
    }

    if (v->pitch_env > 0.0f) {
        const float freq_hz = v->base_freq_hz + (v->pitch_span_hz * v->pitch_env);
        v->phase += sp1_hz_to_phase_inc(freq_hz);
    } else {
        /* 45.0 Hz * (2^32 / 48000) = 4,026,532 */
        v->phase += 4026532U;
    }

    /* Transient phase-distortion punch (clamped < INT32_MAX) */
    uint32_t pd_offset = 0U;
    float click_chirp = 0.0f;
    if (v->click_env > 0.0f) {
        pd_offset = (uint32_t)((int32_t)(v->click_env * v->pd_depth_scale));
        click_chirp = sp1_sine_lut((v->phase << 2U) + pd_offset) * v->click_env * 0.26f;
    }
    const float sub_sine = sp1_sine_lut(v->phase + pd_offset);
    const float sub_tri = sp1_triangle_osc(v->phase);

    const float core = (sub_sine * 0.85f) + (sub_tri * 0.15f) + click_chirp;
    const float pre_drive = core * (1.15f + (v->drive_param * 1.15f));

    /* Blend division-free cubic tanh soft-clipper with West-Coast wavefolder LUT */
    const float sat = sp1_soft_clip(pre_drive);
    float shaped = sat;
    if (v->drive_param > 0.01f) {
        const float folded = sp1_wavefold_lut(pre_drive * 0.75f);
        shaped = sat + ((folded - sat) * (v->drive_param * 0.30f));
    }

    const float out = shaped * v->amp_env * v->velocity_gain;

    v->amp_env *= v->amp_decay_coeff;
    v->pitch_env *= v->pitch_decay_coeff;
    v->click_env *= v->click_decay_coeff;
    if (v->click_env < SP1_DSP_ENV_FLOOR) {
        v->click_env = 0.0f;
    }
    if (v->pitch_env < SP1_DSP_ENV_FLOOR) {
        v->pitch_env = 0.0f;
    }

    if (v->amp_env < SP1_DSP_ENV_FLOOR) {
        v->amp_env = 0.0f;
        v->pitch_env = 0.0f;
        v->click_env = 0.0f;
        v->active = false;
    }
    return out;
}

static inline float render_snare_sample(sp1_snare_voice_t *v) {
    if (!v->active) {
        return 0.0f;
    }

    const float m = v->tone_morph;

    float osc_rim = 0.0f;
    float resonator = 0.0f;
    if (v->body_env > 0.0f) {
        /* Dual detuned resonators:
         * 1. ~185 Hz shell body + fast pitch-transient knock
         * 2. ~330-480 Hz woody rim overtone + sharp pitch-transient knock */
        const float body_hz = 185.0f + (135.0f * v->knock_env);
        const float rim_hz = v->rim_freq_hz + (245.0f * v->knock_env);

        v->phase_body += sp1_hz_to_phase_inc(body_hz);
        v->phase_rim += sp1_hz_to_phase_inc(rim_hz);

        const float osc_body =
            (0.65f * sp1_sine_lut(v->phase_body)) + (0.35f * sp1_triangle_osc(v->phase_body));
        osc_rim =
            (0.55f * sp1_triangle_osc(v->phase_rim)) + (0.45f * sp1_sine_lut(v->phase_rim));

        const float body_weight = 0.45f + (0.30f * m);
        const float rim_weight = 0.85f - (0.50f * m);
        const float knock_boost = 1.0f + ((1.0f - m) * 0.85f * v->knock_env);
        const float resonator_raw =
            ((osc_body * body_weight) + (osc_rim * rim_weight)) * knock_boost;
        resonator = sp1_soft_clip(resonator_raw * 1.25f) * v->body_env;
    }

    /* Signed xorshift32 noise filtered through Cytomic TPT SVF bandpass + highpass */
    const float raw_noise = sp1_xorshift32_noise(&v->rng);
    float svf_bp = 0.0f;
    float svf_hp = 0.0f;
    sp1_svf_process(&v->wire_svf, raw_noise + (osc_rim * 0.12f * (1.0f - m)), NULL, &svf_bp, &svf_hp);

    const float wire_gain = 0.05f + (0.75f * m);
    const float wire_sig = ((svf_bp * 0.62f) + (svf_hp * 0.52f)) * v->noise_env * wire_gain;

    const float out = sp1_soft_clip(resonator + wire_sig) * v->velocity_gain;

    v->body_env *= v->body_decay_coeff;
    if (v->body_env < SP1_DSP_ENV_FLOOR) {
        v->body_env = 0.0f;
    }
    v->noise_env *= v->noise_decay_coeff;
    v->knock_env *= v->knock_decay_coeff;
    if (v->knock_env < SP1_DSP_ENV_FLOOR) {
        v->knock_env = 0.0f;
    }

    if (v->body_env < SP1_DSP_ENV_FLOOR && v->noise_env < SP1_DSP_ENV_FLOOR) {
        v->body_env = 0.0f;
        v->noise_env = 0.0f;
        v->knock_env = 0.0f;
        v->active = false;
    }
    return out;
}

static inline float render_hat_sample(sp1_hat_voice_t *v) {
    if (!v->active) {
        return 0.0f;
    }

    /* Sum 3 inharmonic metallic square-wave oscillators (205, 522, 800 Hz)
     * with upper harmonic edge tap (8x) for crisp metallic shimmer. */
    int32_t metal_sum_i = 0;
    for (uint32_t i = 0U; i < 3U; ++i) {
        const uint32_t idx = (i == 0U) ? 0U : ((i == 1U) ? 3U : 5U);
        const uint32_t p = v->phase[idx] + v->phase_inc[idx];
        v->phase[idx] = p;
        const int32_t sq_fund = ((int32_t)(p >> 31U) * 2) - 1;
        const int32_t sq_oct3 = ((int32_t)((p >> 28U) & 1U) * 2) - 1;
        metal_sum_i += (sq_fund * 4) + (sq_oct3 * 6);
    }
    const float metal_sum = (float)metal_sum_i * 0.20f;

    const float air_noise = sp1_xorshift32_noise(&v->rng) * 0.35f;
    const float raw_mix = (metal_sum * 0.18f) + air_noise;

    float hp_out = 0.0f;
    float bp_out = 0.0f;
    sp1_svf_process(&v->hp_svf, raw_mix, NULL, &bp_out, &hp_out);

    const float out = sp1_soft_clip((hp_out * 0.82f) + (bp_out * 0.32f)) *
                      v->amp_env * v->velocity_gain;

    v->amp_env *= v->amp_decay_coeff;
    if (v->amp_env < SP1_DSP_ENV_FLOOR) {
        v->amp_env = 0.0f;
        v->active = false;
    }
    return out;
}

static inline float render_perc_sample(sp1_perc_voice_t *v) {
    if (!v->active) {
        return 0.0f;
    }

    const float carrier_hz = v->carrier_base_hz + (v->pitch_span_hz * v->pitch_env);
    const float mod_hz = carrier_hz * v->mod_ratio;

    v->carrier_phase += sp1_hz_to_phase_inc(carrier_hz);
    v->mod_phase += sp1_hz_to_phase_inc(mod_hz);

    /* Modulator self-feedback with clamped fb_phase_scale <= 5.0e8f */
    float fb_scaled = v->prev_mod * v->fb_phase_scale * v->index_env;
    if (fb_scaled > SP1_DSP_MAX_FB_PHASE_SCALE) {
        fb_scaled = SP1_DSP_MAX_FB_PHASE_SCALE;
    } else if (fb_scaled < -SP1_DSP_MAX_FB_PHASE_SCALE) {
        fb_scaled = -SP1_DSP_MAX_FB_PHASE_SCALE;
    }
    const uint32_t fb_offset = (uint32_t)((int32_t)fb_scaled);
    const float mod_sample = sp1_sine_lut(v->mod_phase + fb_offset);
    v->prev_mod = mod_sample;

    /* Clamped mod_phase_scale <= 2.0e9f (< INT32_MAX) avoids float-to-int32 UB */
    float pm_scaled = mod_sample * v->mod_phase_scale * v->index_env;
    if (pm_scaled > SP1_DSP_MAX_MOD_PHASE_SCALE) {
        pm_scaled = SP1_DSP_MAX_MOD_PHASE_SCALE;
    } else if (pm_scaled < -SP1_DSP_MAX_MOD_PHASE_SCALE) {
        pm_scaled = -SP1_DSP_MAX_MOD_PHASE_SCALE;
    }
    const uint32_t pm_offset = (uint32_t)((int32_t)pm_scaled);

    const float carrier = sp1_sine_lut(v->carrier_phase + pm_offset);
    const float fold_drive = 1.10f + (v->timbre_morph * 2.10f * v->index_env);
    const float folded = sp1_wavefold_lut(carrier * fold_drive);

    const float out = folded * v->amp_env * v->velocity_gain;

    v->amp_env *= v->amp_decay_coeff;
    v->pitch_env *= v->pitch_decay_coeff;
    v->index_env *= v->index_decay_coeff;

    if (v->amp_env < SP1_DSP_ENV_FLOOR) {
        v->amp_env = 0.0f;
        v->pitch_env = 0.0f;
        v->index_env = 0.0f;
        v->active = false;
    }
    return out;
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

    if (num_triggers == 0U &&
        !synth->kick.active && !synth->snare.active &&
        !synth->hat.active && !synth->perc.active) {
        const size_t bytes = (size_t)num_frames * sizeof(float);
        memset(out_main_l, 0, bytes);
        memset(out_main_r, 0, bytes);
        if (out_send_l != NULL) {
            memset(out_send_l, 0, bytes);
        }
        if (out_send_r != NULL) {
            memset(out_send_r, 0, bytes);
        }
        return;
    }

    uint32_t next_trig = 0U;

    for (uint32_t i = 0U; i < num_frames; ++i) {
        /* Dispatch all triggers scheduled at or before intra-block sample offset i */
        while (triggers != NULL && next_trig < num_triggers &&
               triggers[next_trig].block_offset <= i) {
            const sp1_seq_trigger_event_t *ev = &triggers[next_trig];
            sp1_drum_voices_trigger(synth, ev->track, ev->velocity, ev->accent);
            next_trig++;
        }

        const float v_kick = render_kick_sample(&synth->kick);
        const float v_snare = render_snare_sample(&synth->snare);
        const float v_hat = render_hat_sample(&synth->hat);
        const float v_perc = render_perc_sample(&synth->perc);

        /* Stereo image placement:
         * - Kick: dead-center mono anchor
         * - Snare/Rimshot: centered with subtle stereo width
         * - Hi-Hat: panned slightly right
         * - 2-Op FM Perc: panned slightly left */
        out_main_l[i] = v_kick + (v_snare * 0.96f) + (v_hat * 0.76f) + (v_perc * 1.05f);
        out_main_r[i] = v_kick + (v_snare * 1.02f) + (v_hat * 1.06f) + (v_perc * 0.78f);

        /* Selective Dub Ping-Pong Delay send:
         * Fed exclusively by Track 2 (Snare/Rimshot) and Track 4 (2-Op FM Perc),
         * keeping Track 1 (Kick) low-end 100% dry and tight. */
        if (out_send_l != NULL) {
            out_send_l[i] = (v_snare * 0.55f) + (v_perc * 0.70f);
        }
        if (out_send_r != NULL) {
            out_send_r[i] = (v_snare * 0.55f) + (v_perc * 0.50f);
        }
    }

    /* Fire any trailing triggers clamped past num_frames - 1 */
    while (triggers != NULL && next_trig < num_triggers) {
        const sp1_seq_trigger_event_t *ev = &triggers[next_trig];
        sp1_drum_voices_trigger(synth, ev->track, ev->velocity, ev->accent);
        next_trig++;
    }
}
