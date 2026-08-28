/*
 * rg_mic.c - PDM MEMS microphone support + interactive test screen
 *
 * Supports ESP32-S3 and ESP32-P4 from the same source; everything that differs
 * between the two chips is selected by the macro block below (RG_MIC_* macros).
 *
 * Target hardware: ST IMP34DT05TR (PDM output, normal mode clock 1.2 - 3.25 MHz).
 * Wiring, taken from the target's config.h:
 *     ESP32-S3-DEVKIT: CLK=GPIO40  DOUT=GPIO41
 *     ESP32-P4-DEVKIT: CLK=GPIO53  DOUT=GPIO54
 *     mic CLK  <- RG_GPIO_MIC_CLK    (ESP32 outputs the clock here)
 *     mic DOUT -> RG_GPIO_MIC_DATA
 *     mic LR   -> GND (slot0) or VDD (slot1). Must NOT be left floating!
 *
 * Implementation notes (esp-idf 5.5.4, checked against components/driver/deprecated/
 * i2s_legacy.c, components/hal/i2s_hal.c and components/esp_driver_i2s/i2s_pdm.c):
 *  - PDM RX is only available on I2S_NUM_0 (i2s_legacy.c: "I2S PDM mode only support
 *    on I2S0"), which rg_audio also uses => in PDM mode we borrow the audio device
 *    and give it back. RAW mode uses plain I2S RX, so it runs on a spare port and
 *    leaves audio playback alive (live monitoring works there).
 *  - In PDM mode the legacy driver fills the *PDM TX* part of the slot config union,
 *    which leaves pdm_rx.slot_mask = 0. i2s_hal_pdm_set_rx_slot() only rescues that
 *    when slot_mode == STEREO (it then forces slot_mask = I2S_PDM_SLOT_BOTH), so we
 *    MUST use I2S_CHANNEL_FMT_RIGHT_LEFT and de-interleave in software. With
 *    I2S_CHANNEL_FMT_ONLY_LEFT the active channel mask stays 0 => only zeros.
 *    This applies to both S3 and P4 (both have SOC_I2S_PDM_MAX_RX_LINES = 4).
 *  - The PDM clock is emitted on the WS pin (i2s_set_pin -> m_rx_ws_sig, same signal
 *    the new driver uses for i2s_pdm_rx_gpio_config_t::clk).
 *  - i2s_set_pdm_rx_down_sample() must be called after install: it sets the
 *    downsampling mode *and* recomputes the clock (bclk = rate * 64 * (dsr16?2:1)).
 *
 * ESP32-P4 specifics (SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER=1, 3 i2s ports):
 *  - The pdm_tx/pdm_rx union aliasing lands on: hp_en=true, hp_cut_off=1 (clamped to
 *    the closest table entry = 23.3Hz), amplify_num=1. Verified against
 *    s_i2s_hal_get_cut_off_coef() in hal/i2s_hal.c: no assert, amplify is 1 (not 0,
 *    which would mute the channel). Consequence: on P4 the DC offset is removed in
 *    hardware, so a dead data line reads 0 instead of a large constant value.
 *  - GPIO0..54 are all valid on P4 (SOC_GPIO_VALID_GPIO_MASK), so 53/54 are fine.
 *
 * ESP32-S3 specifics (no PDM RX high pass filter, 2 i2s ports):
 *  - The PCM output keeps its DC component, so a dead/stuck data line shows up as a
 *    large constant value instead of zeros. DC is removed in software by rg_mic_stats.
 */

#include "rg_system.h"
#include "rg_mic.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifdef RG_MIC_ENABLED

#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <soc/soc_caps.h>

/* ========================================================================== */
/* Per target configuration (macro controlled)                                */
/* ========================================================================== */

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define RG_MIC_CHIP_NAME "P4"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define RG_MIC_CHIP_NAME "S3"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define RG_MIC_CHIP_NAME "ESP32"
#else
#define RG_MIC_CHIP_NAME "generic"
#endif

/* Hardware high-pass filter in the PDM RX path: ESP32-P4 has it, ESP32-S3 does not */
#if defined(SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER) && SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER
#define RG_MIC_HW_HPF 1
#else
#define RG_MIC_HW_HPF 0
#endif

/* PDM RX is hardwired to I2S0 on every current chip, do not change this */
#ifndef RG_MIC_PDM_I2S_PORT
#define RG_MIC_PDM_I2S_PORT I2S_NUM_0
#endif

/* RAW bit capture is plain I2S RX, so it can use a spare port and let the audio
 * driver keep I2S0 (S3 has 2 ports, P4 has 3). */
#ifndef RG_MIC_RAW_I2S_PORT
#if defined(SOC_I2S_NUM) && (SOC_I2S_NUM > 1)
#define RG_MIC_RAW_I2S_PORT I2S_NUM_1
#else
#define RG_MIC_RAW_I2S_PORT I2S_NUM_0
#endif
#endif

/* rg_audio (drivers/audio/i2s.c) always uses I2S_NUM_0 */
#define RG_MIC_AUDIO_I2S_PORT I2S_NUM_0

/* Noise floor used to decide "no signal": lower when the chip filters DC itself */
#ifndef RG_MIC_SILENCE_FLOOR
#if RG_MIC_HW_HPF
#define RG_MIC_SILENCE_FLOOR 25
#else
#define RG_MIC_SILENCE_FLOOR 40
#endif
#endif

/* Defaults, overridable per target in config.h */
#ifndef RG_MIC_DEFAULT_RATE
#define RG_MIC_DEFAULT_RATE 16000
#endif
#ifndef RG_MIC_DEFAULT_DOWNSAMPLE
#define RG_MIC_DEFAULT_DOWNSAMPLE 128
#endif
#ifndef RG_MIC_DEFAULT_SLOT
#define RG_MIC_DEFAULT_SLOT 0
#endif

#define MIC_DMA_DESC_NUM 4
#define MIC_DMA_FRAME_NUM 256

