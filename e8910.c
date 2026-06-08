#include <stdint.h>
#include <string.h>
#include "pd_api.h"
#include "e8910.h"

/* The AY core generates 8-bit mono at this rate; the Playdate audio source runs
 * at 44100, so we upsample 2x (44100 = 2 * 22050) in the source callback. */
#define SOUND_FREQ   22050

/***************************************************************************

  ay8910.c


  Emulation of the AY-3-8910 / YM2149 sound chip.

  Based on various code snippets by Ville Hallik, Michael Cuddy,
  Tatsuyuki Satoh, Fabrice Frances, Nicola Salmoria.

***************************************************************************/

#define MAX_OUTPUT 0x0fff
//#define MAX_OUTPUT 0x7f

#define STEP3 1
#define STEP2 length
#define STEP  2


typedef int           INT32;
typedef unsigned int  UINT32;
typedef char          INT8;
typedef unsigned char UINT8;

struct AY8910 {
	int index;
	int ready;
	unsigned *Regs;
	INT32 lastEnable;
	INT32 PeriodA,PeriodB,PeriodC,PeriodN,PeriodE;
	INT32 CountA,CountB,CountC,CountN,CountE;
	UINT32 VolA,VolB,VolC,VolE;
	UINT8 EnvelopeA,EnvelopeB,EnvelopeC;
	UINT8 OutputA,OutputB,OutputC,OutputN;
	INT8 CountEnv;
	UINT8 Hold,Alternate,Attack,Holding;
	INT32 RNG;
	unsigned int VolTable[32];

} PSG;

/* register id's */
#define AY_AFINE	(0)
#define AY_ACOARSE	(1)
#define AY_BFINE	(2)
#define AY_BCOARSE	(3)
#define AY_CFINE	(4)
#define AY_CCOARSE	(5)
#define AY_NOISEPER	(6)
#define AY_ENABLE	(7)
#define AY_AVOL		(8)
#define AY_BVOL		(9)
#define AY_CVOL		(10)
#define AY_EFINE	(11)
#define AY_ECOARSE	(12)
#define AY_ESHAPE	(13)

#define AY_PORTA	(14)
#define AY_PORTB	(15)

/* Applies a register write to the PSG state. Runs ONLY on the audio thread
 * (drained from the queue in the source callback) so it never races the
 * generator's read-modify-writes of the same Count/Output fields. */
