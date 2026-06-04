/*
 * Retro-Go Input Integration for tiny386 DOS emulator
 *
 * This replaces the original GPIO-polling input with retro-go's
 * rg_input_read_gamepad() API, following the same pattern as
 * fmsx's ProcessEvents().
 *
 * Key mapping:
 *   D-Pad arrows -> Arrow keys
 *   A            -> Space
 *   B            -> Escape
 *   Select + A   -> Enter
 *   Select + B   -> Backspace
 *   Select + X   -> Tab
 *   Select + Y   -> Win key
 *   Start        -> OSD menu
 *   Select+Start -> Toggle virtual keyboard
 *   Select+Dir   -> Scroll viewport (text mode)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "../../i8042.h"
#include "../../pc.h"
#include "common.h"
#include "menu.h"
#include <rg_input.h>
#include <rg_system.h>  /* For RG_PATH_MAX, RG_BASE_PATH_SAVES */

static const char *TAG = "input";

/* External input mode from main.c (0 = joystick, 1 = keyboard) */
extern int InputMode;

/* PS/2 Set 1 scancodes (keycodes used by ps2_put_keycode) */
#define KEYCODE_UP        0x67
#define KEYCODE_DOWN      0x6c
#define KEYCODE_LEFT      0x69
#define KEYCODE_RIGHT     0x6a
#define KEYCODE_SPACE     0x39
#define KEYCODE_ENTER     0x1c
#define KEYCODE_BACKSPACE 0x0e
#define KEYCODE_ESC       0x01
#define KEYCODE_Y         0x15
#define KEYCODE_N         0x31
#define KEYCODE_WIN       0x5B
#define KEYCODE_TAB       0x0F

/* Scroll step in pixels per button press */
#define SCROLL_STEP      16

/* Repeat timing (microseconds) */
#define KEY_REPEAT_DELAY_US   250000
#define KEY_REPEAT_RATE_US    100000

/* Forward declarations & external references */
void screenshot_save(void);
static void handle_scroll(int dir_x, int dir_y);
extern uint8_t *g_framebuffer;
extern int vk_active;
extern int vk_handle_input(int key);
extern void vk_enter(void);
extern int emu_paused;

/* State tracking for retro-go gamepad */
static uint32_t prev_joystick = 0;

/* Timestamps for repeat timing */
static int64_t arrow_press_time = 0;
static int64_t arrow_last_repeat = 0;
static int arrow_held_key = 0;

/* Flag indicating retro-go menu is active */
bool is_rg_menu = false;

/* Helper: send a PS/2 key press (is_down=1) or release (is_down=0) */
static inline void send_key(int is_down, int keycode)
{
    if (globals.kbd) {
        ps2_put_keycode((PS2KbdState *)globals.kbd, is_down, keycode);
    }
}

/* Main input processing function.
 * Called periodically from the emulator's main loop.
 * Reads retro-go gamepad state via rg_input_read_gamepad()
 * and maps to PS/2 scancodes sent to the emulated keyboard. */
