/**
 * Unified PS/2 Driver for RP2350
 * 
 * Single PIO program shared between keyboard and mouse state machines.
 * Uses interrupt-driven streaming mode for performant, non-blocking operation.
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ps2.h"
#include "ps2.pio.h"

#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <string.h>
#include <stdio.h>

//=============================================================================
// PS/2 Mouse Commands
//=============================================================================

#define PS2_CMD_RESET             0xFF
#define PS2_CMD_RESEND            0xFE
#define PS2_CMD_SET_DEFAULTS      0xF6
#define PS2_CMD_DISABLE_STREAM    0xF5
#define PS2_CMD_ENABLE_STREAM     0xF4
#define PS2_CMD_SET_SAMPLE_RATE   0xF3
#define PS2_CMD_GET_DEVICE_ID     0xF2
#define PS2_CMD_SET_REMOTE        0xF0
#define PS2_CMD_READ_DATA         0xEB
#define PS2_CMD_SET_RESOLUTION    0xE8
#define PS2_CMD_SET_SCALING_1_1   0xE6

#define PS2_RESP_ACK              0xFA
#define PS2_RESP_RESEND           0xFE
#define PS2_RESP_ERROR            0xFC
#define PS2_RESP_BAT_OK           0xAA

//=============================================================================
// Driver State
//=============================================================================

// Enforce a minimum gap between host-to-device bytes. Without this, on
// USB_HID=1 release builds, the mouse rejects back-to-back bytes with
// 0xFC. See MOUSE_FIX.md.
static uint32_t mouse_last_tx_us = 0;
#define MOUSE_INTER_BYTE_GAP_US 50000

static PIO ps2_pio = NULL;   // mouse PIO (primary; used by mouse IRQ/FIFO paths)
static uint ps2_program_offset = 0;

// Keyboard state machine — when kbd and mouse live on different PIOs, kbd
// uses kbd_pio; otherwise kbd_pio == ps2_pio.
static PIO  kbd_pio = NULL;
static uint kbd_sm = 0;
static uint kbd_clk_pin = 0;
static bool kbd_initialized = false;

// Mouse state machine
static uint mouse_sm = 0;
static uint mouse_clk_pin = 0;
static uint mouse_data_pin = 0;
static bool mouse_pio_initialized = false;

// Mouse device state
static ps2_mouse_state_t mouse_state = {0};
static uint8_t mouse_packet[4];
static uint8_t mouse_packet_idx = 0;
static uint8_t mouse_packet_size = 3;

// Ring buffer for interrupt-driven reception
#define MOUSE_RX_BUFFER_SIZE 128
static volatile uint8_t mouse_rx_buffer[MOUSE_RX_BUFFER_SIZE];
static volatile uint8_t mouse_rx_head = 0;  // ISR writes here
static volatile uint8_t mouse_rx_tail = 0;  // Main loop reads from here
static volatile bool mouse_streaming = false;

// Error counters
static uint32_t mouse_frame_errors = 0;
static uint32_t mouse_parity_errors = 0;
static uint32_t mouse_sync_errors = 0;

// Debug counters (total bytes/packets seen since init)
static volatile uint32_t mouse_raw_bytes = 0;
static uint32_t mouse_valid_packets_total = 0;

//=============================================================================
// PIO Helpers
//=============================================================================

static void pio_sm_stop(PIO pio, uint sm) {
    pio_sm_set_enabled(pio, sm, false);
}

static void pio_sm_restart_rx(PIO pio, uint sm) {
    // Clear FIFOs, restart SM, and jump to program start
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    // Jump to program start (wrap_target = offset 0)
    pio_sm_exec(pio, sm, pio_encode_jmp(ps2_program_offset));
    pio_sm_set_enabled(pio, sm, true);
}

//=============================================================================
// Mouse PIO Interrupt Handler
//=============================================================================

// Drain mouse PIO RX FIFO into the ring buffer. Safe to call from both the
// PIO IRQ handler AND from the main-loop poll path — on RP2350 the mouse
// PIO IRQ is unreliable for us (see ps2_mouse_poll), so we rely on the
// poll-path call as the primary source and keep the IRQ as a backup.
static void mouse_pio_drain(void) {
    while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
        uint32_t raw = pio_sm_get(ps2_pio, mouse_sm);

        // Skip all-zero frames (noise/glitch)
        if (raw == 0) continue;

        // Decode frame
        int result = ps2_rx_decode_frame(raw);
        if (result >= 0) {
            mouse_raw_bytes++;
            // Valid byte - add to ring buffer
            uint8_t next_head = (mouse_rx_head + 1) % MOUSE_RX_BUFFER_SIZE;
            if (next_head != mouse_rx_tail) {  // Not full
                mouse_rx_buffer[mouse_rx_head] = (uint8_t)result;
                mouse_rx_head = next_head;
            }
            // If buffer full, drop the byte (better than blocking ring)
        } else {
            // Track errors but don't block
            if (result == -1) mouse_frame_errors++;
            else mouse_parity_errors++;
        }
    }
}

static void mouse_pio_irq_handler(void) {
    mouse_pio_drain();
}

static uint mouse_irq_num(void) {
    if (ps2_pio == pio0) return PIO0_IRQ_1;
    if (ps2_pio == pio1) return PIO1_IRQ_1;
#ifdef PIO2_IRQ_1
    if (ps2_pio == pio2) return PIO2_IRQ_1;
#endif
    return PIO0_IRQ_1;
}

static void mouse_enable_irq(void) {
    // Use IRQ_1 to avoid conflicts with other drivers that might use IRQ_0
    uint irq_num = mouse_irq_num();

    // Enable RXNEMPTY interrupt for mouse state machine on IRQ index 1
    pio_set_irqn_source_enabled(ps2_pio, 1, pis_sm0_rx_fifo_not_empty + mouse_sm, true);

    // Set up interrupt handler
    irq_set_exclusive_handler(irq_num, mouse_pio_irq_handler);
    irq_set_enabled(irq_num, true);
}

static void mouse_disable_irq(void) {
    uint irq_num = mouse_irq_num();

    // Disable interrupt
    irq_set_enabled(irq_num, false);
    pio_set_irqn_source_enabled(ps2_pio, 1, pis_sm0_rx_fifo_not_empty + mouse_sm, false);
}

// Get byte from ring buffer (non-blocking, called from main loop)
static int mouse_rx_get_byte(void) {
    if (mouse_rx_head == mouse_rx_tail) {
        return -1;  // Buffer empty
    }
    uint8_t data = mouse_rx_buffer[mouse_rx_tail];
    mouse_rx_tail = (mouse_rx_tail + 1) % MOUSE_RX_BUFFER_SIZE;
    return data;
}

// Check how many bytes available in ring buffer
static uint8_t mouse_rx_available(void) {
    return (mouse_rx_head - mouse_rx_tail + MOUSE_RX_BUFFER_SIZE) % MOUSE_RX_BUFFER_SIZE;
}

//=============================================================================
// Mouse Host-to-Device Communication (bit-bang while PIO stopped)
//=============================================================================

static inline void mouse_clk_low(void) {
    gpio_set_dir(mouse_clk_pin, GPIO_OUT);
    gpio_put(mouse_clk_pin, 0);
}

static inline void mouse_clk_release(void) {
    gpio_set_dir(mouse_clk_pin, GPIO_IN);
}

static inline void mouse_data_low(void) {
    gpio_set_dir(mouse_data_pin, GPIO_OUT);
    gpio_put(mouse_data_pin, 0);
}

static inline void mouse_data_high(void) {
    gpio_set_dir(mouse_data_pin, GPIO_OUT);
    gpio_put(mouse_data_pin, 1);
}

static inline void mouse_data_release(void) {
    gpio_set_dir(mouse_data_pin, GPIO_IN);
}

static inline bool mouse_read_clk(void) {
    return gpio_get(mouse_clk_pin);
}

static inline bool mouse_read_data(void) {
    return gpio_get(mouse_data_pin);
}

static bool mouse_wait_clk(bool state, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(mouse_clk_pin) != state) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

static bool mouse_wait_data(bool state, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(mouse_data_pin) != state) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

static uint8_t calc_odd_parity(uint8_t data) {
    uint8_t parity = 1;
    while (data) {
        parity ^= (data & 1);
        data >>= 1;
    }
    return parity;
}

/**
 * Send a byte to mouse using PS/2 host-to-device protocol.
 * Stops PIO and disables interrupt during transmission.
 */
