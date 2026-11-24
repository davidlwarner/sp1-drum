#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/usb/bos.h>
#include <hal/nrf_power.h>

extern "C" {
#include <ump_stream_responder.h>
}

#include "AppStateMachine.hpp"
#include "Leds.hpp"
#include "MidiController.hpp"
#include "power/PowerManager.hpp"
#include "system/ResetBreadcrumbs.hpp"
#include "system/Watchdog.hpp"
#include "hal/sp1_hal.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

// ── USB composite device (CDC ACM + MIDI) ───────────────────────

#define SP_USB_VID  0x1209
#define SP_USB_PID  0x0001

USBD_DEVICE_DEFINE(sp_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   SP_USB_VID, SP_USB_PID);

USBD_DESC_LANG_DEFINE(sp_lang);
USBD_DESC_MANUFACTURER_DEFINE(sp_mfr, "Teenage Engineering");
USBD_DESC_PRODUCT_DEFINE(sp_product, "SP-1 MIDI Controller");
USBD_DESC_CONFIG_DEFINE(sp_fs_cfg, "FS Configuration");

USBD_CONFIGURATION_DEFINE(sp_fs_config, 0, 125, &sp_fs_cfg);

static struct usbd_context *init_usb(void) {
    int err;

    printk("USB: adding descriptors\r\n");
    err = usbd_add_descriptor(&sp_usbd, &sp_lang);
    if (err) { printk("USB lang desc: %d\r\n", err); return nullptr; }

    err = usbd_add_descriptor(&sp_usbd, &sp_mfr);
    if (err) { printk("USB mfr desc: %d\r\n", err); return nullptr; }

    err = usbd_add_descriptor(&sp_usbd, &sp_product);
    if (err) { printk("USB product desc: %d\r\n", err); return nullptr; }

    printk("USB: adding FS configuration\r\n");
    err = usbd_add_configuration(&sp_usbd, USBD_SPEED_FS, &sp_fs_config);
    if (err) { printk("USB config: %d\r\n", err); return nullptr; }

    printk("USB: registering all classes\r\n");
    err = usbd_register_all_classes(&sp_usbd, USBD_SPEED_FS, 1, nullptr);
    if (err) { printk("USB register classes: %d\r\n", err); return nullptr; }

    // Composite device: use IAD triple (Misc/0x02/0x01)
    usbd_device_set_code_triple(&sp_usbd, USBD_SPEED_FS,
                                USB_BCC_MISCELLANEOUS, 0x02, 0x01);

    printk("USB: init\r\n");
    err = usbd_init(&sp_usbd);
    if (err) { printk("USB init: %d\r\n", err); return nullptr; }

    printk("USB: enable\r\n");
    err = usbd_enable(&sp_usbd);
    if (err) { printk("USB enable: %d\r\n", err); return nullptr; }

    printk("USB: composite device ready\r\n");
    return &sp_usbd;
}

// ── USB MIDI device from device tree ────────────────────────────

#define USB_MIDI_DT_NODE DT_NODELABEL(usb_midi)
static const struct device *const midi_dev = DEVICE_DT_GET(USB_MIDI_DT_NODE);

// ── Globals ─────────────────────────────────────────────────────

static midi::MidiController g_midi;
static leds::Leds g_leds;
static power::PowerManager g_power;

/* Static RAM: 768 B sequencer + 80 B UI mapper + 296 B 4-voice synth + 41,124 B master FX */
static sp1_sequencer_t g_sequencer;
static sp1_ui_mapper_t g_ui_mapper;
static sp1_drum_synth_t g_drum_synth;
static sp1_master_fx_t g_master_fx;
static uint64_t g_audio_frame_counter = 0ULL;
static uint32_t g_audio_ctrl_decim = 0U;
static uint32_t g_led_track_packed = 0U;
static uint32_t g_led_play_packed = 0U;
static uint32_t g_target_volume = SP1_UI_VOLUME_DEFAULT;
static int g_main_wdt_channel = -1;

