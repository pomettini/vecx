# Playdate Fast-Memory (TCM) Code Relocation — Field Guide

Hard-won findings from relocating an emulator CPU core into fast SRAM on Playdate
(rev B hardware, STM32F746, SDK 3.0.6). Written so it can be reused for other
emulators. Every claim here was verified on device (PDU1-Y024621), builds 68–120.

## Result (vecx, Mine Storm)

**33.2 FPS baseline -> 38.2 FPS (+15%)** by relocating a compact decoded 6809 hot
core into DTCM and inlining the side-effect-free operand fast-paths. The win is
real but bounded: it removes the hot-code I-cache-miss component; the rest of the
gap to 50 Hz native is diffuse data-cache + compute. Build progression: +13% from
the hot opcode path in TCM (the bulk), +0.4 from inlining operand reads; data
relocation and cold opcodes added nothing (or regressed).

## TL;DR

- Game code runs from **0x60000000** (external memory) which is SLOW: a cache
  miss costs **~265 CPU cycles** (~1.5 µs). The 16 KB I-cache + 16 KB D-cache
  sit in front of it.
- **0x20000000** is internal **DTCM** (64 KB on the F746): single-cycle, not
  cached. The game's **stack** lives here. This is the only fast memory you can
  reach from a normal game.
- You CAN relocate code into DTCM at runtime and execute it, and it WINS for a
  CPU interpreter (its working set blows the 16 KB I-cache). The pool goes in the
  unused stack region; the usable safe gap is **~5+ KB** (between resident
  firmware data below and the live stack above) — an earlier "~4 KB ceiling" was
  a misdiagnosis of pool PLACEMENT, see the SOLVED section.
- **Relocate code, NOT data.** Hot data (a 1 KB array) already fits the 16 KB
  D-cache; moving it to DTCM via a runtime pointer REGRESSED (-4.5%). But INLINING
  the side-effect-free memory fast-paths to skip the dispatch CALL helped (+0.4).
- There is no clean SDK API for this; it is a documented community hack
  ("dirty optimization secrets for C", devforum thread 23011).

## Measured numbers (this device)

| Thing | Value |
| --- | --- |
| Code region (image base) | 0x60000000 (FMC/external, slow) |
| DTCM / stack region | 0x20000000–0x2000FFFF (64 KB) |
| `__builtin_frame_address(0)` in eventInit/relocate | ~0x20009b88 |
| ALU op, no memory (microbench) | ~18 ns/op (~3 cyc @ ~180 MHz — core is FAST) |
| Cache-HIT random read (4 KB working set) | ~57 ns |
| Cache-MISS random read (256 KB working set) | ~1474 ns (~265 cyc) |
| Image bytes the loader keeps resident | ~baseline image size; extra code beyond it faults on read |
| Largest CONTIGUOUS writable DTCM from frame-0x2180 down | **~4.2 KB** (0x200069c0–0x20007a00; first hole at 0x20006980) |
| Another firmware hole (lower) | 0x20003cc0 |

## The mechanism (the parts that work)

1. **Mark the function** for a custom section:
   ```c
   #define VECX_ITCM __attribute__((section(".itcm"), noinline, used))
   unsigned VECX_ITCM e6809_sstep(unsigned a, unsigned b) { ... }
   ```
   `noinline`+`used` keep it a real, addressable symbol under -flto.

2. **Local linker script** (`override LDSCRIPT = ./link_map.ld` in the Makefile;
   copy the SDK's `C_API/buildsupport/link_map.ld` and add, between `.text` and
   `.data`):
   ```
   .itcm :
   {
       . = ALIGN(16);
       __itcm_start = .;
       *(.itcm)
       *(.itcm.*)
       . = ALIGN(16);
       __itcm_end = .;
   }
   ```

3. **At init**, copy `[__itcm_start, __itcm_end)` into a DTCM pool carved from
   the unused deep stack, flush the I-cache, and call via a computed pointer
   with the Thumb bit set:
   ```c
   extern char __itcm_start[], __itcm_end[];
   uintptr_t size = (uintptr_t)__itcm_end - (uintptr_t)__itcm_start;
   uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
   uintptr_t pool  = (frame - 0x2180u - size) & ~(uintptr_t)0xf;   /* see caveats */
   const uint32_t   *src = (const uint32_t*)__itcm_start; /* cacheable! */
   volatile uint32_t *dst = (volatile uint32_t*)pool;     /* stops memcpy fold */
   for (uint32_t i = 0; i < size/4; i++) dst[i] = src[i]; /* MANUAL copy! */
   pd->system->clearICache();
   uintptr_t off = ((uintptr_t)e6809_sstep & ~1u) - (uintptr_t)__itcm_start;
   e6809_sstep_p = (fnptr)((pool + off) | 1u);            /* Thumb bit */
   ```
   Then dispatch through `e6809_sstep_p` instead of calling `e6809_sstep`.

