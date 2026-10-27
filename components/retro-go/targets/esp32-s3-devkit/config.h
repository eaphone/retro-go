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
/* Panel tuning notes (all values below come from the TK024F3036 vendor demo):
 *  0xC3 VRHS     0x15 = 4.6V.  0x1B / 0x1F -> brighter, more contrast.
 *  0xD0 PWCTRL1  0xA4,0xA1.  Second byte 0x71 (VDDS 0.8V) -> more contrast.
 *  0xBB VCOM     0x66.  Sweep 0x3C..0x66 to cancel a cool/warm cast.
 *  0xE0 / 0xE1   gamma +/- (14 points, V0..V63). Raising the middle values
 *                increases "punch"; keep both tables roughly symmetric.
 *  0x20 / 0x21   INVOFF / INVON. Use 0x21 if the picture is a photo negative.
 *  0x36 MADCTL   0x60 = MX+MV.  Use 0xE0 (adds MY) if the image is upside down.
 */
#define RG_SCREEN_INIT()                                                                                         \
    /* Full vendor sequence for TK024F3036 (ST7789S). Ported from the vendor demo */                             \
    /* ESP32S3_TK024F3036_LVGL_9.2.0/components/LCD_TK024F3036/LCD_TK024F3036.c.  */                             \
    /* The old sequence only set MADCTL/COLMOD, so VCOM, gate voltages, VRH/VDV  */                              \
    /* and both gamma tables stayed at their power-on defaults: dull, washed out. */                             \
    rg_task_delay(120);                                                                                          \
    ILI9341_CMD(0x11);                               /* Sleep out */                                             \
    rg_task_delay(120);                                                                                          \
    ILI9341_CMD(0x36, 0x60);                         /* MADCTL: MX=1,MV=1, RGB order (|0x08 = BGR) */            \
    ILI9341_CMD(0x3A, 0x55);                         /* COLMOD: 16bit RGB565 */                                  \
    ILI9341_CMD(0xB0, 0x00, 0xF0);                   /* RAMCTRL: big-endian RGB565 */                            \
    ILI9341_CMD(0xB2, 0x1F, 0x1F, 0x00, 0x33, 0x33); /* PORCTRL */                                               \
    ILI9341_CMD(0xB7, 0x12);                         /* GATE: VGH=12.54V, VGL=-8.23V */                          \
    ILI9341_CMD(0xBB, 0x66);                         /* VCOM */                                                  \
    ILI9341_CMD(0xC0, 0x2C);                         /* LCMCTRL */                                               \
    ILI9341_CMD(0xC2, 0x01);                         /* VDVVRHEN */                                              \
    ILI9341_CMD(0xC3, 0x15);                         /* VRHS 4.6V  <- contrast knob */                           \
    ILI9341_CMD(0xC4, 0x20);                         /* VDVS 0V */                                               \
    ILI9341_CMD(0xC6, 0x13);                         /* FRCTRL2 */                                               \
    ILI9341_CMD(0xD0, 0xA4, 0xA1);                   /* PWCTRL1    <- contrast knob */                           \
    ILI9341_CMD(0xD6, 0xA1);                         /* gate output GND while asleep */                          \
    ILI9341_CMD(0xE0, 0xF0, 0x06, 0x0D, 0x0B, 0x0A, 0x07, 0x2E, 0x43, 0x45, 0x38, 0x14, 0x13, 0x25, 0x29);  /* gamma + */ \
    ILI9341_CMD(0xE1, 0xF0, 0x07, 0x0A, 0x08, 0x07, 0x23, 0x2E, 0x33, 0x44, 0x3A, 0x16, 0x17, 0x26, 0x2C);  /* gamma - */ \
    ILI9341_CMD(0x20);                               /* INVOFF (use 0x21 for a negative image) */                \
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