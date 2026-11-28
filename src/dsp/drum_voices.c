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

/* ── Flash-Resident 257-Entry West-Coast Wavefolder LUT (1028 B in .rodata) ── */

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

/* 4 Curated Drum Synth Kit Presets in .rodata Flash (256 B, 0 B SRAM):
 *  - Kit 0: Classic 808 / 909 (Boot Default)
 *  - Kit 1: Dry Woody Rim & Tight Sub
 *  - Kit 2: Industrial FM & Buchla Metal
 *  - Kit 3: Lo-Fi Dub & Open Sizzle */
static const float k_drum_kit_presets[SP1_DRUM_NUM_KITS][SP1_SEQ_NUM_TRACKS][SP1_DRUM_NUM_VOICE_PARAMS] = {
    {   /* Kit 0: Classic 808 / 909 */
        { 0.50f, 0.45f, 0.50f, 0.35f }, /* Kick: 45 Hz, balanced punch & drive */
        { 0.50f, 0.65f, 0.65f, 0.50f }, /* Snare: crisp rim-snare */
        { 0.50f, 0.20f, 0.50f, 0.50f }, /* Hat: tight closed metallic 808 hat */
        { 0.50f, 0.45f, 0.45f, 0.45f }, /* Perc: balanced FM / wavefold */
    },
    {   /* Kit 1: Dry Woody Rim & Tight Sub */
        { 0.64f, 0.22f, 0.35f, 0.18f }, /* Kick: tight warm 54 Hz punch */
        { 0.52f, 0.08f, 0.15f, 0.35f }, /* Snare: pure woody 808 rimshot */
        { 0.66f, 0.08f, 0.65f, 0.30f }, /* Hat: ultra-short high metallic tick */
        { 0.38f, 0.12f, 0.28f, 0.12f }, /* Perc: hollow woody FM conga/clave */
    },
    {   /* Kit 2: Industrial FM & Buchla Metal */
        { 0.34f, 0.72f, 0.88f, 0.88f }, /* Kick: aggressive folded sub */
        { 0.72f, 0.82f, 0.75f, 0.84f }, /* Snare: bright metallic crack */
        { 0.22f, 0.55f, 0.32f, 0.15f }, /* Hat: dark clangorous gong cluster */
        { 0.68f, 0.88f, 0.68f, 0.92f }, /* Perc: gnarly West-Coast wavefold clang */
    },
    {   /* Kit 3: Lo-Fi Dub & Open Sizzle */
        { 0.20f, 0.84f, 0.28f, 0.45f }, /* Kick: deep 35 Hz sound-system sub */
        { 0.38f, 0.55f, 0.80f, 0.26f }, /* Snare: dark vintage dub snare */
        { 0.56f, 0.72f, 0.68f, 0.78f }, /* Hat: shimmering open sizzle hat */
        { 0.76f, 0.28f, 0.86f, 0.42f }, /* Perc: dub syndrum / laser sweep */
    },
};

#define SP1_DSP_BIPOLAR_LOW     0.45f
#define SP1_DSP_BIPOLAR_HIGH    0.55f
#define SP1_DSP_INV_BIPOLAR     2.22222222f /* 1.0f / 0.45f */

static inline float clamp01(float x) {
    if (x < 0.0f) {
        return 0.0f;
    }
    if (x > 1.0f) {
        return 1.0f;
    }
    return x;
}

/* Maps [0.0..1.0] with [0.45..0.55] center detent to [-1.0..+1.0]. */
static inline float bipolar_detent(float norm) {
    const float c = clamp01(norm);
    if (c >= SP1_DSP_BIPOLAR_LOW && c <= SP1_DSP_BIPOLAR_HIGH) {
        return 0.0f;
    }
    if (c < SP1_DSP_BIPOLAR_LOW) {
        return -((SP1_DSP_BIPOLAR_LOW - c) * SP1_DSP_INV_BIPOLAR);
    }
    return (c - SP1_DSP_BIPOLAR_HIGH) * SP1_DSP_INV_BIPOLAR;
}

/* ── Cytomic TPT SVF & 1-Pole Coefficient Updates (Control Rate Only) ────── */

static inline float fast_tan_pi_control(float w) {
    const float x = 3.14159265f * w;
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - (6.0f * x2));
}

