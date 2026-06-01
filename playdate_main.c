#include <stdint.h>
#include <string.h>

#include "pd_api.h"

#include "e8910.h"
#include "osint.h"
#include "vecx.h"

#define TARGET_FPS VECTREX_UPDATE_HZ
#define EMU_CYCLES_PER_UPDATE ((long)(VECTREX_MHZ / TARGET_FPS))
#define BENCHMARK_LOG_MS 5000U
#define BUILD_VARIANT "align32rs2"
#define BUILD_LABEL __DATE__ " " __TIME__ " " BUILD_VARIANT
#define PERSISTENCE_FRAMES 3
#define PERSISTENCE_VECTOR_CAP 512
#define VECTOR_LINE_WIDTH 1
#define SAMPLE_LOG_TOP 5
#define DIRECT_FRAMEBUFFER_RENDER 0
#define RENDER_FRAME_SKIP 2

static PlaydateAPI* pd;
static int screen_w = LCD_COLUMNS;
static int screen_h = LCD_ROWS;
static int scale_factor = 1;
static int offset_x = 0;
static int offset_y = 0;

static uint32_t bench_window_start_ms;
static uint32_t bench_update_count;
static uint32_t bench_render_count;
static uint32_t bench_skipped_frames;
static uint32_t bench_total_update_ms;
static uint32_t bench_total_render_ms;
static uint32_t bench_total_vectors;
static uint32_t bench_total_emu_cycles;
static uint32_t bench_total_emu_instructions;
static uint32_t bench_total_wait_skips;
static uint32_t bench_total_wait_skip_cycles;
static uint32_t bench_total_delay_skips;
static uint32_t bench_total_delay_skip_cycles;
static uint32_t bench_min_update_ms;
static uint32_t bench_max_update_ms;

static vector_t persistence_vectors[PERSISTENCE_FRAMES][PERSISTENCE_VECTOR_CAP];
static long persistence_counts[PERSISTENCE_FRAMES];
static int persistence_next_frame;
static int persistence_frame_count;
static unsigned render_skip_phase;

static int update(void* userdata);

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

static int read_exact_file(const char* path, unsigned char* dst, unsigned int len)
{
	SDFile* file = pd->file->open(path, kFileRead);
	unsigned int total = 0;

	if (file == NULL)
		return 0;

	while (total < len) {
		int got = pd->file->read(file, dst + total, len - total);
		if (got <= 0)
			break;
		total += (unsigned int)got;
	}

	pd->file->close(file);
	return total == len;
}

static unsigned int read_partial_file(const char* path, unsigned char* dst, unsigned int len)
{
	SDFile* file = pd->file->open(path, kFileRead);
	unsigned int total = 0;

	if (file == NULL)
		return 0;

	while (total < len) {
		int got = pd->file->read(file, dst + total, len - total);
		if (got <= 0)
			break;
		total += (unsigned int)got;
	}

	pd->file->close(file);
	return total;
}

static void load_roms(void)
{
	if (!read_exact_file("rom.dat", rom, sizeof(rom)))
		pd->system->error("Failed to load rom.dat from PDX data");

	memset(cart, 0, sizeof(cart));

	{
		const char* cart_name = "cart.vec";
		unsigned int cart_bytes = read_partial_file(cart_name, cart, sizeof(cart));

		if (cart_bytes == 0) {
			cart_name = "mine_storm.vec";
			cart_bytes = read_partial_file(cart_name, cart, sizeof(cart));
		}

		if (cart_bytes > 0) {
			pd->system->logToConsole("vecx: loaded %s (%u bytes)", cart_name, cart_bytes);
		} else {
			pd->system->logToConsole("vecx: no cart.vec or mine_storm.vec found; using BIOS only");
		}
	}
}

