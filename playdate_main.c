#include <stdint.h>
#include <string.h>

#include "pd_api.h"

#include "e8910.h"
#include "osint.h"
#include "vecx.h"
#include "e6809.h"
#include "render.h"
#include "rom_picker.h"

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

/* Minimal snprintf for the ROM-picker submodule (it only formats "%s%s%s").
 * Providing our own keeps newlib's vfprintf -> malloc chain out of the link --
 * the device links no heap, and that chain is fragile here. Handles %s %c %d %u
 * %x %%; returns the length that would be written, like the standard.
 * DEVICE ONLY: the Mac simulator has a full libc (and treats snprintf as a
 * fortify builtin), so there it uses the system snprintf. */
#if !defined(TARGET_SIMULATOR)
int snprintf(char* str, size_t size, const char* fmt, ...)
{
	va_list ap;
	size_t pos = 0;   /* bytes placed (kept < size to leave room for NUL) */
	size_t total = 0; /* bytes that would be written */
	const char* f;
	char num[16];

	va_start(ap, fmt);
	for (f = fmt; *f != '\0'; f++) {
		if (*f != '%') {
			if (pos + 1 < size) str[pos++] = *f;
			total++;
			continue;
		}

		switch (*++f) {
		case 's': {
			const char* s = va_arg(ap, const char*);
			if (s == NULL)
				s = "(null)";
			for (; *s != '\0'; s++) {
				if (pos + 1 < size) str[pos++] = *s;
				total++;
			}
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			if (pos + 1 < size) str[pos++] = c;
			total++;
			break;
		}
		case 'd':
		case 'u':
		case 'x': {
			unsigned base = (*f == 'x') ? 16u : 10u;
			unsigned long v;
			int neg = 0;
			int n = 0;

			if (*f == 'd') {
				long sv = va_arg(ap, int);
				if (sv < 0) { neg = 1; v = (unsigned long)(-sv); }
				else v = (unsigned long)sv;
			} else {
				v = va_arg(ap, unsigned int);
			}
			do {
				unsigned dig = (unsigned)(v % base);
				num[n++] = (char)(dig < 10u ? '0' + dig : 'a' + dig - 10u);
				v /= base;
			} while (v != 0 && n < (int)sizeof(num));
			if (neg) { if (pos + 1 < size) str[pos++] = '-'; total++; }
			while (n-- > 0) {
				if (pos + 1 < size) str[pos++] = num[n];
				total++;
			}
			break;
		}
		case '%':
			if (pos + 1 < size) str[pos++] = '%';
			total++;
			break;
		case '\0':
			f--; /* trailing '%': stop */
			break;
		default:
			if (pos + 1 < size) str[pos++] = '%';
			total++;
			if (pos + 1 < size) str[pos++] = *f;
			total++;
			break;
		}
	}
	va_end(ap);

	if (size > 0)
		str[pos] = '\0';
	return (int)total;
}
#endif /* !TARGET_SIMULATOR */

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

static void load_bios(void)
{
	if (!read_exact_file("rom.dat", rom, sizeof(rom)))
		pd->system->error("Failed to load rom.dat from PDX data");
}

/* load a .vec cart from an absolute path (the ROM picker hands us the full path,
 * e.g. /Shared/Emulation/vec/games/foo.vec). */
static void load_cart(const char* path)
{
	unsigned int n;

	memset(cart, 0, sizeof(cart));
	n = read_partial_file(path, cart, sizeof(cart));

	if (n > 0)
		pd->system->logToConsole("vecx: loaded cart %s (%u bytes)", path, n);
	else
		pd->system->logToConsole("vecx: FAILED to load cart %s", path);
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

	/* HLE scoping: region split (is BIOS worth replacing?) + hottest BIOS
	 * 256-byte windows (which routines dominate). */
	{
		unsigned bk_values[SAMPLE_LOG_TOP] = { 0 };
		unsigned long bk_counts[SAMPLE_LOG_TOP] = { 0 };
		unsigned long rcart = vecx_sample_region[0];
		unsigned long rram  = vecx_sample_region[1];
		unsigned long rbios = vecx_sample_region[2];
		unsigned long roth  = vecx_sample_region[3];
		unsigned long tot   = vecx_sample_total ? vecx_sample_total : 1;

		for (i = 0; i < VECX_SAMPLE_BIOS_BUCKETS; i++)
			sample_insert_top(0xe000u + (i << 8), vecx_sample_bios_bucket[i],
				bk_values, bk_counts);

		pd->system->logToConsole(
			"vecx region samples=%lu cart=%lu(%lu%%) ram=%lu bios=%lu(%lu%%) other=%lu | bios_top=%04x:%lu,%04x:%lu,%04x:%lu,%04x:%lu,%04x:%lu",
			vecx_sample_total,
			rcart, (rcart * 100ul) / tot,
			rram,
			rbios, (rbios * 100ul) / tot,
			roth,
			bk_values[0], bk_counts[0],
			bk_values[1], bk_counts[1],
			bk_values[2], bk_counts[2],
			bk_values[3], bk_counts[3],
			bk_values[4], bk_counts[4]);
	}
}