static bool mouse_send_byte(uint8_t data) {
    uint8_t parity = calc_odd_parity(data);

    // Enforce inter-byte gap (see MOUSE_FIX.md).
    if (mouse_last_tx_us) {
        uint32_t elapsed = time_us_32() - mouse_last_tx_us;
        if (elapsed < MOUSE_INTER_BYTE_GAP_US) {
            busy_wait_us_32(MOUSE_INTER_BYTE_GAP_US - elapsed);
        }
    }

    // Disable interrupt and stop PIO to take over GPIO
    if (mouse_streaming) {
        mouse_disable_irq();
    }
    pio_sm_set_enabled(ps2_pio, mouse_sm, false);
    
    // Re-init GPIO for bit-bang (switch from PIO to SIO)
    gpio_init(mouse_clk_pin);
    gpio_init(mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    gpio_set_dir(mouse_clk_pin, GPIO_IN);
    gpio_set_dir(mouse_data_pin, GPIO_IN);
    
    // Wait for bus idle (both CLK and DATA high). Some slower/older mice
    // take longer to release the lines after BAT or after sending us a byte,
    // so poll for up to ~5 ms before we assume the bus is ours.
    {
        absolute_time_t idle_deadline = make_timeout_time_us(5000);
        while (!time_reached(idle_deadline)) {
            if (gpio_get(mouse_clk_pin) && gpio_get(mouse_data_pin)) break;
        }
    }
    sleep_us(50);

    // Mask NVIC for the timing-sensitive host-to-device frame.
    // See MOUSE_FIX.md for why.
    uint32_t irq_save = save_and_disable_interrupts();
    bool ok = false;

    // 1. Inhibit communication - pull clock low >100us
    mouse_clk_low();
    busy_wait_us_32(150);

    // 2. Request-to-send - pull data low
    mouse_data_low();
    busy_wait_us_32(10);

    // 3. Release clock - device will start clocking
    mouse_clk_release();

    // 4. Wait for device to pull clock low
    if (!mouse_wait_clk(false, 15000)) {
        mouse_data_release();
        restore_interrupts(irq_save);
        goto restart_pio;
    }

    // 5. Send 8 data bits on falling clock edges
    for (int i = 0; i < 8; i++) {
        if (data & (1 << i)) {
            mouse_data_release();
        } else {
            mouse_data_low();
        }
        if (!mouse_wait_clk(true, 5000)) goto fail_irq_disabled;
        if (!mouse_wait_clk(false, 5000)) goto fail_irq_disabled;
    }

    // 6. Send parity bit
    if (parity) {
        mouse_data_release();
    } else {
        mouse_data_low();
    }
    if (!mouse_wait_clk(true, 5000)) goto fail_irq_disabled;
    if (!mouse_wait_clk(false, 5000)) goto fail_irq_disabled;

    // 7. Release data for stop bit
    mouse_data_release();
    if (!mouse_wait_clk(true, 5000)) goto fail_irq_disabled;

    // 8. Wait for ACK (device pulls data low)
    if (!mouse_wait_data(false, 5000)) goto fail_irq_disabled;
    if (!mouse_wait_clk(false, 5000)) goto fail_irq_disabled;
    if (!mouse_wait_clk(true, 5000)) goto fail_irq_disabled;
    if (!mouse_wait_data(true, 5000)) goto fail_irq_disabled;

    ok = true;

fail_irq_disabled:
    restore_interrupts(irq_save);

    if (!ok) {
        mouse_data_release();
        mouse_clk_release();
        goto restart_pio;
    }

    // Wait for bus idle before handing pins back to PIO (see MOUSE_FIX.md).
    for (int spin = 0; spin < 100; spin++) {
        if (gpio_get(mouse_clk_pin) && gpio_get(mouse_data_pin)) break;
        busy_wait_us_32(1);
    }

    // Atomic SIO → PIO handoff under NVIC mask.
    uint32_t handoff_irq = save_and_disable_interrupts();
    pio_sm_set_enabled(ps2_pio, mouse_sm, false);
    pio_gpio_init(ps2_pio, mouse_clk_pin);
    pio_gpio_init(ps2_pio, mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    pio_sm_clear_fifos(ps2_pio, mouse_sm);
    pio_sm_exec(ps2_pio, mouse_sm, pio_encode_jmp(ps2_program_offset));
    pio_sm_set_enabled(ps2_pio, mouse_sm, true);
    restore_interrupts(handoff_irq);

    if (mouse_streaming) {
        mouse_enable_irq();
    }
    mouse_last_tx_us = time_us_32();
    return true;

restart_pio:
    pio_gpio_init(ps2_pio, mouse_clk_pin);
    pio_gpio_init(ps2_pio, mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    pio_sm_restart_rx(ps2_pio, mouse_sm);

    if (mouse_streaming) {
        mouse_enable_irq();
    }
    mouse_last_tx_us = time_us_32();
    return false;
}

/**
 * Get a byte from mouse PIO FIFO with timeout.
 */
static int mouse_get_byte(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    
    while (!time_reached(deadline)) {
        if (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
            uint32_t raw = pio_sm_get(ps2_pio, mouse_sm);
            
            // Skip all-zero frames (noise/glitch)
            if (raw == 0) {
                continue;
            }
            
            int result = ps2_rx_decode_frame(raw);
            if (result >= 0) {
                return result;
            }
            // Frame/parity error - try next
            if (result == -1) mouse_frame_errors++;
            else mouse_parity_errors++;
        }
        sleep_us(100);
    }
    return -1;
}

/**
 * Send command and wait for ACK.
 *
 * Per the PS/2 spec the device may reply with 0xFE (RESEND) if it decoded
 * the host byte incorrectly. In practice some older/no-name mice instead
 * reply 0xFC (ERROR) for the same condition post-BAT, so we treat both
 * the same and retry the byte. We also retry if the RTS step itself
 * didn't get a response (send_byte failure), which happens when the
 * mouse briefly holds the lines after reset or after its own error byte.
 *
 * Before each attempt we drain any stale bytes sitting in the PIO RX
 * FIFO so we don't consume a leftover error/ACK from a previous command
 * as the response to this one.
 */
static bool mouse_send_command(uint8_t cmd) {
    // Two tries is enough. PS/2 devices in an error state need tens of
    // milliseconds to recover; our earlier 4-retry loop at 5 ms flooded
    // some mice into a locked state that even a subsequent reset
    // couldn't recover from. If two paced retries don't work, the
    // caller should escalate (usually via bus recovery + full reset).
    const int MAX_TRIES = 2;
    int resp = -1;
    for (int attempt = 0; attempt < MAX_TRIES; attempt++) {
        // Flush any stragglers from a previous (possibly failed) exchange.
        while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
            pio_sm_get(ps2_pio, mouse_sm);
        }

        if (!mouse_send_byte(cmd)) {
            if (attempt + 1 < MAX_TRIES) {
                sleep_ms(30);
                continue;
            }
            printf("Mouse: send_byte(0x%02X) failed\n", cmd);
            return false;
        }

        resp = mouse_get_byte(100);
        if (resp == PS2_RESP_ACK) {
            return true;
        }
        if (resp == PS2_RESP_RESEND || resp == PS2_RESP_ERROR) {
            // Device asks us to resend (0xFE) or reports a decode error
            // (0xFC); give it time to fully return to the idle state
            // before retrying the same byte.
            sleep_ms(30);
            continue;
        }
        // Timeout or unknown byte — bail out.
        break;
    }
    if (resp < 0) {
        printf("Mouse: cmd 0x%02X timed out waiting for ACK\n", cmd);
    } else {
        printf("Mouse: cmd 0x%02X got 0x%02X (expected ACK)\n",
               cmd, (unsigned)resp & 0xFF);
    }
    return false;
}

/**
 * Send command with parameter.
 */
static bool mouse_send_command_param(uint8_t cmd, uint8_t param) {
    if (!mouse_send_command(cmd)) return false;
    return mouse_send_command(param);
}

//=============================================================================
// Mouse Packet Processing
//=============================================================================

// Track when button was last pressed/released for timeout
static uint32_t mouse_button_press_time = 0;
static uint8_t mouse_last_buttons = 0;
static uint32_t mouse_valid_packet_count = 0;

static void mouse_process_packet(void) {
    uint8_t status = mouse_packet[0];

    // Validate sync bit (bit 3 must be 1)
    if (!(status & 0x08)) {
        mouse_sync_errors++;
        return;
    }

    // Skip on overflow - data is unreliable
    if (status & 0xC0) {
        return;
    }

    // Get raw movement bytes
    uint8_t x_raw = mouse_packet[1];
    uint8_t y_raw = mouse_packet[2];

    // Sign bits from status byte (9th bit for 9-bit two's complement)
    bool x_neg = (status & 0x10) != 0;
    bool y_neg = (status & 0x20) != 0;

    // 9-bit two's complement: range is -256 to +255
    int16_t dx = x_neg ? ((int16_t)x_raw - 256) : (int16_t)x_raw;
    int16_t dy = y_neg ? ((int16_t)y_raw - 256) : (int16_t)y_raw;

    // Sanity check: a 200 Hz PS/2 mouse physically cannot produce
    // |delta| > ~100 counts per sample at resolution 3. Values near the
    // 9-bit edges (sign bit set with tiny raw byte, i.e. dx ≈ -255) are
    // almost always mis-framed packets slipping past parity — squash
    // them to zero so motion stops drifting, but keep processing the
    // packet so the button state still updates.
    const int16_t MAX_ABS_DELTA = 100;
    if (dx >  MAX_ABS_DELTA || dx < -MAX_ABS_DELTA) dx = 0;
    if (dy >  MAX_ABS_DELTA || dy < -MAX_ABS_DELTA) dy = 0;

    // Track valid packets for debugging
    mouse_valid_packet_count++;
    mouse_valid_packets_total++;

    // Extract button state from packet
    uint8_t new_buttons = status & 0x07;

    // Debug: Log button state changes
    if (new_buttons != mouse_last_buttons) {
        printf("PS2: Button state changed: 0x%02X -> 0x%02X (packet #%lu, status=0x%02X)\n",
               mouse_last_buttons, new_buttons, mouse_valid_packet_count, status);
        mouse_last_buttons = new_buttons;

        // Track when buttons are pressed for timeout detection
        if (new_buttons != 0) {
            mouse_button_press_time = time_us_32();
        }
    }

    // Update button state
    mouse_state.buttons = new_buttons;

    // Accumulate deltas with overflow protection (clamp to int16_t range)
    int32_t new_dx = (int32_t)mouse_state.delta_x + dx;
    int32_t new_dy = (int32_t)mouse_state.delta_y + dy;
    if (new_dx > 32767) new_dx = 32767;
    if (new_dx < -32768) new_dx = -32768;
    if (new_dy > 32767) new_dy = 32767;
    if (new_dy < -32768) new_dy = -32768;
    mouse_state.delta_x = (int16_t)new_dx;
    mouse_state.delta_y = (int16_t)new_dy;

    // Wheel (IntelliMouse)
    if (mouse_packet_size == 4) {
        int8_t wheel = (int8_t)(mouse_packet[3] & 0x0F);
        if (wheel & 0x08) wheel |= 0xF0;
        mouse_state.wheel += wheel;
    }
}

//=============================================================================
// Mouse Device Initialization
//=============================================================================

/**
 * Force the PS/2 bus into a clean idle state. Used between full-init
 * attempts to unstick a mouse that's gotten wedged mid-transaction.
 *
 * Per the PS/2 "Communication Inhibit" pattern: the host holds CLK low
 * for an extended period (spec says >100 µs; we use 20 ms to clobber any
 * device that's halfway through clocking out a byte), then releases
 * both lines and lets the bus settle.
 */
static void mouse_bus_recover(void) {
    if (mouse_streaming) {
        mouse_disable_irq();
    }
    pio_sm_set_enabled(ps2_pio, mouse_sm, false);

    gpio_init(mouse_clk_pin);
    gpio_init(mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);

    // Drive CLK low long enough to abort any device-side transaction.
    gpio_set_dir(mouse_clk_pin, GPIO_OUT);
    gpio_put(mouse_clk_pin, 0);
    gpio_set_dir(mouse_data_pin, GPIO_IN);
    sleep_ms(20);

    // Release both lines and let pull-ups take over.
    gpio_set_dir(mouse_clk_pin, GPIO_IN);
    sleep_ms(30);

    // Hand pins back to PIO and restart the RX state machine.
    pio_gpio_init(ps2_pio, mouse_clk_pin);
    pio_gpio_init(ps2_pio, mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    pio_sm_clear_fifos(ps2_pio, mouse_sm);
    pio_sm_exec(ps2_pio, mouse_sm, pio_encode_jmp(ps2_program_offset));
    pio_sm_set_enabled(ps2_pio, mouse_sm, true);
}

static bool mouse_enable_intellimouse(void) {
    // Magic knock: three sample-rate writes (200, 100, 80) followed by
    // GET_DEVICE_ID. IntelliMouse-capable devices answer 0x03 (wheel)
    // or 0x04 (5-button), legacy devices stay at 0x00.
    //
    // If any step here fails we abandon the probe and leave the mouse
    // in a plain 3-byte packet mode — don't try to "fix" it, the caller
    // will continue with basic configuration.
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 200)) return false;
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 100)) return false;
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 80)) return false;

    if (!mouse_send_command(PS2_CMD_GET_DEVICE_ID)) return false;

    int id = mouse_get_byte(100);
    printf("Mouse: Device ID after magic: 0x%02X\n", id);

    if (id == 0x03 || id == 0x04) {
        mouse_packet_size = 4;
        mouse_state.has_wheel = 1;
        return true;
    }
    return false;
}

