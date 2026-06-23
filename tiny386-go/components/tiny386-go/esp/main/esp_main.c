/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * This is the standalone ESP-IDF entry point (app_main).
 * When building for retro-go (RETRO_GO defined), tiny386_main.c is used instead.
 */

#ifndef RETRO_GO

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "esp_log.h"

#include "../../ini.h"
#include "../../pc.h"
#include "common.h"
#include "menu.h"

//
static const char *TAG = "esp_main";
#include "esp_private/system_internal.h"
#include "esp_private/rtc_clk.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "esp_rom_sys.h"

#ifdef __XTENSA__
/* ===== ESP32-S3 Xtensa: BBPLL I2C overclock + CCOUNT freq measurement ===== */

extern uint64_t esp_rtc_get_time_us(void);

/* BBPLL I2C register interface (ESP32-S3 ROM) */
extern void rom_i2c_writeReg(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t data);
extern uint8_t rom_i2c_readReg(uint8_t block, uint8_t host_id, uint8_t reg_add);

/* BBPLL I2C bus parameters (ESP32-S3) */
#define I2C_BBPLL           0x66
#define I2C_BBPLL_HOSTID    1
#define I2C_BBPLL_OC_DIV_7_0  3   /* PLL divider register */

/*
 * Overclock level range
 * level: integer, each step modifies PLL divider
 * ESP32-S3: OC_DIV7_MULTIPLIER=1, range -8 ~ +8
 */
#define OC_MAX_LEVEL           8
#define OC_MIN_LEVEL          -8
#define OC_DIV7_MULTIPLIER     1

/* Overclock state */
static int overclockLevel = 0;
static int overclockMhz = 0;

/* Measure real-time CPU frequency (MHz) via RTC time + CCOUNT
 *
 * RTC clock is not affected by CPU/APB clock, making it the only
 * reliable time reference.
 * real_freq = (c1 - c0) / (t1 - t0) us = MHz
 *
 * Uses inline asm to read Xtensa CCOUNT register. */
static uint32_t get_real_cpu_freq_mhz(void)
{
    uint32_t c0, c1;
    uint64_t t0, t1;

    t0 = esp_rtc_get_time_us();
    __asm__ __volatile__ ("rsr.ccount %0" : "=r"(c0));

    /* Wait ~100ms for accurate measurement */
    esp_rom_delay_us(100000);

    __asm__ __volatile__ ("rsr.ccount %0" : "=r"(c1));
    t1 = esp_rtc_get_time_us();

    uint64_t elapsed_us = t1 - t0;
    if (elapsed_us > 0) {
        uint32_t cycles = c1 - c0;
        return (uint32_t)((double)cycles / (double)elapsed_us + 0.5);
    }
    return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}

/*
 * Runtime overclock/underclock ESP32-S3 CPU
 *
 * level: overclock level, positive = faster, negative = slower
 * Modifies BBPLL OC_DIV_7_0 via I2C bus.
 *
 * BBPLL default output 480MHz (XTAL=40MHz * 12)
 * PLL out = XTAL * (OC_DIV_7_0 + 4) / (OC_DIV_8_0 + 1)
 * OC_DIV_8_0 defaults to 0 (unchanged)
 * So PLL = 40 * (OC_DIV_7_0 + 4)
 *
 * Default OC_DIV_7_0 = 8 -> PLL = 40 * 12 = 480MHz
 * CPU freq = PLL / CPU_HP_CLK_DIV_NUM (fixed /2 = 240MHz)
 */
void rg_system_set_overclock(int level)
{
    if (level < OC_MIN_LEVEL || level > OC_MAX_LEVEL) {
        ESP_LOGW(TAG, "Invalid level %d, min:%d max:%d", level, OC_MIN_LEVEL, OC_MAX_LEVEL);
        return;
    }

    static int original_div7_0 = -1;
    if (original_div7_0 == -1) {
        original_div7_0 = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0);
        ESP_LOGI(TAG, "Original BBPLL DIV7 value: %d (PLL=%dMHz, CPU=%dMHz)",
                 original_div7_0, 40 * (original_div7_0 + 4), 40 * (original_div7_0 + 4) / 2);
    }

    uint8_t div7_0 = original_div7_0 + (level * OC_DIV7_MULTIPLIER);
    /* Clamp to 2~15 (PLL=240~760MHz, CPU=120~380MHz) */
    if (div7_0 < 2) div7_0 = 2;
    if (div7_0 > 15) div7_0 = 15;

    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0, div7_0);
    esp_rom_delay_us(20);

    /* Verify with RTC time (unaffected by CPU/APB clock) */
    uint64_t t = esp_rtc_get_time_us();
    uint32_t cc;
    __asm__ __volatile__ ("rsr.ccount %0" : "=r"(cc));

    esp_rom_delay_us(100000);

    uint32_t cc_after;
    __asm__ __volatile__ ("rsr.ccount %0" : "=r"(cc_after));
    int real_mhz = (double)(cc_after - cc) / (double)(esp_rtc_get_time_us() - t);

    /* ESP32-S3: APB clock = 80MHz fixed, not affected by PLL */
    overclockLevel = level;
    overclockMhz = real_mhz;
    ESP_LOGW(TAG, "Overclock level %d applied: %dMHz (div7:%d)", level, real_mhz, div7_0);
}

/* Legacy interface: MHz-based overclock */
static void set_cpu_freq_mhz(uint32_t target_mhz)
{
    int target_div7 = (int)(target_mhz * 2 / 40 - 4);
    int level = target_div7 - 8; /* default DIV7=8 */
    rg_system_set_overclock(level);
}

#else /* RISC-V (ESP32-P4, etc.) */

/* No BBPLL I2C overclock on RISC-V targets.
 * Use the configured CPU frequency. */
