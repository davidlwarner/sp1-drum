#include "dsp/drum_voices.h"
#include "dsp/master_fx.h"
#include "hal/sp1_hal.h"

#include <cstdio>

int main() {
    static sp1_drum_synth_t synth{};
    static sp1_master_fx_t fx{};

    sp1_drum_voices_init(&synth);
    sp1_master_fx_init(&fx);

    sp1_drum_voices_set_snare_morph(&synth, 0.25f);
    sp1_drum_voices_set_hat_decay(&synth, 0.50f);
    sp1_drum_voices_set_perc_timbre(&synth, 0.75f);

    sp1_master_fx_set_dj_filter(&fx, 0.50f, 1.85f);
    sp1_master_fx_set_stutter(&fx, SP1_STUTTER_OFF);
    sp1_master_fx_set_bitcrusher(&fx, 0.0f);
    sp1_master_fx_set_dub_delay(&fx, 0.25f, 0.45f);

    const sp1_seq_trigger_event_t trig = {
        0ULL, 0U, 0U, 0U, 120U, true
    };

    float main_l[SP1_AUDIO_BLOCK_FRAMES]{};
    float main_r[SP1_AUDIO_BLOCK_FRAMES]{};
    float send_l[SP1_AUDIO_BLOCK_FRAMES]{};
    float send_r[SP1_AUDIO_BLOCK_FRAMES]{};
    int16_t pcm[SP1_AUDIO_BLOCK_SAMPLES]{};

    sp1_drum_voices_render_block(
        &synth, &trig, 1U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
    sp1_master_fx_process_block(
        &fx, 6000U, main_l, main_r, send_l, send_r, pcm, SP1_AUDIO_BLOCK_FRAMES);

    bool non_zero = false;
    for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_SAMPLES; ++i) {
        if (pcm[i] != 0) {
            non_zero = true;
            break;
        }
    }

    if (!non_zero) {
        std::fprintf(stderr, "FAIL: expected non-zero DSP render output from C++17\n");
        return 1;
    }

    std::printf("[PASS] test_dsp_cpp_compat\n");
    return 0;
}