static inline float fast_onepole_g_control(float cutoff_hz) {
    float w = cutoff_hz * SP1_DSP_INV_FS_HZ;
    if (w < 0.0005f) {
        w = 0.0005f;
    }
    if (w > 0.42f) {
        w = 0.42f;
    }
    const float g = fast_tan_pi_control(w);
    return g / (1.0f + g);
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
    const float tune_bi = bipolar_detent(k->tune_param);
    k->base_freq_hz = (tune_bi < 0.0f) ? (45.0f + (13.0f * tune_bi))   /* 32..45 Hz */
                                       : (45.0f + (30.0f * tune_bi));  /* 45..75 Hz */

    /* Morph amp decay from ~80 ms (0.99915f) to ~450 ms (0.99982f) */
    k->amp_decay_coeff = 0.99915f + (0.00067f * k->decay_param);
    k->pitch_decay_coeff = 0.99820f;
    k->click_decay_coeff = 0.99050f;
    k->pitch_span_base_hz = 65.0f + (180.0f * k->punch_param); /* 155 Hz at default 0.50 */
    k->pd_depth_scale = (1.2e8f + (6.0e8f * k->punch_param));  /* 4.2e8f at default 0.50 */
}

static void update_snare_coeffs(sp1_snare_voice_t *s) {
    const float pitch_bi = bipolar_detent(s->pitch_param);
    const float pitch_scale = (pitch_bi < 0.0f) ? (1.0f + (0.30f * pitch_bi))
                                                : (1.0f + (0.45f * pitch_bi));
    const float m = s->tone_morph;
    const float wd = s->wire_decay_param;
    const float wf = s->wire_filter_param;

    s->body_decay_coeff = 0.99885f + (0.00060f * m);
    s->noise_decay_coeff = 0.99820f + (0.00154f * wd);
    s->knock_decay_coeff = 0.99350f;
    s->body_freq_hz = 185.0f * pitch_scale;
    s->rim_freq_hz = (465.0f - (130.0f * m)) * pitch_scale;

    const float filter_offset = (wf - 0.50f) * 2400.0f;
    float svf_cutoff = 1900.0f + (950.0f * m) + filter_offset;
    if (svf_cutoff < 900.0f) {
        svf_cutoff = 900.0f;
    } else if (svf_cutoff > 5200.0f) {
        svf_cutoff = 5200.0f;
    }
    const float svf_q = 2.4f - (0.8f * m);
    sp1_svf_set_params(&s->wire_svf, svf_cutoff, svf_q);
}

static void update_hat_coeffs(sp1_hat_voice_t *h) {
    const float tune_bi = bipolar_detent(h->tune_param);
    const float tune_scale = (tune_bi < 0.0f) ? (1.0f + (0.35f * tune_bi))
                                               : (1.0f + (0.55f * tune_bi));
    for (uint32_t i = 0U; i < SP1_DSP_HAT_NUM_OSC; ++i) {
        h->phase_inc[i] = sp1_hz_to_phase_inc(k_hat_metallic_hz[i] * tune_scale);
    }

    const float m = h->decay_morph;
    h->amp_decay_coeff = 0.99895f + (0.00093f * m);

    const float cutoff = 2500.0f + (6200.0f * h->cutoff_param); /* 5,600 Hz at default 0.50 */
    sp1_svf_set_params(&h->hp_svf, cutoff, 2.25f);

    /* Default 0.50 -> metal_gain = 0.036f, noise_gain = 0.35f (matches original balance) */
    const float nm = h->noise_mix_param;
    h->metal_gain = 0.060f * (1.0f - (0.80f * nm));
    h->noise_gain = 0.70f * nm;
}