extern uint64_t esp_rtc_get_time_us(void);

static uint32_t get_real_cpu_freq_mhz(void)
{
    return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}

void rg_system_set_overclock(int level)
{
    ESP_LOGW(TAG, "Overclock not supported on this target (level=%d ignored)", level);
}

#endif /* __XTENSA__ */

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
Console *console_init(int width, int height)
{
    Console *c = malloc(sizeof(Console));
    memset(c, 0, sizeof(Console));
    
    if (globals.panel_fb) {
        ESP_LOGI(TAG, "Using zero-copy mode: panel_fb = %p", globals.panel_fb);
        c->fb = globals.panel_fb;
    } else {
        ESP_LOGI(TAG, "Using allocated framebuffer");
        c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
        ESP_LOGI(TAG, "fb allocated at %p, size %d", c->fb, LCD_WIDTH * LCD_HEIGHT * 2);
    }
    g_framebuffer = c->fb;
    
    return c;
}

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src);
static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
    Console *s = opaque;
    if (!s || !s->fb) return;
    
    /* Clip dirty rect to screen */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
	if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
	if (w <= 0 || h <= 0) return;

	lcd_draw(x, y, x + w, y + h, s->fb);
}
    
void *esp_psram_get(size_t *size);
void vga_task(void *arg);
void i2s_main();
void wifi_main(const char *, const char *);
void storage_init(void);
void input_init(void);
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

	/* Check critical config */
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

		/* Initialize GPIO keyboard input (uses globals.kbd) */
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
			/* Menu active: check every 50ms whether to resume */
			vTaskDelay(pdMS_TO_TICKS(50));
		}
		pc_step(pc);
	}
	return 0;
}

//

struct esp_ini_config {
	const char *filename;
	char ssid[16];
	char pass[32];
};

static void i386_task(void *arg)
{
	struct esp_ini_config *config = arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "main runs on core %d\n", core_id);
	/* Wait for LCD panel (and panel_fb) to be ready before starting the
	 * PC emulator.  console_init() uses globals.panel_fb if set. */
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
	struct esp_ini_config *conf = user;
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

void app_main(void)
{
	/* Suppress noisy ESP-IDF debug logs */
	esp_log_level_set("gpio", ESP_LOG_WARN);
	esp_log_level_set("I2C", ESP_LOG_WARN);

	global_event_group = xEventGroupCreate();

#ifdef ESPDEBUG
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity	= UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	uart_param_config(UART_NUM_0, &uart_config);
	if (uart_driver_install(UART_NUM_0, 2 * 1024, 0, 0, NULL, 0) != ESP_OK) {
		assert(false);
	}
#endif

		/* Log current CPU frequency */
#ifdef __XTENSA__
		ESP_LOGI(TAG, "ESP32-S3 configured CPU frequency: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
		ESP_LOGI(TAG, "ESP32-S3 real-time CPU frequency: %" PRIu32 " MHz", get_real_cpu_freq_mhz());
#else
		ESP_LOGI(TAG, "ESP32-P4 configured CPU frequency: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif
	
				/* Overclock disabled (affects USB clock) */
				//set_cpu_freq_mhz(280);


	i2s_main();
	storage_init();

	esp_psram_init();
#ifndef PSRAM_ALLOC_LEN
	// use the whole psram
	size_t len;
	psram = esp_psram_get(&len);
	psram_len = len;
#else
	psram_len = PSRAM_ALLOC_LEN;
	psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
#endif

				const static char *files[] = {
					"/sdcard/tiny386.ini",
					"/sdcard/roms/dos/.system/tiny386.ini",
					"/spiflash/tiny386.ini",
					NULL,
				};
	static struct esp_ini_config config;
	bool ini_found = false;
	for (int i = 0; files[i]; i++) {
		if (ini_parse(files[i], parse_ini, &config) == 0) {
			config.filename = files[i];
			ini_found = true;
			fprintf(stderr, "Using config: %s\n", files[i]);
			break;
		}
	}
	if (!ini_found) {
		/* Try to create a default config on SPIFFS */
		fprintf(stderr, "No config found, creating default on SPIFFS...\n");
		FILE *f = fopen("/spiflash/tiny386.ini", "w");
		if (f) {
			fprintf(f,
				"[pc]\n"
				"bios = bios.bin\n"
				"vga_bios = vgabios.bin\n"
				"mem_size = 8M\n"
				"vga_mem_size = 256K\n"
				"\n"
				"[display]\n"
				"width = %d\n"
				"height = %d\n"
				"\n"
				"[cpu]\n"
				"gen = 4\n"
				"fpu = 0\n",
				LCD_WIDTH, LCD_HEIGHT);
			fclose(f);
			fprintf(stderr, "Default config written to /spiflash/tiny386.ini\n");
			/* Try again with the newly created config */
			if (ini_parse("/spiflash/tiny386.ini", parse_ini, &config) == 0) {
				config.filename = "/spiflash/tiny386.ini";
				ini_found = true;
				fprintf(stderr, "Using config: /spiflash/tiny386.ini\n");
			}
		} else {
			fprintf(stderr, "Failed to create default config on SPIFFS\n");
		}
	}
	if (!ini_found) {
		fprintf(stderr, "FATAL: No config file found (tried SD card, SPIFFS)\n");
		return;  /* Don't start emulator tasks */
	}
#ifdef esp32s3
	if (config.ssid[0]) {
		wifi_main(config.ssid, config.pass);
	}
#endif

	if (psram) {
		xTaskCreatePinnedToCore(i386_task, "i386_main", 4096, &config, 3, NULL, 1);
		xTaskCreatePinnedToCore(vga_task, "vga_task", 4096, NULL, 0, NULL, 0);
	}
}

#endif /* !RETRO_GO */
