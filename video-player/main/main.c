#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpeg_decoder.h"
#include "avi_parse.h"

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;

static bool save_state_handler(const char *filename) { (void)filename; return true; }
static bool load_state_handler(const char *filename) { (void)filename; return true; }
static bool reset_handler(bool hard) { (void)hard; return true; }
static void event_handler(int event, void *arg) {
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static void audio_task(void *arg) {
    avi_player_t *player = (avi_player_t*)arg;
    rg_audio_init(player->info.audio.sample_rate);
    while (true) {
        uint8_t *aud_data = NULL;
        uint32_t aud_size = 0;
        if (avi_player_get_next_audio(player, &aud_data, &aud_size) == 0) {
            // 等待音频缓冲区有空位
            while (rg_audio_get_driver() < aud_size) {
                rg_task_delay(10);
            }
                        rg_audio_submit((const rg_audio_frame_t *)aud_data, aud_size / (player->info.audio.channels * 2));
            free(aud_data);
        } else {
            break; // 音频播放完毕
        }
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
            1000000.0f / info->microsec_per_frame);

    // 帧缓冲区内部 SRAM（硬件解码要求）
    uint32_t fb_size = info->width * info->height * 2;
    uint16_t *frame_buf = rg_alloc(fb_size, MEM_SLOW|MEM_32BIT);
    if (!frame_buf) {
        RG_LOGE("Frame buffer alloc failed");
        avi_player_close(&player);
        return -1;
    }

    // 解码工作缓冲区内部 SRAM
    size_t work_size = 8192;
    uint8_t *work_buf = rg_alloc(work_size, MEM_FAST|MALLOC_CAP_8BIT);
    if (!work_buf) {
        free(frame_buf);
        avi_player_close(&player);
        return -1;
    }

    // 居中显示参数
    int dst_x = (RG_SCREEN_WIDTH - info->width) / 2;
    int dst_y = (RG_SCREEN_HEIGHT - info->height) / 2;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    int copy_w = (info->width < RG_SCREEN_WIDTH) ? info->width : RG_SCREEN_WIDTH;
    int copy_h = (info->height < RG_SCREEN_HEIGHT) ? info->height : RG_SCREEN_HEIGHT;

    uint32_t frame_count = 0;
    uint32_t start_time = rg_system_timer();

    if(info->has_audio && false){
        rg_task_create("audio", audio_task, &player, 2048, 1, RG_TASK_PRIORITY_6, 1);
    }else{
        rg_audio_init(48000);
    }
    bool playing = true;

    while (playing && avi_player_has_more_frames(&player)) {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            continue;
        }

        uint8_t *jpeg_data = NULL;
        uint32_t jpeg_size = 0;
        int ret = avi_player_get_next_frame(&player, &jpeg_data, &jpeg_size);
        if (ret != 0) {
            if (ret == 1) break;
            RG_LOGE("Frame read error, ret=%d", ret);
            break;
        }

        esp_jpeg_image_cfg_t cfg = {
            .indata = jpeg_data,
            .indata_size = jpeg_size,
            .outbuf = (uint8_t*)frame_buf,
            .outbuf_size = fb_size,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = JPEG_IMAGE_SCALE_0,
            .flags.swap_color_bytes = 0,
            .advanced.working_buffer = work_buf,
            .advanced.working_buffer_size = work_size,
        };
        esp_jpeg_image_output_t out;
        esp_err_t err = esp_jpeg_decode(&cfg, &out);
        free(jpeg_data);
        if (err != ESP_OK) {
            RG_LOGW("Frame %lu decode failed: %d", (unsigned long)frame_count, err);
            continue;
        }

        // 复制到表面
        uint16_t *dst = (uint16_t*)currentUpdate->data + dst_y * RG_SCREEN_WIDTH + dst_x;
        uint16_t *src = frame_buf;
        for (int y = 0; y < copy_h; y++) {
            memcpy(dst, src, copy_w * 2);
            dst += RG_SCREEN_WIDTH;
            src += info->width;
        }
        rg_display_submit(currentUpdate, 0);
        //currentUpdate=updates[currentUpdate == updates[0]];

        frame_count++;

        uint32_t delay_ms = info->microsec_per_frame / 1000;
        if (delay_ms == 0) delay_ms = 30;
        uint32_t elapsed = rg_system_timer() - start_time;
        uint32_t expected = frame_count * delay_ms;
        if (elapsed < expected) rg_task_delay(expected - elapsed);

        if (frame_count % 100 == 0) {
            float fps = frame_count * 1000.0f / (rg_system_timer() - start_time);
            RG_LOGV("Frame %lu, %.1f fps", (unsigned long)frame_count, fps);
        }
    }
    rg_task_delay(5000);

    free(work_buf);
    free(frame_buf);
    avi_player_close(&player);
    RG_LOGI("Playback finished: %lu frames", (unsigned long)frame_count);
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
    config.mallocAlwaysInternal = 10*1024;

    app = rg_system_init(&config);
    app->configNs = "video-player";

    const char *video_path = app->romPath;
    updates[0] = rg_surface_create(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, RG_PIXEL_565_LE , MEM_FAST);
    //updates[1] = rg_surface_create(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, RG_PIXEL_565_LE , MEM_FAST);
    currentUpdate = updates[0];

    if (video_path && strlen(video_path) > 0) {
        RG_LOGI("Starting video playback: %s", video_path);
        mjpeg_play(video_path);
    } else {
        RG_LOGE("No video file specified");
    }

    RG_LOGI("exit video player");
    rg_surface_free(updates[0]);
    //rg_surface_free(updates[1]);
    rg_system_exit();
}