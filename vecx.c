#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "e6809.h"
#include "vecx.h"
#include "osint.h"
#include "e8910.h"

#define einline __inline
/* FAST_BATCH batches the analog/VIA stepping over multiple cycles. Batching
 * blindly mangles the font, because it samples the beam ramp (Timer-1 PB7, which
 * sets stroke length) once per batch; vecx_machine_advance now caps each batch at
 * the next PB7 toggle so the ramp is constant within a batch -> correct font AND
 * batched speed. WAIT_LOOP_SKIP skips the BIOS idle wait (big speed lever, never
 * touches the beam).
 */
#define VECX_FAST_BATCH 1
#define VECX_MACHINE_ADVANCE_BATCH 1
#define VECX_WAIT_LOOP_SKIP 1
#define VECX_BIOS_DELAY_SKIP 0
#define VECX_SAMPLE_PROFILE 0   /* HLE scoping rig (region split + BIOS buckets); flip to 1 to profile */
#define VECX_SAMPLE_INTERVAL 64
/* HLE milestone 2: active F410 intercept (Mov_Draw_VL). Works + renders correctly
 * at +17-25%, but skipping the routine desyncs a BIOS timer/state interaction that
 * occasionally FREEZES the game (watchdog reset) after sustained play -- confirmed
 * via crashlog.txt (watchdog, not a fault). Cause localized to the timing path
 * (emission ruled out by bisection) but not fully fixed. EXPERIMENTAL: off by
 * default; set to 1 to use it (accepting the occasional freeze). See NOTES.md. */
#define VECX_HLE_ACTIVE 0
#define VECX_HLE_CAPTURE 0      /* shadow capture/validator (set 1 + ACTIVE 0 to re-validate geometry) */

unsigned char rom[8192];
unsigned char cart[32768];
/* ram is a plain (non-relocated) array: it stays D-cache-resident. Exposed
 * (non-static) so the relocated hot core can inline reads of it (hot_read8),
 * skipping the vecx_read8 dispatch CALL for side-effect-free $C8xx accesses.
 * NOTE: a DTCM-relocation experiment (build 118) made ram a POINTER into a fast
 * DTCM buffer and REGRESSED -4.5%: the 1KB ram already fit the D-cache, so DTCM
 * bought nothing while the pointer indirection cost. Keep it a fixed array.
 */
unsigned char ram[1024];

/* the sound chip registers */

unsigned snd_regs[16];
static unsigned snd_select;

/* the via 6522 registers */

static unsigned via_ora;
static unsigned via_orb;
static unsigned via_ddra;
static unsigned via_ddrb;
static unsigned via_t1on;  /* is timer 1 on? */
static unsigned via_t1int; /* are timer 1 interrupts allowed? */
static unsigned via_t1c;
static unsigned via_t1ll;
static unsigned via_t1lh;
static unsigned via_t1pb7; /* timer 1 controlled version of pb7 */
static unsigned via_t2on;  /* is timer 2 on? */
static unsigned via_t2int; /* are timer 2 interrupts allowed? */
static unsigned via_t2c;
static unsigned via_t2ll;
static unsigned via_sr;
static unsigned via_srb;   /* number of bits shifted so far */
static unsigned via_src;   /* shift counter */
static unsigned via_srclk;
static unsigned via_acr;
static unsigned via_pcr;
static unsigned via_ifr;
static unsigned via_ier;
static unsigned via_ca2;
static unsigned via_cb2h;  /* basic handshake version of cb2 */
static unsigned via_cb2s;  /* version of cb2 controlled by the shift register */

/* analog devices */

static unsigned alg_rsh;  /* zero ref sample and hold */
static unsigned alg_xsh;  /* x sample and hold */
static unsigned alg_ysh;  /* y sample and hold */
static unsigned alg_zsh;  /* z sample and hold */
unsigned alg_jch0;		  /* joystick direction channel 0 */
unsigned alg_jch1;		  /* joystick direction channel 1 */
unsigned alg_jch2;		  /* joystick direction channel 2 */
unsigned alg_jch3;		  /* joystick direction channel 3 */
static unsigned alg_jsh;  /* joystick sample and hold */

static unsigned alg_compare;

static long alg_dx;     /* delta x */
static long alg_dy;     /* delta y */
static long alg_curr_x; /* current x position */
static long alg_curr_y; /* current y position */

enum {
	VECTREX_PDECAY	= VECTREX_RENDER_HZ, /* phosphor decay rate */

	/* number of 6809 cycles before a frame redraw */

	FCYCLES_INIT    = VECTREX_MHZ / VECTREX_PDECAY,

	/* max number of possible vectors that maybe on the screen at one time.
	 * one only needs VECTREX_MHZ / VECTREX_PDECAY but we need to also store
	 * deleted vectors in a single table
	 */

	VECTOR_CNT		= VECTREX_VECTOR_CAP,

	VECTOR_HASH     = VECTREX_VECTOR_HASH
};

static unsigned alg_vectoring; /* are we drawing a vector right now? */
static long alg_vector_x0;
static long alg_vector_y0;
static long alg_vector_x1;
static long alg_vector_y1;
static long alg_vector_dx;
static long alg_vector_dy;
static unsigned char alg_vector_color;

long vector_draw_cnt;
long vector_erse_cnt;
static vector_t vectors_set[2 * VECTOR_CNT];
vector_t *vectors_draw;
vector_t *vectors_erse;
unsigned long vecx_emu_cycle_count;
unsigned long vecx_emu_instruction_count;
unsigned long vecx_wait_skip_count;
unsigned long vecx_wait_skip_cycles;
unsigned long vecx_delay_skip_count;
unsigned long vecx_delay_skip_cycles;
unsigned long vecx_sample_total;
unsigned long vecx_sample_opcode_counts[VECX_SAMPLE_OPCODES];
unsigned vecx_sample_pc_addr[VECX_SAMPLE_PC_SLOTS];
unsigned long vecx_sample_pc_count[VECX_SAMPLE_PC_SLOTS];
unsigned long vecx_sample_region[VECX_SAMPLE_REGIONS];
unsigned long vecx_sample_bios_bucket[VECX_SAMPLE_BIOS_BUCKETS];

static int16_t vector_hash[VECTOR_HASH];

static long fcycles;

static einline unsigned vecx_peek8 (unsigned address);

void vecx_sample_reset (void)
{
	vecx_sample_total = 0;
	memset (vecx_sample_opcode_counts, 0, sizeof (vecx_sample_opcode_counts));
	memset (vecx_sample_pc_addr, 0, sizeof (vecx_sample_pc_addr));
	memset (vecx_sample_pc_count, 0, sizeof (vecx_sample_pc_count));
	memset (vecx_sample_region, 0, sizeof (vecx_sample_region));
	memset (vecx_sample_bios_bucket, 0, sizeof (vecx_sample_bios_bucket));
}

