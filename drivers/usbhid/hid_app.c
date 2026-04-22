/*
 * USB HID host driver
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 */

#include "tusb.h"
#include "usbhid.h"
#include <stdio.h>
#include <string.h>

// Only compile if USB Host is enabled
#if CFG_TUH_ENABLED

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------

#define MAX_REPORT 4

// Per-device, per-instance HID info for generic report parsing
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

// Previous keyboard report for detecting key changes
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

// Previous mouse report for detecting button changes
static hid_mouse_report_t prev_mouse_report = { 0 };

// Accumulated mouse movement
static volatile int16_t cumulative_dx = 0;
static volatile int16_t cumulative_dy = 0;
static volatile int8_t cumulative_wheel = 0;
static volatile uint8_t current_buttons = 0;
static volatile int mouse_has_motion = 0;

// Gamepad state — two slots for two USB gamepads
#define MAX_GAMEPADS 2

typedef struct {
    volatile int8_t axis_x;
    volatile int8_t axis_y;
    volatile uint8_t dpad;
    volatile uint16_t buttons;
    volatile int connected;
    uint8_t dev_addr;   // TinyUSB device address (0 = slot free)
    uint8_t instance;   // TinyUSB HID instance
} gamepad_slot_t;

static gamepad_slot_t gamepad_slots[MAX_GAMEPADS] = {0};

// Find gamepad slot by dev_addr+instance, or allocate a new one. Returns -1 if full.
static int find_or_alloc_gamepad_slot(uint8_t dev_addr, uint8_t inst) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected && gamepad_slots[i].dev_addr == dev_addr && gamepad_slots[i].instance == inst)
            return i;
    }
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!gamepad_slots[i].connected) {
            gamepad_slots[i].dev_addr = dev_addr;
            gamepad_slots[i].instance = inst;
            return i;
        }
    }
    return -1;
}

// Find gamepad slot by dev_addr (for unmount). Returns -1 if not found.
static int find_gamepad_slot_by_dev(uint8_t dev_addr) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected && gamepad_slots[i].dev_addr == dev_addr)
            return i;
    }
    return -1;
}

// Device connection state
static volatile int keyboard_connected = 0;
static volatile int mouse_connected = 0;

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

//--------------------------------------------------------------------
// Internal functions
//--------------------------------------------------------------------

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

// Forward declarations
static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report);
static void process_mouse_report(hid_mouse_report_t const *report);
static void process_gamepad_report(int slot, uint8_t const *report, uint16_t len);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

//--------------------------------------------------------------------
// Process keyboard report
//--------------------------------------------------------------------

static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods = report->modifier & ~(prev_report->modifier);

    if (released_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
        queue_key_action(0xE1, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
        queue_key_action(0xE1, 1);
    if (released_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        queue_key_action(0xE0, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        queue_key_action(0xE0, 1);
    if (released_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        queue_key_action(0xE2, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        queue_key_action(0xE2, 1);

    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode))
            queue_key_action(keycode, 0);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode))
            queue_key_action(keycode, 1);
    }
}

//--------------------------------------------------------------------
// Process mouse report
//--------------------------------------------------------------------

static void process_mouse_report(hid_mouse_report_t const *report) {
    cumulative_dx += report->x;
    cumulative_dy += -report->y;
    cumulative_wheel += report->wheel;
    current_buttons = report->buttons & 0x07;
    if (report->x != 0 || report->y != 0) mouse_has_motion = 1;
    prev_mouse_report = *report;
}

//--------------------------------------------------------------------
// Process gamepad report (per-slot)
//--------------------------------------------------------------------

static void process_gamepad_report(int slot, uint8_t const *report, uint16_t len) {
    if (slot < 0 || slot >= MAX_GAMEPADS || report == NULL || len < 7) return;

    gamepad_slot_t *gp = &gamepad_slots[slot];

    // D-pad from bytes 3-4 (0x00=min, 0x7F=center, 0xFF=max)
    gp->dpad = 0;
    if (report[3] < 0x40) gp->dpad |= 0x04; // Left
    if (report[3] > 0xC0) gp->dpad |= 0x08; // Right
    if (report[4] < 0x40) gp->dpad |= 0x01; // Up
    if (report[4] > 0xC0) gp->dpad |= 0x02; // Down

    // Buttons: A(0x20), B(0x40), X(0x10), Y(0x80) in byte 5
    //          L(0x01), R(0x02), Select(0x10), Start(0x20) in byte 6
    gp->buttons = 0;
    if (report[5] & 0x20) gp->buttons |= 0x0001; // A
    if (report[5] & 0x40) gp->buttons |= 0x0002; // B
    if (report[5] & 0x10) gp->buttons |= 0x0004; // X
    if (report[5] & 0x80) gp->buttons |= 0x0008; // Y
    if (report[6] & 0x01) gp->buttons |= 0x0010; // L
    if (report[6] & 0x02) gp->buttons |= 0x0020; // R
    if (report[6] & 0x20) gp->buttons |= 0x0040; // Start
    if (report[6] & 0x10) gp->buttons |= 0x0080; // Select
}

