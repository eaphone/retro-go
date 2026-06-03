/*
 * tiny386 - adapted for Retro-Go integration
 * 
 * This is the adapted esp_main.c from tiny386, modified to work
 * as a library called from retro-go's app_main.
 *
 * Changes from original:
 *  - Renamed app_main() -> tiny386_start()
 *  - Removed rg_system_set_overclock() (retro-go provides its own)
 *  - Uses retro-go's display driver instead of the native LCD drivers
 *  - Uses retro-go's audio system instead of direct I2S
 *  - Uses retro-go's input system (rg_input_read_gamepad) instead of direct GPIO polling
 *  - Reads config from the path passed in by retro-go
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "esp_log.h"
#include "rg_utils.h"
#include "rg_surface.h"
#include <rg_input.h>

#include "../../ini.h"
#include "../../pc.h"
#include "common.h"
#include "menu.h"

//
static const char *TAG = "tiny386";
#include "esp_private/system_internal.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "esp_rom_sys.h"

extern uint64_t esp_rtc_get_time_us(void);

uint32_t get_uticks()
{
	return esp_system_get_time();
}

void *psmalloc(long size);
void *fbmalloc(long size);
void *bigmalloc(size_t size)
{
	return psmalloc(size);
}

static char *pcram;
static long pcram_off;
static long pcram_len;
void *pcmalloc(long size)
{
	void *ret = pcram + pcram_off;

	size = (size + 31) / 32 * 32;
	if (pcram_off + size > pcram_len) {
		fprintf(stderr, "pcram error %ld %ld %ld\n", size, pcram_off, pcram_len);
		abort();
	}
	pcram_off += size;
	return ret;
}

void pcmalloc_init(void *ptr, long len)
{
	pcram = ptr;
	pcram_len = len;
}

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	if (file && file[0] == '/') {
		FILE *fp = fopen(file, "rb");
		assert(fp);
		fseek(fp, 0, SEEK_END);
		int len = ftell(fp);
		fprintf(stderr, "%s len %d\n", file, len);
		rewind(fp);
		if (backward)
			fread(phys_mem + addr - len, 1, len, fp);
		else
			fread(phys_mem + addr, 1, len, fp);
		fclose(fp);
		return len;
	}
	const esp_partition_t *part =
		esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					 ESP_PARTITION_SUBTYPE_ANY,
					 file);
	if (!part) {
		fprintf(stderr, "ERROR: partition '%s' not found\n", file);
		return 0;
	}
	int len = part->size;
	fprintf(stderr, "%s len %d\n", file, len);
	esp_err_t err;
	if (backward) {
		if (addr < (uword)len) {
			fprintf(stderr, "ERROR: addr 0x%lx < len %d for backward load\n",
				(unsigned long)addr, len);
			return 0;
		}
		err = esp_partition_read(part, 0, phys_mem + addr - len, len);
	} else {
		err = esp_partition_read(part, 0, phys_mem + addr, len);
	}
	if (err != ESP_OK) {
		fprintf(stderr, "ERROR: esp_partition_read failed: 0x%x\n", err);
		return 0;
	}
	return len;
}

//
EventGroupHandle_t global_event_group;
struct Globals globals;

/* Viewport scroll offset for text mode */
int g_scroll_x = 0;
int g_scroll_y = 0;
int scroll_x_max = 0;
int scroll_y_max = 0;
int scroll_active = 0;
int emu_paused = 0;

typedef struct {
	PC *pc;
	u8 *fb;
} Console;

uint8_t *g_framebuffer = NULL;

/* RG display surface for retro-go integration */
rg_surface_t *rg_surf = NULL;

Console *console_init(int width, int height)
{
    Console *c = malloc(sizeof(Console));
    memset(c, 0, sizeof(Console));
    
    /* Use retro-go surface for display */
    if (!rg_surf) {
        rg_surf = rg_surface_create(width, height, RG_PIXEL_565_LE, MEM_SLOW|MEM_32BIT);
        ESP_LOGI(TAG, "Created RG surface: %dx%d at %p", width, height, rg_surf->data);
    }
    c->fb = (uint8_t *)rg_surf->data;
    g_framebuffer = c->fb;
    
    return c;
}

/* This is called by VGA emulator to draw dirty rectangles */
static void redraw(void *opaque, int x, int y, int w, int h)
{
    Console *s = opaque;
    if (!s || !s->fb) return;
    
    /* Clip to screen */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    /* Submit the whole dirty region to retro-go display */
    if (rg_surf) {
        rg_surf->offset = 0; /* Full framebuffer */
        rg_display_submit(rg_surf, 0);
    }
}

void *esp_psram_get(size_t *size);
void vga_task(void *arg);
void wifi_main(const char *, const char *);
void storage_init(void);
void input_init(void);
void input_process(void);  /* New retro-go based input processing */
void i2s_main(void);

