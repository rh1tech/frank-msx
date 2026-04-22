#include "nespad.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include <stdio.h>

#define nespad_wrap_target 0
#define nespad_wrap 7

// Auto-polling PIO program: continuously reads the controller every ~700us
// without needing a TX trigger.  This minimises input latency from ~16ms
// (once per frame) down to < 1ms.
static const uint16_t nespad_program_instructions[] = {
    //     .wrap_target
    0xea01, //  0: set    pins, 1         side 0 [10]  ; latch high
    0xe02f, //  1: set    x, 15           side 0       ; 16 bits to read
    0xe000, //  2: set    pins, 0         side 0       ; latch low
    0x4402, //  3: in     pins, 2         side 0 [4]   ; read 2 data pins
    0xf500, //  4: set    pins, 0         side 1 [5]   ; clock high
    0x0043, //  5: jmp    x--, 3          side 0       ; bit loop
    0xe03f, //  6: set    x, 31           side 0       ; delay counter
    0x0f47, //  7: jmp    x--, 7          side 0 [15]  ; ~512us inter-poll delay
            //     .wrap
};

static const struct pio_program nespad_program = {
    .instructions = nespad_program_instructions,
    .length = 8,
    .origin = -1,
};

static inline pio_sm_config nespad_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + nespad_wrap_target, offset + nespad_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

static PIO pio = pio1;
static uint8_t sm = -1;
uint32_t nespad_state = 0;  // Joystick 1
uint32_t nespad_state2 = 0; // Joystick 2

bool nespad_begin(uint32_t cpu_khz, uint8_t clkPin, uint8_t dataPin, uint8_t latPin) {
    if (pio_can_add_program(pio, &nespad_program) &&
        ((sm = pio_claim_unused_sm(pio, true)) >= 0)) {
        uint offset = pio_add_program(pio, &nespad_program);
        pio_sm_config c = nespad_program_get_default_config(offset);

        sm_config_set_sideset_pins(&c, clkPin);
        sm_config_set_in_pins(&c, dataPin);
        sm_config_set_set_pins(&c, latPin, 1);
        pio_gpio_init(pio, clkPin);
        pio_gpio_init(pio, dataPin);
        pio_gpio_init(pio, dataPin + 1); // +1 Pin for Joystick2
        pio_gpio_init(pio, latPin);
        gpio_set_pulls(clkPin, true, false);      // Pull clock high
        gpio_set_pulls(dataPin, true, false);     // Pull data high, 0xFF if unplugged
        gpio_set_pulls(dataPin + 1, true, false); // Pull data high for Joystick2
        gpio_set_pulls(latPin, true, false);      // Pull latch high

        pio_sm_set_pindirs_with_mask(pio, sm,
                                      (1 << clkPin) | (1 << latPin), // Outputs
                                      (1 << clkPin) | (1 << latPin) |
                                          (1 << dataPin) | (1 << (dataPin + 1))); // All pins
        sm_config_set_in_shift(&c, true, true, 32);                               // R shift, autopush @ 32 bits
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX); // 8-deep RX, no TX needed

        sm_config_set_clkdiv_int_frac(&c, cpu_khz / 1000, 0); // 1 MHz clock

        pio_sm_clear_fifos(pio, sm);

        pio_sm_init(pio, sm, offset, &c);
        pio_sm_set_enabled(pio, sm, true);
        // PIO auto-polls continuously — no TX trigger needed
        return true; // Success
    }
    return false;
}

// Read NES/SNES gamepad state — drain FIFO to get the freshest sample
void nespad_read() {
    if (sm < 0)
        return;

    // PIO auto-polls continuously; drain all queued results, keep the latest
    uint32_t temp;
    bool got_data = false;
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        temp = pio->rxf[sm];
        got_data = true;
    }
    if (!got_data)
        return;

    // Right-shift was used in sm config so bit order matches NES controller
    temp ^= 0xFFFFFFFF;
    nespad_state = temp & 0x555555;        // Joy1
    nespad_state2 = temp >> 1 & 0x555555;  // Joy2
}