// Set by mouse_reset_and_init() when the device sent at least one byte
// in response to our RESET. Lets the outer retry loop distinguish
// "mouse connected but init failed" (retry worth) from "no mouse on
// the bus" (bail immediately).
static bool mouse_saw_response = false;

static bool mouse_reset_and_init(void) {
    printf("Mouse: Sending reset...\n");
    mouse_saw_response = false;

    // Drain any garbage from FIFO first
    while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
        pio_sm_get(ps2_pio, mouse_sm);
    }

    if (!mouse_send_byte(PS2_CMD_RESET)) {
        printf("Mouse: Reset send failed\n");
        return false;
    }

    // Mouse reset takes 300-500ms for self-test
    // Wait for ACK (0xFA), then BAT OK (0xAA), then device ID (0x00)
    int resp = mouse_get_byte(2000);  // Longer timeout for reset
    printf("Mouse: Response 1: 0x%02X\n", resp);
    if (resp >= 0) mouse_saw_response = true;

    if (resp == PS2_RESP_ACK) {
        // Got ACK, wait for BAT
        resp = mouse_get_byte(2000);
        printf("Mouse: Response 2: 0x%02X\n", resp);
        if (resp >= 0) mouse_saw_response = true;
    }

    if (resp != PS2_RESP_BAT_OK) {
        printf("Mouse: BAT failed (got 0x%02X)\n", resp);
        return false;
    }
    
    // Get device ID
    int id = mouse_get_byte(100);
    printf("Mouse: Device ID: 0x%02X\n", id);

    // Some mice need a breather between BAT completion and the first
    // real host-to-device command. Without this delay, cheap/older
    // mice lock up on the first 0xF3 (sample-rate) write. 50 ms is
    // plenty for everything we've tested.
    sleep_ms(50);

    // Drain any straggler bytes the device may have queued during BAT.
    while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
        pio_sm_get(ps2_pio, mouse_sm);
    }

