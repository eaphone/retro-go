#include <rg_system.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_parse.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "jpeg_decoder.h"

#define AUDIO_SUBMIT_FRAMES 256
#define AUDIO_PREBUFFER_MS 250
#define AUDIO_TASK_QUEUE_LENGTH 16
#define MP3_OUTPUT_BUFFER_SIZE 4608
#define PLAYER_CONTROLS_TIMEOUT_US 3000000LL
#define PLAYER_TOAST_TIMEOUT_US 1200000LL
#define PLAYER_SEEK_SECONDS 10U

#define PLAYER_COLOR_PANEL 0x0862
#define PLAYER_COLOR_TRACK 0x52AB
#define PLAYER_COLOR_ACCENT 0xFB20

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;

typedef struct {
    uint8_t *data;
    uint32_t size;
    bool eos;
} audio_chunk_t;

typedef struct {
    avi_player_t *player;
    rg_task_t *task;
    volatile bool running;
    volatile bool task_ready;
    volatile bool stop_requested;
    volatile bool paused;
    bool end_of_stream;
    uint64_t queued_frames;
    volatile uint64_t submitted_frames;
    volatile uint32_t submitted_chunks;
    volatile uint32_t decode_errors;
    bool mp3_registered;
    bool mp3_info_ready;
    esp_audio_simple_dec_handle_t mp3_decoder;
    uint8_t *mp3_output;
    uint32_t mp3_output_size;
} audio_playback_t;

typedef struct {
    bool paused;
    bool controls_visible;
    bool exit_requested;
    bool has_frame;
    int64_t controls_hide_at;
    int64_t pause_started_at;
    int64_t toast_hide_at;
    uint32_t last_buttons;
    char toast[32];
} player_ui_t;

static bool save_state_handler(const char *filename) { (void)filename; return true; }
static bool load_state_handler(const char *filename) { (void)filename; return true; }
static bool reset_handler(bool hard) { (void)hard; return true; }

static void event_handler(int event, void *arg)
{
    (void)arg;
    if (event == RG_EVENT_REDRAW && currentUpdate) {
        rg_display_submit(currentUpdate, 0);
    }
}

static uint16_t read_pcm_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool audio_format_is_mpeg(uint16_t format_tag)
{
    return format_tag == AVI_AUDIO_FORMAT_MPEG || format_tag == AVI_AUDIO_FORMAT_MP3;
}

static uint32_t read_pcm_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t pcm_sample_to_s16(const uint8_t *sample, uint16_t bits_per_sample)
{
    switch (bits_per_sample) {
        case 8:
            // 8-bit PCM in a WAVE stream is unsigned.
            return (int16_t)(((int)sample[0] - 128) * 256);
        case 16:
            return (int16_t)read_pcm_le16(sample);
        case 24: {
            int32_t value = (int32_t)((uint32_t)sample[0] |
                                      ((uint32_t)sample[1] << 8) |
                                      ((uint32_t)sample[2] << 16));
            if (value & 0x00800000) value -= 0x01000000;
            return (int16_t)(value >> 8);
        }
        case 32:
            return (int16_t)((int32_t)read_pcm_le32(sample) >> 16);
        default:
            return 0;
    }
}

static void submit_pcm_chunk(audio_playback_t *playback, const uint8_t *data, uint32_t size)
{
    const avi_audio_info_t *audio = &playback->player->info.audio;
    const uint32_t bytes_per_sample = audio->bits_per_sample / 8;
    const uint32_t frame_count = size / audio->block_align;
    rg_audio_frame_t frames[AUDIO_SUBMIT_FRAMES];
    uint32_t frame_pos = 0;

    while (frame_pos < frame_count && !playback->stop_requested) {
        while (playback->paused && !playback->stop_requested) {
            rg_task_delay(5);
        }
        if (playback->stop_requested) break;

        uint32_t batch_count = frame_count - frame_pos;
        if (batch_count > AUDIO_SUBMIT_FRAMES) batch_count = AUDIO_SUBMIT_FRAMES;

        for (uint32_t i = 0; i < batch_count; ++i) {
            const uint8_t *input = data + (frame_pos + i) * audio->block_align;
            int16_t left = pcm_sample_to_s16(input, audio->bits_per_sample);
            int16_t right = audio->channels > 1
                ? pcm_sample_to_s16(input + bytes_per_sample, audio->bits_per_sample)
                : left;
            frames[i].left = left;
            frames[i].right = right;
        }

        rg_audio_submit(frames, batch_count);
        playback->submitted_frames += batch_count;
        frame_pos += batch_count;
    }
}

