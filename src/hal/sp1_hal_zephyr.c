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
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/class/usbd_midi2.h>

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

/* ── Static RAM Allocations (Explicit Byte Budgets) ──────────────────────── */

/* 96 stereo frames * 2 channels * 4 bytes (24-bit-in-32-bit I2S word) = 768 B/block */
#define SP1_I2S_DMA_BLOCK_BYTES     (SP1_AUDIO_BLOCK_SAMPLES * sizeof(int32_t))
#define SP1_I2S_DMA_BLOCK_COUNT     8U
#define SP1_AUDIO_THREAD_STACK_SIZE 4096U
#define SP1_AUDIO_THREAD_PRIO       2

/* Static RAM: 8 * 768 B = 6144 B I2S EasyDMA TX slab */
K_MEM_SLAB_DEFINE_STATIC(g_i2s_tx_slab, SP1_I2S_DMA_BLOCK_BYTES, SP1_I2S_DMA_BLOCK_COUNT, 4);

/* Static RAM: 4096 B audio render thread stack + ~128 B k_thread control block */
K_THREAD_STACK_DEFINE(g_audio_thread_stack, SP1_AUDIO_THREAD_STACK_SIZE);
static struct k_thread g_audio_thread_data;
static bool g_audio_thread_created = false;

/* Static RAM: 384 B DSP scratch block (96 stereo int16_t frames) */
static int16_t g_audio_dsp_scratch[SP1_AUDIO_BLOCK_SAMPLES];

/* Static RAM: 2 * 524 B = 1048 B lock-free MIDI RX/TX SPSC queues */
static sp1_midi_spsc_queue_t g_midi_rx_q;
static sp1_midi_spsc_queue_t g_midi_tx_q;
static uint32_t g_midi_ready_flag = 0U;

/* Static RAM: 16 B fader filters + 40 B live controls + 84 B double-buffered bus = 140 B */
static sp1_fader_filter_t g_fader_filters[SP1_NUM_FADERS];
static sp1_controls_snapshot_t g_live_controls;
static sp1_controls_bus_t g_controls_bus;

static sp1_audio_render_cb_t g_audio_cb = NULL;
static void *g_audio_ctx = NULL;
static uint32_t g_audio_running_flag = 0U;
static bool g_audio_configured = false;

/* ── Safety Gate: Reboot Rather Than Halt on Fatal Kernel Faults ─────────── */

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    (void)reason;
    (void)esf;
    sys_reboot(SYS_REBOOT_COLD);
}

/* ── High-Priority Audio Render Thread ───────────────────────────────────── */

static void sp1_audio_thread_entry(void *p1, void *p2, void *p3) {
    (void)p1;
    (void)p2;
    (void)p3;

    while (__atomic_load_n(&g_audio_running_flag, __ATOMIC_ACQUIRE) != 0U) {
        void *dma_block = NULL;
        if (k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_MSEC(20)) != 0) {
            continue;
        }

        if (g_audio_cb != NULL) {
            g_audio_cb(g_audio_dsp_scratch, SP1_AUDIO_BLOCK_FRAMES, g_audio_ctx);
        } else {
            memset(g_audio_dsp_scratch, 0, sizeof(g_audio_dsp_scratch));
        }

        sp1_audio_pack_s16_to_i2s24_in_32(
            g_audio_dsp_scratch, (int32_t *)dma_block, SP1_AUDIO_BLOCK_FRAMES);

        if (i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES) < 0) {
            k_mem_slab_free(&g_i2s_tx_slab, dma_block);
        }
    }
}

/* ── HAL Controls & 8-Channel PWM LEDs ───────────────────────────────────── */

int sp1_hal_controls_init(void) {
    for (uint32_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        sp1_fader_filter_init(&g_fader_filters[i]);
    }
    sp1_controls_snapshot_init(&g_live_controls);
    sp1_controls_bus_init(&g_controls_bus);
    return 0;
}

