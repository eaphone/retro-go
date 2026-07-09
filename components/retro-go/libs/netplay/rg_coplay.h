#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// CoPlay - SPI Direct-Connect Multiplayer for Retro-Go
// Two ESP32-P4 devices connected via SPI for 2-player gaming
// ============================================================

#define COPLAY_FRAME_WIDTH     320
#define COPLAY_FRAME_HEIGHT    240
#define COPLAY_FRAME_SIZE      (COPLAY_FRAME_WIDTH * COPLAY_FRAME_HEIGHT * 2) // 153600 bytes RGB565
#define COPLAY_CHUNK_SIZE      4092

// Protocol packet types
#define COPLAY_PKT_HANDSHAKE   0x01  // Host → Client: handshake request
#define COPLAY_PKT_HANDSHAKE_ACK 0x02 // Client → Host: handshake ack
#define COPLAY_PKT_FRAME_CHUNK 0x03  // Host → Client: frame data chunk
#define COPLAY_PKT_FRAME_DONE  0x04  // Host → Client: frame complete, display it
#define COPLAY_PKT_INPUT_DATA  0x05  // Client → Host: gamepad input data

// Packet header (4 bytes)
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t seq;
    uint16_t len;  // payload length (bytes)
} coplay_packet_hdr_t;

#define COPLAY_HDR_SIZE  (sizeof(coplay_packet_hdr_t)) // 4 bytes

// Gamepad state (2 bytes, same bit layout as RG_KEY_*)
typedef uint16_t coplay_gamepad_t;

// ============================================================
// Host APIs (called from emulator core)
// ============================================================

/**
 * Initialize SPI as host (master).
 * Must be called before any other coplay functions.
 */
void rg_coplay_host_init(void);

/**
 * Wait for client to connect.
 * @param timeout_ms  Timeout in ms, -1 = infinite
 * @return true if client connected, false on timeout
 */
bool rg_coplay_wait_for_client(int timeout_ms);

/**
 * Send one video frame to the client.
 * Also retrieves client's gamepad input in the same transaction.
 * @param frame_buffer  RGB565 LE frame buffer
 * @param width         Frame width in pixels
 * @param height        Frame height in pixels
 * @param client_input  [out] Receives the client's gamepad state
 */
void rg_coplay_send_frame(uint16_t *frame_buffer, int width, int height, coplay_gamepad_t *client_input);

/**
 * Check if client is still connected.
 */
bool rg_coplay_is_client_connected(void);

/**
 * Disconnect client and shut down host.
 */
void rg_coplay_host_deinit(void);

/**
 * rg_coplay_host_start - Full host session: wait for client, then wrap the emulator loop.
 * Called from the emulator game menu.
 * Returns when the user exits coplay mode.
 */
void rg_coplay_host_start(void);

// ============================================================
// Client APIs (called from coplay client app)
// ============================================================

/**
 * Initialize SPI as client (slave).
 */
void rg_coplay_client_init(void);

/**
 * Wait for host to start sending frames.
 * @param timeout_ms  Timeout in ms, -1 = infinite
 * @return true if host detected, false on timeout
 */
bool rg_coplay_wait_for_host(int timeout_ms);

/**
 * Receive one frame from host. Blocks until frame is complete.
 * @param frame_buffer  Buffer to receive RGB565 LE frame (allocated by caller)
 * @param max_size      Maximum bytes frame_buffer can hold
 * @param width         [out] Received frame width
 * @param height        [out] Received frame height
 * @return true if frame received, false on error/disconnect
 */
bool rg_coplay_recv_frame(uint16_t *frame_buffer, int max_size, int *width, int *height);

/**
 * Send local gamepad input to the host.
 * The input is queued and sent with the next SPI transaction.
 * @param gamepad  Gamepad state bitmask (RG_KEY_* format)
 */
void rg_coplay_send_input(coplay_gamepad_t gamepad);

/**
 * rg_coplay_client_run - Main loop for the CoPlay client.
 *
 * Initializes SPI slave, waits for host, then enters a loop:
 *   receive frame -> display -> read input -> send input
 * Call this from the launcher or a dedicated app.
 * Returns when user presses B+START or connection is lost.
 */
void rg_coplay_client_run(void);

/**
 * Disconnect and shut down client.
 */
void rg_coplay_client_deinit(void);

#ifdef __cplusplus
}
#endif