4. **No -fPIC** (Playdate game builds don't use it; confirmed). Required for the
   relocated code's absolute references to remain valid.

## The gotchas (each one cost a build to find)

1. **`memcpy` HARD-FAULTS copying into the DTCM pool.** Its optimized LDM/STM
   path trips on that region. Use a **manual `uint32_t` loop**. Make the
   destination `volatile` so the compiler can't turn the loop back into memcpy.

2. **The source read must be CACHEABLE.** A `volatile` (uncached) sequential
   read of the 0x60000000 region faults after a burst (~few KB). Declare `src`
   as plain `const uint32_t*` (cacheable); only the `dst` needs `volatile`.

3. **MOVE code into .itcm; do NOT ADD it.** The loader only keeps ~the baseline
   image size resident. A large *extra* .itcm (e.g. a 20 KB test blob on top of
   the game) pushes the tail past the mapped region and the source read faults
   partway. Moving an existing ~20 KB function into .itcm keeps the image size
   ~constant, so the source stays resident. (Verify: total `text` size from
   `arm-none-eabi-size` should be ~unchanged.)

4. **DTCM below the stack top is full of firmware holes.** DTCM holds the game
   stack AND firmware stack/data. Unused regions are NOT uniformly writable —
   there are small (64-byte-granular) spots that HARD-FAULT on write
   (e.g. 0x20003cc0, 0x20006980 on this unit). A *sparse* probe (1 KB steps)
   walks ~29 KB "OK" because it steps over them; a *dense* copy hits them.
   **The largest contiguous safe pool, taken from `frame-0x2180` downward, is
   only ~4 KB.** So the relocated code must be ≤ ~4 KB. (The devforum author
   shrank their emulator core to ~2 KB for exactly this reason.)

5. **Relocated code's outbound calls.** If you relocate one function but its
   callees stay in slow memory, compile that translation unit with
   **`-mlong-calls -fno-lto`** so every call becomes an absolute
   (load-address-then-`blx`) call that still reaches the originals after the
   move. Verify with `objdump -d` that no `bl`/`b` inside the .itcm range
   targets an address outside it. (Alternative: relocate the entire closed call
   graph as one contiguous block, so intra-block PC-relative calls survive the
   move — but that needs every callee, incl. compiler-emitted helpers, in
   .itcm, which is fragile.)

6. **Switch jump tables**: GCC emits Thumb `tbb`/`tbh` with the table INLINE in
   the function, so it moves with the code — fine. Watch for any `.rodata` jump
   tables (absolute) which would NOT move; pull them into .itcm too if present.

7. **THE BIG ONE — a separate `.itcm` output section is NOT relocated by the
   loader.** The Playdate ships relocatable images (`--emit-relocs`); the loader
   fixes up `R_ARM_ABS32` literals at load time. The toolchain here uses
   `-mword-relocations`, so EVERY address (globals like `&reg_a`, and call
   targets like `&e6809_sstep` under `-mlong-calls`) is an absolute `.word`
   literal that the loader must patch. **But the loader only patches relocations
   for the standard output sections (`.text`/`.data`/`.bss`). A custom output
   section named `.itcm` gets its relocations in a separate `.rel.itcm` that the
   loader IGNORES** — so those literals keep their link-time (garbage/offset)
   values. Result: a relocated function that touches NO globals and makes NO
   calls runs fine (build-73-style self-test), but the instant it reads a global
   or calls out, it dereferences a bad literal and HARD-FAULTS.
   - Symptom that fingerprints this: a self-contained probe at the relocated
     entry returns correctly, but a probe that reads one global crashes.
   - **FIX: do NOT give `.itcm` its own output section. Collect the `.itcm`
     INPUT sections INSIDE the `.text` OUTPUT section**, bracketed by
     `__itcm_start`/`__itcm_end`, e.g.:
     ```
     .text :
     {
         *(.text) *(.text.*)
         *(.rodata*)
         . = ALIGN(16);
         __itcm_start = .;
         *(.itcm)
         . = ALIGN(16);
         __itcm_end = .;
         KEEP(*(.eh_frame*))
     }
     ```
     Now the relocations land in `.rel.text`, the loader applies them, and the
     copied code's global/call literals are correct. (Verify: `readelf -S` shows
     NO separate `.itcm` section; `__itcm_start/end` still defined.)

