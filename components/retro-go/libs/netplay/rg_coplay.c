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

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_NET_MOSI,
        .miso_io_num = RG_NET_MISO,
        .sclk_io_num = RG_NET_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = COPLAY_FRAME_SIZE,
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

    // Handshake GPIO (input, pulled down - host drives it high when active)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RG_NET_HS),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
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
            // Try to receive handshake
            uint8_t type, seq;
            uint16_t len;
            if (client_recv_packet(&type, &seq, NULL, &len)) {
                if (type == COPLAY_PKT_HANDSHAKE) {
                    coplay_connected = true;
                    RG_LOGI("coplay: Host detected!");
                    return true;
                }
            }
        }

        attempt++;
        if (attempt % 20 == 0) {
            RG_COPLAY_LOG("coplay: Waiting for host... (attempt %d)", attempt);
        }
        rg_task_delay(50);
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

    rg_display_clear(C_BLACK);
    rg_gui_draw_message("CoPlay Client\n\nInitializing...");

    // Initialize SPI as client
    rg_coplay_client_init();
    if (!coplay_initialized) {
        rg_gui_draw_message("CoPlay Client\n\nSPI init failed!");
        rg_task_delay(2000);
        return;
    }

    // Create a surface for received frames
    rg_surface_t *display_surf = rg_surface_create(COPLAY_FRAME_WIDTH, COPLAY_FRAME_HEIGHT,
                                                    RG_PIXEL_565_LE, MEM_SLOW);
    if (!display_surf) {
        rg_gui_draw_message("CoPlay Client\n\nOut of memory!");
        rg_coplay_client_deinit();
        rg_task_delay(2000);
        return;
    }

    // Set display geometry to match game resolution
    rg_display_set_geometry(COPLAY_FRAME_WIDTH, COPLAY_FRAME_HEIGHT,
                            &(rg_margins_t){0, 0, 0, 0});

    // Wait for host
    rg_gui_draw_message("CoPlay Client\n\nWaiting for host...\n(Press B to cancel)");

    int64_t start_wait = rg_system_timer();
    bool host_found = false;

    while (rg_system_timer() - start_wait < 30000000) { // 30s timeout
        host_found = rg_coplay_wait_for_host(100);
        if (host_found) break;

        // Check for cancel
        if (rg_input_read_gamepad() & RG_KEY_B) {
            break;
        }
    }

    if (!host_found) {
        rg_gui_draw_message("CoPlay Client\n\nConnection cancelled!");
        rg_task_delay(1500);
        rg_surface_free(display_surf);
        rg_coplay_client_deinit();
        return;
    }

    // Main client loop
    RG_LOGI("coplay: Client connected, entering main loop");
    rg_display_clear(C_BLACK);
    rg_gui_draw_message("CoPlay Client\n\nConnected!");
    rg_task_delay(500);

    uint32_t frames_received = 0;
    int64_t fps_timer = rg_system_timer();

    while (coplay_connected) {
        // Receive frame
        uint16_t *fb = (uint16_t *)display_surf->data;
        int rx_w, rx_h;
        if (!rg_coplay_recv_frame(fb, COPLAY_FRAME_SIZE, &rx_w, &rx_h)) {
            break;
        }
        // Update surface dimensions if they changed
        if (rx_w != display_surf->width || rx_h != display_surf->height) {
            rg_display_set_geometry(rx_w, rx_h, &(rg_margins_t){0, 0, 0, 0});
            display_surf->width = rx_w;
            display_surf->height = rx_h;
            display_surf->stride = rx_w * 2;
        }

        // Display the received frame
        rg_display_submit(display_surf, 0);

        // Read local input and send to host
        coplay_gamepad_t gamepad = (coplay_gamepad_t)rg_input_read_gamepad();
        rg_coplay_send_input(gamepad);

        frames_received++;

        // FPS counter (every ~60 frames)
        if (frames_received % 60 == 0) {
            int64_t elapsed = rg_system_timer() - fps_timer;
            RG_LOGI("coplay: Client FPS=%.1f", 60000000.0 / elapsed);
            fps_timer = rg_system_timer();
        }

        // Check for exit (B + SELECT together)
        if (gamepad & RG_KEY_B && gamepad & RG_KEY_START) {
            RG_LOGI("coplay: Client exiting on user request");
            break;
        }
    }

    RG_LOGI("coplay: Client disconnected after %ld frames", frames_received);

    rg_gui_draw_message("CoPlay Client\n\nDisconnected.");
    rg_task_delay(1000);

    rg_surface_free(display_surf);
    rg_coplay_client_deinit();
}

// ===========================================================================
// Host session wrapper (called from emulator game menu)
// ===========================================================================

void rg_coplay_host_start(void)
{
    RG_LOGI("coplay: Starting host session from game menu");

    rg_display_clear(C_BLACK);
    rg_gui_draw_message("CoPlay Host\n\nInitializing...");

    rg_coplay_host_init();

    if (!coplay_initialized) {
        rg_gui_draw_message("CoPlay Host\n\nSPI init failed!");
        rg_task_delay(1500);
        return;
    }

    // Wait for client with cancel option
    rg_gui_draw_message("CoPlay Host\n\nWaiting for client...\n(Press B to cancel)");

    int64_t deadline = rg_system_timer() + 60000000; // 60s timeout
    bool connected = false;

    while (rg_system_timer() < deadline) {
        connected = rg_coplay_wait_for_client(500);
        if (connected) break;

        if (rg_input_read_gamepad() & RG_KEY_B) {
            break;
        }

        // Update the waiting indicator
        static int dot_count = 0;
        dot_count = (dot_count + 1) % 4;
        char msg[64];
        snprintf(msg, sizeof(msg), "CoPlay Host\n\nWaiting for client%s",
                 "." + (4 - dot_count));
        rg_gui_draw_message(msg);
    }

    if (!connected) {
        rg_gui_draw_message("CoPlay Host\n\nCancelled.");
        rg_task_delay(1000);
        rg_coplay_host_deinit();
        return;
    }

    // Client connected! Set up P2 input and return to emulator loop.
    // The emulator loop will call rg_coplay_send_frame() each frame.
    rg_display_clear(C_BLACK);
    rg_gui_draw_message("CoPlay Host\n\nClient connected!\n\nP2 Ready!");
    rg_task_delay(1000);
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
