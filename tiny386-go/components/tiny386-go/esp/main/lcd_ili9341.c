#ifdef USE_LCD_ILI9341

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "menu.h"
#include "image_lionstdio.h"
#include <rg_display.h>  /* For rg_surface_t, rg_display_submit() */
#include "esp_timer.h"   /* for esp_timer_get_time() profiling */

extern int vk_active;
extern int vk_need_refresh;
extern void vk_draw(void);
extern int emu_paused;
void pc_vga_step(void *o);
static const char *TAG = "lcd";
#define SPI_MAX_CHUNK_SIZE     (4096)  // 每次最多传输 4KB

/* ---- 显示参数 ---- */
#define LCD_WIDTH               (320)
#define LCD_HEIGHT              (240)
#define SCALE_2_1

/* ---- ILI9341 引脚定义 (根据你的硬件修改) ---- */
#define PIN_NUM_SPI_MOSI        (GPIO_NUM_6)
#define PIN_NUM_SPI_SCLK        (GPIO_NUM_5)
#define PIN_NUM_SPI_CS          (GPIO_NUM_15)
#define PIN_NUM_SPI_DC          (GPIO_NUM_7)
#define PIN_NUM_SPI_RST         (GPIO_NUM_2)
#define PIN_NUM_SPI_BL          (GPIO_NUM_4)

/* ---- SPI 配置 ---- */
#define SPI_HOST                (SPI2_HOST)
#define SPI_CLOCK_SPEED         (40 * 1000 * 1000)  // 40MHz

/* ---- 背光 PWM ---- */
#define LCD_LEDC_CH             1
#define LCD_LEDC_TIMER          1
#define LCD_LEDC_DUTY_RES       10

/* ---- ILI9341 命令 ---- */
#define ILI9341_SWRESET         0x01
#define ILI9341_SLEEP_OUT       0x11
#define ILI9341_DISPLAY_ON      0x29
#define ILI9341_DISPLAY_OFF     0x28
#define ILI9341_COLMOD          0x3A
#define ILI9341_MADCTL          0x36
#define ILI9341_CASET           0x2A
#define ILI9341_PASET           0x2B
#define ILI9341_RAMWR           0x2C
#define ILI9341_POWER_CTRL      0xC0
#define ILI9341_GAMMA_SEL       0x20
#define ILI9341_NORMAL_MODE     0x13

/* MADCTL 参数 */
#define MADCTL_MY               0x80
#define MADCTL_MX               0x40
#define MADCTL_MV               0x20
#define MADCTL_ML               0x10
#define MADCTL_BGR              0x08
#define MADCTL_MH               0x04

static spi_device_handle_t spi_handle = NULL;
static SemaphoreHandle_t spi_mutex = NULL;

/* ---- SPI 传输函数 ---- */
static void spi_transfer(const uint8_t *data, size_t len, bool is_data)
{
    if (!spi_handle) return;
    
    // 设置 DC 引脚：命令=低，数据=高
    gpio_set_level(PIN_NUM_SPI_DC, is_data ? 1 : 0);
    
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data,
        .rx_buffer = NULL,
    };
    
    esp_err_t ret = spi_device_transmit(spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
    }
}

static void lcd_write_cmd(uint8_t cmd)
{
    spi_transfer(&cmd, 1, false);
}

static void lcd_write_data(uint8_t data)
{
    spi_transfer(&data, 1, true);
}

static void lcd_write_data16(uint16_t data)
{
    uint8_t buf[2] = {data >> 8, data & 0xFF};
    spi_transfer(buf, 2, true);
}

/* ---- 设置显示窗口 ---- */
static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    lcd_write_cmd(ILI9341_CASET);  // 列地址
    lcd_write_data16(x1);
    lcd_write_data16(x2);
    
    lcd_write_cmd(ILI9341_PASET);  // 行地址
    lcd_write_data16(y1);
    lcd_write_data16(y2);
    
    lcd_write_cmd(ILI9341_RAMWR);  // 开始写入内存
}