static void update_scaling(void)
{
	int scale_x = (ALG_MAX_Y + screen_w - 1) / screen_w;
	int scale_y = (ALG_MAX_X + screen_h - 1) / screen_h;
	int content_w;
	int content_h;

	scale_factor = scale_x > scale_y ? scale_x : scale_y;
	if (scale_factor < 1)
		scale_factor = 1;

	content_w = ALG_MAX_Y / scale_factor;
	content_h = ALG_MAX_X / scale_factor;
	offset_x = (screen_w - content_w) / 2;
	offset_y = (screen_h - content_h) / 2;
}

static void update_input(void)
{
	PDButtons current;
	unsigned buttons = 0xff;

	pd->system->getButtonState(&current, NULL, NULL);

	if ((current & kButtonUp) && !(current & kButtonDown))
		alg_jch0 = 0x00;
	else if ((current & kButtonDown) && !(current & kButtonUp))
		alg_jch0 = 0xff;
	else
		alg_jch0 = 0x80;

	if ((current & kButtonLeft) && !(current & kButtonRight))
		alg_jch1 = 0xff;
	else if ((current & kButtonRight) && !(current & kButtonLeft))
		alg_jch1 = 0x00;
	else
		alg_jch1 = 0x80;

	if (current & kButtonA)
		buttons &= ~0x04U;
	if (current & kButtonB)
		buttons &= ~0x08U;

	snd_regs[14] = buttons;
}

static unsigned long ratio_x100(uint32_t numerator, uint32_t elapsed_ms)
{
	if (elapsed_ms == 0)
		return 0;

	return (unsigned long)(((uint64_t)numerator * 100000ULL) / elapsed_ms);
}

static void sample_insert_top(unsigned value, unsigned long count,
	unsigned values[SAMPLE_LOG_TOP], unsigned long counts[SAMPLE_LOG_TOP])
{
	int pos;

	if (count == 0)
		return;

	for (pos = 0; pos < SAMPLE_LOG_TOP; pos++) {
		if (count > counts[pos]) {
			int move;

			for (move = SAMPLE_LOG_TOP - 1; move > pos; move--) {
				values[move] = values[move - 1];
				counts[move] = counts[move - 1];
			}

			values[pos] = value;
			counts[pos] = count;
			return;
		}
	}
}

static void log_sample_profile(void)
{
	unsigned op_values[SAMPLE_LOG_TOP] = { 0 };
	unsigned long op_counts[SAMPLE_LOG_TOP] = { 0 };
	unsigned pc_values[SAMPLE_LOG_TOP] = { 0 };
	unsigned long pc_counts[SAMPLE_LOG_TOP] = { 0 };
	unsigned i;

	if (vecx_sample_total == 0)
		return;

	for (i = 0; i < VECX_SAMPLE_OPCODES; i++)
		sample_insert_top(i, vecx_sample_opcode_counts[i], op_values, op_counts);

	for (i = 0; i < VECX_SAMPLE_PC_SLOTS; i++)
		sample_insert_top(vecx_sample_pc_addr[i], vecx_sample_pc_count[i], pc_values, pc_counts);

	pd->system->logToConsole(
		"vecx sample build=\"%s\" samples=%lu ops=%02x:%lu,%02x:%lu,%02x:%lu,%02x:%lu,%02x:%lu pcs=%04x:%lu,%04x:%lu,%04x:%lu,%04x:%lu,%04x:%lu",
		BUILD_LABEL,
		vecx_sample_total,
		op_values[0],
		op_counts[0],
		op_values[1],
		op_counts[1],
		op_values[2],
		op_counts[2],
		op_values[3],
		op_counts[3],
		op_values[4],
		op_counts[4],
		pc_values[0],
		pc_counts[0],
		pc_values[1],
		pc_counts[1],
		pc_values[2],
		pc_counts[2],
		pc_values[3],
		pc_counts[3],
		pc_values[4],
		pc_counts[4]);
}