void input_process(void)
{
    int64_t now = rg_system_timer();

    /* Read current retro-go gamepad state */
    uint32_t joystick = rg_input_read_gamepad();

    /* Check for Menu/Option buttons to open retro-go's own menus */
    if (joystick == RG_KEY_MENU)
    {
        is_rg_menu=true;
        rg_gui_game_menu();
        is_rg_menu=false;
        prev_joystick = joystick;
        return;
    }
    if (joystick == RG_KEY_OPTION)
    {
        is_rg_menu=true;
        rg_gui_options_menu();
        is_rg_menu=false;
        prev_joystick = joystick;
        return;
    }

    /* Detect pressed/released edges */
    uint32_t pressed = joystick & ~prev_joystick;
    uint32_t released = prev_joystick & ~joystick;

    /* Modifier keys */
    int select_pressed = (joystick & RG_KEY_SELECT) != 0;

    /* Handle Start button */
    //if (pressed & RG_KEY_START)
    //{
    //    if (vk_active) {
    //        vk_handle_input('S');
    //    } else if (select_pressed) {
    //        vk_enter();
    //    } else if (menu_active) {
    //        menu_exit();
    //        emu_paused = 0;
    //    } else {
    //        menu_enter();
    //        emu_paused = 1;
    //    }
    //}

    /* Process directional keys (with repeat support) */
    int dir_pressed = 0;
    int dir_keycode = 0;

    if (joystick & RG_KEY_UP)    { dir_pressed = 1; dir_keycode = KEYCODE_UP; }
    if (joystick & RG_KEY_DOWN)  { dir_pressed = 1; dir_keycode = KEYCODE_DOWN; }
    if (joystick & RG_KEY_LEFT)  { dir_pressed = 1; dir_keycode = KEYCODE_LEFT; }
    if (joystick & RG_KEY_RIGHT) { dir_pressed = 1; dir_keycode = KEYCODE_RIGHT; }

    if (dir_pressed)
    {
        if (pressed & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT))
        {
            /* Direction just pressed */
            arrow_press_time = now;
            arrow_last_repeat = now;
            arrow_held_key = dir_keycode;

            if (vk_active) {
                char vk_key = 0;
                if (joystick & RG_KEY_UP)    vk_key = 'U';
                if (joystick & RG_KEY_DOWN)  vk_key = 'D';
                if (joystick & RG_KEY_LEFT)  vk_key = 'L';
                if (joystick & RG_KEY_RIGHT) vk_key = 'R';
                if (vk_key) vk_handle_input(vk_key);
            } else if (menu_active) {
                char menu_key = 0;
                if (joystick & RG_KEY_UP)    menu_key = 'U';
                if (joystick & RG_KEY_DOWN)  menu_key = 'D';
                if (joystick & RG_KEY_LEFT)  menu_key = 'L';
                if (joystick & RG_KEY_RIGHT) menu_key = 'R';
                if (menu_key) menu_handle_input(menu_key, 1);
            } else if (select_pressed) {
                int dx = 0, dy = 0;
                if (joystick & RG_KEY_UP)    dy = -1;
                if (joystick & RG_KEY_DOWN)  dy =  1;
                if (joystick & RG_KEY_LEFT)  dx = -1;
                if (joystick & RG_KEY_RIGHT) dx =  1;
                handle_scroll(dx, dy);
            } else {
                send_key(1, dir_keycode);
            }
        }
        else
        {
            /* Direction is being held - handle repeat */
            if (arrow_held_key == dir_keycode && !select_pressed && !menu_active && !vk_active)
            {
                int64_t delay = (arrow_last_repeat == arrow_press_time)
                    ? KEY_REPEAT_DELAY_US : KEY_REPEAT_RATE_US;
                if (now - arrow_last_repeat >= delay)
                {
                    arrow_last_repeat = now;
                    send_key(1, dir_keycode);
                }
            }

            if (select_pressed && !menu_active && !vk_active)
            {
                int64_t delay = (arrow_last_repeat == arrow_press_time)
                    ? KEY_REPEAT_DELAY_US : KEY_REPEAT_RATE_US;
                if (now - arrow_last_repeat >= delay)
                {
                    arrow_last_repeat = now;
                    int dx = 0, dy = 0;
                    if (joystick & RG_KEY_UP)    dy = -1;
                    if (joystick & RG_KEY_DOWN)  dy =  1;
                    if (joystick & RG_KEY_LEFT)  dx = -1;
                    if (joystick & RG_KEY_RIGHT) dx =  1;
                    handle_scroll(dx, dy);
                }
            }
        }
    }
    else
    {
        if (arrow_held_key)
        {
            if (!select_pressed && !menu_active && !vk_active) {
                send_key(0, arrow_held_key);
            }
            arrow_held_key = 0;
        }
    }

    /* Process A button */
    if (pressed & RG_KEY_A)
    {
        if (vk_active) {
            vk_handle_input('A');
        } else if (menu_active) {
            menu_handle_input('A', 1);
        } else if (select_pressed) {
            send_key(1, KEYCODE_ENTER);
        } else {
            send_key(1, KEYCODE_SPACE);
        }
    }
    if (released & RG_KEY_A)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(0, KEYCODE_ENTER);
            else
                send_key(0, KEYCODE_SPACE);
        }
    }

    /* Process B button */
    if (pressed & RG_KEY_B)
    {
        if (vk_active) {
            vk_handle_input('B');
        } else if (menu_active) {
            menu_handle_input('B', 1);
        } else if (select_pressed) {
            send_key(1, KEYCODE_BACKSPACE);
        } else {
            send_key(1, KEYCODE_ESC);
        }
    }
    if (released & RG_KEY_B)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(0, KEYCODE_BACKSPACE);
            else
                send_key(0, KEYCODE_ESC);
        }
    }

    /* Process X button */
    if (pressed & RG_KEY_X)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(1, KEYCODE_TAB);
            else
                send_key(1, KEYCODE_N);
        }
    }
    if (released & RG_KEY_X)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(0, KEYCODE_TAB);
            else
                send_key(0, KEYCODE_N);
        }
    }

    /* Process Y button */
    if (pressed & RG_KEY_Y)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(1, KEYCODE_WIN);
            else
                send_key(1, KEYCODE_Y);
        }
    }
    if (released & RG_KEY_Y)
    {
        if (!vk_active && !menu_active) {
            if (select_pressed)
                send_key(0, KEYCODE_WIN);
            else
                send_key(0, KEYCODE_Y);
        }
    }

    prev_joystick = joystick;
}