static einline void vecx_sample_record (unsigned pc)
{
	unsigned opcode = vecx_peek8 (pc);
	unsigned min_slot = 0;
	unsigned long min_count = vecx_sample_pc_count[0];
	unsigned slot;

	vecx_sample_total++;
	vecx_sample_opcode_counts[opcode & 0xff]++;

	/* HLE scoping: which region is this PC in, and (for BIOS) which routine? */
	{
		unsigned p = pc & 0xffffu;
		if (p < 0x8000u)
			vecx_sample_region[0]++;            /* cart ROM (game code) */
		else if (p >= 0xc800u && p < 0xd000u)
			vecx_sample_region[1]++;            /* Vectrex RAM */
		else if (p >= 0xe000u) {
			vecx_sample_region[2]++;            /* BIOS ROM */
			vecx_sample_bios_bucket[(p - 0xe000u) >> 8]++;
		} else
			vecx_sample_region[3]++;            /* VIA / unmapped / other */
	}

	for (slot = 0; slot < VECX_SAMPLE_PC_SLOTS; slot++) {
		if (vecx_sample_pc_count[slot] == 0) {
			vecx_sample_pc_addr[slot] = pc & 0xffff;
			vecx_sample_pc_count[slot] = 1;
			return;
		}

		if (vecx_sample_pc_addr[slot] == (pc & 0xffff)) {
			vecx_sample_pc_count[slot]++;
			return;
		}

		if (vecx_sample_pc_count[slot] < min_count) {
			min_count = vecx_sample_pc_count[slot];
			min_slot = slot;
		}
	}

	vecx_sample_pc_addr[min_slot] = pc & 0xffff;
	vecx_sample_pc_count[min_slot] = 1;
}

/* HLE of Mine Storm's hot draw loop (Mov_Draw_VL list walk at F410: per 3-byte
 * entry [mode, dy, dx], spins on the T1 timer at F425, loops via F42E, exits to
 * F430 -> $F34F; stroke scale = T1 low latch via_t1ll/$D004). VECX_HLE_CAPTURE
 * shadow-validates the derived geometry; VECX_HLE_ACTIVE intercepts the loop. */
#define HLE_ENTRY_PC 0xf410u
#define HLE_EXIT_PC  0xf430u
#define HLE_PRED_MAX 128

/* globals are always defined so playdate_main's logging/reset link regardless
 * of which HLE mode is compiled in. */
unsigned long vecx_hle_calls, vecx_hle_tot_count, vecx_hle_tot_cyc, vecx_hle_tot_vec;
unsigned vecx_hle_s_count, vecx_hle_s_listx, vecx_hle_s_list[6];
long vecx_hle_s_cyc, vecx_hle_s_vec, vecx_hle_s_sx, vecx_hle_s_sy;
long vecx_hle_s_v[2][5];
int vecx_hle_s_valid;
unsigned long vecx_hle_ok, vecx_hle_cntmiss, vecx_hle_geommiss;
int vecx_hle_mm_valid;
long vecx_hle_mm_idx, vecx_hle_mm_npred, vecx_hle_mm_nreal;
long vecx_hle_mm_p[4], vecx_hle_mm_r[4];
int vecx_hle_enabled = 1;            /* gated by VECX_HLE_ACTIVE (compile flag) */
unsigned long vecx_hle_exec_calls;  /* F410 calls HLE'd this window */
unsigned long vecx_hle_max_scale, vecx_hle_max_ent;  /* diagnostics */

void vecx_hle_reset (void)
{
	vecx_hle_calls = vecx_hle_tot_count = vecx_hle_tot_cyc = vecx_hle_tot_vec = 0;
	vecx_hle_ok = vecx_hle_cntmiss = vecx_hle_geommiss = 0;
	vecx_hle_s_valid = 0;
	vecx_hle_mm_valid = 0;
	vecx_hle_exec_calls = 0;
	vecx_hle_max_scale = 0;
	vecx_hle_max_ent = 0;
}

#if VECX_HLE_CAPTURE
static int hle_active;
static unsigned hle_count0, hle_listx;
static unsigned long hle_cyc0;
static long hle_vcnt0, hle_sx, hle_sy;
/* shadow prediction (derived geometry): x0,y0,x1,y1,color per emitted vector. */
static long hle_pred[HLE_PRED_MAX][5];
static int hle_npred;

/* Predict the vectors the F410 loop will emit, from the snapshot state, using
 * the geometry derived from the ground-truth capture:
 *   per 3-byte entry [mode, dyb, dxb]; terminate when (signed)mode > 0;
 *   dx = dxb_signed * scale, dy = -dyb_signed * scale;
 *   mode==0 -> "move": a dot at the new position; else "draw": a line. */
static void vecx_hle_predict (void)
{
	unsigned x = hle_listx;
	long cx = hle_sx, cy = hle_sy;
	long color = (long) alg_zsh;
	long scale = (long) via_t1ll;

	hle_npred = 0;
	for (;;) {
		int mode = (int) (signed char) vecx_peek8 (x);
		long dyb, dxb, ex, ey;

		if (mode > 0)
			break;                          /* positive mode = list terminator */

		dyb = (long) (signed char) vecx_peek8 (x + 1);
		dxb = (long) (signed char) vecx_peek8 (x + 2);
		x += 3;
		ex = cx + dxb * scale;
		ey = cy - dyb * scale;

		if (hle_npred < HLE_PRED_MAX) {
			long *p = hle_pred[hle_npred];
			if (mode == 0) {                /* move: dot at the new position */
				p[0] = ex; p[1] = ey; p[2] = ex; p[3] = ey;
			} else {                        /* draw: line to the new position */
				p[0] = cx; p[1] = cy; p[2] = ex; p[3] = ey;
			}
			p[4] = color;
		}
		hle_npred++;
		cx = ex; cy = ey;
		if (hle_npred >= HLE_PRED_MAX)
			break;
	}
}

