/*
 * rg_coplay.c - CoPlay SPI Direct-Connect Multiplayer
 *
 * Connects two ESP32-P4 devices via SPI for 2-player gaming.
 * Host sends frame data, client receives and displays.
 * Client sends gamepad input to host for P2 control.
 *
 * Protocol:
 *   Full-duplex SPI at 40MHz.
 *   Each transaction is a packet: [4-byte header][payload].
 *   Frame data (153600 bytes) is sent in chunks of COPLAY_CHUNK_SIZE.
 *   After frame is complete, host sends INPUT_POLL to get client input.
 *   During frame chunks, client's tx_buffer is all zeros.
 */

#include "rg_system.h"
#include "rg_coplay.h"
#include "rg_display.h"
#include "rg_input.h"

#include <string.h>
#include <driver/spi_master.h>
#include <driver/spi_slave.h>
#include <driver/gpio.h>
#include "soc/usb_serial_jtag_reg.h"
#include "hal/usb_serial_jtag_ll.h"

#if defined(RG_NET_SPI_HOST)

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------
#ifndef RG_COPLAY_LOG
#define RG_COPLAY_LOG(...)  // Disable by default, enable with RG_LOGI
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define COPLAY_NUM_CHUNKS   ((COPLAY_FRAME_SIZE + COPLAY_CHUNK_SIZE - 1) / COPLAY_CHUNK_SIZE)
#define COPLAY_TIMEOUT_MS   1000
#define COPLAY_RETRY_MS     10
#define COPLAY_HANDSHAKE_RETRIES 50

// GPIO levels for handshake signal
#define HS_IDLE     0
#define HS_ACTIVE   1

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool coplay_initialized = false;
static bool coplay_connected = false;

// Pin debug info
typedef struct {
    const char *name;
    gpio_num_t num;
} debug_pin_t;

static const debug_pin_t debug_pins[] = {
    {"SCK",  RG_NET_SCK},
    {"CS",   RG_NET_CS},
    {"MOSI", RG_NET_MOSI},
    {"MISO", RG_NET_MISO},
    {"HS",   RG_NET_HS},
};
#define DEBUG_PIN_COUNT (sizeof(debug_pins) / sizeof(debug_pins[0]))

// Manual pin control state
#define PIN_MODE_AUTO     0  // SPI driver controls this pin
#define PIN_MODE_INPUT    1  // Manually set as input (high-Z)
#define PIN_MODE_OUT_HIGH 2  // Manually driven HIGH
#define PIN_MODE_OUT_LOW  3  // Manually driven LOW

static int selected_pin_idx = 0;
static int pin_modes[DEBUG_PIN_COUNT] = {PIN_MODE_AUTO};

// ---------------------------------------------------------------------------
// Pin status display
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Release USB Serial/JTAG control over GPIO24/25 so they can be used as
// normal GPIO (e.g., SPI MOSI/MISO).
//
// On ESP32-P4, GPIO24 = USB D-, GPIO25 = USB D+.
// The USB D+ pin has an internal pull-up that keeps it HIGH by default.
// We must:
//   1. Disable the USB D+ pull-up resistor
//   2. Reset both pins via gpio_reset_pin() so GPIO matrix takes over
//
// WARNING: After calling this, USB Serial/JTAG console will STOP WORKING.
// You'll need to use UART for serial output, or re-enable USB before flashing.
// ---------------------------------------------------------------------------
static void usb_pins_release(void)
{
    gpio_reset_pin(RG_NET_MOSI);
    gpio_reset_pin(RG_NET_MISO);
    gpio_reset_pin(RG_NET_SCK);
    gpio_reset_pin(RG_NET_CS);
    gpio_reset_pin(RG_NET_HS);

    // Step 1: Disable USB D+ internal pull-up (this is what keeps GPIO25 HIGH)
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);

    // Also disable D- pull if present
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DM_PULLUP);

    // Small delay for register write to take effect
    rg_task_delay(10);

    // Step 2: Reset both pins so GPIO driver takes full control
    gpio_reset_pin(RG_NET_MOSI); // GPIO25 / USB D+
    gpio_reset_pin(RG_NET_MISO); // GPIO24 / USB D-

    RG_LOGI("coplay: USB Serial/JTAG pins released (GPIO%d/%d now free)",
            RG_NET_MOSI, RG_NET_MISO);
}