static void submit_s16_chunk(audio_playback_t *playback, const uint8_t *data, uint32_t size,
                             uint8_t channels)
{
    const uint32_t bytes_per_frame = channels * sizeof(int16_t);
    const uint32_t frame_count = bytes_per_frame ? size / bytes_per_frame : 0;
    rg_audio_frame_t frames[AUDIO_SUBMIT_FRAMES];
    uint32_t frame_pos = 0;

    while (frame_pos < frame_count && !playback->stop_requested) {
        while (playback->paused && !playback->stop_requested) {
            rg_task_delay(5);
        }
        if (playback->stop_requested) break;

        uint32_t batch_count = frame_count - frame_pos;
        if (batch_count > AUDIO_SUBMIT_FRAMES) batch_count = AUDIO_SUBMIT_FRAMES;
        for (uint32_t i = 0; i < batch_count; ++i) {
            const uint8_t *input = data + (frame_pos + i) * bytes_per_frame;
            int16_t left = (int16_t)read_pcm_le16(input);
            int16_t right = channels > 1 ? (int16_t)read_pcm_le16(input + 2) : left;
            frames[i].left = left;
            frames[i].right = right;
        }
        rg_audio_submit(frames, batch_count);
        playback->submitted_frames += batch_count;
        frame_pos += batch_count;
    }
}

static bool init_mp3_decoder(audio_playback_t *playback)
{
    esp_audio_err_t error = esp_mp3_dec_register();
    if (error != ESP_AUDIO_ERR_OK) {
        RG_LOGE("MP3 decoder registration failed: %d", error);
        return false;
    }
    playback->mp3_registered = true;

    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    error = esp_audio_simple_dec_open(&config, &playback->mp3_decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        RG_LOGE("MP3 decoder open failed: %d", error);
        esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
        playback->mp3_registered = false;
        return false;
    }

    playback->mp3_output_size = MP3_OUTPUT_BUFFER_SIZE;
    playback->mp3_output = malloc(playback->mp3_output_size);
    if (!playback->mp3_output) {
        RG_LOGE("MP3 output buffer allocation failed");
        esp_audio_simple_dec_close(playback->mp3_decoder);
        playback->mp3_decoder = NULL;
        esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
        playback->mp3_registered = false;
        return false;
    }
    RG_LOGI("MP3 decoder initialized");
    return true;
}

static void close_mp3_decoder(audio_playback_t *playback)
{
    if (playback->mp3_decoder) {
        esp_audio_simple_dec_close(playback->mp3_decoder);
        playback->mp3_decoder = NULL;
    }
    free(playback->mp3_output);
    playback->mp3_output = NULL;
    playback->mp3_output_size = 0;
    if (playback->mp3_registered) {
        esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
        playback->mp3_registered = false;
    }
}

