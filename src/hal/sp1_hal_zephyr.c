#include "hal/sp1_hal.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/audio/codec.h>
#include <zephyr/audio/midi.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_midi2.h>

#include <soc.h>

#include "audio/cs42l42_codec.h"

LOG_MODULE_REGISTER(sp1_hal, LOG_LEVEL_INF);

/* ── Static Hardware Device Bindings (stem_player.dts / app.overlay) ─────── */

static const struct device *const g_i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const g_cs42l42_dev = DEVICE_DT_GET(DT_NODELABEL(cs42l42));
static const struct device *const g_tas2505_dev = DEVICE_DT_GET(DT_NODELABEL(tas2505));
static const struct device *const g_usb_midi_dev = DEVICE_DT_GET(DT_NODELABEL(usb_midi));
static const struct device *const g_gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static const struct gpio_dt_spec g_cs42l42_rst =
    GPIO_DT_SPEC_GET(DT_NODELABEL(cs42l42), reset_gpios);
static const struct gpio_dt_spec g_tas2505_rst =
    GPIO_DT_SPEC_GET(DT_NODELABEL(tas2505), reset_gpios);

static const struct pwm_dt_spec g_track_leds[SP1_NUM_TRACK_LEDS] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(led_track_1)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_track_2)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_track_3)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_track_4)),
};

static const struct pwm_dt_spec g_play_leds[SP1_NUM_PLAY_LEDS] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(led_play_1)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_play_2)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_play_3)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_play_4)),
};

/* External 3.072 MHz audio oscillator enable pin (P0.13, active high) */
#define SP1_OSC_EN_PIN              13U
/* BTN_COM resistor-ladder & fader supply rail enable pin (P1.10, active high) */
#define SP1_BTN_COM_PIN             10U

/* ── Static RAM Allocations (Explicit Byte Budgets) ──────────────────────── */

/* 240 stereo frames (5.0 ms @ 48 kHz) * 2 channels * 4 bytes (24-bit-in-32-bit I2S word) = 1920 B/block.
 * Rendered as 3 x 80-frame DSP sub-blocks so portable 96-frame scratch buffers stay unchanged. */
#define SP1_I2S_DMA_SUB_FRAMES      80U
#define SP1_I2S_DMA_SUB_BLOCKS      3U
#define SP1_I2S_DMA_BLOCK_FRAMES    (SP1_I2S_DMA_SUB_FRAMES * SP1_I2S_DMA_SUB_BLOCKS)
#define SP1_I2S_DMA_BLOCK_SAMPLES   (SP1_I2S_DMA_BLOCK_FRAMES * SP1_AUDIO_CHANNELS)
#define SP1_I2S_DMA_BLOCK_BYTES     (SP1_I2S_DMA_BLOCK_SAMPLES * sizeof(int32_t))
#define SP1_I2S_DMA_BLOCK_COUNT     8U  /* 8 * 5.0 ms = 40 ms total DMA ring headroom */
#define SP1_I2S_PRIME_BLOCKS        4U  /* 4 * 5.0 ms = 20 ms pre-queued before START */
#define SP1_AUDIO_THREAD_STACK_SIZE 4096U
/* Preemptive priority 0 (above main at priority 1 and shell_uart at priority 7) so
 * 5 ms audio block rendering preempts control/logging work without starving cooperative
 * kernel driver threads (udc_nrf, usbd, sysworkq). */
#define SP1_AUDIO_THREAD_PRIO       K_PRIO_PREEMPT(0)

/* Static RAM: 8 * 1920 B = 15360 B I2S EasyDMA TX slab */
K_MEM_SLAB_DEFINE_STATIC(g_i2s_tx_slab, SP1_I2S_DMA_BLOCK_BYTES, SP1_I2S_DMA_BLOCK_COUNT, 4);

/* Static RAM: 4096 B audio render thread stack + ~128 B k_thread control block */
K_THREAD_STACK_DEFINE(g_audio_thread_stack, SP1_AUDIO_THREAD_STACK_SIZE);
static struct k_thread g_audio_thread_data;
static bool g_audio_thread_created = false;

/* Static RAM: 384 B DSP scratch block (up to 96 stereo int16_t frames per sub-block) */
static int16_t g_audio_dsp_scratch[SP1_AUDIO_BLOCK_SAMPLES];

/* Static RAM: 2 * 524 B = 1048 B lock-free MIDI RX/TX SPSC queues */
static sp1_midi_spsc_queue_t g_midi_rx_q;
static sp1_midi_spsc_queue_t g_midi_tx_q;
static uint32_t g_midi_ready_flag = 0U;
static int64_t g_midi_tx_backoff_until_ms = 0;

/* Static RAM: 16 B fader filters + 40 B live controls + 84 B double-buffered bus + 2 B edge state + spinlock */
static struct k_spinlock g_controls_lock;
static sp1_fader_filter_t g_fader_filters[SP1_NUM_FADERS];
static sp1_controls_snapshot_t g_live_controls;
static sp1_controls_bus_t g_controls_bus;
static uint16_t g_prev_polled_buttons_down = 0U;
static uint16_t g_latched_pressed_buttons = 0U;
static uint16_t g_latched_released_buttons = 0U;

/* Static RAM: 32 B DWT CYCCNT audio block cycle telemetry + spinlock */
static struct k_spinlock g_cycle_stats_lock;
static sp1_audio_cycle_stats_t g_audio_cycle_stats;

