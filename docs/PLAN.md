# Plan: BRK Repeat for the Bastl Citadel

Full findings: https://claude.ai/artifact/P445D2L1zAFJhUYGpuDzLZ (2026-11-23).

**Goal (BRAEK, 2026-11-23):** a beat repeater / micro sound repeater firmware for the Citadel, with added features.
Uses the `dsp-research` skill (BRAEK: yes).

## Platform
RP2040 (2 cores, 176 MHz, 264 kB RAM, no FPU), 8 MB flash, 44.1 kHz / 16-bit stereo. 7 knobs, 2 buttons, 3 RGB LEDs,
clock in / out, gates, CV ins (0-5 V), 3 CV outs, USB MIDI. Code: Pico SDK + C++ (Bastl's Kastle 2 framework, MIT).
Limit: about 1.2 s stereo / 2.4 s mono of audio memory (LAZERS REPEAT allows 4 s).

## Status
- Phase 0 done (2026-11-23). Phase 1 + 2 built as BRK Repeat 0.1, waiting for BRAEK's hardware test.
- The Phase 1 repeater is a textbook ring-buffer slicer (no new DSP), so no `dsp-research` run was needed yet;
  use it for Phase 3 ideas.

## Phases
0. Setup: fork, cloud build, BRAEK flashes stock FX Wizard from our build (proves build and the way back).
1. First playable repeater: clock-synced buffer, TIME, REPEATS, POSITION, MIX, HOLD, click-free crossfades.
2. BRAEK character, ported from LAZERS REPEAT in fixed point: BRAEK, WONK, DAMAGE, FREEZE vs HOLD; CV and gates; LEDs.
3. Added features (BRAEK chooses): rolls that speed up / slow down, pitch ramp, probability, stereo spread,
   clock out per repeat, USB MIDI, saved settings.
4. Release: controls map, panel overlay, manual, version, community app ID.