//--------------------------------------------------------------------
// Process generic HID report
//--------------------------------------------------------------------

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr;
    if (instance >= CFG_TUH_HID || report == NULL || len == 0) return;

    uint8_t const rpt_count = hid_info[instance].report_count;
    if (rpt_count == 0 || rpt_count > MAX_REPORT) return;

    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        rpt_info = &rpt_info_arr[0];
    } else {
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count && i < MAX_REPORT; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) return;

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (rpt_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
                break;
            case HID_USAGE_DESKTOP_MOUSE:
                process_mouse_report((hid_mouse_report_t const *)report);
                break;
            case HID_USAGE_DESKTOP_GAMEPAD:
            case HID_USAGE_DESKTOP_JOYSTICK: {
                int slot = find_or_alloc_gamepad_slot(dev_addr, instance);
                if (slot >= 0) {
                    gamepad_slots[slot].connected = 1;
                    process_gamepad_report(slot, report, len);
                }
                break;
            }
            default:
                break;
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Callbacks
//--------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("USB HID mounted: dev_addr=%d, instance=%d, protocol=%d\n", dev_addr, instance, itf_protocol);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        printf("  -> Keyboard detected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 1;
        printf("  -> Mouse detected\n");
    }

    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        printf("  -> Generic HID device (protocol=NONE)\n");
        if (instance >= CFG_TUH_HID) {
            printf("  -> Instance %d out of range (max %d)\n", instance, CFG_TUH_HID);
            tuh_hid_receive_report(dev_addr, instance);
            return;
        }

        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
        printf("  -> Parsed %d reports from descriptor\n", hid_info[instance].report_count);

        bool is_gamepad = false;
        for (uint8_t i = 0; i < hid_info[instance].report_count && i < MAX_REPORT; i++) {
            printf("  -> Report %d: usage_page=0x%02X, usage=0x%02X\n",
                   i, hid_info[instance].report_info[i].usage_page,
                   hid_info[instance].report_info[i].usage);
            if (hid_info[instance].report_info[i].usage_page == HID_USAGE_PAGE_DESKTOP) {
                uint8_t usage = hid_info[instance].report_info[i].usage;
                if (usage == HID_USAGE_DESKTOP_GAMEPAD || usage == HID_USAGE_DESKTOP_JOYSTICK)
                    is_gamepad = true;
            }
        }

        if (!is_gamepad && hid_info[instance].report_count == 0) {
            printf("  -> No reports parsed, assuming gamepad\n");
            is_gamepad = true;
        }

        if (is_gamepad) {
            int slot = find_or_alloc_gamepad_slot(dev_addr, instance);
            if (slot >= 0) {
                gamepad_slots[slot].connected = 1;
                printf("  -> GAMEPAD assigned to slot %d\n", slot);
            } else {
                printf("  -> GAMEPAD slots full, ignored\n");
            }
        }
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        int slot = find_gamepad_slot_by_dev(dev_addr);
        if (slot >= 0) {
            printf("USB HID unmounted: gamepad slot %d disconnected\n", slot);
            gamepad_slots[slot].connected = 0;
            gamepad_slots[slot].buttons = 0;
            gamepad_slots[slot].dpad = 0;
            gamepad_slots[slot].axis_x = 0;
            gamepad_slots[slot].axis_y = 0;
            gamepad_slots[slot].dev_addr = 0;
        }
    }
}

static int report_debug_counter = 0;

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (report && len >= sizeof(hid_keyboard_report_t)) {
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
            }
            break;
        case HID_ITF_PROTOCOL_MOUSE:
            if (report && len >= sizeof(hid_mouse_report_t))
                process_mouse_report((hid_mouse_report_t const *)report);
            break;
        default:
            if (report && len >= 2) {
                if (report_debug_counter < 5) {
                    printf("Gamepad report (dev=%d, len=%d): ", dev_addr, len);
                    for (int i = 0; i < len && i < 16; i++) printf("%02X ", report[i]);
                    printf("\n");
                    report_debug_counter++;
                }
                int slot = find_or_alloc_gamepad_slot(dev_addr, instance);
                if (slot >= 0) {
                    gamepad_slots[slot].connected = 1;
                    process_gamepad_report(slot, report, len);
                }
            }
            break;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// Public API