void sp1_hal_controls_on_button_event(sp1_button_id_t btn, bool pressed) {
    if ((unsigned)btn >= (unsigned)SP1_BTN_COUNT) {
        return;
    }

    const uint16_t bit = SP1_BTN_BIT(btn);
    const bool was_down = (g_live_controls.buttons_down & bit) != 0U;

    if (pressed) {
        g_live_controls.buttons_down |= bit;
        if (!was_down) {
            g_live_controls.buttons_pressed |= bit;
        }
    } else {
        g_live_controls.buttons_down &= (uint16_t)(~bit);
        if (was_down) {
            g_live_controls.buttons_released |= bit;
        }
    }

    g_live_controls.seq++;
    sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
}

void sp1_hal_controls_on_fader_raw(uint8_t fader_idx, int16_t raw_sample) {
    if (fader_idx >= SP1_NUM_FADERS) {
        return;
    }

    float norm = 0.0f;
    uint8_t cc7 = 0U;
    if (sp1_fader_filter_update(&g_fader_filters[fader_idx], raw_sample, &norm, &cc7)) {
        g_live_controls.fader_raw[fader_idx] = (uint16_t)g_fader_filters[fader_idx].raw_clamped;
        g_live_controls.fader_norm[fader_idx] = norm;
        g_live_controls.fader_cc[fader_idx] = cc7;
        g_live_controls.seq++;
        sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
    }
}

