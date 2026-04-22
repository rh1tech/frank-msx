/*
 * msx_ui.c — loader overlay state machine + renderer.
 */

#include "msx_ui.h"
#include "msx_loader.h"
#include "msx_settings.h"
#include "ui_draw.h"
#include "board_config.h"

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
    UI_SELECT_TARGET,
    UI_SELECT_FILE,
    UI_SELECT_ACTION,
    UI_SETTINGS,             /* edit settings, last row is Apply */
    UI_SETTINGS_CONFIRM,     /* "reset required — continue?" dialog */
    UI_BUSY,
    UI_MESSAGE,
} ui_state_t;

static volatile ui_state_t s_state = UI_HIDDEN;
static int  s_target   = 0;   /* index into TARGET_LABELS */
static int  s_file     = 0;   /* highlight in file list */
static int  s_scroll   = 0;
static int  s_action   = 0;   /* 0 Mount / 1 Mount+Reset / 2 Cancel */
static int  s_setting_row = 0; /* 0..MSX_SETTING_COUNT = Apply row   */
static bool s_dirty    = true;
static char s_msg[96];

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
            s_state = UI_SELECT_FILE;
            s_file = 0; s_scroll = 0;
            s_dirty = true; return true;
        case XK_Escape:
            msx_ui_hide(); return true;
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
            s_state = UI_SELECT_TARGET;
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
                snprintf(s_msg, sizeof(s_msg), "Wrong file type for %s", TARGET_LABELS[s_target]);
                s_state = UI_MESSAGE;
                s_dirty = true; return true;
            }
            s_state  = UI_SELECT_ACTION;
            s_action = 0;
            s_dirty  = true;
            return true;
        }
    }
    return false;
}

static void perform_mount(bool reset_after) {
    bool has_parent = strcmp(msx_current_dir, "/") != 0;
    int e = s_file - (has_parent ? 1 : 0);
    s_state = UI_BUSY;
    s_dirty = true;

    int rc = msx_mount_entry(e, (msx_target_t)s_target, reset_after);
    if (rc == 0) {
        msx_ui_hide();
    } else {
        snprintf(s_msg, sizeof(s_msg), "Mount failed (code %d)", rc);
        s_state = UI_MESSAGE;
        s_dirty = true;
    }
}

static bool handle_action_page(unsigned int xk) {
    switch (xk) {
        case XK_Up:
            if (s_action > 0) --s_action; else s_action = 2;
            s_dirty = true; return true;
        case XK_Down:
            if (++s_action > 2) s_action = 0;
            s_dirty = true; return true;
        case XK_Escape:
            s_state = UI_SELECT_FILE;
            s_dirty = true; return true;
        case XK_Return:
            if (s_action == 0)       perform_mount(false);
            else if (s_action == 1)  perform_mount(true);
            else                      { s_state = UI_SELECT_FILE; s_dirty = true; }
            return true;
    }
    return false;
}

bool msx_ui_handle_key(unsigned int xk) {
    if (xk == XK_F11) { msx_ui_toggle(); return true; }
    if (s_state == UI_HIDDEN) return false;
    if (s_state == UI_MESSAGE) {
        if (xk == XK_Return || xk == XK_Escape) {
            s_state = UI_SELECT_FILE;
            s_dirty = true;
        }
        return true;
    }
    if (s_state == UI_BUSY) return true;
    if (s_state == UI_SELECT_TARGET)      return handle_target_page(xk);
    if (s_state == UI_SELECT_FILE)        return handle_file_page(xk);
    if (s_state == UI_SELECT_ACTION)      return handle_action_page(xk);
    if (s_state == UI_SETTINGS)           return handle_settings_page(xk);
    if (s_state == UI_SETTINGS_CONFIRM)   return handle_settings_confirm(xk);
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
    int y = content_y() + 6;
    int max_chars = (content_w() - 4) / UI_CHAR_W;
    for (int i = 0; i < MSX_TARGET_COUNT; ++i) {
        ui_draw_menu_item(fb, stride, content_x(), y, content_w(),
                          TARGET_LABELS[i], max_chars, i == s_target);
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

static void render_action_page(uint8_t *fb, int stride) {
    char title[48];
    snprintf(title, sizeof(title), " %s ", TARGET_LABELS[s_target]);
    draw_chrome(fb, stride, title);

    int x = content_x(), y = content_y();
    int max_chars = (content_w() - 20) / UI_CHAR_W;
    int max_chars_narrow = (content_w() - 4) / UI_CHAR_W;

    bool has_parent = strcmp(msx_current_dir, "/") != 0;
    int ei = s_file - (has_parent ? 1 : 0);
    char line[MSX_MAX_FILENAME_LEN + 16];
    snprintf(line, sizeof(line), "File: %s", msx_entries[ei].name);
    ui_draw_string_truncated(fb, stride, x, y, line, max_chars_narrow, UI_COLOR_FG);
    y += UI_LINE_H + 4;

    ui_draw_string(fb, stride, x, y, "Select action:", UI_COLOR_FG);
    y += UI_LINE_H + 4;

    ui_draw_menu_item(fb, stride, x + 10, y, content_w() - 20,
                      "Mount",              max_chars, s_action == 0);
    y += UI_LINE_H + 2;
    ui_draw_menu_item(fb, stride, x + 10, y, content_w() - 20,
                      "Mount and reset",    max_chars, s_action == 1);
    y += UI_LINE_H + 2;
    ui_draw_menu_item(fb, stride, x + 10, y, content_w() - 20,
                      "Cancel",             max_chars, s_action == 2);

    draw_footer(fb, stride, "UP/DN  ENTER confirm  ESC back");
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
        case UI_SELECT_TARGET:    render_target_page(fb, stride);     break;
        case UI_SELECT_FILE:      render_file_page(fb, stride);       break;
        case UI_SELECT_ACTION:    render_action_page(fb, stride);     break;
        case UI_SETTINGS:         render_settings_page(fb, stride);   break;
        case UI_SETTINGS_CONFIRM: render_settings_confirm(fb, stride); break;
        case UI_BUSY:             draw_chrome(fb, stride, " Working... "); break;
        case UI_MESSAGE:          render_message_page(fb, stride);    break;
        default: break;
    }
    s_dirty = false;
}
