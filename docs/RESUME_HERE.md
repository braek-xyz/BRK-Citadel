# Resume here

**Last session: 2026-11-23**

Done:
- Platform research (`docs/PLAN.md`), fork `braek-xyz/BRK-Citadel`, cloud build (PR #1, merged; run green).
- **BRK Repeat 0.1** (Phase 1 + most of Phase 2): `code/src/apps/BrkRepeat/`. Fixed-point ring-buffer repeater:
  TIME (synced / free), REPEATS 1-16, POSITION, BRAEK, WONK, DAMAGE, MIX, FREEZE (cycle-synced), HOLD (instant),
  CV on the main controls, gate out per repeat, LEDs. Controls named for the Wave Bard panel (`docs/BRK-REPEAT.md`).
- Engine host tests: `code/tests/brk_repeat_test.cpp`, 12 checks, 0 failed (passthrough bit-exact, slicing,
  previous-cycle read, freeze loop, hold loop, fuzz under ASan/UBSan, click test: largest step 251 on a 110 Hz sine).
- Firmware builds: flash 279 KB (53 %), static RAM 219 KB (89 %; FX Wizard needs about 232 KB incl. its heap delays).

Not tested: nothing on a Citadel yet. CPU load, knob feel, clock sync, CV ranges, LEDs and gate out are untested.

Hardware test 1 (BRAEK, 0.1): LEDs run (LED 2 white cycle flashes) but no sound heard; LED 1 flashed red =
HOLD switching on by itself. Cause found: TRIG in is normalled to an internal gate on the Citadel. 0.2 removes
TRIG -> HOLD and RESET -> FREEZE. Hardware test 2 (0.2): silence was the Citadel's stored INPUT level (SHIFT + PITCH MOD), left low from Wave Bard use.
After raising it, levels OK. Manual should tell users to check SHIFT + PITCH MOD / SHIFT + PITCH first.

Hardware test 3 (0.2): knobs did nothing; HOLD worked at a fixed length. Cause: FancyPot values update only in
ReadValue(), which must be called from UiLoop() (as FX Wizard does). Fixed in 0.3.

Hardware test 4 (0.3): BRAEK: "sounds really bad", no control over the synced time; wants it to behave like the ER-301
brk repeat (and the feel of a Data Bender, without copying it). BRAEK's clock: 1/4 notes (1 pulse per beat), ~110 bpm.
Decisions: stereo, full quality (longest cycle ~0.56 s; synced lengths halve to fit); keep WONK; BRAEKBEATZ effects
may be added later (BRAEK's own code; which ones still to ask; dsp-research for new DSP).

**BRK Repeat 0.4** (this session): the engine is now a port of the ER-301 engine (BRK-ER301 `dsp/RepeatEngine.cpp`
= LAZERS REPEAT): block logic in double, line for line; per-sample read / record / crossfades / DAMAGE / mix in fixed
point. Engine runs one 48-sample block per audio loop. TIME: the ten LAZERS notes (synced) or 12.5 ms..0.56 s (free).
SHIFT + SAMPLE = CLOCK: FREE | 1 | 2 | 4 | 24 pulses per beat (default 1). HOLD / FREEZE as on the ER-301.
- Host test: `code/tests/brk_repeat_test.cpp` runs it next to the ER-301 engine (copied in `code/tests/er301_reference`):
  20 checks, 0 failed; outputs within about -56 to -86 dB of the ER-301 engine (DAMAGE at settings with fractional
  hold counts differs by single-sample hold timing, -25 dB, compared loosely).
- Build: flash 291 KB, static RAM 228 KB (93 %; 0.3 ran with 219 KB). `rdMap` runs from RAM (`.time_critical`),
  the block function from flash (compiled -Os; -O3 made it 18 KB).
- Not tested: CPU load on the chip (no emulator here; estimate well under the 1.09 ms per block), heap headroom,
  clock sync on hardware, sound.

Next:
1. BRAEK flashes 0.4 and reports: sound vs the ER-301, TIME note steps at 110 bpm, CLOCK setting, HOLD / FREEZE,
   any crackle (= CPU) or failure to start (= memory).
2. Ask BRAEK which BRAEKBEATZ effects to bring in, and what about the Data Bender feel matters most.
3. Later: ask Bastl to add app ID 0x42 "BRK Repeat" to APP_LIST.md (community list).