//--------------------------------------------------------------------

void usbhid_init(void) {
    tuh_init(BOARD_TUH_RHPORT);
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    memset(&prev_mouse_report, 0, sizeof(prev_mouse_report));
    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    current_buttons = 0;
    mouse_has_motion = 0;
    key_action_head = 0;
    key_action_tail = 0;
}

void usbhid_task(void) {
    tuh_task();
}

int usbhid_keyboard_connected(void) { return keyboard_connected; }
int usbhid_mouse_connected(void) { return mouse_connected; }

void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state) {
    if (state) {
        memcpy(state->keycode, prev_kbd_report.keycode, 6);
        state->modifier = prev_kbd_report.modifier;
        state->has_key = (key_action_head != key_action_tail);
    }
}

void usbhid_get_mouse_state(usbhid_mouse_state_t *state) {
    if (state) {
        state->dx = cumulative_dx;
        state->dy = cumulative_dy;
        state->wheel = cumulative_wheel;
        state->buttons = current_buttons;
        state->has_motion = mouse_has_motion;
        cumulative_dx = 0;
        cumulative_dy = 0;
        cumulative_wheel = 0;
        mouse_has_motion = 0;
    }
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail) return 0;
    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

// Convert HID keycode to keyboard state bit (SNES layout)
static uint16_t hid_to_kbd_state_bit(uint8_t keycode) {
    switch (keycode) {
        // Arrow keys -> D-pad
        case 0x52: return (1 << 0);  // Up    -> KBD_STATE_UP
        case 0x51: return (1 << 1);  // Down  -> KBD_STATE_DOWN
        case 0x50: return (1 << 2);  // Left  -> KBD_STATE_LEFT
        case 0x4F: return (1 << 3);  // Right -> KBD_STATE_RIGHT

        // X key -> A, Z key -> B (standard NES/SNES convention)
        case 0x1B: return (1 << 4);  // X key -> KBD_STATE_A
        case 0x1D: return (1 << 5);  // Z key -> KBD_STATE_B

        // S key -> X, A key -> Y (SNES left-side face buttons)
        case 0x16: return (1 << 6);  // S key -> KBD_STATE_X
        case 0x04: return (1 << 7);  // A key -> KBD_STATE_Y

        // Q key -> L, W key -> R (shoulder buttons)
        case 0x14: return (1 << 8);  // Q key -> KBD_STATE_L
        case 0x1A: return (1 << 9);  // W key -> KBD_STATE_R

        // Enter -> Start, Space -> Select
        case 0x28: return (1 << 10); // Enter       -> KBD_STATE_START
        case 0x58: return (1 << 10); // Keypad Enter -> KBD_STATE_START
        case 0x2C: return (1 << 11); // Space       -> KBD_STATE_SELECT

        // ESC -> Settings menu
        case 0x29: return (1 << 12); // Escape -> KBD_STATE_ESC

        // F12 -> Settings menu (alternative)
        case 0x45: return (1 << 13); // F12 -> KBD_STATE_F12

        default: return 0;
    }
}

uint16_t usbhid_get_kbd_state(void) {
    uint16_t state = 0;
    for (int i = 0; i < 6; i++) {
        if (prev_kbd_report.keycode[i] != 0)
            state |= hid_to_kbd_state_bit(prev_kbd_report.keycode[i]);
    }
    return state;
}

// Per-slot gamepad API
int usbhid_gamepad_connected_idx(int idx) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return 0;
    return gamepad_slots[idx].connected;
}

void usbhid_get_gamepad_state_idx(int idx, usbhid_gamepad_state_t *state) {
    if (!state) return;
    if (idx < 0 || idx >= MAX_GAMEPADS || !gamepad_slots[idx].connected) {
        memset(state, 0, sizeof(*state));
        return;
    }
    state->axis_x = gamepad_slots[idx].axis_x;
    state->axis_y = gamepad_slots[idx].axis_y;
    state->dpad = gamepad_slots[idx].dpad;
    state->buttons = gamepad_slots[idx].buttons;
    state->connected = gamepad_slots[idx].connected;
}

// Legacy combined API
int usbhid_gamepad_connected(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected) return 1;
    }
    return 0;
}

void usbhid_get_gamepad_state(usbhid_gamepad_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected) {
            state->dpad |= gamepad_slots[i].dpad;
            state->buttons |= gamepad_slots[i].buttons;
            state->connected = 1;
        }
    }
}

#endif // CFG_TUH_ENABLED
