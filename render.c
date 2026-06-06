#include <string.h>

#include "pd_api.h"
#include "vecx.h"
#include "render.h"

#define PERSISTENCE_FRAMES 3
#define PERSISTENCE_VECTOR_CAP 512
/* 0 = draw only the current (now-complete) frame: crisp, no trails/doubling.
 * 1 = phosphor-glow trails (looks doubled with slow-mo emulation at 50Hz render).
 */
#define RENDER_PERSISTENCE 0
#define VECTOR_LINE_WIDTH 1
#define DIRECT_FRAMEBUFFER_RENDER 0

/* Supersampling: rasterise vectors into an SS-times-resolution 1-bit coverage
 * buffer, then downscale to the screen (any covered sub-pixel -> black pixel).
 * This stops sub-pixel Vectrex font segments from dropping out, keeping small
 * text legible at full-screen. 1 = off (draw straight to the screen).
 */
#define SUPERSAMPLE 1

static PlaydateAPI* pd;
static int screen_w = LCD_COLUMNS;
static int screen_h = LCD_ROWS;
static int scale_factor = 1;
static int offset_x = 0;
static int offset_y = 0;

static vector_t persistence_vectors[PERSISTENCE_FRAMES][PERSISTENCE_VECTOR_CAP];
static long persistence_counts[PERSISTENCE_FRAMES];
static int persistence_next_frame;
static int persistence_frame_count;

#if DIRECT_FRAMEBUFFER_RENDER
static uint8_t* frame_buffer;

static int int_abs(int value)
{
	return value < 0 ? -value : value;
}

static void clear_framebuffer_white(void)
{
	if (frame_buffer == NULL)
		frame_buffer = pd->graphics->getFrame();

	memset(frame_buffer, 0xff, LCD_ROWSIZE * LCD_ROWS);
}

static void draw_framebuffer_pixel(int x, int y)
{
	uint8_t* row;

	if ((unsigned)x >= (unsigned)screen_w || (unsigned)y >= (unsigned)screen_h)
		return;

	row = frame_buffer + y * LCD_ROWSIZE;
	row[x >> 3] &= (uint8_t)~(0x80U >> (x & 7));
}

static void draw_framebuffer_line(int x0, int y0, int x1, int y1)
{
	int dx = int_abs(x1 - x0);
	int sx = x0 < x1 ? 1 : -1;
	int dy = -int_abs(y1 - y0);
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		int e2;

		draw_framebuffer_pixel(x0, y0);
		if (x0 == x1 && y0 == y1)
			break;

		e2 = err << 1;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}
#endif

/* current zoom: 100 = fit the whole Vectrex screen (smallest text); higher zooms
 * in (bigger, more legible text) but crops the outer edges (clipped off-screen).
 * Driven by the crank (see update_zoom_from_crank); docked crank leaves it at 100.
 */
#define RENDER_ZOOM_MIN 100
#define RENDER_ZOOM_MAX 500
static int zoom_pct = RENDER_ZOOM_MIN;

static void update_scaling(void)
{
	/* DIRECT orientation (no transpose): Vectrex X -> screen X, Y -> screen Y,
	 * so the image (and text) is upright like the real Vectrex.
	 */
	int scale_x = (ALG_MAX_X + screen_w - 1) / screen_w;
	int scale_y = (ALG_MAX_Y + screen_h - 1) / screen_h;
	int content_w;
	int content_h;

	/* fit scale (whole screen visible), then divide by the zoom factor */
	scale_factor = scale_x > scale_y ? scale_x : scale_y;
	scale_factor = (scale_factor * 100 + zoom_pct - 1) / zoom_pct;
	if (scale_factor < 1)
		scale_factor = 1;

	content_w = ALG_MAX_X / scale_factor;
	content_h = ALG_MAX_Y / scale_factor;
	offset_x = (screen_w - content_w) / 2;
	offset_y = (screen_h - content_h) / 2;
}

#if SUPERSAMPLE > 1
#define SS SUPERSAMPLE
#define SS_W (LCD_COLUMNS * SS)
#define SS_H (LCD_ROWS * SS)
static uint8_t ss_buf[(SS_W * SS_H + 7) / 8];