static void e8910_apply(int r, int v)
{
	int old;

    if (PSG.Regs == NULL) return;
	PSG.Regs[r] = v;

	/* A note about the period of tones, noise and envelope: for speed reasons,*/
	/* we count down from the period to 0, but careful studies of the chip     */
	/* output prove that it instead counts up from 0 until the counter becomes */
	/* greater or equal to the period. This is an important difference when the*/
	/* program is rapidly changing the period to modulate the sound.           */
	/* To compensate for the difference, when the period is changed we adjust  */
	/* our internal counter.                                                   */
	/* Also, note that period = 0 is the same as period = 1. This is mentioned */
	/* in the YM2203 data sheets. However, this does NOT apply to the Envelope */
	/* period. In that case, period = 0 is half as period = 1. */
	switch( r )
	{
	case AY_AFINE:
	case AY_ACOARSE:
		PSG.Regs[AY_ACOARSE] &= 0x0f;
		old = PSG.PeriodA;
		PSG.PeriodA = (PSG.Regs[AY_AFINE] + 256 * PSG.Regs[AY_ACOARSE]) * STEP3;
		if (PSG.PeriodA == 0) PSG.PeriodA = STEP3;
		PSG.CountA += PSG.PeriodA - old;
		if (PSG.CountA <= 0) PSG.CountA = 1;
		break;
	case AY_BFINE:
	case AY_BCOARSE:
		PSG.Regs[AY_BCOARSE] &= 0x0f;
		old = PSG.PeriodB;
		PSG.PeriodB = (PSG.Regs[AY_BFINE] + 256 * PSG.Regs[AY_BCOARSE]) * STEP3;
		if (PSG.PeriodB == 0) PSG.PeriodB = STEP3;
		PSG.CountB += PSG.PeriodB - old;
		if (PSG.CountB <= 0) PSG.CountB = 1;
		break;
	case AY_CFINE:
	case AY_CCOARSE:
		PSG.Regs[AY_CCOARSE] &= 0x0f;
		old = PSG.PeriodC;
		PSG.PeriodC = (PSG.Regs[AY_CFINE] + 256 * PSG.Regs[AY_CCOARSE]) * STEP3;
		if (PSG.PeriodC == 0) PSG.PeriodC = STEP3;
		PSG.CountC += PSG.PeriodC - old;
		if (PSG.CountC <= 0) PSG.CountC = 1;
		break;
	case AY_NOISEPER:
		PSG.Regs[AY_NOISEPER] &= 0x1f;
		old = PSG.PeriodN;
		PSG.PeriodN = PSG.Regs[AY_NOISEPER] * STEP3;
		if (PSG.PeriodN == 0) PSG.PeriodN = STEP3;
		PSG.CountN += PSG.PeriodN - old;
		if (PSG.CountN <= 0) PSG.CountN = 1;
		break;
	case AY_ENABLE:
		PSG.lastEnable = PSG.Regs[AY_ENABLE];
		break;
	case AY_AVOL:
		PSG.Regs[AY_AVOL] &= 0x1f;
		PSG.EnvelopeA = PSG.Regs[AY_AVOL] & 0x10;
		PSG.VolA = PSG.EnvelopeA ? PSG.VolE : PSG.VolTable[PSG.Regs[AY_AVOL] ? PSG.Regs[AY_AVOL]*2+1 : 0];
		break;
	case AY_BVOL:
		PSG.Regs[AY_BVOL] &= 0x1f;
		PSG.EnvelopeB = PSG.Regs[AY_BVOL] & 0x10;
		PSG.VolB = PSG.EnvelopeB ? PSG.VolE : PSG.VolTable[PSG.Regs[AY_BVOL] ? PSG.Regs[AY_BVOL]*2+1 : 0];
		break;
	case AY_CVOL:
		PSG.Regs[AY_CVOL] &= 0x1f;
		PSG.EnvelopeC = PSG.Regs[AY_CVOL] & 0x10;
		PSG.VolC = PSG.EnvelopeC ? PSG.VolE : PSG.VolTable[PSG.Regs[AY_CVOL] ? PSG.Regs[AY_CVOL]*2+1 : 0];
		break;
	case AY_EFINE:
	case AY_ECOARSE:
		old = PSG.PeriodE;
		PSG.PeriodE = ((PSG.Regs[AY_EFINE] + 256 * PSG.Regs[AY_ECOARSE])) * STEP3;
		//if (PSG.PeriodE == 0) PSG.PeriodE = STEP3 / 2;
		if (PSG.PeriodE == 0) PSG.PeriodE = STEP3;
		PSG.CountE += PSG.PeriodE - old;
		if (PSG.CountE <= 0) PSG.CountE = 1;
		break;
	case AY_ESHAPE:
		/* envelope shapes:
        C AtAlH
        0 0 x x  \___

        0 1 x x  /___

        1 0 0 0  \\\\

        1 0 0 1  \___

        1 0 1 0  \/\/
                  ___
        1 0 1 1  \

        1 1 0 0  ////
                  ___
        1 1 0 1  /

        1 1 1 0  /\/\

        1 1 1 1  /___

        The envelope counter on the AY-3-8910 has 16 steps. On the YM2149 it
        has twice the steps, happening twice as fast. Since the end result is
        just a smoother curve, we always use the YM2149 behaviour.
        */
		PSG.Regs[AY_ESHAPE] &= 0x0f;
		PSG.Attack = (PSG.Regs[AY_ESHAPE] & 0x04) ? 0x1f : 0x00;
		if ((PSG.Regs[AY_ESHAPE] & 0x08) == 0)
		{
			/* if Continue = 0, map the shape to the equivalent one which has Continue = 1 */
			PSG.Hold = 1;
			PSG.Alternate = PSG.Attack;
		}
		else
		{
			PSG.Hold = PSG.Regs[AY_ESHAPE] & 0x01;
			PSG.Alternate = PSG.Regs[AY_ESHAPE] & 0x02;
		}
		PSG.CountE = PSG.PeriodE;
		PSG.CountEnv = 0x1f;
		PSG.Holding = 0;
		PSG.VolE = PSG.VolTable[PSG.CountEnv ^ PSG.Attack];
		if (PSG.EnvelopeA) PSG.VolA = PSG.VolE;
		if (PSG.EnvelopeB) PSG.VolB = PSG.VolE;
		if (PSG.EnvelopeC) PSG.VolC = PSG.VolE;
		break;
	case AY_PORTA:
		break;
	case AY_PORTB:
		break;
	}
}