static void update_perc_coeffs(sp1_perc_voice_t *p) {
    const float pitch_bi = bipolar_detent(p->pitch_param);
    const float pitch_scale = (pitch_bi < 0.0f) ? (1.0f + (0.48f * pitch_bi))
                                                : (1.0f + (0.80f * pitch_bi));
    const float r = p->ratio_param;
    const float d = p->decay_param;
    const float m = p->timbre_morph;

    p->amp_decay_coeff = 0.99890f + (0.00075f * d);
    p->pitch_decay_coeff = 0.99580f + (0.00220f * d);
    p->index_decay_coeff = 0.99840f;
    p->carrier_base_hz = (290.0f + (190.0f * m)) * pitch_scale;
    p->pitch_span_hz = (140.0f + (320.0f * d)) * pitch_scale;
    p->mod_ratio = 1.00f + (2.40f * r);

    float mod_scale = (2.5e8f + (1.50e9f * m));
    if (mod_scale > SP1_DSP_MAX_MOD_PHASE_SCALE) {
        mod_scale = SP1_DSP_MAX_MOD_PHASE_SCALE;
    }
    p->mod_phase_scale = mod_scale;

    float fb_scale = (1.0e8f + (3.5e8f * m));
    if (fb_scale > SP1_DSP_MAX_FB_PHASE_SCALE) {
        fb_scale = SP1_DSP_MAX_FB_PHASE_SCALE;
    }
    p->fb_phase_scale = fb_scale;
}

/* ── Division-Free Register-Resident Voice Segment Renderers ─────────────── */

#if defined(__GNUC__) || defined(__clang__)
#define SP1_DSP_NOINLINE __attribute__((noinline))
#else
#define SP1_DSP_NOINLINE
#endif

static SP1_DSP_NOINLINE void render_kick_segment(sp1_kick_voice_t *v,
                                                 sp1_track_strip_t *strip,
                                                 float *out_main_l,
                                                 float *out_main_r,
                                                 float *out_send_l,
                                                 float *out_send_r,
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

    const float ch_gain = v->velocity_gain * strip->level_gain;
    const float gain_l = ch_gain * strip->pan_l;
    const float gain_r = ch_gain * strip->pan_r;
    const float send_amt = strip->delay_send * ch_gain;
    const bool has_send = (send_amt > 0.001f) && (out_send_l != NULL) && (out_send_r != NULL);
    const uint8_t tone_mode = strip->tone_mode;
    const float tone_g = strip->tone_g;
    float tone_z = strip->tone_z;
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

        float sig = shaped * amp_env;
        if (tone_mode != 0U) {
            tone_z += tone_g * (sig - tone_z);
            sig = (tone_mode == 1U) ? tone_z : (sig - tone_z);
        }

        out_main_l[i] += sig * gain_l;
        out_main_r[i] += sig * gain_r;
        if (has_send) {
            const float snd = sig * send_amt;
            out_send_l[i] += snd;
            out_send_r[i] += snd;
        }

        amp_env *= amp_decay;
        if (amp_env < SP1_DSP_ENV_FLOOR) {
            amp_env = 0.0f;
            pitch_env = 0.0f;
            click_env = 0.0f;
            active = false;
            break;
        }
    }

#if !defined(__ARM_FP) || (__ARM_FP == 0)
    if (fabsf(tone_z) < SP1_DSP_DENORMAL_FLOOR) {
        tone_z = 0.0f;
    }
#endif
    strip->tone_z = tone_z;
    v->phase = phase;
    v->amp_env = amp_env;
    v->pitch_env = pitch_env;
    v->click_env = click_env;
    v->active = active;
}

