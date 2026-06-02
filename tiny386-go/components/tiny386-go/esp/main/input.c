/*
 * ESP32-S3 GPIO Keyboard Input for tiny386 DOS emulator
 *
 * Key mapping:
 *   RG_KEY_UP     = GPIO_NUM_11  → Up arrow    (0x67)
 *   RG_KEY_DOWN   = GPIO_NUM_12  → Down arrow  (0x6c)
 *   RG_KEY_LEFT   = GPIO_NUM_13  → Left arrow  (0x69)
 *   RG_KEY_RIGHT  = GPIO_NUM_14  → Right arrow (0x6a)
 *   RG_KEY_A      = GPIO_NUM_0   → Enter        (0x1c)
 *   RG_KEY_B      = GPIO_NUM_48  → Escape       (0x01)
 *   RG_KEY_START  = GPIO_NUM_21  → (combo only, no key sent)
 *   RG_KEY_SELECT = GPIO_NUM_47  → (reserved)
 *   RG_KEY_X      = GPIO_NUM_41  → N            (0x31)
 *   RG_KEY_Y      = GPIO_NUM_42  → Y            (0x15)
 *
 * Key behavior:
 *   - Direction: send arrow keys to DOS
 *   - Select + Direction: scroll/pan viewport (text mode)
 *   - A: Space
 *   - B: Escape
 *   - Select + A: Enter
 *   - Select + B: Backspace
 *   - Select + Start: toggle virtual keyboard
 *   - X: N
 *   - Y: Y
 *   - Start: OSD menu (when vk_active: toggle shift)
 *   - B (when vk_active): exit virtual keyboard
 *   - Select: combo modifier
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "../../i8042.h"
#include "../../pc.h"
#include "common.h"
#include "menu.h"

static const char *TAG = "input";

/* ============================================================
 * 按键引脚定义
 * ============================================================ */
#define RG_KEY_UP       GPIO_NUM_11
#define RG_KEY_DOWN     GPIO_NUM_12
#define RG_KEY_LEFT     GPIO_NUM_13
#define RG_KEY_RIGHT    GPIO_NUM_14
#define RG_KEY_START    GPIO_NUM_21
#define RG_KEY_SELECT   GPIO_NUM_47
#define RG_KEY_X        GPIO_NUM_41
#define RG_KEY_Y        GPIO_NUM_42
#define RG_KEY_A        GPIO_NUM_0
#define RG_KEY_B        GPIO_NUM_48

/* ============================================================
 * PS/2 Set 1 scancodes (keycodes used by ps2_put_keycode)
 * ============================================================ */
#define KEYCODE_UP       0x67
#define KEYCODE_DOWN     0x6c
#define KEYCODE_LEFT     0x69
#define KEYCODE_RIGHT    0x6a
#define KEYCODE_SPACE    0x39    /* Select+A → Space */
#define KEYCODE_ENTER    0x1c    /* A button → Enter */
#define KEYCODE_BACKSPACE 0x0e   /* Select+B → Backspace */
#define KEYCODE_ESC      0x01    /* B button */
#define KEYCODE_Y        0x15    /* Y button */
#define KEYCODE_N        0x31    /* X button */
#define KEYCODE_WIN      0x5B    /* Select+Y → Win */
#define KEYCODE_TAB      0x0F    /* Select+N → Tab */

/* Repeat delay and rate for held directional keys (ms) */
#define KEY_REPEAT_DELAY   250   /* initial delay before repeat starts */
#define KEY_REPEAT_RATE    100   /* repeat interval while held */

/* Scroll step in pixels per button press */
#define SCROLL_STEP      16

/* Screenshot filename (BMP format, saved to SD card root) */
#define SCREENSHOT_PATH "/sdcard/screenshot.bmp"

/* Number of GPIO buttons */
#define NUM_BUTTONS      10

/* External framebuffer for screenshot capture */
extern uint8_t *g_framebuffer;

