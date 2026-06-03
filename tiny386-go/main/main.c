/*
 * tiny386-go: tiny386 PC Emulator wrapped as a Retro-Go app
 *
 * This integrates the tiny386 386 PC emulator (ESP32-S3 port)
 * as a component that can be launched from retro-go's launcher.
 *
 * Input handling follows the same pattern as fmsx: using
 * rg_input_read_gamepad() in the emulator's main loop rather
 * than polling GPIO pins directly.
 */

#include <rg_system.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AUDIO_SAMPLE_RATE (44100)

static rg_app_t *app;

/* Input mode: 0 = joystick (arrows + A/B as Enter/Esc),
 *             1 = keyboard emulation (arrows as arrow keys)
 * Selected+Start toggles virtual keyboard */
static int InputMode = 1;

/* Forward declaration of the tiny386 entry point */
extern void tiny386_start(const char *config_path);

/* ============================================================
 * Retro-Go handlers
 * ============================================================ */

static bool save_state_handler(const char *filename)
{
    (void)filename;
    return false; /* Save states not supported yet */
}

static bool load_state_handler(const char *filename)
{
    (void)filename;
    return false;
}

static bool reset_handler(bool hard)
{
    (void)hard;
    return false;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    (void)filename;
    (void)width;
    (void)height;
    return false;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        /* Request redraw */
    }
}

/* ============================================================
 * Input mode selector callback (like fmsx's input_select_cb)
 * ============================================================ */
static rg_gui_event_t input_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        InputMode = !InputMode;
        rg_settings_set_number(NS_APP, "Input", InputMode);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, InputMode ? _("Keyboard") : _("Joystick"));
    return RG_DIALOG_VOID;
}
/* ============================================================
 * Options handler
 * ============================================================ */
static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Input"), "-", RG_DIALOG_FLAG_NORMAL, &input_select_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

/* ============================================================
 * About handler
 * ============================================================ */
static void about_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "tiny386 PC Emulator", NULL, RG_DIALOG_FLAG_MESSAGE, NULL};
    *dest++ = (rg_gui_option_t){0, "8086/80186/386 Emulator", NULL, RG_DIALOG_FLAG_MESSAGE, NULL};
    *dest++ = (rg_gui_option_t){0, "By: superzazu", NULL, RG_DIALOG_FLAG_MESSAGE, NULL};
    *dest++ = (rg_gui_option_t){0, "Retro-Go port: eaphone", NULL, RG_DIALOG_FLAG_MESSAGE, NULL};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

/* ============================================================
 * Main entry point
 * ============================================================ */
void app_main(void)
{
    /* Initialize retro-go system */
    app = rg_system_init(&(const rg_config_t){
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .loadState = &load_state_handler,
            .saveState = &save_state_handler,
            .reset = &reset_handler,
            .screenshot = &screenshot_handler,
            .event = &event_handler,
            .options = &options_handler,
            .about = &about_handler,
        },
    });
    app->configNs = "tiny386";

    RG_LOGI("tiny386-go initializing...");

    /* Restore saved input mode */
    InputMode = rg_settings_get_number(NS_APP, "Input", 1);

    /* Check if a config INI file was provided */
    char config_path[RG_PATH_MAX + 1] = {0};

    /* Try default config paths */
    const char *paths[] = {
        RG_BASE_PATH_ROMS "/dos/tiny386.ini",
        NULL,
    };
    for (int i = 0; paths[i]; i++)
    {
        if (rg_storage_exists(paths[i]))
        {
            snprintf(config_path, RG_PATH_MAX, "%s", paths[i]);
            RG_LOGI("Found config: %s", config_path);
            break;
        }
    }

    if (config_path[0] == 0)
    {
        RG_LOGE("No tiny386 config file found!");
        rg_gui_alert("Configuration missing",
            "Place a tiny386.ini config file in:\n"
            RG_BASE_PATH_ROMS "/tiny386.ini\n"
            "See the tiny386 documentation for details.");
        rg_system_exit();
        return;
    }

    /* Start the tiny386 emulator */
    tiny386_start(config_path);

    RG_LOGI("tiny386-go exiting...");
    rg_system_exit();
}

