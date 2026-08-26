/****************************************************************************
 * Target definition for ESP32-S3-DEVKIT                                    *
 ****************************************************************************/
#define RG_TARGET_NAME             "ESP32-S3-DEVKIT"


/****************************************************************************
 * Status LED                                                               *
 ****************************************************************************/
#define RG_LED_DRIVER               1   // 1 = GPIO
#define RG_GPIO_LED                 GPIO_NUM_NC
// #define RG_GPIO_LED_INVERT          // Uncomment if the LED is active LOW


/****************************************************************************
 * I2C / GPIO Extender                                                      *
 ****************************************************************************/
// #define RG_I2C_GPIO_DRIVER          0   // 1 = AW9523, 2 = PCF9539, 3 = MCP23017, 4 = PCF8575, 5 = PCF8574
// #define RG_I2C_GPIO_ADDR            0x00
// #define RG_GPIO_I2C_SDA             GPIO_NUM_15
// #define RG_GPIO_I2C_SCL             GPIO_NUM_4


/****************************************************************************
 * Storage                                                                  *
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_2
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_39
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_1
#define RG_GPIO_SDSPI_CS            GPIO_NUM_38
// #define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
// #define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_FLASH_PARTITION  "vfs"


/****************************************************************************
 * Audio                                                                    *
 ****************************************************************************/
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable
#define RG_AUDIO_USE_BUZZER_PIN     0   // See drivers/audio/buzzer.c for details
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_43
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_42
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_44
#define RG_GPIO_MIC_DATA            GPIO_NUM_41
#define RG_GPIO_MIC_CLK             GPIO_NUM_40
// #define RG_GPIO_SND_AMP_ENABLE      18

/****************************************************************************
 * Video                                                                    *
 ****************************************************************************/

// SPI Display (back up working)
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_16
#define RG_GPIO_LCD_CLK             GPIO_NUM_15
#define RG_GPIO_LCD_CS              GPIO_NUM_7
#define RG_GPIO_LCD_DC              GPIO_NUM_6
#define RG_GPIO_LCD_BCKL            GPIO_NUM_4
#define RG_GPIO_LCD_RST             GPIO_NUM_5

#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M // SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_INIT()                                                                                             \
    rg_task_delay(120);                                                                                              \
    /* Sleep Out (0x11) */                                                                                           \
    ILI9341_CMD(0x11);                                                                                               \
    rg_task_delay(120);                                                                                              \
    /* MADCTL (0x36) - MY=1, MX=1, MV=1, BGR=1 (180deg rotate, BGR order) */                                       \
    ILI9341_CMD(0x36, 0x60);                                                                                        \
    /* COLMOD (0x3A) - 16-bit RGB565 */                                                                             \
    ILI9341_CMD(0x3A, 0x05);                                                                                        \
    /* RAMCTRL (0xB0) - set big-endian RGB565 */                                                                 \
    ILI9341_CMD(0xB0, 0x00, 0xF0);                                                                                  \
    /* Gamma curve selected */                                                                                       \
    ILI9341_CMD(0x26, 0x01);                                                                                       \
    ILI9341_CMD(0x20, 0x00);                                                                                        \
    ILI9341_CMD(0x13, 0x00);                                                                                        \
    /* Power Control VRH[5:0] */                                                                                    \
    rg_task_delay(50);

/****************************************************************************
 * Input                                                                    *
 ****************************************************************************/
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
#define RG_GAMEPAD_ADC_MAP {\
}
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_UP, .num = GPIO_NUM_21, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN, .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_LEFT, .num = GPIO_NUM_47, .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT, .num = GPIO_NUM_48, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_10, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT,  .num = GPIO_NUM_0, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,      .num = GPIO_NUM_9, .pullup = 1, .level = 0},\
    {RG_KEY_OPTION,      .num = GPIO_NUM_8, .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_11, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_14, .pullup = 1, .level = 0},\
}

/****************************************************************************
 * Battery                                                                  *
 ****************************************************************************/
#define RG_BATTERY_DRIVER           1  
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_2
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_7
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3200.f) / (3900.f - 3200.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)

/****************************************************************************
 * Net                                                                      *
 ****************************************************************************/
#define RG_NET_SCK                  GPIO_NUM_13
#define RG_NET_CS                   GPIO_NUM_12
#define RG_NET_MOSI                 GPIO_NUM_46
#define RG_NET_MISO                 GPIO_NUM_3
#define RG_NET_HS                   GPIO_NUM_17
#define FRAME_BUFFER_SIZE           1024