/* ---- lcd_draw 函数：绘制矩形区域 ---- */
void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
    if (!spi_handle || !src) return;
    
    // 确保坐标在屏幕范围内
    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;
    if (x_end > LCD_WIDTH) x_end = LCD_WIDTH;
    if (y_end > LCD_HEIGHT) y_end = LCD_HEIGHT;
    if (x_start >= x_end || y_start >= y_end) return;
    
    int width = x_end - x_start;
    int height = y_end - y_start;
    int row_bytes = width * 2;            // 每行像素字节数 (BPP=16)
    int stride = LCD_WIDTH * 2;           // 源 framebuffer 行跨度（字节）
    
    // 取互斥锁
    if (spi_mutex) xSemaphoreTake(spi_mutex, portMAX_DELAY);
    
    // 设置窗口
    lcd_set_window(x_start, y_start, x_end - 1, y_end - 1);
    gpio_set_level(PIN_NUM_SPI_DC, 1);  // 数据模式
    
    /*
     * 逐行传输像素。
     *
     * ILI9341 在写入 RAMWR 后，GRAM 地址会在窗口内自动换列/换行，
     * 所以我们只需要逐行地向它发送连续的像素流。
     *
     * 但 framebuffer 的 stride (= LCD_WIDTH×2) 可能大于 window 的
     * 行字节数 (= width×2)，所以不能把整个矩形当作一块连续内存
     * 发送 —— 必须逐行处理，每行从正确的 framebuffer 偏移处读取。
     */
    uint8_t *base = (uint8_t *)src;
    for (int row = 0; row < height; row++) {
        uint8_t *line_start = base + (y_start + row) * stride + x_start * 2;
        size_t remaining = row_bytes;
        
        while (remaining > 0) {
            size_t chunk = (remaining < SPI_MAX_CHUNK_SIZE) ? remaining : SPI_MAX_CHUNK_SIZE;
            
            spi_transaction_t trans = {
                .length = chunk * 8,
                .tx_buffer = line_start,
                .rx_buffer = NULL,
                .flags = 0,
            };
            
            esp_err_t ret = spi_device_transmit(spi_handle, &trans);
            if (ret != ESP_OK) {
                static int err_count = 0;
                if (err_count++ % 100 == 0) {
                    ESP_LOGE(TAG, "SPI draw failed: %s", esp_err_to_name(ret));
                }
                if (spi_mutex) xSemaphoreGive(spi_mutex);
                return;
            }
            
            line_start += chunk;
            remaining -= chunk;
        }
    }
    
    if (spi_mutex) xSemaphoreGive(spi_mutex);
}

/* ---- 背光初始化 ---- */
static void backlight_init(void)
{
    ESP_LOGI(TAG, "Initialize backlight");
    
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    
    ledc_channel_config_t channel_cfg = {
        .gpio_num = PIN_NUM_SPI_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
}

void backlight_set(int percent)
{
    uint32_t duty = ((1 << LCD_LEDC_DUTY_RES) - 1) * percent / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH);
    ESP_LOGI(TAG, "Backlight: %d%%", percent);
}

/* ---- ILI9341 初始化 ---- */
static void lcd_init_hardware(void)
{
    ESP_LOGI(TAG, "Initializing ILI9341...");
    
    // 复位
    gpio_set_level(PIN_NUM_SPI_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_SPI_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    
    // 软件复位
    lcd_write_cmd(ILI9341_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));
    
    // 退出睡眠 (0x11)
    ESP_LOGI(TAG, "CMD: Sleep Out (0x11)");
    lcd_write_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // MADCTL (0x36) - from test.txt: 0xA0
    ESP_LOGI(TAG, "CMD: MADCTL (0x36) = 0xA0");
    lcd_write_cmd(0x36);
    lcd_write_data(0x60);  // MY=1, MX=1, MV=1, BGR=1 (180deg rotate, BGR order)

    // COLMOD (0x3A) - 16-bit RGB565
    ESP_LOGI(TAG, "CMD: COLMOD (0x3A) = 0x05");
    lcd_write_cmd(0x3A);
    lcd_write_data(0x05);

    // RAMCTRL (0xB0) - set little-endian RGB565
    ESP_LOGI(TAG, "CMD: RAMCTRL (0xB0) = 0x00, 0xF8");
    lcd_write_cmd(0xB0);
    lcd_write_data(0x00);
    lcd_write_data(0xF8);  // little-endian (bit 3=1)

    // Gamma settings removed (using ILI9341 defaults)

    // (negative gamma also removed)

    // Display Inversion On (0x21)
    //ESP_LOGI(TAG, "CMD: Display Inversion On (0x21)");
    //// lcd_write_cmd(0x21);

    // Display On (0x29)
    ESP_LOGI(TAG, "CMD: Display On (0x29)");
    lcd_write_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "ILI9341 initialization complete");
}

