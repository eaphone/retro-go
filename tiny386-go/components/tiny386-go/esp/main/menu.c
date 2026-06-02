#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "common.h"

static const char *TAG = "menu";

/* ============================================================
 * 菜单状态
 * ============================================================ */
int menu_active = 0;

/* 亮度/音量（通过外部函数读写） */
static int s_brightness = 80;

/* 菜单条目 */
typedef enum {
    MENU_ITEM_BATTERY,      /* 只读 */
    MENU_ITEM_TIME,         /* 只读 */
    MENU_ITEM_BRIGHTNESS,   /* 可调 */
    MENU_ITEM_VOLUME,       /* 可调 */
    MENU_ITEM_DISPLAY_INFO, /* 可执行 */
    MENU_ITEM_SCREENSHOT,   /* 可执行 */
    MENU_ITEM_COUNT
} menu_item_t;

static const char *s_item_labels[MENU_ITEM_COUNT] = {
    "Battery",
    "Time",
    "Brightness",
    "Volume",
    "Display Info",
    "Screenshot",
};

/* 当前选中项 */
static int s_selected = 2;  /* 默认选中 Brightness */
static int s_dirty = 1;

/* OSD 信息显示开关 */
int display_info_on = 0;

/* 外部引用 framebuffer */
extern uint8_t *g_framebuffer;

/* 外部暂停标志 */
extern int emu_paused;

/* 前向声明 */
static void menu_draw(void);
void menu_exit(void);
void hud_toggle(void);
void vk_enter(void);
void vk_exit(void);
int vk_handle_input(int key);
void vk_draw(void);
extern int vk_active;

/* ============================================================
 * 颜色定义 (RGB565)
 * ============================================================ */
#define COL_BG      0x52AA   /* 半透明灰色背景 */
#define COL_WHITE   0xFFFF
#define COL_GRAY    0x8410   /* 灰色 */
#define COL_SEL     0x07E0   /* 选中绿色 */
#define COL_SEL_BG  0x2104   /* 选中行深色半透明背景 */
#define COL_TITLE   0xFD20   /* 标题黄色 */
#define COL_DIM     0xAD55   /* 暗淡文字 */

/* ============================================================
 * 在 framebuffer 上画像素
 * ============================================================ */
static inline void draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    if (!g_framebuffer) return;
    ((uint16_t *)g_framebuffer)[y * LCD_WIDTH + x] = color;
}

/* ============================================================
 * 在 framebuffer 上填充矩形
 * ============================================================ */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!g_framebuffer) return;
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= LCD_HEIGHT) continue;
        uint16_t *line = (uint16_t *)g_framebuffer + yy * LCD_WIDTH;
        for (int col = 0; col < w; col++) {
            int xx = x + col;
            if (xx < 0 || xx >= LCD_WIDTH) continue;
            line[xx] = color;
        }
    }
}

/* ============================================================
 * 用 8x16 像素块绘制 ASCII 字符
 *
 * 使用 vga.c 中定义的 vgafont16 字体。
 * ============================================================ */
extern const uint8_t vgafont16[256 * 16];

static void draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 0 || ch > 127) ch = ' ';
    const uint8_t *glyph = &vgafont16[(unsigned char)ch * 16];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= LCD_WIDTH || py < 0 || py >= LCD_HEIGHT) continue;
            draw_pixel(px, py, (bits & (0x80 >> col)) ? fg : bg);
        }
    }
}

/* ============================================================
 * 绘制字符串
 * ============================================================ */
static void draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        draw_char(x, y, *str, fg, bg);
        x += 8;
        str++;
    }
}

/* ============================================================
 * 进度条
 * ============================================================ */
static void draw_progress(int x, int y, int w, int h, int percent, uint16_t fg, uint16_t bg)
{
    fill_rect(x, y, w, h, bg);
    int fill_w = percent * w / 100;
    if (fill_w > 0) fill_rect(x, y, fill_w, h, fg);
}

/* ============================================================
 * 绘制菜单界面
 * ============================================================ */
