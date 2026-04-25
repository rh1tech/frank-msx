/*
 * platform.c — fMSX host-layer implementation for RP2350 (replaces Unix.c).
 *
 * Responsibilities:
 *   - InitMachine / TrashMachine lifecycle
 *   - Framebuffer + palette plumbing via the HDMI driver
 *   - Joystick()/Keyboard()/Mouse() hooks mapped to PS/2 + NES pad
 *   - PutImage() — swap display buffers
 *   - SetColor() — push palette entries into the HDMI driver
 *   - Stubs for EMULib's audio backend (InitAudio/TrashAudio/WriteAudio/
 *     GetFreeAudio/PauseAudio/SetSyncTimer) that integrate with our I2S
 *     driver on core 1.
 *
 * RefreshLine[] screen-mode handlers live in CommonMux.h. We include it
 * here so the per-depth refresh routines get compiled into this TU;
 * CommonMux.h's first include of Common.h (without depth suffixes) also
 * provides the bare `RefreshLine0..Tx80` / `RefreshBorder` / `RefreshScreen`
 * symbols that MSX.c references directly.
 */

#include "MSX.h"
#include "EMULib.h"
#include "Sound.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/sync.h"

#include "board_config.h"
#include "HDMI.h"
#ifndef HDMI_HSTX
#include "audio.h"
#endif

#include "ps2kbd_wrapper.h"
#include "usbhid_wrapper.h"
#include "nespad/nespad.h"

#include "msx_ui.h"
#include "msx_settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Framebuffer sizing (NARROW: no WideScreen buffer) --------------- */
#define WIDTH   MSX_FB_WIDTH   /* 272 */
#define HEIGHT  MSX_FB_HEIGHT  /* 228 */

/* Globals the Common.h / Wide.h screen drivers (included via CommonMux.h)
 * expect to exist in this file. They mirror the Unix.c layout. */
Image NormScreen;              /* Main screen image */
Image WideScreen;              /* Wide screen image (unused in NARROW build) */
static pixel *XBuf;            /* NormScreen backing   (from Common.h) */
static pixel *WBuf;            /* WideScreen backing   (from Wide.h)   */
static unsigned int XPal[80];  /* Current palette (indexes into HW palette) */
static unsigned int BPal[256]; /* SCREEN8 palette */
static unsigned int XPal0;     /* Background color entry (transparent) */

/* SCREEN[] / current_buffer live in main.c */
extern uint8_t *SCREEN[2];
extern volatile uint32_t current_buffer;

volatile byte XKeyState[20];   /* Extended keyboard state */

int UseEffects  = 0;
int InMenu      = 0;
int UseZoom     = 1;
int UseSound    = 22050;
int SyncFreq    = 60;
int FastForward = 0;
int SndSwitch;
int SndVolume;
int OldScrMode  = 0;

const char *Title = "fMSX 6.0";

/* Joystick holder updated by polling tasks */
static volatile unsigned int g_last_joystick = 0;

/* Frame pacing (60 Hz sync timer) — see SetSyncTimer / WaitSyncTimer. */
static int g_sync_hz = 0;

/* Forward decls */
void PutImage(void);
static unsigned int alloc_palette_entry(unsigned char R, unsigned char G, unsigned char B);

/* Pull in the per-depth refresh routines and screen dispatch table.
 *
 * CommonMux.h first includes Common.h + Wide.h WITHOUT depth suffixes,
 * producing bare RefreshLine0..Tx80, RefreshBorder, RefreshScreen, etc.
 * Those are the symbols MSX.c references directly. It then re-includes
 * them three times (BPP8/16/32) with suffixed names and defines
 * SetScreenDepth() to populate fMSX's RefreshLine[] table at runtime.
 */
#include "CommonMux.h"

/* ==================================================================
 * EMULib Image / Video API (replaces LibUnix.c implementations)
 * ================================================================== */