/* Button descriptor */
typedef struct {
    gpio_num_t gpio;
    uint8_t    keycode;        /* PS/2 scancode sent to the emulator */
    uint8_t    prev_state;     /* 1 = pressed, 0 = released */
    uint8_t    sent_release;   /* 1 = release already sent for this press */
    uint32_t   press_time;     /* tick when button was pressed (for repeat delay) */
    uint32_t   last_repeat;    /* tick when last repeat key was sent */
    uint8_t    is_scroll;      /* 1 = this button is a direction (for scroll combo) */
    uint8_t    repeatable;     /* 1 = held key triggers repeats */
} button_t;

/* Button table */
static button_t buttons[NUM_BUTTONS] = {
    { .gpio = RG_KEY_UP,     .keycode = KEYCODE_UP,     .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 1, .repeatable = 1 },
    { .gpio = RG_KEY_DOWN,   .keycode = KEYCODE_DOWN,   .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 1, .repeatable = 1 },
    { .gpio = RG_KEY_LEFT,   .keycode = KEYCODE_LEFT,   .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 1, .repeatable = 1 },
    { .gpio = RG_KEY_RIGHT,  .keycode = KEYCODE_RIGHT,  .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 1, .repeatable = 1 },
    { .gpio = RG_KEY_A,      .keycode = KEYCODE_SPACE,   .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 1 }, /* A → Space (Select+A → Enter) */
    { .gpio = RG_KEY_B,      .keycode = KEYCODE_ESC,      .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 0 }, /* B → ESC (Select+B → Backspace) */
    { .gpio = RG_KEY_START,  .keycode = 0,               .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 0 }, /* screenshot */
    { .gpio = RG_KEY_SELECT, .keycode = 0,               .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 0 }, /* combo only */
    { .gpio = RG_KEY_X,      .keycode = KEYCODE_N,       .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 0 }, /* X → N */
    { .gpio = RG_KEY_Y,      .keycode = KEYCODE_Y,       .prev_state = 0, .sent_release = 1, .press_time = 0, .last_repeat = 0, .is_scroll = 0, .repeatable = 0 }, /* Y → Y */
};

/* Index of buttons in the button table */
#define BTN_START   6
#define BTN_SELECT  7
#define BTN_UP      0
#define BTN_DOWN    1
#define BTN_LEFT    2
#define BTN_RIGHT   3
#define BTN_A       4
#define BTN_B       5
#define BTN_X       8
#define BTN_Y       9

/* Forward declaration */
static void handle_scroll(int dir_x, int dir_y);
void screenshot_save(void);
extern int vk_active;
extern int vk_handle_input(int key);
extern void vk_enter(void);
extern int emu_paused;

/* ============================================================
 * 按键扫描任务 – 轮询所有按键，支持持续触发
 * ============================================================ */
