# vecx Playdate Port Notes

## Current Status

- Goal: port `vecx` to a Playdate-only, C-only project.
- Priority: smoothness and speed on real Playdate hardware before emulation accuracy.
- Initial Playdate C build succeeds locally and produces `vecx.pdx`.
- The first Playdate milestone has visible vector rendering, D-pad/A/B input, FPS display, and low-frequency benchmark logs.
- Audio is deferred. `e8910.c` is currently a no-op sound stub so the project has no SDL dependency.
- Build 7 sets the Playdate update callback and core vector redraw cadence to 60 Hz (`VECTREX_MHZ / 60` cycles per update) as the next smoothness/speed experiment.
- Real-device testing shows the first build is CPU/emulation-bound, not render-bound.
- The inverted-color build was pushed to PDU1-Y024621 on 2026-05-31 03:43 CEST with `mine_storm.vec` packaged.
- Build 2 confirmed `mine_storm.vec` loads, but performance remains CPU/emulation-bound at about 5.8 FPS.
- Build 3 tested instruction-batched analog stepping plus `-O3` device compilation. It regressed performance, so build 4 disabled the batching path and added better emulator counters.
- Build 4 shows about 14k 6809 instructions and 50k emulated cycles per attempted update. Build 5 will try corrected full instruction batching across VIA timers and analog stepping.
- Build 5 improved to about 7.1 FPS.
- Build 6 regressed to about 5.9 FPS, likely because forced inline/direct CPU memory access increased code size and hurt instruction cache behavior.
- Build 7 returns to the build 5 CPU shape and raises the vector redraw cadence to 60 Hz, halving the emulated cycles per rendered frame as an explicit speed/smoothness tradeoff.
- Build 7 was pushed to PDU1-Y024621 on 2026-05-31 17:19 CEST. It reaches about 14.3 FPS by halving emulated cycles per rendered vector frame, but actual emulated throughput is still about 360k cycles/sec.
- Build 8 enables LTO for the Playdate device build. It reaches about 15.9-16.1 FPS and about 400k emulated cycles/sec; `pdex.bin` shrank from about 19.7 KB to 15.8 KB.
- Build 9 tests direct, non-inlined vecx memory calls from the 6809 core. This removes function-pointer dispatch overhead without allowing LTO to inline the whole memory mapper into the CPU switch. It was pushed to PDU1-Y024621 on 2026-05-31 17:28 CEST.
- Build 9 is effectively neutral versus build 8: about 15.9-16.3 FPS and 61.1-62.7 ms updates, so direct noinline memory calls are not a major standalone win.
- Build 10 tests cheaper CPU condition-code helpers and zero tests only. It regressed to about 15.1-15.3 FPS and 65-66 ms updates, so the original flag helper shape is better for this compiler/core.

## Repository Findings

- `vecx.c` contains the Vectrex machine/VIA/analog/vector core.
- `e6809.c` and `e6809.h` contain the Motorola 6809 CPU emulator.
- `playdate_main.c` owns the Playdate platform layer: ROM/cart loading, fixed update timing, input, vector rendering, FPS display, and benchmark logging.
- `e8910.c` is a temporary no-op sound stub.
- `osint.c` was removed. `osint.h` remains only as the render hook declaration used by `vecx.c`.
- `vecx.c` calls `osint_render()` whenever the emulated vector frame is ready, and the Playdate platform layer now implements it.
- `Makefile` now uses the Playdate C SDK build support instead of SDL.

## Porting Plan

1. Done: replace the SDL Makefile with a Playdate C SDK Makefile.
2. Done: create `Source/pdxinfo` Playdate metadata.
3. Done: move the BIOS ROM into `Source/rom.dat` so it is packaged into the `.pdx`.
4. Done: add a Playdate C entry point that:
   - loads `rom.dat` with `pd->file`,
   - optionally loads a local `cart.vec` or `mine_storm.vec` if present in the PDX,
   - resets the emulator,
   - sets a C update callback,
   - runs fixed Vectrex cycles per Playdate update.
5. Done: implement `osint_render()` with Playdate graphics calls.
6. Done: map Playdate controls:
   - D-pad to Vectrex joystick X/Y,
   - A to Vectrex button 1,
   - B to Vectrex button 2,
   - buttons 3 and 4 remain unassigned for the first milestone.
7. Done: show FPS with `pd->system->drawFPS(0, 0)`.
8. Done: add low-frequency benchmark logging through `pd->system->logToConsole`.
9. Done: remove SDL source/build code.
10. Next: run on real hardware and iterate using benchmark logs.

## Playdate Rendering Strategy

- Start with Playdate `graphics->clear()` and `graphics->drawLine()`.
- Render through Playdate display inversion so the device shows white vectors and FPS text on a black background.
- Fit the original portrait Vectrex coordinate space into the 400x240 Playdate screen, centered horizontally.
- Use integer scaling first to avoid unnecessary floating-point cost.
- Later benchmark alternatives:
  - rotated landscape layout,
  - direct framebuffer drawing,
  - line thinning or intensity thresholding,
  - frame skipping if real hardware cannot keep pace.

## Benchmark Plan

- Log once every 5 seconds, not every frame.
- Track:
  - build label,
  - update count,
  - rendered vector frame count,
  - average FPS,
  - average update/emulation time,
  - average render time,
  - min/max update frame time,
  - skipped frame count.
