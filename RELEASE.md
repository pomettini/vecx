# CrankTrex — Release Checklist (v1.0.0)

CrankTrex is the Playdate release of this `vecx` Vectrex emulator port. This is the
step list to get from the current dev state to a polished public v1.0.0. Grouped by
workstream; ordered roughly by dependency (rename + picker are low-risk foundations;
controls + adaptive retune are the meaty ones).

## Guiding principles (standing constraints)

- **Best performance on BOTH Rev A (STM32F746) and Rev B (STM32H7); no regression to either.** Every perf-affecting change is validated on both. See the hardware-comparison section in [NOTES.md](NOTES.md) for the baseline (Rev B ≈ 1.33–1.41× Rev A).
- **Rev A can only be tested in idle/boot regimes** — its input digitizer failed, so active-play validation is Rev B-only. Rev A's job now is "no-regression on idle/boot".
- **The DTCM hot-core relocation and the `itcm D1–D6` probe logs are load-bearing layout.** Removing the probes or reordering e6809.c / vecx.c / playdate_main.c shifts the whole-binary I-cache packing (cost ~8% once). Any such edit is a perf change — re-measure on both devices.
- **Device-only builds; always test on hardware, never the simulator.**

---

## 1 — Rename to CrankTrex

- [ ] `Source/pdxinfo`: `name=CrankTrex`, `bundleID=com.pomettini.cranktrex`, update `description`, add `version=1.0.0` and `buildNumber=1`, set `imagePath=` to the card art (WS7).
- [ ] `Makefile`: `PRODUCT = CrankTrex.pdx`.
- [ ] `BUILD_LABEL` / the `"vecx-render"` build string and the `"vecx: …"` console prefixes → CrankTrex (cosmetic; touches playdate_main.c → re-check the FPS band after).
- [ ] README title + all repo references; rename the GitHub repo + update `.gitmodules`/remotes if desired.
- [ ] Keep the ROM path `/Shared/Emulation/vec/games/` unless aligning with the PokeMini/FamiCrank convention — decide and document in the README.

## 2 — Controls: Hybrid mapping (crank flicks + menu remap)

Vectrex has 4 buttons; Playdate has A, B, crank. Chosen approach = both.
- [ ] **Crank flick → buttons 1 & 2:** forward flick = button 1, backward = button 2, momentary pulse, debounced (one flick = one press). Read via `getCrankChange()`; threshold + short hold.
- [ ] **Crank-docked aware:** no flicks fire while the crank is docked (`isCrankDocked`).
- [ ] **Menu remap item** for the A/B pair (e.g. "A/B → 3+4 / 4+3 / 1+2"), so players can match whichever buttons a given game uses. Shares the swappable menu slot design already used for Sound/ROM-Picker, or add a dedicated item.
- [ ] Confirm the sensible **default** during testing: A = primary fire (Mine Storm fire is button 4), B = secondary. Current code is A→3 / B→4 — verify and set the good default.
- [ ] Update `update_input()` in playdate_main.c (linked first → re-check FPS band after).

## 3 — Integrate pd-rom-picker upstream — CODE DONE, needs device test

Submodule was ~5 commits behind: max ROMs 1024, heap-allocated file list, left/right page scrolling, UTF-8 filenames, robustness fixes.
- [x] `git submodule update --remote pd-rom-picker` → `dae3284`. Public API unchanged, so `rom_picker_unit.c` glue needed no edits.
- [x] **Build fix required:** the new heap-allocated file list is the first thing in the project to call `calloc`/`free`, which exposed a latent LTO bug — the SDK's `setup.c` overrides newlib's allocator hooks (`_malloc_r` → `system->realloc`), but under `-flto` those are slim IR and the plugin drops them (their only consumer, `libc.a`, is not an LTO unit), so the link failed with `undefined reference to _malloc_r`. Fixed in the Makefile with `-Wl,-u,_malloc_r -Wl,-u,_free_r -Wl,-u,_realloc_r`.
- [ ] Device-test: large ROM folder (page scroll L/R), UTF-8/accented filenames, empty-folder first-run.

## 4 — Performance parity (Rev A + Rev B) — CODE DONE, needs device test