static struct
{
    rg_mic_config_t config;
    const char *last_error;
    bool initialized;
    bool audio_borrowed;
    int audio_rate;
    int clock_hz;
    int port;
} mic = {
    .config = {RG_MIC_DEFAULT_RATE, RG_MIC_DEFAULT_DOWNSAMPLE, RG_MIC_MODE_PDM},
    .port = RG_MIC_PDM_I2S_PORT,
};

static int mic_port_for_mode(int mode)
{
    return (mode == RG_MIC_MODE_RAW) ? RG_MIC_RAW_I2S_PORT : RG_MIC_PDM_I2S_PORT;
}

static void mic_uninstall(void)
{
    if (!mic.initialized)
        return;
    i2s_stop((i2s_port_t)mic.port);
    i2s_driver_uninstall((i2s_port_t)mic.port);
    gpio_reset_pin(RG_GPIO_MIC_CLK);
    gpio_reset_pin(RG_GPIO_MIC_DATA);
    mic.initialized = false;
    mic.clock_hz = 0;
}

static void mic_borrow_audio(void)
{
    if (mic.audio_borrowed)
        return;
    mic.audio_rate = rg_audio_get_sample_rate();
    mic.audio_borrowed = (rg_audio_get_sink() != NULL);
    if (mic.audio_borrowed)
    {
        rg_audio_deinit();
        RG_LOGI("mic: audio device released (shared I2S%d)", (int)RG_MIC_AUDIO_I2S_PORT);
    }
}

static void mic_return_audio(void)
{
    if (!mic.audio_borrowed)
        return;
    mic.audio_borrowed = false;
    rg_audio_init(mic.audio_rate > 0 ? mic.audio_rate : 32000);
    RG_LOGI("mic: audio device restored");
}

bool rg_mic_init(const rg_mic_config_t *config)
{
    mic_uninstall();

    if (config)
        mic.config = *config;
    if (mic.config.sample_rate < 4000 || mic.config.sample_rate > 96000)
        mic.config.sample_rate = RG_MIC_DEFAULT_RATE;
    if (mic.config.downsample != 64 && mic.config.downsample != 128)
        mic.config.downsample = RG_MIC_DEFAULT_DOWNSAMPLE;
    mic.last_error = NULL;
    mic.port = mic_port_for_mode(mic.config.mode);

    // Only the PDM peripheral collides with the audio output
    if (mic.port == RG_MIC_AUDIO_I2S_PORT)
        mic_borrow_audio();
    else
        mic_return_audio();

    const int target_clock = mic.config.sample_rate * mic.config.downsample;
    const i2s_port_t port = (i2s_port_t)mic.port;
    esp_err_t ret;

    if (mic.config.mode == RG_MIC_MODE_RAW)
    {
        /* Plain I2S RX: bclk = rate * slots * bits = rate * 32. Every bit received is
           one PDM bit sampled from DOUT, the mic just sees BCK as its clock. */
        int std_rate = target_clock / 32;
        i2s_config_t i2s_cfg = {
            .mode = I2S_MODE_MASTER | I2S_MODE_RX,
            .sample_rate = std_rate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_desc_num = MIC_DMA_DESC_NUM,
            .dma_frame_num = MIC_DMA_FRAME_NUM,
            .use_apll = false,
            .tx_desc_auto_clear = false,
            .fixed_mclk = 0,
        };
        ret = i2s_driver_install(port, &i2s_cfg, 0, NULL);
        if (ret != ESP_OK)
        {
            mic.last_error = esp_err_to_name(ret);
            RG_LOGE("mic: raw i2s_driver_install failed: %s", mic.last_error);
            return false;
        }
        // The mic clock is BCK here, WS is left unconnected
        ret = i2s_set_pin(port, &(i2s_pin_config_t){
                                    .mck_io_num = I2S_PIN_NO_CHANGE,
                                    .bck_io_num = RG_GPIO_MIC_CLK,
                                    .ws_io_num = I2S_PIN_NO_CHANGE,
                                    .data_out_num = I2S_PIN_NO_CHANGE,
                                    .data_in_num = RG_GPIO_MIC_DATA,
                                });
        if (ret != ESP_OK)
        {
            mic.last_error = esp_err_to_name(ret);
            RG_LOGE("mic: raw i2s_set_pin failed: %s", mic.last_error);
            i2s_driver_uninstall(port);
            return false;
        }
        mic.clock_hz = std_rate * 32;
        i2s_start(port);
        mic.initialized = true;
        RG_LOGI("mic[%s]: RAW bitstream capture on I2S%d, bck=GPIO%d din=GPIO%d, clock %dHz",
                RG_MIC_CHIP_NAME, mic.port, (int)RG_GPIO_MIC_CLK, (int)RG_GPIO_MIC_DATA, mic.clock_hz);
    }
    else
    {
        i2s_config_t i2s_cfg = {
            .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM,
            .sample_rate = mic.config.sample_rate,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            // Must be stereo, see the file header comment!
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_desc_num = MIC_DMA_DESC_NUM,
            .dma_frame_num = MIC_DMA_FRAME_NUM,
            .use_apll = false,
            .tx_desc_auto_clear = false,
            .fixed_mclk = 0,
        };
        ret = i2s_driver_install(port, &i2s_cfg, 0, NULL);
        if (ret != ESP_OK)
        {
            mic.last_error = esp_err_to_name(ret);
            RG_LOGE("mic: i2s_driver_install failed: %s", mic.last_error);
            return false;
        }
        // In PDM RX master mode the clock is generated on the WS pin
        ret = i2s_set_pin(port, &(i2s_pin_config_t){
                                    .mck_io_num = I2S_PIN_NO_CHANGE,
                                    .bck_io_num = I2S_PIN_NO_CHANGE,
                                    .ws_io_num = RG_GPIO_MIC_CLK,
                                    .data_out_num = I2S_PIN_NO_CHANGE,
                                    .data_in_num = RG_GPIO_MIC_DATA,
                                });
        if (ret != ESP_OK)
        {
            mic.last_error = esp_err_to_name(ret);
            RG_LOGE("mic: i2s_set_pin failed: %s", mic.last_error);
            i2s_driver_uninstall(port);
            return false;
        }
        // 16000 * 128 = 2.048MHz, inside the IMP34DT05TR normal mode range
        ret = i2s_set_pdm_rx_down_sample(port,
                                         (mic.config.downsample == 128) ? I2S_PDM_DSR_16S : I2S_PDM_DSR_8S);
        if (ret != ESP_OK)
        {
            RG_LOGW("mic: i2s_set_pdm_rx_down_sample failed: %s (clock stays at rate*64)", esp_err_to_name(ret));
            mic.config.downsample = 64;
        }
        mic.clock_hz = mic.config.sample_rate * mic.config.downsample;
        i2s_start(port);
        mic.initialized = true;
        RG_LOGI("mic[%s]: PDM RX on I2S%d (data=GPIO%d clk=GPIO%d) %dHz x%d = %dHz clock, hw_hpf=%d",
                RG_MIC_CHIP_NAME, mic.port, (int)RG_GPIO_MIC_DATA, (int)RG_GPIO_MIC_CLK, mic.config.sample_rate,
                mic.config.downsample, mic.clock_hz, RG_MIC_HW_HPF);
    }

    // The mic needs a few ms to wake up and the DMA is full of garbage: flush it
    int16_t *junk = calloc(MIC_DMA_FRAME_NUM * 2, sizeof(int16_t));
    if (junk)
    {
        rg_task_delay(30);
        for (int i = 0; i < MIC_DMA_DESC_NUM + 1; ++i)
            rg_mic_read(junk, MIC_DMA_FRAME_NUM * 2, 100);
        free(junk);
    }

    return true;
}