/* Generation now runs on the main thread (e8910_generate, called right after the
 * emulator), so register writes can be applied directly here -- no cross-thread
 * sharing of the PSG state. */
void e8910_write(int r, int v)
{
	e8910_apply(r, v);
}

static void
e8910_callback(int16_t *stream, int length)
{
	int outn;
	int16_t* buf1 = stream;

	/* hack to prevent us from hanging when starting filtered outputs */
	if (!PSG.ready)
	{
		memset(stream, 0, length * sizeof(*stream));
		return;
	}

	/* One output sample per outer iteration. The original doubled length and then
	 * wrote only every other sample (decimate-by-2 with no filter), which aliased
	 * the square-wave harmonics; we generate at the output rate directly instead.
	 * Each iteration advances the generators by STEP units, so the per-sample rate
	 * is unchanged when this runs at 44100 vs the original 22050. */

	/* The 8910 has three outputs, each output is the mix of one of the three */
	/* tone generators and of the (single) noise generator. The two are mixed */
	/* BEFORE going into the DAC. The formula to mix each channel is: */
	/* (ToneOn | ToneDisable) & (NoiseOn | NoiseDisable). */
	/* Note that this means that if both tone and noise are disabled, the output */
	/* is 1, not 0, and can be modulated changing the volume. */


	/* If the channels are disabled, set their output to 1, and increase the */
	/* counter, if necessary, so they will not be inverted during this update. */
	/* Setting the output to 1 is necessary because a disabled channel is locked */
	/* into the ON state (see above); and it has no effect if the volume is 0. */
	/* If the volume is 0, increase the counter, but don't touch the output. */
	if (PSG.Regs[AY_ENABLE] & 0x01)
	{
		if (PSG.CountA <= STEP2) PSG.CountA += STEP2;
		PSG.OutputA = 1;
	}
	else if (PSG.Regs[AY_AVOL] == 0)
	{
		/* note that I do count += length, NOT count = length + 1. You might think */
		/* it's the same since the volume is 0, but doing the latter could cause */
		/* interferencies when the program is rapidly modulating the volume. */
		if (PSG.CountA <= STEP2) PSG.CountA += STEP2;
	}
	if (PSG.Regs[AY_ENABLE] & 0x02)
	{
		if (PSG.CountB <= STEP2) PSG.CountB += STEP2;
		PSG.OutputB = 1;
	}
	else if (PSG.Regs[AY_BVOL] == 0)
	{
		if (PSG.CountB <= STEP2) PSG.CountB += STEP2;
	}
	if (PSG.Regs[AY_ENABLE] & 0x04)
	{
		if (PSG.CountC <= STEP2) PSG.CountC += STEP2;
		PSG.OutputC = 1;
	}
	else if (PSG.Regs[AY_CVOL] == 0)
	{
		if (PSG.CountC <= STEP2) PSG.CountC += STEP2;
	}

	/* for the noise channel we must not touch OutputN - it's also not necessary */
	/* since we use outn. */
	if ((PSG.Regs[AY_ENABLE] & 0x38) == 0x38)	/* all off */
		if (PSG.CountN <= STEP2) PSG.CountN += STEP2;

	outn = (PSG.OutputN | PSG.Regs[AY_ENABLE]);

	/* buffering loop */
	while (length > 0)
	{
        unsigned vol;
    int left  = 2;
		/* vola, volb and volc keep track of how long each square wave stays */
		/* in the 1 position during the sample period. */

		int vola,volb,volc;
		vola = volb = volc = 0;

		do
		{
			int nextevent;

			if (PSG.CountN < left) nextevent = PSG.CountN;
			else nextevent = left;

			if (outn & 0x08)
			{
				if (PSG.OutputA) vola += PSG.CountA;
				PSG.CountA -= nextevent;
				/* PeriodA is the half period of the square wave. Here, in each */
				/* loop I add PeriodA twice, so that at the end of the loop the */
				/* square wave is in the same status (0 or 1) it was at the start. */
				/* vola is also incremented by PeriodA, since the wave has been 1 */
				/* exactly half of the time, regardless of the initial position. */
				/* If we exit the loop in the middle, OutputA has to be inverted */
				/* and vola incremented only if the exit status of the square */
				/* wave is 1. */
				while (PSG.CountA <= 0)
				{
					PSG.CountA += PSG.PeriodA;
					if (PSG.CountA > 0)
					{
						PSG.OutputA ^= 1;
						if (PSG.OutputA) vola += PSG.PeriodA;
						break;
					}
					PSG.CountA += PSG.PeriodA;
					vola += PSG.PeriodA;
				}
				if (PSG.OutputA) vola -= PSG.CountA;
			}
			else
			{
				PSG.CountA -= nextevent;
				while (PSG.CountA <= 0)
				{
					PSG.CountA += PSG.PeriodA;
					if (PSG.CountA > 0)
					{
						PSG.OutputA ^= 1;
						break;
					}
					PSG.CountA += PSG.PeriodA;
				}
			}

			if (outn & 0x10)
			{
				if (PSG.OutputB) volb += PSG.CountB;
				PSG.CountB -= nextevent;
				while (PSG.CountB <= 0)
				{
					PSG.CountB += PSG.PeriodB;
					if (PSG.CountB > 0)
					{
						PSG.OutputB ^= 1;
						if (PSG.OutputB) volb += PSG.PeriodB;
						break;
					}
					PSG.CountB += PSG.PeriodB;
					volb += PSG.PeriodB;
				}
				if (PSG.OutputB) volb -= PSG.CountB;
			}
			else
			{
				PSG.CountB -= nextevent;
				while (PSG.CountB <= 0)
				{
					PSG.CountB += PSG.PeriodB;
					if (PSG.CountB > 0)
					{
						PSG.OutputB ^= 1;
						break;
					}
					PSG.CountB += PSG.PeriodB;
				}
			}

			if (outn & 0x20)
			{
				if (PSG.OutputC) volc += PSG.CountC;
				PSG.CountC -= nextevent;
				while (PSG.CountC <= 0)
				{
					PSG.CountC += PSG.PeriodC;
					if (PSG.CountC > 0)
					{
						PSG.OutputC ^= 1;
						if (PSG.OutputC) volc += PSG.PeriodC;
						break;
					}
					PSG.CountC += PSG.PeriodC;
					volc += PSG.PeriodC;
				}
				if (PSG.OutputC) volc -= PSG.CountC;
			}
			else
			{
				PSG.CountC -= nextevent;
				while (PSG.CountC <= 0)
				{
					PSG.CountC += PSG.PeriodC;
					if (PSG.CountC > 0)
					{
						PSG.OutputC ^= 1;
						break;
					}
					PSG.CountC += PSG.PeriodC;
				}
			}

			PSG.CountN -= nextevent;
			if (PSG.CountN <= 0)
			{
				/* Is noise output going to change? */
				if ((PSG.RNG + 1) & 2)	/* (bit0^bit1)? */
				{
					PSG.OutputN = ~PSG.OutputN;
					outn = (PSG.OutputN | PSG.Regs[AY_ENABLE]);
				}

				/* The Random Number Generator of the 8910 is a 17-bit shift */
				/* register. The input to the shift register is bit0 XOR bit3 */
				/* (bit0 is the output). This was verified on AY-3-8910 and YM2149 chips. */

				/* The following is a fast way to compute bit17 = bit0^bit3. */
				/* Instead of doing all the logic operations, we only check */
				/* bit0, relying on the fact that after three shifts of the */
				/* register, what now is bit3 will become bit0, and will */
				/* invert, if necessary, bit14, which previously was bit17. */
				if (PSG.RNG & 1) PSG.RNG ^= 0x24000; /* This version is called the "Galois configuration". */
				PSG.RNG >>= 1;
				PSG.CountN += PSG.PeriodN;
			}

			left -= nextevent;
		} while (left > 0);

		/* update envelope */
		if (PSG.Holding == 0)
		{
			PSG.CountE -= STEP;
			if (PSG.CountE <= 0)
			{
				do
				{
					PSG.CountEnv--;
					PSG.CountE += PSG.PeriodE;
				} while (PSG.CountE <= 0);

				/* check envelope current position */
				if (PSG.CountEnv < 0)
				{
					if (PSG.Hold)
					{
						if (PSG.Alternate)
							PSG.Attack ^= 0x1f;
						PSG.Holding = 1;
						PSG.CountEnv = 0;
					}
					else
					{
						/* if CountEnv has looped an odd number of times (usually 1), */
						/* invert the output. */
						if (PSG.Alternate && (PSG.CountEnv & 0x20))
 							PSG.Attack ^= 0x1f;

						PSG.CountEnv &= 0x1f;
					}
				}

				PSG.VolE = PSG.VolTable[PSG.CountEnv ^ PSG.Attack];
				/* reload volume */
				if (PSG.EnvelopeA) PSG.VolA = PSG.VolE;
				if (PSG.EnvelopeB) PSG.VolB = PSG.VolE;
				if (PSG.EnvelopeC) PSG.VolC = PSG.VolE;
			}
		}

    vol = (vola * PSG.VolA + volb * PSG.VolB + volc * PSG.VolC) / (3 * STEP);
    /* emit the raw (unipolar) level; the source callback DC-blocks + gains it.
     * vol is small (a few thousand at most) so it fits an int16 directly. */
    length--;
    *(buf1++) = (int16_t)(vol > 32767u ? 32767u : vol);
	}
}


