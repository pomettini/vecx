#include <stdint.h>
#include <string.h>

#include "pd_api.h"
#include "jit.h"

/* 6809 dynamic recompiler.
 *
 * M1: runtime ARM codegen works (emit + clearICache + call). Buffer is in PSRAM.
 * M2: Thumb emitter + load/modify/store primitives.
 * M3: translate a representative 6809 kernel (loads/stores/AND with N/Z flags) to
 *     a native block, verify it matches a switch interpreter, then time both for a
 *     go/no-go speedup. Both run from PSRAM so it isolates the JIT's structural win
 *     (no per-op fetch/decode/dispatch) from memory speed.
 */

static uint16_t jit_code[512] __attribute__((aligned(8)));

typedef struct {
	uint16_t *code;
	unsigned  len;
	unsigned  cap;
} jit_buf;

static void emit16(jit_buf *b, unsigned hw)
{
	if (b->len < b->cap)
		b->code[b->len] = (uint16_t)hw;
	b->len++;
}

/* Thumb-1, low registers r0-r7 */
static void emit_movs_imm8(jit_buf *b, unsigned rd, unsigned imm8)
{ emit16(b, 0x2000u | (rd << 8) | (imm8 & 0xffu)); }
static void emit_ldrb_imm(jit_buf *b, unsigned rt, unsigned rn, unsigned imm5)
{ emit16(b, 0x7800u | ((imm5 & 0x1fu) << 6) | (rn << 3) | rt); }
static void emit_strb_imm(jit_buf *b, unsigned rt, unsigned rn, unsigned imm5)
{ emit16(b, 0x7000u | ((imm5 & 0x1fu) << 6) | (rn << 3) | rt); }
static void emit_adds_imm3(jit_buf *b, unsigned rd, unsigned rn, unsigned imm3)
{ emit16(b, 0x1c00u | ((imm3 & 7u) << 6) | (rn << 3) | rd); }
static void emit_subs_imm3(jit_buf *b, unsigned rd, unsigned rn, unsigned imm3)
{ emit16(b, 0x1e00u | ((imm3 & 7u) << 6) | (rn << 3) | rd); }
static void emit_lsr_imm5(jit_buf *b, unsigned rd, unsigned rm, unsigned imm5)
{ emit16(b, 0x0800u | ((imm5 & 0x1fu) << 6) | (rm << 3) | rd); }
static void emit_lsl_imm5(jit_buf *b, unsigned rd, unsigned rm, unsigned imm5)
{ emit16(b, 0x0000u | ((imm5 & 0x1fu) << 6) | (rm << 3) | rd); }
static void emit_ands(jit_buf *b, unsigned rd, unsigned rm)
{ emit16(b, 0x4000u | (rm << 3) | rd); }
static void emit_orrs(jit_buf *b, unsigned rd, unsigned rm)
{ emit16(b, 0x4300u | (rm << 3) | rd); }
static void emit_bics(jit_buf *b, unsigned rd, unsigned rm)
{ emit16(b, 0x4380u | (rm << 3) | rd); }
static void emit_eors(jit_buf *b, unsigned rd, unsigned rm)
{ emit16(b, 0x4040u | (rm << 3) | rd); }
static void emit_adds_reg(jit_buf *b, unsigned rd, unsigned rn, unsigned rm)
{ emit16(b, 0x1800u | (rm << 6) | (rn << 3) | rd); }
static void emit_mov_reg(jit_buf *b, unsigned rd, unsigned rm)
{ emit16(b, 0x0000u | (rm << 3) | rd); } /* lsls rd, rm, #0 */
static void emit_bx_lr(jit_buf *b)
{ emit16(b, 0x4770u); }
static void emit_push_r456_lr(jit_buf *b)
{ emit16(b, 0xb570u); }        /* push {r4,r5,r6,lr} */
static void emit_pop_r456_pc(jit_buf *b)
{ emit16(b, 0xbd70u); }        /* pop  {r4,r5,r6,pc}  (returns) */

static void *jit_finalize(PlaydateAPI *pd, jit_buf *b)
{
	pd->system->clearICache();
	return (void *)(((uintptr_t)b->code) | 1u);
}

/* ----------------------------------------------------------------- M3 kernel */

#define KZ 0x04u
#define KN 0x08u

typedef struct {
	uint8_t a, b, cc, pad;
	uint8_t mem[256];
} kcpu; /* a@0 b@1 cc@2 mem@4 */

