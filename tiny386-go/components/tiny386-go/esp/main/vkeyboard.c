#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../../i8042.h"
#include "../../pc.h"
#include "../../i386.h"
#include "common.h"
#include "menu.h"

static const char *TAG = "vkeyboard";

extern uint8_t *g_framebuffer;
extern struct Globals globals;

/* ============================================================
 * 绘图辅助
 * ============================================================ */
static inline void vk_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    if (!g_framebuffer) return;
    ((uint16_t *)g_framebuffer)[y * LCD_WIDTH + x] = color;
}

static void vk_fill(int x, int y, int w, int h, uint16_t color)
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

extern const uint8_t vgafont16[256 * 16];

#define COL_TRANSPARENT 0xFFFF  /* 标记透明 (不会被实际使用到) */

static void vk_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 0 || ch > 127) ch = ' ';
    const uint8_t *glyph = &vgafont16[(unsigned char)ch * 16];
    int transparent = (bg == COL_TRANSPARENT);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col, py = y + row;
            if (px < 0 || px >= LCD_WIDTH || py < 0 || py >= LCD_HEIGHT) continue;
            if (bits & (0x80 >> col))
                vk_pixel(px, py, fg);
            /* 透明模式：不绘制背景像素，保留底下内容 */
        }
    }
}

/* ============================================================
 * 虚拟键盘参数
 * ============================================================ */
#define VK_W     160           /* 宽度 160 */
#define VK_H     80            /* 高度 80 */
#define VK_COLS  12            /* 每行 12 个字符 */
#define VK_ROWS  4

/* ============================================================
 * 键盘布局
 * ============================================================ */
static const char s_lower[VK_ROWS][VK_COLS + 1] = {
    "1234567890-=",
    "qwertyuiop[]",
    "asdfghjkl;'.",
    "zxcvbnm,./\x08"   /* 最后一个字符 0x08 = ← (退格) */
};

static const char s_upper[VK_ROWS][VK_COLS + 1] = {
    "!@#$%^&*()_+",
    "QWERTYUIOP{}",
    "ASDFGHJKL:\".",
    "ZXCVBNM<>? \x08"   /* 退格保持为 ←，前一个位置用空格占位（实际布局 12 字符） */
};

/* 每行固定 12 个字符 */

/* ============================================================
 * PS/2 scancode 映射
 * ============================================================ */
static uint8_t char_to_scancode(char ch)
{
    switch (ch) {
    case '1': return 0x02; case '2': return 0x03; case '3': return 0x04;
    case '4': return 0x05; case '5': return 0x06; case '6': return 0x07;
    case '7': return 0x08; case '8': return 0x09; case '9': return 0x0a;
    case '0': return 0x0b;
    case '-': return 0x0c; case '=': return 0x0d;
    case 'q': case 'Q': return 0x10;
    case 'w': case 'W': return 0x11;
    case 'e': case 'E': return 0x12;
    case 'r': case 'R': return 0x13;
    case 't': case 'T': return 0x14;
    case 'y': case 'Y': return 0x15;
    case 'u': case 'U': return 0x16;
    case 'i': case 'I': return 0x17;
    case 'o': case 'O': return 0x18;
    case 'p': case 'P': return 0x19;
    case '[': return 0x1a; case ']': return 0x1b;
    case 'a': case 'A': return 0x1e;
    case 's': case 'S': return 0x1f;
    case 'd': case 'D': return 0x20;
    case 'f': case 'F': return 0x21;
    case 'g': case 'G': return 0x22;
    case 'h': case 'H': return 0x23;
    case 'j': case 'J': return 0x24;
    case 'k': case 'K': return 0x25;
    case 'l': case 'L': return 0x26;
    case ';': return 0x27; case '\'': return 0x28;
    case 'z': case 'Z': return 0x2c;
    case 'x': case 'X': return 0x2d;
    case 'c': case 'C': return 0x2e;
    case 'v': case 'V': return 0x2f;
    case 'b': case 'B': return 0x30;
    case 'n': case 'N': return 0x31;
    case 'm': case 'M': return 0x32;
    case ',': return 0x33; case '.': return 0x34; case '/': return 0x35;
    case '!': return 0x02; case '@': return 0x03; case '#': return 0x04;
    case '$': return 0x05; case '%': return 0x06; case '^': return 0x07;
    case '&': return 0x08; case '*': return 0x09; case '(': return 0x0a;
    case ')': return 0x0b; case '_': return 0x0c; case '+': return 0x0d;
    case '{': return 0x1a; case '}': return 0x1b;
    case ':': return 0x27; case '"': return 0x28;
    case '<': return 0x33; case '>': return 0x34; case '?': return 0x35;
    /* 0x08 是退格键码，映射为 Backspace */
    default: return 0;
    }
}

static int needs_shift(char ch)
{
    const char *shift_chars = "!@#$%^&*()_+{}|:\"<>?";
    return strchr(shift_chars, ch) != NULL;
}

/* ============================================================
 * 虚拟键盘状态
 * ============================================================ */
int vk_active = 0;
int vk_need_refresh = 0;  /* 1 = LCD 需要刷新（input 变化时设置） */
static int vk_shift = 0;
static int vk_cx = 0;
static int vk_cy = 0;

/* ============================================================
 * 绘制虚拟键盘
 * ============================================================ */
