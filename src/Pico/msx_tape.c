/*
 * msx_tape.c — cassette I/O for frank-msx.
 *
 * See msx_tape.h for the interface contract. Implementation notes below.
 *
 * .CAS format recap
 * -----------------
 * A .CAS file is a plain stream of bytes. The 8-byte magic
 *   { 1F A6 DE BA CC 13 7D 74 }
 * appears at every 8-byte boundary immediately before each "block" and
 * is *not* serialised onto the wire by a real tape — the recorder
 * would emit a pilot tone instead. So our waveform generator replaces
 * each magic occurrence with a pilot, and serialises every other byte
 * as {start=0, d0..d7 LSB-first, stop=1, stop=1} at 1200 baud.
 *
 * MSX tape modulation
 * -------------------
 * Audio FSK, two tones:
 *   bit "0" -> one cycle of 1200 Hz square wave
 *   bit "1" -> two cycles of 2400 Hz square wave
 * Both take the same wall-clock time: 1/1200 s = 833.333 µs per bit
 * at the default MSX "1200 baud" speed. A pilot is a long unbroken
 * stream of "1" bits.
 *
 * The MSX tape-in hardware only sees the *square wave*, not the bits —
 * it's the BIOS (or a custom loader) that decodes the waveform back
 * into bits by timing zero-crossings. So as far as msx_tape_psg_bit7()
 * is concerned, its only job is to emit the correct square-wave level
 * as a function of wall-clock time.
 *
 * Hot-path contract
 * -----------------
 * msx_tape_psg_bit7() is called from inside the Z80 inner loop via
 * InZ80() at port 0xA2 reads. Budget: a few dozen cycles at most. We
 * keep the work down to:
 *   - one time_us_64()
 *   - one 64-bit subtract + constant multiply for phase
 *   - a small state machine advance when we cross a bit/cycle boundary
 * Everything runs without locks. Writes to the CAS state happen only
 * from the UI thread (Core 0) while the emulator is paused in the
 * overlay; during normal run the state is read-only from both cores.
 */

#include "msx_tape.h"

#include "board_config.h"
#include "psram_allocator.h"
#include "ff.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fMSX integration ---------------------------------------------- */
/* ChangeTape() / RewindTape() in fMSX/MSX.c — keeps the BIOS-trap path
 * (Patch.c TAPION/TAPIN/...) working in parallel with our real-time
 * waveform generator. A few loaders use both: BIOS for the initial
 * block header scan, then fall through to a custom loop for speed. */
typedef unsigned char byte;
extern byte ChangeTape(const char *FileName);
extern void RewindTape(void);

/* ---- Timing constants --------------------------------------------- */

/* 1200 baud. A bit period is one 1200 Hz cycle OR two 2400 Hz cycles —
 * both take the same wall-clock time. We express everything in 1/24
 * of a bit period so integer math stays exact:
 *
 *   BIT_PERIOD_US         =  1e6 / 1200           = 833 µs + 1/3
 *   HALF_2400_US          =  BIT_PERIOD_US / 4    ≈ 208 µs
 *   HALF_1200_US          =  BIT_PERIOD_US / 2    ≈ 416 µs
 *
 * We use 2500 / 3 = 833 and track fractional microseconds as a running
 * residual so long tapes don't drift.
 */
#define TAPE_BIT_PERIOD_NS     833333u   /* 1 / 1200 sec in ns */
#define TAPE_HALF_CYCLE_1_NS   208333u   /* 2400 Hz half-cycle */
#define TAPE_HALF_CYCLE_0_NS   416666u   /* 1200 Hz half-cycle */

/* Pilot / inter-block gap in bits ("1" bits). The MSX BIOS standard-
 * header pilot is 16000 cycles of 2400 Hz = ~6.6 s. For short
 * (non-standard) headers BIOS uses a ~4000-cycle pilot (~1.6 s). We
 * emit the long pilot before the first block and the short one
 * between blocks; the BIOS is tolerant of length above its minimum. */
