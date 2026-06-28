#include "rg_system.h"
#include "rg_coplay.h"
#include <driver/spi_master.h>
#include <driver/spi_slave.h>
#include <driver/gpio.h>

#if defined(RG_ENABLE_NETPLAY) && defined(RG_NET_SPI_HOST)
#define CHUNK_SIZE 4092
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static spi_device_handle_t spi_handle;
static uint16_t *receive_buffer;
// 初始化主机 SPI
void rg_coplay_host_spi_init(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_NET_MOSI,
        .miso_io_num = RG_NET_MISO,
        .sclk_io_num = RG_NET_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BUFFER_SIZE,
    };
    
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  // 40 MHz
        .mode = 0,
        .spics_io_num = RG_NET_CS,
        .queue_size = 1,
        .cs_ena_pretrans = 1,
        .cs_ena_posttrans = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    
    spi_bus_initialize(RG_NET_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(RG_NET_SPI_HOST, &dev_cfg, &spi_handle);
    
    // 初始化握手信号 GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RG_NET_HS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(RG_NET_HS, 0);  // 初始低电平
}

// 发送一帧画面
void rg_coplay_send_frame(uint16_t *frame_buffer) {
    // 等待从机就绪（握手信号为高）
    // 注意：如果从机没有反馈，可以省略这个等待
    // while(gpio_get_level(RG_NET_HS) == 0) { vTaskDelay(1); }
    spi_transaction_t trans = {
        .length = FRAME_BUFFER_SIZE * 8,  // 位长度
        .tx_buffer = frame_buffer,
        .rx_buffer = NULL,
    };
    
    // 发送 CS 低电平选中从机
    gpio_set_level(RG_NET_HS, 1);  // 握手信号：开始发送
    
    spi_device_transmit(spi_handle, &trans);
    
    gpio_set_level(RG_NET_HS, 0);  // 发送完成
    RG_LOGI("send frame %d", frame_buffer[0]);
}

// 初始化从机 SPI
void rg_coplay_slave_spi_init(void) {
    receive_buffer = heap_caps_malloc(FRAME_BUFFER_SIZE, MALLOC_CAP_DMA);
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_NET_MOSI,
        .miso_io_num = RG_NET_MISO,
        .sclk_io_num = RG_NET_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    
    spi_slave_interface_config_t slave_cfg = {
        .spics_io_num = RG_NET_CS,
        .queue_size = 3,
        .mode = 0,
        .flags = 0,
    };
    
    spi_slave_initialize(RG_NET_SPI_HOST, &bus_cfg, &slave_cfg, SPI_DMA_CH_AUTO);
    
    // 初始化握手信号 GPIO（作为输出，通知主机已就绪）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RG_NET_HS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(RG_NET_HS, 1);  // 就绪信号：高电平表示可以接收
}

// 接收一帧画面（阻塞）
void rg_coplay_receive_frame(uint16_t *frame_buffer) {
     spi_slave_transaction_t trans = {
        .length = FRAME_BUFFER_SIZE * 8,
        .tx_buffer = NULL,
        .rx_buffer = frame_buffer,
    };

    // 降低握手信号表示正在接收
    gpio_set_level(RG_NET_HS, 0);
    
    spi_slave_transmit(RG_NET_SPI_HOST, &trans, portMAX_DELAY);
    
    // 接收完成，恢复就绪状态
    gpio_set_level(RG_NET_HS, 1);
    if(frame_buffer[0] != 0) {
        RG_LOGI("receive frame %d", frame_buffer[0]);
    }
    
    // 将接收到的画面显示到屏幕
    rg_display_write_rect(0,0,RG_SCREEN_WIDTH,RG_SCREEN_HEIGHT,0, dst, 0);
}
#endif