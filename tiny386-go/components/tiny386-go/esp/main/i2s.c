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
#ifdef RETRO_GO
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
        int64_t _a1 = rg_system_timer();

        mixer_callback(globals.pc, (uint8_t *)audio_buf, MIXER_BUF_LEN * 2);
        int64_t _a2 = rg_system_timer();

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
        int64_t _a3 = rg_system_timer();

        /* Submit to retro-go audio system */
        rg_audio_submit((void *)audio_buf, MIXER_BUF_LEN);
        int64_t _a4 = rg_system_timer();

        #ifdef ESPPROFILE
        /* Profile: mix vs vol vs submit */
        {
            static int64_t t_mix = 0, t_vol = 0, t_sub = 0;
            static int asub = 0, acnt = 0;
            t_mix += (_a2 - _a1);
            t_vol += (_a3 - _a2);
            t_sub += (_a4 - _a3);
            asub++; acnt++;
            if (acnt >= 128) {
                fprintf(stderr, "AUDIO: mix=%lu vol=%lu submit=%lu us (avg %d subs)\n",
                    (unsigned long)(t_mix / asub),
                    (unsigned long)(t_vol / asub),
                    (unsigned long)(t_sub / asub),
                    asub);
                t_mix = 0; t_vol = 0; t_sub = 0;
                asub = 0; acnt = 0;
            }
        }
        #endif
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

#else
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "common.h"

static i2s_chan_handle_t                tx_chan;        // I2S tx channel handler
void mixer_callback (void *opaque, uint8_t *stream, int free);

/* 全局音量 (0-100) */
static int s_volume = 50;

#ifndef MIXER_BUF_LEN
#define MIXER_BUF_LEN 128
#endif

void volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

int volume_get(void)
{
    return s_volume;
}

/* 限幅函数：将 sample 限幅到 [-32768, 32767] */
static inline int16_t clamp_sample(int x)
{
	if (x > 32767) return 32767;
	if (x < -32768) return -32768;
	return (int16_t)x;
}

static void i2s_task(void *arg)
{
	int16_t buf[MIXER_BUF_LEN];
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "i2s runs on core %d\n", core_id);

	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	i2s_channel_enable(tx_chan);
	for (;;) {
		size_t bwritten;
		memset(buf, 0, MIXER_BUF_LEN * 2);
		mixer_callback(globals.pc, (uint8_t *) buf, MIXER_BUF_LEN * 2);
		/* 应用音量缩放（无固定衰减，用限幅代替）
		 * vol=100 → 无衰减，直接输出
		 * vol=50  → 衰减一半
		 * vol=0   → 静音 */
		int vol = s_volume;
		if (vol == 100) {
			/* 满音量：直接限幅即可（mixer_callback 中三路混音可能溢出） */
			for (int i = 0; i < MIXER_BUF_LEN; i++) {
				buf[i] = clamp_sample(buf[i]);
			}
		} else if (vol > 0) {
			for (int i = 0; i < MIXER_BUF_LEN; i++) {
				buf[i] = clamp_sample(buf[i] * vol / 100);
			}
		} else {
			memset(buf, 0, MIXER_BUF_LEN * 2);
		}
		i2s_channel_write(tx_chan, buf, MIXER_BUF_LEN * 2, &bwritten, portMAX_DELAY);
	}
	i2s_channel_disable(tx_chan);
}

void i2s_main()
{
#ifdef I2S_MCLK
	/* Setp 1: Determine the I2S channel configuration and allocate two channels one by one
	 * The default configuration can be generated by the helper macro,
	 * it only requires the I2S controller id and I2S role
	 * The tx and rx channels here are registered on different I2S controller,
	 * Except ESP32 and ESP32-S2, others allow to register two separate tx & rx channels on a same controller */
	i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));
	/* Step 2: Setting the configurations of standard mode and initialize each channels one by one
	 * The slot configuration and clock configuration can be generated by the macros
	 * These two helper macros is defined in 'i2s_std.h' which can only be used in STD mode.
	 * They can help to specify the slot and clock configurations for initialization or re-configuring */
	i2s_std_config_t tx_std_cfg = {
		.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = I2S_MCLK,
			.bclk = I2S_BCLK,
			.ws   = I2S_WS,
			.dout = I2S_DOUT,
			.din  = -1,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv   = false,
			},
		},
	};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
	xTaskCreatePinnedToCore(i2s_task, "i2s_task", 4096, NULL, 0, NULL, 0);
#endif
}
#endif