static constexpr uint32_t kAudioCtrlDecimBlocks = 12U; /* 12 * 80 frames = 960 frames = 20 ms (50 Hz) */
static constexpr uint32_t kAudioCtrlCadenceMs = 20U;

/* Static RAM: 4 * 96 * 4 B = 1536 B audio callback float block buffers */
static float g_dsp_main_l[SP1_AUDIO_BLOCK_FRAMES];
static float g_dsp_main_r[SP1_AUDIO_BLOCK_FRAMES];
static float g_dsp_send_l[SP1_AUDIO_BLOCK_FRAMES];
static float g_dsp_send_r[SP1_AUDIO_BLOCK_FRAMES];

// ── 48 kHz Hard-RT Audio Render Callback (96 frames / 2.0 ms) ───

static void sp1_audio_render_cb(int16_t *interleaved_stereo,
                                uint32_t num_frames,
                                void *user_ctx) {
    (void)user_ctx;
    if (interleaved_stereo == nullptr || num_frames == 0U) {
        return;
    }

    const uint32_t block_frames =
        (num_frames > SP1_AUDIO_BLOCK_FRAMES) ? SP1_AUDIO_BLOCK_FRAMES : num_frames;

    const bool run_ctrl_tick = (g_audio_ctrl_decim == 0U);
    const bool run_led_tick = (g_audio_ctrl_decim == 6U);
    g_audio_ctrl_decim++;
    if (g_audio_ctrl_decim >= kAudioCtrlDecimBlocks) {
        g_audio_ctrl_decim = 0U;
    }

    if (run_ctrl_tick) {
        sp1_controls_snapshot_t snap{};
        sp1_hal_controls_poll(&snap);
        sp1_ui_mapper_update(&g_ui_mapper, &g_sequencer, &snap);

        const sp1_ui_dsp_params_t *dsp = sp1_ui_mapper_get_dsp_params(&g_ui_mapper);
        if (dsp != nullptr) {
            sp1_drum_voices_set_kick_params(&g_drum_synth, dsp->kick_decay, dsp->kick_drive);
            sp1_drum_voices_set_snare_morph(&g_drum_synth, dsp->snare_morph);
            sp1_drum_voices_set_hat_decay(&g_drum_synth, dsp->hat_decay);
            sp1_drum_voices_set_perc_timbre(&g_drum_synth, dsp->perc_timbre);

            sp1_master_fx_set_dj_filter(&g_master_fx, dsp->dj_filter_pos, dsp->dj_resonance_q);
            sp1_master_fx_set_stutter(
                &g_master_fx, static_cast<sp1_stutter_div_t>(dsp->stutter_div));
            sp1_master_fx_set_bitcrusher(&g_master_fx, dsp->crush_amount);
            sp1_master_fx_set_dub_delay(
                &g_master_fx, dsp->dub_delay_wet, dsp->dub_delay_feedback);

            __atomic_store_n(&g_target_volume,
                             static_cast<uint32_t>(dsp->master_volume),
                             __ATOMIC_RELEASE);
        }
    }

    sp1_seq_trigger_event_t triggers[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    const uint32_t trig_count = sp1_seq_process_block(&g_sequencer,
                                                      g_audio_frame_counter,
                                                      block_frames,
                                                      triggers,
                                                      SP1_SEQ_MAX_BLOCK_TRIGGERS);
    sp1_ui_mapper_on_triggers(&g_ui_mapper, triggers, trig_count);

    if (run_led_tick) {
        sp1_led_frame_t led_frame{};
        sp1_ui_mapper_render_leds(&g_ui_mapper, &g_sequencer, kAudioCtrlCadenceMs, &led_frame);
        const uint32_t track_w =
            static_cast<uint32_t>(led_frame.track[0]) |
            (static_cast<uint32_t>(led_frame.track[1]) << 8U) |
            (static_cast<uint32_t>(led_frame.track[2]) << 16U) |
            (static_cast<uint32_t>(led_frame.track[3]) << 24U);
        const uint32_t play_w =
            static_cast<uint32_t>(led_frame.play[0]) |
            (static_cast<uint32_t>(led_frame.play[1]) << 8U) |
            (static_cast<uint32_t>(led_frame.play[2]) << 16U) |
            (static_cast<uint32_t>(led_frame.play[3]) << 24U);
        __atomic_store_n(&g_led_track_packed, track_w, __ATOMIC_RELEASE);
        __atomic_store_n(&g_led_play_packed, play_w, __ATOMIC_RELEASE);
    }

    sp1_drum_voices_render_block(&g_drum_synth,
                                 triggers,
                                 trig_count,
                                 g_dsp_main_l,
                                 g_dsp_main_r,
                                 g_dsp_send_l,
                                 g_dsp_send_r,
                                 block_frames);

    const uint32_t step_16th_frames =
        sp1_clock_get_step_frames(sp1_seq_clock_const(&g_sequencer));
    sp1_master_fx_process_block(&g_master_fx,
                                step_16th_frames,
                                g_dsp_main_l,
                                g_dsp_main_r,
                                g_dsp_send_l,
                                g_dsp_send_r,
                                interleaved_stereo,
                                block_frames);

    g_audio_frame_counter += static_cast<uint64_t>(block_frames);
}

// ── UMP Stream Responder ────────────────────────────────────────

static const struct ump_endpoint_dt_spec ump_ep_dt =
    UMP_ENDPOINT_DT_SPEC_GET(USB_MIDI_DT_NODE);

static const struct ump_stream_responder_cfg responder_cfg =
    UMP_STREAM_RESPONDER(midi_dev, usbd_midi_send, &ump_ep_dt);

// ── USB MIDI callbacks ──────────────────────────────────────────

static void on_midi_rx(const struct device*, const struct midi_ump ump) {
    if (UMP_MT(ump) == UMP_MT_UMP_STREAM) {
        ump_stream_respond(&responder_cfg, ump);
        return;
    }
    sp1_hal_midi_on_rx_ump32(ump.data[0]);
    g_midi.on_rx(ump);
}

static void on_midi_ready(const struct device*, const bool ready) {
    LOG_INF("USB MIDI %s", ready ? "ready" : "disconnected");
    g_midi.ready.store(ready, std::memory_order_relaxed);
    sp1_hal_midi_set_ready(ready);
}

static const struct usbd_midi_ops midi_ops = {
    .rx_packet_cb = on_midi_rx,
    .ready_cb = on_midi_ready,
};

// ── Input subsystem callback (buttons → HAL controls & MIDI) ────

static bool map_input_key_to_sp1_btn(uint16_t code, sp1_button_id_t *out) {
    switch (code) {
    case INPUT_KEY_1:           *out = SP1_BTN_TRACK_1;  return true;
    case INPUT_KEY_2:           *out = SP1_BTN_TRACK_2;  return true;
    case INPUT_KEY_3:           *out = SP1_BTN_TRACK_3;  return true;
    case INPUT_KEY_4:           *out = SP1_BTN_TRACK_4;  return true;
    case INPUT_KEY_PLAY:        *out = SP1_BTN_PLAY;     return true;
    case INPUT_KEY_BACK:        *out = SP1_BTN_RWD;      return true;
    case INPUT_KEY_VOLUMEDOWN:  *out = SP1_BTN_VOL_DOWN; return true;
    case INPUT_KEY_FASTFORWARD: *out = SP1_BTN_FWD;      return true;
    case INPUT_KEY_VOLUMEUP:    *out = SP1_BTN_VOL_UP;   return true;
    case INPUT_KEY_MENU:        *out = SP1_BTN_FN;       return true;
    default:                    return false;
    }
}

static void on_input_event(struct input_event *evt, void*) {
    if (evt == nullptr || evt->type != INPUT_EV_KEY || evt->code == INPUT_KEY_RESERVED) {
        return;
    }

    const bool pressed = (evt->value != 0);

    sp1_button_id_t hal_btn = SP1_BTN_TRACK_1;
    if (map_input_key_to_sp1_btn(evt->code, &hal_btn)) {
        sp1_hal_controls_on_button_event(hal_btn, pressed);
    } else if (!k_is_in_isr()) {
        LOG_INF("Input unmapped key code=%u val=%d",
                static_cast<unsigned int>(evt->code),
                static_cast<int>(evt->value));
    }
}
INPUT_CALLBACK_DEFINE(NULL, on_input_event, NULL);

// ── main ────────────────────────────────────────────────────────

BUILD_ASSERT(!IS_ENABLED(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT),
             "sp1-drum owns the single composite USBD context; do not pass "
             "-DSNIPPET=cdc-acm-console (it enables CDC_ACM_SERIAL_INITIALIZE_AT_BOOT "
             "and duplicates cdc_acm_uart0). Build without -DSNIPPET=cdc-acm-console.");

static void run_boot_led_sweep(int wdt_channel) {
    // Immediate visual proof on hardware that the MCU booted past the SP-1
    // bootloader (0x00000..0x1FFFF) and reached main(), before USB or audio init.
    g_leds.all_off();
    for (int i = 0; i < leds::kNumTrack; ++i) {
        g_leds.set_track(i, 220U);
        core::watchdog::feed(wdt_channel);
        k_msleep(40);
        g_leds.set_track(i, 40U);
    }
    for (int i = 0; i < leds::kNumPlay; ++i) {
        g_leds.set_play(i, 220U);
        core::watchdog::feed(wdt_channel);
        k_msleep(40);
        g_leds.set_play(i, 40U);
    }
    std::array<uint8_t, leds::kNumTotal> pulse{};
    pulse.fill(180U);
    g_leds.set_all(pulse);
    core::watchdog::feed(wdt_channel);
    k_msleep(80);
    g_leds.all_off();
    core::watchdog::feed(wdt_channel);
}

static bool start_sp1_runtime(void) {
    // Boot stopped: wait for the user to press Play before advancing steps.

    // Seed initial LED frame immediately so hardware shows stopped sequencer state.
    {
        sp1_led_frame_t init_leds{};
        sp1_ui_mapper_render_leds(&g_ui_mapper, &g_sequencer, 0U, &init_leds);
        const uint32_t track_w =
            static_cast<uint32_t>(init_leds.track[0]) |
            (static_cast<uint32_t>(init_leds.track[1]) << 8U) |
            (static_cast<uint32_t>(init_leds.track[2]) << 16U) |
            (static_cast<uint32_t>(init_leds.track[3]) << 24U);
        const uint32_t play_w =
            static_cast<uint32_t>(init_leds.play[0]) |
            (static_cast<uint32_t>(init_leds.play[1]) << 8U) |
            (static_cast<uint32_t>(init_leds.play[2]) << 16U) |
            (static_cast<uint32_t>(init_leds.play[3]) << 24U);
        __atomic_store_n(&g_led_track_packed, track_w, __ATOMIC_RELEASE);
        __atomic_store_n(&g_led_play_packed, play_w, __ATOMIC_RELEASE);
        sp1_hal_leds_set_frame(&init_leds);
    }
    if (g_main_wdt_channel >= 0) {
        core::watchdog::feed(g_main_wdt_channel);
    }
    core::watchdog::feed();

    bool audio_active = false;
    if (sp1_hal_audio_init(sp1_audio_render_cb, nullptr) < 0) {
        LOG_WRN("SP-1 Audio HAL init failed; continuing with live UI/LEDs");
    } else if (sp1_hal_audio_start() < 0) {
        LOG_WRN("SP-1 Audio HAL start failed; continuing with live UI/LEDs");
    } else {
        audio_active = true;
        LOG_INF("48 kHz I2S0 EasyDMA audio streaming active");
    }
    if (g_main_wdt_channel >= 0) {
        core::watchdog::feed(g_main_wdt_channel);
    }
    core::watchdog::feed();
    return audio_active;
}

int main(void) {
    core::reset_breadcrumbs::capture_early();
    core::reset_breadcrumbs::clear_live_markers();

    core::watchdog::start();
    const int main_wdt = core::watchdog::register_channel("main", 5000);
    g_main_wdt_channel = main_wdt;
    core::watchdog::feed(main_wdt);

    // ── LEDs & Power Monitor FIRST ──────────────────────────────
    g_leds.track[0] = PWM_DT_SPEC_GET(DT_NODELABEL(led_track_1));
    g_leds.track[1] = PWM_DT_SPEC_GET(DT_NODELABEL(led_track_2));
    g_leds.track[2] = PWM_DT_SPEC_GET(DT_NODELABEL(led_track_3));
    g_leds.track[3] = PWM_DT_SPEC_GET(DT_NODELABEL(led_track_4));
    g_leds.play[0] = PWM_DT_SPEC_GET(DT_NODELABEL(led_play_1));
    g_leds.play[1] = PWM_DT_SPEC_GET(DT_NODELABEL(led_play_2));
    g_leds.play[2] = PWM_DT_SPEC_GET(DT_NODELABEL(led_play_3));
    g_leds.play[3] = PWM_DT_SPEC_GET(DT_NODELABEL(led_play_4));
    g_leds.all_off();

    g_power.init();
    core::watchdog::feed(main_wdt);

    // Skip the boot LED sweep when waking on battery from SYSTEM_OFF via Function;
    // STATE_IDLE will animate the 1.5 s hold-to-power-on Play 1..4 LED fill instead.
    if (!(g_power.woke_from_system_off() && !g_power.is_external_power_present())) {
        run_boot_led_sweep(main_wdt);
    }

    LOG_INF("SP-1 Euclidean Drum Machine Booting...");

    // ── MIDI controller (register ops before enabling USB) ──────
    g_midi.midi_dev = midi_dev;
    g_midi.track_led_pwm = nullptr;

    if (device_is_ready(midi_dev)) {
        usbd_midi_set_ops(midi_dev, &midi_ops);
        LOG_INF("USB MIDI configured");
    } else {
        LOG_ERR("MIDI device not ready");
    }
    core::watchdog::feed(main_wdt);

    // ── USB composite device (CDC ACM console + MIDI) ───────────
    if (init_usb() == nullptr) {
        LOG_ERR("USB init failed");
    } else {
        LOG_INF("USB composite device ready (CDC ACM + MIDI)");
    }
    core::watchdog::feed(main_wdt);

    // ── Portable SP-1 Core, 4-Voice Drum Synth, Master FX & HAL ─
    if (sp1_hal_init() < 0) {
        LOG_WRN("SP-1 HAL init returned error");
    }
    sp1_seq_init(&g_sequencer);
    sp1_ui_mapper_init(&g_ui_mapper);
    sp1_drum_voices_init(&g_drum_synth);
    sp1_master_fx_init(&g_master_fx);
    g_audio_frame_counter = 0ULL;
    g_audio_ctrl_decim = 0U;
    __atomic_store_n(&g_target_volume, SP1_UI_VOLUME_DEFAULT, __ATOMIC_RELEASE);
    core::watchdog::feed(main_wdt);

    // ── Start HSM (always reached; STATE_IDLE gates battery SYSTEM_OFF wake) ─
    app::AppMachine machine{};
    machine.midi = &g_midi;
    machine.leds = &g_leds;
    machine.power = &g_power;
    machine.seq = &g_sequencer;
    machine.ui = &g_ui_mapper;
    machine.synth = &g_drum_synth;
    machine.fx = &g_master_fx;
    machine.led_track_packed = &g_led_track_packed;
    machine.led_play_packed = &g_led_play_packed;
    machine.target_volume = &g_target_volume;
    machine.start_runtime = start_sp1_runtime;
    machine.main_watchdog_channel = main_wdt;
    machine.audio_active = false;
    machine.runtime_started = false;
    machine.applied_volume = SP1_UI_VOLUME_DEFAULT;

    LOG_INF("Boot init complete, entering standalone HSM");
    app::run(machine);

    return 0;
}