static sp1_audio_render_cb_t g_audio_cb = NULL;
static void *g_audio_ctx = NULL;
static uint32_t g_audio_running_flag = 0U;
static bool g_audio_configured = false;
static bool g_cs42l42_ready = false;
static bool g_tas2505_ready = false;
static uint8_t g_audio_volume = 64U;
static bool g_audio_global_muted = false;
static int8_t g_hp_routed_state = -1; /* -1 = unapplied, 0 = speaker, 1 = headphones */
static int8_t g_pending_hp_state = -1;
static uint8_t g_plug_read_fail_count = 0U;
static uint8_t g_plug_poll_backoff_ticks = 0U;
static int16_t g_last_plug_status = -1;

/* ── Hardware Watchdog Reload Helper (Safe Across Slow I2C Bring-Up) ─────── */

static inline void sp1_wdt_feed_all(void) {
#if defined(NRF_WDT)
    for (uint32_t ch = 0U; ch < 8U; ++ch) {
        NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
    }
#endif
}

/* ── ARMv7E-M DWT CYCCNT Cycle Counter Instrumentation (Cortex-M4F) ──────── */

static inline void sp1_dwt_cyccnt_init(void) {
#if defined(__ARM_ARCH_7EM__)
    /* ARMv7E-M CoreDebug DEMCR (0xE000EDFC) bit 24 = TRCENA;
     * DWT CTRL (0xE0001000) bit 0 = CYCCNTENA; DWT CYCCNT (0xE0001004). */
    volatile uint32_t *const demcr = (volatile uint32_t *)0xE000EDFCUL;
    volatile uint32_t *const dwt_ctrl = (volatile uint32_t *)0xE0001000UL;
    volatile uint32_t *const dwt_cyccnt = (volatile uint32_t *)0xE0001004UL;
    *demcr |= (1UL << 24U);
    *dwt_cyccnt = 0UL;
    *dwt_ctrl |= 1UL;
#endif
}

static inline uint32_t sp1_dwt_cyccnt_read(void) {
#if defined(__ARM_ARCH_7EM__)
    volatile const uint32_t *const dwt_cyccnt = (volatile const uint32_t *)0xE0001004UL;
    return *dwt_cyccnt;
#else
    return k_cycle_get_32();
#endif
}

/* Minimal atomic fatal fault handler (records .noinit crash breadcrumbs and
 * immediately cold-reboots) lives in
 * subsys/system/ResetBreadcrumbs.cpp::k_sys_fatal_error_handler(). */

static void sp1_i2s_prime_silent(uint32_t count) {
    for (uint32_t i = 0U; i < count; ++i) {
        void *dma_block = NULL;
        if (k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_NO_WAIT) != 0) {
            break;
        }
        memset(dma_block, 0, SP1_I2S_DMA_BLOCK_BYTES);
        if (i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES) < 0) {
            k_mem_slab_free(&g_i2s_tx_slab, dma_block);
            break;
        }
    }
}

/* ── High-Priority Audio Render Thread ───────────────────────────────────── */

static void sp1_audio_thread_entry(void *p1, void *p2, void *p3) {
    (void)p1;
    (void)p2;
    (void)p3;

#if defined(__ARM_FP) && (__ARM_FP != 0)
    /* Enable Cortex-M4F FPU Flush-To-Zero (FZ, bit 24) and Default-NaN (DN, bit 25)
     * on the real-time audio render thread to prevent subnormal trap stalls. */
    uint32_t fpscr = 0U;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1U << 24U) | (1U << 25U);
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr));
#endif

    sp1_dwt_cyccnt_init();

    uint32_t consecutive_write_fails = 0U;
    uint32_t recovery_count = 0U;
    int64_t last_recovery_log_ms = -10000;

    while (__atomic_load_n(&g_audio_running_flag, __ATOMIC_ACQUIRE) != 0U) {
        void *dma_block = NULL;
        if (k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_MSEC(50)) != 0) {
            k_spinlock_key_t key = k_spin_lock(&g_cycle_stats_lock);
            g_audio_cycle_stats.underrun_count += 1U;
            k_spin_unlock(&g_cycle_stats_lock, key);
            continue;
        }

        const uint32_t cyc_start = sp1_dwt_cyccnt_read();

        int32_t *dst_i2s = (int32_t *)dma_block;
        for (uint32_t sub = 0U; sub < SP1_I2S_DMA_SUB_BLOCKS; ++sub) {
            if (g_audio_cb != NULL) {
                g_audio_cb(g_audio_dsp_scratch, SP1_I2S_DMA_SUB_FRAMES, g_audio_ctx);
            } else {
                memset(g_audio_dsp_scratch, 0,
                       SP1_I2S_DMA_SUB_FRAMES * SP1_AUDIO_CHANNELS * sizeof(int16_t));
            }
            sp1_audio_pack_s16_to_i2s24_in_32(
                g_audio_dsp_scratch, dst_i2s, SP1_I2S_DMA_SUB_FRAMES);
            dst_i2s += (SP1_I2S_DMA_SUB_FRAMES * SP1_AUDIO_CHANNELS);
        }

        const uint32_t cyc_elapsed = sp1_dwt_cyccnt_read() - cyc_start;
        const uint32_t cyc_per_96 =
            (uint32_t)(((uint64_t)cyc_elapsed * SP1_AUDIO_BLOCK_FRAMES) / SP1_I2S_DMA_BLOCK_FRAMES);
        sp1_hal_audio_record_cycles(cyc_per_96);

        const int write_err = i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES);
        if (write_err < 0) {
            k_mem_slab_free(&g_i2s_tx_slab, dma_block);
            k_spinlock_key_t key = k_spin_lock(&g_cycle_stats_lock);
            g_audio_cycle_stats.underrun_count += 1U;
            k_spin_unlock(&g_cycle_stats_lock, key);

            consecutive_write_fails += 1U;
            if (write_err == -EIO) {
                /* In Zephyr i2s_nrfx, I2S_TRIGGER_DROP transitions ERROR (4) -> READY (1)
                 * and triggers NRF_I2S->TASKS_STOP. Always wait 5 ms for the asynchronous
                 * EVENTS_STOPPED ISR (nrfx_i2s_uninit + last_tx_buffer free) to complete
                 * before re-priming and re-starting I2S0. */
                const int drop_err = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
                k_msleep(5);
                int start_err = drop_err;
                if (drop_err == 0 &&
                    __atomic_load_n(&g_audio_running_flag, __ATOMIC_ACQUIRE) != 0U) {
                    sp1_i2s_prime_silent(SP1_I2S_PRIME_BLOCKS);
                    start_err = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
                }
                recovery_count += 1U;
                const int64_t now_ms = k_uptime_get();
                if ((now_ms - last_recovery_log_ms) >= 2000) {
                    last_recovery_log_ms = now_ms;
                    if (drop_err == 0 && start_err == 0) {
                        LOG_WRN("I2S0 TX recovered from state ERROR (count=%u)",
                                (unsigned)recovery_count);
                    } else {
                        LOG_ERR("I2S0 TX recovery failed (drop=%d start=%d count=%u)",
                                drop_err, start_err, (unsigned)recovery_count);
                    }
                }
                if (drop_err < 0 || start_err < 0 || consecutive_write_fails > 1U) {
                    k_msleep(5);
                }
            } else {
                k_msleep(2);
            }
            continue;
        }
        consecutive_write_fails = 0U;
    }
    g_audio_thread_created = false;
}

