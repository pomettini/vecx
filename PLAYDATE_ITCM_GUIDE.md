# Playdate Fast-Memory (TCM) Code Relocation — Field Guide

Hard-won findings from relocating an emulator CPU core into fast SRAM on Playdate
(rev B hardware, STM32F746, SDK 3.0.6). Written so it can be reused for other
emulators. Every claim here was verified on device (PDU1-Y024621), builds 68–89.

## TL;DR

- Game code runs from **0x60000000** (external memory) which is SLOW: a cache
  miss costs **~265 CPU cycles** (~1.5 µs). The 16 KB I-cache + 16 KB D-cache
  sit in front of it.
- **0x20000000** is internal **DTCM** (64 KB on the F746): single-cycle, not
  cached. The game's **stack** lives here. This is the only fast memory you can
  reach from a normal game.
- You CAN relocate code into DTCM at runtime and execute it. The mechanism
  works. BUT the usable **contiguous** region is only **~4 KB** (the unused top
  of the game stack), because the rest of DTCM is riddled with firmware-owned
  holes. So the relocated core must be **≤ ~4 KB**.
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
FPS), but still below the 33.2 FPS plain baseline. Why below baseline: the
hot-core + fallback design makes the ~65% cold opcodes pay an extra dispatch and
re-fetch (`hotcore -> e6809_sstep`, two calls vs baseline's one), and that
overhead outweighs the fast-memory win on the hot 35%. So: **TCM execution helps,
but a hot-core-with-fallback masks it.** Only a FULL compact interpreter in TCM
(every opcode handled once, no fallback) would realize the benefit cleanly — and
even then it is bounded by the irreducible data-side cost (opcode/operand fetches
read cart/rom and memory operands read ram/io, all in slow RAM, regardless of
where the code lives). Don't expect TCM code relocation alone to be a silver
bullet for a memory/data-bound interpreter; it mainly removes the I-cache/code
miss component.

Checklist to make relocated code actually run:
1. `.itcm` INPUT collected inside the `.text` OUTPUT section (relocations
   applied) — the #1 gotcha.
2. Manual word-copy (not memcpy), cacheable `const` source, `volatile` dest.
3. Pool in the ~4 KB hole-free region near `frame-0x2180` (avoid the firmware
   holes; place to also avoid the game's deep render stack).
4. `-mlong-calls -fno-lto` on the relocated TU; verify no direct branch leaves
   `.itcm`; jump tables inline.
5. `clearICache()` after copy; call via pointer with Thumb bit set.
