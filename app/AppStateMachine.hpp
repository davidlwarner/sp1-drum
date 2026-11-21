#pragma once

#include <stem_player/Smf.hpp>
#include <zephyr/kernel.h>
#include <cstdint>

#include "Leds.hpp"
#include "MidiController.hpp"
#include "core/sequencer.h"
#include "core/ui_mapper.h"
#include "dsp/drum_voices.h"
#include "dsp/master_fx.h"

namespace power { class PowerManager; }

namespace app {

enum AppState : uint8_t {
    STATE_IDLE,
    STATE_RUNNING,
    STATE_SHUTTING_DOWN,
    STATE_DEEP_SLEEP,
    STATE_COUNT,
};

struct AppMachine {
    smf_ctx ctx;
    midi::MidiController* midi = nullptr;
    leds::Leds* leds = nullptr;
    power::PowerManager* power = nullptr;
    sp1_sequencer_t* seq = nullptr;
    sp1_ui_mapper_t* ui = nullptr;
    sp1_drum_synth_t* synth = nullptr;
    sp1_master_fx_t* fx = nullptr;
    const uint32_t* led_track_packed = nullptr;
    const uint32_t* led_play_packed = nullptr;
    int main_watchdog_channel = -1;
    bool audio_active = false;
    uint64_t fallback_frame_counter = 0ULL;

    // Function-button hold detection (for shutdown)
    int64_t fn_hold_start_ms = 0;
    bool fn_was_pressed = false;

    // Shutdown button-release debounce
    int64_t fn_released_ms = 0;
};

extern const smf_state app_states[STATE_COUNT];

void run(AppMachine& m);

}  // namespace app