pixel *NewImage(Image *Img, int Width, int Height) {
    if (!Img) return NULL;
    size_t pix = (size_t)Width * (size_t)Height * sizeof(pixel);
    Img->Data = (pixel *)malloc(pix);
    if (!Img->Data) { Img->D = 0; return NULL; }
    memset(Img->Data, 0, pix);
    Img->W = Img->L = Width;
    Img->H = Height;
    Img->D = 8;  /* palette-indexed on RP2350 HDMI driver */
    Img->Cropped = 0;
    return Img->Data;
}

void FreeImage(Image *Img) {
    if (!Img || !Img->Data) return;
    free(Img->Data);
    Img->Data = NULL;
    Img->W = Img->H = Img->L = 0;
}

void ClearImage(Image *Img, pixel C) {
    if (!Img || !Img->Data) return;
    memset(Img->Data, (int)C, (size_t)Img->W * (size_t)Img->H * sizeof(pixel));
}

Image *VideoImg = NULL;
int VideoX = 0, VideoY = 0, VideoW = 0, VideoH = 0;

void SetVideo(Image *Img, int X, int Y, int W, int H) {
    VideoImg = Img;
    VideoX = X; VideoY = Y; VideoW = W; VideoH = H;
}

int ShowVideo(void) {
    /* Nothing to do: HDMI.c scans `SCREEN[!current_buffer]` directly.
     * PutImage() already updated the buffer pointer. */
    return 1;
}

pixel GetColor(unsigned char R, unsigned char G, unsigned char B) {
    return (pixel)alloc_palette_entry(R, G, B);
}

/* Raw RGB cache for every palette slot we ever programmed.
 * Every path that writes the HDMI palette routes through pal_push(),
 * which stores the requested color here and re-applies the active
 * color filter before handing the value to the HDMI driver. On a
 * filter change we just iterate this table and push fresh values. */
#include "msx_settings.h"
static uint8_t  s_raw_rgb[256][3];
static uint8_t  s_pal_valid[256];        /* 1 if slot is in use */
static uint8_t  s_color_filter = MSX_COLOR_NORMAL;

