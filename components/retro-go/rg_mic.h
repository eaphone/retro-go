#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * rg_mic - PDM MEMS microphone support (ST IMP34DT05TR and compatible)
 *
 * NOTE: include this AFTER rg_system.h (it needs the target's config.h macros).
 *
 * Required in your target's config.h:
 *   #define RG_GPIO_MIC_DATA   GPIO_NUM_xx   // mic DOUT -> ESP32 (input)
 *   #define RG_GPIO_MIC_CLK    GPIO_NUM_xx   // mic CLK  <- ESP32 (output)
 * Currently defined by:
 *   esp32-s3-devkit : DOUT=GPIO41  CLK=GPIO40
 *   esp32-p4-devkit : DOUT=GPIO54  CLK=GPIO53
 *
 * Optional per target overrides (config.h), all have sane defaults:
 *   RG_MIC_DEFAULT_RATE        16000  // PCM sample rate
 *   RG_MIC_DEFAULT_DOWNSAMPLE  128    // pdm_clk = rate * downsample
 *   RG_MIC_DEFAULT_SLOT        0      // 0 = mic LR tied low, 1 = LR tied high
 *   RG_MIC_PDM_I2S_PORT        I2S_NUM_0  // hardware requirement, do not change
 *   RG_MIC_RAW_I2S_PORT        I2S_NUM_1  // spare port used by RAW mode
 *   RG_MIC_SILENCE_FLOOR       25/40  // |peak| under this counts as silence
 *
 * IMPORTANT HARDWARE FACTS (verified in esp-idf 5.5.4 for both ESP32-S3 and ESP32-P4):
 *  - The PDM RX peripheral only exists on I2S port 0 ("PDM is only supported on I2S0"),
 *    and rg_audio uses that same port for playback. So in PDM mode the microphone and
 *    the speaker cannot run at the same time: rg_mic_init() releases the audio device
 *    (rg_audio_deinit) and rg_mic_deinit() restores it (rg_audio_init).
 *    RAW mode does not use the PDM peripheral, so it runs on a spare port (I2S1) and
 *    leaves the audio output alone - live monitoring is possible there.
 *  - The legacy i2s driver (driver/i2s.h, which rg_audio uses) only configures a
 *    usable PDM RX slot mask in *stereo* mode, so we always capture 2 interleaved
 *    slots. One slot carries the mic (which one depends on the mic's LR pin),
 *    the other is usually silent/duplicated. rg_mic_extract_channel() picks one.
 *  - PDM clock = sample_rate * downsample. The IMP34DT05TR needs 1.2-3.25 MHz for
 *    normal mode, so 16000 * 128 = 2.048 MHz is the recommended setting.
 *  - ESP32-P4 only: the PDM RX block has a hardware high-pass filter, which the
 *    legacy driver ends up enabling (cut-off ~23.3Hz, amplify x1). So on P4 the DC
 *    offset is already removed in hardware and a dead line reads 0 instead of a big
 *    constant. The code adapts through the RG_MIC_HW_HPF macro.
 *
 * Two capture modes are available:
 *   PDM mode - the hardware PDM2PCM filter gives ready to use 16bit PCM samples.
 *   RAW mode - plain I2S RX is used to sample the DOUT line bit by bit (the clock
 *              is generated on BCK instead of WS). Each 16bit word then holds 16
 *              consecutive PDM bits. This bypasses the whole PDM decimator and is
 *              the definitive test for "is the microphone driving the data line?".
 *              rg_mic_decode_raw() converts it to PCM in software.
 */

#if defined(RG_GPIO_MIC_DATA) && defined(RG_GPIO_MIC_CLK) && defined(ESP_PLATFORM)
#define RG_MIC_ENABLED 1
#endif

#define RG_MIC_CH_0 0 // first interleaved slot
#define RG_MIC_CH_1 1 // second interleaved slot

#define RG_MIC_MODE_PDM 0 // hardware PDM2PCM
#define RG_MIC_MODE_RAW 1 // raw bitstream capture

typedef struct
{
    int sample_rate; // PCM sample rate per slot (Hz), eg 16000
    int downsample;  // PDM decimation: 64 or 128. pdm_clk = sample_rate * downsample
    int mode;        // RG_MIC_MODE_PDM or RG_MIC_MODE_RAW
} rg_mic_config_t;

typedef struct
{
    size_t samples;         // number of samples analyzed
    int32_t dc;             // mean value (DC offset)
    int16_t min;            // lowest raw sample
    int16_t max;            // highest raw sample
    int32_t peak;           // max |sample - dc|
    int32_t rms;            // rms of (sample - dc)
    int32_t zero_crossings; // sign changes (rough "is this audio?" indicator)
    bool all_same;          // min == max (dead line / stuck bus)
} rg_mic_stats_t;

typedef struct
{
    uint32_t bits;  // total bits examined
    uint32_t ones;  // bits that were 1
    uint32_t edges; // 0->1 and 1->0 transitions
    uint16_t min_word;
    uint16_t max_word;
} rg_mic_raw_stats_t;

typedef struct
{
    int data_pullup;   // level read on DOUT with an internal pull-up (0/1)
    int data_pulldown; // level read on DOUT with an internal pull-down (0/1)
    int clk_pullup;    // same for the CLK pin
    int clk_pulldown;
    bool data_floating; // follows the pull => nothing is driving the line
    bool data_stuck_high;
    bool data_stuck_low;
} rg_mic_probe_t;

// In PDM mode this takes over I2S0 and the audio output device. Returns true on success.
bool rg_mic_init(const rg_mic_config_t *config);
// Releases the i2s port and restores the audio output device if it was borrowed.
void rg_mic_deinit(void);
bool rg_mic_is_initialized(void);
// NULL when no error
const char *rg_mic_get_error(void);
rg_mic_config_t rg_mic_get_config(void);
// Actual clock driven on the mic CLK pin, in Hz (0 if not initialized)
int rg_mic_get_clock_hz(void);
// I2S port currently used (0 in PDM mode, usually 1 in RAW mode)
int rg_mic_get_i2s_port(void);
// True when the audio output device is currently held by the microphone
bool rg_mic_holds_audio(void);
// Chip name of the active per-target macro block ("S3", "P4", ...)
const char *rg_mic_get_chip(void);
// True when the chip removes DC in hardware (ESP32-P4)
bool rg_mic_has_hw_hpf(void);
// Reads raw data: interleaved PCM samples in PDM mode, packed bits in RAW mode.
size_t rg_mic_read(int16_t *buffer, size_t max_samples, int timeout_ms);
// De-interleaves one slot (PDM mode). Returns the number of mono samples written.
size_t rg_mic_extract_channel(const int16_t *src, size_t src_count, int channel, int16_t *dest, size_t dest_size);
// Software PDM->PCM (RAW mode): sums `decim` bits per output sample (sinc1).
size_t rg_mic_decode_raw(const int16_t *src, size_t src_count, int decim, int16_t *dest, size_t dest_size);
void rg_mic_stats(const int16_t *buffer, size_t count, rg_mic_stats_t *stats);
void rg_mic_raw_stats(const int16_t *buffer, size_t count, rg_mic_raw_stats_t *stats);
// Static line test: stops the clock and checks whether anything drives DOUT.
// The mic must be re-initialized afterwards (this uninstalls the i2s driver).
void rg_mic_probe_pins(rg_mic_probe_t *probe);

// Interactive full screen test/diagnostic tool. Returns when the user exits.
void rg_mic_test_run(void);
