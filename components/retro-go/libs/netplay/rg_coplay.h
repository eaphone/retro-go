#pragma once

#include <stdint.h>

void rg_coplay_host_spi_init(void);
void rg_coplay_slave_spi_init(void);
void rg_coplay_send_frame(uint16_t *frame_buffer);
void rg_coplay_receive_frame(uint16_t *frame_buffer);
uint16_t* rg_coplay_get_receive_buffer(void);