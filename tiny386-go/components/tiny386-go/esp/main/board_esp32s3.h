#define BUILD_ESP32

#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR

#define ESPDEBUG
#define BPP 16
#define FULL_UPDATE
// No SWAPXY — 800x480 landscape matches VGA output directly
#define SCALE_2_1
// SWAP_BYTEORDER_BPP16    // LCD uses little-endian RGB565 (set via RAMCTRL)
#define USE_LCD_ILI9341
#define LCD_WIDTH  320
#define LCD_HEIGHT 240

// PSRAM bump-allocator pool (guest RAM 6 MB + overhead)
#define PSRAM_ALLOC_LEN (int)(6.5 * 1024 * 1024)

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
#define MIXER_BUF_LEN 256