/* Handle viewport scrolling (text mode panning) */
static void handle_scroll(int dir_x, int dir_y)
{
    if ((scroll_x_max <= 0 && scroll_y_max <= 0) || !scroll_active) {
        return;
    }

    int new_x = g_scroll_x + dir_x * SCROLL_STEP;
    int new_y = g_scroll_y + dir_y * SCROLL_STEP;

    if (new_x < 0) new_x = 0;
    if (new_x > scroll_x_max) new_x = scroll_x_max;
    if (new_y < 0) new_y = 0;
    if (new_y > scroll_y_max) new_y = scroll_y_max;

    if (new_x != g_scroll_x || new_y != g_scroll_y) {
        g_scroll_x = new_x;
        g_scroll_y = new_y;
        ESP_LOGI(TAG, "Scroll: (%d, %d) [max: %d, %d]",
                 g_scroll_x, g_scroll_y, scroll_x_max, scroll_y_max);
        if (globals.pc) {
            ((PC *)globals.pc)->full_update = 1;
        }
    }
}

/* Initialize retro-go based input (no GPIO setup needed) */
void input_init(void)
{
    ESP_LOGI(TAG, "Initializing retro-go input integration");
    ESP_LOGI(TAG, "Using rg_input_read_gamepad() instead of direct GPIO polling");
    prev_joystick = 0;
    arrow_held_key = 0;
}

/* Screenshot (BMP format) */
void screenshot_save(void)
{
    if (!g_framebuffer) {
        ESP_LOGE(TAG, "Screenshot failed: no framebuffer");
        return;
    }

    int w = LCD_WIDTH;
    int h = LCD_HEIGHT;
    int row_padded = (w * 3 + 3) & ~3;
    int file_size = 54 + row_padded * h;

    ESP_LOGI(TAG, "Screenshot: %dx%d, file=%d bytes", w, h, file_size);

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        bmp = (uint8_t *)malloc(file_size);
        if (!bmp) {
            ESP_LOGE(TAG, "Screenshot failed: malloc %d bytes", file_size);
            return;
        }
    }

    memset(bmp, 0, file_size);

    bmp[0]  = 'B';
    bmp[1]  = 'M';
    *(uint32_t *)(bmp + 2)  = file_size;
    *(uint32_t *)(bmp + 10) = 54;
    *(uint32_t *)(bmp + 14) = 40;
    *(int32_t  *)(bmp + 18) = w;
    *(int32_t  *)(bmp + 22) = h;
    *(uint16_t *)(bmp + 26) = 1;
    *(uint16_t *)(bmp + 28) = 24;
    *(uint32_t *)(bmp + 34) = row_padded * h;

    int stride = w * 2;
    uint8_t *src = g_framebuffer;
    for (int y = 0; y < h; y++) {
        uint8_t *dst_row = bmp + 54 + (h - 1 - y) * row_padded;
        for (int x = 0; x < w; x++) {
            uint16_t pixel = *(uint16_t *)(src + y * stride + x * 2);
            dst_row[x * 3 + 0] = ((pixel)       & 0x1f) << 3;
            dst_row[x * 3 + 1] = ((pixel >> 5)  & 0x3f) << 2;
            dst_row[x * 3 + 2] = ((pixel >> 11) & 0x1f) << 3;
        }
    }

    /* Use retro-go storage for saving screenshot */
    char path[RG_PATH_MAX + 1];
    snprintf(path, RG_PATH_MAX, "%s/screenshot.bmp", RG_BASE_PATH_SAVES);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Screenshot failed: cannot open file");
        heap_caps_free(bmp);
        return;
    }

    size_t written = fwrite(bmp, 1, file_size, fp);
    fclose(fp);
    heap_caps_free(bmp);

    if (written == file_size) {
        ESP_LOGI(TAG, "Screenshot saved (%d bytes)", file_size);
    } else {
        ESP_LOGE(TAG, "Screenshot failed: wrote %d/%d bytes", written, file_size);
    }
}
