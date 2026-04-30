/*
 * msx_ui.c — loader overlay state machine + renderer.
 */

#include "msx_ui.h"
#include "msx_loader.h"
#include "msx_settings.h"
#include "msx_state.h"
#include "msx_tape.h"
#include "ui_draw.h"
#include "board_config.h"
#include "HDMI.h"

#include "MSX.h"
#include "EMULib.h"

#include <stdio.h>
#include <string.h>

/* Virtual key constants — mirror the subset our platform.c uses. */
#define XK_F11         0xFFC8
#define XK_Escape      0xFF1B
#define XK_Return      0xFF0D
#define XK_Up          0xFF52
#define XK_Down        0xFF54
#define XK_Left        0xFF51
#define XK_Right       0xFF53
#define XK_Page_Up     0xFF55
#define XK_Page_Down   0xFF56
#define XK_Tab         0xFF09
#define XK_Delete      0xFFFF

/* Layout inside the 256×228 MSX framebuffer. Use the full 256 px of
 * screen width so long labels and footers don't run out of room.
 * Height leaves a thin margin top and bottom for visual breathing. */
#define WIN_X      0
#define WIN_Y      12
#define WIN_W      256
#define WIN_H      200
#define WIN_PAD    6
#define MAX_ROWS   14   /* visible list rows */

/* Targets shown on the first page. First four rows map 1:1 to
 * msx_target_t; the two trailing rows are state-save actions that
 * route straight to the state-slot picker, bypassing the slot-action
 * context menu. */
#define TARGET_ROW_SAVE_STATE  (MSX_TARGET_COUNT + 0)
#define TARGET_ROW_LOAD_STATE  (MSX_TARGET_COUNT + 1)
#define TARGET_PAGE_ROWS       (MSX_TARGET_COUNT + 2)

static const char *TARGET_LABELS[TARGET_PAGE_ROWS] = {
    "Cartridge A",
    "Cartridge B",
    "Disk A",
    "Disk B",
    "Cassette tape",
    "Save current state",
    "Load state",
};

typedef enum {
    UI_HIDDEN = 0,
    UI_SELECT_TARGET,        /* pick Cart A/B, Disk A/B */
    UI_SLOT_ACTION,          /* context menu for the chosen slot/drive */
    UI_SELECT_FILE,          /* browse SD to pick a ROM/DSK to mount  */
    UI_SELECT_MAPPER,        /* after picking a cart ROM, choose mapper */
    UI_STATES,               /* Save-state slot picker (F5)            */
    UI_STATES_CONFIRM_DELETE,/* "Delete slot N?" prompt                */
    UI_SETTINGS,
    UI_SETTINGS_CONFIRM,
    UI_BUSY,
    UI_MESSAGE,
} ui_state_t;

static volatile ui_state_t s_state = UI_HIDDEN;
static int  s_target      = 0;
static int  s_file        = 0;
static int  s_scroll      = 0;
static int  s_slot_action = 0;   /* row in UI_SLOT_ACTION */
static int  s_setting_row = 0;
static int  s_settings_scroll = 0;
static int  s_mapper_row  = 0;    /* row in UI_SELECT_MAPPER */
static int  s_pending_file_idx = -1; /* entry chosen before mapper picker */
static int  s_state_row   = 0;    /* row in UI_STATES */
static int  s_state_action = 0;   /* 0=Save, 1=Load */
static ui_state_t s_msg_return = UI_SLOT_ACTION; /* page to return to after UI_MESSAGE */
static bool s_dirty       = true;
static char s_msg[96];

/* Which "mount" behaviour to apply when a file is picked. Cartridge
 * slots force a reset; floppy drives don't. Set when entering
 * UI_SELECT_FILE from UI_SLOT_ACTION. */
static bool s_mount_reset_after = false;

extern int InMenu;             /* fMSX flag — silences input */

/* Forward decls so page helpers that live later in the file can call
 * the chrome/content renderers that are defined near the bottom. */
static void draw_chrome(uint8_t *fb, int stride, const char *title);
static void draw_footer(uint8_t *fb, int stride, const char *hint);
static int  content_x(void);
static int  content_y(void);
static int  content_w(void);

void msx_ui_init(void) {
    ui_draw_install_palette();
    s_state = UI_HIDDEN;
    InMenu  = 0;
}

bool msx_ui_is_visible(void) { return s_state != UI_HIDDEN; }

void msx_ui_show(void) {
    if (s_state != UI_HIDDEN) return;
    msx_rescan();
    s_state  = UI_SELECT_TARGET;
    s_target = 0;
    s_file   = 0;
    s_scroll = 0;
    s_dirty  = true;
    InMenu   = 1;
}

void msx_ui_show_settings(void) {
    if (s_state != UI_HIDDEN) return;
    msx_settings_init_from_bootstate();
    s_setting_row     = 0;
    s_settings_scroll = 0;
    s_state  = UI_SETTINGS;
    s_dirty  = true;
    InMenu   = 1;
}

extern volatile byte XKeyState[20];
extern int ps2kbd_get_key(int *pressed, unsigned char *sc);
#include "usbhid_wrapper.h"

void msx_ui_hide(void) {
    s_state = UI_HIDDEN;
    InMenu  = 0;
    /* Drain any host key events that were buffered while the overlay
     * was active — otherwise a stale release (e.g. the Enter we used
     * to confirm a load-state) bleeds into the MSX matrix right after
     * LoadState restores the snapshot. */
    { int p; unsigned char sc;
      while (ps2kbd_get_key(&p, &sc))          { }
      while (usbhid_wrapper_get_key(&p, &sc))  { } }
    /* Reset both the host-visible shadow matrix and the core-facing
     * KeyState, so Joystick()'s XKeyState→KeyState copy can't re-inject
     * the pre-overlay state. 0xFF = "no keys pressed" in fMSX's matrix. */
    memset((void *)XKeyState, 0xFF, sizeof(XKeyState));
    memset((void *)KeyState,  0xFF, sizeof(KeyState));
}