static const uint8_t kprog[] = {
	0x86, 0x5a,   /* LDA  #0x5a */
	0x97, 0x10,   /* STA  $10   */
	0xd6, 0x10,   /* LDB  $10   */
	0xc4, 0x0f,   /* ANDB #0x0f */
	0xd7, 0x11,   /* STB  $11   */
	0x96, 0x11,   /* LDA  $11   */
	0x85, 0x08,   /* BITA #0x08 */
};

static void ksetnz(kcpu *c, unsigned v)
{
	c->cc = (uint8_t)((c->cc & ~(KZ | KN)) | (v ? 0u : KZ) | ((v & 0x80u) ? KN : 0u));
}

static void kinterp(kcpu *c)
{
	unsigned pc = 0;

	while (pc < sizeof(kprog)) {
		unsigned op = kprog[pc++];

		switch (op) {
		case 0x86: c->a = kprog[pc++]; ksetnz(c, c->a); break;
		case 0x97: c->mem[kprog[pc++]] = c->a; ksetnz(c, c->a); break;
		case 0xd6: c->b = c->mem[kprog[pc++]]; ksetnz(c, c->b); break;
		case 0xc4: c->b &= kprog[pc++]; ksetnz(c, c->b); break;
		case 0xd7: c->mem[kprog[pc++]] = c->b; ksetnz(c, c->b); break;
		case 0x96: c->a = c->mem[kprog[pc++]]; ksetnz(c, c->a); break;
		case 0x85: ksetnz(c, c->a & kprog[pc++]); break;
		default: return;
		}
	}
}

/* set N/Z in r3(cc) from value reg rv (rv must NOT be r4/r5); clobbers r4,r5 */
static void emit_setnz(jit_buf *b, unsigned rv)
{
	emit_lsr_imm5(b, 4, rv, 4);   /* r4 = rv >> 4                 */
	emit_movs_imm8(b, 5, 0x08u);  /* r5 = 0x08                    */
	emit_ands(b, 4, 5);           /* r4 = (rv>>4)&8 = N bit       */
	emit_subs_imm3(b, 5, rv, 1);  /* r5 = rv - 1                  */
	emit_lsr_imm5(b, 5, 5, 31);   /* r5 = (rv==0) ? 1 : 0         */
	emit_lsl_imm5(b, 5, 5, 2);    /* r5 = Z bit (0x04 or 0)       */
	emit_orrs(b, 4, 5);           /* r4 = N | Z                   */
	emit_movs_imm8(b, 5, 0x0cu);  /* r5 = 0x0c                    */
	emit_bics(b, 3, 5);           /* r3 &= ~0x0c                  */
	emit_orrs(b, 3, 4);           /* r3 |= N|Z                    */
}

/* r0=state, r1=a, r2=b, r3=cc, r4/r5=setnz temps, r6=BITA temp */
static void jit_translate_kernel(jit_buf *b)
{
	unsigned pc = 0;

	emit_push_r456_lr(b);
	emit_ldrb_imm(b, 1, 0, 0);
	emit_ldrb_imm(b, 2, 0, 1);
	emit_ldrb_imm(b, 3, 0, 2);

	while (pc < sizeof(kprog)) {
		unsigned op = kprog[pc++];

		switch (op) {
		case 0x86: emit_movs_imm8(b, 1, kprog[pc++]); emit_setnz(b, 1); break;
		case 0x97: emit_strb_imm(b, 1, 0, 4 + kprog[pc++]); emit_setnz(b, 1); break;
		case 0xd6: emit_ldrb_imm(b, 2, 0, 4 + kprog[pc++]); emit_setnz(b, 2); break;
		case 0xc4: emit_movs_imm8(b, 4, kprog[pc++]); emit_ands(b, 2, 4); emit_setnz(b, 2); break;
		case 0xd7: emit_strb_imm(b, 2, 0, 4 + kprog[pc++]); emit_setnz(b, 2); break;
		case 0x96: emit_ldrb_imm(b, 1, 0, 4 + kprog[pc++]); emit_setnz(b, 1); break;
		case 0x85: emit_movs_imm8(b, 6, kprog[pc++]); emit_ands(b, 6, 1); emit_setnz(b, 6); break;
		default: pc = sizeof(kprog); break;
		}
	}

	emit_strb_imm(b, 1, 0, 0);
	emit_strb_imm(b, 2, 0, 1);
	emit_strb_imm(b, 3, 0, 2);
	emit_pop_r456_pc(b);
}