## Hot-core architecture (small relocated core + slow fallback)

The full interpreter won't fit the ~4KB pool, so relocate only a compact
hot-opcode core and fall back to the full (slow) interpreter for cold opcodes:

```c
unsigned VECX_ITCM e6809_hotcore(unsigned irq_i, unsigned irq_f) {
    if (irq_i || irq_f || special_state) return e6809_sstep(irq_i, irq_f); /* delegate */
    unsigned pc0 = reg_pc;
    unsigned op = hot_pc_read8();              /* INLINED cart/rom fetch */
    switch (op) {
    case 0x12: return 2;                       /* hot ops: bit-exact copies of */
    /* ... NOP, branches, immediate ALU ... */ /* the e6809_sstep cases */
    default: reg_pc = pc0; return e6809_sstep(0, 0); /* cold: rewind + full interp */
    }
}
```
- Dispatch `vecx_emu` through a pointer (`e6809_hotcore_p`) that init repoints to
  the relocated copy. Validate correctness IN PLACE first (matching
  instr-count/CPI vs baseline ⇒ bit-exact) before adding relocation.
- COST: cold opcodes pay a double fetch/dispatch (hot core + full interp). That
  is a ~12% regression when the hot core runs in SLOW memory; it only pays off
  once the hot core is in fast TCM (the double-dispatch overhead becomes cheap
  and the hot opcodes run single-cycle). So an in-place hot core looking slower
  is expected and not predictive of the relocated result.
- Inline the fetch (`cart[a]`/`rom[a&0x1fff]`) so hot ops never call the slow
  `vecx_read8`; let rare cases (RAM/IO PC) fall to `vecx_read8`.

## Debugging recipe that cracked this (isolation probes)

Add self-contained probes at the function entry, keyed by magic argument values,
each testing ONE capability, and call them from a shallow stack at init:
`return 0x42` (entry executable?), `return some_global` (data reloc ok?),
`return callee()` (call reloc ok?). Whichever probe is the last to log before a
crash names the exact broken capability. This separated "is it XN?" (no — entry
executed) from "is it the relocation?" (yes — global read faulted) in two builds
instead of guessing. Confirmed executable down to at least 0x20007660.

## Debugging on device (essential, because crashes hide info)

