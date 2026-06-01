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
- Build 11 reverts Build 10's flag-helper regression and tests a CrankBoy-style layout pass: local linker map, 32-byte hot-section fences, hot/cold function sections, and `-falign-loops=32`. It regressed hard to about 12.4-12.6 FPS and 79-80 ms updates, so this simple layout pass should be reverted.
- Build 12 returns to the Build 8/9 code-generation shape and tests a targeted fast PC fetch path for opcode/immediate reads from cart/BIOS ROM. It regressed to about 13.0 FPS and 76 ms updates; the extra code/cache cost outweighed the saved memory-map branches.
- Build 13 reverts the fast PC fetch path and adds low-frequency opcode histogram logging so the next CPU work is based on Mine Storm's actual hot instruction mix. It was pushed to PDU1-Y024621 on 2026-05-31 18:06 CEST.
- Build 13 shows the diagnostic counter costs about 2.5 FPS versus build 8/9, but the hot instruction mix is clear: `BEQ` (`0x27`) and `BITA direct` (`0x95`) account for roughly 40% of executed instructions, followed by `STA direct` (`0x97`), `NOP` (`0x12`), and `LDA indexed` (`0xa6`).
- Build 14 adds targeted hot-op profiling for `BEQ` taken/not-taken counts, `BITA direct` operands, `STA direct` operands, and `LDA indexed` postbytes. It was pushed to PDU1-Y024621 on 2026-05-31 18:10 CEST and should identify whether the hotspot is a single VIA polling loop.
- Build 14 confirms the hottest loop is VIA polling: `BITA <$0d` is the only hot `BITA direct` operand and maps to VIA IFR at `$D00D`; `BEQ` is taken roughly 97.5% of the time. Hot `STA direct` operands are `$0a`, `$00`, and `$01`, which map to the VIA shift register and output ports.
- Build 15 removes the profiling counters and tests two small hot-op fast paths: branchful `BNE`/`BEQ`, and a `BITA <$0d` shortcut that reads VIA IFR directly when DP is `$D0`. It was pushed to PDU1-Y024621 on 2026-05-31 18:15 CEST.
- Build 15 regressed versus the build 8/9 no-profiler baseline, reaching about 15.4-15.6 FPS and 64-65 ms updates. Build 16 removes the VIA IFR shortcut and keeps only the branchful `BNE`/`BEQ` handling to isolate which part caused the regression.
- Build 16 regressed further to about 12.0 FPS and 82-84 ms updates, so branchful `BNE`/`BEQ` is the bad shape on this compiler/core. Build 17 reverts that path back to the original branch helper.
- Build 17 recovers the build 8/9 baseline at about 15.9-16.3 FPS and 61-63 ms updates.
- Build 18 tests a tiny `LDA indexed` specialization for Mine Storm's hot `A6` postbytes: `$c0` (`LDA ,U+`) and `$86` (`LDA A,X`), bypassing the large indexed-addressing switch for those two cases.
- Build 18 regressed to about 12.7-12.9 FPS and 77-79 ms updates, confirming that adding opcode-specific branches inside the 6809 switch is usually worse than the work it saves.
- Build 19 restores the recovered CPU shape and tests a 75 Hz Vectrex/update cadence, reducing cycles per visual update from 25,000 to 20,000 as a deliberate smoothness-over-accuracy tradeoff.
- Build 19 improves visible cadence to about 19.3-19.6 FPS and 50-52 ms updates. Render cost stays around 1 ms and emulated throughput remains around 390k cycles/sec, so the gain comes from fewer emulated cycles per visual update rather than a faster CPU core.
- Build 20 tests the next cadence step at 90 Hz, reducing cycles per visual update to about 16,666.
- Build 20 improves visible cadence to about 22.2-22.7 FPS and 44 ms updates. Render cost drops below 1 ms with about 84-87 vectors/render; emulated throughput is about 373k-379k cycles/sec.
- Build 21 tests a 120 Hz Vectrex/update cadence, reducing cycles per visual update to 12,500.
- Build 21 improves visible cadence to about 27.4-27.8 FPS and 35-36 ms updates. Render cost remains below 1 ms with about 63-65 vectors/render.
- Build 22 rotates the Vectrex output 90 degrees counter-clockwise for a Playdate landscape fill and remaps the D-pad axes to match the rotated display. It was pushed to PDU1-Y024621 on 2026-06-01 00:24 CEST.
- Build 22's direction was wrong for the desired handheld orientation: it used +90 degrees instead of -90 degrees.
- Build 23 flips the landscape transform to -90 degrees and adds a short vector-history persistence pass: draw the three previous vector frames, then draw the current frame. It was pushed to PDU1-Y024621 on 2026-06-01 00:30 CEST.
- Build 23 mostly removes the visible flicker, but text strokes are still hard to read on the Playdate screen.
- Build 24 keeps build 23's rotation and persistence, then tests 2-pixel vector line width for readability. It was pushed to PDU1-Y024621 on 2026-06-01 00:35 CEST.
- Build 24 made readability worse and raised render cost to about 3.2-3.4 ms, so the 2-pixel stroke should be reverted.
- Build 25 reverts to 1-pixel vectors and changes rotated rendering from independent X/Y scaling to one uniform scale factor. This preserves the emulator coordinate aspect ratio instead of stretching the image to the Playdate's wider screen. It was pushed to PDU1-Y024621 on 2026-06-01 00:40 CEST.
- Build 25 preserves aspect ratio and lowers render cost versus build 24, but text is still not readable enough. Readability is now deferred; full-speed emulation is the priority.
- Build 26 tests a conservative VIA IFR polling-loop skip. It detects the exact `BITA <$0d` + self-branching `BEQ` loop while DP is `$D0`, then fast-forwards VIA/analog time to the next relevant timer interrupt instead of executing repeated polling instructions. It was pushed to PDU1-Y024621 on 2026-06-01 00:47 CEST.
- Build 26 works: `wait_skips` is about 99-104 per 5-second window, `wait_skip_cycles` is about 568k-598k cycles/window, instruction count drops from about 3.5k to about 2.2k instructions/update, and update time drops from about 44-46 ms to about 37 ms.
- ESPboy Vectrex findings: it preserves aspect ratio with one scale factor, uses a 128x128 unrotated framebuffer, draws points for degenerate vectors, keeps one-pixel lines, and does not appear to use persistence. Its main performance knobs are lower render cadence (`PDECAY_INIT 27`), 12,500-cycle emulation chunks (`VECTREX_MHZ_DIV 120`), smaller vector/hash buffers (`VECTOR_CNT_INIT 600`), disabled sound by default, and narrower integer types suited to the ESP8266 toolchain.
- Build 27 decouples emulation update cadence from vector render cadence: update chunks remain 120 Hz / 12,500 cycles, while vector redraw cadence changes to 27 Hz / about 55,555 cycles like ESPboy. It also changes vector storage from cycle-derived capacity to fixed caps: 4096 vectors and an 8191-entry hash. It was pushed to PDU1-Y024621 on 2026-06-01 04:15 CEST.
- Build 27 is a bad visual tradeoff on Playdate: because the emulator only reaches about 0.41M emulated cycles/sec, the 27 Hz emulated-time render cadence becomes only about 7.4 real redraws/sec. It raises update FPS to about 33, but each redraw contains about 700-800 vectors and the beam/dotted-line feel changes.
- Build 28 restores vector render cadence to the 120 Hz update cadence while keeping the useful `avg_render_fps` benchmark field and the smaller fixed vector/hash buffers. It was pushed to PDU1-Y024621 on 2026-06-01 04:20 CEST.
- Build 28 recovers the expected dotted-line visual character: `avg_render_fps` matches `avg_fps` at about 26.2-26.5, render cost stays about 2.4-2.5 ms, and the VIA wait-loop skip still saves about 580k-605k cycles per 5-second window.
- Build 29 tests a direct `$D0xx` VIA read/write fast path in `vecx_read8()` and `vecx_write8()`. Mine Storm's remaining hot direct I/O traffic is mostly VIA register access, so this avoids the full memory-map branch ladder without changing render cadence. It was pushed to PDU1-Y024621 on 2026-06-01 04:26 CEST.
- Build 30 changes the Playdate button mapping: A now presses Vectrex button 3, and B now presses Vectrex button 4. It was pushed to PDU1-Y024621 on 2026-06-01 04:29 CEST.
- Build 30 benchmarks at about 26.8-27.1 FPS with render FPS matching update FPS. The direct `$D0xx` VIA fast path from build 29 appears to be a small win over build 28, but render cost also rose slightly to about 2.6-2.8 ms.
- ESPboy comparison: its biggest apparent speed trick is not a faster CPU core. It runs 12,500-cycle emulation slices, but renders only every `1500000 / 27` emulated cycles. On Playdate, build 27 proved that transplant directly gives only about 7.4 real render FPS at our current emulated throughput and changes the dotted-line visual feel. The remaining useful ESPboy ideas are smaller vector/hash buffers, point drawing for degenerate vectors, no persistence, no sound, and narrower integer state; each needs hardware benchmarking because ARM codegen/cache behavior has already made several "obvious" optimizations regress.
- Build 31 tests the Playdate optimization thread's instruction-cache advice by compiling only `e6809.c` with `-Os` while leaving the rest of the project on `-O3`. The goal is to shrink the large 6809 interpreter hot path without changing emulation/render behavior. It was pushed to PDU1-Y024621 on 2026-06-01 04:41 CEST.
- Build 31 successfully shrinks code: `pdex.bin` drops from 17249 to 12811 bytes, ARM `.text` drops from 31887 to 20292 bytes, and `e6809_sstep` drops from about 20 KB (`0x4f08`) to about 8.5 KB (`0x2164`).
- Build 31 regresses badly on hardware: despite the smaller interpreter, FPS drops to about 18.1-19.4 and update time rises to about 50.7-54.5 ms. The `-Os` rule should be removed; smaller code alone is not enough if the generated interpreter path is slower.
- Build 32 removes the `e6809.c` `-Os` rule and tests the safer ESPboy-style data-footprint change: reduce vector capacity/hash from 4096/8191 to 768/1021 and store hash indices as `int16_t`. It was pushed to PDU1-Y024621 on 2026-06-01 04:44 CEST.
- Build 32 restores `e6809_sstep` to the `-O3` size (`0x4f08`) while cutting ARM BSS from 269724 to 105884 bytes. `pdex.bin` is 17315 bytes.
- Build 32 is neutral to slightly worse versus build 30: about 26.2-26.6 FPS and 36.6-37.3 ms/update. Keep the smaller vector/hash storage for now because it greatly reduces BSS and does not obviously damage visuals, but it is not a speed win.
- Build 33 adds low-overhead Playdate DWT cycle-counter profiling inside `vecx_emu()`. It reports the relative core time spent in 6809 instruction stepping, VIA/analog machine advancement, and wait-loop detection. This is a diagnostic build; compare proportions first, FPS second. It was pushed to PDU1-Y024621 on 2026-06-01 04:49 CEST.
- Build 33 crashes before logging on hardware. Likely cause: direct DWT/debug register access hard-faults on Playdate firmware. Remove the DWT profiling path and do not use raw Cortex-M debug registers for device profiling.
- Build 34 is a recovery build: remove build 33's DWT profiling and restore the build 32 benchmark format/behavior while keeping the smaller vector/hash storage. It was pushed to PDU1-Y024621 on 2026-06-01 04:52 CEST.

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
   - A to Vectrex button 3,
   - B to Vectrex button 4.
