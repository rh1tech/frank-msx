/*
 * frank-msx — MSX emulator for RP2350
 *
 * main() brings up the Pico (overclock, PSRAM, HDMI, audio, PS/2, NES pad,
 * SD card) and then hands control to fMSX by calling StartMSX(). The core
 * emulator expects the usual argv-style entry, so we synthesize one.
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "board_config.h"
#include "HDMI.h"
#include "psram_init.h"
#include "psram_allocator.h"
#ifndef HDMI_HSTX
#include "audio.h"
#endif
#include "ff.h"
#include "nespad/nespad.h"
#include "ps2kbd_wrapper.h"
#include "usbhid_wrapper.h"

#include "MSX.h"
#include "EMULib.h"

/* ---- Framebuffers the HDMI driver expects ---------------------------- */
/* HDMI.c reads `SCREEN[!current_buffer]` at the configured stride. We
 * allocate 272x228 per buffer (fMSX "NARROW" mode output). */
#define FB_W MSX_FB_WIDTH
#define FB_H MSX_FB_HEIGHT

static uint8_t __attribute__((aligned(4))) screen_mem[2][FB_W * FB_H];
uint8_t *SCREEN[2] = { screen_mem[0], screen_mem[1] };
volatile uint32_t current_buffer = 0;

/* Exposed to fMSX platform layer (platform.c) */
uint8_t *fmsx_frontbuffer(void) { return SCREEN[current_buffer]; }
void     fmsx_swap_frontbuffer(void) {
    current_buffer ^= 1;
}

/* ---- FatFS ----------------------------------------------------------- */
static FATFS g_fs;

/* ---- Overclocking helpers (RP2350 + PSRAM + flash) ------------------- */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor < 1) divisor = 1;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000u
                        | ((uint32_t)rxdelay  << QMI_M0_TIMING_RXDELAY_LSB)
                        | ((uint32_t)divisor  << QMI_M0_TIMING_CLKDIV_LSB);
}

/* ---- Audio path --------------------------------------------------------
 *
 * Producer (core 0): platform.c WriteAudio() writes mono 16-bit samples
 * at UseSound = 22050 Hz into g_audio_ring. Each sample is duplicated
 * into an L/R pair (stereo) on its way into the ring.
 *
 * Consumer (core 1): render_core() pulls AUDIO_FRAMES_PER_CHUNK frames
 * at a time and hands them to i2s_dma_write, which blocks on DMA
 * buffer availability. When the producer falls behind we zero-fill so
 * the I2S stream never truncates.
 *
 * Lock-free SPSC ring with a power-of-two size and 32-bit indices. */
#define AUDIO_SAMPLE_RATE       22050
#define AUDIO_FRAMES_PER_CHUNK  367                     /* ≈ 22050 / 60 */
#define AUDIO_RING_FRAMES       (1u << 12)              /* 4096 frames ≈ 186 ms */
#define AUDIO_RING_MASK         (AUDIO_RING_FRAMES - 1)

static uint32_t __attribute__((aligned(4))) g_audio_ring[AUDIO_RING_FRAMES];
static volatile uint32_t g_audio_prod = 0;  /* core 0 writes */
static volatile uint32_t g_audio_cons = 0;  /* core 1 reads  */

/* Producer API — called from platform.c:WriteAudio() on the PIO HDMI
 * path. The HSTX path drives I2S + HDMI audio directly from core 0 and
 * doesn't need the ring, but we keep these symbols defined so linker
 * references from unused fallback code still resolve. Samples are 16-bit
 * signed mono; we broadcast into L+R. Returns samples written (drops on
 * overflow rather than blocking core 0). */
unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free) count = free;
    for (unsigned i = 0; i < count; ++i) {
        int16_t s = samples[i];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned audio_ring_free(void) {
    return AUDIO_RING_FRAMES - (g_audio_prod - g_audio_cons);
}

/* ---- Render core (Core 1): boots audio + drains the ring ------------- */
static volatile bool core1_ready = false;

#ifndef HDMI_HSTX
void __time_critical_func(render_core)(void) {
    static i2s_config_t i2s_cfg;
    i2s_cfg = i2s_get_default_config();
    i2s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;
    i2s_cfg.dma_trans_count = AUDIO_FRAMES_PER_CHUNK;
    i2s_volume(&i2s_cfg, 0);
    i2s_init(&i2s_cfg);

    /* Staging buffer the DMA driver can memcpy from. Lives in SRAM. */
    static uint32_t __attribute__((aligned(32))) chunk[AUDIO_FRAMES_PER_CHUNK];

    __dmb();
    core1_ready = true;
    __dmb();

    while (true) {
        uint32_t prod = g_audio_prod;
        uint32_t cons = g_audio_cons;
        uint32_t avail = prod - cons;

        if (avail >= AUDIO_FRAMES_PER_CHUNK) {
            for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
                chunk[i] = g_audio_ring[(cons + i) & AUDIO_RING_MASK];
            __dmb();
            g_audio_cons = cons + AUDIO_FRAMES_PER_CHUNK;
        } else {
            /* Underrun — feed silence; i2s_dma_write will still pace us
             * via DMA buffer availability so we don't busy-spin. */
            for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
                chunk[i] = 0;
        }
        i2s_dma_write(&i2s_cfg, (const int16_t *)chunk);
    }
}
#endif /* !HDMI_HSTX */

/* ---- Entry point ----------------------------------------------------- */

/* fMSX's main() lives in fMSX.c. We replace it by defining our own main()
 * here and passing a synthetic argv that tells fMSX to come up in MSX1 /
 * MS-BASIC mode. The real fMSX entry is reimplemented inline. */
extern int InitMachine(void);
extern void TrashMachine(void);
extern int StartMSX(int NewMode, int NewRAMPages, int NewVRAMPages);
extern void TrashMSX(void);

int main(void) {
    /* 1. Voltage + clock for overclocking.
     *
     * Only touch vreg / QMI flash timings when genuinely overclocking
     * beyond the stock RP2350 operating point. Doing it at 252 MHz on a
     * stock board can push Flash/USB timing out of spec and stop the CDC
     * serial device from enumerating before stdio_init_all() runs. */
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
    sleep_ms(100);
#endif

    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }

    stdio_init_all();

    /* 5s startup delay for USB CDC enumeration — matches the pattern used
     * by the murmduke32 / murmapple reference projects. Unconditional
     * sleep so even cold-plugged hosts get the boot banner. */
    for (int i = 0; i < 10; ++i) sleep_ms(500);

    printf("\n========================================\n");
    printf("  frank-msx — fMSX for RP2350\n");
    printf("  version %s  board M2\n", FRANK_MSX_VERSION);
    printf("  cpu=%lu MHz psram=%d MHz flash=%d MHz\n",
           clock_get_hz(clk_sys) / 1000000u,
           PSRAM_MAX_FREQ_MHZ, FLASH_MAX_FREQ_MHZ);
    printf("========================================\n");

    /* 2. Status LED */
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    /* 3. PSRAM */
    uint psram_pin = get_psram_pin();
    printf("PSRAM pin: %u\n", psram_pin);
    psram_init(psram_pin);
    psram_reset();
    printf("PSRAM initialized (8 MB)\n");

    /* 4. SD card */
    printf("Mounting SD card...\n");
    FRESULT fr = f_mount(&g_fs, "", 1);
    if (fr != FR_OK) {
        printf("WARNING: SD card not mounted (%d). Continuing without storage.\n", fr);
    } else {
        printf("SD card mounted\n");
    }

    /* 5. Clear framebuffers */
    memset(screen_mem, 0, sizeof(screen_mem));

    /* 6. HDMI (Core 0 init, Core 1 runs audio) */
    printf("Initializing HDMI...\n");
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, FB_H);
    graphics_set_shift((320 - FB_W) / 2, 0);  /* centre horizontally */
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("HDMI initialized\n");

    /* 7. PS/2 + NES pad first, so they grab their PIO1 state machines
     * (SM0 + SM2 for PS/2) before I2S audio claims the next unused one.
     * If audio comes up first it takes PIO1 SM0 and the PS/2 driver's
     * hard-coded pio_sm_claim(pio1, 0) panics. */
    printf("Initializing PS/2 keyboard + mouse...\n");
    ps2kbd_init();