// Apply manual control mode to a pin
static void apply_pin_mode(int idx)
{
    gpio_num_t num = debug_pins[idx].num;
    switch (pin_modes[idx]) {
        case PIN_MODE_AUTO:
            // Reset to default — SPI driver will reconfigure on next init/transaction
            gpio_reset_pin(num);
            break;
        case PIN_MODE_INPUT:
            gpio_set_direction(num, GPIO_MODE_INPUT);
            gpio_set_pull_mode(num, GPIO_FLOATING);
            break;
        case PIN_MODE_OUT_HIGH:
            gpio_set_direction(num, GPIO_MODE_OUTPUT);
            gpio_set_pull_mode(num, GPIO_FLOATING);
            gpio_set_level(num, 1);
            break;
        case PIN_MODE_OUT_LOW:
            gpio_set_direction(num, GPIO_MODE_OUTPUT);
            gpio_set_pull_mode(num, GPIO_FLOATING);
            gpio_set_level(num, 0);
            break;
    }
}

static void draw_pin_status(bool is_client, const char *title)
{
    char buf[64];
    int y;

    // Title bar
    rg_display_clear(C_BLACK);
    snprintf(buf, sizeof(buf), "%s", title);
    rg_gui_draw_text(0, 4, 320, buf, C_CYAN, C_BLACK, RG_TEXT_ALIGN_CENTER);

    // Subtitle with instructions
    rg_gui_draw_text(0, 20, 320,
        "UP/DOWN=select | A=HIGH | B=LOW | SEL=Input(auto) | START=All Auto",
        C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);

    // Column headers
    y = 38;
    rg_gui_draw_text(2, y, 316, "PIN   GPIO   LEVEL   CTRL-MODE",
                     C_YELLOW, C_BLACK, 0);
    y += 14;
    rg_gui_draw_text(2, y, 316, "----  -----  ------  ------------",
                     C_DARK_GRAY, C_BLACK, 0);
    y += 18;

    // Each pin row
    for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
        int level = gpio_get_level(debug_pins[i].num);
        bool selected = (i == selected_pin_idx);

        // Row background — selected gets bright highlight
        int row_h = 24;
        rg_color_t row_bg;
        if (selected) {
            row_bg = 0x0020; // dark blue highlight
        } else if (level != 0) {
            row_bg = 0x1008; // subtle red tint when HIGH
        } else {
            row_bg = C_BLACK;
        }
        // Draw selection border
        if (selected) {
            rg_gui_draw_rect(1, y - 1, 318, row_h, 2, C_CYAN, row_bg);
        }

        // Selection arrow
        snprintf(buf, sizeof(buf), "%s %-4s", selected ? ">" : " ", debug_pins[i].name);
        rg_gui_draw_text(6, y + 2, 90, buf, selected ? C_CYAN : C_GREEN, row_bg, 0);

        // GPIO number
        snprintf(buf, sizeof(buf), "%2d", debug_pins[i].num);
        rg_gui_draw_text(72, y + 2, 36, buf, C_WHITE, row_bg, 0);

        // Level: HIGH/LOW with color
        const char *lvl_str = level ? "HIGH" : "LOW ";
        rg_color_t lvl_color = level ? C_RED : C_BLUE;
        rg_gui_draw_text(110, y + 2, 48, lvl_str, lvl_color, row_bg, RG_TEXT_BIGGER);

        // Visual indicator bar
        int bar_x = 164;
        int bar_w = 70;
        int bar_h = 18;
        rg_gui_draw_rect(bar_x, y + 2, bar_w, bar_h, 1, C_GRAY, C_BLACK);
        if (level) {
            rg_gui_draw_rect(bar_x + 2, y + 4, bar_w - 4, bar_h - 4, 0, C_RED, C_RED);
        } else {
            rg_gui_draw_rect(bar_x + 2, y + 4, bar_w - 4, bar_h - 4, 0, C_DARK_BLUE, C_DARK_BLUE);
        }

        // Control mode text
        const char *mode_str;
        rg_color_t mode_color;
        switch (pin_modes[i]) {
            case PIN_MODE_OUT_HIGH: mode_str = "FORCE HI"; mode_color = C_RED; break;
            case PIN_MODE_OUT_LOW:  mode_str = "FORCE LO"; mode_color = C_BLUE; break;
            case PIN_MODE_INPUT:    mode_str = "INPUT(Z)"; mode_color = C_YELLOW; break;
            default:                mode_str = "AUTO(SPI)"; mode_color = C_DARK_GRAY; break;
        }
        rg_gui_draw_text(240, y + 2, 76, mode_str, mode_color, row_bg, 0);

        y += row_h;
    }

    // Connection status line at bottom
    y += 8;
    const char *status;
    rg_color_t st_color;
    if (is_client) {
        status = "Status: Pin Debug Mode (no SPI active)";
        st_color = C_CYAN;
    } else {
        status = "Status: Pin Debug Mode (no SPI active)";
        st_color = C_ORANGE;
    }
    rg_gui_draw_text(0, y, 320, status, st_color, C_BLACK, RG_TEXT_ALIGN_CENTER);

    // Mode label
    y += 14;
    snprintf(buf, sizeof(buf), "Role: %s", is_client ? "CLIENT (Slave)" : "HOST (Master)");
    rg_gui_draw_text(0, y, 320, buf, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);

    // Tip line
    y += 14;
    snprintf(buf, sizeof(buf), "[%s] sel=%d mode=%d",
             debug_pins[selected_pin_idx].name,
             debug_pins[selected_pin_idx].num,
             pin_modes[selected_pin_idx]);
    rg_gui_draw_text(0, y, 320, buf, C_DARK_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);

    rg_display_sync();
}