static void input_task(void *arg)
{
    ESP_LOGI(TAG, "Input task started");

    /* Track Select button state for Start+Direction scroll combo */
    int select_pressed = 0;

    TickType_t poll_interval = pdMS_TO_TICKS(30);  /* ~33 Hz polling */

    while (1) {
        /* Read current Select button state */
        int select_level = gpio_get_level(RG_KEY_SELECT);
        int new_select = (select_level == 0) ? 1 : 0;

        /* Handle Select state change (edge detection via saved state) */
        button_t *btn_select = &buttons[BTN_SELECT];
        if (new_select != btn_select->prev_state) {
            btn_select->prev_state = new_select;
        }
        select_pressed = new_select;

        /* Poll all buttons for state changes and repeat handling */
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < NUM_BUTTONS; i++) {
            button_t *btn = &buttons[i];
            int level = gpio_get_level(btn->gpio);
            int is_down = (level == 0) ? 1 : 0;

            if (is_down != btn->prev_state) {
                /* State changed */
                btn->prev_state = is_down;

                if (is_down) {
                    /* Button just pressed */
                    btn->sent_release = 0;
                    btn->press_time = now;
                    btn->last_repeat = now;

                    if (i == BTN_START) {
                        /* Start button:
                         *   - vk_active: 切换上档
                         *   - Select held: 打开/关闭虚拟键盘
                         *   - menu_active: 退出菜单
                         *   - 其他: 打开菜单 */
                        if (vk_active) {
                            vk_handle_input('S');  /* Start → 切换上档 */
                        } else if (select_pressed) {
                            /* Select + Start → 打开/关闭虚拟键盘 */
                            vk_enter();
                        } else if (menu_active) {
                            menu_exit();
                            emu_paused = 0;
                        } else {
                            menu_enter();
                            emu_paused = 1;
                        }
                    } else if (vk_active) {
                        /* 虚拟键盘激活时：转发按键到虚拟键盘 */
                        btn->sent_release = 1;
                        char vk_key = 0;
                        if (i == BTN_UP)    vk_key = 'U';
                        if (i == BTN_DOWN)  vk_key = 'D';
                        if (i == BTN_LEFT)  vk_key = 'L';
                        if (i == BTN_RIGHT) vk_key = 'R';
                        if (i == BTN_A)     vk_key = 'A';
                        if (i == BTN_B)     vk_key = 'B';
                        if (vk_key) vk_handle_input(vk_key);
                        /* B 键关闭虚拟键盘后也退出菜单（如果菜单开着） */
                        if (i == BTN_B && !vk_active && menu_active) {
                            menu_exit();
                            emu_paused = 0;
                        }
                    } else if (menu_active) {
                        /* Menu is active: forward all other buttons to menu */
                        /* Mark sent_release to prevent repeat handling */
                        btn->sent_release = 1;
                        char menu_key = 0;
                        if (i == BTN_UP)    menu_key = 'U';
                        if (i == BTN_DOWN)  menu_key = 'D';
                        if (i == BTN_LEFT)  menu_key = 'L';
                        if (i == BTN_RIGHT) menu_key = 'R';
                        if (i == BTN_A)     menu_key = 'A';
                        if (i == BTN_B)     menu_key = 'B';
                        if (menu_key) menu_handle_input(menu_key, 1);
                    } else if (select_pressed && btn->is_scroll) {
                        /* Select + direction → scroll */
                        int dx = 0, dy = 0;
                        switch (i) {
                            case BTN_UP:    dy = -1; break;
                            case BTN_DOWN:  dy =  1; break;
                            case BTN_LEFT:  dx = -1; break;
                            case BTN_RIGHT: dx =  1; break;
                        }
                        handle_scroll(dx, dy);
                        /* Don't send directional key */
                    } else if (select_pressed && i == BTN_A) {
                        /* Select + A → Enter */
                        if (globals.kbd) {
                            ps2_put_keycode((PS2KbdState *)globals.kbd, 1, KEYCODE_ENTER);
                        }
                    } else if (select_pressed && i == BTN_B) {
                        /* Select + B → Backspace */
                        if (globals.kbd) {
                            ps2_put_keycode((PS2KbdState *)globals.kbd, 1, KEYCODE_BACKSPACE);
                        }
                    } else if (select_pressed && i == BTN_Y) {
                        /* Select + Y → Win */
                        if (globals.kbd) {
                            ps2_put_keycode((PS2KbdState *)globals.kbd, 1, KEYCODE_WIN);
                        }
                    } else if (select_pressed && i == BTN_X) {
                        /* Select + N (X button) → Tab */
                        if (globals.kbd) {
                            ps2_put_keycode((PS2KbdState *)globals.kbd, 1, KEYCODE_TAB);
                        }
                    } else if (btn->keycode != 0 && globals.kbd) {
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 1, btn->keycode);
                        ESP_LOGD(TAG, "DOWN: 0x%02x", btn->keycode);
                    }
                } else {
                    /* Button just released */
                    btn->sent_release = 1;
                    btn->press_time = 0;

                    if (menu_active || vk_active) {
                        /* Menu or virtual keyboard active: ignore release events for buttons */
                    } else if (select_pressed && i == BTN_A && globals.kbd) {
                        /* Select + A release → Enter release */
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, KEYCODE_ENTER);
                    } else if (select_pressed && i == BTN_B && globals.kbd) {
                        /* Select + B release → Backspace release */
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, KEYCODE_BACKSPACE);
                    } else if (select_pressed && i == BTN_Y && globals.kbd) {
                        /* Select + Y release → Win release */
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, KEYCODE_WIN);
                    } else if (select_pressed && i == BTN_X && globals.kbd) {
                        /* Select + N release → Tab release */
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, KEYCODE_TAB);
                    } else if (btn->keycode != 0 && globals.kbd) {
                        /* Don't send direction release if Select combo was active */
                        if (!select_pressed || !btn->is_scroll) {
                            ps2_put_keycode((PS2KbdState *)globals.kbd, 0, btn->keycode);
                            ESP_LOGD(TAG, "UP:   0x%02x", btn->keycode);
                        }
                    }
                }
            } else if (is_down && btn->repeatable && !btn->sent_release && !menu_active && !vk_active) {
                /* Button is held down – handle repeat.
                 * First repeat uses KEY_REPEAT_DELAY, subsequent ones use KEY_REPEAT_RATE. */
                TickType_t threshold = (btn->last_repeat == btn->press_time)
                    ? pdMS_TO_TICKS(KEY_REPEAT_DELAY)
                    : pdMS_TO_TICKS(KEY_REPEAT_RATE);
                TickType_t dt = now - btn->last_repeat;

                if (dt >= threshold) {
                    btn->last_repeat = now;

                    if (select_pressed) {
                        /* Select + direction held → keep scrolling */
                        int dx = 0, dy = 0;
                        switch (i) {
                            case BTN_UP:    dy = -1; break;
                            case BTN_DOWN:  dy =  1; break;
                            case BTN_LEFT:  dx = -1; break;
                            case BTN_RIGHT: dx =  1; break;
                        }
                        handle_scroll(dx, dy);
                    } else if (btn->keycode != 0 && globals.kbd) {
                        ps2_put_keycode((PS2KbdState *)globals.kbd, 1, btn->keycode);
                        ESP_LOGD(TAG, "REPEAT: 0x%02x", btn->keycode);
                    }
                }
            }
        }

        vTaskDelay(poll_interval);
    }
}