static einline void vecx_hle_capture (unsigned pc, unsigned long cyc)
{
	if (!hle_active) {
		if (pc == HLE_ENTRY_PC && e6809_get_dp () == 0xd0u) {
			hle_active = 1;
			hle_count0 = via_t1ll;            /* stroke scale ($D004 latch) */
			hle_listx = e6809_get_x ();
			hle_cyc0 = cyc;
			hle_vcnt0 = vector_draw_cnt;
			hle_sx = alg_curr_x;
			hle_sy = alg_curr_y;
			vecx_hle_predict ();             /* shadow: predict, do NOT mutate state */
		}
		return;
	}

	if (pc == HLE_EXIT_PC) {
		long dv = vector_draw_cnt - hle_vcnt0;
		long ip = 0, ir = 0;
		int geom_ok = 1;

		hle_active = 0;
		vecx_hle_calls++;
		vecx_hle_tot_count += hle_count0;
		vecx_hle_tot_cyc += (cyc - hle_cyc0);
		vecx_hle_tot_vec += (dv > 0 ? (unsigned long) dv : 0ul);

		/* ---- dual-path diff, comparing only the VISIBLE lines ----
		 * The real alg path emits zero-length junction dots between drawn lines
		 * (invisible). Skip degenerate (x0==x1 && y0==y1) vectors on both sides
		 * and compare the remaining lines two-pointer style. */
		for (;;) {
			vector_t *rv;
			long *p;

			while (ip < hle_npred &&
				   hle_pred[ip][0] == hle_pred[ip][2] &&
				   hle_pred[ip][1] == hle_pred[ip][3])
				ip++;
			while (ir < dv &&
				   vectors_draw[hle_vcnt0 + ir].x0 == vectors_draw[hle_vcnt0 + ir].x1 &&
				   vectors_draw[hle_vcnt0 + ir].y0 == vectors_draw[hle_vcnt0 + ir].y1)
				ir++;

			if (ip >= hle_npred && ir >= dv)
				break;                        /* both exhausted -> match */
			if (ip >= hle_npred || ir >= dv) {
				geom_ok = 0;                  /* one side has extra lines */
				vecx_hle_cntmiss++;
				break;
			}

			p = hle_pred[ip];
			rv = &vectors_draw[hle_vcnt0 + ir];
			if (p[0] != rv->x0 || p[1] != rv->y0 ||
				p[2] != rv->x1 || p[3] != rv->y1) {
				geom_ok = 0;
				if (!vecx_hle_mm_valid) {     /* capture the first mismatch */
					vecx_hle_mm_valid = 1;
					vecx_hle_mm_idx = ip;
					vecx_hle_mm_npred = hle_npred;
					vecx_hle_mm_nreal = dv;
					vecx_hle_mm_p[0] = p[0]; vecx_hle_mm_p[1] = p[1];
					vecx_hle_mm_p[2] = p[2]; vecx_hle_mm_p[3] = p[3];
					vecx_hle_mm_r[0] = rv->x0; vecx_hle_mm_r[1] = rv->y0;
					vecx_hle_mm_r[2] = rv->x1; vecx_hle_mm_r[3] = rv->y1;
				}
				break;
			}
			ip++; ir++;
		}
		if (geom_ok)
			vecx_hle_ok++;
		else
			vecx_hle_geommiss++;

		if (!vecx_hle_s_valid && dv > 0) {
			int k;
			vecx_hle_s_valid = 1;
			vecx_hle_s_count = hle_count0;
			vecx_hle_s_listx = hle_listx;
			vecx_hle_s_cyc = (long) (cyc - hle_cyc0);
			vecx_hle_s_vec = dv;
			vecx_hle_s_sx = hle_sx;
			vecx_hle_s_sy = hle_sy;
			for (k = 0; k < 6; k++)
				vecx_hle_s_list[k] = vecx_peek8 (hle_listx + k);
			for (k = 0; k < 2; k++) {
				long idx = hle_vcnt0 + k;
				if (k < dv) {
					vecx_hle_s_v[k][0] = vectors_draw[idx].x0;
					vecx_hle_s_v[k][1] = vectors_draw[idx].y0;
					vecx_hle_s_v[k][2] = vectors_draw[idx].x1;
					vecx_hle_s_v[k][3] = vectors_draw[idx].y1;
					vecx_hle_s_v[k][4] = vectors_draw[idx].color;
				}
			}
		}
	}
}
#endif

/* update the snd chips internal registers when via_ora/via_orb changes */

static einline void snd_update (void)
{
	switch (via_orb & 0x18) {
	case 0x00:
		/* the sound chip is disabled */
		break;
	case 0x08:
		/* the sound chip is sending data */
		break;
	case 0x10:
		/* the sound chip is recieving data */

		if (snd_select != 14) {
			snd_regs[snd_select] = via_ora;
			e8910_write(snd_select, via_ora);
		}

		break;
	case 0x18:
		/* the sound chip is latching an address */

		if ((via_ora & 0xf0) == 0x00) {
			snd_select = via_ora & 0x0f;
		}

		break;
	}
}

/* update the various analog values when orb is written. */

static einline void alg_update (void)
{
	switch (via_orb & 0x06) {
	case 0x00:
		alg_jsh = alg_jch0;

		if ((via_orb & 0x01) == 0x00) {
			/* demultiplexor is on */
			alg_ysh = alg_xsh;
		}

		break;
	case 0x02:
		alg_jsh = alg_jch1;

		if ((via_orb & 0x01) == 0x00) {
			/* demultiplexor is on */
			alg_rsh = alg_xsh;
		}

		break;
	case 0x04:
		alg_jsh = alg_jch2;

		if ((via_orb & 0x01) == 0x00) {
			/* demultiplexor is on */

			if (alg_xsh > 0x80) {
				alg_zsh = alg_xsh - 0x80;
			} else {
				alg_zsh = 0;
			}
		}

		break;
	case 0x06:
		/* sound output line */
		alg_jsh = alg_jch3;
		break;
	}

	/* compare the current joystick direction with a reference */

	if (alg_jsh > alg_xsh) {
		alg_compare = 0x20;
	} else {
		alg_compare = 0;
	}

	/* compute the new "deltas" */

	alg_dx = (long) alg_xsh - (long) alg_rsh;
	alg_dy = (long) alg_rsh - (long) alg_ysh;
}

/* update IRQ and bit-7 of the ifr register after making an adjustment to
 * ifr.
 */

static einline void int_update (void)
{
	if ((via_ifr & 0x7f) & (via_ier & 0x7f)) {
		via_ifr |= 0x80;
	} else {
		via_ifr &= 0x7f;
	}
}

static einline unsigned char via_read_reg (unsigned reg)
{
	unsigned char data = 0xff;

	switch (reg & 0xf) {
	case 0x0:
		/* compare signal is an input so the value does not come from
		 * via_orb.
		 */

		if (via_acr & 0x80) {
			/* timer 1 has control of bit 7 */

			data = (unsigned char) ((via_orb & 0x5f) | via_t1pb7 | alg_compare);
		} else {
			/* bit 7 is being driven by via_orb */

			data = (unsigned char) ((via_orb & 0xdf) | alg_compare);
		}

		break;
	case 0x1:
		/* register 1 also performs handshakes if necessary */

		if ((via_pcr & 0x0e) == 0x08) {
			/* if ca2 is in pulse mode or handshake mode, then it
			 * goes low whenever ira is read.
			 */

			via_ca2 = 0;
		}

		/* fall through */

	case 0xf:
		if ((via_orb & 0x18) == 0x08) {
			/* the snd chip is driving port a */

			data = (unsigned char) snd_regs[snd_select];
		} else {
			data = (unsigned char) via_ora;
		}

		break;
	case 0x2:
		data = (unsigned char) via_ddrb;
		break;
	case 0x3:
		data = (unsigned char) via_ddra;
		break;
	case 0x4:
		/* T1 low order counter */

		data = (unsigned char) via_t1c;
		via_ifr &= 0xbf; /* remove timer 1 interrupt flag */

		via_t1on = 0; /* timer 1 is stopped */
		via_t1int = 0;
		via_t1pb7 = 0x80;

		int_update ();

		break;
	case 0x5:
		/* T1 high order counter */

		data = (unsigned char) (via_t1c >> 8);

		break;
	case 0x6:
		/* T1 low order latch */

		data = (unsigned char) via_t1ll;
		break;
	case 0x7:
		/* T1 high order latch */

		data = (unsigned char) via_t1lh;
		break;
	case 0x8:
		/* T2 low order counter */

		data = (unsigned char) via_t2c;
		via_ifr &= 0xdf; /* remove timer 2 interrupt flag */

		via_t2on = 0; /* timer 2 is stopped */
		via_t2int = 0;

		int_update ();

		break;
	case 0x9:
		/* T2 high order counter */

		data = (unsigned char) (via_t2c >> 8);
		break;
	case 0xa:
		data = (unsigned char) via_sr;
		via_ifr &= 0xfb; /* remove shift register interrupt flag */
		via_srb = 0;
		via_srclk = 1;

		int_update ();

		break;
	case 0xb:
		data = (unsigned char) via_acr;
		break;
	case 0xc:
		data = (unsigned char) via_pcr;
		break;
	case 0xd:
		/* interrupt flag register */

		data = (unsigned char) via_ifr;
		break;
	case 0xe:
		/* interrupt enable register */

		data = (unsigned char) (via_ier | 0x80);
		break;
	}

	return data;
}