// Host-side handles
static spi_device_handle_t spi_host_handle;
static spi_host_device_t spi_host = RG_NET_SPI_HOST;

// Client-side
static bool is_client = false;
static coplay_gamepad_t client_pending_input = 0;
static bool client_input_dirty = false;

// Frame buffer for client (DMA-capable)


// ---------------------------------------------------------------------------
// Packet helpers
// ---------------------------------------------------------------------------
static inline void build_header(uint8_t *buf, uint8_t type, uint8_t seq, uint16_t len)
{
    buf[0] = type;
    buf[1] = seq;
    buf[2] = len & 0xFF;
    buf[3] = (len >> 8) & 0xFF;
}

static inline void parse_header(const uint8_t *buf, uint8_t *type, uint8_t *seq, uint16_t *len)
{
    *type = buf[0];
    *seq = buf[1];
    *len = buf[2] | ((uint16_t)buf[3] << 8);
}

// ---------------------------------------------------------------------------
// General-purpose SPI transaction (full-duplex)
// ---------------------------------------------------------------------------
static bool spi_transact(const void *tx_data, size_t tx_len,
                         void *rx_data, size_t rx_len, int timeout_ms)
{
    if (is_client) {
        // Client mode (slave)
        spi_slave_transaction_t t = {
            .length = (tx_len > rx_len ? tx_len : rx_len) * 8,
            .tx_buffer = tx_data,
            .rx_buffer = rx_data,
        };
        esp_err_t ret = spi_slave_transmit(spi_host, &t, timeout_ms > 0
            ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
        return ret == ESP_OK;
    } else {
        // Host mode (master)
        spi_transaction_t t = {
            .length = (tx_len > rx_len ? tx_len : rx_len) * 8,
            .tx_buffer = tx_data,
            .rx_buffer = rx_data,
            .flags = 0, // Full-duplex
        };
        esp_err_t ret = spi_device_transmit(spi_host_handle, &t);
        return ret == ESP_OK;
    }
}

// ---------------------------------------------------------------------------
// Host: send one SPI packet with header + payload, receive response
// ---------------------------------------------------------------------------
static bool host_send_packet(uint8_t type, uint8_t seq,
                              const void *payload, uint16_t payload_len,
                              void *rx_buf, uint16_t rx_buf_len)
{
    size_t total_tx = COPLAY_HDR_SIZE + payload_len;
    size_t total_rx = rx_buf_len > 0 ? COPLAY_HDR_SIZE + rx_buf_len : 4; // at least 4 bytes

    // Use stack for small packets, heap for large
    uint8_t tx_buf[512];
    uint8_t *tx_ptr = tx_buf;
    uint8_t *rx_ptr = NULL;

    uint8_t rx_stack[64];
    rx_ptr = (total_rx <= sizeof(rx_stack)) ? rx_stack : malloc(total_rx);
    if (total_tx > sizeof(tx_buf)) {
        tx_ptr = malloc(total_tx);
    }

    if (!tx_ptr || (!rx_ptr && total_rx > 0)) {
        if (tx_ptr && tx_ptr != tx_buf) free(tx_ptr);
        if (rx_ptr && rx_ptr != rx_stack) free(rx_ptr);
        return false;
    }

    build_header(tx_ptr, type, seq, payload_len);
    if (payload && payload_len > 0) {
        memcpy(tx_ptr + COPLAY_HDR_SIZE, payload, payload_len);
    }

    memset(rx_ptr, 0, total_rx);

    bool ok = spi_transact(tx_ptr, total_tx, rx_ptr, total_rx, COPLAY_TIMEOUT_MS);

    if (ok && rx_buf && rx_buf_len > 0 && total_rx >= COPLAY_HDR_SIZE) {
        // Parse response header
        uint8_t rtype, rseq;
        uint16_t rlen;
        parse_header(rx_ptr, &rtype, &rseq, &rlen);
        uint16_t copy_len = (rlen < rx_buf_len) ? rlen : rx_buf_len;
        if (rlen > 0 && copy_len > 0) {
            memcpy(rx_buf, rx_ptr + COPLAY_HDR_SIZE, copy_len);
        }
        // Return the response type via the first byte of rx_buf if enough space
        if (rx_buf_len >= 1) {
            ((uint8_t *)rx_buf)[0] = rtype;
        }
    }

    if (tx_ptr && tx_ptr != tx_buf) free(tx_ptr);
    if (rx_ptr && rx_ptr != rx_stack) free(rx_ptr);

    return ok;
}

// ---------------------------------------------------------------------------
// Client: receive one SPI packet
// ---------------------------------------------------------------------------
static bool client_recv_packet(uint8_t *type, uint8_t *seq, void *payload, uint16_t *payload_len)
{
    uint8_t hdr_buf[COPLAY_HDR_SIZE];
    uint8_t response[64];

    // Wait for host to send something
    build_header(response, COPLAY_PKT_HANDSHAKE_ACK, 0, 0);
    memset(hdr_buf, 0, sizeof(hdr_buf));

    if (!spi_transact(response, 4, hdr_buf, 4, COPLAY_TIMEOUT_MS)) {
        return false;
    }

    parse_header(hdr_buf, type, seq, payload_len);

    // If there's a payload, receive it in a second transaction
    if (*payload_len > 0 && payload) {
        uint16_t to_read = (*payload_len < 65535) ? *payload_len : 65535;
        uint16_t chunks = (to_read + COPLAY_CHUNK_SIZE - 1) / COPLAY_CHUNK_SIZE;
        uint8_t *p = (uint8_t *)payload;

        // Client: send gamepad state in tx_buffer of each chunk
        for (uint16_t c = 0; c < chunks; c++) {
            uint16_t chunk_len = (to_read > COPLAY_CHUNK_SIZE) ? COPLAY_CHUNK_SIZE : to_read;

            uint8_t chunk_resp[COPLAY_HDR_SIZE + 2];
            build_header(chunk_resp, COPLAY_PKT_INPUT_DATA, 0, 2);
            *(uint16_t *)(chunk_resp + COPLAY_HDR_SIZE) = client_pending_input;
            client_pending_input = 0;

            if (!spi_transact(chunk_resp, COPLAY_HDR_SIZE + 2, p, chunk_len, COPLAY_TIMEOUT_MS)) {
                return false;
            }
            p += chunk_len;
            to_read -= chunk_len;
        }
    }

    return true;
}

// ===========================================================================
// Host Implementation
// ===========================================================================

void rg_coplay_host_init(void)
{
    if (coplay_initialized) return;

    is_client = false;

    printf("\n*** coplay: Host SPI init start (host=%d) ***\n", spi_host);
    gpio_reset_pin(RG_NET_MOSI);
    gpio_reset_pin(RG_NET_MISO);
    gpio_reset_pin(RG_NET_SCK);
    gpio_reset_pin(RG_NET_CS);
    RG_LOGI("coplay: Host SPI init: host=%d MOSI=%d MISO=%d SCK=%d CS=%d HS=%d",
            spi_host, RG_NET_MOSI, RG_NET_MISO, RG_NET_SCK, RG_NET_CS, RG_NET_HS);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_NET_MOSI,
        .miso_io_num = RG_NET_MISO,
        .sclk_io_num = RG_NET_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = COPLAY_CHUNK_SIZE * 2,  // 8184 bytes - large enough for DMA efficiency
    };

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  // 40 MHz
        .mode = 0,
        .spics_io_num = RG_NET_CS,
        .queue_size = 3,
        .flags = 0, // Full-duplex
    };

    ESP_ERROR_CHECK(spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(spi_host, &dev_cfg, &spi_host_handle));

    // Handshake GPIO (output)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RG_NET_HS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(RG_NET_HS, HS_IDLE);

    coplay_initialized = true;
    coplay_connected = false;

    RG_LOGI("coplay: Host SPI initialized (MOSI=%d MISO=%d SCK=%d CS=%d HS=%d)",
            RG_NET_MOSI, RG_NET_MISO, RG_NET_SCK, RG_NET_CS, RG_NET_HS);
}

