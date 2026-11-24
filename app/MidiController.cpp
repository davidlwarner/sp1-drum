#include "MidiController.hpp"
#include "hal/sp1_hal.h"
#include "system/ResetBreadcrumbs.hpp"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <algorithm>
#include <cmath>

LOG_MODULE_REGISTER(midi_ctrl, LOG_LEVEL_INF);

namespace midi {

void MidiController::send_cc(uint8_t cc, uint8_t value) {
    if (!ready.load(std::memory_order_relaxed) || midi_dev == nullptr) {
        return;
    }
    const int64_t now_ms = k_uptime_get();
    if (now_ms < tx_backoff_until_ms_) {
        return;
    }
    const struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
        static_cast<uint32_t>(kMidiGroup), UMP_MIDI_CONTROL_CHANGE, kMidiChannel, cc, value);
    if (usbd_midi_send(midi_dev, ump) < 0) {
        tx_backoff_until_ms_ = now_ms + 1000;
    }
}

void MidiController::send_note(uint8_t note, bool on, uint8_t velocity) {
    if (!ready.load(std::memory_order_relaxed) || midi_dev == nullptr) {
        return;
    }
    const int64_t now_ms = k_uptime_get();
    if (now_ms < tx_backoff_until_ms_) {
        return;
    }
    const uint8_t cmd = on ? UMP_MIDI_NOTE_ON : UMP_MIDI_NOTE_OFF;
    const struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
        static_cast<uint32_t>(kMidiGroup), cmd, kMidiChannel, note, on ? velocity : 0U);
    if (usbd_midi_send(midi_dev, ump) < 0) {
        tx_backoff_until_ms_ = now_ms + 1000;
    }
}

void MidiController::send_mmc(uint8_t command) {
    if (!ready.load(std::memory_order_relaxed) || midi_dev == nullptr) {
        return;
    }
    const int64_t now_ms = k_uptime_get();
    if (now_ms < tx_backoff_until_ms_) {
        return;
    }

    // MMC SysEx: F0 7F 7F 06 <cmd> F7
    // In UMP MT=3 (Data Message 64-bit):
    //   Word 0: [MT=3][Group][Status=0(complete)][NumBytes=4][0x7F][0x7F]
    //   Word 1: [0x06][cmd][0x00][0x00]
    const struct midi_ump ump = {.data = {
        (UMP_MT_DATA_64 << 28)
        | (static_cast<uint32_t>(kMidiGroup) << 24)
        | (0x0U << 20)           // status = complete SysEx
        | (0x4U << 16)           // 4 payload bytes
        | (0x7FU << 8)           // byte 0: universal real-time
        | 0x7FU,                 // byte 1: all devices
        (0x06U << 24)            // byte 2: MMC command
        | (static_cast<uint32_t>(command) << 16) // byte 3: sub-command
    }};
    if (usbd_midi_send(midi_dev, ump) < 0) {
        tx_backoff_until_ms_ = now_ms + 1000;
    }
}

void MidiController::poll_faders() {
    int16_t buf[kNumFaders] = {};
    uint32_t channels_mask = 0U;
    for (int i = 0; i < kNumFaders; ++i) {
        channels_mask |= BIT(fader_adc[i].channel_id);
    }

    struct adc_sequence seq = {};
    (void)adc_sequence_init_dt(&fader_adc[0], &seq);
    seq.channels = channels_mask;
    seq.buffer = buf;
    seq.buffer_size = sizeof(buf);

    core::reset_breadcrumbs::mark_control_phase(0x21U);
    const int ret = adc_read(fader_adc[0].dev, &seq);
    if (ret < 0) {
        if (poll_count_ < 3) {
            LOG_WRN("Fader batch ADC read failed: %d", ret);
        }
        ++poll_count_;
        return;
    }

    /* Map SAADC hardware-channel-sorted buf[] back to fader indices 0..3. */
    for (int i = 0; i < kNumFaders; ++i) {
        int buf_idx = 0;
        for (int j = 0; j < kNumFaders; ++j) {
            if (fader_adc[j].channel_id < fader_adc[i].channel_id) {
                ++buf_idx;
            }
        }
        int16_t sample = static_cast<int16_t>(std::clamp<int>(buf[buf_idx], 0, 0x0FF0));
        sp1_hal_controls_on_fader_raw(static_cast<uint8_t>(i), sample);

        if (poll_count_ < 3) {
            LOG_INF("Fader %d: raw=%d cc=%d", i, sample, sample >> 5);
        }

        if (std::abs(sample - last_fader_raw[i]) < kFaderDeadband) {
            continue;
        }

        last_fader_raw[i] = sample;

        const uint8_t cc_val = static_cast<uint8_t>(
            std::clamp<int>(sample >> 5, 0, 127));

        if (cc_val != last_fader_cc[i]) {
            last_fader_cc[i] = cc_val;
            core::reset_breadcrumbs::mark_control_phase(0x28U);
            send_cc(kCcFader1 + static_cast<uint8_t>(i), cc_val);
        }
    }
    ++poll_count_;
}

void MidiController::set_track_led(int index, uint8_t brightness) {
    if (track_led_pwm == nullptr || index < 0 || index >= kNumTrackLeds) {
        return;
    }
    const struct pwm_dt_spec* spec = &track_led_pwm[index];
    const uint32_t pulse = (spec->period * brightness) / 127U;
    pwm_set_pulse_dt(spec, pulse);
}

void MidiController::on_rx(const struct midi_ump& ump) {
    if (UMP_MT(ump) != UMP_MT_MIDI1_CHANNEL_VOICE) {
        return;
    }

    const uint8_t cmd = UMP_MIDI_COMMAND(ump);
    const uint8_t note = UMP_MIDI1_P1(ump);
    const uint8_t vel = UMP_MIDI1_P2(ump);

    if (note >= kNoteTrack1 && note < kNoteTrack1 + kNumTrackLeds) {
        const int led = note - kNoteTrack1;
        if (cmd == UMP_MIDI_NOTE_ON) {
            set_track_led(led, vel);
        } else if (cmd == UMP_MIDI_NOTE_OFF) {
            set_track_led(led, 0);
        }
    }
}

}  // namespace midi
