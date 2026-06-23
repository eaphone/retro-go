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

// PSRAM bump-allocator pool (guest RAM 6 MB + overhead, surface is separate)
#define PSRAM_ALLOC_LEN (int)(10 * 1024 * 1024)

// Override VGA retrace interval: 60fps = ~16667us (was 5000us = 200fps).
// The LCD panel refreshes at 60Hz, so rendering faster is wasted work.
#define RETRACE_INTERVAL_US 16667

// SD card: SPI mode via SPI3_HOST
#define SD_SPI_HOST SPI3_HOST
#define SD_SPI_MISO  GPIO_NUM_45
#define SD_SPI_MOSI  GPIO_NUM_39
#define SD_SPI_SCK   GPIO_NUM_38
#define SD_SPI_CS    GPIO_NUM_40
#define SD_SPI_FREQ_KHZ 20000

// I2C for backlight (CH422G I/O expander)

// I2S audio output for HT517 amplifier
// HT517 is an I2S DAC amplifier, no external codec config needed
// BCK=GPIO8, WS=GPIO3, DATA=GPIO18, MCLK unused
// Kept at 256: 512 caused bursty audio submission starving pc_step.
#define MIXER_BUF_LEN 256