bool rg_coplay_wait_for_client(int timeout_ms)
{
    if (!coplay_initialized) {
        RG_LOGE("coplay: Host not initialized");
        return false;
    }

    uint8_t rx_resp[4];
    int retries = (timeout_ms < 0) ? COPLAY_HANDSHAKE_RETRIES
                                   : (timeout_ms / 100);

    RG_LOGI("coplay: Waiting for client...");

    for (int i = 0; i < retries; i++) {
        // Assert handshake signal
        gpio_set_level(RG_NET_HS, HS_ACTIVE);
        rg_task_delay(1);

        // Send handshake packet
        memset(rx_resp, 0, sizeof(rx_resp));
        bool ok = host_send_packet(COPLAY_PKT_HANDSHAKE, 0, NULL, 0, rx_resp, 1);

        rg_task_delay(50);
        gpio_set_level(RG_NET_HS, HS_IDLE);

        if (ok && rx_resp[0] == COPLAY_PKT_HANDSHAKE_ACK) {
            coplay_connected = true;
            RG_LOGI("coplay: Client connected!");
            return true;
        }

        if (timeout_ms > 0 && i * 100 >= timeout_ms) {
            break;
        }

        rg_task_delay(90);
    }

    RG_LOGW("coplay: No client detected after %d ms", timeout_ms >= 0 ? timeout_ms : retries * 100);
    return false;
}

