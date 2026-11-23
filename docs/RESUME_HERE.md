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
TRIG -> HOLD and RESET -> FREEZE. Still open: confirm audio IN was patched, and whether stock FX Wizard passes sound.

Next:
1. BRAEK flashes `kastle2-brk-repeat.uf2` and reports: does it pass audio, do REPEATS / TIME / HOLD / FREEZE work,
   any clicks, LED colours, clock sync.
2. Fix what BRAEK finds. Then Phase 3 "added features" (BRAEK chooses; `dsp-research` for anything new).
3. Later: ask Bastl to add app ID 0x42 "BRK Repeat" to APP_LIST.md (community list).
