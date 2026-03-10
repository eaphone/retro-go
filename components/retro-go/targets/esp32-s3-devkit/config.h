// Target definition
#define RG_TARGET_NAME             "ESP32-S3-DEVKIT"

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
// #define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_FLASH_PARTITION  "vfs"

// Audio
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M // SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_INIT()                                                                                            \
    ILI9341_CMD(0x11, 0x00);                                                                                        \
    rg_usleep(120 * 1000);                                                                                          \
    ILI9341_CMD(0x36, 0x60);                                                                                        \
    ILI9341_CMD(0x3A, 0x05);                                                                                        \
    ILI9341_CMD(0xC0, 0x28);                 /* Power control   //VRH[5:0] */                                       \
    ILI9341_CMD(0x20, 0x00);                 /* Gamma curve selected */                                             \
    ILI9341_CMD(0x13, 0x00);                 /* Gamma curve selected */                                             \
    ILI9341_CMD(0x29, 0x00);                                                                                        \
    rg_usleep(50 * 1000);                                                                                           \


// Input
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
#define RG_GAMEPAD_ADC_MAP {\
}
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_UP, .num = GPIO_NUM_11, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN, .num = GPIO_NUM_12, .pullup = 1, .level = 0},\
    {RG_KEY_LEFT, .num = GPIO_NUM_13, .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT, .num = GPIO_NUM_14, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_21, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT,  .num = GPIO_NUM_47, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,      .num = GPIO_NUM_41, .pullup = 1, .level = 0},\
    {RG_KEY_OPTION,      .num = GPIO_NUM_42, .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_0, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_48, .pullup = 1, .level = 0},\
}

// Battery
#define RG_BATTERY_DRIVER           1  
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_2
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_5
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 2500.f) / (4200.f - 2500.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)


// Status LED
#define RG_GPIO_LED                 GPIO_NUM_NC

// SPI Display (back up working)
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_6
#define RG_GPIO_LCD_CLK             GPIO_NUM_5
#define RG_GPIO_LCD_CS              GPIO_NUM_15
#define RG_GPIO_LCD_DC              GPIO_NUM_7
#define RG_GPIO_LCD_BCKL            GPIO_NUM_4
#define RG_GPIO_LCD_RST             GPIO_NUM_2

#define RG_GPIO_SDSPI_MISO          GPIO_NUM_45
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_39
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_38
#define RG_GPIO_SDSPI_CS            GPIO_NUM_40

// External I2S DAC
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_8
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_3
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_18
// #define RG_GPIO_SND_AMP_ENABLE      18