7. Done: show FPS with `pd->system->drawFPS(0, 0)`.
8. Done: add low-frequency benchmark logging through `pd->system->logToConsole`.
9. Done: remove SDL source/build code.
10. Next: run on real hardware and iterate using benchmark logs.

## Playdate Rendering Strategy

- Start with Playdate `graphics->clear()` and `graphics->drawLine()`.
- Render through Playdate display inversion so the device shows white vectors and FPS text on a black background.
- Rotate the original portrait Vectrex coordinate space into the Playdate's 400x240 landscape screen.
- Build 23 changes the rotation to -90 degrees for the desired device orientation.
- Build 23 adds vector persistence by caching the previous three vector lists and redrawing them before the current vector list. This should make text and sparse strokes easier to see, at the cost of more `drawLine()` calls per render.
- Build 24 drew vectors with a 2-pixel stroke, but that made readability worse and increased render cost.
- Build 25 uses one uniform integer scale factor after rotation. The fitted Vectrex field is about 297x239 pixels, centered horizontally, preserving the `ALG_MAX_Y:ALG_MAX_X` ratio instead of stretching to 400x240.
- Keep `pd->system->drawFPS(0, 0)` in the Playdate screen's top-left corner.
- ESPboy's renderer also uses a single aspect-preserving scale factor and one-pixel lines. Its text likely benefits from preserving the source geometry; it does not contain a special text reconstruction path.
- Build 39 tests a raw framebuffer renderer for one-pixel vectors, clearing the 1-bit framebuffer directly and plotting Bresenham lines instead of calling `pd->graphics->drawLine()` per vector.
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
  `vecx bench build="..." window_ms=... updates=... renders=... avg_fps=... avg_render_fps=... avg_update_ms=... avg_render_ms=... min_update_ms=... max_update_ms=... emu_cycles=... emu_instr=... avg_instr_update=... avg_cpi=... avg_vectors=... wait_skips=... wait_skip_cycles=... delay_skips=... delay_skip_cycles=... skipped=...`