void rg_coplay_send_frame(uint16_t *frame_buffer, int width, int height, coplay_gamepad_t *client_input)
{
    if (!coplay_initialized || !coplay_connected) {
        if (client_input) *client_input = 0;
        return;
    }

    uint32_t frame_size = width * height * 2;

    gpio_set_level(RG_NET_HS, HS_ACTIVE);

    // Send frame header (width, height) as first 4 bytes
    uint8_t hdr[4] = {
        width & 0xFF, (width >> 8) & 0xFF,
        height & 0xFF, (height >> 8) & 0xFF
    };
    spi_transaction_t hdr_t = {
        .length = 4 * 8,
        .tx_buffer = hdr,
        .rx_buffer = NULL,
        .flags = 0,
    };
    if (spi_device_transmit(spi_host_handle, &hdr_t) != ESP_OK) {
        RG_LOGE("coplay: Failed to send frame header");
        coplay_connected = false;
        gpio_set_level(RG_NET_HS, HS_IDLE);
        if (client_input) *client_input = 0;
        return;
    }

    // Send frame in chunks
    uint32_t remaining = frame_size;
    uint8_t *data = (uint8_t *)frame_buffer;
    uint8_t seq = 0;

    while (remaining > 0) {
        uint16_t chunk_len = (remaining > COPLAY_CHUNK_SIZE) ? COPLAY_CHUNK_SIZE : remaining;
        uint8_t rx_buf[8] = {0};

        spi_transaction_t t = {
            .length = chunk_len * 8,
            .tx_buffer = data,
            .rx_buffer = rx_buf,
            .flags = 0,
        };

        esp_err_t ret = spi_device_transmit(spi_host_handle, &t);

        if (ret != ESP_OK) {
            RG_LOGE("coplay: SPI transmit failed on chunk seq=%d", seq);
            coplay_connected = false;
            gpio_set_level(RG_NET_HS, HS_IDLE);
            if (client_input) *client_input = 0;
            return;
        }

        // Check if client sent input (first 2 bytes of rx buffer)
        if (seq == 0 && client_input) {
            uint16_t possible_input = *(uint16_t *)rx_buf;
            if (possible_input != 0 && possible_input != 0xFFFF) {
                *client_input = possible_input;
            }
        }

        data += chunk_len;
        remaining -= chunk_len;
        seq++;
    }

    gpio_set_level(RG_NET_HS, HS_IDLE);

    // Poll for client input after frame is sent
    if (client_input) {
        uint8_t input_resp[4] = {0};
        if (host_send_packet(COPLAY_PKT_INPUT_DATA, 0, NULL, 0, input_resp, 3)) {
            if (input_resp[0] == COPLAY_PKT_INPUT_DATA) {
                *client_input = (uint16_t)(input_resp[1] | ((uint16_t)input_resp[2] << 8));
            }
        }
    }
}

