#include <string.h>

#include "pd_api.h"
#include "vecx.h"
#include "render.h"

/* Phosphor-glow trails by RE-DRAWING old frames. Superseded by PHOSPHOR_DECAY
 * (cheaper, no doubling); kept off. */
#define RENDER_PERSISTENCE 0
#define PERSISTENCE_FRAMES 3
#define PERSISTENCE_VECTOR_CAP 512
#define VECTOR_LINE_WIDTH 1

/* Phosphor decay: instead of clearing, fade old (black) pixels toward white with
 * an alternating dither, so un-redrawn vectors vanish over (number of masks)
 * frames -- faint in between. Emulates the Vectrex phosphor and hides the object-
 * multiplex flicker, at ~no cost (one framebuffer OR pass replacing the clear,
 * no re-drawing). Longer mask table = slower decay = longer glow. */
#define PHOSPHOR_DECAY 1

static PlaydateAPI* pd;
static int screen_w = LCD_COLUMNS;
static int screen_h = LCD_ROWS;
static int scale_factor = 1;
static int offset_x = 0;
static int offset_y = 0;

#if PHOSPHOR_DECAY
/* fade dither masks, cycled per frame; their OR covers every bit, so old pixels
 * fully fade over (table length) frames. 2 entries ~= a 2-frame phosphor glow. */
static const uint8_t phosphor_masks[] = { 0xAAu, 0x55u };
static unsigned phosphor_phase;
#endif

#if RENDER_PERSISTENCE
static vector_t persistence_vectors[PERSISTENCE_FRAMES][PERSISTENCE_VECTOR_CAP];
static long persistence_counts[PERSISTENCE_FRAMES];
static int persistence_next_frame;
static int persistence_frame_count;
#endif

/* screen rotation, chosen via the system menu (see render_init):
 *   ROT_LEFT  (-90) = rotated 90 degrees counter-clockwise
 *   ROT_NONE  (  0) = Vectrex upright (default)
 *   ROT_RIGHT (+90) = rotated 90 degrees clockwise
 * Always FIT: scaled to fill the limiting screen dimension while keeping the
 * Vectrex aspect ratio (bars on the other dimension, no cropping).
 */
enum { ROT_LEFT = 0, ROT_NONE = 1, ROT_RIGHT = 2 };
static int rotation = ROT_NONE;

static void update_scaling(void)
{
	/* when rotated 90 degrees the content is transposed on screen */
	int rotated = (rotation != ROT_NONE);
	int alg_w = rotated ? ALG_MAX_Y : ALG_MAX_X;
	int alg_h = rotated ? ALG_MAX_X : ALG_MAX_Y;
	int scale_x = (alg_w + screen_w - 1) / screen_w;
	int scale_y = (alg_h + screen_h - 1) / screen_h;
	int content_w;
	int content_h;

	/* FIT: larger scale = whole image visible (fills one axis, bars on the other) */
	scale_factor = scale_x > scale_y ? scale_x : scale_y;
	if (scale_factor < 1)
		scale_factor = 1;

	content_w = alg_w / scale_factor;
	content_h = alg_h / scale_factor;
	offset_x = (screen_w - content_w) / 2;
	offset_y = (screen_h - content_h) / 2;
}

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

		if (rotation == ROT_LEFT) {
			/* -90 (counter-clockwise): Y -> screen X, X -> screen Y (flipped) */
			x0 = offset_x + (int)(v->y0 / scale_factor);
			y0 = offset_y + (int)((ALG_MAX_X - v->x0) / scale_factor);
			x1 = offset_x + (int)(v->y1 / scale_factor);
			y1 = offset_y + (int)((ALG_MAX_X - v->x1) / scale_factor);
		} else if (rotation == ROT_RIGHT) {
			/* +90 (clockwise): Y -> screen X (flipped), X -> screen Y */
			x0 = offset_x + (int)((ALG_MAX_Y - v->y0) / scale_factor);
			y0 = offset_y + (int)(v->x0 / scale_factor);
			x1 = offset_x + (int)((ALG_MAX_Y - v->y1) / scale_factor);
			y1 = offset_y + (int)(v->x1 / scale_factor);
		} else {
			/* upright: direct mapping, matches a real Vectrex / the ESPboy port */
			x0 = offset_x + (int)(v->x0 / scale_factor);
			y0 = offset_y + (int)(v->y0 / scale_factor);
			x1 = offset_x + (int)(v->x1 / scale_factor);
			y1 = offset_y + (int)(v->y1 / scale_factor);
		}

		pd->graphics->drawLine(x0, y0, x1, y1, VECTOR_LINE_WIDTH, kColorBlack);
		drawn_vectors++;
	}

	return drawn_vectors;
}

#if RENDER_PERSISTENCE
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
#endif

static PDMenuItem *rotation_item;
static const char *rotation_options[] = { "-90", "0", "90" };

static void rotation_menu_callback(void *userdata)
{
	(void)userdata;
	rotation = pd->system->getMenuItemValue(rotation_item);
	update_scaling();
}

/* render every (frame_skip + 1)th frame; the rest are skipped. Selected via menu. */
static PDMenuItem *frameskip_item;
static const char *frameskip_options[] = { "0", "1" };
static int frame_skip;

static void frameskip_menu_callback(void *userdata)
{
	(void)userdata;
	frame_skip = pd->system->getMenuItemValue(frameskip_item);
}

int render_frame_skip(void)
{
	return frame_skip;
}

void render_init(PlaydateAPI *playdate, int screen_width, int screen_height)
{
	pd = playdate;
	screen_w = screen_width;
	screen_h = screen_height;

	rotation_item = pd->system->addOptionsMenuItem(
		"Rotation", rotation_options,
		(int)(sizeof(rotation_options) / sizeof(rotation_options[0])),
		rotation_menu_callback, NULL);
	pd->system->setMenuItemValue(rotation_item, ROT_NONE); /* default 0 degrees */

	frameskip_item = pd->system->addOptionsMenuItem(
		"Frameskip", frameskip_options,
		(int)(sizeof(frameskip_options) / sizeof(frameskip_options[0])),
		frameskip_menu_callback, NULL); /* default index 0 = no frameskip */

	update_scaling();
}

uint32_t render_draw_frame(void)
{
	uint32_t drawn_vectors = 0;

#if PHOSPHOR_DECAY
	{
		uint8_t *frame = pd->graphics->getFrame();
		uint8_t fade = phosphor_masks[phosphor_phase++ %
			(sizeof(phosphor_masks) / sizeof(phosphor_masks[0]))];
		int total = LCD_ROWSIZE * LCD_ROWS;
		int i;

		/* fade old black pixels toward white (1 = white); vectors re-drawn below
		 * stay solid, un-redrawn ones dither out over the mask cycle. */
		for (i = 0; i < total; i++)
			frame[i] |= fade;
	}
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

#if PHOSPHOR_DECAY
	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
#endif

	return drawn_vectors;
}
