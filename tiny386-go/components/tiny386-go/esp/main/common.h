#ifndef COMMON_H
#define COMMON_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* Virtual display resolution (VGA renders at this size into framebuffer) */
#ifndef VIRT_WIDTH
#define VIRT_WIDTH  640
#endif
#ifndef VIRT_HEIGHT
#define VIRT_HEIGHT 400
#endif

struct Globals {
	void *pc;
	void *kbd;
	void *mouse;
	void *panel;
	void *panel_fb;   /* RGB panel DMA frame buffer (NULL if not used) */
};

/* Scroll/pan offsets for viewport into virtual framebuffer */
extern int g_scroll_x;
extern int g_scroll_y;
extern int scroll_x_max;
extern int scroll_y_max;
extern int scroll_active;

extern EventGroupHandle_t global_event_group;
extern struct Globals globals;

/* 背光控制（各 LCD 驱动实现） */
extern void backlight_set(int percent);

/* 音量控制（i2s.c 实现） */
extern void volume_set(int percent);
extern int volume_get(void);

#endif /* COMMON_H */

