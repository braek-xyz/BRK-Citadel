# Provenance

| What | Source | Licence | Notes |
|---|---|---|---|
| Everything outside our own additions | Bastl Instruments, github.com/bastl-instruments/kastle2 (forked 2026-11-23 at a4a7a04) | Code MIT; schematics / docs CC BY-SA 4.0; panel art and samples all rights reserved | Unchanged. Never ship SAMPLES.BIN. |
| `.github/workflows/build.yml`, `CLAUDE.md`, `docs/PLAN.md`, `docs/RESUME_HERE.md`, this file | BRAEK (written for this repo) | BRAEK's own | |
| `code/src/apps/BrkRepeat/*`, `code/tests/brk_repeat_test.cpp`, `docs/BRK-REPEAT.md` | BRAEK (written for this repo, 2026-11-23). Design follows BRAEK's own LAZERS REPEAT + FREEZE (controls, slicing, BRAEK / WONK / DAMAGE ideas, WONK speed set), rewritten from scratch in fixed point; no LAZERS code copied. `main.cpp` and `CMakeLists.txt` follow the structure of Bastl's Template app (MIT). | MIT | Uses Bastl's framework (FancyPot, Clock, LEDs) unchanged. |
| Build tools (downloaded at build time, not in the repo) | xPack Arm GCC 14.2.1-1.1; Raspberry Pi Pico SDK 2.1.1 | GCC: GPL with runtime exception (tool only); Pico SDK BSD-3 | |

Changes to Bastl files: none yet.