- Keep benchmark output concise enough to paste back between device builds.
- Current log format is one line every 5 seconds:
  `vecx bench build="..." window_ms=... updates=... renders=... avg_fps=... avg_update_ms=... avg_render_ms=... min_update_ms=... max_update_ms=... emu_cycles=... emu_instr=... avg_instr_update=... avg_cpi=... avg_vectors=... skipped=...`

## Known Limitations

- Audio is disabled for the first Playdate milestone.
- Only two of four Vectrex buttons are mapped initially because the Playdate has A/B plus the D-pad.
- The first renderer is correctness-oriented and simple, not the final fast path.
- Real-device benchmark data from PDU1-Y024621 shows roughly 5.8 FPS, with rendering around 3 ms and emulation/update around 171 ms.
- Local build warnings:
  - Simulator linking warns about reducing `__DATA,__common` alignment because the emulator keeps large static vector buffers.

## Benchmark History

| Build | Device | Avg FPS | Avg Update | Avg Render | Min/Max Update | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Initial SDL code | n/a | n/a | n/a | n/a | n/a | Desktop-only baseline before Playdate port. |
| Playdate C build 1 | local build only | n/a | n/a | n/a | n/a | `make` succeeds and packages `vecx.pdx`; no hardware benchmark yet. |
| Playdate C build 1 | PDU1-Y024621 | 5.82-5.87 | 170.06-172.72 ms | 2.76-3.03 ms | 137-182 ms | BIOS only; no cart found. Renderer is cheap, emulator stepping is the bottleneck. |
| Playdate C build 2 | PDU1-Y024621 | 5.76-5.91 | 168.86-173.03 ms | 2.90-3.03 ms | 128-189 ms | `mine_storm.vec` loaded; inverted colors; performance unchanged, so the bottleneck is emulator stepping. |
| Playdate C build 3 | PDU1-Y024621 | 4.73-4.88 | 204.44-210.83 ms | 2.48-2.66 ms | 158-224 ms | Instruction-batched analog stepping regressed update time, despite slightly lower render cost. Disable it. |
| Playdate C build 4 | PDU1-Y024621 | 5.59-5.73 | 174.13-178.44 ms | 4.55-4.86 ms | 133-191 ms | `-O3` plus counters, no batching. About 14k 6809 instructions/update, 50k cycles/update, 500 vectors/render. |
| Playdate C build 5 | PDU1-Y024621 | 7.12-7.19 | 138.40-140.11 ms | 1.83-2.13 ms | 114-157 ms | Corrected full instruction batching improved speed; about 14k instructions/update, 180 vectors/render. Visual quality still needs confirmation. |
| Playdate C build 6 | PDU1-Y024621 | 5.89-5.93 | 168.13-169.36 ms | 1.86-2.10 ms | 143-180 ms | CPU direct-call/forced-inline experiment regressed badly; larger code likely hurt cache. Revert this direction. |
| Playdate C build 7 | PDU1-Y024621 | 14.23-14.43 | 68.95-70.02 ms | 1.41-1.55 ms | 40-79 ms | 60 Hz redraw cadence doubled visible FPS, with about 7k instructions/update and 125-131 vectors/render. Total emulated throughput remains about 360k cycles/sec. |
| Playdate C build 8 | PDU1-Y024621 | 15.83-16.05 | 61.88-62.93 ms | 0.98-1.10 ms | 34-72 ms | LTO helped: about 400k emulated cycles/sec, 7k instructions/update, and 124-130 vectors/render. `pdex.bin` is about 15.8 KB. |
| Playdate C build 9 | PDU1-Y024621 | 15.90-16.26 | 61.06-62.65 ms | 1.12-1.24 ms | 18-78 ms | Direct noinline memory calls were neutral/slightly noisy versus build 8. About 400-406k emulated cycles/sec. |
| Playdate C build 10 | PDU1-Y024621 | 15.07-15.32 | 64.93-66.07 ms | 1.41-1.50 ms | 29-77 ms | Cheaper condition-code helpers regressed versus build 8/9. Revert this direction. |

## Next Steps

- Revert build 10's flag helper changes before the next performance experiment.
- Keep watching visual quality: the 60 Hz vector cadence may improve smoothness but also makes each rendered vector frame represent less emulated time.

## CrankBoy/Playdate Optimization Notes

- CrankBoy reaches full-speed Game Boy emulation through a heavily modified Peanut-GB core plus Playdate-specific memory/layout work, not through a stock portable emulator core.
- Relevant techniques to evaluate for vecx:
  - custom linker map with 32-byte and 0x1000 alignment fences to reduce cache/branch-predictor performance lottery,
  - split hot/cold code into explicit sections such as core, main, framebuffer, and rare paths,
  - TCM relocation/allocation for very hot core code or small hot state,
  - fast-path memory access using direct region/page pointers with fallback only for I/O/special cases,
  - specialized opcode/immediate fetch paths that bypass full memory-map decoding for ROM/cart fetches,
  - direct framebuffer writes and dirty-row marking instead of high-level draw calls when rendering becomes relevant,
  - dynamic frame pacing/interlace/frame-skip style options that keep emulation speed separate from display refresh where possible.
- The most promising vecx transfer is a small Playdate-specific 6809 memory/fetch layer plus linker/section placement. Rendering tricks are lower priority because current render time is only about 1-1.5 ms.
