#define BUILD_ESP32
#define esp32p4

/* Note: sram_low is too small for full cpu_exec1 (~50KB overflow).
 * Hot helpers (try_jcc8, store8/16/32) already have IRAM_ATTR but
 * are disabled by TINY386_NO_IRAM.  Can selectively enable smaller
 * functions if profiling shows they're worth the IRAM cost. */
#define ESPDEBUG
//#define ESPPROFILE
#define BPP 16
// FULL_UPDATE removed: enables dirty-rect tracking for text mode.
// Only changed characters are redrawn, drastically reducing VGA render cost.
// No SWAPXY — landscape matches VGA output directly
#define SCALE_2_1
// SWAP_BYTEORDER_BPP16    // LCD uses little-endian RGB565 (set via RAMCTRL)
#define USE_LCD_ILI9341
#define LCD_WIDTH  320
#define LCD_HEIGHT 240
/* ---- ILI9341 引脚定义 (根据你的硬件修改) ---- */
#define SPI_HOST                SPI2_HOST
#define PIN_NUM_SPI_MOSI        GPIO_NUM_18
#define PIN_NUM_SPI_SCLK        GPIO_NUM_19
#define PIN_NUM_SPI_CS          GPIO_NUM_20
#define PIN_NUM_SPI_DC          GPIO_NUM_21
#define PIN_NUM_SPI_RST         GPIO_NUM_22
#define PIN_NUM_SPI_BL          GPIO_NUM_23

// PSRAM bump-allocator pool (guest RAM 6 MB + overhead, surface is separate)
#define PSRAM_ALLOC_LEN (int)(10 * 1024 * 1024)

// Override VGA retrace interval: 60fps = ~16667us (was 5000us = 200fps).
// The LCD panel refreshes at 60Hz, so rendering faster is wasted work.
#define RETRACE_INTERVAL_US 16667

// SD card: SDMMC 1-bit native mode via GPIO matrix (Slot 1)
// Pin mapping matches retro-go esp32-p4-devkit target
#define SDMMC_HOST_SLOT  SDMMC_HOST_SLOT_1
#define SDMMC_CLK        GPIO_NUM_7
#define SDMMC_CMD        GPIO_NUM_8
#define SDMMC_D0         GPIO_NUM_6
#define SDMMC_FREQ_KHZ   SDMMC_FREQ_HIGHSPEED

// I2C for backlight (CH422G I/O expander)

// I2S audio output for HT517 amplifier
// HT517 is an I2S DAC amplifier, no external codec config needed
// BCK=GPIO8, WS=GPIO3, DATA=GPIO18, MCLK unused
// Kept at 256: 512 caused bursty audio submission starving pc_step.
#define MIXER_BUF_LEN 256
#define I2S_MCLK GPIO_NUM_NC
#define I2S_BCLK GPIO_NUM_4
#define I2S_WS   GPIO_NUM_2
#define I2S_DOUT GPIO_NUM_3

#define RG_KEY_UP       GPIO_NUM_35
#define RG_KEY_DOWN     GPIO_NUM_39
#define RG_KEY_LEFT     GPIO_NUM_40
#define RG_KEY_RIGHT    GPIO_NUM_41
#define RG_KEY_START    GPIO_NUM_42
#define RG_KEY_SELECT   GPIO_NUM_43
#define RG_KEY_X        GPIO_NUM_46
#define RG_KEY_Y        GPIO_NUM_47
#define RG_KEY_A        GPIO_NUM_44
#define RG_KEY_B        GPIO_NUM_45
#define RG_BATTERY_KEY  GPIO_NUM_28