/* Configuration for the ESP32-P4 dev board
 * The GPIOs were chosen arbitrarily, but you can choose whatever you want thanks to the I/O MUX
 * command to build: python rg_tool.py --target esp32-p4-devkit build-img --no-networking
*/

/****************************************************************************
 * Target definition for ESP32-P4 Dev-Board                                 *
 ****************************************************************************/
#define RG_TARGET_NAME             "ESP32-P4-DEVKIT"
#define USE_ADC_DRIVER_NG       1   // 1 = Use new ADC driver, 0 = Use legacy ADC driver (not recommended)

/****************************************************************************
 * Status LED                                                               *
 ****************************************************************************/
#define RG_LED_DRIVER               1   // 1 = GPIO
#define RG_GPIO_LED                 GPIO_NUM_NC
// #define RG_GPIO_LED_INVERT          // Uncomment if the LED is active LOW


/****************************************************************************
 * I2C / GPIO Extender                                                      *
 ****************************************************************************/
// #define RG_I2C_GPIO_DRIVER          0   // 1 = AW9523, 2 = PCF9539, 3 = MCP23017, 4 = PCF8575
// #define RG_I2C_GPIO_ADDR            0x00
// #define RG_GPIO_I2C_SDA             GPIO_NUM_15
// #define RG_GPIO_I2C_SCL             GPIO_NUM_4


/****************************************************************************
 * Storage                                                                  *
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED
#define RG_GPIO_SDMMC_CLK           GPIO_NUM_7
#define RG_GPIO_SDMMC_CMD	        GPIO_NUM_8
#define RG_GPIO_SDMMC_D0	        GPIO_NUM_6
#define RG_GPIO_SDMMC_D1	        GPIO_NUM_5
#define RG_GPIO_SDMMC_D2	        GPIO_NUM_10
#define RG_GPIO_SDMMC_D3	        GPIO_NUM_9
// #define RG_STORAGE_FLASH_PARTITION  "vfs"


/****************************************************************************
 * Audio                                                                    *
 ****************************************************************************/
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_4
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_2
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_3
#define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_NC
#define RG_GPIO_MIC_DATA            GPIO_NUM_54
#define RG_GPIO_MIC_CLK             GPIO_NUM_53
// #define RG_GPIO_SND_AMP_ENABLE_INVERT // Uncomment if the mute = HIGH


/****************************************************************************
 * Video                                                                    *
 ****************************************************************************/
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_PIXEL_FORMAT      0   // Possible values are 0=565_BE, 1=565_LE
#define RG_SCREEN_HOST              SPI3_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M // SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
//#define RG_SCREEN_PARTIAL_UPDATES   1
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

#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_18
#define RG_GPIO_LCD_CLK             GPIO_NUM_19
#define RG_GPIO_LCD_CS              GPIO_NUM_20
#define RG_GPIO_LCD_DC              GPIO_NUM_21
#define RG_GPIO_LCD_RST             GPIO_NUM_22
#define RG_GPIO_LCD_BCKL            GPIO_NUM_23
// #define RG_GPIO_LCD_BCKL_INVERT  // Uncomment if the LED is active LOW


/****************************************************************************
 * Input                                                                    *
 ****************************************************************************/
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_LEFT,   .num = GPIO_NUM_40, .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_41, .pullup = 1, .level = 0},\
    {RG_KEY_UP,     .num = GPIO_NUM_35, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_39, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_43, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_42, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,   .num = GPIO_NUM_NC, .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_44, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_X,      .num = GPIO_NUM_46, .pullup = 1, .level = 0},\
    {RG_KEY_Y,      .num = GPIO_NUM_47, .pullup = 1, .level = 0},\
    {RG_KEY_L,      .num = GPIO_NUM_NC, .pullup = 1, .level = 0},\
    {RG_KEY_R,      .num = GPIO_NUM_NC,  .pullup = 1, .level = 0},\
}


/****************************************************************************
 * Battery                                                                  *
 ****************************************************************************/
#define RG_BATTERY_DRIVER           1  
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_2
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_3
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3200.f) / (3900.f - 3200.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)


/****************************************************************************
 * Updater                                                                  *
 ****************************************************************************/
// #define RG_UPDATER_ENABLE               1
// #define RG_UPDATER_APPLICATION          RG_APP_FACTORY
// #define RG_UPDATER_DOWNLOAD_LOCATION    RG_STORAGE_ROOT "/odroid/firmware"



/****************************************************************************
 * Miscellaneous                                                            *
 ****************************************************************************/
#define RG_RECOVERY_BTN                 RG_KEY_MENU // Keep this button pressed to open the recovery menu

#define RG_CUSTOM_PLATFORM_INIT() \
    /* Arbitrary code executed very early during retro-go init */

/****************************************************************************
 * Net                                                                      *
 ****************************************************************************/
#define RG_NET_SCK                  GPIO_NUM_38
#define RG_NET_CS                   GPIO_NUM_37
#define RG_NET_MOSI                 GPIO_NUM_25
#define RG_NET_MISO                 GPIO_NUM_24
#define RG_NET_HS                   GPIO_NUM_17
#define FRAME_BUFFER_SIZE           1024
// See components/retro-go/config.h for more things you can define here!