#ifdef USB_HID_ENABLED
    // On USB_HID=1 release builds, two-byte command sequences fail
    // with the device returning 0xFC. Skip the IntelliMouse probe and
    // the config writes; rely on post-BAT defaults (100 Hz, 4 counts/mm,
    // 3-byte packets). See MOUSE_FIX.md.
    printf("Mouse: using post-BAT defaults (HID=1 workaround)\n");
#else
    // On USB_HID=0 dev builds, multi-byte commands are reliable.
    // Apply the full original init so the mouse runs at the same
    // config as pre-fix builds — 200 Hz sample rate, 8 counts/mm,
    // 1:1 scaling. This matches the runtime behavior that historically
    // did NOT cause the welcome-screen HDMI resync.
    if (mouse_enable_intellimouse()) {
        printf("Mouse: IntelliMouse enabled\n");
    }
    mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 200);
    mouse_send_command_param(PS2_CMD_SET_RESOLUTION, 3);
    mouse_send_command(PS2_CMD_SET_SCALING_1_1);
#endif

    // Enable streaming mode FIRST (before enabling IRQ!)
    // The ACK for this command must be received via polling, not IRQ
    if (!mouse_send_command(PS2_CMD_ENABLE_STREAM)) {
        printf("Mouse: Enable stream failed\n");
        return false;
    }
    
    // Clear ring buffer before enabling interrupt reception
    mouse_rx_head = 0;
    mouse_rx_tail = 0;
    
    // NOW enable PIO interrupt for non-blocking reception of mouse data
    mouse_enable_irq();
    mouse_streaming = true;

    printf("Mouse: Streaming mode enabled with interrupts\n");
    return true;
}