/* HLE milestone-1: dump the ground-truth capture of the F3DD BIOS draw loop.
 * Per window: how many draw calls, and avg vectors/cycles each (the spec a
 * native intercept must reproduce). Plus one full sample: the list bytes, the
 * beam start, and the first 2 emitted vectors (to derive the geometry). */
static void log_hle_capture(void)
{
	unsigned long c = vecx_hle_calls ? vecx_hle_calls : 1;

	if (vecx_hle_exec_calls != 0) {
		pd->system->logToConsole(
			"vecx hle-active: HLE'd %lu F410 calls, max_scale=%lu max_ent=%lu (enabled=%d)",
			vecx_hle_exec_calls, vecx_hle_max_scale, vecx_hle_max_ent, vecx_hle_enabled);
		return;
	}

	if (vecx_hle_calls == 0)
		return;

	pd->system->logToConsole(
		"vecx hle: calls=%lu OK=%lu cntmiss=%lu geommiss=%lu avg_vec=%lu (F410 shadow-validate)",
		vecx_hle_calls, vecx_hle_ok, vecx_hle_cntmiss, vecx_hle_geommiss,
		vecx_hle_tot_vec / c);

	if (vecx_hle_mm_valid)
		pd->system->logToConsole(
			"vecx hle-MISMATCH: i=%ld npred=%ld nreal=%ld pred=(%ld,%ld)->(%ld,%ld) real=(%ld,%ld)->(%ld,%ld)",
			vecx_hle_mm_idx, vecx_hle_mm_npred, vecx_hle_mm_nreal,
			vecx_hle_mm_p[0], vecx_hle_mm_p[1], vecx_hle_mm_p[2], vecx_hle_mm_p[3],
			vecx_hle_mm_r[0], vecx_hle_mm_r[1], vecx_hle_mm_r[2], vecx_hle_mm_r[3]);
	else if (vecx_hle_s_valid)
		pd->system->logToConsole(
			"vecx hle-sample: scale=%u vec=%ld listX=%04x list=%02x,%02x,%02x,%02x,%02x,%02x start=(%ld,%ld) v0=(%ld,%ld)->(%ld,%ld) v1=(%ld,%ld)->(%ld,%ld)",
			vecx_hle_s_count, vecx_hle_s_vec, vecx_hle_s_listx,
			vecx_hle_s_list[0], vecx_hle_s_list[1], vecx_hle_s_list[2],
			vecx_hle_s_list[3], vecx_hle_s_list[4], vecx_hle_s_list[5],
			vecx_hle_s_sx, vecx_hle_s_sy,
			vecx_hle_s_v[0][0], vecx_hle_s_v[0][1], vecx_hle_s_v[0][2], vecx_hle_s_v[0][3],
			vecx_hle_s_v[1][0], vecx_hle_s_v[1][1], vecx_hle_s_v[1][2], vecx_hle_s_v[1][3]);
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
	vecx_hle_reset();
}

/* sound diagnostics: snd_regs is the AY register file (vecx.c); the dbg_* counters
 * are bumped by the audio-thread callback (e8910.c). */
extern unsigned snd_regs[16];
extern volatile unsigned e8910_dbg_calls;
extern volatile unsigned e8910_dbg_active; /* underrun samples */
extern volatile int e8910_dbg_maxvol;
extern volatile int e8910_dbg_ratio_x1000;
extern unsigned e8910_ring_fill(void);

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

	pd->system->logToConsole(
		"vecx snd: callbacks=%u underruns=%u ringfill=%u ratio=%d maxvol=%d volA=%u volB=%u volC=%u enable=0x%02x",
		e8910_dbg_calls, e8910_dbg_active, e8910_ring_fill(), e8910_dbg_ratio_x1000, e8910_dbg_maxvol,
		snd_regs[8], snd_regs[9], snd_regs[10], snd_regs[7]);
	e8910_dbg_maxvol = 0;
	e8910_dbg_calls = 0;
	e8910_dbg_active = 0;

	log_sample_profile();
	log_hle_capture();
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

/* ROM picker: at boot we show the picker; once a .vec is chosen we load it and
 * switch to running the emulator. */
static const char* rom_extensions[] = { "vec", NULL };
static int picker_active = 1;
static int want_picker = 0;
static char selected_rom[ROM_PICKER_MAX_PATH];