/* ---- SPI 初始化 ---- */
static void spi_init(void)
{
    spi_mutex = xSemaphoreCreateMutex();
    
	spi_bus_config_t buscfg = {
		.mosi_io_num = PIN_NUM_SPI_MOSI,
		.miso_io_num = -1,
		.sclk_io_num = PIN_NUM_SPI_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = SPI_MAX_CHUNK_SIZE,  // 限制为 4KB
	};
    
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_SPEED,
        .mode = 0,
        .spics_io_num = PIN_NUM_SPI_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST, &devcfg, &spi_handle));
    
    ESP_LOGI(TAG, "SPI initialized");
}

/* ---- 测试图案 ---- */
static void lcd_test_pattern(void)
{
    ESP_LOGI(TAG, "Drawing test pattern...");

    // 显示狮子图像 (160x155, 居中放置)
    int offset_x = (LCD_WIDTH - image_lionstdio.width) / 2;
    int offset_y = (LCD_HEIGHT - image_lionstdio.height) / 2;

    // 直接在 spi 中逐行传输图像像素，避免额外分配大缓冲区
    if (spi_mutex) xSemaphoreTake(spi_mutex, portMAX_DELAY);

    // 分三段绘制：上方空白 → 图像(含左右留空) → 下方空白
    uint16_t *blank_line = malloc(LCD_WIDTH * sizeof(uint16_t));
    if (blank_line) {
        memset(blank_line, 0, LCD_WIDTH * sizeof(uint16_t));

        // 1) 上方空白
        for (int y = 0; y < offset_y; y++) {
            lcd_set_window(0, y, LCD_WIDTH - 1, y);
            gpio_set_level(PIN_NUM_SPI_DC, 1);
            spi_transaction_t trans = {
                .length = LCD_WIDTH * 16,
                .tx_buffer = blank_line,
                .rx_buffer = NULL,
                .flags = 0,
            };
            spi_device_transmit(spi_handle, &trans);
        }

        // 2) 图像区域：每行 = 左留空 + 图像数据 + 右留空
        const uint8_t *img_data = image_lionstdio.pixel_data;
        int img_row_bytes = image_lionstdio.width * 2;

        for (int y = 0; y < image_lionstdio.height; y++) {
            lcd_set_window(0, offset_y + y, LCD_WIDTH - 1, offset_y + y);
            gpio_set_level(PIN_NUM_SPI_DC, 1);

            // 左侧空白
            size_t left_bytes = offset_x * 2;
            if (left_bytes > 0) {
                spi_transaction_t trans = {
                    .length = left_bytes * 8,
                    .tx_buffer = blank_line,
                    .rx_buffer = NULL,
                    .flags = 0,
                };
                spi_device_transmit(spi_handle, &trans);
            }

            // 图像行
            size_t remaining = img_row_bytes;
            const uint8_t *line_ptr = img_data + y * img_row_bytes;
            while (remaining > 0) {
                size_t chunk = (remaining < SPI_MAX_CHUNK_SIZE) ? remaining : SPI_MAX_CHUNK_SIZE;
                spi_transaction_t trans = {
                    .length = chunk * 8,
                    .tx_buffer = line_ptr,
                    .rx_buffer = NULL,
                    .flags = 0,
                };
                spi_device_transmit(spi_handle, &trans);
                line_ptr += chunk;
                remaining -= chunk;
            }

            // 右侧空白
            size_t right_bytes = (LCD_WIDTH - offset_x - image_lionstdio.width) * 2;
            if (right_bytes > 0) {
                spi_transaction_t trans = {
                    .length = right_bytes * 8,
                    .tx_buffer = blank_line,
                    .rx_buffer = NULL,
                    .flags = 0,
                };
                spi_device_transmit(spi_handle, &trans);
            }
        }

        // 3) 下方空白
        for (int y = offset_y + image_lionstdio.height; y < LCD_HEIGHT; y++) {
            lcd_set_window(0, y, LCD_WIDTH - 1, y);
            gpio_set_level(PIN_NUM_SPI_DC, 1);
            spi_transaction_t trans = {
                .length = LCD_WIDTH * 16,
                .tx_buffer = blank_line,
                .rx_buffer = NULL,
                .flags = 0,
            };
            spi_device_transmit(spi_handle, &trans);
        }

        free(blank_line);
    }

    if (spi_mutex) xSemaphoreGive(spi_mutex);
    ESP_LOGI(TAG, "Test pattern (lion image) drawn");
}

