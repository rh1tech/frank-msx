/*
 * msx_ui.h — overlay UI that lets the user choose a cartridge or disk
 *            image from the SD card and mount it into fMSX.
 *
 * The UI repaints fMSX's framebuffer directly (256×228 8bpp) and runs
 * purely event-driven: the main loop hands every PS/2 key down to
 * `msx_ui_handle_key()` while `msx_ui_is_visible()` is true, and skips
 * the normal MSX key matrix update in that case.
 *
 * Toggle with F11. The state machine mirrors murmapple's:
 *
 *   HIDDEN → SELECT_TARGET → SELECT_FILE → SELECT_ACTION
 *     (target = Cart A/B or Disk A/B; action = Mount / Mount+Reset / Cancel)
 */
#ifndef MSX_UI_H
#define MSX_UI_H

#include <stdint.h>
#include <stdbool.h>

/* Call once during InitMachine() to install the UI palette slots. */
void msx_ui_init(void);

/* True while the overlay is active — the main emulation loop checks
 * this and pauses fMSX refresh while true. */
bool msx_ui_is_visible(void);

/* F11 toggles the cartridge/disk loader; F12 toggles the Settings
 * dialog. Both are the same overlay but open on different pages. */
void msx_ui_toggle(void);
void msx_ui_toggle_settings(void);
void msx_ui_show(void);
void msx_ui_show_settings(void);
void msx_ui_hide(void);

/* Non-blocking keyboard event — returns true if consumed. Receives
 * the XK_*-style virtual key (fMSX uses XK_* in HandleKeys(); we map
 * the UI's own very small set: F11, Escape, Enter, Up, Down,
 * PageUp, PageDown). */
bool msx_ui_handle_key(unsigned int xk);

/* Paints the overlay onto the supplied back buffer. Called from
 * PutImage()'s back-half whenever the UI is visible. */
void msx_ui_render(uint8_t *back_buffer, int stride, int height);

#endif /* MSX_UI_H */