- **`logToConsole` output is LOST on a hard fault** (it's buffered). After each
  diagnostic line, busy-wait ~150 ms so it transmits before any later fault:
  ```c
  static void drain(void){ uint32_t t=pd->system->getCurrentTimeMilliseconds();
      while(pd->system->getCurrentTimeMilliseconds()-t<150u){} }
  ```
  Without this you cannot tell *where* it crashed.
- **`getElapsedTime()` costs ~10 µs/call** — fine for coarse timing, useless for
  per-instruction profiling (it dominates).
- **Raw DWT / Cortex-M debug registers HARD-FAULT** on Playdate firmware — do
  not use them for cycle counting.
- The boot banner prints `crashed=1` after a faulting run — a quick "did it
  fault last time" signal.
- To find the writable pool empirically: dense descending probe from
  `frame-0x2180`, writing one word per 64 bytes, draining after each, until it
  faults. Last logged address = the floor of the contiguous region.

## Verdict / status

End-to-end relocation of a real, global-touching, call-making function into the
fast DTCM pool WORKS once all the above is handled (proven build 97: entry +
global-read + call probes all pass from the relocated copy). The remaining
constraint is size: the contiguous safe pool is ~4 KB, so only a compact
hot-opcode core (or a fully rewritten ≤4 KB decoded interpreter) fits. A 20 KB
switch-interpreter does not.

MEASURED PAYOFF (vecx, build 97): a ~1 KB hot-opcode core (NOP/branches/imm-ALU,
~35% of executed opcodes) relocated to TCM ran the game at ~30.5 FPS — up from
~29.1 FPS for the SAME core running in slow memory (fast memory gave a real +1.4
FPS), but still below the 33.2 FPS plain baseline, because the hot-core+fallback
design makes the ~65% cold opcodes pay an extra dispatch (`hotcore -> e6809_sstep`).

**BIGGER PAYOFF (vecx, build 108/110): a wider compact core BEATS baseline —
35.5 FPS vs 33.2 (+7%).** Going from ~35% to ~85-90% of executed opcodes handled
inline in TCM (8-bit ALU imm/dir/idx + STA, decoded by addr-mode/reg/op-nibble;
only extended/16-bit/stack/misc fall back) flips the result: the fallback is now
rare enough that the fast-memory win on the hot path dominates. So the trend is
real and monotonic — **the more of the hot path you cover in TCM, the better, and
once the fallback is rare it clears baseline.** Still bounded by the irreducible
data-side cost (opcode/operand fetches read cart/rom, memory operands read ram/io,
all in slow RAM regardless of where the code lives); TCM relocation removes the
I-cache/code-miss component, not the data-miss component.

### SOLVED: the apparent "size ceiling" was pool PLACEMENT, not size
Symptom: the relocated `e6809_hotcore` ran at <=1328 B but hard-faulted mid-game
at >=1592 B — entry + all helper calls probing fine, objdump perfectly clean.
It looked like a function-size limit. **It was not.** Two experiments cracked it:
- **Inert padding** (300 B of `.space`, zero new code/calls) ALSO crashed at
  1636 B → not the RMW/ext code, purely footprint.
- **Shift test**: the EXACT working 1328 B function, placed ~0x180 LOWER, ALSO
  crashed → not size at all, it's the *lowest address the pool reaches*.

Root cause: the pool is `[frame - margin - size, frame - margin]`. With the
original `margin = 0x2180` the TOP is pinned at `frame-0x2180` and a bigger
function grows the pool DOWNWARD — and below ~`0x200074d0` it overwrites LIVE
firmware/SDK data resident in DTCM, corrupting it (faults later, when that data
is used; the copy itself succeeds and the entry/probes still run). The real safe
region is the GAP between that firmware-data floor (~`0x200074d0`) and the live
stack ceiling, and the stack ceiling is much higher than `frame-0x2180`: raising
the top to `frame-0x1180` (0x20008a00) still ran clean. So the usable gap is
**~5+ KB**, not 1.3 KB — `0x2180` was just a very conservative margin.

FIX: shrink `margin` so the pool TOP rises into the gap and the BOTTOM stays
ABOVE the firmware floor (`frame - margin - size >= ~0x200074d0`). Result: a
1756 B core (8-bit ALU all modes + STA + RMW group) relocated cleanly and ran the
game at 36.5 FPS (+10% vs 33.2 baseline). Practical recipe:
1. Find the firmware-data FLOOR: binary-search the lowest pool bottom that still
   runs (shift the same working function down until it faults). Here ~`0x200074d0`.
2. Find the stack CEILING: raise the top (shrink margin) until it faults. Here
   the stack stayed above `0x20008a00` comfortably.
3. Place the pool inside `[floor, ceiling]`; keep a safety buffer below the
   ceiling (the render/update call chain is loop-based = bounded depth, so the
   ceiling is roughly constant across scenes, but leave margin anyway).
Caveat: these addresses are device/SDK/stack-size specific (`STACK_SIZE` in the
Makefile sets the stack span); re-probe per project. The bisection METHOD
(padding test to separate code-from-footprint, shift test to separate
size-from-address) is the reusable part.

Method note — **how to bisect a relocated-execution crash**: gate each decode
path behind a compile flag and selectively `goto cold` (fall back) per addressing
mode / opcode group; relocated builds that fall back enough to run vs crash
localize the trigger in a few device cycles. Pair with per-callee entry probes
(magic `irq_i` values that return a sentinel) to prove the relocation plumbing
itself is sound before suspecting any one opcode.

### CODE relocation helps; hot-DATA relocation does NOT (measured)
A natural follow-on is "relocate the hot DATA into TCM too." On vecx this was a
**net loss** (build 118): moving the 1 KB 6809 RAM into a DTCM buffer regressed
-4.5% (37.7 -> 36.0 FPS). Two reasons, both general:
- The 1 KB RAM already FIT the 16 KB D-cache, so it was effectively D-cache-
  resident — DTCM (single-cycle) is no faster than a D-cache hit. Zero benefit.
- A *runtime-placed* DTCM buffer (address derived from the stack frame) forces
  the array to become a POINTER, adding a load-the-pointer indirection to EVERY
  access. That cost is paid on the hot path and outweighed the (nonexistent) win.
Rule of thumb: **relocate code when the hot code working set exceeds the 16 KB
I-cache (it does — a 6809 interpreter is ~20 KB); do NOT relocate hot data that
already fits the 16 KB D-cache.** Code and data caches are separate, so moving
the interpreter to TCM frees I-cache pressure without touching the D-cache-
resident data — which is exactly the win you want. Only consider data relocation
if the hot data working set itself blows the D-cache (large LUTs, framebuffers),
and even then prefer a compile-time-fixed TCM address to avoid the pointer
indirection.

### Inline the side-effect-free memory fast-paths (skip the dispatch CALL)
Separate from relocation, a cheap win once the hot core is in TCM: the core's
operand reads/writes were calling the full `vecx_read8`/`vecx_write8` dispatch
even for cart/rom/ram (side-effect-free). Inlining those regions directly in the
core (and only falling back to the dispatcher for VIA/io, which HAS side effects)
removed a function CALL on the hot path and gained +0.4 FPS (37.7 -> 38.1). Notes:
- This is NOT data relocation — the arrays stay put (D-cache-resident, fixed
  address, no pointer). You're only skipping the call/branch-ladder, not moving
  data. That's why it helps where build 118's pointer-redirect hurt.
- READS dominate (every ALU op reads an operand) so read-inlining is the lever;
  WRITE-inlining was ~neutral (writes are rarer, and stack writes went through a
  separate push helper). Profile which side is hot before bothering.
- Replicate the dispatcher's address decode EXACTLY and inline ONLY the
  side-effect-free ranges; route anything with read/write side effects (memory-
  mapped io) to the original dispatcher.

### The +3 FPS is WHOLE-BINARY I-cache packing, and it's load-bearing
A hard lesson from "consolidating" the working build: removing the diagnostic
probes + shrinking `itcm_relocate` dropped 38.2 -> 35 FPS (-8%), with NO change
to the relocated DTCM core. Why: the relocated core runs from DTCM (no I-cache),
but it CALLS helpers that stay in PSRAM (`ea_indexed`, the `inst_*` ALU ops).
Those helpers live in the 16 KB I-cache, and whether the hot ones conflict in the
same cache sets is decided by their absolute addresses — i.e. by the EXACT sizes
and order of every function across ALL translation units. Editing anything (the
probes in e6809.c, the size of `itcm_relocate` in playdate_main.c) shifts the
later object files and reshuffles the conflict pattern. Confirmed empirically:
- probes back + 16-align alone = 35 (not enough);
- probes + 16-align + the *full* (bigger) `itcm_relocate` = 38.2, pool address
  identical to the original fast build.
Key sub-findings:
- A runtime alignment sweep of the DTCM core base (4-byte steps) moved it only
  +-0.5 FPS — the relocated core's alignment is NOT the lever. The PSRAM helper
  I-cache packing is.
- Uniform padding can't fix it: shifting everything by N preserves relative
  addresses, so set-conflicts (which depend on address DIFFERENCES) are
  unchanged. Only changing function sizes/order changes the packing.
- So a fast layout is fragile: any source edit can reshuffle it. The diagnostic
  probes + full relocate here are kept deliberately as LOAD-BEARING layout; the
  code says so. Re-measure on device after ANY edit to these TUs.
The robust (but unattempted) alternative: relocate the hot helpers into TCM too
(PC-relative intra-cluster calls, route the few PSRAM out-calls via pointers) so
the hot path has no I-cache dependency at all — stable performance, no layout
luck. It risks the pool size budget and the working layout, so weigh carefully.

Checklist to make relocated code actually run:
1. `.itcm` INPUT collected inside the `.text` OUTPUT section (relocations
   applied) — the #1 gotcha.
2. Manual word-copy (not memcpy), cacheable `const` source, `volatile` dest.
3. Pool in the ~4 KB hole-free region near `frame-0x2180` (avoid the firmware
   holes; place to also avoid the game's deep render stack).
4. `-mlong-calls -fno-lto` on the relocated TU; verify no direct branch leaves
   `.itcm`; jump tables inline.
5. `clearICache()` after copy; call via pointer with Thumb bit set.
