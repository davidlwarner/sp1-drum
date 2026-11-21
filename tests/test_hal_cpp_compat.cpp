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

    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);

    sp1_controls_snapshot_t snap{};
    sp1_hal_controls_poll(&snap);

    if (!sp1_controls_is_valid_shift_chord(snap.buttons_down, SP1_BTN_FN, SP1_BTN_TRACK_1)) {
        std::fprintf(stderr, "FAIL: expected valid FN+T1 chord from C++17\n");
        return 1;
    }

    const sp1_midi_msg_t clk = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, sp1_hal_time_us());
    uint32_t ump_word0 = 0U;
    if (!sp1_midi_encode_ump32(&clk, 0U, &ump_word0)) {
        std::fprintf(stderr, "FAIL: UMP32 encode failed from C++17\n");
        return 1;
    }
    sp1_hal_midi_on_rx_ump32(ump_word0);

    sp1_midi_msg_t rx{};
    if (!sp1_hal_midi_receive(&rx) || rx.status != SP1_MIDI_STATUS_CLOCK) {
        std::fprintf(stderr, "FAIL: MIDI RX roundtrip failed from C++17\n");
        return 1;
    }

    sp1_hal_audio_reset_cycle_stats();
    sp1_hal_audio_record_cycles(48000U);
    sp1_audio_cycle_stats_t cyc{};
    sp1_hal_audio_get_cycle_stats(&cyc);
    if (cyc.block_count != 1U || cyc.max_cycles != 48000U ||
        sp1_audio_cycle_stats_avg(&cyc) != 48000U) {
        std::fprintf(stderr, "FAIL: cycle stats roundtrip failed from C++17\n");
        return 1;
    }

    sp1_hal_shutdown();
    std::printf("[PASS] test_hal_cpp_compat\n");
    return 0;
}
