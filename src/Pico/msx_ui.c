/*
 * msx_ui.c — loader overlay state machine + renderer.
 */

#include "msx_ui.h"
#include "msx_loader.h"
#include "msx_settings.h"
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

/* Layout inside the 256×228 MSX framebuffer. Use the full 256 px of
 * screen width so long labels and footers don't run out of room.
 * Height leaves a thin margin top and bottom for visual breathing. */
#define WIN_X      0
#define WIN_Y      12
#define WIN_W      256
#define WIN_H      200
#define WIN_PAD    6
#define MAX_ROWS   14   /* visible list rows */

/* Targets shown on the first page. Must match msx_target_t order. */
static const char *TARGET_LABELS[MSX_TARGET_COUNT] = {
    "Cartridge A",
    "Cartridge B",
    "Disk A",
    "Disk B",
};

typedef enum {
    UI_HIDDEN = 0,
    UI_SELECT_TARGET,        /* pick Cart A/B, Disk A/B */
    UI_SLOT_ACTION,          /* context menu for the chosen slot/drive */
    UI_SELECT_FILE,          /* browse SD to pick a ROM/DSK to mount  */
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
static bool s_dirty       = true;
static char s_msg[96];

/* Which "mount" behaviour to apply when a file is picked. Cartridge
 * slots force a reset; floppy drives don't. Set when entering
 * UI_SELECT_FILE from UI_SLOT_ACTION. */
static bool s_mount_reset_after = false;

extern int InMenu;             /* fMSX flag — silences input */

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
    s_setting_row = 0;
    s_state  = UI_SETTINGS;
    s_dirty  = true;
    InMenu   = 1;
}

void msx_ui_hide(void) {
    s_state = UI_HIDDEN;
    InMenu  = 0;
    /* Invalidate keyboard state so fMSX doesn't see phantom keys. */
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
    return false;
}

/* ---- key handling ---------------------------------------------- */

static bool handle_target_page(unsigned int xk) {
    switch (xk) {
        case XK_Up:
            if (s_target > 0) --s_target; else s_target = MSX_TARGET_COUNT - 1;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_target >= MSX_TARGET_COUNT) s_target = 0;
            s_dirty = true; return true;
        case XK_Return:
            s_state        = UI_SLOT_ACTION;
            s_slot_action  = 0;
            s_dirty        = true;
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
    SLOT_ACT_INSERT = 0,      /* cart: insert + reset, disk: insert    */
    SLOT_ACT_CHANGE,          /* same as INSERT but shown as "Change"   */
    SLOT_ACT_EJECT,
    SLOT_ACT_CREATE_BLANK,    /* disk only */
    SLOT_ACT_SAVE_DSK,        /* disk only */
    SLOT_ACT_SAVE_FDI,        /* disk only */
    SLOT_ACT_CANCEL,
} slot_act_kind_t;

typedef struct {
    slot_act_kind_t kind;
    const char     *label;
} slot_act_row_t;

#define MAX_SLOT_ACTS 7