static void jit_m3_benchmark(PlaydateAPI *pd)
{
	jit_buf b;
	void (*blk)(kcpu *);
	kcpu ci, cj;
	unsigned i, ok;
	uint32_t t0, t1, t2;
	const unsigned K = 200000u;

	b.code = jit_code;
	b.len = 0;
	b.cap = sizeof(jit_code) / sizeof(jit_code[0]);
	jit_translate_kernel(&b);
	blk = (void (*)(kcpu *))jit_finalize(pd, &b);

	/* correctness: same start state -> same end state */
	memset(&ci, 0, sizeof(ci));
	memset(&cj, 0, sizeof(cj));
	kinterp(&ci);
	blk(&cj);
	ok = (ci.a == cj.a && ci.b == cj.b && ci.cc == cj.cc &&
		memcmp(ci.mem, cj.mem, sizeof(ci.mem)) == 0);

	pd->system->logToConsole(
		"vecx jit: M3 verify interp(a=%02x b=%02x cc=%02x) jit(a=%02x b=%02x cc=%02x) %s (%u hw)",
		ci.a, ci.b, ci.cc, cj.a, cj.b, cj.cc, ok ? "OK" : "MISMATCH", b.len);

	if (!ok)
		return;

	t0 = pd->system->getCurrentTimeMilliseconds();
	for (i = 0; i < K; i++)
		kinterp(&ci);
	t1 = pd->system->getCurrentTimeMilliseconds();
	for (i = 0; i < K; i++)
		blk(&cj);
	t2 = pd->system->getCurrentTimeMilliseconds();

	{
		uint32_t interp_ms = t1 - t0;
		uint32_t jit_ms = t2 - t1;
		uint32_t ratio_x100 = jit_ms ? (interp_ms * 100u) / jit_ms : 0u;

		pd->system->logToConsole(
			"vecx jit: M3 bench K=%u interp=%ums jit=%ums speedup=%u.%02ux (both PSRAM)",
			K, (unsigned)interp_ms, (unsigned)jit_ms,
			(unsigned)(ratio_x100 / 100u), (unsigned)(ratio_x100 % 100u));
	}
}

/* ------------------------------------------------- M4: full-flag ADD codegen */
/* CC: H=0x20 N=0x08 Z=0x04 V=0x02 C=0x01 */

static unsigned ref_addb(unsigned bval, unsigned imm, unsigned *cc_out)
{
	unsigned sum = bval + imm;
	unsigned r = sum & 0xffu;
	unsigned cc = 0;

	if (sum & 0x100u)                              cc |= 0x01u; /* C */
	if (((bval & 0xfu) + (imm & 0xfu)) & 0x10u)    cc |= 0x20u; /* H */
	if ((bval ^ r) & (imm ^ r) & 0x80u)            cc |= 0x02u; /* V */
	if (r & 0x80u)                                 cc |= 0x08u; /* N */
	if (r == 0u)                                   cc |= 0x04u; /* Z */

	*cc_out = cc;
	return r;
}

/* void addb(uint8_t *s)  // s[0]=b s[1]=cc ; b += imm with full 6809 flags.
 * r0=s, r2=b, r3=cc, r4/r5/r6 = temps (r5 holds the result). */