static void menu_draw(void)
{
    if (!g_framebuffer) return;

    /* 半透明菜单区域 — 居中，固定宽高 300x184 */
    int pan_w = 300;
    int pan_h = 184;
    int pan_x = (LCD_WIDTH - pan_w) / 2;
    int pan_y = (LCD_HEIGHT - pan_h) / 2;
    if (pan_x < 0) pan_x = 0;
    if (pan_y < 0) pan_y = 0;

    fill_rect(pan_x, pan_y, pan_w, pan_h, COL_BG);
    /* 边框 */
    fill_rect(pan_x, pan_y, pan_w, 1, COL_DIM);
    fill_rect(pan_x, pan_y + pan_h - 1, pan_w, 1, COL_DIM);
    fill_rect(pan_x, pan_y, 1, pan_h, COL_DIM);
    fill_rect(pan_x + pan_w - 1, pan_y, 1, pan_h, COL_DIM);

    /* 标题 */
    draw_string(pan_x + 8, pan_y + 4, "Menu", COL_TITLE, COL_BG);
    fill_rect(pan_x + 2, pan_y + 22, pan_w - 4, 1, COL_DIM);

    int y = pan_y + 30;
    int step = 18;
    int label_x = pan_x + 8;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int is_selected = (i == s_selected);
        uint16_t fg = is_selected ? COL_SEL : COL_WHITE;

        /* 选中高亮背景 */
        if (is_selected) {
            fill_rect(pan_x + 2, y - 2, pan_w - 4, step, COL_SEL_BG);
        }

        /* 条目名称 */
        draw_string(label_x, y, s_item_labels[i], fg, is_selected ? COL_SEL_BG : COL_BG);

        /* 右侧值部分 */
        char buf[32];
        switch (i) {
        case MENU_ITEM_BATTERY:
            snprintf(buf, sizeof(buf), "--%%");
            break;
        case MENU_ITEM_TIME: {
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            if (t) strftime(buf, sizeof(buf), "%H:%M", t);
            else snprintf(buf, sizeof(buf), "--:--");
            break;
        }
        case MENU_ITEM_BRIGHTNESS:
            snprintf(buf, sizeof(buf), "%d%%", s_brightness);
            draw_progress(label_x + 88, y + 6, 100, 6, s_brightness, COL_SEL, COL_DIM);
            draw_string(pan_x + pan_w - 48, y, buf, fg, is_selected ? COL_SEL_BG : COL_BG);
            y += step;
            continue;
        case MENU_ITEM_VOLUME:
            snprintf(buf, sizeof(buf), "%d%%", volume_get());
            draw_progress(label_x + 88, y + 6, 100, 6, volume_get(), COL_SEL, COL_DIM);
            draw_string(pan_x + pan_w - 48, y, buf, fg, is_selected ? COL_SEL_BG : COL_BG);
            y += step;
            continue;
        case MENU_ITEM_SCREENSHOT:
            snprintf(buf, sizeof(buf), "[A]");
            break;
        case MENU_ITEM_DISPLAY_INFO:
            snprintf(buf, sizeof(buf), display_info_on ? "ON" : "OFF");
            break;
        }

        /* 绘制只读/可执行条目的值 */
        uint16_t val_color = (i <= MENU_ITEM_TIME) ? COL_DIM : fg;
        uint16_t val_bg = is_selected ? COL_SEL_BG : COL_BG;
        draw_string(pan_x + pan_w - 48, y, buf, val_color, val_bg);

        y += step;
    }

    /* 底部提示 */
    draw_string(pan_x + 8, pan_y + pan_h - 20, "B: back", COL_DIM, COL_BG);
}

/* ============================================================
 * 输入处理（由 input.c 轮询时调用）
 * ============================================================ */
int menu_handle_input(int key, int is_down)
{
    if (!is_down) return 1; /* 只处理按下 */

    switch (key) {
    case 'U': /* 上 (由 input.c 用自定义码传入) */
        if (s_selected > 0) {
            s_selected--;
            /* 跳过只读条目 */
            while (s_selected >= 0 && s_selected <= MENU_ITEM_TIME) s_selected--;
            if (s_selected < 0) s_selected = 0;
            s_dirty = 1;
        }
        return 0;
    case 'D': /* 下 */
        if (s_selected < MENU_ITEM_COUNT - 1) {
            s_selected++;
            /* 跳过只读条目 */
            while (s_selected <= MENU_ITEM_TIME) s_selected++;
            if (s_selected >= MENU_ITEM_COUNT) s_selected = MENU_ITEM_COUNT - 1;
            s_dirty = 1;
        }
        return 0;
    case 'L': /* 左 */
        if (s_selected == MENU_ITEM_BRIGHTNESS && s_brightness > 0) {
            s_brightness -= 5; s_dirty = 1;
            backlight_set(s_brightness);
        }
        if (s_selected == MENU_ITEM_VOLUME) {
            int v = volume_get();
            if (v > 0) { v -= 5; volume_set(v); s_dirty = 1; }
        }
        return 0;
    case 'R': /* 右 */
        if (s_selected == MENU_ITEM_BRIGHTNESS && s_brightness < 100) {
            s_brightness += 5; s_dirty = 1;
            backlight_set(s_brightness);
        }
        if (s_selected == MENU_ITEM_VOLUME) {
            int v = volume_get();
            if (v < 100) { v += 5; volume_set(v); s_dirty = 1; }
        }
        return 0;
    case 'A': /* A 键 = 执行 */
        if (s_selected == MENU_ITEM_SCREENSHOT) {
            extern void screenshot_save(void);
            screenshot_save();
            return 0;
        }
        if (s_selected == MENU_ITEM_DISPLAY_INFO) {
            hud_toggle();
            return 0;
        }
        return 1;
    case 'B': /* B 键 = 退出 */
        menu_exit();
        return 0;
    default:
        return 1;
    }
}