void vk_draw(void)
{
    if (!vk_active || !g_framebuffer) return;

    int pan_x = (LCD_WIDTH - VK_W) / 2;
    int pan_y = LCD_HEIGHT - VK_H - 20;
    if (pan_x < 0) pan_x = 0;
    if (pan_y < 0) pan_y = 0;

    const uint16_t COL_BG  = 0x2945;  /* 半透明 30% 灰色 (RGB565: 00101 001010 00101) 用于背景和选中 */
    const uint16_t COL_W   = 0xFFFF;
    const uint16_t COL_SEL = 0x07E0;
    const uint16_t COL_DIM = 0xAD55;

    /* 背景（半透明） */
    vk_fill(pan_x, pan_y, VK_W, VK_H, COL_BG);
    /* 边框 */
    vk_fill(pan_x, pan_y,     VK_W, 1, COL_DIM);
    vk_fill(pan_x, pan_y + VK_H - 1, VK_W, 1, COL_DIM);
    vk_fill(pan_x, pan_y,     1, VK_H, COL_DIM);
    vk_fill(pan_x + VK_W - 1, pan_y, 1, VK_H, COL_DIM);

    /* 计算字符布局：每行 12 字符，宽度 320，水平均匀分布 */
    int key_area_w = VK_W - 12;      /* 左右各留 6px 边距 */
    int step_x     = key_area_w / VK_COLS;  /* 每个字符占用宽度 */
    int key_x0     = pan_x + 6;            /* 第一个字符的 x 起始 */
    int key_y0     = pan_y + 4;            /* 第一行 y 起始 */
    int step_y     = (VK_H - 8) / VK_ROWS; /* 每行高度 */

    for (int row = 0; row < VK_ROWS; row++) {
        const char *keys = vk_shift ? s_upper[row] : s_lower[row];
        int n = VK_COLS;
        int x = key_x0;
        int y = key_y0 + row * step_y;

        for (int col = 0; col < n; col++) {
            int sel = (row == vk_cy && col == vk_cx);
            uint16_t fg = sel ? COL_SEL : COL_W;
            uint16_t bg = sel ? COL_BG : COL_TRANSPARENT;

            if (sel) {
                /* 选中高亮：填充半透明背景 */
                for (int yy = 0; yy < 16; yy++)
                    vk_fill(x, y + yy, 8, 1, bg);
            }

            vk_char(x, y, keys[col], fg, bg);
            x += step_x;
        }
    }
}

/* ============================================================
 * 发送按键
 * ============================================================ */
static void vk_send_char(char ch)
{
    if (!globals.kbd) {
        ESP_LOGE(TAG, "vk_send_char: kbd NULL");
        return;
    }

    if (ch == 0x08) {
        ps2_put_keycode((PS2KbdState *)globals.kbd, 1, 0x0e);
        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, 0x0e);
        goto trigger_irq;
    }

    uint8_t sc = char_to_scancode(ch);
    if (sc == 0) {
        ESP_LOGE(TAG, "vk_send_char: no scancode for '%c'", ch);
        return;
    }

    ESP_LOGI(TAG, "vk_send_char: '%c' sc=0x%02x", ch >= 32 ? ch : '?', sc);

    int need_shift = (ch >= 'A' && ch <= 'Z') || needs_shift(ch);
    if (need_shift)
        ps2_put_keycode((PS2KbdState *)globals.kbd, 1, 0x2a);

    ps2_put_keycode((PS2KbdState *)globals.kbd, 1, sc);
    ps2_put_keycode((PS2KbdState *)globals.kbd, 0, sc);

    if (need_shift)
        ps2_put_keycode((PS2KbdState *)globals.kbd, 0, 0x2a);

trigger_irq:
    /* 直接调用 cpui386_raise_irq 设置 CPU intr 标志 */
    {
        PC *pc = (PC *)globals.pc;
        cpui386_raise_irq((CPUI386 *)pc->cpu);
        ESP_LOGI(TAG, "vk_send_char: raised IRQ");
    }
}

/* ============================================================
 * 输入处理
 * ============================================================ */
int vk_handle_input(int key)
{
    if (!vk_active) return 0;

    switch (key) {
    case 'U':
        if (vk_cy > 0) { vk_cy--; if (vk_cx >= VK_COLS) vk_cx = VK_COLS - 1; vk_need_refresh = 1; }
        return 1;
    case 'D':
        if (vk_cy < VK_ROWS - 1) { vk_cy++; if (vk_cx >= VK_COLS) vk_cx = VK_COLS - 1; vk_need_refresh = 1; }
        return 1;
    case 'L':
        if (vk_cx > 0) { vk_cx--; vk_need_refresh = 1; }
        return 1;
    case 'R':
        if (vk_cx < VK_COLS - 1) { vk_cx++; vk_need_refresh = 1; }
        return 1;
    case 'A': {
        const char *keys = vk_shift ? s_upper[vk_cy] : s_lower[vk_cy];
        if (vk_cx < VK_COLS) {
            vk_send_char(keys[vk_cx]);
            vk_need_refresh = 1;
        }
        return 1;
    }
    case 'B':
        vk_active = 0;
        vk_need_refresh = 1;
        return 1;
    case 'S':
        vk_shift = !vk_shift;
        vk_need_refresh = 1;
        return 1;
    default:
        return 0;
    }
}

/* ============================================================
 * 进入/退出
 * ============================================================ */
void vk_enter(void)
{
    vk_active = 1;
    vk_need_refresh = 1;
    vk_shift = 0;
    vk_cx = 0;
    vk_cy = 0;
    ESP_LOGI(TAG, "Virtual keyboard entered");
}

void vk_exit(void)
{
    vk_active = 0;
    ESP_LOGI(TAG, "Virtual keyboard exited");
}