static void decode_mp3_chunk(audio_playback_t *playback, audio_chunk_t *chunk)
{
    esp_audio_simple_dec_raw_t raw = {
        .buffer = chunk->data,
        .len = chunk->size,
        .eos = chunk->eos,
    };

    while (raw.len && !playback->stop_requested) {
        esp_audio_simple_dec_out_t output = {
            .buffer = playback->mp3_output,
            .len = playback->mp3_output_size,
        };
        esp_audio_err_t error = esp_audio_simple_dec_process(playback->mp3_decoder, &raw, &output);
        if (error == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && output.needed_size > playback->mp3_output_size) {
            uint8_t *larger = realloc(playback->mp3_output, output.needed_size);
            if (!larger) {
                RG_LOGE("MP3 output buffer resize to %lu bytes failed",
                        (unsigned long)output.needed_size);
                return;
            }
            playback->mp3_output = larger;
            playback->mp3_output_size = output.needed_size;
            continue;
        }
        if (error != ESP_AUDIO_ERR_OK) {
            RG_LOGE("MP3 decode failed: %d", error);
            playback->decode_errors++;
            return;
        }

        if (output.decoded_size) {
            esp_audio_simple_dec_info_t info;
            if (!playback->mp3_info_ready) {
                memset(&info, 0, sizeof(info));
                error = esp_audio_simple_dec_get_info(playback->mp3_decoder, &info);
                if (error != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
                    !info.sample_rate || (info.channel != 1 && info.channel != 2)) {
                    RG_LOGE("Invalid decoded MP3 format: error=%d, %lu Hz, %u channel(s), %u-bit",
                            error, (unsigned long)info.sample_rate, (unsigned)info.channel,
                            (unsigned)info.bits_per_sample);
                    playback->decode_errors++;
                    return;
                }
                if ((int)info.sample_rate != rg_audio_get_sample_rate()) {
                    rg_audio_set_sample_rate(info.sample_rate);
                }
                playback->mp3_info_ready = true;
                RG_LOGI("MP3 audio ready: %lu Hz, %u channel(s), %u-bit, %lu bps",
                        (unsigned long)info.sample_rate, (unsigned)info.channel,
                        (unsigned)info.bits_per_sample, (unsigned long)info.bitrate);
            } else {
                memset(&info, 0, sizeof(info));
                if (esp_audio_simple_dec_get_info(playback->mp3_decoder, &info) != ESP_AUDIO_ERR_OK) {
                    return;
                }
            }
            submit_s16_chunk(playback, output.buffer, output.decoded_size, info.channel);
        }

        if (raw.consumed > raw.len) {
            RG_LOGE("MP3 decoder reported invalid consumed size");
            playback->decode_errors++;
            return;
        }
        if (!raw.consumed) {
            RG_LOGE("MP3 decoder made no input progress");
            playback->decode_errors++;
            return;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
}

static void audio_task(void *arg)
{
    audio_playback_t *playback = arg;
    rg_task_msg_t message;
    // rg_task_create creates its queue in the child wrapper immediately before
    // entering this function. Signal the video task only after that queue exists.
    playback->task_ready = true;

    while (rg_task_receive(&message, -1)) {
        if (message.type == RG_TASK_MSG_STOP) break;
        if (message.type != 0 || !message.dataPtr) continue;

        audio_chunk_t *chunk = (audio_chunk_t *)message.dataPtr;
        if (audio_format_is_mpeg(playback->player->info.audio.format_tag)) {
            decode_mp3_chunk(playback, chunk);
        } else {
            submit_pcm_chunk(playback, chunk->data, chunk->size);
        }
        playback->submitted_chunks++;
        free(chunk->data);
        free(chunk);
    }

    playback->running = false;
}

static uint32_t estimate_mpeg_sample_frames(const uint8_t *data, uint32_t size)
{
    static const uint16_t mpeg1_bitrate_kbps[3][16] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0},
    };
    static const uint16_t mpeg2_bitrate_kbps[3][16] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160, 0},
        {0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160, 0},
    };
    static const uint32_t base_sample_rates[3] = {44100, 48000, 32000};
    uint32_t offset = 0;
    uint32_t total_samples = 0;

    while (offset + 4 <= size) {
        uint32_t header = ((uint32_t)data[offset] << 24) |
                          ((uint32_t)data[offset + 1] << 16) |
                          ((uint32_t)data[offset + 2] << 8) |
                          data[offset + 3];
        uint8_t version = (header >> 19) & 3U;
        uint8_t layer_bits = (header >> 17) & 3U;
        uint8_t bitrate_index = (header >> 12) & 15U;
        uint8_t rate_index = (header >> 10) & 3U;
        if ((header & 0xFFE00000U) != 0xFFE00000U || version == 1 ||
            layer_bits == 0 || !bitrate_index || bitrate_index == 15 || rate_index == 3) {
            offset++;
            continue;
        }

        uint8_t layer = 3U - layer_bits; // 0=Layer I, 1=Layer II, 2=Layer III
        uint32_t bitrate_kbps = version == 3
            ? mpeg1_bitrate_kbps[layer][bitrate_index]
            : mpeg2_bitrate_kbps[layer][bitrate_index];
        uint32_t sample_rate = base_sample_rates[rate_index];
        if (version == 2) sample_rate /= 2;
        if (version == 0) sample_rate /= 4;
        uint32_t padding = (header >> 9) & 1U;
        uint32_t frame_size;
        uint32_t frame_samples;
        if (layer == 0) {
            frame_size = ((12U * bitrate_kbps * 1000U) / sample_rate + padding) * 4U;
            frame_samples = 384;
        } else if (layer == 2 && version != 3) {
            frame_size = (72U * bitrate_kbps * 1000U) / sample_rate + padding;
            frame_samples = 576;
        } else {
            frame_size = (144U * bitrate_kbps * 1000U) / sample_rate + padding;
            frame_samples = 1152;
        }
        if (frame_size < 4) {
            offset++;
            continue;
        }

        total_samples += frame_samples;
        if (frame_size > size - offset) break;
        offset += frame_size;
    }
    return total_samples;
}

