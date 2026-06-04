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

## Verdict for a ~20 KB interpreter

Code-fetch from slow PSRAM is the dominant cost (~265 cyc/miss, ~7–8 misses per
emulated instruction => ~2000 host cyc/instr). Putting the hot core in DTCM
fixes that — but the contiguous pool is ~4 KB, so a 20 KB switch-interpreter
does not fit. You must **rewrite the core compact (decoded/table-driven) to
≤ ~4 KB** to relocate it as one block. That is the only path to the win on this
hardware, and it is a large, correctness-sensitive rewrite.