#ifdef NESPAD_GPIO_CLK
    if (nespad_begin(clock_get_hz(clk_sys) / 1000, NESPAD_GPIO_CLK,
                     NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH)) {
        printf("NES/SNES gamepad on CLK=%d DATA=%d LATCH=%d\n",
               NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH);
    }
#endif

    /* USB HID host (keyboard/mouse/gamepad). Stubs out to nothing when
     * USB_HID_ENABLED is off — the native USB port is then owned by
     * pico_stdio_usb for CDC printf instead. */
    printf("Initializing USB HID host...\n");
    usbhid_wrapper_init();

    /* 8. Launch audio core */
#ifdef HDMI_HSTX
    /* HSTX path: Core 1 is already running `video_output_core1_run` from
     * graphics_init(). Audio rides on HDMI via data-island packets pushed
     * from WriteAudio() (same model as murmnes's default HDMI audio). */
    core1_ready = true;
    printf("HDMI_HSTX: Core 1 running HSTX scanout, audio on HDMI\n");
#else
    printf("Starting render core (audio)...\n");
    multicore_launch_core1(render_core);
    while (!core1_ready) tight_loop_contents();
    printf("Render core ready\n");
#endif

#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
#endif

    /* 9. Boot the configured MSX model.
     *
     * Model selection comes from the CMake cache via FRANK_MSX_MODEL:
     *   1  -> MSX1   (needs /MSX1/MSX.ROM)
     *   2  -> MSX2   (needs /MSX2/MSX2.ROM + MSX2EXT.ROM)
     *   3  -> MSX2+  (needs /MSX2P/MSX2P.ROM + MSX2PEXT.ROM)
     */
    Verbose = 1;
#ifndef FRANK_MSX_MODEL
#define FRANK_MSX_MODEL 3   /* default: MSX2+ */
#endif

#if FRANK_MSX_MODEL == 3
    /* MSX2+ defaults: PAL region, 512 kB main RAM, 512 kB VRAM.
     * Some MegaROMs (e.g. Castlevania ROM hack of Vampire Killer)
     * hang on NTSC-60 Hz or <512 kB configs — their H.TIMI handler
     * assumes European PAL timing and mapper regions beyond bank 3.
     * PSRAM has plenty of room for max-size RAM+VRAM. */
    Mode      = MSX_MSX2P | MSX_PAL | MSX_GUESSA | MSX_GUESSB;
    RAMPages  = 32;  /* 512 kB main RAM  */
    VRAMPages = 32;  /* 512 kB VRAM      */
#elif FRANK_MSX_MODEL == 2
    Mode      = MSX_MSX2 | MSX_PAL | MSX_GUESSA | MSX_GUESSB;
    RAMPages  = 32;  /* 512 kB main RAM (matches Philips NMS-8280)   */
    VRAMPages = 8;   /* 128 kB VRAM                                  */
#else
    Mode      = MSX_MSX1 | MSX_PAL | MSX_GUESSA | MSX_GUESSB;
    RAMPages  = 4;   /* 64 kB main RAM */
    VRAMPages = 2;   /* 32 kB VRAM     */
#endif
    /* Single BIOS / ROM directory on the SD card — drop MSX.ROM,
     * MSX2.ROM/MSX2EXT.ROM, MSX2P.ROM/MSX2PEXT.ROM and optional
     * DISK.ROM side by side under /MSX/. */
    ProgDir = "/MSX";

    if (!InitMachine()) {
        printf("InitMachine() FAILED\n");
        while (true) tight_loop_contents();
    }

    /* Enter emulation — runs the Z80 until ExitNow is raised. */
    StartMSX(Mode, RAMPages, VRAMPages);

    TrashMSX();
    TrashMachine();

    printf("Emulation ended. Halting.\n");
    while (true) tight_loop_contents();
    return 0;
}