/* ============================================================
 * 初始化 GPIO 按键（纯轮询模式，不使用 ISR）
 * ============================================================ */
void input_init(void)
{
    ESP_LOGI(TAG, "Initializing GPIO keyboard input (polling mode)");

    /* Configure GPIO pins as inputs with pull-up resistors */
    uint64_t pin_mask = 0;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        pin_mask |= (1ULL << buttons[i].gpio);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* enable internal pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,      /* no interrupts, polling only */
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Read initial button states */
    for (int i = 0; i < NUM_BUTTONS; i++) {
        int level = gpio_get_level(buttons[i].gpio);
        buttons[i].prev_state = (level == 0) ? 1 : 0;
        buttons[i].press_time = 0;
        buttons[i].last_repeat = 0;
        buttons[i].sent_release = (buttons[i].prev_state == 0) ? 1 : 0;
        ESP_LOGI(TAG, "  GPIO %d → keycode 0x%02x%s%s",
                 buttons[i].gpio, buttons[i].keycode,
                 buttons[i].is_scroll ? " (scroll)" : "",
                 buttons[i].repeatable ? " (repeat)" : "");
    }

    /* Create the input processing task (pinned to core 0 with vga_task) */
    xTaskCreatePinnedToCore(input_task, "input_task", 4096, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "GPIO keyboard input initialized");
}

