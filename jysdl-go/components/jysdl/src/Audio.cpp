/* jysdl-go Audio: WAV SFX + MIDI music via retro-go's rg_audio.
 *
 * SFX are 8-bit PCM mono WAVs (~11025 Hz): decoded, resampled to 22050 Hz and
 * mixed (up to MAX_WAV_CHANNELS concurrent) in a dedicated audio task.
 *
 * Music are General MIDI files (sound/game*.mid), rendered with TinySoundFont
 * (tsf.h/tml.h, bundled). TSF needs a SoundFont: look for font.sf2 next to the
 * .mid files (<JY_CurrentPath>sound/font.sf2) or at the game root.
 */
#include "Audio.h"
#include "jymain.h"

#include <rg_system.h>
#include <rg_audio.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "tsf.h"
#include "tml.h"

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAMES_PER_BUF 512
#define MAX_WAV_CHANNELS 8

struct WavChannel
{
    int16_t *data;      // mono int16, resampled to AUDIO_SAMPLE_RATE
    uint32_t length;    // samples
    uint32_t pos;       // play position
    uint8_t volume;     // 0..100
    bool active;
};

static WavChannel g_channels[MAX_WAV_CHANNELS];
static rg_mutex_t *g_audio_lock = NULL;
static volatile bool g_audio_running = false;
static uint8_t g_sfx_volume = 100;

// MIDI state
static tsf *g_font = NULL;
static tml_message *g_midi_list = NULL;   // full event list (for tml_free)
static tml_message *g_midi_next = NULL;   // next unprocessed event
static double g_midi_ms = 0.0;
static bool g_midi_playing = false;
static uint8_t g_music_volume = 100;

// Mix buffers (only touched by the audio task)
static float g_fmix[AUDIO_FRAMES_PER_BUF * 2];
static int16_t g_smix[AUDIO_FRAMES_PER_BUF * 2];

char Audio::currentfile[255] = "";

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Decode a PCM WAV file into mono int16 resampled to AUDIO_SAMPLE_RATE.
static int16_t *decode_wav(const char *path, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        fclose(f);
        return NULL;
    }

    int channels = 1, sample_rate = 22050, bits = 8;
    uint8_t *raw = NULL;
    uint32_t raw_len = 0;

    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8)
    {
        uint32_t csize = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) |
                         ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            channels = (int)fmt[2] | ((int)fmt[3] << 8);
            sample_rate = (int)fmt[4] | ((int)fmt[5] << 8) | ((int)fmt[6] << 16) | ((int)fmt[7] << 24);
            bits = (int)fmt[14] | ((int)fmt[15] << 8);
            if (csize > 16) fseek(f, csize - 16, SEEK_CUR);
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            raw = (uint8_t *)malloc(csize);
            if (!raw) break;
            if (fread(raw, 1, csize, f) != csize) { free(raw); raw = NULL; break; }
            raw_len = csize;
            break;
        }
        else
        {
            fseek(f, csize, SEEK_CUR);
        }
    }
    fclose(f);

    if (!raw || raw_len == 0 || channels < 1 || channels > 2 || (bits != 8 && bits != 16))
    {
        free(raw);
        return NULL;
    }

    int bytes_per_sample = bits / 8;
    uint32_t frame_count = raw_len / (channels * bytes_per_sample);
    if (frame_count == 0) { free(raw); return NULL; }

    int16_t *mono = (int16_t *)malloc(frame_count * sizeof(int16_t));
    if (!mono) { free(raw); return NULL; }

    if (bits == 8)
    {
        for (uint32_t i = 0; i < frame_count; i++)
        {
            int32_t sum = 0;
            for (int c = 0; c < channels; c++)
                sum += ((int32_t)raw[i * channels + c] - 128) * 256;
            mono[i] = (int16_t)(sum / channels);
        }
    }
    else
    {
        for (uint32_t i = 0; i < frame_count; i++)
        {
            int32_t sum = 0;
            for (int c = 0; c < channels; c++)
            {
                int idx = (i * channels + c) * 2;
                sum += (int16_t)((uint8_t)raw[idx] | ((uint8_t)raw[idx + 1] << 8));
            }
            mono[i] = (int16_t)(sum / channels);
        }
    }
    free(raw);

    int16_t *out;
    uint32_t res_len;
    if (sample_rate == AUDIO_SAMPLE_RATE)
    {
        out = mono;
        res_len = frame_count;
    }
    else
    {
        float step = (float)sample_rate / AUDIO_SAMPLE_RATE;
        res_len = (uint32_t)(frame_count / step);
        out = (int16_t *)malloc(res_len * sizeof(int16_t));
        if (!out) { free(mono); return NULL; }
        for (uint32_t i = 0; i < res_len; i++)
        {
            float pos = i * step;
            uint32_t i0 = (uint32_t)pos;
            float frac = pos - i0;
            int32_t v0 = mono[i0];
            int32_t v1 = (i0 + 1 < frame_count) ? mono[i0 + 1] : v0;
            out[i] = (int16_t)(v0 + (int32_t)((v1 - v0) * frac));
        }
        free(mono);
    }

    *out_len = res_len;
    return out;
}

