/*
 * SD/SPI storage for tiny386.
 * 
 * When RETRO_GO is defined, the SD card is managed by retro-go's
 * rg_storage subsystem. In that case, storage_init() acquires the
 * SD card handle from retro-go rather than initializing the card
 * from scratch (which would conflict with retro-go's own mount).
 */

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

/* ---- Native tiny386 storage_init (used when NOT built under retro-go) ---- */
#ifndef RETRO_GO

void storage_init(void)
{
    bool sd_mount_ok = false;
    sdmmc_card_t *card = NULL;
    
#ifdef esp32s3

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
    host_config.slot = SD_SPI_HOST;
    host_config.max_freq_khz = SD_SPI_FREQ_KHZ;
    host_config.do_transaction = sdcard_do_transaction;
    
    // 4. 配置 SDSPI 设备
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SD_SPI_HOST;
    slot_config.gpio_cs = SD_SPI_CS;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    
    // 5. 挂载配置
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,
    };
    
        // 6. 尝试挂载（让 sdspi_host 自动处理初始化）
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sd", &host_config, &slot_config, &mount_config, &card);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Mount failed at %d kHz (0x%x), trying slower speed...", 
                 SD_SPI_FREQ_KHZ, err);
        
        // 降速前复位 SD 卡：发送至少 80 个时钟周期（CS=高），让卡状态机重置
        ESP_LOGI(TAG, "Resetting SD card for retry at lower speed...");
        if (card) {
            esp_vfs_fat_sdcard_unmount("/sd", card);
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
        err = esp_vfs_fat_sdspi_mount("/sd", &host_config, &slot_config, &mount_config, &card);
    }
#else
    ESP_LOGI(TAG, "Initializing SD card via SDMMC Slot %d...", SDMMC_HOST_SLOT);
    
    // 1. Configure SDMMC host (native mode, matching retro-go esp32-p4-devkit)
    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.slot = SDMMC_HOST_SLOT;
    host_config.max_freq_khz = SDMMC_FREQ_KHZ;

    // 2. Configure SDMMC slot with GPIO matrix pins
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit mode
    slot_config.clk = SDMMC_CLK;
    slot_config.cmd = SDMMC_CMD;
    slot_config.d0  = SDMMC_D0;
    // d1-d3 not used in 1-bit mode, set to -1
    slot_config.d1 = slot_config.d2 = slot_config.d3 = GPIO_NUM_NC;

    // 3. FAT mount config
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,
    };
    
    // 4. Mount SD card
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host_config, &slot_config, &mount_config, &card);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Mount failed at %d kHz (0x%x), retrying at lower speed...",
                 SDMMC_FREQ_KHZ, err);

        /* If a partially-initialized card was returned, clean it up first */
        if (card) {
            esp_vfs_fat_sdcard_unmount("/sd", card);
            card = NULL;
        }

        host_config.max_freq_khz = SDMMC_FREQ_PROBING;  // 400kHz
        err = esp_vfs_fat_sdmmc_mount("/sd", &host_config, &slot_config, &mount_config, &card);
    }
#endif

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

/* ---- RETRO_GO version: obtain SD card handle from retro-go ---- */
#ifdef RETRO_GO
#include "rg_system.h"
#include <rg_storage.h>

void storage_init(void)
{
    ESP_LOGI(TAG, "Obtaining SD card handle from retro-go...");
    rawsd = (sdmmc_card_t *)rg_storage_get_card_handle();
    if (rawsd) {
        ESP_LOGI(TAG, "SD card handle acquired: %p", rawsd);
        if (rawsd) {
            ESP_LOGI(TAG, "SD Card Info:");
            ESP_LOGI(TAG, "  Name: %s", rawsd->cid.name);
            ESP_LOGI(TAG, "  Type: %s", (rawsd->ocr) ? "SDHC" : "SDSC");
            ESP_LOGI(TAG, "  Size: %llu MB", (uint64_t)rawsd->csd.capacity * rawsd->csd.sector_size / (1024 * 1024));
        }
    } else {
        ESP_LOGW(TAG, "No SD card handle available from retro-go");
    }
}
#endif /* RETRO_GO */