bool rg_coplay_is_client_connected(void)
{
    return coplay_connected;
}

void rg_coplay_host_deinit(void)
{
    if (!coplay_initialized) return;

    coplay_connected = false;
    coplay_initialized = false;

    if (spi_host_handle) {
        spi_bus_remove_device(spi_host_handle);
        spi_host_handle = NULL;
    }
    spi_bus_free(spi_host);

    gpio_set_level(RG_NET_HS, HS_IDLE);

    RG_LOGI("coplay: Host deinitialized");
}

// ===========================================================================
// Client Implementation
// ===========================================================================

void rg_coplay_client_init(void)
{
    if (coplay_initialized) return;
    gpio_reset_pin(RG_NET_MOSI);
    gpio_reset_pin(RG_NET_MISO);
    gpio_reset_pin(RG_NET_SCK);
    gpio_reset_pin(RG_NET_CS);

    is_client = true;
    // No large DMA pre-allocation — frame buffers provided by caller,
    // SPI DMA is handled internally by the driver.

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

    ESP_ERROR_CHECK(spi_slave_initialize(spi_host, &bus_cfg, &slave_cfg, SPI_DMA_CH_AUTO));

    // Handshake GPIO (input, no pull — host drives it high/low)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RG_NET_HS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    client_pending_input = 0;
    client_input_dirty = false;
    coplay_initialized = true;
    coplay_connected = false;

    RG_LOGI("coplay: Client SPI initialized");
}

bool rg_coplay_wait_for_host(int timeout_ms)
{
    if (!coplay_initialized) {
        RG_LOGE("coplay: Client not initialized");
        return false;
    }

    RG_LOGI("coplay: Waiting for host...");

    int64_t deadline = (timeout_ms > 0) ? (rg_system_timer() + (int64_t)timeout_ms * 1000)
                                         : 0;
    int attempt = 0;

    while (timeout_ms < 0 || rg_system_timer() < deadline) {
        // Check if host asserted handshake signal
        if (gpio_get_level(RG_NET_HS) == HS_ACTIVE) {
            attempt+=1;
            rg_gui_draw_message("CoPlay Client\n\nhandshaking with host...");
            // Try to receive handshake
            uint8_t type, seq;
            uint16_t len;
            if (client_recv_packet(&type, &seq, NULL, &len)) {
                if (type == COPLAY_PKT_HANDSHAKE) {
                    coplay_connected = true;
                    RG_LOGI("coplay: Host detected!");
                    return true;
                }
            }else{
                rg_gui_draw_message("CoPlay Client\n\nrecv packet failed...");
            }
        }
        rg_task_delay(50);
    }
    if (attempt>0){
        rg_gui_draw_message("CoPlay Client\n\nWaiting for host...\n(Press B to cancel)");
    }

    RG_LOGE("coplay: Host not found after timeout");
    return false;
}