#define TAPE_PILOT_LONG_BITS   16000
#define TAPE_PILOT_SHORT_BITS   4000

/* Standard MSX tape block header — same constant used in Patch.c. */
static const uint8_t TAPE_HEADER_MAGIC[8] = {
    0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74
};

/* ---- State --------------------------------------------------------- */

typedef enum {
    WAVE_IDLE = 0,     /* no tape / between logical sections — output 0 */
    WAVE_PILOT,        /* emitting a run of "1" bits (pilot tone) */
    WAVE_BYTE,         /* emitting start + 8 data + 2 stop bits */
} wave_phase_t;

typedef struct {
    /* Mounted image — null when no tape loaded. */
    const uint8_t *data;
    size_t         size;
    char           path[384];

    /* Cursor into data[]. Byte-granular. */
    size_t         cursor;

    /* Source selector. */
    msx_tape_source_t source;

    /* Motor gate. When off we freeze the wave-clock so the BIOS never
     * consumes bits before MOTOR ON. */
    volatile bool motor_on;

    /* Wall-clock anchor for the waveform generator. All bit timing
     * derives from (now - t_anchor). Updated only when the motor
     * toggles or when the cursor is forcibly reset. */
    uint64_t t_anchor_us;

    /* Total "wave bits" already consumed from the stream. A "wave bit"
     * is one 1200-baud slot — whether it carries a data bit, pilot,
     * or framing depends on phase. We add BYTE_BITS_COUNT (11) per
     * emitted byte and pilot_run per pilot. */
    uint64_t bits_emitted;

    /* Logical phase. */
    wave_phase_t phase;
    int          pilot_remaining;  /* >0 while in WAVE_PILOT */
    uint8_t      byte_value;       /* current byte being serialised */
    int          byte_bit_idx;     /* 0..10, 0=start, 1..8=data, 9..10=stop */
} tape_state_t;

#define BYTE_BITS_COUNT 11

static tape_state_t s_tape;

static bool s_line_gpio_ready = false;

/* See msx_tape_psg_bit7(): waveform generator off by default so BIOS
 * TAPION traps serve bytes without interference. Flipped to true by
 * the turbo-loader setting. */
static bool s_waveform_enabled = false;

/* ---- Private helpers ---------------------------------------------- */

/* Clear the byte serialiser + phase without touching the mounted
 * image. Called on rewind / eject / motor-toggle. */
static void reset_wave_clock(void) {
    s_tape.phase           = WAVE_IDLE;
    s_tape.pilot_remaining = 0;
    s_tape.byte_bit_idx    = 0;
    s_tape.byte_value      = 0;
    s_tape.bits_emitted    = 0;
    s_tape.t_anchor_us     = time_us_64();
}

/* Look at bytes[cursor..] and decide whether the next thing to emit is
 * a block header pilot or another data byte. Returns true if there's
 * anything to emit; false when EOF. Advances cursor past the magic
 * marker. Requires the caller already ensured data != NULL. */
static bool advance_to_next_unit(bool *is_pilot, int *pilot_len) {
    if (!s_tape.data || s_tape.cursor >= s_tape.size) return false;

    /* MSX .CAS files align block headers on 8-byte boundaries. A naive
     * `memcmp` at every byte would match on an accidental run of the
     * magic in data; the alignment rule cuts that to (1/256)^8 per
     * unit. */
    if ((s_tape.cursor & 7) == 0 &&
        s_tape.cursor + 8 <= s_tape.size &&
        memcmp(s_tape.data + s_tape.cursor, TAPE_HEADER_MAGIC, 8) == 0) {
        s_tape.cursor += 8;
        *is_pilot   = true;
        /* First block in file -> long pilot; subsequent -> short. */
        *pilot_len  = (s_tape.cursor == 8)
                          ? TAPE_PILOT_LONG_BITS
                          : TAPE_PILOT_SHORT_BITS;
        return true;
    }

    *is_pilot = false;
    return true;
}

