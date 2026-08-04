#ifndef RENDER_H
#define RENDER_H

#include "pd_api.h"

/* Vector renderer (Vectrex vectors -> Playdate lines).
 *
 * Deliberately its OWN translation unit, LINKED LAST (see Makefile SRC order).
 * The CPU hot path's performance depends on the whole-binary I-cache packing of
 * e6809.o/vecx.o; objects linked AFTER them don't shift them, so iterating on
 * the renderer here cannot reshuffle that packing and disturb the FPS. Keep all
 * rendering/scaling code in this file. See PLAYDATE_ITCM_GUIDE.md
 * ("whole-binary I-cache packing is load-bearing").
 */
void render_init(PlaydateAPI *playdate, int screen_width, int screen_height);

/* (re)add the renderer's Rotation + Frameskip system-menu items, preserving the
 * current selections. Called after the caller adds its first menu item, so the
 * 3-item menu can be rebuilt while keeping a stable order. */
void render_refresh_menu(void);

/* clear and draw the current frame's vectors (with the menu-selected rotation and
 * FIT scaling); returns the number of vectors drawn (for the benchmark).
 */
uint32_t render_draw_frame(void);

/* menu-selected frameskip: 0 = render every frame, N = skip N frames between
 * renders, and -1 = "Auto" -- the host decides per frame (it skips only while
 * the update loop is missing the 50 Hz deadline; see osint_render). */
int render_frame_skip(void);

/* menu-selected screen rotation in degrees: -90, 0 or 90. Input mapping uses it
 * to counter-rotate the d-pad so directions stay screen-relative. */
int render_rotation(void);

/* Persisted settings, as raw menu indices (rotation 0..2 = -90/0/90; frameskip
 * 0..2 = Auto/0/1). The host writes these to the data folder and restores them
 * at boot. Setters are safe to call before render_refresh_menu(). */
int render_get_rotation_index(void);
void render_set_rotation_index(int index);
int render_get_frameskip_index(void);
void render_set_frameskip_index(int index);

/* Called whenever one of the renderer's own menu items changes, so the host can
 * persist the new value. Register before the menu is built. */
void render_set_on_settings_changed(void (*callback)(void));

#endif /* RENDER_H */
