/*
 * PS/2 Keyboard Wrapper for Duke3D on RP2350
 * Uses unified PS/2 driver, converts Set 2 scancodes to Duke3D scancodes
 */
#include "ps2kbd_wrapper.h"
#include "ps2.h"
#include "board_config.h"
#include <string.h>

// Duke3D scancode definitions (from keyboard.h)
#define sc_None         0
#define sc_Escape       0x01
#define sc_1            0x02
#define sc_2            0x03
#define sc_3            0x04
#define sc_4            0x05
#define sc_5            0x06
#define sc_6            0x07
#define sc_7            0x08
#define sc_8            0x09
#define sc_9            0x0a
#define sc_0            0x0b
#define sc_Minus        0x0c
#define sc_Equals       0x0d
#define sc_BackSpace    0x0e
#define sc_Tab          0x0f
#define sc_Q            0x10
#define sc_W            0x11
#define sc_E            0x12
#define sc_R            0x13
#define sc_T            0x14
#define sc_Y            0x15
#define sc_U            0x16
#define sc_I            0x17
#define sc_O            0x18
#define sc_P            0x19
#define sc_OpenBracket  0x1a
#define sc_CloseBracket 0x1b
#define sc_Return       0x1c
#define sc_LeftControl  0x1d
#define sc_A            0x1e
#define sc_S            0x1f
#define sc_D            0x20
#define sc_F            0x21
#define sc_G            0x22
#define sc_H            0x23
#define sc_J            0x24
#define sc_K            0x25
#define sc_L            0x26
#define sc_SemiColon    0x27
#define sc_Quote        0x28
#define sc_Tilde        0x29
#define sc_LeftShift    0x2a
#define sc_BackSlash    0x2b
#define sc_Z            0x2c
#define sc_X            0x2d
#define sc_C            0x2e
#define sc_V            0x2f
#define sc_B            0x30
#define sc_N            0x31
#define sc_M            0x32
#define sc_Comma        0x33
#define sc_Period       0x34
#define sc_Slash        0x35
#define sc_RightShift   0x36
#define sc_Kpad_Star    0x37
#define sc_LeftAlt      0x38
#define sc_Space        0x39
#define sc_CapsLock     0x3a
#define sc_F1           0x3b
#define sc_F2           0x3c
#define sc_F3           0x3d
#define sc_F4           0x3e
#define sc_F5           0x3f
#define sc_F6           0x40
#define sc_F7           0x41
#define sc_F8           0x42
#define sc_F9           0x43
#define sc_F10          0x44
#define sc_NumLock      0x45
#define sc_ScrollLock   0x46
#define sc_kpad_7       0x47
#define sc_kpad_8       0x48
#define sc_kpad_9       0x49
#define sc_kpad_Minus   0x4a
#define sc_kpad_4       0x4b
#define sc_kpad_5       0x4c
#define sc_kpad_6       0x4d
#define sc_kpad_Plus    0x4e
#define sc_kpad_1       0x4f
#define sc_kpad_2       0x50
#define sc_kpad_3       0x51
#define sc_kpad_0       0x52
#define sc_kpad_Period  0x53
#define sc_F11          0x57
#define sc_F12          0x58
#define sc_Pause        0x59
#define sc_UpArrow      0x5a
#define sc_Insert       0x5e
#define sc_Delete       0x5f
#define sc_Home         0x61
#define sc_End          0x62
#define sc_PgUp         0x63
#define sc_PgDn         0x64
#define sc_RightAlt     0x65
#define sc_RightControl 0x66
#define sc_kpad_Slash   0x67
#define sc_kpad_Enter   0x68
#define sc_DownArrow    0x6a
#define sc_LeftArrow    0x6b
#define sc_RightArrow   0x6c

// Event queue
#define MAX_KEY_EVENTS 32
typedef struct {
    int pressed;
    unsigned char key;
} KeyEvent;

static KeyEvent event_queue[MAX_KEY_EVENTS];
static int queue_head = 0;
static int queue_tail = 0;

// State for scancode decoding
static int expecting_break = 0;    // F0 prefix received
static int expecting_extended = 0; // E0 prefix received