/* ============================================================
 * 处理滚动/平移视口
 * ============================================================ */
static void handle_scroll(int dir_x, int dir_y)
{
    /* Only scroll if we have a scrollable virtual framebuffer
     * AND the VGA is currently outputting content larger than LCD
     * (text mode at virtual resolution). */
    if ((scroll_x_max <= 0 && scroll_y_max <= 0) || !scroll_active) {
        return;
    }

    int new_x = g_scroll_x + dir_x * SCROLL_STEP;
    int new_y = g_scroll_y + dir_y * SCROLL_STEP;

    /* Clamp to valid range */
    if (new_x < 0) new_x = 0;
    if (new_x > scroll_x_max) new_x = scroll_x_max;
    if (new_y < 0) new_y = 0;
    if (new_y > scroll_y_max) new_y = scroll_y_max;

    if (new_x != g_scroll_x || new_y != g_scroll_y) {
        g_scroll_x = new_x;
        g_scroll_y = new_y;
        ESP_LOGI(TAG, "Scroll: (%d, %d) [max: %d, %d]",
                 g_scroll_x, g_scroll_y, scroll_x_max, scroll_y_max);
        /* Request a full VGA redraw to update the viewport */
        if (globals.pc) {
            ((PC *)globals.pc)->full_update = 1;
        }
    }
}

/* ============================================================
 * 保存截图 (BMP format, RGB565 → RGB888 conversion)
 *
 * 使用 PSRAM 分配大缓冲区，省栈空间
 * ============================================================ */
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

    /* Allocate from PSRAM first, fall back to DRAM */
    uint8_t *bmp = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        bmp = (uint8_t *)malloc(file_size);
        if (!bmp) {
            ESP_LOGE(TAG, "Screenshot failed: malloc %d bytes", file_size);
            return;
        }
    }

    memset(bmp, 0, file_size);

    /* BMP file header (14 bytes) */
    bmp[0]  = 'B';
    bmp[1]  = 'M';
    *(uint32_t *)(bmp + 2)  = file_size;
    *(uint32_t *)(bmp + 10) = 54;

    /* DIB header (BITMAPINFOHEADER, 40 bytes) */
    *(uint32_t *)(bmp + 14) = 40;
    *(int32_t  *)(bmp + 18) = w;
    *(int32_t  *)(bmp + 22) = h;
    *(uint16_t *)(bmp + 26) = 1;
    *(uint16_t *)(bmp + 28) = 24;
    *(uint32_t *)(bmp + 34) = row_padded * h;

    /* Convert RGB565 → BGR888 (BMP stores BGR), bottom-up rows */
    int stride = w * 2;
    uint8_t *src = g_framebuffer;
    for (int y = 0; y < h; y++) {
        uint8_t *dst_row = bmp + 54 + (h - 1 - y) * row_padded;
        for (int x = 0; x < w; x++) {
            uint16_t pixel = *(uint16_t *)(src + y * stride + x * 2);
            dst_row[x * 3 + 0] = ((pixel)       & 0x1f) << 3;  /* B */
            dst_row[x * 3 + 1] = ((pixel >> 5)  & 0x3f) << 2;  /* G */
            dst_row[x * 3 + 2] = ((pixel >> 11) & 0x1f) << 3;  /* R */
        }
    }

    /* Write to SD card */
    FILE *fp = fopen(SCREENSHOT_PATH, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Screenshot failed: cannot open %s", SCREENSHOT_PATH);
        heap_caps_free(bmp);
        return;
    }

    size_t written = fwrite(bmp, 1, file_size, fp);
    fclose(fp);
    heap_caps_free(bmp);

    if (written == file_size) {
        ESP_LOGI(TAG, "Screenshot saved: %s (%d bytes)", SCREENSHOT_PATH, file_size);
    } else {
        ESP_LOGE(TAG, "Screenshot failed: wrote %d/%d bytes", written, file_size);
    }
}
