#ifndef __VECX_H
#define __VECX_H

#if defined(__GNUC__) && defined(TARGET_PLAYDATE)
#define VECX_NOINLINE __attribute__((noinline))
#else
#define VECX_NOINLINE
#endif

/* place a function in the .itcm section so it can be relocated at runtime into
 * fast TCM-backed SRAM. moved (not added) code keeps the image size ~constant.
 */
#if defined(__GNUC__) && defined(TARGET_PLAYDATE)
#define VECX_ITCM __attribute__((section(".itcm"), noinline, used))
#else
#define VECX_ITCM
#endif

enum {
	VECTREX_MHZ		= 1500000, /* speed of the vectrex being emulated */
	VECTREX_UPDATE_HZ = 120,
	/* render/phosphor rate: a full Vectrex screen redraw is ~50Hz (~30000
	 * cycles). Rendering faster than this (it was = UPDATE_HZ = 120) reset the
	 * vector list mid-redraw, so each frame only held ~42% of the screen's
	 * strokes -> sparse/illegible text. 50 = one complete screen per render.
	 */
	VECTREX_RENDER_HZ = 50,
	VECTREX_VECTOR_CAP = 768,
	VECTREX_VECTOR_HASH = 1021,
	VECTREX_COLORS  = 128,     /* number of possible colors ... grayscale */
	VECX_SAMPLE_OPCODES = 256,
	VECX_SAMPLE_PC_SLOTS = 32,
	/* HLE scoping: split sampled PCs by memory region, and bucket BIOS
	 * (0xE000-0xFFFF, 8KB) into 256-byte windows to localize hot routines. */
	VECX_SAMPLE_REGIONS = 4,        /* cart, ram, bios, other */
	VECX_SAMPLE_BIOS_BUCKETS = 32,  /* (0xFFFF-0xE000+1)/256 */

	ALG_MAX_X		= 33000,
	ALG_MAX_Y		= 41000
};

typedef struct vector_type {
	long x0, y0; /* start coordinate */
	long x1, y1; /* end coordinate */

	/* color [0, VECTREX_COLORS - 1], if color = VECTREX_COLORS, then this is
	 * an invalid entry and must be ignored.
	 */
	unsigned char color;
} vector_t;

extern unsigned char rom[8192];
extern unsigned char cart[32768];
extern unsigned char ram[1024];

extern unsigned snd_regs[16];
extern unsigned alg_jch0;
extern unsigned alg_jch1;
extern unsigned alg_jch2;
extern unsigned alg_jch3;

extern long vector_draw_cnt;
extern long vector_erse_cnt;
extern vector_t *vectors_draw;
extern vector_t *vectors_erse;
extern unsigned long vecx_emu_cycle_count;
extern unsigned long vecx_emu_instruction_count;
extern unsigned long vecx_wait_skip_count;
extern unsigned long vecx_wait_skip_cycles;
extern unsigned long vecx_delay_skip_count;
extern unsigned long vecx_delay_skip_cycles;
extern unsigned long vecx_sample_total;
extern unsigned long vecx_sample_opcode_counts[VECX_SAMPLE_OPCODES];
extern unsigned vecx_sample_pc_addr[VECX_SAMPLE_PC_SLOTS];
extern unsigned long vecx_sample_pc_count[VECX_SAMPLE_PC_SLOTS];
extern unsigned long vecx_sample_region[VECX_SAMPLE_REGIONS];
extern unsigned long vecx_sample_bios_bucket[VECX_SAMPLE_BIOS_BUCKETS];

/* HLE milestone-1 ground-truth capture of the BIOS F3DD draw loop. */
extern unsigned long vecx_hle_calls, vecx_hle_tot_count, vecx_hle_tot_cyc, vecx_hle_tot_vec;
extern unsigned vecx_hle_s_count, vecx_hle_s_listx, vecx_hle_s_list[6];
extern long vecx_hle_s_cyc, vecx_hle_s_vec, vecx_hle_s_sx, vecx_hle_s_sy;
extern long vecx_hle_s_v[2][5];
extern int vecx_hle_s_valid;
extern unsigned long vecx_hle_ok, vecx_hle_cntmiss, vecx_hle_geommiss;
extern int vecx_hle_mm_valid;
extern long vecx_hle_mm_idx, vecx_hle_mm_npred, vecx_hle_mm_nreal;
extern long vecx_hle_mm_p[4], vecx_hle_mm_r[4];
extern int vecx_hle_enabled;
extern unsigned long vecx_hle_exec_calls;
extern unsigned long vecx_hle_declines;
extern unsigned long vecx_hle_max_scale, vecx_hle_max_ent;
void vecx_hle_reset (void);

VECX_NOINLINE unsigned char vecx_read8 (unsigned address);
VECX_NOINLINE void vecx_write8 (unsigned address, unsigned char data);
void vecx_reset (void);
void vecx_emu (long cycles);
void vecx_sample_reset (void);

#endif