void rg_mic_deinit(void)
{
    mic_uninstall();
    mic_return_audio();
    RG_LOGI("mic: released");
}

bool rg_mic_is_initialized(void)
{
    return mic.initialized;
}

const char *rg_mic_get_error(void)
{
    return mic.last_error;
}

rg_mic_config_t rg_mic_get_config(void)
{
    return mic.config;
}

int rg_mic_get_clock_hz(void)
{
    return mic.initialized ? mic.clock_hz : 0;
}

int rg_mic_get_i2s_port(void)
{
    return mic.port;
}

bool rg_mic_holds_audio(void)
{
    return mic.audio_borrowed;
}

const char *rg_mic_get_chip(void)
{
    return RG_MIC_CHIP_NAME;
}

bool rg_mic_has_hw_hpf(void)
{
    return RG_MIC_HW_HPF ? true : false;
}

size_t rg_mic_read(int16_t *buffer, size_t max_samples, int timeout_ms)
{
    if (!mic.initialized || !buffer || !max_samples)
        return 0;

    size_t bytes_read = 0;
    esp_err_t ret = i2s_read((i2s_port_t)mic.port, buffer, max_samples * sizeof(int16_t), &bytes_read,
                             pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK)
    {
        mic.last_error = esp_err_to_name(ret);
        return 0;
    }
    return bytes_read / sizeof(int16_t);
}