static bool queue_next_audio_chunk(audio_playback_t *playback)
{
    if (!playback->running || playback->end_of_stream) return false;

    audio_chunk_t *chunk = malloc(sizeof(*chunk));
    if (!chunk) {
        RG_LOGE("Failed to allocate audio chunk descriptor");
        playback->end_of_stream = true;
        return false;
    }

    int result = avi_player_get_next_audio(playback->player, &chunk->data, &chunk->size);
    if (result != 0) {
        free(chunk);
        playback->end_of_stream = true;
        if (result < 0) RG_LOGE("Audio chunk read failed: %d", result);
        return false;
    }
    chunk->eos = playback->player->audio_next_offset == 0;

    const avi_audio_info_t *audio = &playback->player->info.audio;
    uint64_t queued_frames;
    if (audio->format_tag == AVI_AUDIO_FORMAT_PCM) {
        queued_frames = chunk->size / audio->block_align;
    } else {
        queued_frames = estimate_mpeg_sample_frames(chunk->data, chunk->size);
        if (!queued_frames && audio->avg_bytes_per_sec) {
            queued_frames = (uint64_t)chunk->size * audio->sample_rate /
                            audio->avg_bytes_per_sec;
        }
        // Some AVI muxers write zero for nAvgBytesPerSec. Most MPEG audio
        // chunks contain one complete MPEG-1 frame, which represents 1152
        // sample frames; use that only if no header could be found.
        if (!queued_frames) queued_frames = 1152;
    }

    rg_task_msg_t message = {.type = 0, .dataPtr = chunk};
    if (!rg_task_send(playback->task, &message, -1)) {
        free(chunk->data);
        free(chunk);
        playback->end_of_stream = true;
        RG_LOGE("Failed to queue audio chunk");
        return false;
    }
    playback->queued_frames += queued_frames;
    return true;
}

static void queue_audio_until(audio_playback_t *playback, uint64_t target_frames)
{
    while (playback->queued_frames < target_frames && queue_next_audio_chunk(playback)) {
        // Keep reading on the video task so FatFs is never accessed concurrently.
    }
}

static void stop_audio_task(audio_playback_t *playback)
{
    if (!playback->running) return;
    playback->stop_requested = true;
    playback->paused = false;
    rg_task_msg_t message = {.type = RG_TASK_MSG_STOP};
    rg_task_send(playback->task, &message, -1);
    while (playback->running) {
        rg_task_delay(5);
    }
}

static bool start_audio_task(audio_playback_t *playback, avi_player_t *player,
                             uint64_t position_frames, bool paused)
{
    memset(playback, 0, sizeof(*playback));
    playback->player = player;
    playback->paused = paused;
    playback->queued_frames = position_frames;
    playback->submitted_frames = position_frames;

    if (!player->info.has_audio) return false;
    if (audio_format_is_mpeg(player->info.audio.format_tag) &&
        !init_mp3_decoder(playback)) {
        return false;
    }

    playback->running = true;
    playback->task = rg_task_create("video_audio", audio_task, playback, 20 * 1024,
                                    AUDIO_TASK_QUEUE_LENGTH, RG_TASK_PRIORITY_6, 1);
    if (!playback->task) {
        playback->running = false;
        close_mp3_decoder(playback);
        RG_LOGE("Failed to start audio playback task");
        return false;
    }

    while (!playback->task_ready) {
        rg_task_delay(1);
    }
    return true;
}

static void fill_rect(rg_surface_t *surface, int x, int y, int width, int height,
                      rg_color_t color)
{
    if (!surface || !surface->data || width <= 0 || height <= 0) return;
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > surface->width) width = surface->width - x;
    if (y + height > surface->height) height = surface->height - y;
    if (width <= 0 || height <= 0) return;

    uint16_t pixel = (uint16_t)color;
    if (surface->format == RG_PIXEL_565_BE) pixel = (pixel << 8) | (pixel >> 8);
    for (int row = 0; row < height; ++row) {
        uint16_t *dst = (uint16_t *)((uint8_t *)surface->data + surface->offset +
                                     (y + row) * surface->stride) + x;
        for (int column = 0; column < width; ++column) dst[column] = pixel;
    }
}

static void draw_play_icon(rg_surface_t *surface, int center_x, int center_y, bool paused)
{
    const int box_size = 34;
    fill_rect(surface, center_x - box_size / 2, center_y - box_size / 2,
              box_size, box_size, PLAYER_COLOR_ACCENT);

    if (paused) {
        for (int row = -9; row <= 9; ++row) {
            int icon_width = 10 - abs(row);
            fill_rect(surface, center_x - 5, center_y + row, icon_width, 1, C_WHITE);
        }
    } else {
        fill_rect(surface, center_x - 6, center_y - 9, 5, 19, C_WHITE);
        fill_rect(surface, center_x + 2, center_y - 9, 5, 19, C_WHITE);
    }
}