static int pc_main(const char *file)
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	int err = ini_parse(file, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "FATAL: ini_parse('%s') error %d\n", file, err);
		return err;
	}

	if (conf.bios == NULL && conf.linuxstart == NULL) {
		fprintf(stderr, "FATAL: no BIOS/linuxstart configured in '%s'\n", file);
		return -1;
	}

	if (conf.width != LCD_WIDTH || conf.height != LCD_HEIGHT) {
		fprintf(stderr, "fixing width/height mismatch %dx%d => %dx%d\n",
			conf.width, conf.height, LCD_WIDTH, LCD_HEIGHT);
		conf.width = LCD_WIDTH;
		conf.height = LCD_HEIGHT;
	}

	Console *console = console_init(conf.width, conf.height);
	PC *pc = pc_new(redraw, console, console->fb, &conf);
	console->pc = pc;
	globals.pc = pc;
	globals.kbd = pc->kbd;
	globals.mouse = pc->mouse;

	/* Initialize input using retro-go gamepad API (not GPIO) */
	input_init();

	/* Initialize OSD menu */
	menu_init();
	
	load_bios_and_reset(pc);

	/* Signal vga_task that PC is fully initialized (BIOS loaded, reset done) */
	xEventGroupSetBits(global_event_group, BIT0);

	/* Initialize scroll limits based on LCD and text mode sizes.
	 * scroll_active will be set to 1 when VGA text mode is detected
	 * (in vga.c).  We pre-set the max limits here. */
	scroll_x_max = VIRT_WIDTH - LCD_WIDTH;
	scroll_y_max = VIRT_HEIGHT - LCD_HEIGHT;

	pc->boot_start_time = get_uticks();
	for (; pc->shutdown_state != 8;) {
		while (emu_paused) {
			vTaskDelay(pdMS_TO_TICKS(50));
		}
        
		/* Process retro-go gamepad input */
		input_process();

		/* Step the emulator */
		pc_step(pc);
	}
	return 0;
}

struct esp_ini_config {
	const char *filename;
	char ssid[16];
	char pass[32];
};

static void i386_task(void *arg)
{
	struct esp_ini_config *config = (struct esp_ini_config *)arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "main runs on core %d\n", core_id);
	/* Wait for display panel to be ready */
	xEventGroupWaitBits(global_event_group,
	                    BIT1,
	                    pdFALSE,
	                    pdFALSE,
	                    portMAX_DELAY);
	int ret = pc_main(config->filename);
	if (ret != 0) {
		fprintf(stderr, "FATAL: pc_main failed with %d\n", ret);
	}
	vTaskDelete(NULL);
}

static char *psram;
static long psram_off;
static long psram_len;
void *psmalloc(long size)
{
	void *ret = psram + psram_off;
	
	size = (size + 4095) / 4096 * 4096;
	if (psram_off + size > psram_len) {
		fprintf(stderr, "psram error %ld %ld %ld\n", size, psram_off, psram_len);
		abort();
	}
	psram_off += size;
	return ret;
}

void *fbmalloc(long size)
{
	void *fb = (uint8_t *) heap_caps_calloc(1, size, MALLOC_CAP_DMA);
	if (!fb) {
		fprintf(stderr, "fbmalloc error %ld\n", size);
		abort();
	}
	return fb;
}

static int parse_ini(void* user, const char* section,
		     const char* name, const char* value)
{
	struct esp_ini_config *conf = (struct esp_ini_config *)user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	if (SEC("esp")) {
		if (NAME("ssid")) {
			if (strlen(value) < 32)
				strcpy(conf->ssid, value);
		} else if (NAME("pass")) {
			if (strlen(value) < 64)
				strcpy(conf->pass, value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}

/* ============================================================
 * This is the entry point called from retro-go main.c
 * It replaces the original app_main() from tiny386.
 * ============================================================ */
void tiny386_start(const char *config_path)
{
	ESP_LOGI(TAG, "Starting tiny386 emulator...");
	ESP_LOGI(TAG, "Config: %s", config_path ? config_path : "(none)");

	esp_log_level_set("gpio", ESP_LOG_WARN);
	esp_log_level_set("I2C", ESP_LOG_WARN);

	global_event_group = xEventGroupCreate();

	static struct esp_ini_config config = {0};
	if (config_path && config_path[0]) {
		if (ini_parse(config_path, parse_ini, &config) == 0) {
			config.filename = config_path;
			fprintf(stderr, "Using config: %s\n", config_path);
		} else {
			fprintf(stderr, "FATAL: Failed to parse config '%s'\n", config_path);
			return;
		}
	} else {
		fprintf(stderr, "FATAL: No config file found\n");
		return;
	}

	/* Allocate PSRAM pool for emulator memory */
	psram_len = PSRAM_ALLOC_LEN;
	psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
	if (!psram) {
		fprintf(stderr, "FATAL: Failed to allocate %d bytes of PSRAM\n", psram_len);
		return;
	}
	fprintf(stderr, "PSRAM allocated: %p, size=%d\n", psram, psram_len);

	if (psram) {
		xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
		xTaskCreatePinnedToCore(vga_task, "vga_task", 8192, NULL, 0, NULL, 0);
	} else {
		fprintf(stderr, "FATAL: No PSRAM available\n");
	}
}