static einline void via_write_reg (unsigned reg, unsigned char data)
{
	switch (reg & 0xf) {
	case 0x0:
		via_orb = data;

		snd_update ();

		alg_update ();

		if ((via_pcr & 0xe0) == 0x80) {
			/* if cb2 is in pulse mode or handshake mode, then it
			 * goes low whenever orb is written.
			 */

			via_cb2h = 0;
		}

		break;
	case 0x1:
		/* register 1 also performs handshakes if necessary */

		if ((via_pcr & 0x0e) == 0x08) {
			/* if ca2 is in pulse mode or handshake mode, then it
			 * goes low whenever ora is written.
			 */

			via_ca2 = 0;
		}

		/* fall through */

	case 0xf:
		via_ora = data;

		snd_update ();

		/* output of port a feeds directly into the dac which then
		 * feeds the x axis sample and hold.
		 */

		alg_xsh = data ^ 0x80;

		alg_update ();

		break;
	case 0x2:
		via_ddrb = data;
		break;
	case 0x3:
		via_ddra = data;
		break;
	case 0x4:
		/* T1 low order counter */

		via_t1ll = data;

		break;
	case 0x5:
		/* T1 high order counter */

		via_t1lh = data;
		via_t1c = (via_t1lh << 8) | via_t1ll;
		via_ifr &= 0xbf; /* remove timer 1 interrupt flag */

		via_t1on = 1; /* timer 1 starts running */
		via_t1int = 1;
		via_t1pb7 = 0;

		int_update ();

		break;
	case 0x6:
		/* T1 low order latch */

		via_t1ll = data;
		break;
	case 0x7:
		/* T1 high order latch */

		via_t1lh = data;
		break;
	case 0x8:
		/* T2 low order latch */

		via_t2ll = data;
		break;
	case 0x9:
		/* T2 high order latch/counter */

		via_t2c = (data << 8) | via_t2ll;
		via_ifr &= 0xdf;

		via_t2on = 1; /* timer 2 starts running */
		via_t2int = 1;

		int_update ();

		break;
	case 0xa:
		via_sr = data;
		via_ifr &= 0xfb; /* remove shift register interrupt flag */
		via_srb = 0;
		via_srclk = 1;

		int_update ();

		break;
	case 0xb:
		via_acr = data;
		break;
	case 0xc:
		via_pcr = data;

		if ((via_pcr & 0x0e) == 0x0c) {
			/* ca2 is outputting low */

			via_ca2 = 0;
		} else {
			/* ca2 is disabled or in pulse mode or is
			 * outputting high.
			 */

			via_ca2 = 1;
		}

		if ((via_pcr & 0xe0) == 0xc0) {
			/* cb2 is outputting low */

			via_cb2h = 0;
		} else {
			/* cb2 is disabled or is in pulse mode or is
			 * outputting high.
			 */

			via_cb2h = 1;
		}

		break;
	case 0xd:
		/* interrupt flag register */

		via_ifr &= ~(data & 0x7f);
		int_update ();

		break;
	case 0xe:
		/* interrupt enable register */

		if (data & 0x80) {
			via_ier |= data & 0x7f;
		} else {
			via_ier &= ~(data & 0x7f);
		}

		int_update ();

		break;
	}
}

VECX_NOINLINE unsigned char vecx_read8 (unsigned address)
{
	unsigned char data = 0xff;

	if ((address & 0xff00) == 0xd000) {
		data = via_read_reg (address);
	} else if ((address & 0xe000) == 0xe000) {
		/* rom */

		data = rom[address & 0x1fff];
	} else if ((address & 0xe000) == 0xc000) {
		if (address & 0x800) {
			/* ram */

			data = ram[address & 0x3ff];
		} else if (address & 0x1000) {
			/* io */

			data = via_read_reg (address);
		}
	} else if (address < 0x8000) {
		/* cartridge */

		data = cart[address];
	} else {
		data = 0xff;
	}

	return data;
}

VECX_NOINLINE void vecx_write8 (unsigned address, unsigned char data)
{
	if ((address & 0xff00) == 0xd000) {
		via_write_reg (address, data);
	} else if ((address & 0xe000) == 0xe000) {
		/* rom */
	} else if ((address & 0xe000) == 0xc000) {
		/* it is possible for both ram and io to be written at the same! */

		if (address & 0x800) {
			ram[address & 0x3ff] = data;
		}

		if (address & 0x1000) {
			via_write_reg (address, data);
		}
	} else if (address < 0x8000) {
		/* cartridge */
	}
}

void vecx_reset (void)
{
	unsigned r;

	/* ram */

	for (r = 0; r < 1024; r++) {
		ram[r] = r & 0xff;
	}

	for (r = 0; r < 16; r++) {
		snd_regs[r] = 0;
		e8910_write(r, 0);
	}

	/* input buttons */

	snd_regs[14] = 0xff;
	e8910_write(14, 0xff);

	snd_select = 0;

	via_ora = 0;
	via_orb = 0;
	via_ddra = 0;
	via_ddrb = 0;
	via_t1on = 0;
	via_t1int = 0;
	via_t1c = 0;
	via_t1ll = 0;
	via_t1lh = 0;
	via_t1pb7 = 0x80;
	via_t2on = 0;
	via_t2int = 0;
	via_t2c = 0;
	via_t2ll = 0;
	via_sr = 0;
	via_srb = 8;
	via_src = 0;
	via_srclk = 0;
	via_acr = 0;
	via_pcr = 0;
	via_ifr = 0;
	via_ier = 0;
	via_ca2 = 1;
	via_cb2h = 1;
	via_cb2s = 0;

	alg_rsh = 128;
	alg_xsh = 128;
	alg_ysh = 128;
	alg_zsh = 0;
	alg_jch0 = 128;
	alg_jch1 = 128;
	alg_jch2 = 128;
	alg_jch3 = 128;
	alg_jsh = 128;

	alg_compare = 0; /* check this */

	alg_dx = 0;
	alg_dy = 0;
	alg_curr_x = ALG_MAX_X / 2;
	alg_curr_y = ALG_MAX_Y / 2;

	alg_vectoring = 0;

	vector_draw_cnt = 0;
	vector_erse_cnt = 0;
	vectors_draw = vectors_set;
	vectors_erse = vectors_set + VECTOR_CNT;

	fcycles = FCYCLES_INIT;

	e6809_reset ();
}