static void jit_translate_addb_imm(jit_buf *b, unsigned imm)
{
	emit_push_r456_lr(b);
	emit_ldrb_imm(b, 2, 0, 0);          /* r2 = b              */
	emit_ldrb_imm(b, 3, 0, 1);          /* r3 = cc             */

	emit_movs_imm8(b, 4, 0x2fu);        /* clear H,N,Z,V,C     */
	emit_bics(b, 3, 4);

	emit_movs_imm8(b, 4, imm & 0xffu);
	emit_adds_reg(b, 5, 2, 4);          /* r5 = b + imm (9-bit)*/

	emit_lsr_imm5(b, 4, 5, 8);          /* C = sum>>8          */
	emit_orrs(b, 3, 4);

	emit_movs_imm8(b, 4, 0xffu);
	emit_ands(b, 5, 4);                 /* r5 = result         */

	emit_mov_reg(b, 4, 2);              /* H: r4 = b&0xF       */
	emit_movs_imm8(b, 6, 0x0fu);
	emit_ands(b, 4, 6);
	emit_movs_imm8(b, 6, imm & 0x0fu);
	emit_adds_reg(b, 4, 4, 6);          /* + imm&0xF           */
	emit_lsr_imm5(b, 4, 4, 4);          /* nibble carry        */
	emit_lsl_imm5(b, 4, 4, 5);          /* -> 0x20             */
	emit_orrs(b, 3, 4);

	emit_mov_reg(b, 4, 2);              /* V: r4 = b ^ result  */
	emit_eors(b, 4, 5);
	emit_movs_imm8(b, 6, imm & 0xffu);
	emit_eors(b, 6, 5);                 /* r6 = imm ^ result   */
	emit_ands(b, 4, 6);
	emit_movs_imm8(b, 6, 0x80u);
	emit_ands(b, 4, 6);
	emit_lsr_imm5(b, 4, 4, 6);          /* 0x80>>6 -> 0x02     */
	emit_orrs(b, 3, 4);

	emit_lsr_imm5(b, 4, 5, 4);          /* N = (result>>4)&8   */
	emit_movs_imm8(b, 6, 0x08u);
	emit_ands(b, 4, 6);
	emit_orrs(b, 3, 4);

	emit_subs_imm3(b, 4, 5, 1);         /* Z = (result==0)?4:0 */
	emit_lsr_imm5(b, 4, 4, 31);
	emit_lsl_imm5(b, 4, 4, 2);
	emit_orrs(b, 3, 4);

	emit_mov_reg(b, 2, 5);              /* b = result          */
	emit_strb_imm(b, 2, 0, 0);
	emit_strb_imm(b, 3, 0, 1);
	emit_pop_r456_pc(b);
}

static void jit_m4_addflags(PlaydateAPI *pd)
{
	static const unsigned imms[] = { 0x01u, 0x0fu, 0x7fu, 0x80u, 0xffu };
	unsigned ti, bv, fails = 0, tested = 0;

	for (ti = 0; ti < sizeof(imms) / sizeof(imms[0]); ti++) {
		jit_buf jb;
		void (*addb)(uint8_t *);

		jb.code = jit_code;
		jb.len = 0;
		jb.cap = sizeof(jit_code) / sizeof(jit_code[0]);
		jit_translate_addb_imm(&jb, imms[ti]);
		addb = (void (*)(uint8_t *))jit_finalize(pd, &jb);

		for (bv = 0; bv < 256u; bv++) {
			uint8_t s[2];
			unsigned rcc, rres;

			s[0] = (uint8_t)bv;
			s[1] = 0;
			addb(s);

			rres = ref_addb(bv, imms[ti], &rcc);
			tested++;
			if (s[0] != (uint8_t)rres || s[1] != (uint8_t)rcc)
				fails++;
		}
	}

	pd->system->logToConsole("vecx jit: M4 ADDB full-flags %u/%u match %s",
		tested - fails, tested, fails == 0 ? "ALL OK" : "FAILS!");
}

void jit_selftest(PlaydateAPI *pd)
{
	jit_buf b;

	/* M1 */
	b.code = jit_code;
	b.len = 0;
	b.cap = sizeof(jit_code) / sizeof(jit_code[0]);
	emit_movs_imm8(&b, 0, 0x42u);
	emit_bx_lr(&b);
	{
		unsigned (*fn)(void) = (unsigned (*)(void))jit_finalize(pd, &b);
		unsigned r = fn();
		pd->system->logToConsole("vecx jit: M1 return-0x42 -> 0x%x %s", r, r == 0x42u ? "OK" : "WRONG");
	}

	/* M2 */
	b.code = jit_code;
	b.len = 0;
	b.cap = sizeof(jit_code) / sizeof(jit_code[0]);
	emit_ldrb_imm(&b, 1, 0, 0);
	emit_adds_imm3(&b, 1, 1, 1);
	emit_strb_imm(&b, 1, 0, 0);
	emit_bx_lr(&b);
	{
		void (*inc)(uint8_t *) = (void (*)(uint8_t *))jit_finalize(pd, &b);
		volatile uint8_t v = 0x10u;
		inc((uint8_t *)&v);
		inc((uint8_t *)&v);
		pd->system->logToConsole("vecx jit: M2 inc x2 (0x10) -> 0x%x %s", (unsigned)v, v == 0x12u ? "OK" : "WRONG");
	}

	/* M3 */
	jit_m3_benchmark(pd);

	/* M4 */
	jit_m4_addflags(pd);
}