/* 清屏为黑色 */
static void lcd_clear_screen(void)
{
    if (!spi_handle) return;
    
    ESP_LOGI(TAG, "Clearing screen to black");
    
    // 分配一行缓冲区
    uint16_t *black_line = malloc(LCD_WIDTH * sizeof(uint16_t));
    if (!black_line) {
        ESP_LOGE(TAG, "Failed to allocate black line buffer");
        return;
    }
    memset(black_line, 0, LCD_WIDTH * sizeof(uint16_t));
    
    if (spi_mutex) xSemaphoreTake(spi_mutex, portMAX_DELAY);
    
    for (int y = 0; y < LCD_HEIGHT; y++) {
        lcd_set_window(0, y, LCD_WIDTH - 1, y);
        gpio_set_level(PIN_NUM_SPI_DC, 1);
        
        spi_transaction_t trans = {
            .length = LCD_WIDTH * 16,
            .tx_buffer = black_line,
            .rx_buffer = NULL,
            .flags = 0,
        };
        
        spi_device_transmit(spi_handle, &trans);
    }
    
    if (spi_mutex) xSemaphoreGive(spi_mutex);
    free(black_line);
    
    ESP_LOGI(TAG, "Screen cleared");
}

/* ---- VGA 主任务 (Retro-Go mode) ---- */
void vga_task(void *arg)
{
    int core_id = esp_cpu_get_core_id();
    fprintf(stderr, "vga runs on core %d\n", core_id);
    
    extern struct Globals globals;
    extern EventGroupHandle_t global_event_group;
    
    // 通知 PC 任务面板已就绪
    globals.panel = (void*)1;  // 非 NULL 表示面板已就绪
    xEventGroupSetBits(global_event_group, BIT1);
    
    // 等待 PC 模拟器初始化完成
    xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
    
        ESP_LOGI(TAG, "Starting VGA loop");
    
    extern uint8_t *g_framebuffer;
    extern rg_surface_t *rg_surf;  /* RG surface from tiny386_main.c */
    uint32_t frame = 0;
    
    while (1) {
        if (emu_paused) {
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }else{
#ifdef RETRO_GO
            int64_t _vt0 = esp_timer_get_time();
#endif
            pc_vga_step(globals.pc);
#ifdef RETRO_GO
            int64_t _vt1 = esp_timer_get_time();
            /* Flush VGA dirty-rect updates after each refresh cycle.
             * redraw() only marks dirty; actual submit happens here. */
            extern void rg_display_flush(void);
            rg_display_flush();
            int64_t _vt2 = esp_timer_get_time();
            {
                static int64_t t_vga = 0, t_flush = 0;
                static int vcnt = 0;
                t_vga += _vt1 - _vt0;
                t_flush += _vt2 - _vt1;
                vcnt++;
                if (vcnt >= 256) {
                    fprintf(stderr, "VGA: step=%lu flush=%lu us (avg %d calls)\n",
                        (unsigned long)(t_vga / vcnt),
                        (unsigned long)(t_flush / vcnt),
                        vcnt);
                    t_vga = 0;
                    t_flush = 0;
                    vcnt = 0;
                }
            }
#endif
            if (menu_active || vk_active) {
                menu_tick();
            }
        }
        
        frame++;
        /* Periodic full submit for menu/overlay updates that bypass redraw().
         * ~10Hz is enough for UI; lower than old 20Hz to save bandwidth. */
        if (frame % 10 == 0 && rg_surf) {
            rg_surf->offset = 0;
            rg_display_submit(rg_surf, 0);
        }
        if (frame % 100 == 0) {
            ESP_LOGD(TAG, "Frame %lu", frame);
        }
        
        vTaskDelay(pdMS_TO_TICKS(16));  /* ~60 fps */
    }
}

#endif