//=============================================================================
// Public API - Initialization
//=============================================================================

bool ps2_init(PIO pio, uint kbd_clk, uint mouse_clk) {
    ps2_pio = pio;
    kbd_pio = pio;
    kbd_clk_pin = kbd_clk;
    mouse_clk_pin = mouse_clk;
    mouse_data_pin = mouse_clk + 1;
    
    printf("PS/2: Initializing on PIO%d\n", pio == pio0 ? 0 : 1);
    printf("PS/2: Keyboard CLK=%d DATA=%d\n", kbd_clk, kbd_clk + 1);
    printf("PS/2: Mouse CLK=%d DATA=%d\n", mouse_clk, mouse_clk + 1);
    
    // Add PIO program (only once)
    if (!pio_can_add_program(pio, &ps2_rx_program)) {
        printf("PS/2: Cannot add PIO program\n");
        return false;
    }
    ps2_program_offset = pio_add_program(pio, &ps2_rx_program);
    
    // Claim state machines
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2: No free SM for keyboard\n");
        return false;
    }
    kbd_sm = (uint)sm;
    
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2: No free SM for mouse\n");
        pio_sm_unclaim(pio, kbd_sm);
        return false;
    }
    mouse_sm = (uint)sm;
    
    printf("PS/2: Keyboard SM=%d, Mouse SM=%d\n", kbd_sm, mouse_sm);
    
    // Initialize keyboard state machine
    ps2_rx_program_init(pio, kbd_sm, ps2_program_offset, kbd_clk);
    kbd_initialized = true;
    
    // Initialize mouse state machine
    ps2_rx_program_init(pio, mouse_sm, ps2_program_offset, mouse_clk);
    mouse_pio_initialized = true;
    
    return true;
}