// PS/2 Set 2 to Duke3D scancode mapping (normal keys)
static unsigned char set2_to_duke3d(uint8_t code) {
    static const unsigned char map[256] = {
        [0x00] = sc_None,
        [0x01] = sc_F9,
        [0x03] = sc_F5,
        [0x04] = sc_F3,
        [0x05] = sc_F1,
        [0x06] = sc_F2,
        [0x07] = sc_F12,
        [0x09] = sc_F10,
        [0x0A] = sc_F8,
        [0x0B] = sc_F6,
        [0x0C] = sc_F4,
        [0x0D] = sc_Tab,
        [0x0E] = sc_Tilde,
        [0x11] = sc_LeftAlt,
        [0x12] = sc_LeftShift,
        [0x14] = sc_LeftControl,
        [0x15] = sc_Q,
        [0x16] = sc_1,
        [0x1A] = sc_Z,
        [0x1B] = sc_S,
        [0x1C] = sc_A,
        [0x1D] = sc_W,
        [0x1E] = sc_2,
        [0x21] = sc_C,
        [0x22] = sc_X,
        [0x23] = sc_D,
        [0x24] = sc_E,
        [0x25] = sc_4,
        [0x26] = sc_3,
        [0x29] = sc_Space,
        [0x2A] = sc_V,
        [0x2B] = sc_F,
        [0x2C] = sc_T,
        [0x2D] = sc_R,
        [0x2E] = sc_5,
        [0x31] = sc_N,
        [0x32] = sc_B,
        [0x33] = sc_H,
        [0x34] = sc_G,
        [0x35] = sc_Y,
        [0x36] = sc_6,
        [0x3A] = sc_M,
        [0x3B] = sc_J,
        [0x3C] = sc_U,
        [0x3D] = sc_7,
        [0x3E] = sc_8,
        [0x41] = sc_Comma,
        [0x42] = sc_K,
        [0x43] = sc_I,
        [0x44] = sc_O,
        [0x45] = sc_0,
        [0x46] = sc_9,
        [0x49] = sc_Period,
        [0x4A] = sc_Slash,
        [0x4B] = sc_L,
        [0x4C] = sc_SemiColon,
        [0x4D] = sc_P,
        [0x4E] = sc_Minus,
        [0x52] = sc_Quote,
        [0x54] = sc_OpenBracket,
        [0x55] = sc_Equals,
        [0x58] = sc_CapsLock,
        [0x59] = sc_RightShift,
        [0x5A] = sc_Return,
        [0x5B] = sc_CloseBracket,
        [0x5D] = sc_BackSlash,
        [0x66] = sc_BackSpace,
        [0x69] = sc_kpad_1,
        [0x6B] = sc_kpad_4,
        [0x6C] = sc_kpad_7,
        [0x70] = sc_kpad_0,
        [0x71] = sc_kpad_Period,
        [0x72] = sc_kpad_2,
        [0x73] = sc_kpad_5,
        [0x74] = sc_kpad_6,
        [0x75] = sc_kpad_8,
        [0x76] = sc_Escape,
        [0x77] = sc_NumLock,
        [0x78] = sc_F11,
        [0x79] = sc_kpad_Plus,
        [0x7A] = sc_kpad_3,
        [0x7B] = sc_kpad_Minus,
        [0x7C] = sc_Kpad_Star,
        [0x7D] = sc_kpad_9,
        [0x7E] = sc_ScrollLock,
        [0x83] = sc_F7,
    };
    return map[code];
}

// PS/2 Set 2 extended (E0 prefix) to Duke3D scancode mapping
static unsigned char set2_extended_to_duke3d(uint8_t code) {
    switch (code) {
        case 0x11: return sc_RightAlt;
        case 0x14: return sc_RightControl;
        case 0x4A: return sc_kpad_Slash;
        case 0x5A: return sc_kpad_Enter;
        case 0x69: return sc_End;
        case 0x6B: return sc_LeftArrow;
        case 0x6C: return sc_Home;
        case 0x70: return sc_Insert;
        case 0x71: return sc_Delete;
        case 0x72: return sc_DownArrow;
        case 0x74: return sc_RightArrow;
        case 0x75: return sc_UpArrow;
        case 0x7A: return sc_PgDn;
        case 0x7D: return sc_PgUp;
        default: return sc_None;
    }
}

static void queue_event(int pressed, unsigned char key) {
    if (key == sc_None) return;

    int next_head = (queue_head + 1) % MAX_KEY_EVENTS;
    if (next_head != queue_tail) {
        event_queue[queue_head].pressed = pressed;
        event_queue[queue_head].key = key;
        queue_head = next_head;
    }
}

void ps2kbd_init(void) {
    // HDMI already claimed two state machines on pio0 by this point, so
    // run the PS/2 driver on pio1. The nespad driver also lives on pio1
    // but uses the remaining SMs, and SD card PIO-SPI is only active if
    // not using hardware SPI (we use hardware SPI here).
    //
    // Mouse init is now driven from main.c (ps2_mouse_init_device())
    // so it runs in the right order relative to HDMI init — porting
    // the fix from murmsnes f5113d8 where PS/2 TX perturbs live HDMI.
    ps2_init(pio1, PS2_PIN_CLK, PS2_MOUSE_CLK);

    queue_head = 0;
    queue_tail = 0;
    expecting_break = 0;
    expecting_extended = 0;
}

void ps2kbd_tick(void) {
    // Process all available keyboard bytes
    int byte;
    while ((byte = ps2_kbd_get_byte()) >= 0) {
        uint8_t code = (uint8_t)byte;

        // Handle special prefixes
        if (code == 0xF0) {
            // Break code prefix - next byte is a key release
            expecting_break = 1;
            continue;
        }

        if (code == 0xE0) {
            // Extended key prefix
            expecting_extended = 1;
            continue;
        }

        // Skip other special bytes
        if (code == 0xE1 || code == 0xFA || code == 0xAA) {
            continue;
        }

        // Convert to Duke3D scancode
        unsigned char duke_key;
        if (expecting_extended) {
            duke_key = set2_extended_to_duke3d(code);
            expecting_extended = 0;
        } else {
            duke_key = set2_to_duke3d(code);
        }

        // Queue the event
        if (duke_key != sc_None) {
            queue_event(!expecting_break, duke_key);
        }

        expecting_break = 0;
    }
}

int ps2kbd_get_key(int* pressed, unsigned char* key) {
    if (queue_head == queue_tail) {
        return 0;
    }

    *pressed = event_queue[queue_tail].pressed;
    *key = event_queue[queue_tail].key;
    queue_tail = (queue_tail + 1) % MAX_KEY_EVENTS;
    return 1;
}