- Build 13 adds one companion line every benchmark window:
  `vecx opcodes build="..." top=xx:n,xx:n,xx:n,xx:n,xx:n`
- Build 14 adds one more diagnostic line every benchmark window:
  `vecx hotops build="..." beq_taken=... beq_not=... dir95=xx:n,... dir97=xx:n,... a6pb=xx:n,...`

## Known Limitations

- Audio is disabled for the first Playdate milestone.
- Only two of four Vectrex buttons are mapped because the Playdate has A/B plus the D-pad. Current mapping uses buttons 3 and 4.
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
| Playdate C build 11 | PDU1-Y024621 | 12.41-12.61 | 79.00-80.25 ms | 1.12-1.34 ms | 31-94 ms | CrankBoy-style layout pass regressed badly. About 315k emulated cycles/sec. Revert this direction. |
| Playdate C build 12 | PDU1-Y024621 | 13.02-13.09 | 76.01-76.56 ms | 1.45-1.56 ms | 53-84 ms | Fast PC fetch path regressed. About 327k emulated cycles/sec; `pdex.bin` grew to about 17.3 KB. Revert this direction. |
| Playdate C build 13 | PDU1-Y024621 | 13.45-13.58 | 73.13-74.01 ms | 1.38-1.50 ms | 28-90 ms | Opcode profiling shows top opcodes `27`, `95`, `97`, `12`, `a6`; profiler overhead drops runtime versus build 8/9. `pdex.bin` is 16134 bytes. |
| Playdate C build 14 | PDU1-Y024621 | 13.30-13.42 | 74.07-74.89 ms | 1.32-1.49 ms | 27-87 ms | Hot-op profile: `BEQ` taken about 97.5%; `BITA direct` only targets `$0d`; hot `STA direct` targets `$0a`, `$00`, `$01`; hot `LDA indexed` postbytes are `$c0`, `$86`, `$c4`. |
| Playdate C build 15 | PDU1-Y024621 | 15.35-15.55 | 63.97-64.88 ms | 1.49-1.53 ms | 31-75 ms | Branchful `BNE`/`BEQ` plus direct VIA IFR fast path regressed versus build 8/9. `pdex.bin` is 15880 bytes. |
| Playdate C build 16 | PDU1-Y024621 | 11.83-12.17 | 81.90-84.23 ms | 1.03-1.25 ms | 29-101 ms | Branchful `BNE`/`BEQ` alone regressed badly. Revert this direction. `pdex.bin` is 15758 bytes. |
| Playdate C build 17 | PDU1-Y024621 | 15.90-16.26 | 61.13-62.60 ms | 1.09-1.28 ms | 18-78 ms | Recovery confirmed: back to build 8/9 baseline. Reverted branch handling to original `inst_bra8` helper. `pdex.bin` is 15833 bytes. |
| Playdate C build 18 | PDU1-Y024621 | 12.68-12.93 | 77.03-78.60 ms | 1.09-1.27 ms | 30-94 ms | Direct handling for hot `LDA indexed` postbytes `$c0` and `$86` regressed badly. Revert this direction. `pdex.bin` is 15815 bytes. |
| Playdate C build 19 | PDU1-Y024621 | 19.25-19.64 | 50.49-51.58 ms | 0.92-1.04 ms | 16-67 ms | 75 Hz cadence, 20,000 cycles/update. About 390k emulated cycles/sec and 100-104 vectors/render. `pdex.bin` is 15833 bytes. |
| Playdate C build 20 | PDU1-Y024621 | 22.16-22.72 | 43.59-44.78 ms | 0.78-0.93 ms | 15-63 ms | 90 Hz cadence, about 16,666 cycles/update. About 373k-379k emulated cycles/sec and 84-87 vectors/render. `pdex.bin` is 15833 bytes. |
| Playdate C build 21 | PDU1-Y024621 | 27.37-27.82 | 35.43-36.16 ms | 0.66-0.80 ms | 13-51 ms | 120 Hz cadence, 12,500 cycles/update. About 342k-348k emulated cycles/sec and 63-65 vectors/render. `pdex.bin` is 15834 bytes. |
| Playdate C build 22 | PDU1-Y024621 | pending | pending | pending | pending | Rotates output 90 degrees counter-clockwise and uses non-uniform integer scaling to fill about 398x239 pixels. D-pad axes are remapped for the rotated view. Pushed 2026-06-01 00:24 CEST; `pdex.bin` is 15868 bytes. |
| Playdate C build 23 | PDU1-Y024621 | 22.39-23.03 | 43.09-44.36 ms | 2.10-2.21 ms | 14-62 ms | Correct rotation and three-frame persistence. Flicker is mostly gone; text remains hard to read. About 250-259 vectors/render. `pdex.bin` is 16077 bytes. |
| Playdate C build 24 | PDU1-Y024621 | 21.74-22.51 | 44.14-45.68 ms | 3.18-3.41 ms | 14-64 ms | 2-pixel vector strokes made readability worse. Render cost rose by about 1.1 ms versus build 23. About 249-260 vectors/render. `pdex.bin` is 16076 bytes. |
| Playdate C build 25 | PDU1-Y024621 | 21.79-22.64 | 43.93-45.59 ms | 2.47-2.61 ms | 15-63 ms | Aspect-preserving 1-pixel rendering. Render cost improved versus build 24, but text remains unreadable. About 249-265 vectors/render. `pdex.bin` is 16062 bytes. |
| Playdate C build 26 | PDU1-Y024621 | 25.98-26.34 | 36.96-37.79 ms | 2.41-2.53 ms | 1-55 ms | VIA IFR wait-loop skip is active. About 2.2k-2.3k instructions/update, 99-104 skips/window, and 568k-598k skipped cycles/window. `pdex.bin` is 16759 bytes. |
| Playdate C build 27 | PDU1-Y024621 | 32.59-33.20 | 28.01-29.02 ms | 6.05-6.91 ms | 0-51 ms | ESPboy-style decoupled cadence reached only 7.35-7.39 render FPS, with about 694-794 vectors/render. Faster updates, but visually strange and no longer shows the expected beam/dotted-line character. `pdex.bin` is 16784 bytes. |
| Playdate C build 28 | PDU1-Y024621 | 26.21-26.53 | 36.67-37.34 ms | 2.37-2.51 ms | 1-54 ms | Render FPS matches update FPS again and dotted lines are visible. About 2.2k-2.3k instructions/update, 99-104 skips/window, and 579k-605k skipped cycles/window. `pdex.bin` is 16783 bytes. |
| Playdate C build 29 | PDU1-Y024621 | pending | pending | pending | pending | Test direct `$D0xx` VIA fast path while preserving build 28 render cadence and visual behavior. Pushed 2026-06-01 04:26 CEST; `pdex.bin` is 17250 bytes. |
| Playdate C build 30 | PDU1-Y024621 | 26.78-27.08 | 35.84-36.56 ms | 2.61-2.76 ms | 1-53 ms | A/B now map to buttons 3/4. Direct `$D0xx` VIA path is a small speed win versus build 28; dotted-line cadence is preserved. About 2.2k-2.3k instructions/update, 98-107 skips/window, and 587k-610k skipped cycles/window. `pdex.bin` is 17249 bytes. |
| Playdate C build 31 | PDU1-Y024621 | 18.14-19.41 | 50.67-54.50 ms | 2.95-3.17 ms | 1-79 ms | `e6809.c -Os` is a clear runtime regression despite shrinking `pdex.bin` to 12811 bytes and `e6809_sstep` to `0x2164`; remove this rule. |
| Playdate C build 32 | PDU1-Y024621 | 26.21-26.59 | 36.58-37.28 ms | 2.36-2.50 ms | 1-54 ms | Smaller vector/hash storage is neutral/slightly worse versus build 30, but BSS drops to 105884 bytes. About 2.2k-2.3k instructions/update, 99-104 skips/window, and 580k-605k skipped cycles/window. `pdex.bin` is 17315 bytes. |
| Playdate C build 33 | PDU1-Y024621 | crash | crash | crash | crash | DWT cycle-counter profiling build crashes before logs; likely raw debug-register access hard-faults on Playdate firmware. Do not use this profiling path. `pdex.bin` was 17727 bytes. |
| Playdate C build 34 | PDU1-Y024621 | 26.17-26.53 | 36.54-37.27 ms | 2.39-2.50 ms | 1-54 ms | Recovery build removing DWT profiling while keeping build 32's smaller vector/hash storage. Crash is gone; performance matches build 32. About 2.2k-2.3k instructions/update, 100-104 wait skips/window, and 571k-601k skipped cycles/window. `pdex.bin` is 17316 bytes; BSS is 105884 bytes. |
| Playdate C build 35 | PDU1-Y024621 | 25.12-25.75 | 37.87-39.00 ms | 2.39-2.52 ms | 1-56 ms | Sampled profiler costs about 0.8-1.4 FPS versus build 34 but is stable. Samples cluster in BIOS ROM around `f4c7`-`f4ef`; hot opcodes are mostly `97`, `a6`, `12`, `26`, `86`/`27`/`5a`. About 97-99 wait skips/window and 547k-597k skipped cycles/window. `pdex.bin` is 18349 bytes; BSS is 107168 bytes. |
| Playdate C build 36 | PDU1-Y024621 | 24.33-24.94 | 39.21-40.40 ms | 2.37-2.56 ms | 1-59 ms | BIOS delay skip fires, but regresses real speed. It skips 1366-1410 loops/window and 163k-168k cycles/window, reducing instructions/update to about 1.6k-1.7k, but the guard overhead costs more than it saves. Move the expensive interrupt-limit guard behind a cheap PC check before judging this path. `pdex.bin` is 18532 bytes; BSS is 107184 bytes. |
| Playdate C build 37 | PDU1-Y024621 | 25.35-25.96 | 37.62-38.80 ms | 2.37-2.45 ms | 1-55 ms | Moving the BIOS delay guard behind `PC == f4eb` improves build 36 but still regresses versus build 34. The skip fires 1426-1463 times/window and skips 170k-174k cycles/window, but wall time remains worse. Disable this small-grain skip and look for a larger BIOS routine or machine-level batch instead. `pdex.bin` is 18576 bytes; BSS is 107184 bytes. |
| Playdate C build 38 | PDU1-Y024621 | 26.37-26.73 | 36.35-37.10 ms | 2.43-2.58 ms | 1-54 ms | Recovery confirmed. BIOS `f4eb` skip is disabled (`delay_skips=0`) and the proven VIA IFR wait-loop skip remains active. Performance is back in the build 30/34 band, with 100-105 wait skips/window and 580k-604k skipped cycles/window. `pdex.bin` is 18203 bytes; BSS is 107184 bytes. |
| Playdate C build 39 | PDU1-Y024621 | 23.10-23.89 | 40.65-42.50 ms | 0.67-0.72 ms | 1-63 ms | Direct framebuffer drawing cuts measured render time from about 2.5 ms to about 0.7 ms, but total FPS regresses badly. Likely code/layout/cache effects or framebuffer-write side effects slow the emulator core more than the renderer saves. Revert this path for now. `pdex.bin` is 18352 bytes; BSS is 107188 bytes. |
| Playdate C build 40 | PDU1-Y024621 | 26.38-26.75 | 36.37-37.08 ms | 2.47-2.59 ms | 1-53 ms | Recovery confirmed. Direct framebuffer renderer is disabled and Playdate `drawLine()` is restored. Performance is back in the build 38 band, with 100-105 wait skips/window and 580k-604k skipped cycles/window. `pdex.bin` is 18202 bytes; BSS is 107184 bytes. |
| Playdate C build 41 | PDU1-Y024621 | 25.47-26.50 | 37.08-38.93 ms | 0.84-0.93 ms | 1-48 ms | Machine-advance batching is a visual regression. Rendered vectors collapse to about 51-58/render instead of ~250, wait skips drop sharply, and lines render incorrectly. Revert to per-instruction machine advancement despite similar FPS. `pdex.bin` is 18253 bytes; BSS is 107184 bytes. |
| Playdate C build 42 | PDU1-Y024621 | pending | pending | pending | pending | Reverts `VECX_MACHINE_ADVANCE_BATCH` to 1, restoring the accurate per-instruction VIA/analog advancement path from build 40. Pushed 2026-06-01 13:16 CEST; `pdex.bin` is 18202 bytes; BSS is 107184 bytes. |
| Playdate C build 43 | PDU1-Y024621 | 22.23-23.13 | 42.39-44.45 ms | 3.62-3.73 ms | 1-66 ms | Cold-splitting `0x10`/`0x11` page handlers shrinks the binary, but badly regresses real-device speed and render time. Smaller code is not automatically better here; the noinline/cold layout likely hurts cache/branch locality. Revert this direction. `pdex.bin` was 17563 bytes; text was 31056 bytes. |
| Playdate C build 44 | PDU1-Y024621 | 24.05-24.86 | 39.23-40.77 ms | 2.38-2.52 ms | 1-59 ms | Forced-inline page helpers are still a regression versus build 40/42. The source split itself changes optimizer/layout behavior enough to hurt real-device performance. Restore the original in-switch page opcode blocks. `pdex.bin` was 17983 bytes; text was 33013 bytes. |
| Playdate C build 45 | PDU1-Y024621 | 26.38-26.73 | 36.41-37.09 ms | 2.50-2.55 ms | 1-53 ms | Recovery confirmed. Page opcode handlers are restored inside `e6809_sstep()` and performance is back to the build 40/42 baseline. `pdex.bin` is 18202 bytes; text is 33317 bytes. |
| Playdate C build 46 | PDU1-Y024621 | 28.54-29.13 | 33.13-33.98 ms | 2.36-2.57 ms | 1-49 ms | `-falign-functions=32` is a clear win over the 16-byte baseline. No emulator logic changed; FPS improves by about 8-10%, with similar vector counts and render cost. Keep this unless a nearby alignment test beats it. `pdex.bin` is 18240 bytes; text is 33469 bytes. |
| Playdate C build 47 | PDU1-Y024621 | 26.87-27.13 | 35.81-36.42 ms | 3.20-3.37 ms | 1-55 ms | `-falign-functions=64` is a regression versus build 46. FPS falls back near the pre-align32 baseline and render cost rises by about 0.7-1.0 ms. Return to build 46's 32-byte function alignment. `pdex.bin` is 18237 bytes, text is 33725 bytes. |
| Playdate C build 48 | PDU1-Y024621 | 28.44-29.12 | 32.98-34.19 ms | 2.40-2.59 ms | 1-49 ms | Recovery confirmed. `-falign-functions=32` returns to the build 46 performance band and remains the best baseline. `pdex.bin` is 18240 bytes, text is 33469 bytes. |
| Playdate C build 49 | PDU1-Y024621 | 27.92-28.58 | 33.85-35.01 ms | 3.36-3.55 ms | 1-51 ms | `-falign-functions=32 -falign-jumps=32` regresses versus build 48. FPS drops slightly and render time rises by about 0.8-1.1 ms, likely from extra code size/cache pressure. Revert to plain `-falign-functions=32`. `pdex.bin` is 18580 bytes, text is 35881 bytes. |
| Playdate C build 50 | PDU1-Y024621 | 28.43-29.09 | 33.06-34.06 ms | 2.43-2.55 ms | 1-50 ms | Recovery confirmed. Plain `-falign-functions=32` returns to the build 48 performance band and remains the current baseline. `pdex.bin` is 18240 bytes, text is 33469 bytes. |
| Playdate C build 51 | PDU1-Y024621 | 27.24-27.84 | 34.77-35.82 ms | 2.47-2.62 ms | 1-52 ms | `-falign-functions=32 -falign-loops=32` regresses versus build 50. Loop alignment adds little code size, but it still worsens update time by about 1.7-2.0 ms. Revert to plain `-falign-functions=32`. `pdex.bin` is 18274 bytes, text is 33625 bytes. |
| Playdate C build 52 | PDU1-Y024621 | 28.54-29.13 | 33.13-34.04 ms | 2.40-2.59 ms | 1-50 ms | Recovery confirmed. Plain `-falign-functions=32` returns to the build 50 performance band. `pdex.bin` is 18240 bytes, text is 33469 bytes. |
| Playdate C build 53 | PDU1-Y024621 | 25.15-25.76 | 37.93-39.08 ms | 3.05-3.25 ms | 1-57 ms | Fast 6809 PC fetch is a clear regression. It saves `vecx_read8()` calls for opcode/immediate fetches, but the extra inline address tests and larger interpreter hurt cache/layout badly. Revert to the baseline `pc_read8()` path. `pdex.bin` is 19621 bytes, text is 34945 bytes. |
| Playdate C build 54 | PDU1-Y024621 | 28.46-29.08 | 33.07-34.16 ms | 2.43-2.60 ms | 1-49 ms | Recovery confirmed. Baseline `pc_read8()` plus plain `-falign-functions=32` returns to the build 52 performance band after the PC-fetch regression. `pdex.bin` is 18240 bytes, text is 33469 bytes. |
| Playdate C build 55 | PDU1-Y024621 | pending | pending | pending | pending | Tests render frame skipping: emulate every 120 Hz slice, but draw only every second Vectrex frame. Intended to measure whether saving render work improves emulated cycles/sec enough to justify lower visual cadence. Local build succeeds and was pushed on 2026-06-01 18:54 CEST; `pdex.bin` is 18205 bytes, text is 33277 bytes. Device copy verified by `cmp`, with no `._*` files found. |

## Next Steps

- Test build 55 against build 54. Direct PC fetch regressed, so keep the smaller interpreter shape and measure a coarse render-skip tradeoff before attempting more invasive BIOS/vector traps.
- Full-speed emulation target is still far away: build 34 gets through roughly 327k-333k emulated cycles/sec of 1.5MHz Vectrex time, so we still need about a 4.5x gain.

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