/* ============================================================
 * 公开函数
 * ============================================================ */
void menu_init(void)
{
    ESP_LOGI(TAG, "Menu system initialized");
}

void menu_enter(void)
{
    menu_active = 1;
    s_selected = 2;   /* 默认选亮度 */
    s_dirty = 1;
    /* 同步硬件状态到菜单显示 */
    backlight_set(s_brightness);
    volume_get();  /* 确保 i2s 模块已初始化（触发生成音量变量） */
    ESP_LOGI(TAG, "Menu entered");
}

void menu_exit(void)
{
    menu_active = 0;
    emu_paused = 0;
    s_dirty = 1;
    ESP_LOGI(TAG, "Menu exited");
}

void menu_mark_dirty(void)
{
    s_dirty = 1;
}

/* ============================================================
 * OSD HUD 信息显示 — 屏幕四角小字叠加
 * ============================================================ */
static void hud_draw(void)
{
    if (!display_info_on || !g_framebuffer) return;

    char buf[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* 左上: time:xx */
    if (t) strftime(buf, sizeof(buf), "time:%H:%M", t);
    else snprintf(buf, sizeof(buf), "time:--:--");
    draw_string(2, 2, buf, COL_GRAY, 0x0000);

    /* 右上: battery:xx% */
    snprintf(buf, sizeof(buf), "battery:--%%");
    int bw = (int)strlen(buf) * 8;
    draw_string(LCD_WIDTH - bw - 2, 2, buf, COL_GRAY, 0x0000);

    /* 左下: image:xx */
    draw_string(2, LCD_HEIGHT - 18, "image:--", COL_GRAY, 0x0000);

    /* 右下: speed:xxMHz */
    snprintf(buf, sizeof(buf), "speed:--MHz");
    bw = (int)strlen(buf) * 8;
    draw_string(LCD_WIDTH - bw - 2, LCD_HEIGHT - 18, buf, COL_GRAY, 0x0000);
}

/* ============================================================
 * 切换 OSD 信息显示
 * ============================================================ */
void hud_toggle(void)
{
    display_info_on = !display_info_on;
    ESP_LOGI(TAG, "Display info %s", display_info_on ? "ON" : "OFF");
    if (!display_info_on && g_framebuffer) {
        /* 关闭 HUD：清除四角区域的黑色背景像素 */
        uint16_t *fb = (uint16_t *)g_framebuffer;
        /* 左上 time 区域：大约 12 字符 * 8 = 96 宽, 16 高 */
        for (int y = 0; y < 18; y++) {
            for (int x = 0; x < 100; x++) {
                if (x < LCD_WIDTH && y < LCD_HEIGHT)
                    fb[y * LCD_WIDTH + x] = 0;
            }
        }
        /* 右上 battery 区域 */
        int bw = (int)strlen("battery:--%") * 8;
        for (int y = 0; y < 18; y++) {
            for (int x = LCD_WIDTH - bw - 2; x < LCD_WIDTH; x++) {
                if (x >= 0 && x < LCD_WIDTH && y < LCD_HEIGHT)
                    fb[y * LCD_WIDTH + x] = 0;
            }
        }
        /* 左下 image 区域 */
        for (int y = LCD_HEIGHT - 18; y < LCD_HEIGHT; y++) {
            for (int x = 0; x < 100; x++) {
                if (x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
                    fb[y * LCD_WIDTH + x] = 0;
            }
        }
        /* 右下 speed 区域 */
        bw = (int)strlen("speed:--MHz") * 8;
        for (int y = LCD_HEIGHT - 18; y < LCD_HEIGHT; y++) {
            for (int x = LCD_WIDTH - bw - 2; x < LCD_WIDTH; x++) {
                if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT)
                    fb[y * LCD_WIDTH + x] = 0;
            }
        }
    }
}

void menu_tick(void)
{
    if (vk_active) {
        static int last_vk_active = 0;
        /* 刚进入 vk_active 时标记脏 */
        if (!last_vk_active) { s_dirty = 1; last_vk_active = 1; }
        /* 退出 vk_active 时标记脏 */
        if (last_vk_active && !vk_active) { s_dirty = 1; last_vk_active = 0; }
        vk_draw();
        return;
    }
    if (menu_active && s_dirty) {
        s_dirty = 0;
        menu_draw();
    }
    if (!menu_active && display_info_on) {
        hud_draw();
    }
}