static void on_rom_picked(const char* path, void* userdata)
{
	(void)userdata;
	strncpy(selected_rom, path, sizeof(selected_rom) - 1);
	selected_rom[sizeof(selected_rom) - 1] = '\0';
}

static void init_rom_picker(void)
{
	RomPickerConfig cfg;

	cfg.folder = "/Shared/Emulation/vec/games/";
	cfg.extensions = rom_extensions;
	cfg.on_select = on_rom_picked;
	cfg.userdata = NULL;
	cfg.auto_load_single = 1; /* one ROM -> skip the picker */
	rom_picker_init(pd, &cfg);
}

/* system-menu "ROM Picker": request a return to the picker (done in update()). */
static void rompicker_menu_cb(void* userdata)
{
	(void)userdata;
	want_picker = 1;
}

/* The system menu caps at 3 items, so one slot is shared: "ROM Picker" while
 * playing (return to the picker), "Sound" while in the picker (arm audio before
 * launching a game). Audio generation costs CPU, so sound is OFF by default and
 * only toggleable from the picker. The menu is rebuilt on each transition so the
 * swappable item stays first, followed by the renderer's Rotation/Frameskip. */
static int sound_enabled;
static PDMenuItem* sound_item;

static void sound_menu_cb(void* userdata)
{
	(void)userdata;
	sound_enabled = pd->system->getMenuItemValue(sound_item);
}

static void rebuild_menu(int in_picker)
{
	pd->system->removeAllMenuItems();
	if (in_picker)
		sound_item = pd->system->addCheckmarkMenuItem("Sound", sound_enabled, sound_menu_cb, NULL);
	else
		pd->system->addMenuItem("ROM Picker", rompicker_menu_cb, NULL);
	render_refresh_menu();
}

static void start_emulation(void)
{
	load_cart(selected_rom);
	memset(ram, 0, sizeof(ram)); /* fresh RAM for the new cart */
	vecx_reset();
	rom_picker_free();
	pd->display->setInverted(1); /* Vectrex look: white vectors on black */
	/* clear so the picker's last (black-on-white) frame isn't shown inverted for
	 * one frame during the switch; white -> inverted -> a clean black frame */
	pd->graphics->clear(kColorWhite);
	reset_benchmark(pd->system->getCurrentTimeMilliseconds());
	want_picker = 0;
	picker_active = 0;
	rebuild_menu(0); /* gameplay: "ROM Picker" slot */
}

/* back to the picker: wipe the machine's memory + reset everything, restore the
 * picker's normal display, and rescan the ROM folder. */
static void return_to_picker(void)
{
	memset(ram, 0, sizeof(ram));
	memset(cart, 0, sizeof(cart));
	vecx_reset();
	selected_rom[0] = '\0';
	want_picker = 0;
	pd->display->setInverted(0);
	/* clear so the emulator's last (inverted) frame isn't shown for one frame
	 * before the picker redraws; white -> non-inverted -> a clean white frame */
	pd->graphics->clear(kColorWhite);
	init_rom_picker();
	picker_active = 1;
	rebuild_menu(1); /* picker: "Sound" slot */
}

static int update(void* userdata)
{
	uint32_t start_ms;
	uint32_t elapsed_ms;
	PlaydateAPI* playdate = userdata;

	(void)playdate;

	if (picker_active) {
		if (selected_rom[0] == '\0')
			rom_picker_update();
		if (selected_rom[0] != '\0')
			start_emulation();
		return 1;
	}

	if (want_picker) {
		return_to_picker();
		return 1;
	}

	start_ms = pd->system->getCurrentTimeMilliseconds();

	update_input();
	vecx_emu(EMU_CYCLES_PER_UPDATE);

	/* Keep the audio buffer topped up to a target each update, so playback is
	 * smooth and at real pitch. The AY thus free-runs ahead of the (~30% speed)
	 * emulation and register changes lag behind -- audible as distortion -- but
	 * it never starves into noise, and generating here on the main thread (not the
	 * audio thread) avoids the free-running corruption that caused the white noise. */
	if (sound_enabled) {
		const int audio_target = 2048; /* ~46 ms buffered at 44100 */
		int nsamp = audio_target - (int)e8910_ring_fill();

		if (nsamp > 0)
			e8910_generate(nsamp);
	}

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
		pd->system->setAutoLockDisabled(1);

		load_bios();
		e8910_init_sound(pd);
		itcm_relocate();

		init_rom_picker();
		rebuild_menu(1); /* boot into the picker: "Sound" + Rotation + Frameskip */

		pd->system->logToConsole("vecx: Playdate C build %s", BUILD_LABEL);
		pd->system->setUpdateCallback(update, pd);
	} else if (event == kEventTerminate) {
		e8910_done_sound();
	}

	return 0;
}