bool ps2_kbd_pio_init(PIO pio, uint kbd_clk) {
    kbd_pio = pio;
    kbd_clk_pin = kbd_clk;

    int pio_idx = (pio == pio0) ? 0 : (pio == pio1) ? 1 : 2;
    printf("PS/2 Kbd: Initializing on PIO%d\n", pio_idx);
    printf("PS/2 Kbd: CLK=%d DATA=%d\n", kbd_clk, kbd_clk + 1);

    if (!pio_can_add_program(pio, &ps2_rx_program)) {
        printf("PS/2 Kbd: Cannot add PIO program\n");
        return false;
    }
    uint kbd_program_offset = pio_add_program(pio, &ps2_rx_program);

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2 Kbd: No free SM\n");
        return false;
    }
    kbd_sm = (uint)sm;
    printf("PS/2 Kbd: SM=%d\n", kbd_sm);

    ps2_rx_program_init(pio, kbd_sm, kbd_program_offset, kbd_clk);
    kbd_initialized = true;
    return true;
}

bool ps2_mouse_pio_init(PIO pio, uint mouse_clk) {
    ps2_pio = pio;
    mouse_clk_pin = mouse_clk;
    mouse_data_pin = mouse_clk + 1;
    
    printf("PS/2 Mouse: Initializing on PIO%d\n", pio == pio0 ? 0 : 1);
    printf("PS/2 Mouse: CLK=%d DATA=%d\n", mouse_clk, mouse_clk + 1);
    
    // Add PIO program
    if (!pio_can_add_program(pio, &ps2_rx_program)) {
        printf("PS/2 Mouse: Cannot add PIO program\n");
        return false;
    }
    ps2_program_offset = pio_add_program(pio, &ps2_rx_program);
    
    // Claim state machine for mouse only
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2 Mouse: No free SM\n");
        return false;
    }
    mouse_sm = (uint)sm;
    
    printf("PS/2 Mouse: SM=%d\n", mouse_sm);
    
    // Initialize mouse state machine
    ps2_rx_program_init(pio, mouse_sm, ps2_program_offset, mouse_clk);
    mouse_pio_initialized = true;
    
    return true;
}