static int build_slot_actions(slot_act_row_t out[MAX_SLOT_ACTS]) {
    msx_target_t t = (msx_target_t)s_target;
    bool is_disk   = (t == MSX_TARGET_DISK_A || t == MSX_TARGET_DISK_B);
    bool loaded    = msx_mounted_name(t) != NULL;
    int n = 0;

    if (loaded) {
        out[n++] = (slot_act_row_t){ SLOT_ACT_CHANGE, is_disk ? "Change disk" : "Change cartridge" };
        out[n++] = (slot_act_row_t){ SLOT_ACT_EJECT,  "Eject" };
        if (is_disk) {
            out[n++] = (slot_act_row_t){ SLOT_ACT_SAVE_DSK, "Save as .DSK" };
            out[n++] = (slot_act_row_t){ SLOT_ACT_SAVE_FDI, "Save as .FDI" };
        }
    } else {
        out[n++] = (slot_act_row_t){ SLOT_ACT_INSERT, is_disk ? "Insert disk" : "Insert cartridge" };
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

/* Row layout: one per setting, then Apply, then Cancel. All rows are
 * rendered at the same left indent so Apply/Cancel look like ordinary
 * menu items rather than floating buttons. */
#define SETTINGS_APPLY_ROW   (MSX_SETTING_COUNT)
#define SETTINGS_CANCEL_ROW  (MSX_SETTING_COUNT + 1)
#define SETTINGS_TOTAL_ROWS  (MSX_SETTING_COUNT + 2)

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
            s_dirty = true; return true;
        case XK_Down:
            if (++s_setting_row >= SETTINGS_TOTAL_ROWS) s_setting_row = 0;
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
            } else if (s_setting_row == SETTINGS_CANCEL_ROW) {
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

            /* Mount immediately. Cartridge = reset after, disk =
             * live swap. The slot-action page set s_mount_reset_after
             * when it picked "Insert cartridge" / "Insert disk". */
            s_state = UI_BUSY;
            s_dirty = true;
            msx_ui_show_busy(s_target < 2 ? "Loading cartridge..."
                                          : "Loading disk image...");
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
            s_state = UI_SLOT_ACTION;
            s_dirty = true;
        }
        return true;
    }
    if (s_state == UI_BUSY) return true;
    if (s_state == UI_SELECT_TARGET)    return handle_target_page(xk);
    if (s_state == UI_SLOT_ACTION)      return handle_slot_action_page(xk);
    if (s_state == UI_SELECT_FILE)      return handle_file_page(xk);
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
    int max_chars = (cw - 4) / UI_CHAR_W;

    for (int i = 0; i < MSX_TARGET_COUNT; ++i) {
        bool sel = (i == s_target);
        uint8_t bg = sel ? UI_COLOR_ACCENT    : UI_COLOR_BG;
        uint8_t fg = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

        /* Background + label on the left. */
        ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);
        ui_draw_string(fb, stride, x + 2, y + 1, TARGET_LABELS[i], fg);

        /* Currently mounted filename (if any) right-aligned in the row. */
        const char *mounted = msx_mounted_name((msx_target_t)i);
        const char *display = NULL;
        char buf[MSX_MAX_FILENAME_LEN + 2];
        if (mounted) {
            const char *slash = strrchr(mounted, '/');
            display = slash ? slash + 1 : mounted;
        } else {
            display = "(empty)";
        }
        int avail = cw - 2 - (int)strlen(TARGET_LABELS[i]) * UI_CHAR_W - 8;
        int max_right = avail > 0 ? avail / UI_CHAR_W : 0;
        int dlen = (int)strlen(display);
        if (max_right < 3) max_right = 3;
        int shown = (dlen <= max_right) ? dlen : max_right;
        int dx = x + cw - 4 - shown * UI_CHAR_W;
        /* Re-use the truncator so long names still make sense. */
        ui_draw_string_truncated(fb, stride, dx, y + 1, display, max_right, fg);
        (void)buf;

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

    /* Each row: label on the left, value on the right. When the row
     * is the selected one, the whole row is drawn inverted. */
    for (int i = 0; i < MSX_SETTING_COUNT; ++i) {
        bool sel = (i == s_setting_row);
        uint8_t bg = sel ? UI_COLOR_ACCENT : UI_COLOR_BG;
        uint8_t fg = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

        ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);
        ui_draw_string(fb, stride, x + 2, y + 1,
                       msx_settings_label((msx_setting_id_t)i), fg);

        /* Value, right-aligned with chevrons hinting that L/R cycles. */
        const char *val = msx_settings_value_label((msx_setting_id_t)i);
        int vlen = (int)strlen(val);
        int vx = x + cw - 4 - (vlen + 2) * UI_CHAR_W;
        if (sel) ui_draw_string(fb, stride, vx - UI_CHAR_W, y + 1, "<", fg);
        ui_draw_string(fb, stride, vx, y + 1, val, fg);
        if (sel) ui_draw_string(fb, stride, vx + vlen * UI_CHAR_W + 2, y + 1, ">", fg);

        y += UI_LINE_H + 1;
    }

    /* Apply / Cancel sit at the same left indent as the setting rows
     * above, so they read as menu items rather than dialog buttons. */
    y += 4;
    ui_draw_menu_item(fb, stride, x, y, cw,
                      "Apply and Reset MSX",
                      (cw - 4) / UI_CHAR_W,
                      s_setting_row == SETTINGS_APPLY_ROW);
    y += UI_LINE_H + 1;
    ui_draw_menu_item(fb, stride, x, y, cw,
                      "Cancel",
                      (cw - 4) / UI_CHAR_W,
                      s_setting_row == SETTINGS_CANCEL_ROW);

    draw_footer(fb, stride, "UP/DN  LEFT/RIGHT  ENTER  ESC");
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
        case UI_SETTINGS:         render_settings_page(fb, stride);    break;
        case UI_SETTINGS_CONFIRM: render_settings_confirm(fb, stride); break;
        case UI_BUSY:             draw_chrome(fb, stride, " Working... "); break;
        case UI_MESSAGE:          render_message_page(fb, stride);     break;
        default: break;
    }
    s_dirty = false;
}