static SP1_DSP_NOINLINE void render_snare_segment(sp1_snare_voice_t *v,
                                                  sp1_track_strip_t *strip,
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

    const float ch_gain = v->velocity_gain * strip->level_gain;
    const float gain_l = ch_gain * 0.96f * strip->pan_l;
    const float gain_r = ch_gain * 1.02f * strip->pan_r;
    const float send_amt = ch_gain * strip->delay_send;
    const bool has_send = (send_amt > 0.001f) && (out_send_l != NULL) && (out_send_r != NULL);
    const uint8_t tone_mode = strip->tone_mode;
    const float tone_g = strip->tone_g;
    float tone_z = strip->tone_z;

    const uint32_t body_base_inc = sp1_hz_to_phase_inc(v->body_freq_hz);
    const uint32_t rim_base_inc = sp1_hz_to_phase_inc(v->rim_freq_hz);
    const float body_knock_inc_f = 135.0f * SP1_DSP_PHASE_SCALE;
    const float rim_knock_inc_f = 245.0f * SP1_DSP_PHASE_SCALE;

    bool active = true;
    uint32_t i = start;

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
        float sig = sp1_soft_clip(resonator + wire_sig);
        if (tone_mode != 0U) {
            tone_z += tone_g * (sig - tone_z);
            sig = (tone_mode == 1U) ? tone_z : (sig - tone_z);
        }

        out_main_l[i] += sig * gain_l;
        out_main_r[i] += sig * gain_r;
        if (has_send) {
            const float snd = sig * send_amt;
            out_send_l[i] += snd;
            out_send_r[i] += snd;
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
            float sig = sp1_soft_clip(wire_sig);
            if (tone_mode != 0U) {
                tone_z += tone_g * (sig - tone_z);
                sig = (tone_mode == 1U) ? tone_z : (sig - tone_z);
            }

            out_main_l[i] += sig * gain_l;
            out_main_r[i] += sig * gain_r;
            if (has_send) {
                const float snd = sig * send_amt;
                out_send_l[i] += snd;
                out_send_r[i] += snd;
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
    if (fabsf(tone_z) < SP1_DSP_DENORMAL_FLOOR) {
        tone_z = 0.0f;
    }
#endif

    strip->tone_z = tone_z;
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
                                                sp1_track_strip_t *strip,
                                                float *out_main_l,
                                                float *out_main_r,
                                                float *out_send_l,
                                                float *out_send_r,
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
    const float metal_gain = v->metal_gain;
    const float noise_gain = v->noise_gain;

    const float ch_gain = v->velocity_gain * strip->level_gain;
    const float gain_l = ch_gain * 0.76f * strip->pan_l;
    const float gain_r = ch_gain * 1.06f * strip->pan_r;
    const float send_amt = ch_gain * strip->delay_send;
    const bool has_send = (send_amt > 0.001f) && (out_send_l != NULL) && (out_send_r != NULL);
    const uint8_t tone_mode = strip->tone_mode;
    const float tone_g = strip->tone_g;
    float tone_z = strip->tone_z;
    bool active = true;

    for (uint32_t i = start; i < end; ++i) {
        p0 += inc0;
        p1 += inc1;
        p2 += inc2;

        int32_t metal_i = (((int32_t)(p0 >> 31U) * 2) - 1) * 4 +
                          (((int32_t)((p0 >> 28U) & 1U) * 2) - 1) * 6;
        metal_i += (((int32_t)(p1 >> 31U) * 2) - 1) * 4 +
                   (((int32_t)((p1 >> 28U) & 1U) * 2) - 1) * 6;
        metal_i += (((int32_t)(p2 >> 31U) * 2) - 1) * 4 +
                   (((int32_t)((p2 >> 28U) & 1U) * 2) - 1) * 6;

        const float v0 = ((float)metal_i * metal_gain) + (sp1_xorshift32_noise(&rng) * noise_gain);

        const float v3 = v0 - ic2;
        const float v1 = (a1 * ic1) + (a2 * v3);
        const float v2 = ic2 + (a2 * ic1) + (a3 * v3);
        ic1 = (2.0f * v1) - ic1;
        ic2 = (2.0f * v2) - ic2;
        const float hp_out = v0 - (svf_k * v1) - v2;

        float sig = sp1_soft_clip((hp_out * 0.82f) + (v1 * 0.32f)) * amp_env;
        if (tone_mode != 0U) {
            tone_z += tone_g * (sig - tone_z);
            sig = (tone_mode == 1U) ? tone_z : (sig - tone_z);
        }

        out_main_l[i] += sig * gain_l;
        out_main_r[i] += sig * gain_r;
        if (has_send) {
            const float snd = sig * send_amt;
            out_send_l[i] += snd;
            out_send_r[i] += snd;
        }

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
    if (fabsf(tone_z) < SP1_DSP_DENORMAL_FLOOR) {
        tone_z = 0.0f;
    }
#endif

    strip->tone_z = tone_z;
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
                                                 sp1_track_strip_t *strip,
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

    const float ch_gain = v->velocity_gain * strip->level_gain;
    const float gain_l = ch_gain * 1.05f * strip->pan_l;
    const float gain_r = ch_gain * 0.78f * strip->pan_r;
    const float send_l_gain = ch_gain * 1.15f * strip->delay_send;
    const float send_r_gain = ch_gain * 0.85f * strip->delay_send;
    const bool has_send = (strip->delay_send > 0.001f) && (out_send_l != NULL) && (out_send_r != NULL);
    const uint8_t tone_mode = strip->tone_mode;
    const float tone_g = strip->tone_g;
    float tone_z = strip->tone_z;
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

        const uint32_t fb_offset = (uint32_t)((int32_t)(prev_mod * fb_scale * index_env));
        const float mod_sample = sp1_sine_lut(mod_phase + fb_offset);
        prev_mod = mod_sample;

        const uint32_t pm_offset = (uint32_t)((int32_t)(mod_sample * mod_scale * index_env));
        const float carrier = sp1_sine_lut(carrier_phase + pm_offset);

        const float fold_drive = 1.10f + (timbre_scale * index_env);
        float sig = sp1_wavefold_lut(carrier * fold_drive) * amp_env;
        if (tone_mode != 0U) {
            tone_z += tone_g * (sig - tone_z);
            sig = (tone_mode == 1U) ? tone_z : (sig - tone_z);
        }

        out_main_l[i] += sig * gain_l;
        out_main_r[i] += sig * gain_r;
        if (has_send) {
            out_send_l[i] += sig * send_l_gain;
            out_send_r[i] += sig * send_r_gain;
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

#if !defined(__ARM_FP) || (__ARM_FP == 0)
    if (fabsf(tone_z) < SP1_DSP_DENORMAL_FLOOR) {
        tone_z = 0.0f;
    }
#endif

    strip->tone_z = tone_z;
    v->carrier_phase = carrier_phase;
    v->mod_phase = mod_phase;
    v->prev_mod = prev_mod;
    v->amp_env = amp_env;
    v->pitch_env = pitch_env;
    v->index_env = index_env;
    v->active = active;
}

static SP1_DSP_NOINLINE void render_siren_block(sp1_dub_siren_t *s,
                                                float *out_main_l,
                                                float *out_main_r,
                                                float *out_send_l,
                                                float *out_send_r,
                                                uint32_t num_frames) {
    if (!s->gate && !s->active) {
        return;
    }

    uint32_t osc_phase = s->osc_phase;
    uint32_t lfo_phase = s->lfo_phase;
    const uint32_t lfo_inc = s->lfo_inc;
    const float base_hz = s->base_freq_hz;
    const float depth = s->lfo_depth_oct;
    const uint8_t lfo_mode = s->lfo_mode;
    const float send_gain = s->delay_send * 0.72f;
    const bool has_send = (send_gain > 0.001f) && (out_send_l != NULL) && (out_send_r != NULL);
    float env = s->amp_env;
    float lfo_vis = s->last_lfo_vis;
    bool active = true;

    for (uint32_t i = 0U; i < num_frames; ++i) {
        if (s->gate) {
            env += 0.0085f * (1.0f - env); /* ~2.5 ms smooth clickless attack */
        } else {
            env *= 0.9975f;                /* ~8.3 ms smooth release (< 1e-5 in < 100 ms) */
            if (env < SP1_DSP_ENV_FLOOR) {
                env = 0.0f;
                active = false;
                break;
            }
        }

        lfo_phase += lfo_inc;
        float mod_bi = 0.0f;
        if (lfo_mode == 1U) {
            /* Smooth wailing triangle LFO */
            mod_bi = sp1_triangle_osc(lfo_phase);
            lfo_vis = (mod_bi * 0.5f) + 0.5f;
        } else if (lfo_mode == 2U) {
            /* Classic Jamaican 2-tone square siren */
            const bool high_tone = ((lfo_phase & 0x80000000U) == 0U);
            mod_bi = high_tone ? 1.0f : -0.65f;
            lfo_vis = high_tone ? 1.0f : 0.0f;
        } else {
            lfo_vis = 0.5f;
        }

        /* Fast division-free pitch modulation around base_hz */
        float mod_mult = 1.0f + (mod_bi * depth);
        if (mod_mult < 0.35f) {
            mod_mult = 0.35f;
        }
        osc_phase += sp1_hz_to_phase_inc(base_hz * mod_mult);

        const float sine_core = sp1_sine_lut(osc_phase);
        const float sq_core = ((osc_phase & 0x80000000U) != 0U) ? -0.55f : 0.55f;
        const float sig = sp1_soft_clip((sine_core * 0.65f) + (sq_core * 0.65f)) * env * 0.36f;

        out_main_l[i] += sig;
        out_main_r[i] += sig;
        if (has_send) {
            const float snd = sig * send_gain;
            out_send_l[i] += snd;
            out_send_r[i] += snd;
        }
    }

    s->osc_phase = osc_phase;
    s->lfo_phase = lfo_phase;
    s->amp_env = env;
    s->last_lfo_vis = lfo_vis;
    s->active = s->gate || active;
}

/* ── Public API Implementation ───────────────────────────────────────────── */

void sp1_drum_voices_load_kit(sp1_drum_synth_t *synth, uint8_t kit_idx) {
    if (synth == NULL) {
        return;
    }
    const uint8_t k = (kit_idx < SP1_DRUM_NUM_KITS) ? kit_idx : 0U;

    synth->kick.tune_param  = k_drum_kit_presets[k][0][0];
    synth->kick.decay_param = k_drum_kit_presets[k][0][1];
    synth->kick.punch_param = k_drum_kit_presets[k][0][2];
    synth->kick.drive_param = k_drum_kit_presets[k][0][3];
    update_kick_coeffs(&synth->kick);

    synth->snare.pitch_param       = k_drum_kit_presets[k][1][0];
    synth->snare.tone_morph        = k_drum_kit_presets[k][1][1];
    synth->snare.wire_decay_param  = k_drum_kit_presets[k][1][2];
    synth->snare.wire_filter_param = k_drum_kit_presets[k][1][3];
    update_snare_coeffs(&synth->snare);

    synth->hat.tune_param      = k_drum_kit_presets[k][2][0];
    synth->hat.decay_morph     = k_drum_kit_presets[k][2][1];
    synth->hat.cutoff_param    = k_drum_kit_presets[k][2][2];
    synth->hat.noise_mix_param = k_drum_kit_presets[k][2][3];
    update_hat_coeffs(&synth->hat);

    synth->perc.pitch_param  = k_drum_kit_presets[k][3][0];
    synth->perc.ratio_param  = k_drum_kit_presets[k][3][1];
    synth->perc.decay_param  = k_drum_kit_presets[k][3][2];
    synth->perc.timbre_morph = k_drum_kit_presets[k][3][3];
    update_perc_coeffs(&synth->perc);
}

void sp1_drum_voices_init(sp1_drum_synth_t *synth) {
    if (synth == NULL) {
        return;
    }
    memset(synth, 0, sizeof(*synth));

    synth->snare.rng = 0x2468ACE1U;
    sp1_svf_reset(&synth->snare.wire_svf);

    synth->hat.rng = 0xA5A55A5AU;
    sp1_svf_reset(&synth->hat.hp_svf);

    sp1_drum_voices_load_kit(synth, 0U);
    synth->kick.pitch_span_hz = synth->kick.pitch_span_base_hz;

    /* Default Channel Strip: unity level (1.0), centered pan, flat tone bypass,
     * and selective Dub Delay sends on Snare (0.55) + Perc (0.60), keeping Kick & Hat dry. */
    static const float k_default_sends[SP1_SEQ_NUM_TRACKS] = { 0.0f, 0.55f, 0.0f, 0.60f };
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        synth->strips[t].level_gain = 1.0f;
        synth->strips[t].pan_l = 1.0f;
        synth->strips[t].pan_r = 1.0f;
        synth->strips[t].delay_send = k_default_sends[t];
        synth->strips[t].tone_g = 0.5f;
        synth->strips[t].tone_z = 0.0f;
        synth->strips[t].tone_mode = 0U; /* Bypassed inside [0.45..0.55] */
    }

    sp1_drum_voices_set_siren(synth, false, 0.42f, 0.35f, 0.78f, 0.65f);
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
    const float c = clamp01(morph_0_rim_to_1_snare);
    synth->snare.tone_morph = c;
    synth->snare.wire_decay_param = c;
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

void sp1_drum_voices_set_voice_param(sp1_drum_synth_t *synth,
                                     uint8_t track,
                                     uint8_t param_idx,
                                     float norm_0_to_1) {
    if (synth == NULL || track >= SP1_SEQ_NUM_TRACKS || param_idx >= SP1_DRUM_NUM_VOICE_PARAMS) {
        return;
    }
    const float c = clamp01(norm_0_to_1);
    if (sp1_drum_voices_get_voice_param(synth, track, param_idx) == c) {
        return;
    }

    switch (track) {
    case 0U:
        if (param_idx == 0U) { synth->kick.tune_param = c; }
        else if (param_idx == 1U) { synth->kick.decay_param = c; }
        else if (param_idx == 2U) { synth->kick.punch_param = c; }
        else { synth->kick.drive_param = c; }
        update_kick_coeffs(&synth->kick);
        break;
    case 1U:
        if (param_idx == 0U) { synth->snare.pitch_param = c; }
        else if (param_idx == 1U) { synth->snare.tone_morph = c; }
        else if (param_idx == 2U) { synth->snare.wire_decay_param = c; }
        else { synth->snare.wire_filter_param = c; }
        update_snare_coeffs(&synth->snare);
        break;
    case 2U:
        if (param_idx == 0U) { synth->hat.tune_param = c; }
        else if (param_idx == 1U) { synth->hat.decay_morph = c; }
        else if (param_idx == 2U) { synth->hat.cutoff_param = c; }
        else { synth->hat.noise_mix_param = c; }
        update_hat_coeffs(&synth->hat);
        break;
    case 3U:
        if (param_idx == 0U) { synth->perc.pitch_param = c; }
        else if (param_idx == 1U) { synth->perc.ratio_param = c; }
        else if (param_idx == 2U) { synth->perc.decay_param = c; }
        else { synth->perc.timbre_morph = c; }
        update_perc_coeffs(&synth->perc);
        break;
    default:
        break;
    }
}

float sp1_drum_voices_get_voice_param(const sp1_drum_synth_t *synth,
                                      uint8_t track,
                                      uint8_t param_idx) {
    if (synth == NULL || track >= SP1_SEQ_NUM_TRACKS || param_idx >= SP1_DRUM_NUM_VOICE_PARAMS) {
        return 0.5f;
    }
    switch (track) {
    case 0U:
        if (param_idx == 0U) { return synth->kick.tune_param; }
        if (param_idx == 1U) { return synth->kick.decay_param; }
        if (param_idx == 2U) { return synth->kick.punch_param; }
        return synth->kick.drive_param;
    case 1U:
        if (param_idx == 0U) { return synth->snare.pitch_param; }
        if (param_idx == 1U) { return synth->snare.tone_morph; }
        if (param_idx == 2U) { return synth->snare.wire_decay_param; }
        return synth->snare.wire_filter_param;
    case 2U:
        if (param_idx == 0U) { return synth->hat.tune_param; }
        if (param_idx == 1U) { return synth->hat.decay_morph; }
        if (param_idx == 2U) { return synth->hat.cutoff_param; }
        return synth->hat.noise_mix_param;
    case 3U:
        if (param_idx == 0U) { return synth->perc.pitch_param; }
        if (param_idx == 1U) { return synth->perc.ratio_param; }
        if (param_idx == 2U) { return synth->perc.decay_param; }
        return synth->perc.timbre_morph;
    default:
        return 0.5f;
    }
}

void sp1_drum_voices_set_track_mix(sp1_drum_synth_t *synth,
                                   uint8_t track,
                                   float level_0_to_1,
                                   float pan_0_to_1,
                                   float delay_send_0_to_1,
                                   float tone_0_to_1) {
    if (synth == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_track_strip_t *st = &synth->strips[track];

    /* 0.87f default maps to ~1.00f unity gain, with +1.2 dB headroom at 1.0f */
    st->level_gain = clamp01(level_0_to_1) * 1.15f;

    const float pan_bi = bipolar_detent(pan_0_to_1);
    if (pan_bi == 0.0f) {
        st->pan_l = 1.0f;
        st->pan_r = 1.0f;
    } else if (pan_bi < 0.0f) {
        st->pan_l = 1.0f + (-0.30f * pan_bi); /* up to 1.30f hard left */
        st->pan_r = 1.0f + pan_bi;            /* down to 0.0f */
    } else {
        st->pan_l = 1.0f - pan_bi;            /* down to 0.0f */
        st->pan_r = 1.0f + (0.30f * pan_bi);  /* up to 1.30f hard right */
    }

    st->delay_send = clamp01(delay_send_0_to_1);

    /* Per-track Tone Filter: strictly bypassed (tone_mode = 0) inside [0.45..0.55] */
    const float tone_bi = bipolar_detent(tone_0_to_1);
    if (tone_bi == 0.0f) {
        st->tone_mode = 0U;
    } else if (tone_bi < 0.0f) {
        const float mag = -tone_bi;
        const float inv = 1.0f - mag;
        const float cutoff = 180.0f + (inv * inv * 14000.0f);
        st->tone_g = fast_onepole_g_control(cutoff);
        st->tone_mode = 1U;
    } else {
        const float cutoff = 30.0f + (tone_bi * tone_bi * 4500.0f);
        st->tone_g = fast_onepole_g_control(cutoff);
        st->tone_mode = 2U;
    }
}

void sp1_drum_voices_set_siren(sp1_drum_synth_t *synth,
                               bool gate,
                               float pitch_0_to_1,
                               float lfo_rate_0_to_1,
                               float lfo_shape_0_to_1,
                               float delay_send_0_to_1) {
    if (synth == NULL) {
        return;
    }
    sp1_dub_siren_t *sr = &synth->siren;
    if (gate && !sr->gate && !sr->active) {
        sr->lfo_phase = 0U;
    }
    sr->gate = gate;
    if (gate) {
        sr->active = true;
    }

    const float p = clamp01(pitch_0_to_1);
    sr->base_freq_hz = 140.0f + (p * p * 1260.0f); /* 140 Hz .. 1,400 Hz */

    const float r = clamp01(lfo_rate_0_to_1);
    const float rate_hz = 0.40f + (r * r * 17.60f); /* 0.4 Hz .. 18.0 Hz */
    sr->lfo_inc = sp1_hz_to_phase_inc(rate_hz);

    const float sh = clamp01(lfo_shape_0_to_1);
    if (sh >= 0.42f && sh <= 0.58f) {
        sr->lfo_mode = 0U;
        sr->lfo_depth_oct = 0.0f;
    } else if (sh < 0.42f) {
        sr->lfo_mode = 1U;
        sr->lfo_depth_oct = ((0.42f - sh) * (1.0f / 0.42f)) * 0.65f;
    } else {
        sr->lfo_mode = 2U;
        sr->lfo_depth_oct = ((sh - 0.58f) * (1.0f / 0.42f)) * 0.55f;
    }

    sr->delay_send = clamp01(delay_send_0_to_1);
}

float sp1_drum_voices_get_siren_lfo_level(const sp1_drum_synth_t *synth) {
    return (synth != NULL) ? synth->siren.last_lfo_vis : 0.5f;
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
    case 0U: { /* Track 1: Kick / Sub */
        sp1_kick_voice_t *k = &synth->kick;
        k->phase = 0U;
        k->amp_env = 1.0f;
        k->pitch_env = 1.0f;
        k->click_env = 1.0f;
        k->pitch_span_hz = k->pitch_span_base_hz + (accent ? 20.0f : 0.0f);
        k->velocity_gain = gain * 0.72f;
        k->active = true;
        break;
    }
    case 1U: { /* Track 2: Snare / Rimshot */
        sp1_snare_voice_t *s = &synth->snare;
        s->phase_body = 0U;
        s->phase_rim = 0x20000000U;
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

    const bool any_drum = (num_triggers > 0U) ||
                          synth->kick.active || synth->snare.active ||
                          synth->hat.active || synth->perc.active;
    const bool any_siren = synth->siren.gate || synth->siren.active;
    if (!any_drum && !any_siren) {
        return;
    }

    if (any_drum) {
        uint32_t next_trig = 0U;
        uint32_t cursor = 0U;

        while (cursor < num_frames) {
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
                if (synth->kick.active) {
                    render_kick_segment(&synth->kick, &synth->strips[0],
                                        out_main_l, out_main_r, out_send_l, out_send_r,
                                        cursor, seg_end);
                }
                if (synth->snare.active) {
                    render_snare_segment(&synth->snare, &synth->strips[1],
                                         out_main_l, out_main_r, out_send_l, out_send_r,
                                         cursor, seg_end);
                }
                if (synth->hat.active) {
                    render_hat_segment(&synth->hat, &synth->strips[2],
                                       out_main_l, out_main_r, out_send_l, out_send_r,
                                       cursor, seg_end);
                }
                if (synth->perc.active) {
                    render_perc_segment(&synth->perc, &synth->strips[3],
                                        out_main_l, out_main_r, out_send_l, out_send_r,
                                        cursor, seg_end);
                }
                cursor = seg_end;
            }
        }
    }

    if (any_siren) {
        render_siren_block(&synth->siren, out_main_l, out_main_r, out_send_l, out_send_r, num_frames);
    }
}