void msx_ui_toggle(void) {
    if (s_state == UI_HIDDEN) msx_ui_show();
    else                      msx_ui_hide();
}

void msx_ui_toggle_settings(void) {
    if (s_state == UI_HIDDEN) msx_ui_show_settings();
    else                      msx_ui_hide();
}

void msx_ui_toggle_states(void) {
    if (s_state != UI_HIDDEN) { msx_ui_hide(); return; }
    s_state_row = 0;
    s_state_action = 0;
    s_state = UI_STATES;
    s_dirty = true;
    InMenu = 1;
}

extern uint8_t *SCREEN[2];
extern volatile uint32_t current_buffer;

/* Synchronously paint a "Working..." dialog with a custom message
 * into the HDMI front buffer and return. Used to bracket long SD /
 * dlmalloc operations so the user sees activity instead of a stuck
 * frame. */
void msx_ui_show_busy(const char *message) {
    uint8_t *fb = SCREEN[current_buffer];
    int x = WIN_X + WIN_PAD;
    int cw = WIN_W - 2 * WIN_PAD;

    ui_fill_rect  (fb, MSX_FB_WIDTH, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_BG);
    ui_draw_border(fb, MSX_FB_WIDTH, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_FG);
    ui_draw_header(fb, MSX_FB_WIDTH, WIN_X, WIN_Y, WIN_W, " Working... ");

    int max_chars = (cw - 4) / UI_CHAR_W;
    ui_draw_string_truncated(fb, MSX_FB_WIDTH, x,
                             WIN_Y + WIN_H / 2 - UI_CHAR_H / 2,
                             message ? message : "Please wait...",
                             max_chars, UI_COLOR_FG);

    /* Point the HDMI scanner at the buffer we just drew. The scanner
     * reads SCREEN[!current_buffer], so swap first. */
    graphics_set_buffer(fb);
    current_buffer ^= 1;
}

/* ---- helpers ---------------------------------------------------- */

static void clamp_file_scroll(void) {
    if (s_file < 0) s_file = 0;
    if (s_file >= msx_entry_count + 1) s_file = msx_entry_count; /* +1 for ".." */
    int total = msx_entry_count + (strcmp(msx_current_dir, "/") != 0 ? 1 : 0);
    if (s_file >= total) s_file = total - 1;
    if (s_file < 0) s_file = 0;
    if (s_file < s_scroll) s_scroll = s_file;
    else if (s_file >= s_scroll + MAX_ROWS) s_scroll = s_file - MAX_ROWS + 1;
    if (s_scroll < 0) s_scroll = 0;
}

/* Returns the "real" entry index for list row `row`, or -1 for the
 * synthetic ".." entry. */
static int list_row_to_entry(int row, bool *is_parent) {
    bool has_parent = strcmp(msx_current_dir, "/") != 0;
    if (is_parent) *is_parent = false;
    if (has_parent) {
        if (row == 0) { if (is_parent) *is_parent = true; return -1; }
        return row - 1;
    }
    return row;
}

/* Decide which target a filename can be mounted to. */
static bool target_accepts(msx_target_t t, msx_entry_kind_t k) {
    if (t == MSX_TARGET_CART_A || t == MSX_TARGET_CART_B) return k == MSX_ENTRY_ROM;
    if (t == MSX_TARGET_DISK_A || t == MSX_TARGET_DISK_B) return k == MSX_ENTRY_DISK;
    if (t == MSX_TARGET_TAPE) return k == MSX_ENTRY_TAPE;
    return false;
}

/* ---- key handling ---------------------------------------------- */

static bool handle_target_page(unsigned int xk) {
    switch (xk) {
        case XK_Up:
            if (s_target > 0) --s_target; else s_target = TARGET_PAGE_ROWS - 1;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_target >= TARGET_PAGE_ROWS) s_target = 0;
            s_dirty = true; return true;
        case XK_Return:
            if (s_target == TARGET_ROW_SAVE_STATE) {
                s_state_row = 0;
                s_state_action = 0;     /* Save */
                s_state = UI_STATES;
                s_dirty = true;
            } else if (s_target == TARGET_ROW_LOAD_STATE) {
                s_state_row = 0;
                s_state_action = 1;     /* Load */
                s_state = UI_STATES;
                s_dirty = true;
            } else {
                s_state        = UI_SLOT_ACTION;
                s_slot_action  = 0;
                s_dirty        = true;
            }
            return true;
        case XK_Escape:
            msx_ui_hide(); return true;
    }
    return false;
}

/* ---- Slot-action page — context menu for the chosen slot/drive ---- */

/* Dynamic action list. The rows depend on slot type (cart vs disk)
 * and whether something is currently mounted. We build a tiny
 * per-action descriptor so the handler and renderer agree. */
typedef enum {
    SLOT_ACT_INSERT = 0,      /* cart: insert + reset, disk/tape: insert */
    SLOT_ACT_CHANGE,          /* same as INSERT but shown as "Change"    */
    SLOT_ACT_EJECT,
    SLOT_ACT_CREATE_BLANK,    /* disk only */
    SLOT_ACT_SAVE_DSK,        /* disk only */
    SLOT_ACT_SAVE_FDI,        /* disk only */
    SLOT_ACT_REWIND,          /* tape only */
    SLOT_ACT_TURBO_TOGGLE,    /* tape only — toggle waveform generator */
    SLOT_ACT_CANCEL,
} slot_act_kind_t;

