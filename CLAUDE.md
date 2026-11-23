# BRK-Citadel (BRAEK firmware for the Bastl Citadel)

BRAEK (Victoria, Australia) makes audio plug-ins. This repository is a fork of Bastl's Kastle 2 code
(`bastl-instruments/kastle2`, MIT) and adds **BRK Repeat**, a beat repeater / micro sound repeater firmware for the
Citadel (Eurorack Kastle 2; same firmware files). Read `docs/RESUME_HERE.md` first, then `docs/PLAN.md`.

## Working with BRAEK
- Plain English, short replies. BRAEK is learning, has no Linux and does not use a terminal.
- BRAEK gets the finished `.uf2` from the GitHub Actions run ("Build firmware"), Artifacts section of the run page,
  and flashes it by drag-and-drop (bootloader mode). BRAEK owns a Citadel Wave Bard.
- Ask before big or irreversible steps; give a recommended option.
- Never claim a build or test passed without its exit code / log output. Say what was and was not tested.
  A cloud session has no Citadel and cannot hear anything: hardware tests are BRAEK's.
- Never print, commit or upload a key, certificate, password or token.

## Rules
- Our code goes in `code/src/apps/BrkRepeat/` (and our own helpers next to it). Keep Bastl's files unchanged where
  possible so `Sync fork` stays easy; any change to a Bastl file is listed in `PROVENANCE.md`.
- LAZERS source is read-only reference (BRAEK's own code, porting is fine); record every ported idea/file in `PROVENANCE.md`.
- Outside code only if permissive and recorded in `PROVENANCE.md`. No GPL / LGPL / AGPL.
- Never build or ship `wave-bard-with-samples` / `SAMPLES.BIN` (Bastl: all rights reserved). Never copy Bastl panel art.
- RP2040 audio code: no allocation, locks, flash or file access in `AudioLoop()`. No FPU: use the q15 / fixed-point
  helpers in `code/src/common/dsp/math`. RAM is about 240 kB: about 1.2 s of stereo 16-bit audio at most.
- Small changes on a branch with a PR; update `docs/RESUME_HERE.md` at the end of every session.

## Build (cloud / Linux)
Arm GCC 14.2.1+ (xPack, see `.github/workflows/build.yml`) and Pico SDK 2.1.1 (`PICO_SDK_PATH`), then
`cmake -S code -B code/build && cmake --build code/build --target brk-repeat fx-wizard`. Output: `code/build/output/*.uf2`.
Engine tests (host): `g++ -std=c++20 -fsanitize=address,undefined -I code/src -I code/tests/er301_reference code/tests/brk_repeat_test.cpp code/tests/er301_reference/RefWrap.cpp` (prints "N checks, 0 failed"; compares with the ER-301 engine).
Engine: `code/src/apps/BrkRepeat/RepeatEngine.hpp` (plain C++, port of the ER-301 / LAZERS REPEAT engine; keep it matching `code/tests/er301_reference`); Citadel wrapper: `AppBrkRepeat.cpp`. Manual: `docs/BRK-REPEAT.md`.

## BRAEK's standing habits (do these without being asked)
- End of every session: ask "Shall I update `docs/RESUME_HERE.md` now? (recommended: yes)".
- End of every phase / release: remind BRAEK to run `/collect-lessons BRK-Citadel` in a PLUGIN-WORKFLOW session.
- New substantial DSP idea: use the `dsp-research` skill (BRAEK said yes for this project, 2026-11-23).