//=============================================================================
// Public API - Mouse
//=============================================================================

bool ps2_mouse_init_device(void) {
    if (!mouse_pio_initialized) {
        printf("Mouse: PIO not initialized\n");
        return false;
    }
    
    memset(&mouse_state, 0, sizeof(mouse_state));
    mouse_packet_idx = 0;
    mouse_packet_size = 3;
    mouse_frame_errors = 0;
    mouse_parity_errors = 0;
    mouse_sync_errors = 0;
    
    sleep_ms(100);
    
    // Check bus state
    printf("Mouse: CLK=%d DATA=%d\n", mouse_read_clk(), mouse_read_data());
    
    // Retry up to MAX_ATTEMPTS times, but bail early if the mouse
    // doesn't respond on the bus at all — a wedged-but-connected mouse
    // sometimes needs several full resets to unstick, while a missing
    // mouse should not hold up boot.
    const int MAX_ATTEMPTS = 10;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        printf("Mouse: Init attempt %d\n", attempt + 1);

        // Before every attempt except the first, force the bus back to
        // a known-idle state. A mouse that got wedged mid-transaction
        // on the previous attempt won't accept a fresh 0xFF (reset)
        // until we clock it out of its current state.
        if (attempt > 0) {
            mouse_bus_recover();
            sleep_ms(300);
        }

        // Clear FIFO
        while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
            pio_sm_get(ps2_pio, mouse_sm);
        }

        if (mouse_reset_and_init()) {
            mouse_state.initialized = 1;
            printf("Mouse: Init SUCCESS\n");
            return true;
        }

        // If the mouse never responded to RESET, stop retrying — it's
        // either not plugged in or the bus is broken. Repeating won't
        // help and only slows boot.
        if (!mouse_saw_response) {
            printf("Mouse: No device on bus, giving up\n");
            break;
        }

        sleep_ms(500);
    }

    printf("Mouse: Init FAILED\n");
    return false;
}