typedef struct {
    slot_act_kind_t kind;
    const char     *label;
} slot_act_row_t;

#define MAX_SLOT_ACTS 8

/* Human-friendly noun for the slot-action labels. */
static const char *slot_noun_for(msx_target_t t) {
    switch (t) {
        case MSX_TARGET_DISK_A:
        case MSX_TARGET_DISK_B: return "disk";
        case MSX_TARGET_TAPE:   return "tape";
        default:                return "cartridge";
    }
}

static int build_slot_actions(slot_act_row_t out[MAX_SLOT_ACTS]) {
    msx_target_t t = (msx_target_t)s_target;
    bool is_disk   = (t == MSX_TARGET_DISK_A || t == MSX_TARGET_DISK_B);
    bool is_tape   = (t == MSX_TARGET_TAPE);
    bool loaded    = msx_mounted_name(t) != NULL;
    int n = 0;
    const char *noun = slot_noun_for(t);

    if (loaded) {
        /* "Change <noun>" / "Eject" are common to all three kinds.
         * Disk gets extra save options; tape gets Rewind + turbo. */
        static char change_label[24];
        snprintf(change_label, sizeof(change_label), "Change %s", noun);
        out[n++] = (slot_act_row_t){ SLOT_ACT_CHANGE, change_label };
        out[n++] = (slot_act_row_t){ SLOT_ACT_EJECT,  "Eject" };
        if (is_disk) {
            out[n++] = (slot_act_row_t){ SLOT_ACT_SAVE_DSK, "Save as .DSK" };
            out[n++] = (slot_act_row_t){ SLOT_ACT_SAVE_FDI, "Save as .FDI" };
        }
        if (is_tape) {
            out[n++] = (slot_act_row_t){ SLOT_ACT_REWIND, "Rewind" };
            /* Label is regenerated on every render so it reflects the
             * live waveform-generator state. Points at a static buffer
             * — there's only ever one turbo row on screen at a time. */
            static char turbo_label[32];
            snprintf(turbo_label, sizeof(turbo_label),
                     "Turbo loader: %s",
                     msx_tape_get_waveform_enabled() ? "On" : "Off");
            out[n++] = (slot_act_row_t){ SLOT_ACT_TURBO_TOGGLE, turbo_label };
        }
    } else {
        static char insert_label[24];
        snprintf(insert_label, sizeof(insert_label), "Insert %s", noun);
        out[n++] = (slot_act_row_t){ SLOT_ACT_INSERT, insert_label };
        if (is_disk)
            out[n++] = (slot_act_row_t){ SLOT_ACT_CREATE_BLANK, "Create new blank disk" };
    }
    out[n++] = (slot_act_row_t){ SLOT_ACT_CANCEL, "Cancel" };
    return n;
}

/* If `success_msg` is non-NULL we stay on the UI and pop a Notice;
 * otherwise we hide the overlay (nothing more to tell the user). */
static void report_status(int rc, const char *fail_prefix,
                          const char *success_msg) {
    if (rc == 0) {
        if (success_msg) {
            snprintf(s_msg, sizeof(s_msg), "%s", success_msg);
            s_state = UI_MESSAGE;
            s_dirty = true;
        } else {
            msx_ui_hide();
        }
    } else {
        snprintf(s_msg, sizeof(s_msg), "%s (code %d)",
                 fail_prefix ? fail_prefix : "Failed", rc);
        s_state = UI_MESSAGE;
        s_dirty = true;
    }
}

static void perform_slot_action(slot_act_kind_t k) {
    msx_target_t t  = (msx_target_t)s_target;
    bool is_cart    = (t == MSX_TARGET_CART_A || t == MSX_TARGET_CART_B);

    switch (k) {
        case SLOT_ACT_INSERT:
        case SLOT_ACT_CHANGE:
            /* Hand off to the file picker. Cartridge always resets
             * after mount; disk does not. */
            s_mount_reset_after = is_cart;
            s_state  = UI_SELECT_FILE;
            s_file   = 0;
            s_scroll = 0;
            s_dirty  = true;
            return;
        case SLOT_ACT_EJECT:
            msx_ui_show_busy("Ejecting...");
            report_status(msx_eject(t), "Eject failed", NULL);
            return;
        case SLOT_ACT_CREATE_BLANK:
            msx_ui_show_busy("Creating blank disk image...");
            report_status(msx_create_blank_disk(t), "Create failed",
                          "Blank disk created and mounted.");
            return;
        case SLOT_ACT_SAVE_DSK:
            msx_ui_show_busy("Saving disk as .DSK...");
            report_status(msx_save_disk(t, 7  /* FMT_MSXDSK */),
                          "Save failed",
                          "Saved as .DSK under /MSX/.");
            return;
        case SLOT_ACT_SAVE_FDI:
            msx_ui_show_busy("Saving disk as .FDI...");
            report_status(msx_save_disk(t, 4  /* FMT_FDI */),
                          "Save failed",
                          "Saved as .FDI under /MSX/.");
            return;
        case SLOT_ACT_REWIND:
            /* Only reachable from the tape slot. No SD/PSRAM work — just
             * reset the tape cursor. */
            msx_tape_rewind();
            snprintf(s_msg, sizeof(s_msg), "Tape rewound.");
            s_state = UI_MESSAGE;
            s_msg_return = UI_SLOT_ACTION;
            s_dirty = true;
            return;
        case SLOT_ACT_TURBO_TOGGLE:
            /* Flip the real-time waveform generator. Route through the
             * settings layer so the toggle is persisted to msx.ini and
             * survives a reboot. Stays on the slot-action page so the
             * user can immediately verify the new state in the label. */
            g_settings.turbo_tape = !g_settings.turbo_tape;
            msx_tape_set_waveform_enabled(g_settings.turbo_tape != 0);
            msx_settings_save();
            s_dirty = true;
            return;
        case SLOT_ACT_CANCEL:
            s_state = UI_SELECT_TARGET;
            s_dirty = true;
            return;
    }
}

