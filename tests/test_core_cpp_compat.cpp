#include "core/clock_sync.h"
#include "core/euclid.h"
#include "core/sequencer.h"
#include "core/ui_mapper.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <cstdio>

int main() {
    const sp1_hal_host_config_t cfg = {nullptr, nullptr, false};
    sp1_hal_host_configure(&cfg);

    if (sp1_hal_init() != 0) {
        std::fprintf(stderr, "FAIL: sp1_hal_init() failed from C++17\n");
        return 1;
    }

    const uint32_t mask = sp1_euclid_compute_mask(3U, 8U, 0U);
    if (mask != 0x00000049U) {
        std::fprintf(stderr, "FAIL: unexpected E(3,8) mask 0x%08X from C++17\n", mask);
        return 1;
    }

    sp1_sequencer_t seq{};
    sp1_ui_mapper_t ui{};
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    sp1_seq_set_bpm(&seq, 120.0f);
    sp1_seq_set_swing(&seq, 0.58f);
    sp1_seq_set_track_nudge_ms(&seq, 1U, 18.0f);
    sp1_seq_start(&seq);

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS]{};
    const uint32_t n = sp1_seq_process_block(&seq, 0ULL, SP1_AUDIO_BLOCK_FRAMES,
                                             trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
    if (n == 0U) {
        std::fprintf(stderr, "FAIL: expected downbeat triggers in block 0 from C++17\n");
        return 1;
    }

    sp1_ui_mapper_on_triggers(&ui, trigs, n);
    sp1_led_frame_t leds{};
    sp1_ui_mapper_render_leds(&ui, &seq, 2U, &leds);
    sp1_hal_leds_set_frame(&leds);

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    std::printf("[PASS] test_core_cpp_compat\n");
    return 0;
}