static void
e8910_build_mixer_table()
{
	int i;
	double out;

	/* calculate the volume->voltage conversion table */
	/* The AY-3-8910 has 16 levels, in a logarithmic scale (3dB per STEP) */
	/* The YM2149 still has 16 levels for the tone generators, but 32 for */
	/* the envelope generator (1.5dB per STEP). */
	out = MAX_OUTPUT;
	for (i = 31;i > 0;i--)
	{
		PSG.VolTable[i] = (unsigned)(out + 0.5);	/* round to nearest */
		out /= 1.188502227;	/* = 10 ^ (1.5/20) = 1.5dB */
	}
	PSG.VolTable[0] = 0;
}


/* The PSG's own register file, written only by e8910_apply on the audio thread.
 * (vecx.c keeps its separate snd_regs for its read-back path; we no longer share
 * it, so the generator never reads registers the emulator is concurrently writing.) */
static unsigned psg_regs[16];

static PlaydateAPI* s_pd;
static SoundSource* s_source;

/* diagnostics (read from the main thread's benchmark log) */
volatile unsigned e8910_dbg_calls;  /* PD audio callbacks fired */
volatile unsigned e8910_dbg_active; /* underrun samples */
volatile int e8910_dbg_maxvol;      /* peak raw vol seen (pre-gain) */
volatile int e8910_dbg_ratio_x1000; /* resample rate * 1000 (1000 = real pitch) */