static bool handle_slot_action_page(unsigned int xk) {
    slot_act_row_t rows[MAX_SLOT_ACTS];
    int n = build_slot_actions(rows);

    switch (xk) {
        case XK_Up:
            if (s_slot_action > 0) --s_slot_action;
            else s_slot_action = n - 1;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_slot_action >= n) s_slot_action = 0;
            s_dirty = true; return true;
        case XK_Escape:
            s_state = UI_SELECT_TARGET;
            s_dirty = true; return true;
        case XK_Return:
            if (s_slot_action >= 0 && s_slot_action < n)
                perform_slot_action(rows[s_slot_action].kind);
            return true;
    }
    return false;
}

/* ---- Settings page ------------------------------------------------ */

/* Row layout: one row per setting, then Apply and "Back to MSX".
 * Restart lives outside the menu on Ctrl+Alt+Del. */
#define SETTINGS_APPLY_ROW   (MSX_SETTING_COUNT)
#define SETTINGS_BACK_ROW    (MSX_SETTING_COUNT + 1)
#define SETTINGS_TOTAL_ROWS  (MSX_SETTING_COUNT + 2)

/* Window height fits ~16 rows after chrome; scroll when settings grow.
 * Leave one row's worth of breathing room above the footer so the last
 * visible setting isn't touching the legend. */
#define SETTINGS_VISIBLE_ROWS 14

static void clamp_settings_scroll(void) {
    if (s_setting_row < s_settings_scroll)
        s_settings_scroll = s_setting_row;
    else if (s_setting_row >= s_settings_scroll + SETTINGS_VISIBLE_ROWS)
        s_settings_scroll = s_setting_row - SETTINGS_VISIBLE_ROWS + 1;
    if (s_settings_scroll < 0) s_settings_scroll = 0;
    if (s_settings_scroll > SETTINGS_TOTAL_ROWS - SETTINGS_VISIBLE_ROWS)
        s_settings_scroll = SETTINGS_TOTAL_ROWS - SETTINGS_VISIBLE_ROWS;
    if (s_settings_scroll < 0) s_settings_scroll = 0;
}

static bool handle_settings_page(unsigned int xk) {
    switch (xk) {
        case XK_Escape:
            /* Settings is a standalone dialog — Esc closes the whole
             * overlay rather than returning to the loader. */
            msx_ui_hide();
            return true;
        case XK_Up:
            if (s_setting_row > 0) --s_setting_row;
            else s_setting_row = SETTINGS_TOTAL_ROWS - 1;
            clamp_settings_scroll();
            s_dirty = true; return true;
        case XK_Down:
            if (++s_setting_row >= SETTINGS_TOTAL_ROWS) s_setting_row = 0;
            clamp_settings_scroll();
            s_dirty = true; return true;
        case XK_Page_Up:
            s_setting_row -= SETTINGS_VISIBLE_ROWS / 2;
            if (s_setting_row < 0) s_setting_row = 0;
            clamp_settings_scroll();
            s_dirty = true; return true;
        case XK_Page_Down:
            s_setting_row += SETTINGS_VISIBLE_ROWS / 2;
            if (s_setting_row >= SETTINGS_TOTAL_ROWS)
                s_setting_row = SETTINGS_TOTAL_ROWS - 1;
            clamp_settings_scroll();
            s_dirty = true; return true;
        case XK_Left:
            if (s_setting_row < MSX_SETTING_COUNT)
                msx_settings_step((msx_setting_id_t)s_setting_row, -1);
            s_dirty = true; return true;
        case XK_Right:
            if (s_setting_row < MSX_SETTING_COUNT)
                msx_settings_step((msx_setting_id_t)s_setting_row, +1);
            s_dirty = true; return true;
        case XK_Return:
            if (s_setting_row == SETTINGS_APPLY_ROW) {
                s_state = UI_SETTINGS_CONFIRM;
                s_dirty = true;
            } else if (s_setting_row == SETTINGS_BACK_ROW) {
                msx_ui_hide();
            }
            return true;
    }
    return false;
}

static bool handle_settings_confirm(unsigned int xk) {
    switch (xk) {
        case XK_Escape:
            s_state = UI_SETTINGS;
            s_dirty = true;
            return true;
        case XK_Return:
            /* Hide UI first so PutImage stops overlaying, then reset
             * the MSX. ResetMSX() reallocates RAM/VRAM, reloads the
             * BIOS, and clears cart state if the Model changed. */
            msx_ui_hide();
            msx_settings_apply_and_reset();
            return true;
    }
    return false;
}

/* ---- Save-state slot picker ------------------------------------- */

/* Layout: header = "Save / Load", row per slot = "Slot N  [used/empty]".
 * Reached from the F11 loader target page (Save current state / Load state
 * rows). Esc returns to that target page. */
