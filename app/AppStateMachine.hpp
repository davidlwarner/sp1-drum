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
    const uint32_t* target_volume = nullptr;
    bool (*start_runtime)(void) = nullptr;
    int main_watchdog_channel = -1;
    bool audio_active = false;
    bool runtime_started = false;
    uint8_t applied_volume = SP1_UI_VOLUME_DEFAULT;
    uint64_t fallback_frame_counter = 0ULL;

    // 1.5-second hold-to-power-on gate (SYSTEM_OFF GPIO wake without USB VBUS)
    bool wake_hold_required = false;
    int64_t wake_hold_start_ms = 0;

    // Function-button hold detection (for shutdown and USB soft-off standby wake)
    int64_t fn_hold_start_ms = 0;
    bool fn_was_pressed = false;
    sp1_ui_power_hold_t power_hold{};
    sp1_ui_power_hold_t standby_wake_hold{};
    bool soft_off_standby = false;

    // Shutdown button-release debounce
    int64_t fn_released_ms = 0;

    // One-shot delayed previous-boot crash breadcrumb report in STATE_RUNNING
    bool boot_report_printed = false;
    int64_t boot_report_due_ms = 0;

    // Deferred first-tick codec volume/routing init and 200 ms jack-detect poll divider
    bool routing_initialized = false;
    uint8_t routing_poll_ticks = 0U;

    // Last logged sequencer running state for edge-triggered serial diagnostics
    bool prev_seq_running = false;
};

extern const smf_state app_states[STATE_COUNT];

void run(AppMachine& m);

}  // namespace app