#define E8910_GAIN 8.0f /* output volume; raise/lower to taste */

/* Sample ring buffer. Single producer = e8910_generate (main thread, run right
 * after the emulator); single consumer = the audio callback. The emulator makes
 * exactly the samples for the cycles it emulated, so audio stays in lockstep with
 * emulation; when the (slow) emulator can't keep up the callback runs dry and
 * emits silence -- choppy but correct, instead of free-running into noise. */
#define SRING_SIZE 8192 /* power of two; ~186 ms at 44100 */
static volatile int16_t sring[SRING_SIZE];
static volatile unsigned sring_head; /* producer (main) */
static volatile unsigned sring_tail; /* consumer (audio) */

unsigned e8910_ring_fill(void)
{
	return sring_head - sring_tail;
}

void e8910_generate(int n)
{
	static int16_t tmp[256];
	static float hp_x, hp_y; /* one-pole DC blocker (centres the unipolar signal) */

	if (!PSG.ready)
		return;

	while (n > 0) {
		int chunk = n > (int)(sizeof(tmp) / sizeof(tmp[0])) ? (int)(sizeof(tmp) / sizeof(tmp[0])) : n;
		int i;

		e8910_callback(tmp, chunk);
		for (i = 0; i < chunk; i++) {
			float x = (float)tmp[i];
			float y = x - hp_x + 0.999f * hp_y; /* DC block */
			int v;

			if (tmp[i] > e8910_dbg_maxvol)
				e8910_dbg_maxvol = tmp[i];
			hp_x = x;
			hp_y = y;
			v = (int)(y * E8910_GAIN);
			if (v > 32767)
				v = 32767;
			else if (v < -32768)
				v = -32768;

			if (sring_head - sring_tail < SRING_SIZE) { /* drop if full */
				sring[sring_head & (SRING_SIZE - 1)] = (int16_t)v;
				sring_head++;
			}
		}
		n -= chunk;
	}
}