static void reset_benchmark(uint32_t now_ms)
{
	bench_window_start_ms = now_ms;
	bench_update_count = 0;
	bench_render_count = 0;
	bench_skipped_frames = 0;
	bench_total_update_ms = 0;
	bench_total_render_ms = 0;
	bench_total_vectors = 0;
	bench_total_emu_cycles = 0;
	bench_total_emu_instructions = 0;
	bench_total_wait_skips = 0;
	bench_total_wait_skip_cycles = 0;
	bench_total_delay_skips = 0;
	bench_total_delay_skip_cycles = 0;
	bench_min_update_ms = UINT32_MAX;
	bench_max_update_ms = 0;
	vecx_sample_reset();
}

static void maybe_log_benchmark(uint32_t now_ms)
{
	uint32_t elapsed_ms = now_ms - bench_window_start_ms;
	unsigned long avg_fps_x100;
	unsigned long avg_render_fps_x100;
	unsigned long avg_update_x100;
	unsigned long avg_render_x100;
	unsigned long avg_instructions_x100;
	unsigned long avg_cycles_per_instruction_x100;
	unsigned long avg_vectors_x100;

	if (elapsed_ms < BENCHMARK_LOG_MS || bench_update_count == 0)
		return;

	avg_fps_x100 = ratio_x100(bench_update_count, elapsed_ms);
	avg_render_fps_x100 = ratio_x100(bench_render_count, elapsed_ms);
	avg_update_x100 = (unsigned long)(((uint64_t)bench_total_update_ms * 100ULL) / bench_update_count);
	avg_render_x100 = bench_render_count > 0
		? (unsigned long)(((uint64_t)bench_total_render_ms * 100ULL) / bench_render_count)
		: 0;
	avg_instructions_x100 = (unsigned long)(((uint64_t)bench_total_emu_instructions * 100ULL) / bench_update_count);
	avg_cycles_per_instruction_x100 = bench_total_emu_instructions > 0
		? (unsigned long)(((uint64_t)bench_total_emu_cycles * 100ULL) / bench_total_emu_instructions)
		: 0;
	avg_vectors_x100 = bench_render_count > 0
		? (unsigned long)(((uint64_t)bench_total_vectors * 100ULL) / bench_render_count)
		: 0;

	pd->system->logToConsole(
		"vecx bench build=\"%s\" window_ms=%lu updates=%lu renders=%lu avg_fps=%lu.%02lu avg_render_fps=%lu.%02lu avg_update_ms=%lu.%02lu avg_render_ms=%lu.%02lu min_update_ms=%lu max_update_ms=%lu emu_cycles=%lu emu_instr=%lu avg_instr_update=%lu.%02lu avg_cpi=%lu.%02lu avg_vectors=%lu.%02lu wait_skips=%lu wait_skip_cycles=%lu delay_skips=%lu delay_skip_cycles=%lu skipped=%lu",
		BUILD_LABEL,
		(unsigned long)elapsed_ms,
		(unsigned long)bench_update_count,
		(unsigned long)bench_render_count,
		avg_fps_x100 / 100UL,
		avg_fps_x100 % 100UL,
		avg_render_fps_x100 / 100UL,
		avg_render_fps_x100 % 100UL,
		avg_update_x100 / 100UL,
		avg_update_x100 % 100UL,
		avg_render_x100 / 100UL,
		avg_render_x100 % 100UL,
		(unsigned long)bench_min_update_ms,
		(unsigned long)bench_max_update_ms,
		(unsigned long)bench_total_emu_cycles,
		(unsigned long)bench_total_emu_instructions,
		avg_instructions_x100 / 100UL,
		avg_instructions_x100 % 100UL,
		avg_cycles_per_instruction_x100 / 100UL,
		avg_cycles_per_instruction_x100 % 100UL,
		avg_vectors_x100 / 100UL,
		avg_vectors_x100 % 100UL,
		(unsigned long)bench_total_wait_skips,
		(unsigned long)bench_total_wait_skip_cycles,
		(unsigned long)bench_total_delay_skips,
		(unsigned long)bench_total_delay_skip_cycles,
		(unsigned long)bench_skipped_frames);

	log_sample_profile();
	reset_benchmark(now_ms);
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

		x0 = offset_x + (int)((ALG_MAX_Y - v->y0) / scale_factor);
		y0 = offset_y + (int)(v->x0 / scale_factor);
		x1 = offset_x + (int)((ALG_MAX_Y - v->y1) / scale_factor);
		y1 = offset_y + (int)(v->x1 / scale_factor);

#if DIRECT_FRAMEBUFFER_RENDER
		draw_framebuffer_line(x0, y0, x1, y1);
#else
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

void osint_render(void)
{
	uint32_t start_ms = pd->system->getCurrentTimeMilliseconds();
	uint32_t drawn_vectors = 0;

	if (RENDER_FRAME_SKIP > 1) {
		render_skip_phase++;
		if (render_skip_phase < RENDER_FRAME_SKIP) {
			bench_skipped_frames++;
			return;
		}
		render_skip_phase = 0;
	}

#if DIRECT_FRAMEBUFFER_RENDER
	clear_framebuffer_white();
#else
	pd->graphics->clear(kColorWhite);
#endif

	drawn_vectors += draw_persistence_frames();
	drawn_vectors += draw_vector_list(vectors_draw, vector_draw_cnt);
	save_persistence_frame();

#if DIRECT_FRAMEBUFFER_RENDER
	pd->graphics->markUpdatedRows(0, screen_h - 1);
#endif

	pd->system->drawFPS(0, 0);

	bench_render_count++;
	bench_total_vectors += drawn_vectors;
	bench_total_render_ms += pd->system->getCurrentTimeMilliseconds() - start_ms;
}

static int update(void* userdata)
{
	uint32_t start_ms;
	uint32_t elapsed_ms;
	PlaydateAPI* playdate = userdata;

	(void)playdate;

	start_ms = pd->system->getCurrentTimeMilliseconds();

	update_input();
	vecx_emu(EMU_CYCLES_PER_UPDATE);
	bench_total_emu_cycles += (uint32_t)vecx_emu_cycle_count;
	bench_total_emu_instructions += (uint32_t)vecx_emu_instruction_count;
	bench_total_wait_skips += (uint32_t)vecx_wait_skip_count;
	bench_total_wait_skip_cycles += (uint32_t)vecx_wait_skip_cycles;
	bench_total_delay_skips += (uint32_t)vecx_delay_skip_count;
	bench_total_delay_skip_cycles += (uint32_t)vecx_delay_skip_cycles;

	elapsed_ms = pd->system->getCurrentTimeMilliseconds() - start_ms;
	bench_update_count++;
	bench_total_update_ms += elapsed_ms;
	if (elapsed_ms < bench_min_update_ms)
		bench_min_update_ms = elapsed_ms;
	if (elapsed_ms > bench_max_update_ms)
		bench_max_update_ms = elapsed_ms;

	maybe_log_benchmark(pd->system->getCurrentTimeMilliseconds());

	return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
	(void)arg;

	if (event == kEventInit) {
		pd = playdate;
		screen_w = pd->display->getWidth();
		screen_h = pd->display->getHeight();
		update_scaling();

		pd->display->setRefreshRate((float)TARGET_FPS);
		pd->display->setInverted(1);
		pd->system->setAutoLockDisabled(1);

		load_roms();
		e8910_init_sound();
		vecx_reset();
		reset_benchmark(pd->system->getCurrentTimeMilliseconds());

		pd->system->logToConsole("vecx: Playdate C build %s", BUILD_LABEL);
		pd->system->setUpdateCallback(update, pd);
	} else if (event == kEventTerminate) {
		e8910_done_sound();
	}

	return 0;
}