static void midi_stop_notes(void)
{
    if (g_font)
        tsf_note_off_all(g_font);
}

// Render up to `frames` stereo float samples of MIDI into `out` (zeros out first).
static void midi_render(float *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(float));

    if (!g_font || !g_midi_playing)
        return;

    int i = 0;
    while (i < frames && g_midi_playing)
    {
        // Dispatch all events scheduled up to the current position.
        while (g_midi_next && g_midi_next->time <= (unsigned int)g_midi_ms)
        {
            tml_message *msg = g_midi_next;
            switch (msg->type)
            {
                case TML_PROGRAM_CHANGE:
                    tsf_channel_set_presetnumber(g_font, msg->channel, msg->program, (msg->channel == 9));
                    break;
                case TML_NOTE_ON:
                    tsf_channel_note_on(g_font, msg->channel, msg->key, msg->velocity / 127.0f);
                    break;
                case TML_NOTE_OFF:
                    tsf_channel_note_off(g_font, msg->channel, msg->key);
                    break;
                case TML_PITCH_BEND:
                    tsf_channel_set_pitchwheel(g_font, msg->channel, msg->pitch_bend);
                    break;
                case TML_CONTROL_CHANGE:
                    tsf_channel_midi_control(g_font, msg->channel, msg->control, msg->control_value);
                    break;
                default:
                    break;
            }
            g_midi_next = msg->next;
        }

        if (!g_midi_next)
        {
            // End of track reached: loop back to the start.
            tsf_reset(g_font);
            g_midi_next = g_midi_list;
            g_midi_ms = 0.0;
            continue;
        }

        int samples_to_next = (int)((g_midi_next->time - g_midi_ms) * AUDIO_SAMPLE_RATE / 1000.0);
        if (samples_to_next < 1) samples_to_next = 1;
        if (samples_to_next > frames - i) samples_to_next = frames - i;

        tsf_render_float(g_font, out + i * 2, samples_to_next, false);
        g_midi_ms += samples_to_next * 1000.0 / AUDIO_SAMPLE_RATE;
        i += samples_to_next;
    }
}

static void audio_task(void *arg)
{
    (void)arg;

    while (g_audio_running)
    {
        // Protect the shared MIDI + WAV state while mixing (PlayMIDI/PlayWAV
        // from the main thread touch the same state).
        rg_mutex_take(g_audio_lock, 100);

        // 1) Render MIDI into the float mix buffer.
        midi_render(g_fmix, AUDIO_FRAMES_PER_BUF);

        // 1b) Apply music volume to the MIDI portion only.
        float music_scale = g_music_volume / 100.0f;
        for (int i = 0; i < AUDIO_FRAMES_PER_BUF * 2; i++)
            g_fmix[i] *= music_scale;

        // 2) Mix WAV channels on top.
        for (int ch = 0; ch < MAX_WAV_CHANNELS; ch++)
        {
            if (!g_channels[ch].active) continue;

            int16_t *d = g_channels[ch].data;
            uint32_t len = g_channels[ch].length;
            uint32_t pos = g_channels[ch].pos;
            float vol = (float)((int)g_channels[ch].volume * g_sfx_volume / 100) / 100.0f;

            for (int i = 0; i < AUDIO_FRAMES_PER_BUF; i++)
            {
                if (pos >= len) { g_channels[ch].active = false; break; }
                float v = (float)d[pos] / 32768.0f * vol;
                g_fmix[i * 2]     += v;
                g_fmix[i * 2 + 1] += v;
                pos++;
            }
            g_channels[ch].pos = pos;
        }

        rg_mutex_give(g_audio_lock);

        // 3) Convert float -> int16 with clamping.
        for (int i = 0; i < AUDIO_FRAMES_PER_BUF * 2; i++)
        {
            float s = g_fmix[i] * 32767.0f;
            if (s != s) s = 0.0f;  // guard against NaN
            g_smix[i] = clamp16((int32_t)s);
        }

        rg_audio_submit((rg_audio_frame_t *)g_smix, AUDIO_FRAMES_PER_BUF);
    }
}

