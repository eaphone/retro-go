/*
 * jysdl-go: 金庸群侠传 (jysdl-bh) port for retro-go / ESP32-P4.
 *
 * The engine entry point (jysdl_main) runs the full game lifecycle:
 * Lua init -> Lua_Config -> InitSDL -> InitGame -> Lua_Main -> ExitGame -> ExitSDL.
 *
 * 金庸群侠传 is a directory-based game (not a single ROM): the launcher passes a
 * .lua file from the game data folder, and we use that file's directory as the
 * game root (JY_CurrentPath), from which the engine loads config.lua.
 */
#include <rg_system.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int jysdl_main(int argc, char **argv);
extern void jysdl_set_current_path(const char *path);

static char jy_path[RG_PATH_MAX + 1] = RG_BASE_PATH_ROMS "/jy/";

static void jy_determine_path(const char *romPath)
{
    /* The selected .lua stub lives in roms/dos root and the game data in a
       folder named after it, e.g. /sd/roms/dos/jinyong.lua -> /sd/roms/dos/jinyong/ */
    if (romPath && romPath[0]) {
        const char *slash = strrchr(romPath, '/');
        const char *dot = strrchr(romPath, '.');
        if (slash && dot && dot > slash) {
            size_t dirlen = (size_t)(slash - romPath) + 1;       // "/sd/roms/dos/"
            size_t namelen = (size_t)(dot - slash - 1);          // "jinyong"
            if (dirlen + namelen + 2 < sizeof(jy_path)) {
                memcpy(jy_path, romPath, dirlen);                 // dir
                memcpy(jy_path + dirlen, slash + 1, namelen);     // basename
                jy_path[dirlen + namelen] = '/';
                jy_path[dirlen + namelen + 1] = '\0';
            }
        }
    }
    jysdl_set_current_path(jy_path);
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = 16000,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = false,
        .handlers = {
            .screenshot = NULL,
            .reset = NULL,
            .event = NULL,
        },
        .mallocAlwaysInternal = 0,
    };

    rg_app_t *app = rg_system_init(&config);
    app->configNs = "jysdl-go";

    jy_determine_path(app->romPath);

    char config_file[RG_PATH_MAX + 16];
    snprintf(config_file, sizeof(config_file), "%sconfig.lua", jy_path);
    if (!rg_storage_exists(config_file)) {
        rg_gui_alert("Game data not found",
                     "Put the JYLDCR game files (config.lua + assets) under "
                     RG_BASE_PATH_ROMS "/jy/");
        rg_system_exit();
    }

    RG_LOGI("Starting Jin Yong. Game data: %s", jy_path);

    jysdl_main(0, NULL);

    RG_LOGI("Jin Yong exited");
    rg_system_exit();
}