static bool handle_states_page(unsigned int xk) {
    switch (xk) {
        case XK_Escape:
            s_state = UI_SELECT_TARGET;
            s_dirty = true;
            return true;
        case XK_Up:
            if (s_state_row > 0) --s_state_row;
            else s_state_row = MSX_STATE_SLOTS - 1;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_state_row >= MSX_STATE_SLOTS) s_state_row = 0;
            s_dirty = true; return true;
        case XK_Delete:
            /* Only prompt when the slot actually holds something —
             * deleting an empty slot is a no-op we don't want to
             * confirm for. */
            if (msx_state_slot_exists(s_state_row)) {
                s_state = UI_STATES_CONFIRM_DELETE;
                s_dirty = true;
            }
            return true;
        case XK_Return: {
            int rc;
            if (s_state_action == 0) {
                msx_ui_show_busy("Saving state to PSRAM + SD...");
                rc = msx_state_save(s_state_row);
                if (rc == 0) {
                    snprintf(s_msg, sizeof(s_msg),
                             "State saved to slot %d.", s_state_row);
                    /* Success notice — dismiss closes the overlay so
                     * the user drops straight back into the game. */
                    s_msg_return = UI_HIDDEN;
                    s_state = UI_MESSAGE; s_dirty = true;
                } else {
                    snprintf(s_msg, sizeof(s_msg),
                             "Save failed (code %d)", rc);
                    s_msg_return = UI_STATES;
                    s_state = UI_MESSAGE; s_dirty = true;
                }
            } else {
                if (!msx_state_slot_exists(s_state_row)) {
                    snprintf(s_msg, sizeof(s_msg),
                             "Slot %d is empty.", s_state_row);
                    s_msg_return = UI_STATES;
                    s_state = UI_MESSAGE; s_dirty = true;
                    return true;
                }
                msx_ui_show_busy("Loading state from SD...");
                rc = msx_state_load(s_state_row);
                if (rc == 0) {
                    /* Hide and resume — the MSX state is now restored. */
                    msx_ui_hide();
                } else {
                    snprintf(s_msg, sizeof(s_msg),
                             "Load failed (code %d)", rc);
                    s_msg_return = UI_STATES;
                    s_state = UI_MESSAGE; s_dirty = true;
                }
            }
            return true;
        }
    }
    return false;
}

static bool handle_states_confirm_delete(unsigned int xk) {
    switch (xk) {
        case XK_Escape:
            s_state = UI_STATES;
            s_dirty = true;
            return true;
        case XK_Return: {
            int rc = msx_state_delete(s_state_row);
            if (rc == 0) {
                snprintf(s_msg, sizeof(s_msg),
                         "Slot %d deleted.", s_state_row);
            } else {
                snprintf(s_msg, sizeof(s_msg),
                         "Delete failed (code %d)", rc);
            }
            /* Back to the slot picker so the user can see the updated
             * [used/empty] hint and continue working. */
            s_msg_return = UI_STATES;
            s_state = UI_MESSAGE;
            s_dirty = true;
            return true;
        }
    }
    return false;
}

static void render_states_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, s_state_action == 0 ? " Save state " : " Load state ");
    int x = content_x(), y = content_y();
    int cw = content_w();
    int max_chars = (cw - 4) / UI_CHAR_W;

    for (int i = 0; i < MSX_STATE_SLOTS; ++i) {
        bool sel = (i == s_state_row);
        char line[48];
        snprintf(line, sizeof(line), "Slot %d   %s",
                 i, msx_state_slot_exists(i) ? "[used]" : "[empty]");
        ui_draw_menu_item(fb, stride, x, y, cw, line, max_chars, sel);
        y += UI_LINE_H + 1;
    }
    y += 6;
    ui_draw_string(fb, stride, x, y,
        s_state_action == 0 ? "ENTER: save selected slot"
                            : "ENTER: load selected slot", UI_COLOR_FG);
    y += UI_LINE_H + 1;
    ui_draw_string(fb, stride, x, y,
        "DEL: erase selected slot", UI_COLOR_FG);
    draw_footer(fb, stride, "UP/DN  ENTER  DEL  ESC back");
}

static void render_states_confirm_delete(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Delete slot ");
    int x = content_x(), y = content_y();
    char line[48];
    snprintf(line, sizeof(line), "Delete save state in slot %d?",
             s_state_row);
    ui_draw_string(fb, stride, x, y, line, UI_COLOR_FG);
    y += UI_LINE_H + 2;
    ui_draw_string(fb, stride, x, y,
        "This cannot be undone.", UI_COLOR_FG);
    draw_footer(fb, stride, "ENTER confirm  ESC cancel");
}

/* ---- Mapper picker (cartridge only) ----------------------------- */

/* MAP_* values from MSX.h: 0=Gen8, 1=Gen16, 2=Konami5, 3=Konami4,
 * 4=ASCII8, 5=ASCII16, 6=GMaster2, 7=FMPAC, 8=GUESS (our default). */
static const struct { int id; const char *label; } MAPPER_CHOICES[] = {
    { 8, "Auto (guess)" },
    { 0, "Generic 8 KB" },
    { 1, "Generic 16 KB" },
    { 2, "Konami (SCC, 5/7/9/B)" },
    { 3, "Konami (4/6/8/A)" },
    { 4, "ASCII 8 KB" },
    { 5, "ASCII 16 KB" },
    { 6, "GameMaster 2" },
    { 7, "FMPAC" },
};
#define MAPPER_CHOICE_COUNT ((int)(sizeof(MAPPER_CHOICES)/sizeof(MAPPER_CHOICES[0])))

static bool handle_mapper_page(unsigned int xk) {
    switch (xk) {
        case XK_Up:
            if (s_mapper_row > 0) --s_mapper_row;
            else s_mapper_row = MAPPER_CHOICE_COUNT - 1;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_mapper_row >= MAPPER_CHOICE_COUNT) s_mapper_row = 0;
            s_dirty = true; return true;
        case XK_Escape:
            s_state = UI_SELECT_FILE;
            s_dirty = true; return true;
        case XK_Return: {
            int mapper = MAPPER_CHOICES[s_mapper_row].id;
            int idx = s_pending_file_idx;
            if (idx < 0) { msx_ui_hide(); return true; }
            s_state = UI_BUSY;
            msx_ui_show_busy("Loading cartridge...");
            int rc = msx_mount_entry_with_mapper(idx, (msx_target_t)s_target,
                                                 mapper, s_mount_reset_after);
            s_pending_file_idx = -1;
            if (rc == 0) msx_ui_hide();
            else {
                snprintf(s_msg, sizeof(s_msg),
                         "Mount failed (code %d)", rc);
                s_state = UI_MESSAGE; s_dirty = true;
            }
            return true;
        }
    }
    return false;
}

