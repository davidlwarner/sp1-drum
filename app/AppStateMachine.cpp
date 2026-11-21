#include "AppStateMachine.hpp"
#include "power/PowerManager.hpp"
#include "system/Watchdog.hpp"
#include "hal/sp1_hal.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

LOG_MODULE_REGISTER(app_hsm, LOG_LEVEL_INF);

namespace app {
namespace {

constexpr int kPollCadenceMs = 20;
constexpr int64_t kFnHoldShutdownMs = 3000;  // 3 second hold to shut down
constexpr int64_t kFnReleaseStableMs = 30;   // debounce before sys_poweroff

void feed(AppMachine& m) {
    core::watchdog::feed(m.main_watchdog_channel);
}

/// Returns true if function button has been held long enough for shutdown.
bool check_fn_hold_shutdown(AppMachine& m) {
    if (m.power == nullptr) {
        return false;
    }

    const bool pressed = m.power->function_button_pressed();
    const int64_t now = k_uptime_get();

    if (pressed && !m.fn_was_pressed) {
        // Rising edge — start hold timer
        m.fn_hold_start_ms = now;
    }

    m.fn_was_pressed = pressed;

    if (pressed && m.fn_hold_start_ms != 0 &&
        (now - m.fn_hold_start_ms) >= kFnHoldShutdownMs) {
        return true;
    }

    if (!pressed) {
        m.fn_hold_start_ms = 0;
    }

    return false;
}

}  // namespace

// ── Idle: transition immediately to standalone Running mode ─────

static void idle_entry(void*) {
    LOG_INF("SP-1 Drum Machine: Idle -> Entering standalone Running state");
}

static smf_state_result idle_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    if (check_fn_hold_shutdown(m)) {
        smf::set_state(m.ctx, app_states[STATE_SHUTTING_DOWN]);
        return SMF_EVENT_HANDLED;
    }

    // Standalone battery / USB-C operation: enter STATE_RUNNING immediately
    // without waiting for a USB host computer.
    smf::set_state(m.ctx, app_states[STATE_RUNNING]);
    return SMF_EVENT_HANDLED;
}

// ── Running: 50 Hz SAADC faders + UI mapper + DSP sync + PWM LEDs ─

static void running_entry(void*) {
    LOG_INF("SP-1 Drum Machine: Running (48 kHz I2S0 audio + 50 Hz control loop)");
}

static smf_state_result running_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    if (check_fn_hold_shutdown(m)) {
        smf::set_state(m.ctx, app_states[STATE_SHUTTING_DOWN]);
        return SMF_EVENT_HANDLED;
    }

    if (m.midi != nullptr) {
        m.midi->poll_faders();
    }

    if (m.ui != nullptr && m.seq != nullptr) {
        sp1_controls_snapshot_t snap{};
        sp1_hal_controls_poll(&snap);
        sp1_ui_mapper_update(m.ui, m.seq, &snap);

        const sp1_ui_dsp_params_t* dsp = sp1_ui_mapper_get_dsp_params(m.ui);
        if (dsp != nullptr) {
            if (m.synth != nullptr) {
                sp1_drum_voices_set_kick_params(m.synth, dsp->kick_decay, dsp->kick_drive);
                sp1_drum_voices_set_snare_morph(m.synth, dsp->snare_morph);
                sp1_drum_voices_set_hat_decay(m.synth, dsp->hat_decay);
                sp1_drum_voices_set_perc_timbre(m.synth, dsp->perc_timbre);
            }
            if (m.fx != nullptr) {
                sp1_master_fx_set_dj_filter(m.fx, dsp->dj_filter_pos, dsp->dj_resonance_q);
                sp1_master_fx_set_stutter(
                    m.fx, static_cast<sp1_stutter_div_t>(dsp->stutter_div));
                sp1_master_fx_set_bitcrusher(m.fx, dsp->crush_amount);
                sp1_master_fx_set_dub_delay(
                    m.fx, dsp->dub_delay_wet, dsp->dub_delay_feedback);
            }
        }

        sp1_led_frame_t led_frame{};
        sp1_ui_mapper_render_leds(m.ui, m.seq, static_cast<uint32_t>(kPollCadenceMs), &led_frame);
        sp1_hal_leds_set_frame(&led_frame);
    }

    if (m.power != nullptr) {
        m.power->service_tick();
    }
    sp1_hal_service_background();

    k_sleep(K_MSEC(kPollCadenceMs));
    return SMF_EVENT_HANDLED;
}

// ── ShuttingDown: stop audio cleanly, fade LEDs, then deep sleep ─

static void shutting_down_entry(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    LOG_INF("SP-1 Drum Machine: Shutting down");

    (void)sp1_hal_audio_stop();

    if (m.leds != nullptr) {
        m.leds->start_shutdown_fade();
    }
}

static smf_state_result shutting_down_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    if (m.leds != nullptr) {
        m.leds->tick_fade();
        if (!m.leds->shutdown_fade_complete()) {
            k_sleep(K_MSEC(kPollCadenceMs));
            return SMF_EVENT_HANDLED;
        }
    }

    smf::set_state(m.ctx, app_states[STATE_DEEP_SLEEP]);
    return SMF_EVENT_HANDLED;
}

// ── DeepSleep: GPIO teardown → wait for fn release → sys_poweroff

static void deep_sleep_entry(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    LOG_INF("MIDI Controller: DeepSleep — powering off");

    if (m.leds != nullptr) {
        m.leds->all_off();
    }
    sp1_hal_shutdown();

    if (m.power != nullptr) {
        m.power->prepare_for_system_off();
    }
    m.fn_released_ms = 0;
}

static smf_state_result deep_sleep_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    // Wait for function button release before calling sys_poweroff,
    // so the wake-on-press doesn't fire immediately.
    if (m.power != nullptr && m.power->function_button_pressed()) {
        m.fn_released_ms = 0;
        k_sleep(K_MSEC(10));
        return SMF_EVENT_HANDLED;
    }

    const int64_t now = k_uptime_get();
    if (m.fn_released_ms == 0) {
        m.fn_released_ms = now;
        k_sleep(K_MSEC(10));
        return SMF_EVENT_HANDLED;
    }

    if (now - m.fn_released_ms < kFnReleaseStableMs) {
        k_sleep(K_MSEC(10));
        return SMF_EVENT_HANDLED;
    }

    sys_poweroff();

    // Should never reach here
    LOG_ERR("sys_poweroff() returned");
    k_sleep(K_FOREVER);
    return SMF_EVENT_HANDLED;
}

// ── State table ─────────────────────────────────────────────────

const smf_state app_states[STATE_COUNT] = {
    [STATE_IDLE] = smf::state(idle_entry, idle_run, nullptr),
    [STATE_RUNNING] = smf::state(running_entry, running_run, nullptr),
    [STATE_SHUTTING_DOWN] = smf::state(shutting_down_entry, shutting_down_run, nullptr),
    [STATE_DEEP_SLEEP] = smf::state(deep_sleep_entry, deep_sleep_run, nullptr),
};

void run(AppMachine& m) {
    smf::set_initial(m.ctx, app_states[STATE_IDLE]);
    while (smf::run(m.ctx) == 0) {
    }
    LOG_ERR("App HSM terminated unexpectedly");
}

}  // namespace app