/* ── HAL Controls & 8-Channel PWM LEDs ───────────────────────────────────── */

static const char *sp1_button_name(sp1_button_id_t btn) {
    switch (btn) {
    case SP1_BTN_TRACK_1:  return "T1";
    case SP1_BTN_TRACK_2:  return "T2";
    case SP1_BTN_TRACK_3:  return "T3";
    case SP1_BTN_TRACK_4:  return "T4";
    case SP1_BTN_PLAY:     return "PLAY";
    case SP1_BTN_RWD:      return "RWD";
    case SP1_BTN_VOL_DOWN: return "VOL-";
    case SP1_BTN_FWD:      return "FWD";
    case SP1_BTN_VOL_UP:   return "VOL+";
    case SP1_BTN_FN:       return "FN";
    default:               return "?";
    }
}

static int16_t g_fader_raw_latest[SP1_NUM_FADERS];
static int8_t g_prev_ain0_btn = -1;
static int8_t g_prev_ain1_btn = -1;

int sp1_hal_controls_init(void) {
#if defined(NRF_P1)
    /* Ensure P1.10 (BTN_COM) rail powering both resistor ladders and 4 faders is driven high. */
    NRF_P1->OUTSET = (1UL << SP1_BTN_COM_PIN);
    NRF_P1->PIN_CNF[SP1_BTN_COM_PIN] =
        (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
    NRF_P1->OUTSET = (1UL << SP1_BTN_COM_PIN);
#endif
    k_spinlock_key_t key = k_spin_lock(&g_controls_lock);
    for (uint32_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        sp1_fader_filter_init(&g_fader_filters[i]);
        g_fader_raw_latest[i] = 0;
    }
    sp1_controls_snapshot_init(&g_live_controls);
    sp1_controls_bus_init(&g_controls_bus);
    g_prev_polled_buttons_down = 0U;
    g_latched_pressed_buttons = 0U;
    g_latched_released_buttons = 0U;
    g_prev_ain0_btn = -1;
    g_prev_ain1_btn = -1;
    k_spin_unlock(&g_controls_lock, key);
    return 0;
}

int16_t sp1_hal_controls_get_fader_raw(uint8_t fader_idx) {
    if (fader_idx >= SP1_NUM_FADERS) {
        return 0;
    }
    return __atomic_load_n(&g_fader_raw_latest[fader_idx], __ATOMIC_ACQUIRE);
}

void sp1_hal_controls_on_button_event(sp1_button_id_t btn, bool pressed) {
    if ((unsigned)btn >= (unsigned)SP1_BTN_COUNT) {
        return;
    }

    const uint16_t bit = SP1_BTN_BIT(btn);
    k_spinlock_key_t key = k_spin_lock(&g_controls_lock);
    const uint16_t prev_down = g_live_controls.buttons_down;
    if (pressed) {
        if ((prev_down & bit) == 0U) {
            g_latched_pressed_buttons |= bit;
        }
        g_live_controls.buttons_down |= bit;
    } else {
        if ((prev_down & bit) != 0U) {
            g_latched_released_buttons |= bit;
        }
        g_live_controls.buttons_down &= (uint16_t)(~bit);
    }
    const uint16_t next_down = g_live_controls.buttons_down;

    g_live_controls.seq++;
    sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
    k_spin_unlock(&g_controls_lock, key);

    if (next_down != prev_down && !k_is_in_isr()) {
        LOG_INF("Buttons: buttons=0x%04x (btn=%u %s %s)",
                (unsigned)next_down,
                (unsigned)btn,
                sp1_button_name(btn),
                pressed ? "DOWN" : "UP");
    }
}

void sp1_hal_controls_on_fader_raw(uint8_t fader_idx, int16_t raw_sample) {
    if (fader_idx >= SP1_NUM_FADERS) {
        return;
    }

    float norm = 0.0f;
    uint8_t cc7 = 0U;
    k_spinlock_key_t key = k_spin_lock(&g_controls_lock);
    if (sp1_fader_filter_update(&g_fader_filters[fader_idx], raw_sample, &norm, &cc7)) {
        g_live_controls.fader_raw[fader_idx] = (uint16_t)g_fader_filters[fader_idx].raw_clamped;
        g_live_controls.fader_norm[fader_idx] = norm;
        g_live_controls.fader_cc[fader_idx] = cc7;
        g_live_controls.seq++;
        sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
    }
    k_spin_unlock(&g_controls_lock, key);
}

void sp1_hal_controls_poll(sp1_controls_snapshot_t *out_snapshot) {
    sp1_controls_snapshot_t snap;
    k_spinlock_key_t key = k_spin_lock(&g_controls_lock);
    sp1_controls_bus_read(&g_controls_bus, &snap);

    /* Derive one-shot rising/falling edge masks merged with latched edges so
     * brief taps between 20 ms polls are never dropped. */
    const uint16_t cur_down = snap.buttons_down;
    const uint16_t prev_down = g_prev_polled_buttons_down;
    snap.buttons_pressed =
        (uint16_t)((cur_down & (uint16_t)(~prev_down)) | g_latched_pressed_buttons);
    snap.buttons_released =
        (uint16_t)(((uint16_t)(~cur_down) & prev_down) | g_latched_released_buttons);
    g_prev_polled_buttons_down = cur_down;
    g_latched_pressed_buttons = 0U;
    g_latched_released_buttons = 0U;
    k_spin_unlock(&g_controls_lock, key);

    if (out_snapshot != NULL) {
        *out_snapshot = snap;
    }
}

static void sp1_pwm_apply(const struct pwm_dt_spec *spec, uint8_t brightness) {
    if (spec == NULL || !pwm_is_ready_dt(spec)) {
        return;
    }
    const uint32_t pulse = (spec->period * (uint32_t)brightness) / 255U;
    (void)pwm_set_pulse_dt(spec, pulse);
}

void sp1_hal_leds_set_frame(const sp1_led_frame_t *frame) {
    if (frame == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
        sp1_pwm_apply(&g_track_leds[i], frame->track[i]);
    }
    for (uint32_t i = 0U; i < SP1_NUM_PLAY_LEDS; ++i) {
        sp1_pwm_apply(&g_play_leds[i], frame->play[i]);
    }
}

void sp1_hal_leds_all_off(void) {
    const sp1_led_frame_t off = { { 0U, 0U, 0U, 0U }, { 0U, 0U, 0U, 0U } };
    sp1_hal_leds_set_frame(&off);
}

/* ── HAL MIDI (Lock-Free SPSC <-> USB-C UMP Bridge) ──────────────────────── */

int sp1_hal_midi_init(void) {
    sp1_midi_queue_init(&g_midi_rx_q);
    sp1_midi_queue_init(&g_midi_tx_q);
    __atomic_store_n(&g_midi_ready_flag, 0U, __ATOMIC_RELEASE);
    g_midi_tx_backoff_until_ms = 0;
    return 0;
}

bool sp1_hal_midi_is_ready(void) {
    return __atomic_load_n(&g_midi_ready_flag, __ATOMIC_ACQUIRE) != 0U;
}

void sp1_hal_midi_set_ready(bool ready) {
    __atomic_store_n(&g_midi_ready_flag, ready ? 1U : 0U, __ATOMIC_RELEASE);
    g_midi_tx_backoff_until_ms = 0;
}

bool sp1_hal_midi_send(const sp1_midi_msg_t *msg) {
    return sp1_midi_queue_push(&g_midi_tx_q, msg);
}

bool sp1_hal_midi_receive(sp1_midi_msg_t *out_msg) {
    return sp1_midi_queue_pop(&g_midi_rx_q, out_msg);
}

void sp1_hal_midi_flush_tx(void) {
    const bool usb_ready = sp1_hal_midi_is_ready() && device_is_ready(g_usb_midi_dev);
    const int64_t now_ms = k_uptime_get();
    const bool in_backoff = (now_ms < g_midi_tx_backoff_until_ms);

    sp1_midi_msg_t msg;
    while (sp1_midi_queue_pop(&g_midi_tx_q, &msg)) {
        if (!usb_ready || in_backoff) {
            continue;
        }
        uint32_t word0 = 0U;
        if (sp1_midi_encode_ump32(&msg, 0U, &word0)) {
            const struct midi_ump ump = { .data = { word0 } };
            if (usbd_midi_send(g_usb_midi_dev, ump) < 0) {
                /* Host is not draining USB MIDI IN (no DAW open): back off 1 s and
                 * drop remaining stale TX packets so net_buf allocation never floods. */
                g_midi_tx_backoff_until_ms = now_ms + 1000;
                while (sp1_midi_queue_pop(&g_midi_tx_q, &msg)) {
                }
                break;
            }
        }
    }
}

void sp1_hal_midi_on_rx_ump32(uint32_t word0) {
    sp1_midi_msg_t msg;
    if (sp1_midi_decode_ump32(word0, sp1_hal_time_us(), &msg)) {
        (void)sp1_midi_queue_push(&g_midi_rx_q, &msg);
    }
}

/* ── HAL Audio (CS42L42 + TAS2505 + I2S0 48 kHz 24-in-32 EasyDMA) ────────── */

int sp1_hal_audio_init(sp1_audio_render_cb_t render_cb, void *user_ctx) {
    g_audio_cb = render_cb;
    g_audio_ctx = user_ctx;
    g_audio_configured = false;
    g_cs42l42_ready = false;
    g_tas2505_ready = false;
    g_audio_volume = 64U;
    g_audio_global_muted = false;
    g_hp_routed_state = -1;
    g_pending_hp_state = -1;
    g_plug_read_fail_count = 0U;
    g_plug_poll_backoff_ticks = 0U;
    g_last_plug_status = -1;
    sp1_hal_audio_reset_cycle_stats();

    if (!device_is_ready(g_i2s_dev)) {
        LOG_WRN("I2S0 device not ready");
        return -ENODEV;
    }

    /* Ensure external 3.072 MHz audio oscillator (P0.13) is enabled prior to codec PLL lock. */
    if (device_is_ready(g_gpio0_dev)) {
        (void)gpio_pin_configure(g_gpio0_dev, SP1_OSC_EN_PIN, GPIO_OUTPUT_ACTIVE);
        k_msleep(5);
    }
    sp1_wdt_feed_all();

    /* On SP-1 hardware, the external 3.072 MHz oscillator drives BCLK (P0.12) and
     * CS42L42 masters LRCK (P0.11), so nRF52840 I2S0 runs as a clock slave. */
    struct i2s_config i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.word_size = 24U;
    i2s_cfg.channels = SP1_AUDIO_CHANNELS;
    i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
    i2s_cfg.options = I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE;
    i2s_cfg.frame_clk_freq = SP1_AUDIO_SAMPLE_RATE_HZ;
    i2s_cfg.mem_slab = &g_i2s_tx_slab;
    i2s_cfg.block_size = SP1_I2S_DMA_BLOCK_BYTES;
    i2s_cfg.timeout = 500;

    int err = i2s_configure(g_i2s_dev, I2S_DIR_TX, &i2s_cfg);
    if (err < 0) {
        LOG_WRN("I2S0 slave configure failed: %d", err);
        return err;
    }
    sp1_wdt_feed_all();

    struct audio_codec_cfg codec_cfg;
    memset(&codec_cfg, 0, sizeof(codec_cfg));
    codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
    codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
    codec_cfg.dai_cfg.i2s = i2s_cfg;

    /* CS42L42 masters the 48 kHz LRCK frame clock; treat I2C failure cleanly without hanging. */
    if (device_is_ready(g_cs42l42_dev)) {
        const int cs_err = audio_codec_configure(g_cs42l42_dev, &codec_cfg);
        if (cs_err == 0) {
            g_cs42l42_ready = true;
        } else {
            LOG_WRN("CS42L42 configure failed (%d); continuing without headphone DAC", cs_err);
        }
    } else {
        LOG_WRN("CS42L42 device not ready");
    }
    sp1_wdt_feed_all();

    /* TAS2505 may NACK when full SWD probe is attached; treat failure as non-fatal. */
    if (device_is_ready(g_tas2505_dev)) {
        const int tas_err = audio_codec_configure(g_tas2505_dev, &codec_cfg);
        if (tas_err == 0) {
            g_tas2505_ready = true;
        } else {
            LOG_WRN("TAS2505 configure failed (%d); continuing without speaker amp", tas_err);
        }
    }
    sp1_wdt_feed_all();

    /* Without CS42L42 mastering LRCK, I2S0 slave DMA will not receive frame clocks. */
    if (!g_cs42l42_ready) {
        return -EIO;
    }

    g_audio_configured = true;
    return 0;
}

static void sp1_audio_apply_routing(bool hp_connected, uint8_t plug_status) {
    audio_property_value_t mute_on;
    audio_property_value_t mute_off;
    audio_property_value_t vol_val;
    mute_on.mute = true;
    mute_off.mute = false;
    vol_val.vol = (int)g_audio_volume;

    sp1_wdt_feed_all();
    if (g_audio_global_muted) {
        if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
            (void)audio_codec_set_property(
                g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_on);
        }
        if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
            (void)audio_codec_set_property(
                g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_on);
        }
        sp1_wdt_feed_all();
        return;
    }

    if (hp_connected) {
        /* Mute speaker first, then set volume & unmute CS42L42 headphones. */
        if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
            (void)audio_codec_set_property(
                g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_on);
        }
        if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
            (void)audio_codec_set_property(
                g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, vol_val);
            (void)audio_codec_set_property(
                g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_off);
            cs42l42_codec_log_state(g_cs42l42_dev, "hp_route");
        }
        LOG_INF("Audio route -> Headphones (CS42L42, vol=%u/127, 0x130F=0x%02x)",
                (unsigned)g_audio_volume, (unsigned)plug_status);
    } else {
        /* Mute headphones first, then set volume & unmute TAS2505 speaker. */
        if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
            (void)audio_codec_set_property(
                g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_on);
        }
        if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
            (void)audio_codec_set_property(
                g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, vol_val);
            (void)audio_codec_set_property(
                g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, mute_off);
        }
        LOG_INF("Audio route -> Speaker (TAS2505, vol=%u/127, 0x130F=0x%02x)",
                (unsigned)g_audio_volume, (unsigned)plug_status);
    }
    sp1_wdt_feed_all();
}