/* perform a single cycle worth of via emulation.
 * via_sstep0 is the first postion of the emulation.
 */

static einline void via_shift_sstep (void)
{
	unsigned t2shift;

	via_src--;

	if ((via_src & 0xff) == 0xff) {
		via_src = via_t2ll;

		if (via_srclk) {
			t2shift = 1;
			via_srclk = 0;
		} else {
			t2shift = 0;
			via_srclk = 1;
		}
	} else {
		t2shift = 0;
	}

	if (via_srb < 8) {
		switch (via_acr & 0x1c) {
		case 0x00:
			/* disabled */
			break;
		case 0x04:
			/* shift in under control of t2 */

			if (t2shift) {
				/* shifting in 0s since cb2 is always an output */

				via_sr <<= 1;
				via_srb++;
			}

			break;
		case 0x08:
			/* shift in under system clk control */

			via_sr <<= 1;
			via_srb++;

			break;
		case 0x0c:
			/* shift in under cb1 control */
			break;
		case 0x10:
			/* shift out under t2 control (free run) */

			if (t2shift) {
				via_cb2s = (via_sr >> 7) & 1;

				via_sr <<= 1;
				via_sr |= via_cb2s;
			}

			break;
		case 0x14:
			/* shift out under t2 control */

			if (t2shift) {
				via_cb2s = (via_sr >> 7) & 1;

				via_sr <<= 1;
				via_sr |= via_cb2s;
				via_srb++;
			}

			break;
		case 0x18:
			/* shift out under system clock control */

			via_cb2s = (via_sr >> 7) & 1;

			via_sr <<= 1;
			via_sr |= via_cb2s;
			via_srb++;

			break;
		case 0x1c:
			/* shift out under cb1 control */
			break;
		}

		if (via_srb == 8) {
			via_ifr |= 0x04;
			int_update ();
		}
	}
}

static einline void via_sstep0 (void)
{
	if (via_t1on) {
		via_t1c--;

		if ((via_t1c & 0xffff) == 0xffff) {
			/* counter just rolled over */

			if (via_acr & 0x40) {
				/* continuous interrupt mode */

				via_ifr |= 0x40;
				int_update ();
				via_t1pb7 = 0x80 - via_t1pb7;

				/* reload counter */

				via_t1c = (via_t1lh << 8) | via_t1ll;
			} else {
				/* one shot mode */

				if (via_t1int) {
					via_ifr |= 0x40;
					int_update ();
					via_t1pb7 = 0x80;
					via_t1int = 0;
				}
			}
		}
	}

	if (via_t2on && (via_acr & 0x20) == 0x00) {
		via_t2c--;

		if ((via_t2c & 0xffff) == 0xffff) {
			/* one shot mode */

			if (via_t2int) {
				via_ifr |= 0x20;
				int_update ();
				via_t2int = 0;
			}
		}
	}

	via_shift_sstep ();
}

/* perform the second part of the via emulation */

static einline void via_sstep1 (void)
{
	if ((via_pcr & 0x0e) == 0x0a) {
		/* if ca2 is in pulse mode, then make sure
		 * it gets restored to '1' after the pulse.
		 */

		via_ca2 = 1;
	}

	if ((via_pcr & 0xe0) == 0xa0) {
		/* if cb2 is in pulse mode, then make sure
		 * it gets restored to '1' after the pulse.
		 */

		via_cb2h = 1;
	}
}

static einline void via_sstep0_batch (unsigned cycles)
{
	unsigned shift_cycles;
	unsigned shift_mode;

	if (via_t1on) {
		unsigned remaining = cycles;

		while (remaining > 0) {
			unsigned step = (via_t1c & 0xffff) + 1;

			if (remaining < step) {
				via_t1c -= remaining;
				break;
			}

			remaining -= step;

			if (via_acr & 0x40) {
				via_ifr |= 0x40;
				int_update ();
				via_t1pb7 = 0x80 - via_t1pb7;
				via_t1c = (via_t1lh << 8) | via_t1ll;
			} else {
				via_t1c -= step;
				if (via_t1int) {
					via_ifr |= 0x40;
					int_update ();
					via_t1pb7 = 0x80;
					via_t1int = 0;
				}

				if (remaining > 0) {
					via_t1c -= remaining;
					break;
				}
			}
		}
	}

	if (via_t2on && (via_acr & 0x20) == 0x00) {
		unsigned step = (via_t2c & 0xffff) + 1;

		via_t2c -= cycles;

		if (cycles >= step && via_t2int) {
			via_ifr |= 0x20;
			int_update ();
			via_t2int = 0;
		}
	}

	if (via_srb >= 8) {
		return;
	}

	shift_mode = via_acr & 0x1c;

	if (shift_mode == 0x00 || shift_mode == 0x0c || shift_mode == 0x1c) {
		return;
	}

	if (shift_mode == 0x08) {
		shift_cycles = cycles;
		if (shift_cycles > 8 - via_srb) {
			shift_cycles = 8 - via_srb;
		}
		via_sr <<= shift_cycles;
		via_srb += shift_cycles;
		if (via_srb == 8) {
			via_ifr |= 0x04;
			int_update ();
		}
		return;
	}

	if (shift_mode == 0x18) {
		shift_cycles = cycles;
		if (shift_cycles > 8 - via_srb) {
			shift_cycles = 8 - via_srb;
		}
		while (shift_cycles-- > 0) {
			via_cb2s = (via_sr >> 7) & 1;
			via_sr <<= 1;
			via_sr |= via_cb2s;
			via_srb++;
		}
		if (via_srb == 8) {
			via_ifr |= 0x04;
			int_update ();
		}
		return;
	}

	shift_cycles = cycles;
	while (shift_cycles-- > 0 && via_srb < 8) {
		via_shift_sstep ();
	}
}

static einline void alg_addline (long x0, long y0, long x1, long y1, unsigned char color)
{
	unsigned long key;
	long index;

	key = (unsigned long) x0;
	key = key * 31 + (unsigned long) y0;
	key = key * 31 + (unsigned long) x1;
	key = key * 31 + (unsigned long) y1;
	key %= VECTOR_HASH;

	/* first check if the line to be drawn is in the current draw list.
	 * if it is, then it is not added again.
	 */

	index = vector_hash[key];

	if (index >= 0 && index < vector_draw_cnt &&
		x0 == vectors_draw[index].x0 &&
		y0 == vectors_draw[index].y0 &&
		x1 == vectors_draw[index].x1 &&
		y1 == vectors_draw[index].y1) {
		vectors_draw[index].color = color;
	} else {
		/* missed on the draw list, now check if the line to be drawn is in
		 * the erase list ... if it is, "invalidate" it on the erase list.
		 */

		if (index >= 0 && index < vector_erse_cnt &&
			x0 == vectors_erse[index].x0 &&
			y0 == vectors_erse[index].y0 &&
			x1 == vectors_erse[index].x1 &&
			y1 == vectors_erse[index].y1) {
			vectors_erse[index].color = VECTREX_COLORS;
		}

		if (vector_draw_cnt >= VECTOR_CNT) {
			return;
		}

		vectors_draw[vector_draw_cnt].x0 = x0;
		vectors_draw[vector_draw_cnt].y0 = y0;
		vectors_draw[vector_draw_cnt].x1 = x1;
		vectors_draw[vector_draw_cnt].y1 = y1;
		vectors_draw[vector_draw_cnt].color = color;
		vector_hash[key] = vector_draw_cnt;
		vector_draw_cnt++;
	}
}

