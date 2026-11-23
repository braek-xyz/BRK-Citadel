# Provenance

| What | Source | Licence | Notes |
|---|---|---|---|
| Everything outside our own additions | Bastl Instruments, github.com/bastl-instruments/kastle2 (forked 2026-11-23 at a4a7a04) | Code MIT; schematics / docs CC BY-SA 4.0; panel art and samples all rights reserved | Unchanged. Never ship SAMPLES.BIN. |
| `.github/workflows/build.yml`, `CLAUDE.md`, `docs/PLAN.md`, `docs/RESUME_HERE.md`, this file | BRAEK (written for this repo) | BRAEK's own | |
| `code/src/apps/BrkRepeat/AppBrkRepeat.*`, `main.cpp`, `CMakeLists.txt`, `code/tests/brk_repeat_test.cpp`, `docs/BRK-REPEAT.md` | BRAEK (written for this repo, 2026-11-23). `main.cpp` and `CMakeLists.txt` follow the structure of Bastl's Template app (MIT). The clock and HOLD handling follow BRAEK's ER-301 unit (BRK-ER301 `units/brk_repeat/BrkRepeat.cpp`). | MIT | Uses Bastl's framework (FancyPot, Clock, LEDs) unchanged. |
| `code/src/apps/BrkRepeat/RepeatEngine.hpp` (0.4 on) | Ported from BRAEK's own BRK-ER301 `dsp/RepeatEngine.cpp` + `dsp/DspCommon.h` (branch `claude/project-thread-8bw4uq`, commit 2ab214f), which is LAZERS 2.0.0 `engine/Engine.cpp` struct `Engine::Master::Repeat` (BRAEK's own code). Control logic kept line for line; per-sample part rewritten in fixed point (differences listed at the top of the file). | BRAEK's own; MIT in this repo | |
| `code/tests/er301_reference/RepeatEngine.cpp`, `RepeatEngine.h`, `DspCommon.h` | BRAEK's own BRK-ER301 `dsp/` (same commit 2ab214f), copied unchanged, used only by the host test as the reference | BRAEK's own | Not part of the firmware. `RefWrap.*` written for this repo. |
| Build tools (downloaded at build time, not in the repo) | xPack Arm GCC 14.2.1-1.1; Raspberry Pi Pico SDK 2.1.1 | GCC: GPL with runtime exception (tool only); Pico SDK BSD-3 | |

Changes to Bastl files: none yet.
