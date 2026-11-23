#include "AppStateMachine.hpp"
#include "power/PowerManager.hpp"
#include "system/ResetBreadcrumbs.hpp"
#include "system/Watchdog.hpp"
#include "hal/sp1_hal.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

LOG_MODULE_REGISTER(app_hsm, LOG_LEVEL_INF);

namespace app {
namespace {

constexpr int kPollCadenceMs = 20;
constexpr int64_t kFnHoldPowerOnMs = 1500;    // 1.5 second hold to power on from SYSTEM_OFF
constexpr int64_t kWakeHoldGraceMs = 60;      // 60 ms GPIO startup grace/debounce window
constexpr int64_t kFnHoldShutdownMs = 3000;   // 3 second hold to shut down
constexpr int64_t kFnReleaseStableMs = 30;    // debounce before sys_poweroff
constexpr int64_t kBootReportDelayMs = 2500;  // delay in STATE_RUNNING for USB-C CDC ACM enum
constexpr uint8_t kRoutingPollDividerTicks = 10U;  // 10 * 20 ms = 200 ms jack-detect poll

void feed(AppMachine& m) {
    core::watchdog::feed(m.main_watchdog_channel);
    if (m.power != nullptr) {
        m.power->feed_watchdog();
    } else {
        core::watchdog::feed();
    }
}

void render_wake_hold_progress(leds::Leds* leds, int64_t elapsed_ms) {
    if (leds == nullptr) {
        return;
    }
    const int64_t clamped =
        (elapsed_ms < 0) ? 0 : ((elapsed_ms > kFnHoldPowerOnMs) ? kFnHoldPowerOnMs : elapsed_ms);
    uint32_t scaled = static_cast<uint32_t>((clamped * 1020) / kFnHoldPowerOnMs);
    if (scaled < 48U) {
        scaled = 48U;  // Immediate visible Play 1 LED confirmation on wake
    }
    for (int i = 0; i < leds::kNumTrack; ++i) {
        leds->set_track(i, 0U);
    }
    uint32_t rem = (scaled > 1020U) ? 1020U : scaled;
    for (int i = 0; i < leds::kNumPlay; ++i) {
        const uint8_t level = (rem >= 255U) ? 255U : static_cast<uint8_t>(rem);
        rem -= level;
        leds->set_play(i, level);
    }
}

void abort_wake_to_system_off(AppMachine& m) {
    LOG_INF("SP-1 Drum Machine: Function released early (< 1.5 s) — returning to SYSTEM_OFF");
    if (m.leds != nullptr) {
        m.leds->all_off();
    }
    sp1_hal_shutdown();
    if (m.power != nullptr) {
        m.power->prepare_for_system_off();
    }
    sys_poweroff();
    LOG_ERR("sys_poweroff() returned during early wake abort");
    k_sleep(K_FOREVER);
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

// ── Idle: 1.5 s hold-to-power-on gate on battery SYSTEM_OFF GPIO wake ──

static void idle_entry(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    m.wake_hold_required = false;
    m.wake_hold_start_ms = 0;

    if (m.power != nullptr) {
        m.power->refresh_usb_power_state();
        const bool woke_from_off = m.power->consume_system_off_gpio_wake();
        const bool vbus_present = m.power->is_external_power_present();

        if (woke_from_off && !vbus_present) {
            m.wake_hold_required = true;
            m.wake_hold_start_ms = k_uptime_get();
            // Immediately light Play 1 LED so user sees confirmation that Function woke the MCU.
            render_wake_hold_progress(m.leds, 0);
            LOG_INF("SP-1 Drum Machine: SYSTEM_OFF GPIO wake — hold Function 1.5 s to power on");
            return;
        }
    }

    LOG_INF("SP-1 Drum Machine: Idle -> Entering standalone Running state");
}

static smf_state_result idle_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    if (m.wake_hold_required) {
        const int64_t elapsed_ms = k_uptime_get() - m.wake_hold_start_ms;
        sp1_controls_snapshot_t wake_snap{};
        sp1_hal_controls_poll(&wake_snap);
        const bool hal_fn_pressed =
            (wake_snap.buttons_down & SP1_BTN_BIT(SP1_BTN_FN)) != 0U;

        if (m.power != nullptr) {
            m.power->refresh_usb_power_state();
            if (m.power->is_external_power_present()) {
                // USB-C VBUS connected during hold: power on immediately.
                m.wake_hold_required = false;
                m.fn_was_pressed = m.power->function_button_pressed();
                m.fn_hold_start_ms = 0;
                smf::set_state(m.ctx, app_states[STATE_RUNNING]);
                return SMF_EVENT_HANDLED;
            }

            const bool gpio_fn_pressed = m.power->function_button_pressed();
            if (elapsed_ms >= kWakeHoldGraceMs && !gpio_fn_pressed && !hal_fn_pressed) {
                abort_wake_to_system_off(m);
                return SMF_EVENT_HANDLED;
            }
        }

        render_wake_hold_progress(m.leds, elapsed_ms);

        if (elapsed_ms >= kFnHoldPowerOnMs) {
            m.wake_hold_required = false;
            // Require a fresh release + press before the 3 s shutdown timer can arm.
            m.fn_was_pressed = true;
            m.fn_hold_start_ms = 0;
            smf::set_state(m.ctx, app_states[STATE_RUNNING]);
            return SMF_EVENT_HANDLED;
        }

        k_sleep(K_MSEC(kPollCadenceMs));
        return SMF_EVENT_HANDLED;
    }

    // Cold boot / reset / USB-C VBUS present: enter STATE_RUNNING immediately.
    smf::set_state(m.ctx, app_states[STATE_RUNNING]);
    return SMF_EVENT_HANDLED;
}

// ── Running: 50 Hz SAADC faders + UI mapper + DSP sync + PWM LEDs ─

static void running_entry(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    if (!m.runtime_started && m.start_runtime != nullptr) {
        m.audio_active = m.start_runtime();
        m.runtime_started = true;
        m.applied_volume = SP1_UI_VOLUME_DEFAULT;
        m.routing_initialized = false;
        m.routing_poll_ticks = 0U;
        feed(m);
    }
    if (!m.boot_report_printed && m.boot_report_due_ms == 0 &&
        core::reset_breadcrumbs::has_boot_report()) {
        m.boot_report_due_ms = k_uptime_get() + kBootReportDelayMs;
    }
    LOG_INF("SP-1 Drum Machine: Running (48 kHz I2S0 audio=%s + 50 Hz control loop)",
            m.audio_active ? "active" : "fallback");
}

static smf_state_result running_run(void* o) {
    auto& m = smf::ctx<AppMachine>(o);
    feed(m);

    if (!m.boot_report_printed && m.boot_report_due_ms != 0 &&
        k_uptime_get() >= m.boot_report_due_ms) {
        m.boot_report_printed = true;
        core::reset_breadcrumbs::print_boot_report();
    }

    if (check_fn_hold_shutdown(m)) {
        smf::set_state(m.ctx, app_states[STATE_SHUTTING_DOWN]);
        return SMF_EVENT_HANDLED;
    }

    if (m.midi != nullptr) {
        m.midi->poll_faders();
    }

    if (m.audio_active) {
        if (!m.routing_initialized) {
            m.routing_initialized = true;
            m.routing_poll_ticks = 0U;
            sp1_hal_audio_set_volume(SP1_UI_VOLUME_DEFAULT);
            m.applied_volume = SP1_UI_VOLUME_DEFAULT;
            sp1_hal_audio_service_routing();
        } else {
            m.routing_poll_ticks++;
            if (m.routing_poll_ticks >= kRoutingPollDividerTicks) {
                m.routing_poll_ticks = 0U;
                sp1_hal_audio_service_routing();
            }
        }

        if (m.led_track_packed != nullptr && m.led_play_packed != nullptr) {
            const uint32_t track_w = __atomic_load_n(m.led_track_packed, __ATOMIC_ACQUIRE);
            const uint32_t play_w = __atomic_load_n(m.led_play_packed, __ATOMIC_ACQUIRE);
            const sp1_led_frame_t led_frame = {
                {
                    static_cast<uint8_t>(track_w & 0xFFU),
                    static_cast<uint8_t>((track_w >> 8U) & 0xFFU),
                    static_cast<uint8_t>((track_w >> 16U) & 0xFFU),
                    static_cast<uint8_t>((track_w >> 24U) & 0xFFU),
                },
                {
                    static_cast<uint8_t>(play_w & 0xFFU),
                    static_cast<uint8_t>((play_w >> 8U) & 0xFFU),
                    static_cast<uint8_t>((play_w >> 16U) & 0xFFU),
                    static_cast<uint8_t>((play_w >> 24U) & 0xFFU),
                },
            };
            sp1_hal_leds_set_frame(&led_frame);
        }

        if (m.target_volume != nullptr) {
            const uint8_t target_vol =
                static_cast<uint8_t>(__atomic_load_n(m.target_volume, __ATOMIC_ACQUIRE) & 0x7FU);
            if (target_vol != m.applied_volume) {
                sp1_hal_audio_set_volume(target_vol);
                m.applied_volume = target_vol;
            }
        }
    } else if (m.seq != nullptr && m.ui != nullptr) {
        /* Fallback 50 Hz visual/control tick when I2S/codec init failed:
         * keeps all 8 PWM LEDs, Euclidean step playback, buttons, faders,
         * and USB-MIDI live with exclusive single-thread ownership of seq/ui. */
        constexpr uint32_t kFallbackBlockFrames =
            (SP1_AUDIO_SAMPLE_RATE_HZ * static_cast<uint32_t>(kPollCadenceMs)) / 1000U;
        sp1_controls_snapshot_t snap{};
        sp1_hal_controls_poll(&snap);
        sp1_ui_mapper_update(m.ui, m.seq, &snap);

        const uint8_t target_vol = sp1_ui_mapper_get_master_volume(m.ui);
        if (target_vol != m.applied_volume) {
            sp1_hal_audio_set_volume(target_vol);
            m.applied_volume = target_vol;
        }

        sp1_seq_trigger_event_t triggers[SP1_SEQ_MAX_BLOCK_TRIGGERS];
        const uint32_t trig_count = sp1_seq_process_block(m.seq,
                                                          m.fallback_frame_counter,
                                                          kFallbackBlockFrames,
                                                          triggers,
                                                          SP1_SEQ_MAX_BLOCK_TRIGGERS);
        sp1_ui_mapper_on_triggers(m.ui, triggers, trig_count);
        m.fallback_frame_counter += static_cast<uint64_t>(kFallbackBlockFrames);

        sp1_led_frame_t led_frame{};
        sp1_ui_mapper_render_leds(
            m.ui, m.seq, static_cast<uint32_t>(kPollCadenceMs), &led_frame);
        sp1_hal_leds_set_frame(&led_frame);
    }

    if (m.power != nullptr) {
        m.power->service_tick();
    }
    sp1_audio_cycle_stats_t cyc_stats{};
    sp1_hal_audio_get_cycle_stats(&cyc_stats);
    (void)cyc_stats;
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
