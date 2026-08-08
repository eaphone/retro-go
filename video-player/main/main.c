#include <rg_system.h>

#include <stdbool.h>
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

    audio_playback_t audio_playback = {
        .player = &player,
        .task = NULL,
        .running = false,
        .task_ready = false,
        .stop_requested = false,
        .paused = false,
        .end_of_stream = false,
        .queued_frames = 0,
    };
    bool audio_decoder_ready = true;
    if (info->has_audio && audio_format_is_mpeg(info->audio.format_tag)) {
        audio_decoder_ready = init_mp3_decoder(&audio_playback);
    }
    if (info->has_audio && audio_decoder_ready) {
        audio_playback.running = true;
        audio_playback.task = rg_task_create("video_audio", audio_task, &audio_playback, 20 * 1024,
                                             AUDIO_TASK_QUEUE_LENGTH,
                                             RG_TASK_PRIORITY_6, 1);
        if (!audio_playback.task) {
            audio_playback.running = false;
            RG_LOGE("Failed to start audio playback task");
        } else {
            while (!audio_playback.task_ready) {
                rg_task_delay(1);
            }
        }
    }

    uint32_t frame_count = 0;
    uint32_t rendered_frames = 0;
    uint32_t dropped_frames = 0;
    if (audio_playback.running) {
        queue_audio_until(&audio_playback,
                          (uint64_t)info->audio.sample_rate * AUDIO_PREBUFFER_MS / 1000U);
    }
    int64_t start_time = rg_system_timer();

    while (avi_player_has_more_frames(&player)) {
        uint32_t joystick = rg_input_read_gamepad();
        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION)) {
            int64_t pause_start = rg_system_timer();
            audio_playback.paused = true;
            if (joystick & RG_KEY_MENU) {
                rg_gui_game_menu();
            } else {
                rg_gui_options_menu();
            }
            start_time += rg_system_timer() - pause_start;
            audio_playback.paused = false;
            continue;
        }

        uint8_t *jpeg_data = NULL;
        uint32_t jpeg_size = 0;
        int result = avi_player_get_next_frame(&player, &jpeg_data, &jpeg_size);
        if (result != 0) {
            if (result < 0) RG_LOGE("Frame read error: %d", result);
            break;
        }

        frame_count++;
        uint32_t frame_time = info->microsec_per_frame ? info->microsec_per_frame : 33333;
        int64_t deadline = start_time + (int64_t)frame_count * frame_time;

        // Keep the same real-time policy as the emulator components: when
        // rendering falls behind, consume but do not decode late video frames.
        // The audio task continues on the real-time clock.
        bool drop_frame = frame_count > 1 && rg_system_timer() > deadline;
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
                uint16_t *dst = (uint16_t *)currentUpdate->data + dst_y * RG_SCREEN_WIDTH + dst_x;
                uint16_t *src = frame_buf;
                for (int y = 0; y < copy_h; ++y) {
                    memcpy(dst, src, copy_w * sizeof(uint16_t));
                    dst += RG_SCREEN_WIDTH;
                    src += info->width;
                }
                rg_display_submit(currentUpdate, 0);
                rendered_frames++;
            }
        }

        if (audio_playback.running) {
            uint64_t target_audio_frames =
                ((uint64_t)frame_count * frame_time * info->audio.sample_rate) / 1000000ULL;
            target_audio_frames +=
                (uint64_t)info->audio.sample_rate * AUDIO_PREBUFFER_MS / 1000U;
            queue_audio_until(&audio_playback, target_audio_frames);
        }
        int64_t remaining = deadline - rg_system_timer();
        if (remaining > 1000) rg_task_delay((uint32_t)(remaining / 1000));

        if (frame_count % 100 == 0) {
            int64_t elapsed = rg_system_timer() - start_time;
            float fps = elapsed > 0 ? frame_count * 1000000.0f / elapsed : 0.0f;
            RG_LOGV("Frame %lu, %.1f fps", (unsigned long)frame_count, fps);
        }
    }

    if (audio_playback.running) {
        uint32_t frame_time = info->microsec_per_frame ? info->microsec_per_frame : 33333;
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