int sp1_hal_audio_start(void) {
    if (!g_audio_configured) {
        return -EINVAL;
    }

    __atomic_store_n(&g_audio_running_flag, 1U, __ATOMIC_RELEASE);

    /* Pre-fill and queue four silent 5 ms DMA TX slabs (20 ms) before triggering I2S0 START. */
    sp1_i2s_prime_silent(SP1_I2S_PRIME_BLOCKS);

    const int err = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (err < 0) {
        __atomic_store_n(&g_audio_running_flag, 0U, __ATOMIC_RELEASE);
        return err;
    }

    if (!g_audio_thread_created) {
        g_audio_thread_created = true;
        (void)k_thread_create(&g_audio_thread_data,
                              g_audio_thread_stack,
                              K_THREAD_STACK_SIZEOF(g_audio_thread_stack),
                              sp1_audio_thread_entry,
                              NULL, NULL, NULL,
                              SP1_AUDIO_THREAD_PRIO,
                              0U,
                              K_NO_WAIT);
        (void)k_thread_name_set(&g_audio_thread_data, "sp1_audio");
    }

    return 0;
}

int sp1_hal_audio_stop(void) {
    __atomic_store_n(&g_audio_running_flag, 0U, __ATOMIC_RELEASE);

    /* Mute speaker amp and headphone DAC before dropping I2S0 clocks to prevent pop. */
    if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
        audio_codec_stop_output(g_tas2505_dev);
    }
    if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
        audio_codec_stop_output(g_cs42l42_dev);
    }
    g_hp_routed_state = -1;
    g_pending_hp_state = -1;
    if (g_audio_configured && device_is_ready(g_i2s_dev)) {
        (void)i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
    }
    if (g_audio_thread_created) {
        (void)k_thread_join(&g_audio_thread_data, K_MSEC(50));
        g_audio_thread_created = false;
    }
    return 0;
}