void rg_mic_probe_pins(rg_mic_probe_t *probe)
{
    if (!probe)
        return;

    memset(probe, 0, sizeof(rg_mic_probe_t));

    // Stop driving the pins from the i2s matrix (keep the borrowed audio device)
    mic_uninstall();

    const gpio_num_t pins[2] = {RG_GPIO_MIC_DATA, RG_GPIO_MIC_CLK};
    int results[2][2] = {{0, 0}, {0, 0}};

    for (int p = 0; p < 2; ++p)
    {
        gpio_reset_pin(pins[p]);
        gpio_set_direction(pins[p], GPIO_MODE_INPUT);
        for (int m = 0; m < 2; ++m)
        {
            gpio_set_pull_mode(pins[p], m ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
            rg_task_delay(10);
            int ones = 0;
            for (int i = 0; i < 32; ++i)
                ones += gpio_get_level(pins[p]);
            results[p][m] = (ones > 16) ? 1 : 0;
        }
        gpio_set_pull_mode(pins[p], GPIO_FLOATING);
    }

    probe->data_pullup = results[0][0];
    probe->data_pulldown = results[0][1];
    probe->clk_pullup = results[1][0];
    probe->clk_pulldown = results[1][1];
    // Follows the pull => nothing drives it. Note: a PDM mic legitimately tri-states
    // DOUT while its clock is stopped, so "floating" here is NOT proof of a fault.
    probe->data_floating = (probe->data_pullup == 1 && probe->data_pulldown == 0);
    probe->data_stuck_high = (probe->data_pullup == 1 && probe->data_pulldown == 1);
    probe->data_stuck_low = (probe->data_pullup == 0 && probe->data_pulldown == 0);
}

#else // !RG_MIC_ENABLED

#define RG_MIC_CHIP_NAME "n/a"
#define RG_MIC_HW_HPF 0
#define RG_MIC_SILENCE_FLOOR 40
#define RG_MIC_DEFAULT_SLOT 0

bool rg_mic_init(const rg_mic_config_t *config)
{
    return false;
}
void rg_mic_deinit(void)
{
}
bool rg_mic_is_initialized(void)
{
    return false;
}
const char *rg_mic_get_error(void)
{
    return "No microphone configured for this target";
}
rg_mic_config_t rg_mic_get_config(void)
{
    return (rg_mic_config_t){0, 0, 0};
}
int rg_mic_get_clock_hz(void)
{
    return 0;
}
int rg_mic_get_i2s_port(void)
{
    return -1;
}
bool rg_mic_holds_audio(void)
{
    return false;
}
const char *rg_mic_get_chip(void)
{
    return RG_MIC_CHIP_NAME;
}
bool rg_mic_has_hw_hpf(void)
{
    return false;
}
size_t rg_mic_read(int16_t *buffer, size_t max_samples, int timeout_ms)
{
    return 0;
}
void rg_mic_probe_pins(rg_mic_probe_t *probe)
{
    if (probe)
        memset(probe, 0, sizeof(rg_mic_probe_t));
}

#endif // RG_MIC_ENABLED

/* ========================================================================== */
/* Chip independent helpers                                                   */
/* ========================================================================== */

size_t rg_mic_extract_channel(const int16_t *src, size_t src_count, int channel, int16_t *dest, size_t dest_size)
{
    if (!src || !dest || !dest_size)
        return 0;
    size_t out = 0;
    for (size_t i = (size_t)(channel & 1); i < src_count && out < dest_size; i += 2)
        dest[out++] = src[i];
    return out;
}

size_t rg_mic_decode_raw(const int16_t *src, size_t src_count, int decim, int16_t *dest, size_t dest_size)
{
    if (!src || !dest || !dest_size)
        return 0;
    if (decim < 16)
        decim = 16;

    const int words = decim / 16;
    const int scale = 32768 / decim;
    size_t out = 0;

    for (size_t i = 0; i + (size_t)words <= src_count && out < dest_size; i += (size_t)words)
    {
        int ones = 0;
        for (int w = 0; w < words; ++w)
            ones += __builtin_popcount((unsigned)(uint16_t)src[i + w]);
        int v = (ones * 2 - decim) * scale;
        dest[out++] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    return out;
}

void rg_mic_raw_stats(const int16_t *buffer, size_t count, rg_mic_raw_stats_t *stats)
{
    if (!stats)
        return;

    memset(stats, 0, sizeof(rg_mic_raw_stats_t));

    if (!buffer || !count)
        return;

    uint16_t mn = 0xFFFF, mx = 0;
    int prev_bit = -1;

    for (size_t i = 0; i < count; ++i)
    {
        uint16_t w = (uint16_t)buffer[i];
        if (w < mn)
            mn = w;
        if (w > mx)
            mx = w;
        stats->ones += (uint32_t)__builtin_popcount((unsigned)w);
        stats->bits += 16;
        for (int b = 15; b >= 0; --b)
        {
            int bit = (w >> b) & 1;
            if (prev_bit >= 0 && bit != prev_bit)
                stats->edges++;
            prev_bit = bit;
        }
    }

    stats->min_word = mn;
    stats->max_word = mx;
}

void rg_mic_stats(const int16_t *buffer, size_t count, rg_mic_stats_t *stats)
{
    if (!stats)
        return;

    memset(stats, 0, sizeof(rg_mic_stats_t));

    if (!buffer || !count)
        return;

    int64_t sum = 0;
    int32_t mn = 32767, mx = -32768;

    for (size_t i = 0; i < count; ++i)
    {
        int32_t s = buffer[i];
        sum += s;
        if (s < mn)
            mn = s;
        if (s > mx)
            mx = s;
    }

    int32_t dc = (int32_t)(sum / (int64_t)count);
    int64_t squares = 0;
    int32_t peak = 0, crossings = 0, prev_sign = 0;

    for (size_t i = 0; i < count; ++i)
    {
        int32_t v = (int32_t)buffer[i] - dc;
        squares += (int64_t)v * v;
        int32_t mag = (v < 0) ? -v : v;
        if (mag > peak)
            peak = mag;
        int32_t sign = (v > 0) ? 1 : ((v < 0) ? -1 : 0);
        if (sign != 0)
        {
            if (prev_sign != 0 && sign != prev_sign)
                crossings++;
            prev_sign = sign;
        }
    }

    stats->samples = count;
    stats->dc = dc;
    stats->min = (int16_t)mn;
    stats->max = (int16_t)mx;
    stats->peak = peak;
    stats->rms = (int32_t)sqrtf((float)((double)squares / (double)count));
    stats->zero_crossings = crossings;
    stats->all_same = (mn == mx);
}

/* ========================================================================== */
/* Interactive test screen                                                    */
/* ========================================================================== */

#ifdef RG_MIC_ENABLED

#define MIC_RAW_SAMPLES 512                    // int16 words read per iteration
#define MIC_MONO_SAMPLES (MIC_RAW_SAMPLES / 2) // mono samples per iteration
#define MIC_REC_SECONDS 2
#define MIC_PLAY_CHUNK 1024

static const struct
{
    int rate;
    int downsample;
} mic_test_modes[] = {
    {16000, 128}, // 2.048 MHz  <- recommended for IMP34DT05TR
    {32000, 64},  // 2.048 MHz
    {24000, 128}, // 3.072 MHz
    {8000, 128},  // 1.024 MHz  (below the datasheet minimum, for comparison)
};
#define MIC_TEST_MODE_COUNT (int)(sizeof(mic_test_modes) / sizeof(mic_test_modes[0]))

static rg_mic_config_t mic_test_config(int mode, int capture_mode)
{
    return (rg_mic_config_t){mic_test_modes[mode].rate, mic_test_modes[mode].downsample, capture_mode};
}

// Convert one captured buffer into mono PCM, whatever the capture mode is
static size_t mic_to_pcm(const int16_t *raw, size_t count, int capture_mode, int channel, int decim, int16_t *dest,
                         size_t dest_size)
{
    if (capture_mode == RG_MIC_MODE_RAW)
        return rg_mic_decode_raw(raw, count, decim, dest, dest_size);
    return rg_mic_extract_channel(raw, count, channel, dest, dest_size);
}

static size_t mic_play_pcm(const int16_t *pcm, size_t count, int pcm_rate, int gain_shift, rg_audio_frame_t *out)
{
    int out_rate = rg_audio_get_sample_rate();
    int repeat = (pcm_rate > 0) ? (out_rate + pcm_rate / 2) / pcm_rate : 1;
    if (repeat < 1)
        repeat = 1;
    if (repeat > 8)
        repeat = 8;

    size_t pos = 0;
    while (pos < count)
    {
        size_t frames = 0;
        while (pos < count && frames + (size_t)repeat <= MIC_PLAY_CHUNK)
        {
            int32_t v = (int32_t)pcm[pos++] << gain_shift;
            int16_t s = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            for (int r = 0; r < repeat; ++r)
                out[frames++] = (rg_audio_frame_t){s, s};
        }
        rg_audio_submit(out, frames);
    }
    return count;
}

// Record a few seconds, then play it back through the speaker.
// In PDM mode the i2s port is shared with the audio output, so we have to release
// the microphone first; in RAW mode the mic sits on a spare port and can stay up.
static void mic_record_and_play(int mode, int capture_mode, int channel, int gain_shift, int text_w, int msg_y)
{
    rg_mic_config_t cfg = rg_mic_get_config();
    const bool shared = (rg_mic_get_i2s_port() == RG_MIC_AUDIO_I2S_PORT);
    size_t capacity = (size_t)cfg.sample_rate * MIC_REC_SECONDS;
    int16_t *rec = rg_alloc(capacity * sizeof(int16_t), MEM_SLOW);
    int16_t *raw = rg_alloc(MIC_RAW_SAMPLES * sizeof(int16_t), MEM_SLOW);
    rg_audio_frame_t *out = rg_alloc(MIC_PLAY_CHUNK * sizeof(rg_audio_frame_t), MEM_SLOW);
    char buf[96];

    if (!rec || !raw || !out)
    {
        free(rec);
        free(raw);
        free(out);
        rg_gui_draw_text(4, msg_y, text_w, "Record failed: out of memory", C_RED, C_BLACK, 0);
        rg_task_delay(1500);
        return;
    }

    size_t recorded = 0;
    int64_t timeout = rg_system_timer() + (MIC_REC_SECONDS + 2) * 1000000;
    while (recorded < capacity && rg_system_timer() < timeout)
    {
        size_t n = rg_mic_read(raw, MIC_RAW_SAMPLES, 200);
        if (!n)
            continue;
        recorded += mic_to_pcm(raw, n, capture_mode, channel, cfg.downsample, rec + recorded, capacity - recorded);
        snprintf(buf, sizeof(buf), "RECORDING... %d%%  (speak now)", (int)(recorded * 100 / capacity));
        rg_gui_draw_text(4, msg_y, text_w, buf, C_RED, C_BLACK, RG_TEXT_ALIGN_CENTER);
    }

    // Remove the DC offset (on P4 the hardware already did it, harmless either way)
    int64_t sum = 0;
    for (size_t i = 0; i < recorded; ++i)
        sum += rec[i];
    int32_t dc = recorded ? (int32_t)(sum / (int64_t)recorded) : 0;
    for (size_t i = 0; i < recorded; ++i)
        rec[i] = (int16_t)(rec[i] - dc);

    if (shared)
        rg_mic_deinit(); // hands I2S0 and the audio device back
    rg_audio_set_mute(false);

    snprintf(buf, sizeof(buf), "PLAYING %d samples (volume %d%%)", (int)recorded, rg_audio_get_volume());
    rg_gui_draw_text(4, msg_y, text_w, buf, C_LIME, C_BLACK, RG_TEXT_ALIGN_CENTER);
    mic_play_pcm(rec, recorded, cfg.sample_rate, gain_shift, out);

    free(rec);
    free(raw);
    free(out);

    if (shared)
    {
        rg_mic_config_t again = mic_test_config(mode, capture_mode);
        rg_mic_init(&again);
    }
}

static void mic_show_probe(int text_w, int lh, int y)
{
    rg_mic_probe_t probe;
    char buf[128];

    rg_gui_draw_text(4, y, text_w, "Probing pins (clock stopped)...", C_YELLOW, C_BLACK, 0);
    rg_mic_probe_pins(&probe);

    rg_display_clear(C_BLACK);
    int line = 4;
    snprintf(buf, sizeof(buf), "STATIC PIN PROBE - %s", RG_MIC_CHIP_NAME);
    rg_gui_draw_text(4, line, text_w, buf, C_CYAN, C_BLACK, RG_TEXT_ALIGN_CENTER);
    line += lh * 2;

    snprintf(buf, sizeof(buf), "DOUT GPIO%d: pullup=%d pulldown=%d", (int)RG_GPIO_MIC_DATA, probe.data_pullup,
             probe.data_pulldown);
    rg_gui_draw_text(4, line, text_w, buf, C_WHITE, C_BLACK, 0);
    line += lh;
    snprintf(buf, sizeof(buf), "CLK  GPIO%d: pullup=%d pulldown=%d", (int)RG_GPIO_MIC_CLK, probe.clk_pullup,
             probe.clk_pulldown);
    rg_gui_draw_text(4, line, text_w, buf, C_WHITE, C_BLACK, 0);
    line += lh * 2;

    const char *msg;
    rg_color_t color;
    if (probe.data_stuck_high)
    {
        msg = "DOUT is held HIGH -> shorted to VDD?";
        color = C_RED;
    }
    else if (probe.data_stuck_low)
    {
        msg = "DOUT is held LOW -> shorted to GND?";
        color = C_RED;
    }
    else
    {
        msg = "DOUT floats (normal: a PDM mic tri-states";
        color = C_SILVER;
    }
    rg_gui_draw_text(4, line, text_w, msg, color, C_BLACK, 0);
    line += lh;
    if (!probe.data_stuck_high && !probe.data_stuck_low)
    {
        rg_gui_draw_text(4, line, text_w, "its output while the clock is stopped)", C_SILVER, C_BLACK, 0);
        line += lh;
    }
    line += lh;
    rg_gui_draw_text(4, line, text_w, "Use RAW mode (LEFT/RIGHT) to see whether", C_LIGHT_GRAY, C_BLACK, 0);
    line += lh;
    rg_gui_draw_text(4, line, text_w, "the mic drives the line while clocked.", C_LIGHT_GRAY, C_BLACK, 0);
    line += lh * 2;
    rg_gui_draw_text(4, line, text_w, "Press any key to continue", C_YELLOW, C_BLACK, RG_TEXT_ALIGN_CENTER);

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_input_wait_for_key(RG_KEY_ANY, true, 15000);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
}
#endif // RG_MIC_ENABLED

void rg_mic_test_run(void)
{
#ifndef RG_MIC_ENABLED
    rg_gui_alert("Mic Test", "No microphone pins defined for this target.\n\n"
                             "Add RG_GPIO_MIC_DATA and RG_GPIO_MIC_CLK to config.h.");
#else
    const int screen_w = rg_display_get_width();
    const int screen_h = rg_display_get_height();
    const int margin = 4;
    const int text_w = screen_w - margin * 2;

    int16_t *raw = rg_alloc(MIC_RAW_SAMPLES * sizeof(int16_t), MEM_SLOW);
    int16_t *mono[2] = {
        rg_alloc(MIC_MONO_SAMPLES * sizeof(int16_t), MEM_SLOW),
        rg_alloc(MIC_MONO_SAMPLES * sizeof(int16_t), MEM_SLOW),
    };
    rg_audio_frame_t *play_buf = rg_alloc(MIC_PLAY_CHUNK * sizeof(rg_audio_frame_t), MEM_SLOW);
    if (!raw || !mono[0] || !mono[1] || !play_buf)
    {
        free(raw);
        free(mono[0]);
        free(mono[1]);
        free(play_buf);
        rg_gui_alert("Mic Test", "Out of memory!");
        return;
    }

    // Line height of the current font
    rg_rect_t probe_rect = rg_gui_draw_text(0, 0, 0, "Ag", C_WHITE, C_BLACK, RG_TEXT_DUMMY_DRAW);
    const int lh = (probe_rect.height > 4) ? probe_rect.height : 12;

    int mode = 0;
    int capture_mode = RG_MIC_MODE_PDM;
    int gain_shift = 0; // gain = 1 << gain_shift
    int channel = RG_MIC_DEFAULT_SLOT;
    bool monitor = false; // live loopback, only possible when audio is free (RAW mode)
    bool running = true;
    int64_t reads_ok = 0, reads_err = 0;
    int32_t peak_hold = 0, peak_session = 0;
    int64_t peak_hold_time = 0;

    rg_display_clear(C_BLACK);
    rg_gui_draw_text(margin, screen_h / 2, text_w, "Starting microphone...", C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_mic_config_t start_cfg = mic_test_config(mode, capture_mode);
    rg_mic_init(&start_cfg);
    rg_display_clear(C_BLACK);

    // Layout: 8 text lines + level bar, waveform, then 3 lines at the bottom
    const int wave_y = margin + 8 * lh + 14 + 4;
    int wave_h = screen_h - margin - 3 * lh - wave_y - 2;
    if (wave_h < 20)
        wave_h = 20;
    const int wave_w = text_w;
    uint16_t *wave_buf = rg_alloc((size_t)wave_w * wave_h * 2, MEM_SLOW);

    while (running)
    {
        rg_mic_config_t cfg = rg_mic_get_config();
        int clock_hz = rg_mic_get_clock_hz();
        const bool audio_free = !rg_mic_holds_audio();
        char buf[128];
        int y = margin;

        /* ---------------- input ---------------- */
        uint32_t keys = rg_input_read_gamepad();

        if (keys & (RG_KEY_B | RG_KEY_MENU))
        {
            running = false;
            break;
        }
        else if (keys & RG_KEY_A)
        {
            monitor = false;
            mic_record_and_play(mode, capture_mode, channel, gain_shift, text_w, margin + 3 * lh);
            rg_display_clear(C_BLACK);
            peak_hold = peak_session = 0;
            rg_input_wait_for_key(RG_KEY_A, false, 1000);
        }
        else if (keys & RG_KEY_START)
        {
            mode = (mode + 1) % MIC_TEST_MODE_COUNT;
            rg_mic_config_t new_cfg = mic_test_config(mode, capture_mode);
            rg_mic_init(&new_cfg);
            peak_hold = peak_session = 0;
            reads_ok = reads_err = 0;
            rg_display_clear(C_BLACK);
            rg_input_wait_for_key(RG_KEY_START, false, 1000);
        }
        else if (keys & (RG_KEY_LEFT | RG_KEY_RIGHT))
        {
            capture_mode = (capture_mode == RG_MIC_MODE_PDM) ? RG_MIC_MODE_RAW : RG_MIC_MODE_PDM;
            monitor = false;
            rg_mic_config_t new_cfg = mic_test_config(mode, capture_mode);
            rg_mic_init(&new_cfg);
            peak_hold = peak_session = 0;
            reads_ok = reads_err = 0;
            rg_display_clear(C_BLACK);
            rg_input_wait_for_key(RG_KEY_LEFT | RG_KEY_RIGHT, false, 1000);
        }
        else if (keys & RG_KEY_SELECT)
        {
            // SELECT picks the interleaved slot in PDM mode; in RAW mode there is only
            // one stream, so it toggles live monitoring instead (audio port is free).
            if (capture_mode == RG_MIC_MODE_RAW)
                monitor = !monitor;
            else
                channel ^= 1;
            peak_hold = peak_session = 0;
            rg_display_clear(C_BLACK);
            rg_input_wait_for_key(RG_KEY_SELECT, false, 1000);
        }
        else if (keys & RG_KEY_OPTION)
        {
            mic_show_probe(text_w, lh, margin + 3 * lh);
            rg_mic_config_t new_cfg = mic_test_config(mode, capture_mode);
            rg_mic_init(&new_cfg);
            rg_display_clear(C_BLACK);
            continue;
        }
        else if (keys & RG_KEY_UP)
        {
            if (gain_shift < 6)
                gain_shift++;
            rg_input_wait_for_key(RG_KEY_UP, false, 300);
        }
        else if (keys & RG_KEY_DOWN)
        {
            if (gain_shift > 0)
                gain_shift--;
            rg_input_wait_for_key(RG_KEY_DOWN, false, 300);
        }

        /* ---------------- capture ---------------- */
        size_t count = rg_mic_read(raw, MIC_RAW_SAMPLES, 200);
        if (count)
            reads_ok++;
        else
            reads_err++;

        rg_mic_stats_t st0, st1;
        rg_mic_raw_stats_t rst;
        const int16_t *sel;
        size_t sel_count;
        const rg_mic_stats_t *st;

        if (capture_mode == RG_MIC_MODE_RAW)
        {
            rg_mic_raw_stats(raw, count, &rst);
            sel_count = rg_mic_decode_raw(raw, count, cfg.downsample, mono[0], MIC_MONO_SAMPLES);
            sel = mono[0];
            rg_mic_stats(mono[0], sel_count, &st0);
            memset(&st1, 0, sizeof(st1));
            st = &st0;
        }
        else
        {
            memset(&rst, 0, sizeof(rst));
            size_t n0 = rg_mic_extract_channel(raw, count, 0, mono[0], MIC_MONO_SAMPLES);
            size_t n1 = rg_mic_extract_channel(raw, count, 1, mono[1], MIC_MONO_SAMPLES);
            rg_mic_stats(mono[0], n0, &st0);
            rg_mic_stats(mono[1], n1, &st1);
            st = channel ? &st1 : &st0;
            sel = mono[channel & 1];
            sel_count = channel ? n1 : n0;
        }

        if (st->peak > peak_session)
            peak_session = st->peak;
        if (st->peak >= peak_hold || rg_system_timer() - peak_hold_time > 1500000)
        {
            peak_hold = st->peak;
            peak_hold_time = rg_system_timer();
        }

        /* live monitoring, only when the mic is not holding the audio device */
        if (monitor && audio_free && sel_count && capture_mode == RG_MIC_MODE_RAW)
        {
            for (size_t i = 0; i < sel_count; ++i)
                mono[1][i] = (int16_t)(sel[i] - st->dc);
            mic_play_pcm(mono[1], sel_count, cfg.sample_rate, gain_shift, play_buf);
        }

        /* ---------------- draw ---------------- */
        snprintf(buf, sizeof(buf), "MIC TEST %s - %s MODE", RG_MIC_CHIP_NAME,
                 capture_mode == RG_MIC_MODE_RAW ? "RAW BIT" : "HW PDM");
        rg_gui_draw_text(margin, y, text_w, buf, C_CYAN, C_BLACK, RG_TEXT_ALIGN_CENTER);
        y += lh;

        snprintf(buf, sizeof(buf), "DOUT:GPIO%d CLK:GPIO%d(%s) I2S%d %s", (int)RG_GPIO_MIC_DATA,
                 (int)RG_GPIO_MIC_CLK, capture_mode == RG_MIC_MODE_RAW ? "bck" : "ws", rg_mic_get_i2s_port(),
                 rg_mic_is_initialized() ? "ok" : "FAIL");
        rg_gui_draw_text(margin, y, text_w, buf, rg_mic_is_initialized() ? C_SILVER : C_RED, C_BLACK, 0);
        y += lh;

        snprintf(buf, sizeof(buf), "clk %d.%03dMHz pcm %dHz hpf:%s audio:%s", clock_hz / 1000000,
                 (clock_hz / 1000) % 1000, cfg.sample_rate, RG_MIC_HW_HPF ? "hw" : "sw", audio_free ? "free" : "held");
        rg_gui_draw_text(margin, y, text_w, buf, (clock_hz >= 1200000 && clock_hz <= 3250000) ? C_SILVER : C_ORANGE,
                         C_BLACK, 0);
        y += lh;

        /* verdict */
        const char *verdict;
        rg_color_t verdict_color;
        if (!rg_mic_is_initialized())
        {
            verdict = rg_mic_get_error() ? rg_mic_get_error() : "I2S init failed";
            verdict_color = C_RED;
        }
        else if (!count)
        {
            verdict = "NO DATA - i2s_read timeout";
            verdict_color = C_RED;
        }
        else if (capture_mode == RG_MIC_MODE_RAW)
        {
            uint32_t pct = rst.bits ? (rst.ones * 100 / rst.bits) : 0;
            if (rst.edges < rst.bits / 100)
            {
                verdict = (pct > 50) ? "LINE STUCK HIGH - no PDM data at all"
                                     : "LINE STUCK LOW - no PDM data at all";
                verdict_color = C_RED;
            }
            else if (pct < 20 || pct > 80)
            {
                verdict = "bitstream is very lopsided (clock too slow?)";
                verdict_color = C_YELLOW;
            }
            else
            {
                verdict = "PDM BITSTREAM PRESENT - mic is alive!";
                verdict_color = C_LIME;
            }
        }
        else if (st0.all_same && st1.all_same)
        {
#if RG_MIC_HW_HPF
            // P4: the hardware high pass filter makes a dead line read exactly 0
            verdict = (st0.min == 0) ? "DEAD LINE - all zeros, try RAW mode"
                                     : "DEAD LINE - constant value, try RAW mode";
#else
            // S3: no HPF, so a stuck line shows up as a large constant instead
            verdict = (st0.min == 0 && st1.min == 0) ? "DEAD LINE - all zeros (DOUT/CLK/VDD?)"
                                                     : "DEAD LINE - stuck value, try RAW mode";
#endif
            verdict_color = C_RED;
        }
        else if (st->all_same)
        {
            verdict = "this slot is silent - press SELECT (LR pin)";
            verdict_color = C_YELLOW;
        }
        else if (peak_session < RG_MIC_SILENCE_FLOOR)
        {
            verdict = "signal is very low - speak/tap near the mic";
            verdict_color = C_YELLOW;
        }
        else
        {
            verdict = "MIC OK - signal detected";
            verdict_color = C_LIME;
        }
        rg_gui_draw_text(margin, y, text_w, verdict, verdict_color, C_BLACK, 0);
        y += lh;

        if (capture_mode == RG_MIC_MODE_RAW)
        {
            snprintf(buf, sizeof(buf), "bits:%u ones:%u.%u%% edges:%u", (unsigned)rst.bits,
                     (unsigned)(rst.bits ? rst.ones * 100 / rst.bits : 0),
                     (unsigned)(rst.bits ? (rst.ones * 1000 / rst.bits) % 10 : 0), (unsigned)rst.edges);
            rg_gui_draw_text(margin, y, text_w, buf, C_WHITE, C_BLACK, 0);
            y += lh;
            snprintf(buf, sizeof(buf), "word min:%04X max:%04X decim:%d mon:%s", (unsigned)rst.min_word,
                     (unsigned)rst.max_word, cfg.downsample, monitor ? "ON" : "off");
            rg_gui_draw_text(margin, y, text_w, buf, monitor ? C_CYAN : C_WHITE, C_BLACK, 0);
            y += lh;
        }
        else
        {
            snprintf(buf, sizeof(buf), "slot0 rms:%-5d pk:%-5d dc:%-6d", (int)st0.rms, (int)st0.peak, (int)st0.dc);
            rg_gui_draw_text(margin, y, text_w, buf, channel == 0 ? C_WHITE : C_DIM_GRAY, C_BLACK, 0);
            y += lh;
            snprintf(buf, sizeof(buf), "slot1 rms:%-5d pk:%-5d dc:%-6d", (int)st1.rms, (int)st1.peak, (int)st1.dc);
            rg_gui_draw_text(margin, y, text_w, buf, channel == 1 ? C_WHITE : C_DIM_GRAY, C_BLACK, 0);
            y += lh;
        }

        float dbfs = (st->rms > 0) ? 20.f * log10f((float)st->rms / 32768.f) : -99.f;
        snprintf(buf, sizeof(buf), "rms:%-5d max:%-5d %6.1fdBFS ok:%d er:%d", (int)st->rms, (int)peak_session,
                 (double)dbfs, (int)reads_ok, (int)reads_err);
        rg_gui_draw_text(margin, y, text_w, buf, C_LIGHT_GRAY, C_BLACK, 0);
        y += lh;

        snprintf(buf, sizeof(buf), "RAW:%04X %04X %04X %04X %04X %04X", (unsigned)(uint16_t)(count > 0 ? raw[0] : 0),
                 (unsigned)(uint16_t)(count > 1 ? raw[1] : 0), (unsigned)(uint16_t)(count > 2 ? raw[2] : 0),
                 (unsigned)(uint16_t)(count > 3 ? raw[3] : 0), (unsigned)(uint16_t)(count > 4 ? raw[4] : 0),
                 (unsigned)(uint16_t)(count > 5 ? raw[5] : 0));
        rg_gui_draw_text(margin, y, text_w, buf, C_DIM_GRAY, C_BLACK, 0);
        y += lh;

        /* level bar with peak hold */
        {
            const int bar_h = 14;
            int inner_w = text_w - 4;
            int level = (int)(((int64_t)st->peak << gain_shift) * inner_w / 32768);
            int hold = (int)(((int64_t)peak_hold << gain_shift) * inner_w / 32768);
            if (level > inner_w)
                level = inner_w;
            if (hold > inner_w)
                hold = inner_w;
            rg_gui_draw_rect(margin, y, text_w, bar_h, 1, C_DARK_GRAY, C_BLACK);
            if (level > 0)
            {
                rg_color_t c = (level > inner_w * 90 / 100) ? C_RED : (level > inner_w * 60 / 100) ? C_YELLOW : C_LIME;
                rg_gui_draw_rect(margin + 2, y + 2, level, bar_h - 4, 0, C_NONE, c);
            }
            if (hold > 2)
                rg_gui_draw_rect(margin + 2 + hold - 2, y + 2, 2, bar_h - 4, 0, C_NONE, C_WHITE);
        }

        /* waveform */
        if (wave_buf && sel_count > 1)
        {
            int mid = wave_h / 2;
            for (int i = 0; i < wave_w * wave_h; ++i)
                wave_buf[i] = C_BLACK;
            for (int x = 0; x < wave_w; ++x)
                wave_buf[mid * wave_w + x] = 0x2104; // faint center line

            int prev_py = mid;
            for (int x = 0; x < wave_w; ++x)
            {
                size_t idx = (size_t)x * sel_count / (size_t)wave_w;
                int32_t v = ((int32_t)sel[idx] - st->dc) << gain_shift;
                int py = mid - (int)((int64_t)v * mid / 32768);
                if (py < 0)
                    py = 0;
                if (py >= wave_h)
                    py = wave_h - 1;
                int from = (prev_py < py) ? prev_py : py;
                int to = (prev_py < py) ? py : prev_py;
                for (int p = from; p <= to; ++p)
                    wave_buf[p * wave_w + x] = C_LIME;
                prev_py = py;
            }
            rg_gui_copy_buffer(margin, wave_y, wave_w, wave_h, wave_w * 2, wave_buf, false);
        }
        y = wave_y + wave_h + 2;

        rg_gui_draw_text(margin, y, text_w, "A:Rec+Play START:Rate", C_SILVER, C_BLACK, 0);
        y += lh;
        snprintf(buf, sizeof(buf), "L/R:PDM<>RAW SELECT:%s OPTION:Pins",
                 capture_mode == RG_MIC_MODE_RAW ? "Monitor" : "Slot");
        rg_gui_draw_text(margin, y, text_w, buf, C_SILVER, C_BLACK, 0);
        y += lh;
        rg_gui_draw_text(margin, y, text_w, "UP/DOWN:Gain   B or MENU:Exit", C_SILVER, C_BLACK, 0);

        if (!monitor)
            rg_task_delay(10);
    }

    rg_mic_deinit(); // also gives the audio device back if it was borrowed
    free(wave_buf);
    free(play_buf);
    free(mono[0]);
    free(mono[1]);
    free(raw);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
#endif
}