void sp1_hal_controls_poll(sp1_controls_snapshot_t *out_snapshot) {
    if (out_snapshot != NULL) {
        sp1_controls_bus_read(&g_controls_bus, out_snapshot);
    }
    if (g_live_controls.buttons_pressed != 0U || g_live_controls.buttons_released != 0U) {
        g_live_controls.buttons_pressed = 0U;
        g_live_controls.buttons_released = 0U;
        sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
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
    return 0;
}

bool sp1_hal_midi_is_ready(void) {
    return __atomic_load_n(&g_midi_ready_flag, __ATOMIC_ACQUIRE) != 0U;
}

void sp1_hal_midi_set_ready(bool ready) {
    __atomic_store_n(&g_midi_ready_flag, ready ? 1U : 0U, __ATOMIC_RELEASE);
}

bool sp1_hal_midi_send(const sp1_midi_msg_t *msg) {
    return sp1_midi_queue_push(&g_midi_tx_q, msg);
}

bool sp1_hal_midi_receive(sp1_midi_msg_t *out_msg) {
    return sp1_midi_queue_pop(&g_midi_rx_q, out_msg);
}

void sp1_hal_midi_flush_tx(void) {
    if (!sp1_hal_midi_is_ready() || !device_is_ready(g_usb_midi_dev)) {
        return;
    }

    sp1_midi_msg_t msg;
    while (sp1_midi_queue_pop(&g_midi_tx_q, &msg)) {
        uint32_t word0 = 0U;
        if (sp1_midi_encode_ump32(&msg, 0U, &word0)) {
            const struct midi_ump ump = { .data = { word0 } };
            (void)usbd_midi_send(g_usb_midi_dev, ump);
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

    if (!device_is_ready(g_i2s_dev)) {
        return -ENODEV;
    }

    struct i2s_config i2s_cfg;
    memset(&i2s_cfg, 0, sizeof(i2s_cfg));
    i2s_cfg.word_size = 24U;
    i2s_cfg.channels = SP1_AUDIO_CHANNELS;
    i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
    i2s_cfg.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
    i2s_cfg.frame_clk_freq = SP1_AUDIO_SAMPLE_RATE_HZ;
    i2s_cfg.mem_slab = &g_i2s_tx_slab;
    i2s_cfg.block_size = SP1_I2S_DMA_BLOCK_BYTES;
    i2s_cfg.timeout = 200;

    int err = i2s_configure(g_i2s_dev, I2S_DIR_TX, &i2s_cfg);
    if (err < 0) {
        return err;
    }

    struct audio_codec_cfg codec_cfg;
    memset(&codec_cfg, 0, sizeof(codec_cfg));
    codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
    codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
    codec_cfg.dai_cfg.i2s = i2s_cfg;

    if (device_is_ready(g_cs42l42_dev)) {
        err = audio_codec_configure(g_cs42l42_dev, &codec_cfg);
        if (err < 0) {
            return err;
        }
    }

    /* TAS2505 may NACK when full SWD probe is attached; treat failure as non-fatal. */
    if (device_is_ready(g_tas2505_dev)) {
        (void)audio_codec_configure(g_tas2505_dev, &codec_cfg);
    }

    g_audio_configured = true;
    return 0;
}

int sp1_hal_audio_start(void) {
    if (!g_audio_configured) {
        return -EINVAL;
    }

    __atomic_store_n(&g_audio_running_flag, 1U, __ATOMIC_RELEASE);

    /* Prime two silent DMA blocks before starting I2S0 EasyDMA. */
    for (uint32_t i = 0U; i < 2U; ++i) {
        void *dma_block = NULL;
        if (k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_NO_WAIT) == 0) {
            memset(dma_block, 0, SP1_I2S_DMA_BLOCK_BYTES);
            if (i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES) < 0) {
                k_mem_slab_free(&g_i2s_tx_slab, dma_block);
            }
        }
    }

    const int err = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (err < 0) {
        __atomic_store_n(&g_audio_running_flag, 0U, __ATOMIC_RELEASE);
        return err;
    }

    if (device_is_ready(g_cs42l42_dev)) {
        audio_codec_start_output(g_cs42l42_dev);
    }
    if (device_is_ready(g_tas2505_dev)) {
        audio_codec_start_output(g_tas2505_dev);
    }

    if (!g_audio_thread_created) {
        (void)k_thread_create(&g_audio_thread_data,
                              g_audio_thread_stack,
                              K_THREAD_STACK_SIZEOF(g_audio_thread_stack),
                              sp1_audio_thread_entry,
                              NULL, NULL, NULL,
                              SP1_AUDIO_THREAD_PRIO,
                              0U,
                              K_NO_WAIT);
        g_audio_thread_created = true;
    }

    return 0;
}

int sp1_hal_audio_stop(void) {
    __atomic_store_n(&g_audio_running_flag, 0U, __ATOMIC_RELEASE);

    if (g_audio_configured && device_is_ready(g_i2s_dev)) {
        (void)i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
    }
    if (device_is_ready(g_tas2505_dev)) {
        audio_codec_stop_output(g_tas2505_dev);
    }
    if (device_is_ready(g_cs42l42_dev)) {
        audio_codec_stop_output(g_cs42l42_dev);
    }
    return 0;
}

int sp1_hal_audio_submit_block(const int16_t *interleaved_stereo, uint32_t num_frames) {
    if (!g_audio_configured || interleaved_stereo == NULL ||
        num_frames != SP1_AUDIO_BLOCK_FRAMES) {
        return -EINVAL;
    }

    void *dma_block = NULL;
    int err = k_mem_slab_alloc(&g_i2s_tx_slab, &dma_block, K_NO_WAIT);
    if (err < 0) {
        return err;
    }

    sp1_audio_pack_s16_to_i2s24_in_32(
        interleaved_stereo, (int32_t *)dma_block, SP1_AUDIO_BLOCK_FRAMES);

    err = i2s_write(g_i2s_dev, dma_block, SP1_I2S_DMA_BLOCK_BYTES);
    if (err < 0) {
        k_mem_slab_free(&g_i2s_tx_slab, dma_block);
    }
    return err;
}

void sp1_hal_audio_set_volume(uint8_t volume_0_to_127) {
    const uint8_t clamped = (volume_0_to_127 > 127U) ? 127U : volume_0_to_127;
    audio_property_value_t val;
    val.vol = (int)clamped;

    if (device_is_ready(g_cs42l42_dev)) {
        (void)audio_codec_set_property(
            g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
    }
    if (device_is_ready(g_tas2505_dev)) {
        (void)audio_codec_set_property(
            g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
    }
}

void sp1_hal_audio_set_mute(bool muted) {
    audio_property_value_t val;
    val.mute = muted;

    if (device_is_ready(g_cs42l42_dev)) {
        (void)audio_codec_set_property(
            g_cs42l42_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
    }
    if (device_is_ready(g_tas2505_dev)) {
        (void)audio_codec_set_property(
            g_tas2505_dev, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, val);
    }
}

void sp1_hal_audio_shutdown(void) {
    (void)sp1_hal_audio_stop();
    sp1_hal_audio_set_mute(true);

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

void sp1_hal_service_background(void) {
    sp1_hal_midi_flush_tx();
}
