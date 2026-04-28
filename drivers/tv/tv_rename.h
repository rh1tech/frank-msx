/*
 * Symbol-rename shim for the murmnes TV driver.
 *
 * The murmnes TV driver (drivers/tv/tv-software.c + graphics.c) exports
 * public symbols named graphics_init / graphics_set_palette /
 * graphics_set_mode / graphics_set_buffer / draw_text / clrScr /
 * graphics_get_video_mode — all of which either collide with, or have
 * a different signature than, frank-msx's own video API in HDMI.h.
 *
 * We keep the murmnes source 100% verbatim by textually renaming each
 * public entry point to a tv_* prefix at compile time. The shim in
 * HDMI_tv.c then wraps the tv_* symbols behind the HDMI.h API.
 *
 * Include this header as a `-include` compile flag for the TV TUs and
 * as a normal #include in HDMI_tv.c.
 */

#ifndef TV_RENAME_H_
#define TV_RENAME_H_

#define graphics_init           tv_graphics_init
#define graphics_set_palette    tv_graphics_set_palette
#define graphics_set_mode       tv_graphics_set_mode
#define graphics_set_buffer     tv_graphics_set_buffer
#define graphics_set_offset     tv_graphics_set_offset
#define graphics_set_textbuffer tv_graphics_set_textbuffer
#define graphics_set_flashmode  tv_graphics_set_flashmode
#define graphics_set_bgcolor    tv_graphics_set_bgcolor
#define graphics_set_modeTV     tv_graphics_set_modeTV
#define graphics_get_video_mode tv_graphics_get_video_mode
#define graphics_get_buffer     tv_graphics_get_buffer
#define graphics_get_width      tv_graphics_get_width
#define graphics_get_height     tv_graphics_get_height
#define graphics_get_palette    tv_graphics_get_palette
#define graphics_set_res        tv_graphics_set_res
#define graphics_set_shift      tv_graphics_set_shift
#define graphics_set_crt_active tv_graphics_set_crt_active
#define graphics_get_crt_active tv_graphics_get_crt_active
#define graphics_set_greyscale  tv_graphics_set_greyscale
#define graphics_get_greyscale  tv_graphics_get_greyscale
#define graphics_restore_sync_colors tv_graphics_restore_sync_colors
#define graphics_init_hdmi      tv_graphics_init_hdmi
#define graphics_set_palette_hdmi tv_graphics_set_palette_hdmi
#define graphics_set_bgcolor_hdmi tv_graphics_set_bgcolor_hdmi
#define startVIDEO              tv_startVIDEO
#define set_palette             tv_set_palette
#define draw_text               tv_draw_text
#define draw_window             tv_draw_window
#define clrScr                  tv_clrScr

#endif /* TV_RENAME_H_ */