void ps2_mouse_poll(void) {
    // Drain the PIO RX FIFO from the main loop. On RP2350 + our board the
    // mouse PIO IRQ doesn't reliably deliver bytes into the ring buffer
    // (FIFO fills up, handler never runs), so the main loop is the
    // primary source. The IRQ is still armed as a backup.
    mouse_pio_drain();

    // Process bytes from the ring buffer
    // Limit processing to prevent runaway loops
    int bytes_processed = 0;
    const int MAX_BYTES_PER_POLL = 32;  // Max ~8 packets per poll

    while (mouse_rx_available() > 0 && bytes_processed < MAX_BYTES_PER_POLL) {
        int data = mouse_rx_get_byte();
        if (data < 0) break;
        bytes_processed++;

        // If we're starting a new packet, validate sync bit
        if (mouse_packet_idx == 0) {
            // Byte 0 must have bit 3 set (sync bit)
            if (!(data & 0x08)) {
                mouse_sync_errors++;
                // If too many sync errors, reset packet state
                if (mouse_sync_errors > 100) {
                    mouse_sync_errors = 0;
                    mouse_packet_idx = 0;
                }
                continue;  // Skip this byte, try next as potential packet start
            }
        }

        mouse_packet[mouse_packet_idx++] = (uint8_t)data;

        // Process complete packet
        if (mouse_packet_idx >= mouse_packet_size) {
            mouse_process_packet();
            mouse_packet_idx = 0;
        }
    }
}

bool ps2_mouse_get_state(int16_t *dx, int16_t *dy, int8_t *wheel, uint8_t *buttons) {
    // Process any pending data from the ring buffer
    ps2_mouse_poll();

    // Disable only the mouse PIO interrupt (not all interrupts)
    // This avoids blocking HDMI and audio
    uint irq_num = mouse_irq_num();
    irq_set_enabled(irq_num, false);

    // FAILSAFE: If button has been pressed for > 2s without release,
    // force it to released state (handles stuck button from sync errors)
    if (mouse_state.buttons != 0 && mouse_button_press_time != 0) {
        uint32_t button_held_us = time_us_32() - mouse_button_press_time;
        if (button_held_us > 2000000) {  // 2 second timeout
            printf("PS2: FAILSAFE - Button stuck for %lu ms, forcing release\n",
                   button_held_us / 1000);
            mouse_state.buttons = 0;
            mouse_last_buttons = 0;
            mouse_button_press_time = 0;
        }
    }

    bool has_data = (mouse_state.delta_x != 0 ||
                     mouse_state.delta_y != 0 ||
                     mouse_state.wheel != 0);

    if (dx) *dx = mouse_state.delta_x;
    if (dy) *dy = mouse_state.delta_y;
    if (wheel) *wheel = mouse_state.wheel;
    if (buttons) *buttons = mouse_state.buttons;

    mouse_state.delta_x = 0;
    mouse_state.delta_y = 0;
    mouse_state.wheel = 0;

    // Re-enable mouse interrupt
    irq_set_enabled(irq_num, true);

    return has_data;
}

bool ps2_mouse_is_initialized(void) {
    return mouse_state.initialized != 0;
}

bool ps2_mouse_has_wheel(void) {
    return mouse_state.has_wheel != 0;
}

void ps2_mouse_get_errors(uint32_t *frame_err, uint32_t *parity_err, uint32_t *sync_err) {
    if (frame_err) *frame_err = mouse_frame_errors;
    if (parity_err) *parity_err = mouse_parity_errors;
    if (sync_err) *sync_err = mouse_sync_errors;
}

void ps2_mouse_get_counters(uint32_t *raw_bytes,
                            uint32_t *valid_packets,
                            uint32_t *ring_level) {
    if (raw_bytes) *raw_bytes = mouse_raw_bytes;
    if (valid_packets) *valid_packets = mouse_valid_packets_total;
    if (ring_level) {
        uint8_t head = mouse_rx_head;
        uint8_t tail = mouse_rx_tail;
        *ring_level = (uint32_t)((head - tail + MOUSE_RX_BUFFER_SIZE)
                                 % MOUSE_RX_BUFFER_SIZE);
    }
}

uint32_t ps2_mouse_pio_fifo_level(void) {
    if (!mouse_pio_initialized || ps2_pio == NULL) return 0;
    return pio_sm_get_rx_fifo_level(ps2_pio, mouse_sm);
}

//=============================================================================
// Public API - Keyboard (raw access)
//=============================================================================

bool ps2_kbd_has_data(void) {
    if (!kbd_pio) return false;
    return !pio_sm_is_rx_fifo_empty(kbd_pio, kbd_sm);
}

uint32_t ps2_kbd_get_raw(void) {
    if (!kbd_pio || pio_sm_is_rx_fifo_empty(kbd_pio, kbd_sm)) {
        return 0;
    }
    return pio_sm_get(kbd_pio, kbd_sm);
}

int ps2_kbd_get_byte(void) {
    if (!kbd_pio || pio_sm_is_rx_fifo_empty(kbd_pio, kbd_sm)) {
        return -1;
    }
    uint32_t raw = pio_sm_get(kbd_pio, kbd_sm);
    return ps2_rx_decode_frame(raw);
}
