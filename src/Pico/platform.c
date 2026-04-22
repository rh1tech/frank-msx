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
#include "audio.h"

#include "ps2kbd_wrapper.h"
#include "nespad/nespad.h"

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

/* Frame pacing (60 Hz sync timer) */
static volatile int g_sync_tick = 0;
static alarm_id_t   g_sync_alarm_id = 0;
static int          g_sync_hz = 0;

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

void SetPalette(pixel N, unsigned char R, unsigned char G, unsigned char B) {
    graphics_set_palette((uint8_t)N, ((uint32_t)R << 16) | ((uint32_t)G << 8) | B);
}

/* fMSX's Common.h uses X11GetColor() to map an (R,G,B) triple to an
 * entry in the hardware palette. We reserve entries 0-15 for the MSX's
 * 16-color palette (set via SetColor) and allocate entries 16..240 on
 * demand for SCREEN8 / YJK colors. */
static uint8_t s_pal_cursor = 16;

static unsigned int alloc_palette_entry(unsigned char R, unsigned char G, unsigned char B) {
    uint32_t rgb = ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
    uint8_t idx = s_pal_cursor++;
    if (s_pal_cursor >= 240) s_pal_cursor = 16;  /* 250-253 reserved for HDMI sync */
    graphics_set_palette(idx, rgb);
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
    uint32_t rgb = ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
    graphics_set_palette(idx, rgb);
    if (N == 0)      XPal0 = idx;
    else if (N < 80) XPal[N] = idx;
}

/* ==================================================================
 * 60Hz sync timer via repeating Pico alarm
 * ================================================================== */
static int64_t sync_alarm_cb(alarm_id_t id, void *ud) {
    (void)id; (void)ud;
    g_sync_tick = 1;
    return (g_sync_hz > 0) ? -(1000000LL / g_sync_hz) : 0;
}

int SetSyncTimer(int Hz) {
    if (g_sync_alarm_id) { cancel_alarm(g_sync_alarm_id); g_sync_alarm_id = 0; }
    g_sync_hz = Hz;
    g_sync_tick = 0;
    if (Hz <= 0) return 0;
    g_sync_alarm_id = add_alarm_in_us(1000000 / Hz, sync_alarm_cb, NULL, true);
    return 1;
}

int WaitSyncTimer(void) {
    while (!g_sync_tick) tight_loop_contents();
    g_sync_tick = 0;
    return 0;
}

int SyncTimerReady(void) { return g_sync_tick; }

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

unsigned int InitAudio(unsigned int Rate, unsigned int Latency) {
    (void)Latency;
    audio_rate_hz = Rate ? Rate : 22050;
    return audio_rate_hz;
}

void TrashAudio(void) { audio_rate_hz = 0; }

unsigned int GetFreeAudio(void)  { return AUDIO_BUF_FRAMES; }
unsigned int GetTotalAudio(void) { return AUDIO_BUF_FRAMES; }

unsigned int WriteAudio(sample *Data, unsigned int Length) {
    (void)Data;
    return Length;  /* swallow samples until I2S path is wired */
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

static void poll_inputs(void) {
    int pressed;
    unsigned char sc;
    ps2kbd_tick();
    while (ps2kbd_get_key(&pressed, &sc)) {
        unsigned char k = sc_to_msx(sc);
        if (!k) continue;
        if (pressed) XKBD_SET(k);
        else         XKBD_RES(k);
    }

#ifdef NESPAD_GPIO_CLK
    nespad_read();
    unsigned int j = 0;
    if (nespad_state & 0x10) j |= BTN_UP;
    if (nespad_state & 0x20) j |= BTN_DOWN;
    if (nespad_state & 0x40) j |= BTN_LEFT;
    if (nespad_state & 0x80) j |= BTN_RIGHT;
    if (nespad_state & 0x01) j |= BTN_FIREA;
    if (nespad_state & 0x02) j |= BTN_FIREB;
    if (nespad_state & 0x04) j |= BTN_SELECT;
    if (nespad_state & 0x08) j |= BTN_START;
    g_last_joystick = j;
#else
    g_last_joystick = 0;
#endif
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

unsigned int Mouse(byte N) { (void)N; return 0; }

/* ==================================================================
 * PlayAllSound()
 *
 * Called by the MSX core each scan period with a duration in
 * microseconds. We forward to RenderAndPlayAudio so the Sound.c path
 * handles mixing; WriteAudio() above is a silent sink for now.
 * ================================================================== */
void PlayAllSound(int uSec) {
    RenderAndPlayAudio(2 * (unsigned int)uSec * (unsigned int)UseSound / 1000000u);
}

/* ==================================================================
 * PutImage() — double-buffer swap
 * ================================================================== */
void PutImage(void) {
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

    /* Sound */
    InitSound(UseSound, 150);
    SndSwitch = (1 << MAXCHANNELS) - 1;
    SndVolume = 64;
    SetChannels(SndVolume, SndSwitch);

    if (SyncFreq > 0 && !SetSyncTimer(SyncFreq * UPeriod / 100)) SyncFreq = 0;

    return 1;
}

void TrashMachine(void) {
    SetSyncTimer(0);
    TrashSound();
}

/* Weak stubs for bits of fMSX we haven't wired yet. */
__attribute__((weak)) void MenuMSX(void) { }