static void load_soundfont(void)
{
    if (g_font) return;

    char sf2[RG_PATH_MAX + 1];
    snprintf(sf2, sizeof(sf2), "%ssound/font.sf2", JY_CurrentPath);
    g_font = tsf_load_filename(sf2);
    if (!g_font)
    {
        snprintf(sf2, sizeof(sf2), "%sfont.sf2", JY_CurrentPath);
        g_font = tsf_load_filename(sf2);
    }
    if (g_font)
    {
        tsf_set_output(g_font, TSF_STEREO_INTERLEAVED, AUDIO_SAMPLE_RATE, -6.0f);
        tsf_set_max_voices(g_font, 20);
        RG_LOGI("Audio: SoundFont loaded, presets=%d\n", tsf_get_presetcount(g_font));
    }
    else
    {
        RG_LOGW("Audio: SoundFont not found, music disabled\n");
    }
}

void Audio::init()
{
    if (g_audio_running) return;

    memset(g_channels, 0, sizeof(g_channels));
    g_audio_lock = rg_mutex_create();
    g_audio_running = true;

    // SoundFont is loaded lazily on the first PlayMIDI (it's 5.7 MB; deferring
    // it leaves room for the 10 MB TTF font at startup).
    RG_LOGI("Audio: sample_rate=%d\n", rg_audio_get_sample_rate());

    rg_task_create("jysdl_audio", audio_task, NULL, 16384, 1, RG_TASK_PRIORITY_2, 1);
}

void Audio::quit()
{
    g_audio_running = false;
    // The audio task exits after the current submit; OS reclaims memory on reboot.
}

int Audio::PlayWAV(const char *filename)
{
    if (!g_audio_running || !filename || !filename[0]) return -1;

    uint32_t len = 0;
    int16_t *data = decode_wav(filename, &len);
    if (!data) return -1;

    rg_mutex_take(g_audio_lock, 100);

    int target = -1;
    for (int ch = 0; ch < MAX_WAV_CHANNELS; ch++)
    {
        if (!g_channels[ch].active) { target = ch; break; }
    }
    if (target < 0)
    {
        target = 0;
        for (int ch = 1; ch < MAX_WAV_CHANNELS; ch++)
        {
            if (g_channels[ch].pos > g_channels[target].pos) target = ch;
        }
        free(g_channels[target].data);
    }

    g_channels[target].data = data;
    g_channels[target].length = len;
    g_channels[target].pos = 0;
    g_channels[target].volume = 100;
    g_channels[target].active = true;

    rg_mutex_give(g_audio_lock);
    return 0;
}

int Audio::PlayMIDI(const char *filename)
{
    if (!g_audio_running) return -1;

    rg_mutex_take(g_audio_lock, 100);

    if (g_midi_list)
    {
        tml_free(g_midi_list);
        g_midi_list = NULL;
    }
    g_midi_next = NULL;
    g_midi_playing = false;
    midi_stop_notes();

    rg_mutex_give(g_audio_lock);

    if (!filename || !filename[0])
        return -1;

    load_soundfont();   // lazy load (5.7 MB) on first music playback
    if (!g_font)
        return -1;

    tml_message *list = tml_load_filename(filename);
    if (!list) return -1;

    rg_mutex_take(g_audio_lock, 100);
    g_midi_list = list;
    g_midi_next = list;
    g_midi_ms = 0.0;
    g_midi_playing = true;
    rg_mutex_give(g_audio_lock);

    return 0;
}

int Audio::StopMIDI()
{
    rg_mutex_take(g_audio_lock, 100);
    g_midi_playing = false;
    midi_stop_notes();
    rg_mutex_give(g_audio_lock);
    return 0;
}

int Audio::PausedMIDI()
{
    rg_mutex_take(g_audio_lock, 100);
    g_midi_playing = false;
    rg_mutex_give(g_audio_lock);
    return 0;
}

int Audio::ResumeMIDI()
{
    rg_mutex_take(g_audio_lock, 100);
    if (g_midi_next)
        g_midi_playing = true;
    rg_mutex_give(g_audio_lock);
    return 0;
}

void Audio::SetVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_sfx_volume = (uint8_t)volume;
}