static void format_player_time(char *buffer, size_t size, uint64_t seconds)
{
    uint64_t hours = seconds / 3600U;
    uint64_t minutes = (seconds / 60U) % 60U;
    uint64_t secs = seconds % 60U;
    if (hours) {
        snprintf(buffer, size, "%02llu:%02llu:%02llu",
                 (unsigned long long)hours, (unsigned long long)minutes,
                 (unsigned long long)secs);
    } else {
        snprintf(buffer, size, "%02llu:%02llu", (unsigned long long)minutes,
                 (unsigned long long)secs);
    }
}

static const char *player_filename(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *name = slash;
    if (backslash && (!name || backslash > name)) name = backslash;
    return name ? name + 1 : path;
}

static void draw_player_ui(rg_surface_t *surface, const char *filepath,
                           uint32_t frame, uint32_t total_frames,
                           uint32_t frame_time, const player_ui_t *ui)
{
    const int width = surface->width;
    const int height = surface->height;
    const int panel_height = 72;
    const int panel_y = height - panel_height;
    const int track_x = 16;
    const int track_width = width - 32;
    const int track_y = height - 47;
    uint32_t progress_width = total_frames
        ? (uint32_t)(((uint64_t)track_width * frame) / total_frames) : 0;
    if (progress_width > (uint32_t)track_width) progress_width = track_width;

    fill_rect(surface, 0, 0, width, 28, PLAYER_COLOR_PANEL);
    fill_rect(surface, 0, panel_y, width, panel_height, PLAYER_COLOR_PANEL);
    fill_rect(surface, track_x, track_y, track_width, 4, PLAYER_COLOR_TRACK);
    if (progress_width) {
        fill_rect(surface, track_x, track_y, progress_width, 4, PLAYER_COLOR_ACCENT);
    }
    fill_rect(surface, track_x + (int)progress_width - 2, track_y - 2, 5, 8,
              PLAYER_COLOR_ACCENT);

    draw_play_icon(surface, width / 2, height / 2, ui->paused);

    uint64_t elapsed_seconds = ((uint64_t)frame * frame_time) / 1000000ULL;
    uint64_t total_seconds = ((uint64_t)total_frames * frame_time) / 1000000ULL;
    char elapsed[32];
    char duration[32];
    char time_text[72];
    format_player_time(elapsed, sizeof(elapsed), elapsed_seconds);
    format_player_time(duration, sizeof(duration), total_seconds);
    snprintf(time_text, sizeof(time_text), "%s / %s", elapsed, duration);

    rg_gui_set_surface(surface);
    rg_gui_draw_text(10, 6, width - 20, player_filename(filepath), C_WHITE,
                     PLAYER_COLOR_PANEL, RG_TEXT_ALIGN_LEFT);
    rg_gui_draw_text(track_x, height - 37, track_width, time_text, C_WHITE,
                     PLAYER_COLOR_PANEL, RG_TEXT_ALIGN_LEFT | RG_TEXT_MONOSPACE);
    rg_gui_draw_text(track_x, height - 19, track_width,
                     "A Play  </> Seek  ^/v Vol  B Back", C_LIGHT_GRAY,
                     PLAYER_COLOR_PANEL, RG_TEXT_ALIGN_CENTER);

    if (ui->toast[0] && rg_system_timer() < ui->toast_hide_at) {
        rg_rect_t text_rect = TEXT_RECT(ui->toast, width - 24);
        int toast_width = text_rect.width + 12;
        int toast_x = (width - toast_width) / 2;
        int toast_y = panel_y - 27;
        fill_rect(surface, toast_x, toast_y, toast_width, 21, PLAYER_COLOR_PANEL);
        rg_gui_draw_text(toast_x + 6, toast_y + 3, text_rect.width, ui->toast,
                         C_WHITE, PLAYER_COLOR_PANEL, RG_TEXT_ALIGN_CENTER);
    }
    rg_gui_set_surface(NULL);
}

static void compose_video_frame(rg_surface_t *surface, const uint16_t *frame_buffer,
                                const avi_info_t *info, int dst_x, int dst_y,
                                int copy_width, int copy_height)
{
    memset(surface->data, 0, surface->width * surface->height * sizeof(uint16_t));
    uint16_t *dst = (uint16_t *)surface->data + dst_y * surface->width + dst_x;
    const uint16_t *src = frame_buffer;
    for (int y = 0; y < copy_height; ++y) {
        memcpy(dst, src, copy_width * sizeof(uint16_t));
        dst += surface->width;
        src += info->width;
    }
}