/* Audio callback (audio thread): play the ring 1:1 (real-time). The main thread
 * keeps it topped up each update, so it stays smooth; on the rare underrun we
 * hold the last sample (no click). */
static int e8910_pd_callback(void* context, int16_t* left, int16_t* right, int len)
{
	static int16_t cur;
	int i;

	(void)context;
	(void)right;

	e8910_dbg_calls++;

	for (i = 0; i < len; i++) {
		if (sring_tail != sring_head) {
			cur = sring[sring_tail & (SRING_SIZE - 1)];
			sring_tail++;
		} else {
			e8910_dbg_active++; /* underrun: hold last sample */
		}
		left[i] = cur;
	}
	return 1;
}

void e8910_init_sound(void* pd)
{
	s_pd = (PlaydateAPI*)pd;

	PSG.Regs = psg_regs;
	PSG.RNG  = 1;
	PSG.OutputA = 0;
	PSG.OutputB = 0;
	PSG.OutputC = 0;
	PSG.OutputN = 0xff;

	/* The audio source starts firing immediately, before the game programs any
	 * sound registers -- so seed valid (non-zero) periods/counts and hold the
	 * envelope. With a zero period the generator's "while (Count <= 0) Count +=
	 * Period" loops (and the negative-nextevent noise path) never terminate,
	 * which would hang the audio thread and trip the watchdog. e8910_write keeps
	 * every period >= 1 once the game starts writing. */
	PSG.PeriodA = PSG.PeriodB = PSG.PeriodC = PSG.PeriodN = PSG.PeriodE = 1;
	PSG.CountA = PSG.CountB = PSG.CountC = PSG.CountN = PSG.CountE = 1;
	PSG.Holding = 1;

	e8910_build_mixer_table();
	PSG.ready = 1;

	s_source = s_pd->sound->addSource(e8910_pd_callback, NULL, 0); /* mono */
}

void e8910_done_sound(void)
{
	PSG.ready = 0;
	if (s_pd != NULL && s_source != NULL) {
		s_pd->sound->removeSource(s_source);
		s_source = NULL;
	}
}