int sp1_hal_audio_submit_block(const int16_t *interleaved_stereo, uint32_t num_frames) {
    if (!g_audio_configured || interleaved_stereo == NULL ||
        num_frames != SP1_I2S_DMA_BLOCK_FRAMES) {
        return -EINVAL;
    }

    void *dma_block = NULL;
    int err = k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_NO_WAIT);
    if (err < 0) {
        return err;
    }

    sp1_audio_pack_s16_to_i2s24_in_32(
        interleaved_stereo, (int32_t *)dma_block, SP1_I2S_DMA_BLOCK_FRAMES);

    err = i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES);
    if (err < 0) {
        k_mem_slab_free(&g_i2s_tx_slab, dma_block);
    }
    return err;
}

void sp1_hal_audio_set_volume(uint8_t volume_0_to_127) {
    const uint8_t clamped = (volume_0_to_127 > 127U) ? 127U : volume_0_to_127;
    const bool changed = (clamped != g_audio_volume);
    g_audio_volume = clamped;

    /* Both CS42L42 and TAS2505 map logical 0..127 perceptually (0 = mute,
     * 1..127 = -42 dB .. 0 dB, 64 = -21 dB) while preserving muted codec state. */
    audio_property_value_t val;
    val.vol = (int)clamped;

    sp1_wdt_feed_all();
    if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
        (void)audio_codec_set_property(
            g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
    }
    if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
        (void)audio_codec_set_property(
            g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
    }
    sp1_wdt_feed_all();

    if (changed && !k_is_in_isr()) {
        LOG_INF("Master volume: vol=%u/127 (route=%s)",
                (unsigned)clamped,
                (g_hp_routed_state == 1) ? "Headphones" : "Speaker");
    }
}