static void show_player_controls(player_ui_t *ui, int64_t now)
{
    ui->controls_visible = true;
    ui->controls_hide_at = now + PLAYER_CONTROLS_TIMEOUT_US;
}

static void set_player_toast(player_ui_t *ui, int64_t now, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(ui->toast, sizeof(ui->toast), format, args);
    va_end(args);
    ui->toast_hide_at = now + PLAYER_TOAST_TIMEOUT_US;
    show_player_controls(ui, now);
}

static int mjpeg_play(const char *filepath)
{
    avi_player_t player;
    if (avi_player_init(&player, filepath) != 0) {
        RG_LOGE("AVI init failed");
        return -1;
    }

    const avi_info_t *info = avi_player_get_info(&player);
    RG_LOGI("Video: %lux%lu, %lu frames, %.2f fps",
            (unsigned long)info->width, (unsigned long)info->height,
            (unsigned long)info->total_frames,
            info->microsec_per_frame ? 1000000.0f / info->microsec_per_frame : 0.0f);

    uint32_t fb_size = info->width * info->height * 2;
    uint16_t *frame_buf = rg_alloc(fb_size, MEM_SLOW | MEM_32BIT);
    if (!frame_buf) {
        RG_LOGE("Frame buffer alloc failed");
        avi_player_close(&player);
        return -1;
    }

    size_t work_size = 8192;
    uint8_t *work_buf = rg_alloc(work_size, MEM_FAST | MALLOC_CAP_8BIT);
    if (!work_buf) {
        free(frame_buf);
        avi_player_close(&player);
        return -1;
    }

    int dst_x = (RG_SCREEN_WIDTH - info->width) / 2;
    int dst_y = (RG_SCREEN_HEIGHT - info->height) / 2;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    int copy_w = info->width < RG_SCREEN_WIDTH ? info->width : RG_SCREEN_WIDTH;
    int copy_h = info->height < RG_SCREEN_HEIGHT ? info->height : RG_SCREEN_HEIGHT;

    // Keep the audio device initialized even for silent videos so the options
    // menu can still access the volume and output settings.
    rg_audio_init(info->has_audio ? info->audio.sample_rate : 48000);

    audio_playback_t audio_playback;
    start_audio_task(&audio_playback, &player, 0, false);

    uint32_t frame_count = 0;
    uint32_t rendered_frames = 0;
    uint32_t dropped_frames = 0;
    uint32_t frame_time = info->microsec_per_frame ? info->microsec_per_frame : 33333;
    if (audio_playback.running) {
        queue_audio_until(&audio_playback,
                          (uint64_t)info->audio.sample_rate * AUDIO_PREBUFFER_MS / 1000U);
    }
    int64_t start_time = rg_system_timer();
    player_ui_t ui = {
        .paused = false,
        .controls_visible = true,
        .controls_hide_at = start_time + PLAYER_CONTROLS_TIMEOUT_US,
        // Ignore the launcher confirmation key until it has been released.
        .last_buttons = rg_input_read_gamepad(),
    };
    bool preview_pending = false;

    while (!ui.exit_requested && avi_player_has_more_frames(&player)) {
        int64_t now = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        uint32_t pressed = joystick & ~ui.last_buttons;
        bool redraw_ui = false;

        if (pressed) show_player_controls(&ui, now);

        if (pressed & (RG_KEY_MENU | RG_KEY_OPTION)) {
            bool was_paused = ui.paused;
            int64_t menu_start = now;
            audio_playback.paused = true;
            if (pressed & RG_KEY_MENU) {
                rg_gui_game_menu();
            } else {
                rg_gui_options_menu();
            }
            now = rg_system_timer();
            if (!was_paused) start_time += now - menu_start;
            audio_playback.paused = was_paused;
            show_player_controls(&ui, now);
            redraw_ui = true;
            joystick = rg_input_read_gamepad();
            pressed = 0;
        }

        if (pressed & RG_KEY_B) {
            ui.exit_requested = true;
            break;
        }

        if (pressed & (RG_KEY_A | RG_KEY_START)) {
            ui.paused = !ui.paused;
            audio_playback.paused = ui.paused;
            if (ui.paused) {
                ui.pause_started_at = now;
                set_player_toast(&ui, now, "Paused");
            } else {
                start_time += now - ui.pause_started_at;
                set_player_toast(&ui, now, "Playing");
            }
            redraw_ui = true;
        }

        if (pressed & (RG_KEY_UP | RG_KEY_DOWN)) {
            int volume = rg_audio_get_volume();
            volume += (pressed & RG_KEY_UP) ? 5 : -5;
            if (volume < 0) volume = 0;
            if (volume > 100) volume = 100;
            rg_audio_set_volume(volume);
            set_player_toast(&ui, now, "Volume %d%%", rg_audio_get_volume());
            redraw_ui = true;
        }

        if (pressed & (RG_KEY_LEFT | RG_KEY_RIGHT)) {
            uint32_t seek_frames = (uint32_t)
                (((uint64_t)PLAYER_SEEK_SECONDS * 1000000ULL + frame_time - 1) / frame_time);
            uint32_t target_frame;
            if (pressed & RG_KEY_LEFT) {
                target_frame = frame_count > seek_frames ? frame_count - seek_frames : 0;
            } else {
                uint64_t target = (uint64_t)frame_count + seek_frames;
                target_frame = target < info->total_frames
                    ? (uint32_t)target : info->total_frames - 1;
            }

            stop_audio_task(&audio_playback);
            close_mp3_decoder(&audio_playback);
            if (avi_player_seek_to_frame(&player, target_frame) == 0) {
                frame_count = target_frame;
                start_time = now - (int64_t)frame_count * frame_time;
                if (ui.paused) ui.pause_started_at = now;

                // Reinitializing the sink clears samples that were queued for
                // the old position before starting the decoder at the target.
                rg_audio_deinit();
                rg_audio_init(info->has_audio ? info->audio.sample_rate : 48000);
                uint64_t audio_position = info->has_audio
                    ? ((uint64_t)frame_count * frame_time * info->audio.sample_rate) / 1000000ULL
                    : 0;
                start_audio_task(&audio_playback, &player, audio_position, ui.paused);
                if (audio_playback.running) {
                    queue_audio_until(&audio_playback,
                                      audio_position + (uint64_t)info->audio.sample_rate *
                                      AUDIO_PREBUFFER_MS / 1000U);
                }
                set_player_toast(&ui, now, pressed & RG_KEY_LEFT ? "-10 seconds" : "+10 seconds");
                preview_pending = true;
            } else {
                RG_LOGW("Could not seek to frame %lu", (unsigned long)target_frame);
                uint64_t audio_position = info->has_audio
                    ? ((uint64_t)frame_count * frame_time * info->audio.sample_rate) / 1000000ULL
                    : 0;
                start_audio_task(&audio_playback, &player, audio_position, ui.paused);
                if (audio_playback.running) {
                    queue_audio_until(&audio_playback,
                                      audio_position + (uint64_t)info->audio.sample_rate *
                                      AUDIO_PREBUFFER_MS / 1000U);
                }
                set_player_toast(&ui, now, "Seek unavailable");
            }
            redraw_ui = true;
        }

        ui.last_buttons = joystick;
        if (ui.toast[0] && now >= ui.toast_hide_at) {
            ui.toast[0] = 0;
            redraw_ui = true;
        }
        if (!ui.paused && ui.controls_visible && now >= ui.controls_hide_at) {
            ui.controls_visible = false;
        }

        if (redraw_ui && ui.has_frame && ui.paused && !preview_pending) {
            compose_video_frame(currentUpdate, frame_buf, info, dst_x, dst_y, copy_w, copy_h);
            draw_player_ui(currentUpdate, filepath, frame_count, info->total_frames,
                           frame_time, &ui);
            rg_display_submit(currentUpdate, 0);
        }

        if (ui.paused && !preview_pending) {
            rg_task_delay(20);
            rg_system_tick(0);
            continue;
        }

        uint8_t *jpeg_data = NULL;
        uint32_t jpeg_size = 0;
        int result = avi_player_get_next_frame(&player, &jpeg_data, &jpeg_size);
        if (result != 0) {
            if (result < 0) RG_LOGE("Frame read error: %d", result);
            break;
        }

        frame_count = avi_player_get_current_frame(&player);
        int64_t deadline = start_time + (int64_t)frame_count * frame_time;

        // Keep the same real-time policy as the emulator components: when
        // rendering falls behind, consume but do not decode late video frames.
        // The audio task continues on the real-time clock.
        bool drop_frame = !ui.paused && frame_count > 1 && rg_system_timer() > deadline;
        if (drop_frame) {
            free(jpeg_data);
            dropped_frames++;
        } else {
            esp_jpeg_image_cfg_t cfg = {
                .indata = jpeg_data,
                .indata_size = jpeg_size,
                .outbuf = (uint8_t *)frame_buf,
                .outbuf_size = fb_size,
                .out_format = JPEG_IMAGE_FORMAT_RGB565,
                .out_scale = JPEG_IMAGE_SCALE_0,
                .flags.swap_color_bytes = 0,
                .advanced.working_buffer = work_buf,
                .advanced.working_buffer_size = work_size,
            };
            esp_jpeg_image_output_t out;
            esp_err_t error = esp_jpeg_decode(&cfg, &out);
            free(jpeg_data);
            if (error != ESP_OK) {
                RG_LOGW("Frame %lu decode failed: %d", (unsigned long)(frame_count - 1), error);
            } else {
                compose_video_frame(currentUpdate, frame_buf, info, dst_x, dst_y, copy_w, copy_h);
                ui.has_frame = true;
                if (ui.controls_visible || ui.paused) {
                    draw_player_ui(currentUpdate, filepath, frame_count, info->total_frames,
                                   frame_time, &ui);
                }
                rg_display_submit(currentUpdate, 0);
                rendered_frames++;
            }
        }
        preview_pending = false;

        if (audio_playback.running) {
            uint64_t target_audio_frames =
                ((uint64_t)frame_count * frame_time * info->audio.sample_rate) / 1000000ULL;
            target_audio_frames +=
                (uint64_t)info->audio.sample_rate * AUDIO_PREBUFFER_MS / 1000U;
            queue_audio_until(&audio_playback, target_audio_frames);
        }
        if (!ui.paused) {
            int64_t remaining = deadline - rg_system_timer();
            if (remaining > 1000) rg_task_delay((uint32_t)(remaining / 1000));
        }

        if (frame_count % 100 == 0) {
            int64_t elapsed = rg_system_timer() - start_time;
            float fps = elapsed > 0 ? frame_count * 1000000.0f / elapsed : 0.0f;
            RG_LOGV("Frame %lu, %.1f fps", (unsigned long)frame_count, fps);
        }
    }

    if (audio_playback.running && !ui.exit_requested) {
        uint64_t final_audio_frames =
            ((uint64_t)frame_count * frame_time * info->audio.sample_rate) / 1000000ULL;
        queue_audio_until(&audio_playback, final_audio_frames);

        // Let the already queued tail reach the DAC instead of cutting it off
        // immediately when the last video frame is consumed.
        int64_t drain_deadline = rg_system_timer() + 500000;
        while (audio_playback.submitted_frames < final_audio_frames &&
               rg_system_timer() < drain_deadline && !audio_playback.decode_errors) {
            rg_task_delay(5);
        }
        RG_LOGI("Audio playback: queued=%lu, submitted=%lu frames, chunks=%lu, errors=%lu",
                (unsigned long)audio_playback.queued_frames,
                (unsigned long)audio_playback.submitted_frames,
                (unsigned long)audio_playback.submitted_chunks,
                (unsigned long)audio_playback.decode_errors);
    }

    stop_audio_task(&audio_playback);
    close_mp3_decoder(&audio_playback);
    rg_audio_deinit();
    free(work_buf);
    free(frame_buf);
    avi_player_close(&player);
    RG_LOGI("Playback finished: %lu frames (%lu rendered, %lu dropped)",
            (unsigned long)frame_count, (unsigned long)rendered_frames,
            (unsigned long)dropped_frames);
    return 0;
}

void app_main(void)
{
    rg_config_t config;
    memset(&config, 0, sizeof(config));

    config.sampleRate = 0;
    config.frameRate = 0;
    config.storageRequired = false;
    config.isLauncher = true;
    config.handlers.loadState = &load_state_handler;
    config.handlers.saveState = &save_state_handler;
    config.handlers.reset = &reset_handler;
    config.handlers.event = &event_handler;
    config.mallocAlwaysInternal = 10 * 1024;

    app = rg_system_init(&config);
    app->configNs = "video-player";

    const char *video_path = app->romPath;
    updates[0] = rg_surface_create(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, RG_PIXEL_565_LE, MEM_FAST);
    currentUpdate = updates[0];

    if (video_path && strlen(video_path) > 0) {
        RG_LOGI("Starting video playback: %s", video_path);
        mjpeg_play(video_path);
    } else {
        RG_LOGE("No video file specified");
    }

    RG_LOGI("Exit video player");
    // The display worker copies the surface asynchronously.  Wait for its
    // final update and for the SPI queue to drain before freeing the surface.
    while (rg_display_is_busy()) {
        rg_task_delay(1);
    }
    rg_task_delay(100);
    rg_surface_free(updates[0]);
    rg_system_exit();
}
