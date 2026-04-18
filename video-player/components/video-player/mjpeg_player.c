#include <rg_system.h>
#include <string.h>
#include <stdlib.h>
#include "jpeg_decoder.h"
#include "avi_parse.h"

int mjpeg_play(const char *filepath, int screen_width, int screen_height)
{
    RG_LOGI("mjpeg_play is called: screen %dx%d", screen_width, screen_height);

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

    // 使用 rg_surface_create 创建表面（之前在红色测试中工作正常）
    rg_surface_t *surface = rg_surface_create(screen_width, screen_height,
                                               RG_PIXEL_PAL565_LE, MEM_SLOW);
    if (!surface) {
        RG_LOGE("Surface creation failed");
        avi_player_close(&player);
        return -1;
    }

    // 帧缓冲区内部 SRAM（硬件解码要求）
    uint32_t fb_size = info->width * info->height * 2;
    uint16_t *frame_buf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame_buf) {
        RG_LOGE("Frame buffer alloc failed");
        rg_surface_free(surface);
        avi_player_close(&player);
        return -1;
    }

    // 解码工作缓冲区内部 SRAM
    size_t work_size = 8192;
    uint8_t *work_buf = (uint8_t*)heap_caps_malloc(work_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work_buf) {
        free(frame_buf);
        rg_surface_free(surface);
        avi_player_close(&player);
        return -1;
    }

    // 居中显示参数
    int dst_x = (screen_width - info->width) / 2;
    int dst_y = (screen_height - info->height) / 2;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    int copy_w = (info->width < screen_width) ? info->width : screen_width;
    int copy_h = (info->height < screen_height) ? info->height : screen_height;

    uint32_t frame_count = 0;
    uint32_t start_time = rg_system_timer();
    bool playing = true;

    // 红色测试屏幕（确保显示正常）
    uint16_t *pixels = (uint16_t*)surface->data;
    for (int i = 0; i < screen_width * screen_height; i++) {
        pixels[i] = 0xF800;
    }
    rg_display_submit(surface, 0);
    RG_LOGI("Red screen test - should be red");
    rg_task_delay(2000);

    while (playing && avi_player_has_more_frames(&player)) {

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
        uint16_t *dst_line = (uint16_t*)surface->data + dst_y * screen_width + dst_x;
        uint16_t *src_line = frame_buf;
        for (int y = 0; y < copy_h; y++) {
            memcpy(dst_line, src_line, copy_w * 2);
            dst_line += screen_width;
            src_line += info->width;
        }

        rg_display_submit(surface, 0);
        frame_count++;

        uint32_t delay_ms = info->microsec_per_frame / 1000;
        if (delay_ms == 0) delay_ms = 41;
        uint32_t elapsed = rg_system_timer() - start_time;
        uint32_t expected = frame_count * delay_ms;
        if (elapsed < expected) rg_task_delay(expected - elapsed);

        if (frame_count % 100 == 0) {
            float fps = frame_count * 1000.0f / (rg_system_timer() - start_time);
            RG_LOGI("Frame %lu, %.1f fps", (unsigned long)frame_count, fps);
        }
    }
    rg_task_delay(5000);

    free(work_buf);
    free(frame_buf);
    rg_surface_free(surface);
    avi_player_close(&player);
    RG_LOGI("Playback finished: %lu frames", (unsigned long)frame_count);
    return 0;
}