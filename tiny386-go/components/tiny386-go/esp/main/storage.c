/*
 * SD/SPI storage for tiny386.
 * 
 * DISABLED when RETRO_GO is defined, since retro-go handles
 * storage initialization (SD card, SPI flash, etc.) itself.
 */
#ifndef RETRO_GO

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "sdmmc_cmd.h"
#include "common.h"
#include "driver/gpio.h"
#include <string.h>
#include <sys/stat.h>
#include "wear_levelling.h"

static const char *TAG = "storage";
sdmmc_card_t *rawsd = NULL;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
#define TEST_SD_SPI_HOST 	SD_SPI_HOST    

/* ---- SD 卡传输回调 ---- */
static esp_err_t sdcard_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    esp_err_t ret = sdspi_host_do_transaction(slot, cmdinfo);
    if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "SD card transaction out of memory");
    }
    return ret;
}

void storage_init(void)
{
    bool sd_mount_ok = false;
    
    ESP_LOGI(TAG, "Initializing SD card on SPI%d...", TEST_SD_SPI_HOST);
    
    // 1. 初始化 SPI 总线（使用 DMA，SDSPI 需要）
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_SPI_MOSI,
        .miso_io_num = SD_SPI_MISO,
        .sclk_io_num = SD_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    esp_err_t ret = spi_bus_initialize(TEST_SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: 0x%x", ret);
        goto try_spiffs;
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPI bus initialized");
    } else {
        ESP_LOGI(TAG, "SPI bus already initialized");
    }
    
    // 2. 配置 CS 引脚（初始高电平）
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << SD_SPI_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_conf);
    gpio_set_level(SD_SPI_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 3. 配置 SDSPI 主机驱动
    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = TEST_SD_SPI_HOST;
    host_config.max_freq_khz = SD_SPI_FREQ_KHZ;
    host_config.do_transaction = sdcard_do_transaction;
    
    // 4. 配置 SDSPI 设备
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = TEST_SD_SPI_HOST;
    slot_config.gpio_cs = SD_SPI_CS;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    
    // 5. 挂载配置
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,
    };
    
    sdmmc_card_t *card = NULL;
    
        // 6. 尝试挂载（让 sdspi_host 自动处理初始化）
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host_config, &slot_config, &mount_config, &card);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Mount failed at %d kHz (0x%x), trying slower speed...", 
                 SD_SPI_FREQ_KHZ, err);
        
        // 降速前复位 SD 卡：发送至少 80 个时钟周期（CS=高），让卡状态机重置
        ESP_LOGI(TAG, "Resetting SD card for retry at lower speed...");
        if (card) {
            esp_vfs_fat_sdcard_unmount("/sdcard", card);
            card = NULL;
        }
        gpio_set_level(SD_SPI_CS, 1);
        /* 发送 80 个空闲时钟脉冲复位 SD 卡 */
        for (int i = 0; i < 80; i++) {
            gpio_set_level(SD_SPI_SCK, 0);
            esp_rom_delay_us(1);
            gpio_set_level(SD_SPI_SCK, 1);
            esp_rom_delay_us(1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;  // 400kHz
        err = esp_vfs_fat_sdspi_mount("/sdcard", &host_config, &slot_config, &mount_config, &card);
    }
    
    if (err == ESP_OK) {
        sd_mount_ok = true;
        rawsd = card;
        ESP_LOGI(TAG, "SD Card mounted successfully");
        
        if (card) {
            ESP_LOGI(TAG, "SD Card Info:");
            ESP_LOGI(TAG, "  Name: %s", card->cid.name);
            ESP_LOGI(TAG, "  Type: %s", (card->ocr) ? "SDHC" : "SDSC");
            ESP_LOGI(TAG, "  Size: %llu MB", (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
            ESP_LOGI(TAG, "  Speed: %d kHz", host_config.max_freq_khz);
        }
        } else {
        rawsd = NULL;
        ESP_LOGE(TAG, "SD Card mount failed with error: 0x%x", err);
        
        // 错误分析
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timeout error - possible causes:");
            ESP_LOGE(TAG, "  1. No SD card inserted");
            ESP_LOGE(TAG, "  2. SD card not formatted (needs FAT32)");
            ESP_LOGE(TAG, "  3. Wiring issue (check CS, MOSI, MISO, SCLK)");
            ESP_LOGE(TAG, "  4. Power issue (SD card needs 3.3V with ~200mA)");
        }
    }

        try_spiffs:
        // 如果 SD 卡挂载失败，尝试挂载 SPIFFS（尝试格式化）
    if (!sd_mount_ok) {
        ESP_LOGI(TAG, "SD card not available, mounting/formatting SPIFFS...");
        const esp_vfs_fat_mount_config_t spiflash_cfg = {
            .max_files = 4,
            .format_if_mount_failed = true,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
        };
        esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl("/spiflash", "storage",
                                                         &spiflash_cfg, &s_wl_handle);
                if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted successfully");
        } else {
            ESP_LOGW(TAG, "SPIFFS mount/format failed: 0x%x", ret);
        }
    }
}
#endif /* RETRO_GO */