static bool handle_file_page(unsigned int xk) {
    bool has_parent = strcmp(msx_current_dir, "/") != 0;
    int total = msx_entry_count + (has_parent ? 1 : 0);

    switch (xk) {
        case XK_Up:
            if (total == 0) return true;
            if (s_file > 0) --s_file; else s_file = total - 1;
            clamp_file_scroll(); s_dirty = true; return true;
        case XK_Down:
            if (total == 0) return true;
            if (++s_file >= total) s_file = 0;
            clamp_file_scroll(); s_dirty = true; return true;
        case XK_Page_Up:
            s_file -= MAX_ROWS / 2;
            clamp_file_scroll(); s_dirty = true; return true;
        case XK_Page_Down:
            s_file += MAX_ROWS / 2;
            clamp_file_scroll(); s_dirty = true; return true;
        case XK_Escape:
            /* Back to the slot's action menu, not all the way to target. */
            s_state = UI_SLOT_ACTION;
            s_dirty = true; return true;
        case XK_Return: {
            if (total == 0) return true;
            bool is_parent;
            int  e = list_row_to_entry(s_file, &is_parent);
            if (is_parent) {
                msx_enter_parent();
                s_file = 0; s_scroll = 0;
                s_dirty = true; return true;
            }
            if (e < 0 || e >= msx_entry_count) return true;
            if (msx_entries[e].kind == MSX_ENTRY_DIR) {
                msx_enter_subdir(msx_entries[e].name);
                s_file = 0; s_scroll = 0;
                s_dirty = true; return true;
            }
            if (!target_accepts((msx_target_t)s_target, msx_entries[e].kind)) {
                snprintf(s_msg, sizeof(s_msg), "Wrong file type for %s",
                         TARGET_LABELS[s_target]);
                s_state = UI_MESSAGE;
                s_dirty = true; return true;
            }

            /* Cartridge: ask for mapper first.
             * Disk: mount immediately (no mapper concept). */
            bool is_cart = (s_target == MSX_TARGET_CART_A ||
                            s_target == MSX_TARGET_CART_B);
            if (is_cart) {
                s_pending_file_idx = e;
                s_mapper_row = 0;       /* default = Auto */
                s_state = UI_SELECT_MAPPER;
                s_dirty = true;
                return true;
            }

            s_state = UI_BUSY;
            s_dirty = true;
            const char *busy_msg = (s_target == MSX_TARGET_TAPE)
                                       ? "Loading tape..."
                                       : "Loading disk image...";
            msx_ui_show_busy(busy_msg);
            int rc = msx_mount_entry(e, (msx_target_t)s_target,
                                     s_mount_reset_after);
            if (rc == 0) {
                msx_ui_hide();
            } else {
                snprintf(s_msg, sizeof(s_msg), "Mount failed (code %d)", rc);
                s_state = UI_MESSAGE;
                s_dirty = true;
            }
            return true;
        }
    }
    return false;
}

bool msx_ui_handle_key(unsigned int xk) {
    if (xk == XK_F11) { msx_ui_toggle(); return true; }
    if (s_state == UI_HIDDEN) return false;
    if (s_state == UI_MESSAGE) {
        if (xk == XK_Return || xk == XK_Escape) {
            /* After dismissing a notice, close the overlay — most
             * messages report the outcome of an action the user just
             * finished (save-state, mount, eject), so returning to
             * the emulator is what they want. The slot-action / other
             * pages that want a different flow should set
             * s_msg_return before raising UI_MESSAGE. */
            if (s_msg_return == UI_HIDDEN) {
                msx_ui_hide();
            } else {
                s_state = s_msg_return;
                s_msg_return = UI_HIDDEN;   /* reset for next time */
                s_dirty = true;
            }
        }
        return true;
    }
    if (s_state == UI_BUSY) return true;
    if (s_state == UI_SELECT_TARGET)    return handle_target_page(xk);
    if (s_state == UI_SLOT_ACTION)      return handle_slot_action_page(xk);
    if (s_state == UI_SELECT_FILE)      return handle_file_page(xk);
    if (s_state == UI_SELECT_MAPPER)    return handle_mapper_page(xk);
    if (s_state == UI_STATES)           return handle_states_page(xk);
    if (s_state == UI_STATES_CONFIRM_DELETE)
                                        return handle_states_confirm_delete(xk);
    if (s_state == UI_SETTINGS)         return handle_settings_page(xk);
    if (s_state == UI_SETTINGS_CONFIRM) return handle_settings_confirm(xk);
    return false;
}

/* ---- renderer --------------------------------------------------- */

static int content_x(void) { return WIN_X + WIN_PAD; }
static int content_y(void) { return WIN_Y + UI_HEADER_H + WIN_PAD; }
static int content_w(void) { return WIN_W - 2 * WIN_PAD; }

static void draw_chrome(uint8_t *fb, int stride, const char *title) {
    ui_fill_rect  (fb, stride, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_BG);
    ui_draw_border(fb, stride, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_FG);
    ui_draw_header(fb, stride, WIN_X, WIN_Y, WIN_W, title);
}