/* Consume one wave-bit from the stream, updating phase. Called at most
 * once per entry into the hot path (and only when the bit counter has
 * moved forward). */
static void step_phase(void) {
    switch (s_tape.phase) {
        case WAVE_IDLE: {
            bool is_pilot = false;
            int  plen     = 0;
            if (!advance_to_next_unit(&is_pilot, &plen)) {
                /* EOF — stay idle. */
                s_tape.phase = WAVE_IDLE;
                return;
            }
            if (is_pilot) {
                s_tape.phase           = WAVE_PILOT;
                s_tape.pilot_remaining = plen;
            } else {
                s_tape.phase         = WAVE_BYTE;
                s_tape.byte_value    = s_tape.data[s_tape.cursor++];
                s_tape.byte_bit_idx  = 0;
            }
            return;
        }

        case WAVE_PILOT:
            if (--s_tape.pilot_remaining > 0) return;
            s_tape.phase = WAVE_IDLE;
            return;

        case WAVE_BYTE:
            if (++s_tape.byte_bit_idx >= BYTE_BITS_COUNT) {
                s_tape.phase = WAVE_IDLE;
            }
            return;
    }
}

/* Return the logical bit value (0 or 1) for the bit slot we're
 * currently occupying. Pilot is always 1, byte serialisation is
 * start=0, data LSB-first, stop=1,1. */
static int current_logical_bit(void) {
    switch (s_tape.phase) {
        case WAVE_PILOT:
            return 1;
        case WAVE_BYTE: {
            int idx = s_tape.byte_bit_idx;
            if (idx == 0) return 0;             /* start bit */
            if (idx <= 8) return (s_tape.byte_value >> (idx - 1)) & 1;
            return 1;                            /* stop bit */
        }
        case WAVE_IDLE:
        default:
            return 0;
    }
}

/* Compute the square-wave level for the current bit based on the
 * phase offset inside the bit period. A "1" bit runs two 2400 Hz
 * cycles, a "0" bit runs one 1200 Hz cycle — so we just slice the
 * period into 4 or 2 equal halves. */
static uint8_t square_level(int logical_bit, uint64_t phase_ns) {
    if (logical_bit) {
        /* 2400 Hz: flip every TAPE_HALF_CYCLE_1_NS. */
        return (phase_ns / TAPE_HALF_CYCLE_1_NS) & 1;
    }
    /* 1200 Hz: flip every TAPE_HALF_CYCLE_0_NS. */
    return (phase_ns / TAPE_HALF_CYCLE_0_NS) & 1;
}

/* ---- Public API --------------------------------------------------- */

void msx_tape_init(void) {
    memset(&s_tape, 0, sizeof(s_tape));
    s_tape.source  = MSX_TAPE_SOURCE_NONE;
    /* BIOS loaders release MOTOR via PPI bit 4. At power-on the MSX
     * raises MOTOR=OFF (bit set to 1); we'll get a PPIOut() transition
     * before any CLOAD, so starting gated is correct. */
    s_tape.motor_on = false;
    reset_wave_clock();

#ifdef TAPE_IN_PIN
    gpio_init(TAPE_IN_PIN);
    gpio_set_dir(TAPE_IN_PIN, GPIO_IN);
    /* Pull-down so an unconnected pin reads 0 (silence on the line).
     * Line-in hardware should drive the pin fully rail-to-rail via an
     * op-amp level shifter; the pull-down only matters when the port
     * is floating. */
    gpio_pull_down(TAPE_IN_PIN);
    s_line_gpio_ready = true;
#else
    s_line_gpio_ready = false;
#endif
}

/* Slurp an entire CAS file into PSRAM. Uses f_read so we go through
 * FatFS directly — no newlib-syscall overhead, no CWD dependency. */
