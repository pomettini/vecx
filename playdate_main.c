#include <stdint.h>
#include <string.h>

#include "pd_api.h"

#include "e8910.h"
#include "osint.h"
#include "vecx.h"
#include "e6809.h"
#include "render.h"
#include "jit.h"

#define TARGET_FPS VECTREX_UPDATE_HZ
#define EMU_CYCLES_PER_UPDATE ((long)(VECTREX_MHZ / TARGET_FPS))
#define BENCHMARK_LOG_MS 5000U
/* Keep this string a FIXED LENGTH (11 chars) and avoid editing playdate_main.c
 * for render work: its .rodata size feeds the whole-binary I-cache layout that
 * sets the 38.2-FPS packing. The compile-time in BUILD_LABEL already
 * distinguishes builds. (11 chars == the layout that measured 38.2.)
 */
#define BUILD_VARIANT "vecx-render"
/* Bytes below the init stack frame for the relocated hot-core pool TOP. The pool
 * extends DOWN by the core size and must land in the safe gap between resident
 * firmware data (floor) and the live stack (ceiling). 0x1180 keeps the top at
 * ~0x20008a00 with ~1.8KB of headroom above the floor for the current core size.
 * See PLAYDATE_ITCM_GUIDE.md before changing (re-probe per device/SDK).
 */
#define POOL_MARGIN 0x1180u
/* DTCM execution is alignment-sensitive (the M7 fetches code from TCM in 64-bit
 * lines, and the decode's branch targets land differently per base alignment).
 * The relocated core's INTERNAL layout is fixed; only its absolute base
 * alignment varies. POOL_NUDGE (multiple of 4) shifts the 32-aligned pool base
 * to land the hot decode on the fast alignment. Swept on device; lock the best.
 */
#define POOL_NUDGE 0u
#define BUILD_LABEL __DATE__ " " __TIME__ " " BUILD_VARIANT
#define SAMPLE_LOG_TOP 5

#if defined(TARGET_PLAYDATE)
#define ITCM_ENABLE 1
extern char __itcm_start[];
extern char __itcm_end[];
#else
#define ITCM_ENABLE 0
#endif

static PlaydateAPI* pd;

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

static unsigned render_skip_phase;

static int update(void* userdata);

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

void osint_render(void)
{
	uint32_t start_ms = pd->system->getCurrentTimeMilliseconds();
	uint32_t drawn_vectors;
	int skip = render_frame_skip();

	if (skip > 0) {
		render_skip_phase++;
		if (render_skip_phase <= skip) {
			bench_skipped_frames++;
			return;
		}
		render_skip_phase = 0;
	}

	drawn_vectors = render_draw_frame();

	pd->system->drawFPS(0, 0);

	bench_render_count++;
	bench_total_vectors += drawn_vectors;
	bench_total_render_ms += pd->system->getCurrentTimeMilliseconds() - start_ms;
}

static void itcm_drain(void)
{
	uint32_t t = pd->system->getCurrentTimeMilliseconds();
	while (pd->system->getCurrentTimeMilliseconds() - t < 120u) {
	}
}

/* Relocate the compact 6809 hot core (e6809_hotcore, marked VECX_ITCM and
 * collected into the .text output so its R_ARM_ABS32 relocations are applied)
 * into a fast DTCM pool, then point e6809_hotcore_p at the copy.
 *
 * The pool lives in the unused stack region below the init frame, in the safe
 * gap between resident firmware data (floor ~0x200074d0) and the live stack.
 * See PLAYDATE_ITCM_GUIDE.md. The relocation self-test probes (D1-D6) are kept:
 * they confirm the relocated copy is executable and call-correct, AND -- on this
 * device -- they are LOAD-BEARING for performance: their presence in e6809.c +
 * here sets the exact whole-binary layout that packs the hot helpers into the
 * I-cache without conflicts (+~3 FPS vs. without). The win is fragile binary-
 * wide alignment; do not "clean up" without re-measuring on device.
 */
static void itcm_relocate(void)
{
#if ITCM_ENABLE
	uintptr_t size = (uintptr_t)__itcm_end - (uintptr_t)__itcm_start;
	uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
	uintptr_t pool = (frame - POOL_MARGIN - size) & ~(uintptr_t)0xf;
	uintptr_t off = ((uintptr_t)e6809_hotcore & ~(uintptr_t)1) - (uintptr_t)__itcm_start;
	const uint32_t *src = (const uint32_t *)__itcm_start;
	volatile uint32_t *dst = (volatile uint32_t *)pool;
	uint32_t words = (uint32_t)(size / 4u);
	uint32_t i;

	pd->system->logToConsole(
		"vecx itcm: A src=%p dst=%p size=%u hotcore=%p", (void*)__itcm_start,
		(void*)pool, (unsigned)size, (void*)e6809_hotcore);
	itcm_drain();

	for (i = 0; i < words; i++)
		dst[i] = src[i];
	pd->system->logToConsole("vecx itcm: B copy done (%u words)", words);
	itcm_drain();

	pd->system->clearICache();
	e6809_hotcore_p = (unsigned (*)(unsigned, unsigned))((pool + off) | 1u);
	pd->system->logToConsole(
		"vecx itcm: C hotcore relocated %p -> %p", (void*)e6809_hotcore,
		(void*)e6809_hotcore_p);
	itcm_drain();

	{
		unsigned r = e6809_hotcore_p (0xabcdu, 0);
		pd->system->logToConsole("vecx itcm: D1 entry=0x%x %s (pool %p-%p)",
			r, r == 0x42u ? "EXEC-OK" : "BAD", (void*)pool, (void*)(pool + size));
		itcm_drain();
		r = e6809_hotcore_p (0xabceu, 0);
		pd->system->logToConsole("vecx itcm: D2 ea_indexed call=0x%x (ok>=0x100)", r);
		itcm_drain();
		r = e6809_hotcore_p (0xabcfu, 0);
		pd->system->logToConsole("vecx itcm: D3 write8 call=0x%x (ok=0x2a5)", r);
		itcm_drain();
		r = e6809_hotcore_p (0xabd0u, 0);
		pd->system->logToConsole("vecx itcm: D4 inst_sub8 call=0x%x (ok=0x340)", r);
		itcm_drain();
		r = e6809_hotcore_p (0xabd1u, 0);
		pd->system->logToConsole("vecx itcm: D5 read8(VIA) call=0x%x (ok>=0x400)", r);
		itcm_drain();
		r = e6809_hotcore_p (0xabd2u, 0);
		pd->system->logToConsole("vecx itcm: D6 write8(VIA) call=0x%x (ok=0x500)", r);
		itcm_drain();
	}
#endif
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
		render_init(pd, pd->display->getWidth(), pd->display->getHeight());

		pd->display->setRefreshRate((float)TARGET_FPS);
		pd->display->setInverted(1);
		pd->system->setAutoLockDisabled(1);

		load_roms();
		e8910_init_sound();
		vecx_reset();
		itcm_relocate();
		jit_selftest(pd);
		reset_benchmark(pd->system->getCurrentTimeMilliseconds());

		pd->system->logToConsole("vecx: Playdate C build %s", BUILD_LABEL);
		pd->system->setUpdateCallback(update, pd);
	} else if (event == kEventTerminate) {
		e8910_done_sound();
	}

	return 0;
}
