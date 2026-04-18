#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mjpeg_player.h>

static rg_app_t *app;

static bool save_state_handler(const char *filename) { (void)filename; return true; }
static bool load_state_handler(const char *filename) { (void)filename; return true; }
static bool reset_handler(bool hard) { (void)hard; return true; }
static void event_handler(int event, void *arg) { (void)event; (void)arg; }

int raw565_play(const char *filepath, int screen_width, int screen_height)
{
    RG_LOGI("raw565_play is called");
    // 注意：这里的 filepath 指向 .raw 文件，不是 .avi
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        RG_LOGE("Cannot open raw file: %s", filepath);
        return -1;
    }

    const uint32_t frame_size = screen_width * screen_height * 2;

    rg_surface_t *surface = rg_surface_create(screen_width, screen_height,
                                               RG_PIXEL_PAL565_BE, MEM_SLOW);
    if (!surface) {
        fclose(f);
        return -1;
    }

    uint16_t *frame = (uint16_t*)malloc(frame_size);
    if (!frame) {
        rg_surface_free(surface);
        fclose(f);
        return -1;
    }

    uint32_t frame_count = 0;
    uint32_t start_time = rg_system_timer();
    bool playing = true;

    while (playing && fread(frame, 1, frame_size, f) == frame_size) {
        uint32_t joy = rg_input_read_gamepad();
        if (joy & RG_KEY_MENU) break;
        if (joy & RG_KEY_START) {
            RG_LOGI("Paused");
            while (playing) {
                rg_task_delay(100);
                joy = rg_input_read_gamepad();
                if (joy & RG_KEY_START) break;
                if (joy & RG_KEY_MENU) { playing = false; break; }
            }
            start_time = rg_system_timer() - (rg_system_timer() - start_time);
            continue;
        }

        uint16_t *src = frame;
        memcpy(surface->data, src, frame_size);

        rg_display_submit(surface, 0);
        frame_count++;

        // 24fps -> 约 41.67ms
        uint32_t delay_ms = 41;
        uint32_t elapsed = rg_system_timer() - start_time;
        uint32_t expected = frame_count * delay_ms;
        if (elapsed < expected) rg_task_delay(expected - elapsed);
    }

    free(frame);
    rg_surface_free(surface);
    fclose(f);
    RG_LOGI("Playback finished: %lu frames", frame_count);
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
    config.mallocAlwaysInternal = 1024;

    app = rg_system_init(&config);
    app->configNs = "video-player";

    const char *video_path = app->romPath;

    if (video_path && strlen(video_path) > 0) {
        RG_LOGI("Starting video playback: %s", video_path);
        
        if(strstr(video_path,".raw")!=NULL){
            raw565_play(video_path, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT);
        }else{
            mjpeg_play(video_path, rg_display_get_width(), rg_display_get_height());
        }
    } else {
        RG_LOGE("No video file specified");
    }

    RG_LOGI("exit video player");
    rg_system_exit();
}