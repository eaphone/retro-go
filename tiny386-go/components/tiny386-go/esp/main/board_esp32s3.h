#define BUILD_ESP32
#define esp32s3
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
/* ---- ILI9341 引脚定义 (根据你的硬件修改) ---- */
#define SPI_HOST                SPI2_HOST
#define PIN_NUM_SPI_MOSI        GPIO_NUM_6
#define PIN_NUM_SPI_SCLK        GPIO_NUM_5
#define PIN_NUM_SPI_CS          GPIO_NUM_15
#define PIN_NUM_SPI_DC          GPIO_NUM_7
#define PIN_NUM_SPI_RST         GPIO_NUM_2
#define PIN_NUM_SPI_BL          GPIO_NUM_4

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
#define LCD_I2C_SDA  GPIO_NUM_15
#define LCD_I2C_SCL  GPIO_NUM_16

// I2S audio output for HT517 amplifier
// HT517 is an I2S DAC amplifier, no external codec config needed
// BCK=GPIO8, WS=GPIO3, DATA=GPIO18, MCLK unused
#define I2S_MCLK I2S_GPIO_UNUSED
#define I2S_BCLK GPIO_NUM_8
#define I2S_WS   GPIO_NUM_3
#define I2S_DOUT GPIO_NUM_18
#define MIXER_BUF_LEN 256
#define PC_STEP_COUNT 512

#define GPIO_KEY_UP       GPIO_NUM_11
#define GPIO_KEY_DOWN     GPIO_NUM_12
#define GPIO_KEY_LEFT     GPIO_NUM_13
#define GPIO_KEY_RIGHT    GPIO_NUM_14
#define GPIO_KEY_START    GPIO_NUM_21
#define GPIO_KEY_SELECT   GPIO_NUM_47
#define GPIO_KEY_X        GPIO_NUM_41
#define GPIO_KEY_Y        GPIO_NUM_42
#define GPIO_KEY_A        GPIO_NUM_0
#define GPIO_KEY_B        GPIO_NUM_48
