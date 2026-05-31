# vecx Playdate Port Notes

## Current Status

- Goal: port `vecx` to a Playdate-only, C-only project.
- Priority: smoothness and speed on real Playdate hardware before emulation accuracy.
- Initial Playdate C build succeeds locally and produces `vecx.pdx`.
- The first Playdate milestone has visible vector rendering, D-pad/A/B input, FPS display, and low-frequency benchmark logs.
- Audio is deferred. `e8910.c` is currently a no-op sound stub so the project has no SDL dependency.
- The Playdate update callback targets 30 Hz, matching the core vector redraw cadence (`VECTREX_MHZ / 30` cycles per update).

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
   - optionally loads a local `cart.vec` if present in the PDX,
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
- Render black lines on a white background for maximum legibility and visible FPS text.
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
  `vecx bench build="..." window_ms=... updates=... renders=... avg_fps=... avg_update_ms=... avg_render_ms=... min_update_ms=... max_update_ms=... skipped=...`

## Known Limitations

- Audio is disabled for the first Playdate milestone.
- Only two of four Vectrex buttons are mapped initially because the Playdate has A/B plus the D-pad.
- The first renderer is correctness-oriented and simple, not the final fast path.
- No real-device benchmark data has been collected yet.
- Local build warnings:
  - Simulator linking warns about reducing `__DATA,__common` alignment because the emulator keeps large static vector buffers.

## Benchmark History

| Build | Device | Avg FPS | Avg Update | Avg Render | Min/Max Update | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Initial SDL code | n/a | n/a | n/a | n/a | n/a | Desktop-only baseline before Playdate port. |
| Playdate C build 1 | local build only | n/a | n/a | n/a | n/a | `make` succeeds and packages `vecx.pdx`; no hardware benchmark yet. |

## Next Steps

- Install `vecx.pdx` on a real Playdate with `make _push`.
- Confirm whether BIOS-only Mine Storm reaches visible gameplay.
- Paste the first 5-second benchmark log into this file.
- Decide whether the next optimization target is frame pacing, renderer cost, or input/cart loading.