static void ss_setpix(int x, int y)
{
	long bit;

	if ((unsigned)x >= (unsigned)SS_W || (unsigned)y >= (unsigned)SS_H)
		return;

	bit = (long)y * SS_W + x;
	ss_buf[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
}

static void ss_line(int x0, int y0, int x1, int y1)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0; /* = -abs(y1-y0) */
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		int e2;

		ss_setpix(x0, y0);
		if (x0 == x1 && y0 == y1)
			break;

		e2 = err << 1;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

/* collapse the SSxSS coverage buffer to the 1-bit screen: any covered sub-pixel
 * makes the screen pixel black. */
static void ss_downscale(void)
{
	uint8_t *frame = pd->graphics->getFrame();
	int sx, sy;

	memset(frame, 0xff, LCD_ROWSIZE * LCD_ROWS);

	for (sy = 0; sy < LCD_ROWS; sy++) {
		uint8_t *row = frame + sy * LCD_ROWSIZE;

		for (sx = 0; sx < LCD_COLUMNS; sx++) {
			int covered = 0;
			int i, j;

			for (j = 0; j < SS && !covered; j++) {
				long base = (long)(sy * SS + j) * SS_W + sx * SS;

				for (i = 0; i < SS; i++) {
					long bit = base + i;
					if (ss_buf[bit >> 3] & (0x80u >> (bit & 7))) {
						covered = 1;
						break;
					}
				}
			}

			if (covered)
				row[sx >> 3] &= (uint8_t)~(0x80u >> (sx & 7));
		}
	}

	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}
#endif

static uint32_t draw_vector_list(const vector_t* vectors, long count)
{
	uint32_t drawn_vectors = 0;
	long i;

	for (i = 0; i < count; i++) {
		const vector_t* v = &vectors[i];
		int x0;
		int y0;
		int x1;
		int y1;

		if (v->color == 0 || v->color >= VECTREX_COLORS)
			continue;

		/* direct mapping (no flips) = the both-flipped image rotated 180 degrees:
		 * upright and reads correctly, matching a real Vectrex / the ESPboy port. */
#if SUPERSAMPLE > 1
		x0 = SS * offset_x + (int)(SS * v->x0 / scale_factor);
		y0 = SS * offset_y + (int)(SS * v->y0 / scale_factor);
		x1 = SS * offset_x + (int)(SS * v->x1 / scale_factor);
		y1 = SS * offset_y + (int)(SS * v->y1 / scale_factor);
		ss_line(x0, y0, x1, y1);
#else
		x0 = offset_x + (int)(v->x0 / scale_factor);
		y0 = offset_y + (int)(v->y0 / scale_factor);
		x1 = offset_x + (int)(v->x1 / scale_factor);
		y1 = offset_y + (int)(v->y1 / scale_factor);
		pd->graphics->drawLine(x0, y0, x1, y1, VECTOR_LINE_WIDTH, kColorBlack);
#endif
		drawn_vectors++;
	}

	return drawn_vectors;
}

static uint32_t draw_persistence_frames(void)
{
	uint32_t drawn_vectors = 0;
	int age;

	for (age = persistence_frame_count; age > 0; age--) {
		int frame = persistence_next_frame - age;

		if (frame < 0)
			frame += PERSISTENCE_FRAMES;

		drawn_vectors += draw_vector_list(persistence_vectors[frame], persistence_counts[frame]);
	}

	return drawn_vectors;
}

static void save_persistence_frame(void)
{
	long count = vector_draw_cnt;

	if (count > PERSISTENCE_VECTOR_CAP)
		count = PERSISTENCE_VECTOR_CAP;

	if (count > 0)
		memcpy(persistence_vectors[persistence_next_frame], vectors_draw, (size_t)count * sizeof(vector_t));

	persistence_counts[persistence_next_frame] = count;
	persistence_next_frame++;
	if (persistence_next_frame >= PERSISTENCE_FRAMES)
		persistence_next_frame = 0;
	if (persistence_frame_count < PERSISTENCE_FRAMES)
		persistence_frame_count++;
}

void render_init(PlaydateAPI *playdate, int screen_width, int screen_height)
{
	pd = playdate;
	screen_w = screen_width;
	screen_h = screen_height;
	update_scaling();
}

/* diagnostic: histogram the on-screen pixel length of the current vectors so we
 * can see whether strokes are sub-pixel dots or real lines. */
static void log_vector_lengths(void)
{
	int b0 = 0, b1 = 0, b2 = 0, b35 = 0, b6 = 0;
	long i;

	for (i = 0; i < vector_draw_cnt; i++) {
		const vector_t* v = &vectors_draw[i];
		int dx, dy, len;

		if (v->color == 0 || v->color >= VECTREX_COLORS)
			continue;

		dx = (int)((v->x0 - v->x1) / scale_factor);
		dy = (int)((v->y0 - v->y1) / scale_factor);
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		len = dx > dy ? dx : dy;

		if (len == 0) b0++;
		else if (len == 1) b1++;
		else if (len == 2) b2++;
		else if (len <= 5) b35++;
		else b6++;
	}

	pd->system->logToConsole(
		"vecx render: scale=%d vec_lens 0px=%d 1px=%d 2px=%d 3-5px=%d 6+px=%d (cnt=%ld)",
		scale_factor, b0, b1, b2, b35, b6, vector_draw_cnt);
}

/* crank zooms: forward (clockwise) zooms in, back zooms out. Docked = no change.
 * ~0.5% per degree, so roughly one full turn spans the whole zoom range.
 */
static void update_zoom_from_crank(void)
{
	float change;
	int z;

	if (pd->system->isCrankDocked())
		return;

	change = pd->system->getCrankChange();
	if (change == 0.0f)
		return;

	z = zoom_pct + (int)(change * 0.5f);
	if (z < RENDER_ZOOM_MIN)
		z = RENDER_ZOOM_MIN;
	if (z > RENDER_ZOOM_MAX)
		z = RENDER_ZOOM_MAX;

	if (z != zoom_pct) {
		zoom_pct = z;
		update_scaling();
	}
}

uint32_t render_draw_frame(void)
{
	uint32_t drawn_vectors = 0;
	static unsigned diag_phase;

	update_zoom_from_crank();

	if (++diag_phase >= 300u) {
		diag_phase = 0;
		log_vector_lengths();
	}

#if SUPERSAMPLE > 1
	memset(ss_buf, 0, sizeof(ss_buf));
#elif DIRECT_FRAMEBUFFER_RENDER
	clear_framebuffer_white();
#else
	pd->graphics->clear(kColorWhite);
#endif

#if RENDER_PERSISTENCE
	drawn_vectors += draw_persistence_frames();
#endif
	drawn_vectors += draw_vector_list(vectors_draw, vector_draw_cnt);
#if RENDER_PERSISTENCE
	save_persistence_frame();
#endif

#if SUPERSAMPLE > 1
	ss_downscale();
#elif DIRECT_FRAMEBUFFER_RENDER
	pd->graphics->markUpdatedRows(0, screen_h - 1);
#endif

	return drawn_vectors;
}
