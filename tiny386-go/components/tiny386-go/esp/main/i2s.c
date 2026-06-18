/*
 * Audio output for tiny386 - DISABLED when RETRO_GO is defined.
 *
 * Retro-Go provides its own audio system via rg_audio. Including this
 * file alongside retro-go's I2S driver will cause a driver conflict:
 *
 *   "CONFLICT! The new i2s driver can't work along with the legacy i2s driver"
 *
 * To avoid this, the entire I2S initialization and audio task are
 * disabled when RETRO_GO is defined, and volume_* stubs are provided.
 */
#include "esp_log.h"
#include "common.h"
#include <rg_system.h>
#include <rg_audio.h>

extern struct Globals globals;
static const char *TAG = "i2s";
static int s_volume = 50;

/* Forward declaration of tiny386's mixer callback */
void mixer_callback(void *opaque, uint8_t *stream, int free);

void volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "Volume: %d%%", s_volume);
}

int volume_get(void)
{
    return s_volume;
}

/* Static audio buffer for submissions */
static int16_t audio_buf[MIXER_BUF_LEN * 2];
static int64_t audio_clock = 0;   /* virtual audio time, advances by period_us per buffer */

void audio_submit_frame(void)
{
    if (!globals.pc)
        return;

    int64_t now = rg_system_timer();
    int64_t period_us = (int64_t)MIXER_BUF_LEN * 1000000LL / 44100LL;

    /* Initialize audio clock on first call */
    if (audio_clock == 0) {
        audio_clock = now - period_us;  /* allow immediate first submit */
    }

    /*
     * Cumulative catch-up: when the main loop runs slower than real-time
     * audio, we submit enough buffers to catch the virtual audio clock
     * up to wall-clock time.  This prevents buffer underruns even when
     * pc_step() takes longer than one audio buffer duration.
     *
     * Cap at MAX_CATCHUP to avoid spinning forever if the emulator
     * stalls or runs extremely slowly.
     */
#define AUDIO_MAX_CATCHUP 4
    int submitted = 0;

    while (audio_clock + period_us <= now && submitted < AUDIO_MAX_CATCHUP) {
        audio_clock += period_us;
        submitted++;

        /* Fill buffer with mixed audio (Adlib + SB16 + PC speaker) */
        memset(audio_buf, 0, sizeof(audio_buf));
        mixer_callback(globals.pc, (uint8_t *)audio_buf, MIXER_BUF_LEN * 2);

        /* Apply volume */
        int vol = s_volume;
        if (vol < 100) {
            for (int i = 0; i < MIXER_BUF_LEN * 2; i++) {
                int sample = audio_buf[i] * vol / 100;
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                audio_buf[i] = (int16_t)sample;
            }
        }

        /* Submit to retro-go audio system */
        rg_audio_submit((void *)audio_buf, MIXER_BUF_LEN);
    }

    /* If we fell too far behind (emulator stall), reset the clock
     * so we don't spam submissions when it recovers. */
    if (submitted >= AUDIO_MAX_CATCHUP) {
        audio_clock = now;
    }
}

void i2s_main(void)
{
    ESP_LOGI(TAG, "Audio ready (inline rg_audio integration)");
    ESP_LOGI(TAG, "rg_audio_get_driver() = '%s'", rg_audio_get_driver());
    audio_clock = 0;
}