/* perform a single cycle worth of analog emulation */

static einline void alg_sstep (void)
{
	long sig_dx, sig_dy;
	unsigned sig_ramp;
	unsigned sig_blank;

	if ((via_acr & 0x10) == 0x10) {
		sig_blank = via_cb2s;
	} else {
		sig_blank = via_cb2h;
	}

	if (via_ca2 == 0) {
		/* need to force the current point to the 'orgin' so just
		 * calculate distance to origin and use that as dx,dy.
		 */

		sig_dx = ALG_MAX_X / 2 - alg_curr_x;
		sig_dy = ALG_MAX_Y / 2 - alg_curr_y;
	} else {
		if (via_acr & 0x80) {
			sig_ramp = via_t1pb7;
		} else {
			sig_ramp = via_orb & 0x80;
		}

		if (sig_ramp == 0) {
			sig_dx = alg_dx;
			sig_dy = alg_dy;
		} else {
			sig_dx = 0;
			sig_dy = 0;
		}
	}

	if (alg_vectoring == 0) {
		if (sig_blank == 1 &&
			alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
			alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {

			/* start a new vector */

			alg_vectoring = 1;
			alg_vector_x0 = alg_curr_x;
			alg_vector_y0 = alg_curr_y;
			alg_vector_x1 = alg_curr_x;
			alg_vector_y1 = alg_curr_y;
			alg_vector_dx = sig_dx;
			alg_vector_dy = sig_dy;
			alg_vector_color = (unsigned char) alg_zsh;
		}
	} else {
		/* already drawing a vector ... check if we need to turn it off */

		if (sig_blank == 0) {
			/* blank just went on, vectoring turns off, and we've got a
			 * new line.
			 */

			alg_vectoring = 0;

			alg_addline (alg_vector_x0, alg_vector_y0,
						 alg_vector_x1, alg_vector_y1,
						 alg_vector_color);
		} else if (sig_dx != alg_vector_dx ||
				   sig_dy != alg_vector_dy ||
				   (unsigned char) alg_zsh != alg_vector_color) {

			/* the parameters of the vectoring processing has changed.
			 * so end the current line.
			 */

			alg_addline (alg_vector_x0, alg_vector_y0,
						 alg_vector_x1, alg_vector_y1,
						 alg_vector_color);

			/* we continue vectoring with a new set of parameters if the
			 * current point is not out of limits.
			 */

			if (alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
				alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {
				alg_vector_x0 = alg_curr_x;
				alg_vector_y0 = alg_curr_y;
				alg_vector_x1 = alg_curr_x;
				alg_vector_y1 = alg_curr_y;
				alg_vector_dx = sig_dx;
				alg_vector_dy = sig_dy;
				alg_vector_color = (unsigned char) alg_zsh;
			} else {
				alg_vectoring = 0;
			}
		}
	}

	alg_curr_x += sig_dx;
	alg_curr_y += sig_dy;

	if (alg_vectoring == 1 &&
		alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
		alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {

		/* we're vectoring ... current point is still within limits so
		 * extend the current vector.
		 */

		alg_vector_x1 = alg_curr_x;
		alg_vector_y1 = alg_curr_y;
	}
}

static einline void alg_sstep_batch (unsigned cycles)
{
	long sig_dx, sig_dy;
	unsigned sig_ramp;
	unsigned sig_blank;
	unsigned move_cycles = cycles;

	if (cycles == 0) {
		return;
	}

	if ((via_acr & 0x10) == 0x10) {
		sig_blank = via_cb2s;
	} else {
		sig_blank = via_cb2h;
	}

	if (via_ca2 == 0) {
		sig_dx = ALG_MAX_X / 2 - alg_curr_x;
		sig_dy = ALG_MAX_Y / 2 - alg_curr_y;
		move_cycles = 1;
	} else {
		if (via_acr & 0x80) {
			sig_ramp = via_t1pb7;
		} else {
			sig_ramp = via_orb & 0x80;
		}

		if (sig_ramp == 0) {
			sig_dx = alg_dx;
			sig_dy = alg_dy;
		} else {
			sig_dx = 0;
			sig_dy = 0;
		}
	}

	if (alg_vectoring == 0) {
		if (sig_blank == 1 &&
			alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
			alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {
			alg_vectoring = 1;
			alg_vector_x0 = alg_curr_x;
			alg_vector_y0 = alg_curr_y;
			alg_vector_x1 = alg_curr_x;
			alg_vector_y1 = alg_curr_y;
			alg_vector_dx = sig_dx;
			alg_vector_dy = sig_dy;
			alg_vector_color = (unsigned char) alg_zsh;
		}
	} else {
		if (sig_blank == 0) {
			alg_vectoring = 0;
			alg_addline (alg_vector_x0, alg_vector_y0,
						 alg_vector_x1, alg_vector_y1,
						 alg_vector_color);
		} else if (sig_dx != alg_vector_dx ||
				   sig_dy != alg_vector_dy ||
				   (unsigned char) alg_zsh != alg_vector_color) {
			alg_addline (alg_vector_x0, alg_vector_y0,
						 alg_vector_x1, alg_vector_y1,
						 alg_vector_color);

			if (alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
				alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {
				alg_vector_x0 = alg_curr_x;
				alg_vector_y0 = alg_curr_y;
				alg_vector_x1 = alg_curr_x;
				alg_vector_y1 = alg_curr_y;
				alg_vector_dx = sig_dx;
				alg_vector_dy = sig_dy;
				alg_vector_color = (unsigned char) alg_zsh;
			} else {
				alg_vectoring = 0;
			}
		}
	}

	alg_curr_x += sig_dx * (long) move_cycles;
	alg_curr_y += sig_dy * (long) move_cycles;

	if (alg_vectoring == 1 &&
		alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
		alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) {
		alg_vector_x1 = alg_curr_x;
		alg_vector_y1 = alg_curr_y;
	}
}

static einline unsigned vecx_peek8 (unsigned address)
{
	address &= 0xffff;

	if ((address & 0xe000) == 0xe000) {
		return rom[address & 0x1fff];
	}

	if (address < 0x8000) {
		return cart[address];
	}

	if ((address & 0xe800) == 0xc800) {
		return ram[address & 0x3ff];
	}

	return 0xff;
}

static einline void vecx_finish_vector_frame (void)
{
	vector_t *tmp;

	fcycles += FCYCLES_INIT;
	osint_render ();

	vector_erse_cnt = vector_draw_cnt;
	vector_draw_cnt = 0;

	tmp = vectors_erse;
	vectors_erse = vectors_draw;
	vectors_draw = tmp;
}

static einline void vecx_machine_advance (unsigned cycles)
{
	while (cycles > 0) {
		unsigned step = cycles;

		if (fcycles >= 0 && (long) step > fcycles) {
			step = (unsigned) fcycles + 1;
		}

#if VECX_FAST_BATCH
		/* Keep the batched beam ramp constant: don't step past the next Timer-1
		 * PB7 toggle, which flips the ramp (= stroke length). Capping here is what
		 * lets FAST_BATCH stay font-correct. The T1 period (a stroke) is far longer
		 * than an instruction, so this rarely shortens a batch. */
		if (via_t1on && (via_acr & 0x80) && ((via_acr & 0x40) || via_t1int)) {
			unsigned t1_event = (via_t1c & 0xffff);

			if (t1_event == 0)
				t1_event = 1; /* expiry happens on the next cycle; take it */
			if (step > t1_event)
				step = t1_event;
		}

		/* While the shift register is clocking out the CB2 blank (via_acr & 0x10,
		 * via_srb < 8), the beam blank can flip every shift; step one cycle at a
		 * time through that window so the batched beam sees the right blank. (Trying
		 * to batch inside the window by predicting the next blank flip regressed:
		 * Mine Storm's pattern flips too often, so the prediction cost outweighs the
		 * batched cycles. The SR is idle/batchable most of the time anyway.) */
		if ((via_acr & 0x10) && via_srb < 8)
			step = 1;

		via_sstep0_batch (step);
		alg_sstep_batch (step);
		via_sstep1 ();
#else
		{
			unsigned c;

			for (c = 0; c < step; c++) {
				via_sstep0 ();
				alg_sstep ();
				via_sstep1 ();
			}
		}
#endif

		fcycles -= (long) step;
		if (fcycles < 0) {
			vecx_finish_vector_frame ();
		}

		cycles -= step;
	}
}

#if VECX_WAIT_LOOP_SKIP || VECX_BIOS_DELAY_SKIP
static einline void vecx_add_wait_candidate (unsigned *best, unsigned cycles)
{
	if (cycles == 0) {
		return;
	}

	if (*best == 0 || cycles < *best) {
		*best = cycles;
	}
}

static einline unsigned vecx_cycles_until_ifr_mask (unsigned poll_mask)
{
	unsigned mask = poll_mask & 0xff;
	unsigned best = 0;

	if (mask & 0x80) {
		mask |= via_ier & 0x7f;
	}

	if ((mask & 0x40) &&
		(via_ifr & 0x40) == 0 &&
		via_t1on &&
		((via_acr & 0x40) || via_t1int)) {
		vecx_add_wait_candidate (&best, (via_t1c & 0xffff) + 1);
	}

	if ((mask & 0x20) &&
		(via_ifr & 0x20) == 0 &&
		via_t2on &&
		via_t2int &&
		(via_acr & 0x20) == 0) {
		vecx_add_wait_candidate (&best, (via_t2c & 0xffff) + 1);
	}

	return best;
}
#endif

#if VECX_WAIT_LOOP_SKIP
static einline unsigned vecx_try_skip_ifr_wait (long remaining_cycles)
{
	unsigned pc;
	unsigned poll_mask;
	unsigned wait_cycles;

	if (remaining_cycles <= 0) {
		return 0;
	}

	pc = e6809_get_pc ();
	if (pc != 0xf19e || e6809_get_dp () != 0xd0) {
		return 0;
	}

	poll_mask = e6809_get_a () & 0xff;
	if (poll_mask == 0 || (via_ifr & poll_mask) != 0) {
		return 0;
	}

	wait_cycles = vecx_cycles_until_ifr_mask (poll_mask);
	if (wait_cycles < 8) {
		return 0;
	}

	if (wait_cycles > (unsigned) remaining_cycles) {
		wait_cycles = (unsigned) remaining_cycles;
	}

	return wait_cycles;
}
#endif

#if VECX_BIOS_DELAY_SKIP
static einline unsigned vecx_try_skip_bios_delay (long remaining_cycles)
{
	unsigned limit;

	if (remaining_cycles <= 0 || e6809_get_pc () != 0xf4eb) {
		return 0;
	}

	if ((via_ifr & 0x80) != 0) {
		return 0;
	}

	if ((via_ier & 0x04) != 0 && via_srb < 8) {
		return 0;
	}

	limit = (unsigned) remaining_cycles;
	{
		unsigned irq_cycles = vecx_cycles_until_ifr_mask (0x80);
		if (irq_cycles != 0 && irq_cycles <= limit) {
			limit = irq_cycles - 1;
		}
	}

	return e6809_skip_bios_delay_f4eb (limit);
}

#endif

#if VECX_HLE_ACTIVE
/* Active HLE of the Mov_Draw_VL list loop at F410. Emit the vectors directly
 * from the geometry proven bit-exact by the shadow validator (dx = dx*scale,
 * dy = -dy*scale, scale = via_t1ll), so there's no dependence on the analog
 * sample-and-hold timing (which made the earlier replicate-the-VIA approach
 * glitch). Bounds-check first and fall back to the real routine for any vector
 * that leaves the screen box (those need alg's exact per-cycle clipping). Then
 * advance the VIA timers / sound / frame by an estimated cycle count with the
 * beam blanked so nothing re-emits. Returns cycles consumed; PC/X advance to the
 * F430 exit. Returns 0 (no HLE) to decline -- the real routine then runs. */
static unsigned vecx_hle_f410_exec (void)
{
	unsigned x0 = e6809_get_x ();
	long scale = (long) via_t1ll;
	long sx = alg_curr_x, sy = alg_curr_y, color = (long) alg_zsh;
	unsigned x, total = 0;
	int guard;
	long cx, cy;

	if (scale == 0)
		return 0;                           /* degenerate; let the real routine run */
	if (sx < 0 || sx >= ALG_MAX_X || sy < 0 || sy >= ALG_MAX_Y)
		return 0;                           /* beam off-screen; real routine clips */

	/* The BIOS loop ALWAYS processes the first entry (it's entered at F410), then
	 * checks each SUBSEQUENT entry's mode for the terminator (F42E BLE: continue
	 * while the next mode <= 0). So the structure is process-entry, then peek-next:
	 * stop when the next mode > 0 (that terminator entry is not drawn). */

	/* pass 1: bounds-check; decline (-> real routine) if any endpoint is OOB. */
	x = x0; cx = sx; cy = sy; guard = 0;
	for (;;) {
		long ex = cx + (long) (signed char) vecx_peek8 (x + 2) * scale;
		long ey = cy - (long) (signed char) vecx_peek8 (x + 1) * scale;

		x += 3;
		if (ex < 0 || ex >= ALG_MAX_X || ey < 0 || ey >= ALG_MAX_Y)
			return 0;
		cx = ex; cy = ey;
		if ((int) (signed char) vecx_peek8 (x) > 0)
			break;                          /* next entry is the terminator */
		if (++guard > 48)
			return 0;                       /* unusually long list -> real routine */
	}

	/* If this call's cycles would cross a frame boundary, decline (-> real
	 * routine) so vecx_finish_vector_frame / osint_render never fires from inside
	 * the intercept. guard+1 ~ entry count; per entry ~ setup(65) + spin(scale). */
	if (fcycles < (long) ((guard + 1) * (scale + 80)))
		return 0;

	/* pass 2: emit the exact predicted lines (mode==0 is a blanked move). */
	x = x0; cx = sx; cy = sy; guard = 0;
	for (;;) {
		int mode = (int) (signed char) vecx_peek8 (x);
		long ex = cx + (long) (signed char) vecx_peek8 (x + 2) * scale;
		long ey = cy - (long) (signed char) vecx_peek8 (x + 1) * scale;

		x += 3;
		if (mode != 0)
			alg_addline (cx, cy, ex, ey, (unsigned char) color);
		cx = ex; cy = ey;
		if ((int) (signed char) vecx_peek8 (x) > 0)
			break;
		if (++guard > 48)
			break;
	}
	alg_curr_x = cx; alg_curr_y = cy;
	alg_vectoring = 0;
	if ((unsigned long) scale > vecx_hle_max_scale)
		vecx_hle_max_scale = (unsigned long) scale;
	if ((unsigned long) (guard + 1) > vecx_hle_max_ent)
		vecx_hle_max_ent = (unsigned long) (guard + 1);

	/* pass 3: timing. The real loop RESTARTS Timer-1 every entry (CLR $D005),
	 * so leaving T1 mid-countdown desyncs the BIOS frame logic. Replicate the
	 * per-entry restart so T1 ends exactly where the routine would. Force the
	 * blank signals + shift register idle so machine_advance emits nothing (no
	 * need to touch $D00A, which would re-clock the SR); restore them after. */
	{
		unsigned s_cb2s = via_cb2s, s_cb2h = via_cb2h, s_srb = via_srb;

		via_cb2s = 0; via_cb2h = 0; via_srb = 8;   /* blanked, SR idle */
		x = x0; guard = 0;
		for (;;) {
			unsigned dur;

			x += 3;
			/* Account for the ~65 cycles of per-entry setup instructions the real
			 * loop runs (LDD/STA/CLR/.../BLE) BEFORE the timer spin -- otherwise the
			 * cycle total runs short and the BIOS frame timer desyncs, eventually
			 * freezing the game (watchdog). Advance them with the beam blanked. */
			vecx_machine_advance (65u);
			total += 65u;
			vecx_write8 (0xd005u, 0x00u);   /* restart T1 from its latch */
			dur = vecx_cycles_until_ifr_mask (0x40u);
			if (dur == 0u)
				dur = 1u;
			vecx_machine_advance (dur);
			total += dur;
			if ((int) (signed char) vecx_peek8 (x) > 0)
				break;
			if (++guard > 100)
				break;
		}
		via_cb2s = s_cb2s; via_cb2h = s_cb2h; via_srb = s_srb;
	}
	alg_curr_x = cx; alg_curr_y = cy;       /* undo any blanked-beam drift */
	alg_vectoring = 0;

	e6809_set_x (x);                        /* X points at the terminator entry */
	e6809_set_pc (0xf430u);                 /* resume at F430 (JMP $F34F) */
	vecx_hle_exec_calls++;
	return total;
}
#endif

void vecx_emu (long cycles)
{
	unsigned icycles;
	unsigned long cycle_count = 0;
	unsigned long instruction_count = 0;
	unsigned long wait_skip_count = 0;
	unsigned long wait_skip_cycles = 0;
	unsigned long delay_skip_count = 0;
	unsigned long delay_skip_cycles = 0;
#if VECX_MACHINE_ADVANCE_BATCH > 1
	unsigned pending_cycles = 0;
	unsigned pending_instructions = 0;
#endif

	while (cycles > 0) {
#if VECX_WAIT_LOOP_SKIP
		unsigned skip_cycles = 0;

#if VECX_MACHINE_ADVANCE_BATCH > 1
		if (pending_cycles == 0) {
			skip_cycles = vecx_try_skip_ifr_wait (cycles);
		}
#else
		/* Probe EVERY instruction: gating it (every 8th) measured -6.5% because the
		 * Wait_Recal IFR-poll loop is often short and the missed short waits cost
		 * far more than the per-instruction e6809_get_pc() probe. Leave it as-is. */
		skip_cycles = vecx_try_skip_ifr_wait (cycles);
#endif

		if (skip_cycles > 0) {
			vecx_machine_advance (skip_cycles);
			cycle_count += skip_cycles;
			wait_skip_cycles += skip_cycles;
			wait_skip_count++;
			cycles -= (long) skip_cycles;
			continue;
		}
#endif

#if VECX_BIOS_DELAY_SKIP
		{
			unsigned delay_cycles = vecx_try_skip_bios_delay (cycles);

			if (delay_cycles > 0) {
				vecx_machine_advance (delay_cycles);
				cycle_count += delay_cycles;
				delay_skip_cycles += delay_cycles;
				delay_skip_count++;
				cycles -= (long) delay_cycles;
				continue;
			}
		}
#endif

#if VECX_SAMPLE_PROFILE
		if ((instruction_count & (VECX_SAMPLE_INTERVAL - 1U)) == 0) {
			vecx_sample_record (e6809_get_pc ());
		}
#endif

#if VECX_HLE_CAPTURE
		vecx_hle_capture (e6809_get_pc (), cycle_count);
#endif

#if VECX_HLE_ACTIVE
		if (vecx_hle_enabled && e6809_get_pc () == 0xf410u &&
			e6809_get_dp () == 0xd0u) {
			unsigned hc = vecx_hle_f410_exec ();
			cycle_count += hc;
			cycles -= (long) hc;
			continue;
		}
#endif

		icycles = e6809_hotcore_p (via_ifr & 0x80, 0);
		instruction_count++;
		cycle_count += icycles;

#if VECX_MACHINE_ADVANCE_BATCH > 1
		pending_cycles += icycles;
		pending_instructions++;
		if (pending_instructions >= VECX_MACHINE_ADVANCE_BATCH || cycles <= (long) icycles) {
			vecx_machine_advance (pending_cycles);
			pending_cycles = 0;
			pending_instructions = 0;
		}
#else
		vecx_machine_advance (icycles);
#endif
		cycles -= (long) icycles;
	}

#if VECX_MACHINE_ADVANCE_BATCH > 1
	if (pending_cycles > 0) {
		vecx_machine_advance (pending_cycles);
	}
#endif

	vecx_emu_cycle_count = cycle_count;
	vecx_emu_instruction_count = instruction_count;
	vecx_wait_skip_count = wait_skip_count;
	vecx_wait_skip_cycles = wait_skip_cycles;
	vecx_delay_skip_count = delay_skip_count;
	vecx_delay_skip_cycles = delay_skip_cycles;
}