void sp1_hal_audio_set_mute(bool muted) {
    g_audio_global_muted = muted;

    if (muted) {
        audio_property_value_t val;
        val.mute = true;
        if (g_cs42l42_ready && device_is_ready(g_cs42l42_dev)) {
            (void)audio_codec_set_property(
                g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
        }
        if (g_tas2505_ready && device_is_ready(g_tas2505_dev)) {
            (void)audio_codec_set_property(
                g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
        }
    } else {
        g_hp_routed_state = -1;
        g_pending_hp_state = -1;
        sp1_hal_audio_service_routing();
    }
}

void sp1_hal_audio_service_routing(void) {
    if (!g_audio_configured || !g_cs42l42_ready || !device_is_ready(g_cs42l42_dev)) {
        return;
    }

    if (g_plug_poll_backoff_ticks > 0U) {
        g_plug_poll_backoff_ticks--;
        return;
    }

    sp1_wdt_feed_all();
    uint8_t plug_status = 0U;
    const bool read_ok = cs42l42_codec_read_plug_status(g_cs42l42_dev, &plug_status);
    sp1_wdt_feed_all();

    if (!read_ok) {
        /* Never treat a transient I2C read error as an unplug event or hammer I2C0. */
        if (g_plug_read_fail_count < 255U) {
            g_plug_read_fail_count++;
        }
        if (g_plug_read_fail_count >= 3U) {
            g_plug_poll_backoff_ticks = 10U; /* 2 s backoff */
        }
        if (g_hp_routed_state < 0) {
            g_hp_routed_state = 0;
            sp1_audio_apply_routing(false, 0x0CU);
        }
        return;
    }
    g_plug_read_fail_count = 0U;

    const bool hp_connected = cs42l42_codec_plug_status_is_connected(plug_status);

    if ((int16_t)plug_status != g_last_plug_status) {
        if (g_last_plug_status >= 0) {
            LOG_INF("CS42L42 TSRS_PLUG_STATUS (0x130F): 0x%02x -> 0x%02x (hp=%u)",
                    (unsigned)g_last_plug_status, (unsigned)plug_status,
                    hp_connected ? 1U : 0U);
        }
        g_last_plug_status = (int16_t)plug_status;
    }

    const int8_t desired = hp_connected ? 1 : 0;
    if (desired == g_hp_routed_state) {
        g_pending_hp_state = desired;
        return;
    }

    /* Apply initial boot route immediately; debounce subsequent plug/unplug transitions
     * across 2 consecutive 200 ms polls to avoid contact-scrape I2C chatter. */
    if (g_hp_routed_state >= 0 && g_pending_hp_state != desired) {
        g_pending_hp_state = desired;
        return;
    }

    g_pending_hp_state = desired;
    g_hp_routed_state = desired;
    sp1_audio_apply_routing(hp_connected, plug_status);
}

void sp1_hal_audio_record_cycles(uint32_t elapsed_cycles) {
    k_spinlock_key_t key = k_spin_lock(&g_cycle_stats_lock);
    sp1_audio_cycle_stats_record(&g_audio_cycle_stats, elapsed_cycles);
    k_spin_unlock(&g_cycle_stats_lock, key);
}

#if defined(__GNUC__)
__attribute__((used, noinline))
#endif
void sp1_hal_audio_get_cycle_stats(sp1_audio_cycle_stats_t *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&g_cycle_stats_lock);
    *out_stats = g_audio_cycle_stats;
    k_spin_unlock(&g_cycle_stats_lock, key);
}

void sp1_hal_audio_reset_cycle_stats(void) {
    k_spinlock_key_t key = k_spin_lock(&g_cycle_stats_lock);
    sp1_audio_cycle_stats_reset(&g_audio_cycle_stats);
    k_spin_unlock(&g_cycle_stats_lock, key);
}

size_t sp1_hal_audio_get_thread_free_stack(void) {
    if (!g_audio_thread_created) {
        return 0U;
    }
    size_t unused = 0U;
    if (k_thread_stack_space_get(&g_audio_thread_data, &unused) == 0) {
        return unused;
    }
    return 0U;
}

void sp1_hal_audio_shutdown(void) {
    (void)sp1_hal_audio_stop();
    sp1_hal_audio_set_mute(true);

#if defined(NRF_I2S)
    /* Force-halt slave-mode I2S0 at register level before cutting clocks for SYSTEM_OFF. */
    NRF_I2S->TASKS_STOP = 1U;
    NRF_I2S->ENABLE = 0U;
#endif

    /* Assert hardware reset on TAS2505 (P0.09) and CS42L42 (P0.15) to prevent pop/drain. */
    if (gpio_is_ready_dt(&g_tas2505_rst)) {
        (void)gpio_pin_configure_dt(&g_tas2505_rst, GPIO_OUTPUT_ACTIVE);
    }
    if (gpio_is_ready_dt(&g_cs42l42_rst)) {
        (void)gpio_pin_configure_dt(&g_cs42l42_rst, GPIO_OUTPUT_ACTIVE);
    }

    /* Disable external 3.072 MHz audio oscillator (P0.13) before sys_poweroff(). */
    if (device_is_ready(g_gpio0_dev)) {
        (void)gpio_pin_configure(g_gpio0_dev, SP1_OSC_EN_PIN, GPIO_OUTPUT_INACTIVE);
    }

    g_audio_configured = false;
    g_cs42l42_ready = false;
    g_tas2505_ready = false;
}

/* ── Top-Level HAL Lifecycle & Monotonic Time ────────────────────────────── */

int sp1_hal_init(void) {
    int err = sp1_hal_controls_init();
    if (err < 0) {
        return err;
    }
    err = sp1_hal_midi_init();
    if (err < 0) {
        return err;
    }
    return 0;
}

void sp1_hal_shutdown(void) {
    sp1_hal_audio_shutdown();
    sp1_hal_leds_all_off();
}

uint32_t sp1_hal_time_us(void) {
    return k_cyc_to_us_floor32(k_cycle_get_32());
}

uint32_t sp1_hal_time_ms(void) {
    return k_uptime_get_32();
}

#if defined(NRF_SAADC)
static bool sp1_saadc_wait_event(volatile uint32_t *evt_reg, uint32_t timeout_us) {
    for (uint32_t t = 0U; t < timeout_us; ++t) {
        if (*evt_reg != 0U) {
            return true;
        }
        k_busy_wait(1);
    }
    return false;
}

static int8_t sp1_decode_ain0_btn(int32_t mv) {
    if (mv < 80) {
        return -1;
    }
    static const struct {
        int32_t mv;
        sp1_button_id_t btn;
    } k_ain0_keys[] = {
        { 190,  SP1_BTN_TRACK_1 },
        { 359,  SP1_BTN_TRACK_2 },
        { 645,  SP1_BTN_TRACK_3 },
        { 1073, SP1_BTN_TRACK_4 },
        { 1604, SP1_BTN_PLAY },
    };
    int32_t best_diff = 100000;
    int8_t best_btn = -1;
    for (size_t i = 0U; i < (sizeof(k_ain0_keys) / sizeof(k_ain0_keys[0])); ++i) {
        const int32_t diff = (mv >= k_ain0_keys[i].mv) ? (mv - k_ain0_keys[i].mv)
                                                       : (k_ain0_keys[i].mv - mv);
        if (diff < best_diff) {
            best_diff = diff;
            best_btn = (int8_t)k_ain0_keys[i].btn;
        }
    }
    return best_btn;
}

static int8_t sp1_decode_ain1_btn(int32_t mv) {
    if (mv < 150) {
        return -1;
    }
    static const struct {
        int32_t mv;
        sp1_button_id_t btn;
    } k_ain1_keys[] = {
        { 358,  SP1_BTN_RWD },
        { 647,  SP1_BTN_VOL_DOWN },
        { 1076, SP1_BTN_FWD },
        { 1606, SP1_BTN_VOL_UP },
    };
    int32_t best_diff = 100000;
    int8_t best_btn = -1;
    for (size_t i = 0U; i < (sizeof(k_ain1_keys) / sizeof(k_ain1_keys[0])); ++i) {
        const int32_t diff = (mv >= k_ain1_keys[i].mv) ? (mv - k_ain1_keys[i].mv)
                                                       : (k_ain1_keys[i].mv - mv);
        if (diff < best_diff) {
            best_diff = diff;
            best_btn = (int8_t)k_ain1_keys[i].btn;
        }
    }
    return best_btn;
}

static void sp1_saadc_scan_6ch(void) {
    static const uint32_t k_pselp[6] = {
        SAADC_CH_PSELP_PSELP_AnalogInput0, /* 0: AIN0 tracks */
        SAADC_CH_PSELP_PSELP_AnalogInput1, /* 1: AIN1 volume */
        SAADC_CH_PSELP_PSELP_AnalogInput2, /* 2: AIN2 fader3 */
        SAADC_CH_PSELP_PSELP_AnalogInput3, /* 3: AIN3 fader1 */
        SAADC_CH_PSELP_PSELP_AnalogInput6, /* 4: AIN6 fader2 */
        SAADC_CH_PSELP_PSELP_AnalogInput7, /* 5: AIN7 fader4 */
    };
    static const uint32_t k_ch_cfg =
        (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
        (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
        (SAADC_CH_CONFIG_GAIN_Gain1_4 << SAADC_CH_CONFIG_GAIN_Pos) |
        (SAADC_CH_CONFIG_REFSEL_VDD1_4 << SAADC_CH_CONFIG_REFSEL_Pos) |
        (SAADC_CH_CONFIG_TACQ_20us << SAADC_CH_CONFIG_TACQ_Pos) |
        (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
        (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);

    int16_t buf[6] = { 0 };

    NRF_SAADC->ENABLE = 0U;
    NRF_SAADC->INTENCLR = 0xFFFFFFFFUL;
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
    NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
    NRF_SAADC->SAMPLERATE = 0U;

    for (uint32_t ch = 0U; ch < 6U; ++ch) {
        NRF_SAADC->CH[ch].PSELP = k_pselp[ch];
        NRF_SAADC->CH[ch].PSELN = SAADC_CH_PSELN_PSELN_NC;
        NRF_SAADC->CH[ch].CONFIG = k_ch_cfg;
    }
    for (uint32_t ch = 6U; ch < 8U; ++ch) {
        NRF_SAADC->CH[ch].PSELP = SAADC_CH_PSELP_PSELP_NC;
        NRF_SAADC->CH[ch].PSELN = SAADC_CH_PSELN_PSELN_NC;
    }

    NRF_SAADC->RESULT.PTR = (uint32_t)(uintptr_t)buf;
    NRF_SAADC->RESULT.MAXCNT = 6U;
    NRF_SAADC->EVENTS_STARTED = 0U;
    NRF_SAADC->EVENTS_END = 0U;
    NRF_SAADC->EVENTS_STOPPED = 0U;
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;

    NRF_SAADC->TASKS_START = 1U;
    const bool started = sp1_saadc_wait_event(&NRF_SAADC->EVENTS_STARTED, 1000U);
    bool ended = false;
    if (started) {
        NRF_SAADC->TASKS_SAMPLE = 1U;
        ended = sp1_saadc_wait_event(&NRF_SAADC->EVENTS_END, 1500U);
    }
    NRF_SAADC->TASKS_STOP = 1U;
    (void)sp1_saadc_wait_event(&NRF_SAADC->EVENTS_STOPPED, 500U);
    NRF_SAADC->ENABLE = 0U;

    if (!ended) {
        return;
    }

    /* Update 4 faders: fader0=AIN3(buf[3]), fader1=AIN6(buf[4]), fader2=AIN2(buf[2]), fader3=AIN7(buf[5]) */
    const int16_t raw_faders[SP1_NUM_FADERS] = { buf[3], buf[4], buf[2], buf[5] };
    for (uint8_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        int16_t s = raw_faders[i];
        if (s < 0) {
            s = 0;
        } else if (s > 0x0FF0) {
            s = 0x0FF0;
        }
        __atomic_store_n(&g_fader_raw_latest[i], s, __ATOMIC_RELEASE);
        sp1_hal_controls_on_fader_raw(i, s);
    }

    /* Decode AIN0 (tracks + Play) and AIN1 (RWD/Vol-/FWD/Vol+) */
    const int32_t ain0_mv = (buf[0] <= 0) ? 0 : (((int32_t)buf[0] * 3300) / 4096);
    const int32_t ain1_mv = (buf[1] <= 0) ? 0 : (((int32_t)buf[1] * 3300) / 4096);

    const int8_t cur_ain0 = sp1_decode_ain0_btn(ain0_mv);
    if (cur_ain0 != g_prev_ain0_btn) {
        if (g_prev_ain0_btn >= 0) {
            sp1_hal_controls_on_button_event((sp1_button_id_t)g_prev_ain0_btn, false);
        }
        if (cur_ain0 >= 0) {
            sp1_hal_controls_on_button_event((sp1_button_id_t)cur_ain0, true);
        }
        g_prev_ain0_btn = cur_ain0;
    }

    const int8_t cur_ain1 = sp1_decode_ain1_btn(ain1_mv);
    if (cur_ain1 != g_prev_ain1_btn) {
        if (g_prev_ain1_btn >= 0) {
            sp1_hal_controls_on_button_event((sp1_button_id_t)g_prev_ain1_btn, false);
        }
        if (cur_ain1 >= 0) {
            sp1_hal_controls_on_button_event((sp1_button_id_t)cur_ain1, true);
        }
        g_prev_ain1_btn = cur_ain1;
    }
}
#endif

void sp1_hal_service_background(void) {
#if defined(NRF_SAADC)
    sp1_saadc_scan_6ch();
#endif
    sp1_hal_midi_flush_tx();
}
