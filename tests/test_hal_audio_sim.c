#include "test_harness.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8U));
}

static uint32_t read_le32(const uint8_t *src) {
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8U) |
           ((uint32_t)src[2] << 16U) |
           ((uint32_t)src[3] << 24U);
}

static void test_i2s24_in_32_packing(void) {
    const int16_t src[6] = { 0, 32767, -32768, 1, -1, 16384 };
    int32_t dst[6] = { 0, 0, 0, 0, 0, 0 };

    sp1_audio_pack_s16_to_i2s24_in_32(src, dst, 3U);

    TEST_ASSERT_EQ_U32(0x00000000U, (uint32_t)dst[0]);
    TEST_ASSERT_EQ_U32(0x007FFF00U, (uint32_t)dst[1]);
    TEST_ASSERT_EQ_U32(0xFF800000U, (uint32_t)dst[2]);
    TEST_ASSERT_EQ_U32(0x00000100U, (uint32_t)dst[3]);
    TEST_ASSERT_EQ_U32(0xFFFFFF00U, (uint32_t)dst[4]);
    TEST_ASSERT_EQ_U32(0x00400000U, (uint32_t)dst[5]);
}

static void test_host_wav_and_midi_render(void) {
    const char *wav_path = "test_audio_sim_out.wav";
    const char *csv_path = "test_audio_sim_midi.csv";

    const sp1_hal_host_config_t cfg = {
        .wav_path = wav_path,
        .midi_csv_path = csv_path,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());

    /* Render 1.0 second (48,000 stereo frames = 500 blocks of 96 frames) of 440 Hz tone
     * with 120 BPM 24 PPQN MIDI clock (48 clock ticks/sec) + quarter-note notes. */
    const uint32_t total_frames = SP1_AUDIO_SAMPLE_RATE_HZ;
    const uint32_t phase_inc = (uint32_t)(440.0f * 89478.485333f);
    uint32_t phase = 0U;

    const uint64_t clock_num = 12000ULL * (uint64_t)SP1_MIDI_PPQN;
    const uint64_t clock_den = 60ULL * 100ULL * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ;
    uint64_t clock_acc = clock_den;
    uint32_t tick_idx = 0U;

    const sp1_midi_msg_t start_msg = sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, 0U);
    TEST_ASSERT(sp1_hal_midi_send(&start_msg));

    int16_t block[SP1_AUDIO_BLOCK_SAMPLES];
    for (uint32_t frame = 0U; frame < total_frames; frame += SP1_AUDIO_BLOCK_FRAMES) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t cur_frame = frame + i;
            if (clock_acc >= clock_den) {
                clock_acc -= clock_den;
                const uint32_t ts_us =
                    (uint32_t)(((uint64_t)cur_frame * 1000000ULL) / SP1_AUDIO_SAMPLE_RATE_HZ);
                const sp1_midi_msg_t clk = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, ts_us);
                TEST_ASSERT(sp1_hal_midi_send(&clk));

                if ((tick_idx % SP1_MIDI_PPQN) == 0U) {
                    const sp1_midi_msg_t on = sp1_midi_msg_note_on(0U, 60U, 100U, ts_us);
                    TEST_ASSERT(sp1_hal_midi_send(&on));
                } else if ((tick_idx % SP1_MIDI_PPQN) == 12U) {
                    const sp1_midi_msg_t off = sp1_midi_msg_note_off(0U, 60U, 0U, ts_us);
                    TEST_ASSERT(sp1_hal_midi_send(&off));
                }
                tick_idx++;
            }
            clock_acc += clock_num;

            const float radians =
                6.28318530718f * ((float)(phase >> 16U) * (1.0f / 65536.0f));
            const int16_t s = (int16_t)(sinf(radians) * 16000.0f);
            block[i * 2U] = s;
            block[(i * 2U) + 1U] = s;
            phase += phase_inc;
        }
        TEST_ASSERT_EQ_INT(0, sp1_hal_audio_submit_block(block, SP1_AUDIO_BLOCK_FRAMES));
        sp1_hal_service_background();
    }

    const sp1_midi_msg_t stop_msg = sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, 1000000U);
    TEST_ASSERT(sp1_hal_midi_send(&stop_msg));
    sp1_hal_shutdown();

    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);
    TEST_ASSERT_EQ_U32(total_frames, (uint32_t)stats.total_frames_written);
    TEST_ASSERT_EQ_U32(1U, stats.midi_tx_start_count);
    TEST_ASSERT_EQ_U32(48U, stats.midi_tx_clock_count);
    TEST_ASSERT_EQ_U32(2U, stats.midi_tx_note_on_count);
    TEST_ASSERT_EQ_U32(2U, stats.midi_tx_note_off_count);
    TEST_ASSERT_EQ_U32(1U, stats.midi_tx_stop_count);
    TEST_ASSERT_EQ_U32(0U, stats.midi_tx_dropped);

    /* Validate generated 44-byte RIFF/WAVE header and stereo 440 Hz PCM content. */
    FILE *fp = fopen(wav_path, "rb");
    TEST_ASSERT(fp != NULL);

    uint8_t hdr[44];
    TEST_ASSERT_EQ_INT(44, (int)fread(hdr, 1U, sizeof(hdr), fp));
    TEST_ASSERT(memcmp(&hdr[0], "RIFF", 4U) == 0);
    TEST_ASSERT(memcmp(&hdr[8], "WAVE", 4U) == 0);
    TEST_ASSERT(memcmp(&hdr[12], "fmt ", 4U) == 0);
    TEST_ASSERT(memcmp(&hdr[36], "data", 4U) == 0);

    const uint32_t expected_data_bytes = total_frames * SP1_AUDIO_CHANNELS * 2U;
    TEST_ASSERT_EQ_U32(36U + expected_data_bytes, read_le32(&hdr[4]));
    TEST_ASSERT_EQ_U32(16U, read_le32(&hdr[16]));
    TEST_ASSERT_EQ_U32(1U, read_le16(&hdr[20]));
    TEST_ASSERT_EQ_U32(2U, read_le16(&hdr[22]));
    TEST_ASSERT_EQ_U32(48000U, read_le32(&hdr[24]));
    TEST_ASSERT_EQ_U32(192000U, read_le32(&hdr[28]));
    TEST_ASSERT_EQ_U32(4U, read_le16(&hdr[32]));
    TEST_ASSERT_EQ_U32(16U, read_le16(&hdr[34]));
    TEST_ASSERT_EQ_U32(expected_data_bytes, read_le32(&hdr[40]));

    int16_t prev_l = 0;
    int32_t max_abs = 0;
    uint32_t rising_zero_crossings = 0U;

    uint8_t frame_bytes[4];
    for (uint32_t f = 0U; f < total_frames; ++f) {
        TEST_ASSERT_EQ_INT(4, (int)fread(frame_bytes, 1U, 4U, fp));
        const int16_t left = (int16_t)read_le16(&frame_bytes[0]);
        const int16_t right = (int16_t)read_le16(&frame_bytes[2]);
        TEST_ASSERT_EQ_INT(left, right);

        const int32_t abs_l = (left < 0) ? -(int32_t)left : (int32_t)left;
        if (abs_l > max_abs) {
            max_abs = abs_l;
        }
        if (f > 0U && prev_l <= 0 && left > 0) {
            rising_zero_crossings++;
        }
        prev_l = left;
    }
    fclose(fp);

    TEST_ASSERT(max_abs >= 15900 && max_abs <= 16100);
    /* 1.0 s of 440 Hz tone should have ~440 rising zero crossings. */
    TEST_ASSERT(rising_zero_crossings >= 439U && rising_zero_crossings <= 441U);

    /* Validate CSV file was written and contains header + 54 MIDI event lines. */
    FILE *csv = fopen(csv_path, "rb");
    TEST_ASSERT(csv != NULL);
    uint32_t line_count = 0U;
    char line_buf[128];
    while (fgets(line_buf, (int)sizeof(line_buf), csv) != NULL) {
        line_count++;
    }
    fclose(csv);
    TEST_ASSERT_EQ_U32(55U, line_count); /* 1 header + 54 MIDI events */

    (void)remove(wav_path);
    (void)remove(csv_path);
}

int main(void) {
    RUN_TEST(test_i2s24_in_32_packing);
    RUN_TEST(test_host_wav_and_midi_render);
    return sp1_test_summary();
}