static int load_cas_to_psram(const char *path, uint8_t **out_data,
                             size_t *out_size) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        printf("tape: open '%s' failed fr=%d\n", path, fr);
        return -1;
    }

    FSIZE_t sz = f_size(&f);
    if (sz == 0 || sz > 8u * 1024u * 1024u) {  /* sanity: < 8 MB */
        f_close(&f);
        printf("tape: '%s' rejected size=%llu\n", path,
               (unsigned long long)sz);
        return -2;
    }

    uint8_t *buf = (uint8_t *)psram_malloc((size_t)sz);
    if (!buf) {
        f_close(&f);
        printf("tape: psram alloc %llu failed\n", (unsigned long long)sz);
        return -3;
    }

    UINT br = 0;
    fr = f_read(&f, buf, (UINT)sz, &br);
    f_close(&f);
    if (fr != FR_OK || br != sz) {
        psram_free(buf);
        printf("tape: read short fr=%d br=%u/%llu\n",
               fr, (unsigned)br, (unsigned long long)sz);
        return -4;
    }

    *out_data = buf;
    *out_size = (size_t)sz;
    return 0;
}

int msx_tape_mount(const char *path) {
    if (!path || !*path) return -1;

    /* Release any previously mounted image before loading the new one.
     * msx_tape_eject() also clears CasStream so Patch.c stops reading
     * the old file. */
    msx_tape_eject();

    uint8_t *data = NULL;
    size_t   size = 0;
    int rc = load_cas_to_psram(path, &data, &size);
    if (rc != 0) return rc;

    /* Tell fMSX about the file too — the BIOS-trap path in Patch.c
     * still uses ChangeTape/CasStream, and several games rely on it
     * even when the main loader samples the PSG bit directly. */
    if (!ChangeTape(path)) {
        printf("tape: ChangeTape('%s') failed (BIOS path disabled)\n", path);
        /* Non-fatal — real-time waveform still works. */
    }

    s_tape.data   = data;
    s_tape.size   = size;
    s_tape.cursor = 0;
    s_tape.source = MSX_TAPE_SOURCE_CAS;
    /* Persist path so the UI can show "Currently loaded: foo.cas". */
    strncpy(s_tape.path, path, sizeof(s_tape.path) - 1);
    s_tape.path[sizeof(s_tape.path) - 1] = 0;
    reset_wave_clock();

    printf("tape: mounted %s (%u bytes)\n", path, (unsigned)size);
    return 0;
}

void msx_tape_eject(void) {
    if (s_tape.data) {
        psram_free((void *)s_tape.data);
    }
    s_tape.data   = NULL;
    s_tape.size   = 0;
    s_tape.cursor = 0;
    s_tape.path[0] = 0;
    /* Keep source at LINE if the user explicitly picked it; otherwise
     * fall back to NONE. */
    if (s_tape.source == MSX_TAPE_SOURCE_CAS)
        s_tape.source = MSX_TAPE_SOURCE_NONE;
    reset_wave_clock();
    /* Close the BIOS-side FILE handle too. */
    (void)ChangeTape(NULL);
}

void msx_tape_rewind(void) {
    if (!s_tape.data) return;
    s_tape.cursor = 0;
    reset_wave_clock();
    RewindTape();
    printf("tape: rewound\n");
}

const char *msx_tape_mounted_name(void) {
    return s_tape.path[0] ? s_tape.path : NULL;
}

msx_tape_source_t msx_tape_get_source(void) {
    return s_tape.source;
}

void msx_tape_set_source(msx_tape_source_t src) {
    if (src >= MSX_TAPE_SOURCE_COUNT) return;
    s_tape.source = src;
    reset_wave_clock();
}

void msx_tape_set_motor(bool on) {
    if (s_tape.motor_on == on) return;
    s_tape.motor_on = on;
    /* Re-anchor the wall clock when MOTOR flips on — otherwise the
     * elapsed time accumulated while idle would be mis-interpreted
     * as "bits already emitted" the moment the BIOS starts reading. */
    if (on) s_tape.t_anchor_us = time_us_64();
}

/* Strong override of MSX.c's weak hooks. fMSX calls fmsx_tape_psg_bit7()
 * inside the Z80 inner loop on every PSG[14] read, and
 * fmsx_tape_set_motor() from PPIOut() whenever PPI port C bit 4 toggles.
 * Kept as C-linkage thin wrappers so MSX.c doesn't need to know about
 * the msx_tape_source_t enum. */