static inline uint8_t clip_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Apply the current filter to (R,G,B) and return the 0xRRGGBB value. */
static uint32_t filter_rgb(uint8_t R, uint8_t G, uint8_t B) {
    /* Luminance approximation: Y = 0.299R + 0.587G + 0.114B */
    int Y = (R * 77 + G * 150 + B * 29) >> 8;
    switch (s_color_filter) {
        case MSX_COLOR_MONO:
            R = G = B = (uint8_t)Y;
            break;
        case MSX_COLOR_SEPIA: {
            /* Classic sepia matrix. */
            int nr = (R * 100 + G * 196 + B *  48) >> 8;
            int ng = (R *  89 + G * 174 + B *  43) >> 8;
            int nb = (R *  69 + G * 136 + B *  33) >> 8;
            R = clip_u8(nr); G = clip_u8(ng); B = clip_u8(nb);
            break;
        }
        case MSX_COLOR_GREEN:
            R = 0; G = (uint8_t)Y; B = 0;
            break;
        case MSX_COLOR_AMBER:
            R = (uint8_t)Y;
            G = clip_u8((Y * 3) / 4);
            B = 0;
            break;
        case MSX_COLOR_NORMAL:
        default:
            break;
    }
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

static void pal_push(uint8_t idx, uint8_t R, uint8_t G, uint8_t B) {
    s_raw_rgb[idx][0] = R;
    s_raw_rgb[idx][1] = G;
    s_raw_rgb[idx][2] = B;
    s_pal_valid[idx]  = 1;
    graphics_set_palette(idx, filter_rgb(R, G, B));
}

void SetPalette(pixel N, unsigned char R, unsigned char G, unsigned char B) {
    pal_push((uint8_t)N, R, G, B);
}

/* Iterate every slot we've ever set and re-push with the new filter.
 * Called by msx_settings_apply_visual() when the Color setting changes. */
void platform_repaint_palette(uint8_t color_filter) {
    s_color_filter = color_filter;
    for (int i = 0; i < 256; ++i) {
        if (s_pal_valid[i])
            graphics_set_palette((uint8_t)i,
                filter_rgb(s_raw_rgb[i][0], s_raw_rgb[i][1], s_raw_rgb[i][2]));
    }
}

/* fMSX's Common.h uses X11GetColor() to map an (R,G,B) triple to an
 * entry in the hardware palette. We reserve entries 0-15 for the MSX's
 * 16-color palette (set via SetColor) and allocate entries 16..240 on
 * demand for SCREEN8 / YJK colors. */
static uint8_t s_pal_cursor = 16;

static unsigned int alloc_palette_entry(unsigned char R, unsigned char G, unsigned char B) {
    uint8_t idx = s_pal_cursor++;
    if (s_pal_cursor >= 240) s_pal_cursor = 16;  /* 250-253 reserved for HDMI sync */
    pal_push(idx, R, G, B);
    return idx;
}

unsigned int X11GetColor(unsigned char R, unsigned char G, unsigned char B) {
    return alloc_palette_entry(R, G, B);
}

/* ==================================================================
 * SetColor() — called by MSX.c to define 16-color palette
 * ================================================================== */
void SetColor(byte N, byte R, byte G, byte B) {
    /* Fixed-slot palette: entry 0..15 = MSX palette. Sync entries are
     * reserved above 250. */
    uint8_t idx = N;
    pal_push(idx, R, G, B);
    if (N == 0)      XPal0 = idx;
    else if (N < 80) XPal[N] = idx;
}

/* ==================================================================
 * Sync timer — wall-clock deadline pacer.
 *
 * Rather than a one-shot latch (which drifts when a frame runs long),
 * we schedule the next frame deadline as an absolute timestamp and
 * busy-wait to it. If the emulator overruns the budget we roll the
 * deadline forward so we don't try to "catch up" by running faster.
 * ================================================================== */
static uint64_t g_next_frame_us = 0;

int SetSyncTimer(int Hz) {
    g_sync_hz = Hz;
    g_next_frame_us = Hz > 0 ? (time_us_64() + (uint64_t)(1000000 / Hz)) : 0;
    return 1;
}

int WaitSyncTimer(void) {
    if (g_sync_hz <= 0) return 0;
    uint64_t period = (uint64_t)(1000000 / g_sync_hz);
    uint64_t now = time_us_64();
    if (now < g_next_frame_us) {
        /* Ahead of schedule: spin until deadline. */
        while (time_us_64() < g_next_frame_us) tight_loop_contents();
        g_next_frame_us += period;
    } else {
        /* Behind schedule: skip waiting, but rebase the deadline so
         * a single long frame doesn't cause "catch-up sprinting". */
        if (now - g_next_frame_us > 2 * period)
            g_next_frame_us = now + period;
        else
            g_next_frame_us += period;
    }
    return 0;
}

int SyncTimerReady(void) { return time_us_64() >= g_next_frame_us; }

void SetEffects(unsigned int effects) { UseEffects = effects; }

/* ==================================================================
 * Audio backend stubs (EMULib Sound.c hooks)
 *
 * For the first-boot path we don't actually route audio to I2S yet; we
 * advertise a 22kHz sink that silently absorbs samples. Later commits
 * can wire audio_mix → Core 1 I2S.
 * ================================================================== */
#define AUDIO_BUF_FRAMES  (22050 / 60)   /* 367 */
static unsigned audio_rate_hz = 0;

extern unsigned audio_ring_free(void);
#define AUDIO_RING_FRAMES_TOTAL  4096u   /* mirrors main.c AUDIO_RING_FRAMES */

unsigned int InitAudio(unsigned int Rate, unsigned int Latency) {
    (void)Latency;
    audio_rate_hz = Rate ? Rate : 22050;
    return audio_rate_hz;
}

void TrashAudio(void) { audio_rate_hz = 0; }

unsigned int GetFreeAudio(void)  { return audio_ring_free(); }
unsigned int GetTotalAudio(void) { return AUDIO_RING_FRAMES_TOTAL; }

/* Core-0 → Core-1 audio ring defined in main.c. */
extern unsigned audio_ring_push_mono(const int16_t *samples, unsigned count);
extern unsigned audio_ring_free(void);

unsigned int WriteAudio(sample *Data, unsigned int Length) {
    if (!Data || !Length) return 0;
#ifdef HDMI_HSTX
    /* HSTX path: audio rides on HDMI via data-island packets (non-
     * blocking ring push). Mirrors murmnes's default HDMI audio
     * routing — we do NOT also push to the external I2S DAC here, as
     * that driver blocks on DMA buffer availability (~40 ms) and
     * would throttle the emulator to a crawl. */
    hdmi_hstx_push_samples((const int16_t *)Data, (int)Length);
    return Length;
#else
    /* PIO HDMI path: hand off to the Core 0 → Core 1 ring. Pacing comes
     * from the sync alarm in PutImage() rather than from here — sharing
     * the pacing between both paths caused deadlocks during long silent
     * runs. */
    return audio_ring_push_mono((const int16_t *)Data, Length);
#endif
}

int PauseAudio(int Switch) { (void)Switch; return 0; }

/* ==================================================================
 * Keyboard input: PS/2 Set-2 scancodes → MSX key matrix
 *
 * The ps2 wrapper we inherited emits Duke3D-style scancodes. We map
 * those directly to MSX KBD_* constants (defined in MSX.h) using the
 * KBD_SET/RES helpers, bypassing the X11-flavoured HandleKeys() path
 * from Unix.c.
 * ================================================================== */

/* Duke scancode values (mirror ps2kbd_wrapper.c) */
enum {
    PSC_Escape=0x01, PSC_1=0x02, PSC_2=0x03, PSC_3=0x04, PSC_4=0x05, PSC_5=0x06,
    PSC_6=0x07, PSC_7=0x08, PSC_8=0x09, PSC_9=0x0A, PSC_0=0x0B, PSC_Minus=0x0C,
    PSC_Equals=0x0D, PSC_BackSpace=0x0E, PSC_Tab=0x0F,
    PSC_Q=0x10, PSC_W=0x11, PSC_E=0x12, PSC_R=0x13, PSC_T=0x14, PSC_Y=0x15,
    PSC_U=0x16, PSC_I=0x17, PSC_O=0x18, PSC_P=0x19, PSC_LBr=0x1A, PSC_RBr=0x1B,
    PSC_Return=0x1C, PSC_LCtrl=0x1D, PSC_A=0x1E, PSC_S=0x1F, PSC_D=0x20,
    PSC_F=0x21, PSC_G=0x22, PSC_H=0x23, PSC_J=0x24, PSC_K=0x25, PSC_L=0x26,
    PSC_Semi=0x27, PSC_Quote=0x28, PSC_Tilde=0x29, PSC_LShift=0x2A,
    PSC_BkSlash=0x2B, PSC_Z=0x2C, PSC_X=0x2D, PSC_C=0x2E, PSC_V=0x2F,
    PSC_B=0x30, PSC_N=0x31, PSC_M=0x32, PSC_Comma=0x33, PSC_Period=0x34,
    PSC_Slash=0x35, PSC_RShift=0x36, PSC_LAlt=0x38, PSC_Space=0x39,
    PSC_CapsLk=0x3A,
    PSC_F1=0x3B, PSC_F2=0x3C, PSC_F3=0x3D, PSC_F4=0x3E, PSC_F5=0x3F,
    PSC_F6=0x40, PSC_F7=0x41, PSC_F8=0x42, PSC_F9=0x43, PSC_F10=0x44,
    PSC_F11=0x57, PSC_F12=0x58,
    PSC_UpA=0x5A, PSC_Insert=0x5E, PSC_Delete=0x5F,
    PSC_Home=0x61, PSC_End=0x62, PSC_PgUp=0x63, PSC_PgDn=0x64,
    PSC_RAlt=0x65, PSC_RCtrl=0x66, PSC_DownA=0x6A, PSC_LeftA=0x6B, PSC_RightA=0x6C,
};

/* Press/Release macros on XKeyState (Unix.c style) */
#define XKBD_SET(K) XKeyState[Keys[K][0]] &= ~Keys[K][1]
#define XKBD_RES(K) XKeyState[Keys[K][0]] |=  Keys[K][1]

/* Map PS/2 scancode → KBD_* constant (returns 0 if no mapping). */
static unsigned char sc_to_msx(unsigned char sc) {
    switch (sc) {
        case PSC_Escape:    return KBD_ESCAPE;
        case PSC_Return:    return KBD_ENTER;
        case PSC_BackSpace: return KBD_BS;
        case PSC_Tab:       return KBD_TAB;
        case PSC_LShift:
        case PSC_RShift:    return KBD_SHIFT;
        case PSC_LCtrl:
        case PSC_RCtrl:     return KBD_CONTROL;
        case PSC_LAlt:
        case PSC_RAlt:      return KBD_GRAPH;
        case PSC_CapsLk:    return KBD_CAPSLOCK;
        case PSC_UpA:       return KBD_UP;
        case PSC_DownA:     return KBD_DOWN;
        case PSC_LeftA:     return KBD_LEFT;
        case PSC_RightA:    return KBD_RIGHT;
        case PSC_Home:      return KBD_HOME;
        case PSC_End:       return KBD_SELECT;
        case PSC_PgUp:      return KBD_STOP;
        case PSC_PgDn:      return KBD_COUNTRY;
        case PSC_Insert:    return KBD_INSERT;
        case PSC_Delete:    return KBD_DELETE;
        case PSC_F1:        return KBD_F1;
        case PSC_F2:        return KBD_F2;
        case PSC_F3:        return KBD_F3;
        case PSC_F4:        return KBD_F4;
        case PSC_F5:        return KBD_F5;
        case PSC_Space:     return ' ';
        case PSC_0:         return '0';
        case PSC_1:         return '1';
        case PSC_2:         return '2';
        case PSC_3:         return '3';
        case PSC_4:         return '4';
        case PSC_5:         return '5';
        case PSC_6:         return '6';
        case PSC_7:         return '7';
        case PSC_8:         return '8';
        case PSC_9:         return '9';
        case PSC_A:         return 'a';
        case PSC_B:         return 'b';
        case PSC_C:         return 'c';
        case PSC_D:         return 'd';
        case PSC_E:         return 'e';
        case PSC_F:         return 'f';
        case PSC_G:         return 'g';
        case PSC_H:         return 'h';
        case PSC_I:         return 'i';
        case PSC_J:         return 'j';
        case PSC_K:         return 'k';
        case PSC_L:         return 'l';
        case PSC_M:         return 'm';
        case PSC_N:         return 'n';
        case PSC_O:         return 'o';
        case PSC_P:         return 'p';
        case PSC_Q:         return 'q';
        case PSC_R:         return 'r';
        case PSC_S:         return 's';
        case PSC_T:         return 't';
        case PSC_U:         return 'u';
        case PSC_V:         return 'v';
        case PSC_W:         return 'w';
        case PSC_X:         return 'x';
        case PSC_Y:         return 'y';
        case PSC_Z:         return 'z';
        case PSC_Minus:     return '-';
        case PSC_Equals:    return '=';
        case PSC_LBr:       return '[';
        case PSC_RBr:       return ']';
        case PSC_Semi:      return ';';
        case PSC_Quote:     return '\'';
        case PSC_Tilde:     return '`';
        case PSC_BkSlash:   return '\\';
        case PSC_Comma:     return ',';
        case PSC_Period:    return '.';
        case PSC_Slash:     return '/';
        default:            return 0;
    }
}

/* Scancodes → XK_* keysyms for the UI layer. Returns 0 for keys we
 * don't forward. */
static unsigned int sc_to_xk(unsigned char sc) {
    switch (sc) {
        case PSC_Escape: return 0xFF1B;  /* Escape */
        case PSC_Return: return 0xFF0D;  /* Return */
        case PSC_UpA:    return 0xFF52;  /* Up */
        case PSC_DownA:  return 0xFF54;  /* Down */
        case PSC_LeftA:  return 0xFF51;  /* Left */
        case PSC_RightA: return 0xFF53;  /* Right */
        case PSC_PgUp:   return 0xFF55;
        case PSC_PgDn:   return 0xFF56;
        case PSC_F11:    return 0xFFC8;
        case PSC_F12:    return 0xFFC9;
        default:         return 0;
    }
}

/* Track Ctrl+Alt state across poll_inputs() calls so we can recognise
 * Ctrl+Alt+Del as a host-level "reset the MSX" chord. */
static bool s_ctrl_down = false;
static bool s_alt_down  = false;

/* Dispatch a single scancode event (press/release) through the same
 * path regardless of whether it came from PS/2 or USB HID. */
static void handle_key_event(int pressed, unsigned char sc) {
    /* Track modifier keys before anything else. */
    if (sc == PSC_LCtrl || sc == PSC_RCtrl) s_ctrl_down = pressed;
    if (sc == PSC_LAlt  || sc == PSC_RAlt ) s_alt_down  = pressed;

    /* Ctrl+Alt+Del -> MSX hard reset. */
    if (sc == PSC_Delete && pressed && s_ctrl_down && s_alt_down) {
        printf("host: Ctrl+Alt+Del -> ResetMSX\n");
        ResetMSX(Mode, RAMPages, VRAMPages);
        s_ctrl_down = s_alt_down = false;
        memset((void *)XKeyState, 0xFF, sizeof(XKeyState));
        memset((void *)KeyState,  0xFF, sizeof(KeyState));
        return;
    }

    /* F11 toggles the loader overlay, F12 the Settings dialog. */
    if (sc == PSC_F11) {
        if (pressed) msx_ui_toggle();
        return;
    }
    if (sc == PSC_F12) {
        if (pressed) msx_ui_toggle_settings();
        return;
    }

    /* While the overlay is visible, forward key-down events to
     * the UI and don't touch the MSX matrix. */
    if (msx_ui_is_visible()) {
        if (pressed) {
            unsigned int xk = sc_to_xk(sc);
            if (xk) msx_ui_handle_key(xk);
        }
        return;
    }

    unsigned char k = sc_to_msx(sc);
    if (!k) return;
    if (pressed) XKBD_SET(k);
    else         XKBD_RES(k);
}

static void poll_inputs(void) {
    int pressed;
    unsigned char sc;

    /* PS/2 keyboard (PIO-based driver). */
    ps2kbd_tick();
    while (ps2kbd_get_key(&pressed, &sc))
        handle_key_event(pressed, sc);

    /* USB HID keyboard + mouse + gamepad. Stubs out to zero when
     * USB_HID_ENABLED is off — no runtime cost on PS/2-only builds. */
    usbhid_wrapper_tick();
    while (usbhid_wrapper_get_key(&pressed, &sc))
        handle_key_event(pressed, sc);

    unsigned int j = 0;
#ifdef NESPAD_GPIO_CLK
    nespad_read();
    /* Mask values come from drivers/nespad/nespad.h — the NES/SNES
     * serial shift layout spreads them across bits 0..14, not 0..7. */
    if (nespad_state & DPAD_UP)     j |= BTN_UP;
    if (nespad_state & DPAD_DOWN)   j |= BTN_DOWN;
    if (nespad_state & DPAD_LEFT)   j |= BTN_LEFT;
    if (nespad_state & DPAD_RIGHT)  j |= BTN_RIGHT;
    if (nespad_state & DPAD_A)      j |= BTN_FIREA;
    if (nespad_state & DPAD_B)      j |= BTN_FIREB;
    if (nespad_state & DPAD_SELECT) j |= BTN_SELECT;
    if (nespad_state & DPAD_START)  j |= BTN_START;
#endif
    /* Merge the USB gamepad on top of the NES/SNES pad so either path
     * drives the MSX joystick. */
    j |= usbhid_wrapper_get_joystick();
    g_last_joystick = j;
}

unsigned int GetJoystick(void) {
    poll_inputs();
    return g_last_joystick;
}

unsigned int GetMouse(void)  { return 0; }
unsigned int GetKey(void)    { return 0; }
unsigned int WaitKey(void)   { return 0; }
unsigned int WaitKeyOrMouse(void) { return 0; }

void SetKeyHandler(void (*handler)(unsigned int)) { (void)handler; }

/* ==================================================================
 * Joystick()/Keyboard()/Mouse() — the MSX core's entry points
 * ================================================================== */

unsigned int Joystick(void) {
    unsigned int J = GetJoystick();

    /* Copy the accumulated key state into the MSX matrix. */
    memcpy((void *)KeyState, (const void *)XKeyState, sizeof(KeyState));

    /* Pace every MSX frame (not just drawn frames). LoopZ80 calls
     * Joystick() once per MSX frame at scanline 192, so spinning to
     * the deadline here locks virtual-MSX time to wall-clock time.
     * PutImage()'s WaitSyncTimer runs at UPeriod/100 × SyncFreq
     * (45 Hz at UPeriod=75) which is too loose to stop a fast Z80
     * from racing ahead between drawn frames.
     *
     * Target rate adapts to PAL/NTSC — VDP[9] bit 1 is set when PAL
     * video is selected. If it drifted from the initial SetSyncTimer
     * value (e.g. BIOS re-programmed VDP[9] mid-boot), refresh the
     * pacer target to match. */
    if (SyncFreq > 0 && !FastForward) {
        int want = (VDP[9] & 0x02) ? 50 : 60;
        if (g_sync_hz != want) SetSyncTimer(want);
        WaitSyncTimer();
    }

    unsigned int I = 0;
    if (J & BTN_LEFT)  I |= JST_LEFT;
    if (J & BTN_RIGHT) I |= JST_RIGHT;
    if (J & BTN_UP)    I |= JST_UP;
    if (J & BTN_DOWN)  I |= JST_DOWN;
    if (J & BTN_FIREA) I |= JST_FIREA;
    if (J & BTN_FIREB) I |= JST_FIREB;
    return I;
}

void Keyboard(void) { /* handled inline by Joystick()/poll_inputs() */ }

unsigned int Mouse(byte N) {
    /* USB mouse (if connected) surfaces on port 0 only. Port 1 stays
     * idle — MSX.c reads Mouse(0) / Mouse(1) once per frame and copes
     * with either returning zero. */
    return usbhid_wrapper_get_mouse((int)N);
}

/* ==================================================================
 * PlayAllSound()
 *
 * Called by the MSX core each scan period with a duration in
 * microseconds. We forward to RenderAndPlayAudio so the Sound.c path
 * handles mixing; WriteAudio() above is a silent sink for now.
 * ================================================================== */
void PlayAllSound(int uSec) {
    /* Request exactly `uSec` of samples at the configured rate. The
     * Unix port used 2× to avoid underruns, but our ring already
     * carries ~186 ms of headroom, so over-requesting just causes
     * core 0 to spin inside RenderAndPlayAudio once the ring fills. */
    RenderAndPlayAudio((unsigned int)uSec * (unsigned int)UseSound / 1000000u);
}

/* ==================================================================
 * PutImage() — double-buffer swap
 * ================================================================== */
void PutImage(void) {
    /* If the loader overlay is visible, paint it directly over the
     * current back buffer before we present. InMenu is raised by
     * msx_ui_show() so fMSX has already skipped its own render into
     * this frame — the buffer still contains the previous frame's
     * MSX image, which we overwrite. */
    if (msx_ui_is_visible()) {
        msx_ui_render((uint8_t *)NormScreen.Data, WIDTH, HEIGHT);
    }

    /* Pacing lives in Joystick() — fires once per MSX frame at
     * scanline 192, regardless of UPeriod, so frame-skipping doesn't
     * change emulation speed. */

    uint32_t next = current_buffer ^ 1;

    /* Tell the HDMI scanner to display what we just finished drawing. */
    graphics_set_buffer(SCREEN[current_buffer]);

    /* Rotate writes to the other buffer for next frame. */
    current_buffer = next;
    NormScreen.Data = (pixel *)SCREEN[current_buffer];
    XBuf = NormScreen.Data;
}

/* ==================================================================
 * InitMachine / TrashMachine
 * ================================================================== */

int InitMachine(void) {
    int J;

    InMenu      = 0;
    FastForward = 0;
    OldScrMode  = 0;
    NormScreen.Data = NULL;
    WideScreen.Data = NULL;

    /* Point NormScreen at the HDMI front-buffer so RefreshLine*() writes
     * into a buffer the HDMI scanner can read. */
    NormScreen.Data = (pixel *)SCREEN[0];
    NormScreen.W = NormScreen.L = WIDTH;
    NormScreen.H = HEIGHT;
    NormScreen.D = 8;
    NormScreen.Cropped = 0;
    XBuf = NormScreen.Data;
    WBuf = NULL;

    if (!SetScreenDepth(NormScreen.D)) return 0;
    SetVideo(&NormScreen, 0, 0, WIDTH, HEIGHT);

    /* Black out the palette. */
    for (J = 0; J < 80; J++) SetColor((byte)J, 0, 0, 0);
    /* SCREEN8 palette: GGGRRRBB */
    for (J = 0; J < 256; J++)
        BPal[J] = alloc_palette_entry(((J >> 2) & 0x07) * 255 / 7,
                                      ((J >> 5) & 0x07) * 255 / 7,
                                      (J & 0x03) * 255 / 3);

    memset((void *)XKeyState, 0xFF, sizeof(XKeyState));
    memset((void *)KeyState,  0xFF, sizeof(KeyState));

    /* Sound.
     *
     * PlayAudio() computes `D = (Wave × MasterVolume) >> 8` and clamps
     * to int16. At MV=128 a single full-volume channel produces
     * ±16320 on the wire — loud without clipping. Busy multi-voice
     * passages clip via the hard clamp (the "analog mixer" look):
     * slightly softened peaks but every channel stays audible. */
    InitSound(UseSound, 150);
    SndSwitch = (1 << MAXCHANNELS) - 1;
    SndVolume = 128;
    SetChannels(SndVolume, SndSwitch);
    printf("sound: MAXCHANNELS=%d SndSwitch=0x%05X volume=%d rate=%d\n",
           MAXCHANNELS, (unsigned)SndSwitch, SndVolume, UseSound);

    /* Match the MSX's internal framerate: PAL = 50 Hz, NTSC = 60 Hz.
     * Re-computed on every ResetMSX() via Joystick()'s adaptive rate
     * adjustment (see below), so this initial value just needs to be
     * in the right ballpark. */
    if (SyncFreq > 0 && !SetSyncTimer((Mode & MSX_PAL) ? 50 : 60)) SyncFreq = 0;

    msx_ui_init();
    /* Apply default visual settings (pass-through filter, CRT off).
     * Re-applied via msx_settings_apply_visual() whenever the user
     * touches Scanlines or Color filter in the Settings dialog. */
    msx_settings_apply_visual();

    return 1;
}

void TrashMachine(void) {
    SetSyncTimer(0);
    TrashSound();
}

/* Weak stubs for bits of fMSX we haven't wired yet. */
__attribute__((weak)) void MenuMSX(void) { }