bool rg_coplay_recv_frame(uint16_t *frame_buffer, int max_size, int *width, int *height)
{
    if (!coplay_initialized || !coplay_connected || !frame_buffer || !width || !height) {
        return false;
    }

    // Wait for HS signal from host (start of frame)
    int wait_count = 0;
    while (gpio_get_level(RG_NET_HS) != HS_ACTIVE) {
        rg_task_delay(1);
        wait_count++;
        if (wait_count > 500) { // ~500ms timeout
            RG_LOGE("coplay: Frame receive timeout");
            coplay_connected = false;
            return false;
        }
    }

    // Read frame header (4 bytes: width, height)
    uint8_t hdr_buf[4] = {0};
    uint8_t resp_hdr[4] = {0};
    if (!spi_transact(resp_hdr, 4, hdr_buf, 4, COPLAY_TIMEOUT_MS)) {
        RG_LOGE("coplay: Failed to receive frame header");
        coplay_connected = false;
        return false;
    }
    *width = hdr_buf[0] | ((int)hdr_buf[1] << 8);
    *height = hdr_buf[2] | ((int)hdr_buf[3] << 8);

    if (*width <= 0 || *width > 640 || *height <= 0 || *height > 480) {
        RG_LOGE("coplay: Invalid frame dimensions %dx%d", *width, *height);
        coplay_connected = false;
        return false;
    }

    uint32_t frame_size = (*width) * (*height) * 2;
    if ((int)frame_size > max_size) {
        RG_LOGE("coplay: Frame buffer too small (%d < %ld)", max_size, frame_size);
        return false;
    }

    // Receive frame chunks
    uint32_t received = 0;
    uint8_t *buf = (uint8_t *)frame_buffer;

    while (received < frame_size) {
        uint16_t chunk_len = COPLAY_CHUNK_SIZE;
        if (frame_size - received < chunk_len) {
            chunk_len = frame_size - received;
        }

        uint8_t resp_buf[COPLAY_CHUNK_SIZE + 4];
        memset(resp_buf, 0, sizeof(resp_buf));
        *(uint16_t *)resp_buf = client_pending_input;
        client_pending_input = 0;

        memset(buf, 0, chunk_len);

        if (!spi_transact(resp_buf, sizeof(resp_buf), buf, chunk_len, COPLAY_TIMEOUT_MS)) {
            RG_LOGE("coplay: SPI receive failed at offset %ld", received);
            coplay_connected = false;
            return false;
        }

        buf += chunk_len;
        received += chunk_len;
    }

    // Wait for poll from host
    uint8_t type, seq;
    uint16_t len;
    if (client_recv_packet(&type, &seq, NULL, &len)) {
        if (type == COPLAY_PKT_INPUT_DATA || type == COPLAY_PKT_FRAME_DONE) {
            // OK, frame complete
        }
    }

    return true;
}

void rg_coplay_send_input(coplay_gamepad_t gamepad)
{
    client_pending_input = gamepad;
    client_input_dirty = true;
}

void rg_coplay_client_deinit(void)
{
    if (!coplay_initialized) return;

    coplay_connected = false;
    coplay_initialized = false;

    spi_slave_free(spi_host);

    RG_LOGI("coplay: Client deinitialized");
}

// ===========================================================================
// Client main loop (standalone)
// ===========================================================================

/**
 * rg_coplay_client_run - Main loop for the CoPlay client.
 *
 * Initializes SPI slave, waits for host, then enters a loop:
 *   receive frame → display → read input → send input
 *
 * Call this from the launcher or a dedicated app.
 * Returns when the user presses B or connection is lost.
 */
void rg_coplay_client_run(void)
{
    RG_LOGI("coplay: Starting CoPlay client mode");

    // Reset all pins to a clean input state first
    selected_pin_idx = 0;
    for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
        pin_modes[i] = PIN_MODE_INPUT;
        gpio_reset_pin(debug_pins[i].num);
        gpio_set_direction(debug_pins[i].num, GPIO_MODE_INPUT);
        gpio_set_pull_mode(debug_pins[i].num, GPIO_FLOATING);
    }
    usb_pins_release();
    coplay_initialized = false;
    is_client = true;

    draw_pin_status(true, "Pin Debug - CLIENT side");

    // Pure pin debug loop — no SPI init, no handshake, no connection
    while (true) {
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_UP) {
            selected_pin_idx = (selected_pin_idx - 1 + DEBUG_PIN_COUNT) % DEBUG_PIN_COUNT;
            draw_pin_status(true, "Pin Debug - CLIENT side");
            rg_task_delay(150);
            continue;
        }
        if (joystick & RG_KEY_DOWN) {
            selected_pin_idx = (selected_pin_idx + 1) % DEBUG_PIN_COUNT;
            draw_pin_status(true, "Pin Debug - CLIENT side");
            rg_task_delay(150);
            continue;
        }
        if (joystick & RG_KEY_A) {
            pin_modes[selected_pin_idx] = PIN_MODE_OUT_HIGH;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(true, "Pin Debug - CLIENT side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_B) {
            pin_modes[selected_pin_idx] = PIN_MODE_OUT_LOW;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(true, "Pin Debug - CLIENT side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_SELECT) {
            pin_modes[selected_pin_idx] = PIN_MODE_INPUT;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(true, "Pin Debug - CLIENT side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_START) {
            // Reset all pins to input (high-Z)
            for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
                pin_modes[i] = PIN_MODE_INPUT;
                apply_pin_mode(i);
            }
            draw_pin_status(true, "Pin Debug - All pins reset to INPUT");
            rg_task_delay(200);
            continue;
        }
        if (joystick & RG_KEY_MENU) {
            break;
        }

        // Redraw periodically for live level updates
        static int redraw_counter = 0;
        if (++redraw_counter >= 8) { // ~400ms
            draw_pin_status(true, "Pin Debug - CLIENT side");
            redraw_counter = 0;
        }
        rg_task_delay(50);
    }

    // Cleanup: release all pins
    for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
        gpio_reset_pin(debug_pins[i].num);
    }
}