- [x] Retuned the adaptive controller: thresholds `ADAPTIVE_GROW_MS 16` / `ADAPTIVE_SHRINK_MS 18` (were 14/17). One pair, correct on both devices: Rev B's light scenes can now climb to `EMU_CYCLES_NATIVE`; Rev A's heavy scenes run 26–30 ms — far above SHRINK — so they stay pinned at the floor exactly as before.
- [ ] Validate: Rev B full active play (Rip-Off should reach `adaptive=30000`); Rev A idle/boot shows no regression.
- [ ] Add the both-devices validation rule + load-bearing caveats to a `CLAUDE.md` (create it) so it survives future sessions.

## 4b — Auto frameskip — CODE DONE, needs device test

- [x] Frameskip menu is now **`Auto` / `0` / `1`**, defaulting to Auto (FamiCrank parity). Auto rides the same smoothed update-time signal as the adaptive controller: it engages at ≥19 ms and releases at ≤16 ms (hysteresis). Rationale: once we miss the 50 Hz deadline the display frame is dropped anyway, so drawing every Vectrex frame only steals time from emulation.
- [ ] Watch for interaction with the adaptive controller (skip lowers update time → controller may grow the budget → equilibrium). Confirm it settles rather than oscillating visibly.

## 5 — Settings persistence — CODE DONE, needs device test

- [x] Rotation / Frameskip / Sound / last-played ROM persist to `settings.cfg` in the data folder; restored before the menu is built so items come up with stored values. Written on every menu change (system menu is open, so no gameplay stall) and on ROM select. Corrupt/unknown keys degrade to defaults (every setter range-checks).
- [x] Last-played ROM auto-resumes at launch **only if the file still exists**; otherwise boots to the picker. The picker stays one menu item away.
- [ ] Reconsider the **Sound default** — currently OFF (a Rev A CPU-saver); Rev B has headroom. Decide default; keep the menu toggle either way.

## 6 — BIOS: bundle it (decision made)

- [ ] Ship `rom.dat` (Vectrex BIOS; contains Mine Storm) in the `.pdx` as today.
- [ ] README **Legal** section disclaims it: not affiliated with GCE/Vectrex or Panic; BIOS/Mine Storm © their owners; provided for interoperability; source is GPLv3.

## 7 — Launcher art & release assets

- [ ] Game **icon** and **card image** (Playdate launcher); set `imagePath` in pdxinfo. Optional launch animation / `launchSoundPath`.
- [ ] README screenshots — `screenshot0.png` / `screenshot1.png` already exist; refresh if the rename changes on-screen text.

## 8 — README (PokeMini / FamiCrank family style)

Sections in order (verbatim family template): **Install → Adding ROMs → Saves → Controls → Options → Credits → License → Legal.**
- [ ] Controls: a two-column table (D-pad → joystick; A/B → buttons; crank fwd/back → buttons 1/2).
- [ ] Options: Sound, Rotation, Frameskip, ROM Picker, A/B remap.
- [ ] Credits: Valavan Manohararajah (original vecx), the MAME team (AY-3-8910 sound core), pd-rom-picker, port by Pomettini, and the AI-assist disclosure your family READMEs carry.
- [ ] License: GPLv3 (link to LICENSE.md).

## 9 — Versioning & housekeeping

- [ ] Set `version=1.0.0` / `buildNumber` in pdxinfo; tag the repo `v1.0.0`.
- [ ] Dev-scaffolding decision: gate the 5-second benchmark console log behind a flag; confirm `VECX_SAMPLE_PROFILE` / `VECX_HLE_CAPTURE` are off; decide disposition of `jit.c` (documented reference) and `tools/`. **Do NOT strip the itcm probes** (load-bearing).
- [ ] Verify GPLv3 compliance: source public (✓), LICENSE present (✓), credits complete.

## 10 — Release gate (definition of done)

- [ ] Full testing matrix on both devices (Rev A idle/boot only): 4KB + 8KB carts, an overlay/color game, built-in Mine Storm, ROM picker at scale (page scroll, UTF-8, empty state), all rotations, Sound on/off.
- [ ] **Soak test:** long unattended run per device — confirm the old F410 HLE watchdog freeze stays dead.
- [ ] Controls verified per-game (primary fire feels right; crank flicks reliable; docked handled).
- [ ] Clean build, correct name/version everywhere, README + Legal final, tag pushed.