static void draw_footer(uint8_t *fb, int stride, const char *hint) {
    int fy = WIN_Y + WIN_H - UI_LINE_H - WIN_PAD;
    ui_fill_rect  (fb, stride, content_x(), fy, content_w(), UI_LINE_H, UI_COLOR_BG);
    ui_draw_string(fb, stride, content_x(), fy + 1, hint, UI_COLOR_FG);
}

static void render_target_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " frank-msx Loader ");
    int y  = content_y() + 6;
    int x  = content_x();
    int cw = content_w();

    for (int i = 0; i < TARGET_PAGE_ROWS; ++i) {
        bool sel = (i == s_target);
        uint8_t bg = sel ? UI_COLOR_ACCENT    : UI_COLOR_BG;
        uint8_t fg = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

        /* Background + label on the left. */
        ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);
        ui_draw_string(fb, stride, x + 2, y + 1, TARGET_LABELS[i], fg);

        /* Right-hand hint:
         *   cart / disk rows    -> currently-mounted filename or (empty)
         *   save-state row      -> count of used slots
         *   load-state row      -> same                                   */
        const char *display = NULL;
        char buf[32];
        if (i < MSX_TARGET_COUNT) {
            const char *mounted = msx_mounted_name((msx_target_t)i);
            if (mounted) {
                const char *slash = strrchr(mounted, '/');
                display = slash ? slash + 1 : mounted;
            } else {
                display = "(empty)";
            }
        } else {
            int used = 0;
            for (int s = 0; s < MSX_STATE_SLOTS; ++s)
                if (msx_state_slot_exists(s)) ++used;
            snprintf(buf, sizeof(buf), "%d/%d used", used, MSX_STATE_SLOTS);
            display = buf;
        }
        int avail = cw - 2 - (int)strlen(TARGET_LABELS[i]) * UI_CHAR_W - 8;
        int max_right = avail > 0 ? avail / UI_CHAR_W : 0;
        int dlen = (int)strlen(display);
        if (max_right < 3) max_right = 3;
        int shown = (dlen <= max_right) ? dlen : max_right;
        int dx = x + cw - 4 - shown * UI_CHAR_W;
        ui_draw_string_truncated(fb, stride, dx, y + 1, display, max_right, fg);

        y += UI_LINE_H + 2;
    }
    draw_footer(fb, stride, "UP/DN  ENTER select  ESC exit");
}

static void render_file_page(uint8_t *fb, int stride) {
    char title[64];
    snprintf(title, sizeof(title), " %s  %s ", TARGET_LABELS[s_target], msx_current_dir);
    draw_chrome(fb, stride, title);

    bool has_parent = strcmp(msx_current_dir, "/") != 0;
    int total = msx_entry_count + (has_parent ? 1 : 0);
    int y = content_y();
    int max_chars = (content_w() - 12) / UI_CHAR_W;

    if (total == 0) {
        ui_draw_string(fb, stride, content_x(), y, "(empty)", UI_COLOR_FG);
        ui_draw_string(fb, stride, content_x(), y + UI_LINE_H + 4,
                       "Put ROMs and DSKs on the SD card", UI_COLOR_FG);
    } else {
        int rows = (total < MAX_ROWS) ? total : MAX_ROWS;
        for (int i = 0; i < rows; ++i) {
            int row = s_scroll + i;
            if (row >= total) break;
            bool is_parent;
            int ei = list_row_to_entry(row, &is_parent);
            const char *label;
            char buf[MSX_MAX_FILENAME_LEN + 4];
            if (is_parent) {
                label = "..";
            } else {
                msx_entry_t *e = &msx_entries[ei];
                if (e->kind == MSX_ENTRY_DIR) {
                    snprintf(buf, sizeof(buf), "[%s]", e->name);
                    label = buf;
                } else {
                    label = e->name;
                }
            }
            ui_draw_menu_item(fb, stride, content_x(), y, content_w() - 8,
                              label, max_chars, row == s_file);
            y += UI_LINE_H;
        }
        if (msx_entry_count > MAX_ROWS) {
            ui_draw_scrollbar(fb, stride,
                              WIN_X + WIN_W - WIN_PAD - 4, content_y(),
                              rows * UI_LINE_H,
                              total, rows, s_scroll);
        }
    }
    draw_footer(fb, stride, "UP/DN PGUP/PGDN  ENTER  ESC back");
}

static void render_slot_action_page(uint8_t *fb, int stride) {
    char title[48];
    snprintf(title, sizeof(title), " %s ", TARGET_LABELS[s_target]);
    draw_chrome(fb, stride, title);

    int x = content_x(), y = content_y();
    int cw = content_w();
    int max_chars = (cw - 4) / UI_CHAR_W;

    /* Show the currently mounted filename, or "(empty)". */
    const char *mounted = msx_mounted_name((msx_target_t)s_target);
    char line[MSX_MAX_FILENAME_LEN + 32];
    if (mounted) {
        const char *slash = strrchr(mounted, '/');
        snprintf(line, sizeof(line), "Loaded: %s", slash ? slash + 1 : mounted);
    } else {
        snprintf(line, sizeof(line), "Loaded: (empty)");
    }
    ui_draw_string_truncated(fb, stride, x, y, line, max_chars, UI_COLOR_FG);
    y += UI_LINE_H + 4;

    /* Build and render the dynamic action list. */
    slot_act_row_t rows[MAX_SLOT_ACTS];
    int n = build_slot_actions(rows);
    if (s_slot_action >= n) s_slot_action = n - 1;
    if (s_slot_action < 0)  s_slot_action = 0;

    for (int i = 0; i < n; ++i) {
        ui_draw_menu_item(fb, stride, x, y, cw,
                          rows[i].label, max_chars, i == s_slot_action);
        y += UI_LINE_H + 1;
    }

    draw_footer(fb, stride, "UP/DN  ENTER  ESC back");
}