// ===========================================================================
// Host session wrapper (called from emulator game menu)
// ===========================================================================

void rg_coplay_host_start(void)
{
    RG_LOGI("coplay: Starting host pin debug mode");

    // Reset all pins to a clean input state first
    selected_pin_idx = 0;
    for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
        pin_modes[i] = PIN_MODE_INPUT;
        gpio_reset_pin(debug_pins[i].num);
        gpio_set_direction(debug_pins[i].num, GPIO_MODE_INPUT);
        gpio_set_pull_mode(debug_pins[i].num, GPIO_FLOATING);
    }
    usb_pins_release();
    coplay_initialized = false;
    is_client = false;

    draw_pin_status(false, "Pin Debug - HOST side");

    // Pure pin debug loop — no SPI init, no handshake, no connection
    while (true) {
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_UP) {
            selected_pin_idx = (selected_pin_idx - 1 + DEBUG_PIN_COUNT) % DEBUG_PIN_COUNT;
            draw_pin_status(false, "Pin Debug - HOST side");
            rg_task_delay(150);
            continue;
        }
        if (joystick & RG_KEY_DOWN) {
            selected_pin_idx = (selected_pin_idx + 1) % DEBUG_PIN_COUNT;
            draw_pin_status(false, "Pin Debug - HOST side");
            rg_task_delay(150);
            continue;
        }
        if (joystick & RG_KEY_A) {
            pin_modes[selected_pin_idx] = PIN_MODE_OUT_HIGH;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(false, "Pin Debug - HOST side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_B) {
            pin_modes[selected_pin_idx] = PIN_MODE_OUT_LOW;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(false, "Pin Debug - HOST side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_SELECT) {
            pin_modes[selected_pin_idx] = PIN_MODE_INPUT;
            apply_pin_mode(selected_pin_idx);
            draw_pin_status(false, "Pin Debug - HOST side");
            rg_task_delay(100);
            continue;
        }
        if (joystick & RG_KEY_START) {
            // Reset all pins to input (high-Z)
            for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
                pin_modes[i] = PIN_MODE_INPUT;
                apply_pin_mode(i);
            }
            draw_pin_status(false, "Pin Debug - All pins reset to INPUT");
            rg_task_delay(200);
            continue;
        }
        if (joystick & RG_KEY_MENU) {
            break;
        }

        // Redraw periodically for live level updates
        static int redraw_counter = 0;
        if (++redraw_counter >= 8) { // ~400ms
            draw_pin_status(false, "Pin Debug - HOST side");
            redraw_counter = 0;
        }
        rg_task_delay(50);
    }

    // Cleanup: release all pins
    for (int i = 0; i < DEBUG_PIN_COUNT; i++) {
        gpio_reset_pin(debug_pins[i].num);
    }
}

#else // !defined(RG_NET_SPI_HOST)

// Stub implementations when no SPI host configured
void rg_coplay_host_init(void) {}
bool rg_coplay_wait_for_client(int timeout_ms) { return false; }
void rg_coplay_send_frame(uint16_t *frame_buffer, int width, int height, coplay_gamepad_t *client_input) {}
bool rg_coplay_is_client_connected(void) { return false; }
void rg_coplay_host_deinit(void) {}
void rg_coplay_host_start(void) {}
void rg_coplay_client_init(void) {}
bool rg_coplay_wait_for_host(int timeout_ms) { return false; }
bool rg_coplay_recv_frame(uint16_t *frame_buffer, int max_size, int *width, int *height) { return false; }
void rg_coplay_send_input(coplay_gamepad_t gamepad) {}
void rg_coplay_client_deinit(void) {}
void rg_coplay_client_run(void) {}

#endif // defined(RG_NET_SPI_HOST)