uint8_t fmsx_tape_psg_bit7(void) { return msx_tape_psg_bit7(); }
void    fmsx_tape_set_motor(int on) { msx_tape_set_motor(on ? true : false); }

void msx_tape_set_waveform_enabled(bool enabled) {
    s_waveform_enabled = enabled;
    /* Re-anchor clock so the waveform starts from the current bit
     * position rather than replaying whatever time elapsed while it
     * was disabled. */
    reset_wave_clock();
}

bool msx_tape_get_waveform_enabled(void) { return s_waveform_enabled; }

/* Hot path. Called from the Z80 inner loop via MSX.c's PSG[14] read
 * path. Must be fast and reentrant-safe from either core. */
uint8_t msx_tape_psg_bit7(void) {
    switch (s_tape.source) {
        case MSX_TAPE_SOURCE_LINE:
#ifdef TAPE_IN_PIN
            if (s_line_gpio_ready) {
                /* gpio_get returns 1/0 already aligned with tape
                 * input polarity: active-high = "carrier present". */
                return (uint8_t)(gpio_get(TAPE_IN_PIN) ? 0x80 : 0x00);
            }
#endif
            return 0x00;

        case MSX_TAPE_SOURCE_CAS: {
            if (!s_tape.data || !s_tape.motor_on) return 0x00;

            /* When the user mounts a .CAS we also let fMSX's BIOS-trap
             * path (Patch.c TAPION/TAPIN) serve bytes out of CasStream.
             * That path is ~instantaneous and self-contained. The
             * waveform generator is only useful for custom loaders
             * that bypass BIOS and poll PSG[14] bit 7 directly.
             *
             * If BOTH run at the same time, BIOS's inter-block gap
             * detector (which polls PSG[14] between TAPION calls)
             * sees our waveform's constant transitions and waits
             * forever for silence — hanging BLOAD after the first
             * header is read. So real-time waveform is disabled by
             * default; a future "Turbo loader" setting will flip it
             * back on for the games that need it. */
            if (!s_waveform_enabled) return 0x00;

            /* Elapsed wall-clock nanoseconds since anchor. Using ns
             * (via µs * 1000) keeps the quotient exact at 833333 ns
             * per bit without floats. 64-bit ns wraps in 585 years,
             * so no overflow in any realistic session. */
            uint64_t elapsed_ns = (time_us_64() - s_tape.t_anchor_us) * 1000ull;
            uint64_t bit_idx    = elapsed_ns / TAPE_BIT_PERIOD_NS;

            /* Advance phase until bits_emitted catches up. The loop
             * bound protects against a pathological case (paused for
             * many seconds) where we'd otherwise spin here for ms; we
             * re-anchor in that case so subsequent reads are cheap. */
            int guard = 0;
            while (s_tape.bits_emitted < bit_idx) {
                step_phase();
                if (s_tape.phase == WAVE_IDLE && !s_tape.data) break;
                if (s_tape.phase == WAVE_IDLE && s_tape.cursor >= s_tape.size)
                    break;   /* EOF — leave remaining elapsed bits on the floor */
                ++s_tape.bits_emitted;
                if (++guard > 4096) {
                    /* Long pause / huge elapsed — re-anchor so the
                     * Z80 loop doesn't stall. Effectively pretends
                     * the skipped interval was silence. */
                    s_tape.t_anchor_us = time_us_64();
                    s_tape.bits_emitted = 0;
                    break;
                }
            }

            uint64_t phase_ns = elapsed_ns -
                (uint64_t)s_tape.bits_emitted * TAPE_BIT_PERIOD_NS;

            int lb = current_logical_bit();
            return square_level(lb, phase_ns) ? 0x80 : 0x00;
        }

        case MSX_TAPE_SOURCE_NONE:
        default:
            return 0x00;
    }
}