static void render_settings_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Settings ");

    int x = content_x(), y = content_y();
    int cw = content_w();

    /* Scrolled window: draw SETTINGS_VISIBLE_ROWS rows starting at
     * s_settings_scroll. Each row is either a setting, an "Apply"
     * action row, or a "Back" action row. */
    int last = s_settings_scroll + SETTINGS_VISIBLE_ROWS;
    if (last > SETTINGS_TOTAL_ROWS) last = SETTINGS_TOTAL_ROWS;

    for (int i = s_settings_scroll; i < last; ++i) {
        bool sel = (i == s_setting_row);
        uint8_t bg = sel ? UI_COLOR_ACCENT : UI_COLOR_BG;
        uint8_t fg = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

        if (i < MSX_SETTING_COUNT) {
            ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);
            ui_draw_string(fb, stride, x + 2, y + 1,
                           msx_settings_label((msx_setting_id_t)i), fg);

            /* Value, right-aligned with chevrons hinting L/R cycles. */
            const char *val = msx_settings_value_label((msx_setting_id_t)i);
            int vlen = (int)strlen(val);
            int vx = x + cw - 4 - (vlen + 2) * UI_CHAR_W;
            if (sel) ui_draw_string(fb, stride, vx - UI_CHAR_W, y + 1, "<", fg);
            ui_draw_string(fb, stride, vx, y + 1, val, fg);
            if (sel) ui_draw_string(fb, stride, vx + vlen * UI_CHAR_W + 2, y + 1, ">", fg);
        } else if (i == SETTINGS_APPLY_ROW) {
            ui_draw_menu_item(fb, stride, x, y, cw,
                              "Apply and Reset MSX",
                              (cw - 4) / UI_CHAR_W, sel);
        } else if (i == SETTINGS_BACK_ROW) {
            ui_draw_menu_item(fb, stride, x, y, cw,
                              "Back to MSX",
                              (cw - 4) / UI_CHAR_W, sel);
        }
        y += UI_LINE_H + 1;
    }

    /* Scrollbar on the right when the list overflows. Minus one so it
     * doesn't touch the row of dashes above the footer. */
    if (SETTINGS_TOTAL_ROWS > SETTINGS_VISIBLE_ROWS) {
        ui_draw_scrollbar(fb, stride,
                          WIN_X + WIN_W - WIN_PAD - 4,
                          content_y(),
                          SETTINGS_VISIBLE_ROWS * (UI_LINE_H + 1) - 1,
                          SETTINGS_TOTAL_ROWS,
                          SETTINGS_VISIBLE_ROWS,
                          s_settings_scroll);
    }

    draw_footer(fb, stride, "UP/DN PG  LEFT/RIGHT  ENTER  ESC");
}

static void render_settings_confirm(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Reset required ");
    int x = content_x(), y = content_y();
    /* Keep each line under 40 chars so it fits in the content area
     * (256px window - 2*6px padding = 244px, 40 glyphs at 6px each). */
    ui_draw_string(fb, stride, x, y,
        "New settings will reset the MSX.",    UI_COLOR_FG);
    y += UI_LINE_H + 2;
    ui_draw_string(fb, stride, x, y,
        "All unsaved program state is lost.",  UI_COLOR_FG);
    y += UI_LINE_H + 6;
    ui_draw_string(fb, stride, x, y, "Continue?", UI_COLOR_FG);
    draw_footer(fb, stride, "ENTER confirm  ESC cancel");
}

static void render_mapper_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " MegaROM mapper ");
    int x = content_x(), y = content_y();
    int cw = content_w();
    int max_chars = (cw - 4) / UI_CHAR_W;

    ui_draw_string(fb, stride, x, y,
        "Pick a mapper (Auto works for most).", UI_COLOR_FG);
    y += UI_LINE_H + 4;

    for (int i = 0; i < MAPPER_CHOICE_COUNT; ++i) {
        ui_draw_menu_item(fb, stride, x, y, cw,
                          MAPPER_CHOICES[i].label, max_chars,
                          i == s_mapper_row);
        y += UI_LINE_H + 1;
    }
    draw_footer(fb, stride, "UP/DN  ENTER  ESC back");
}

static void render_message_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Notice ");
    int x = content_x(), y = content_y() + 10;
    ui_draw_string_truncated(fb, stride, x, y, s_msg,
                             (content_w() - 4) / UI_CHAR_W, UI_COLOR_FG);
    draw_footer(fb, stride, "ENTER / ESC to continue");
}

void msx_ui_render(uint8_t *fb, int stride, int height) {
    (void)height;
    if (s_state == UI_HIDDEN) return;
    /* Always redraw when the UI is visible so that any MSX-side pixels
     * that might have leaked into the back buffer are overwritten. */
    switch (s_state) {
        case UI_SELECT_TARGET:    render_target_page(fb, stride);      break;
        case UI_SLOT_ACTION:      render_slot_action_page(fb, stride); break;
        case UI_SELECT_FILE:      render_file_page(fb, stride);        break;
        case UI_SELECT_MAPPER:    render_mapper_page(fb, stride);      break;
        case UI_STATES:           render_states_page(fb, stride);      break;
        case UI_STATES_CONFIRM_DELETE:
                                  render_states_confirm_delete(fb, stride); break;
        case UI_SETTINGS:         render_settings_page(fb, stride);    break;
        case UI_SETTINGS_CONFIRM: render_settings_confirm(fb, stride); break;
        case UI_BUSY:             draw_chrome(fb, stride, " Working... "); break;
        case UI_MESSAGE:          render_message_page(fb, stride);     break;
        default: break;
    }
    s_dirty = false;
}
