# BRK Repeat for the Citadel

A beat repeater / micro sound repeater. It keeps a short memory of the incoming sound and plays a slice of it back
over and over, in time with your clock. Add random gaps and jumps (BRAEK), random speed and direction (WONK) and
crunch (DAMAGE), or freeze a loop. It is the REPEAT effect from LAZERS, the same engine as the ER-301 "brk repeat"
unit, rebuilt for the Citadel's chip.

**Version 0.4, test build.** New in 0.4: the engine is now the LAZERS / ER-301 one (0.1 to 0.3 used a simpler
engine of its own), TIME has the ten LAZERS note lengths, and SHIFT + SAMPLE sets how many clock pulses make a beat.

## Install
1. Download `brk-citadel-firmware` from the latest green run of "Build firmware" on GitHub (Actions tab), unzip it.
2. Power off, connect the Citadel by USB-C, hold **SHIFT** while powering on: it appears as a USB drive.
3. Drag **`kastle2-brk-repeat.uf2`** onto the drive. It restarts on its own.

**No sound?** Hold SHIFT and turn PITCH MOD up (input level), and SHIFT + PITCH (output level). Coming from
Wave Bard these may be turned down. Then set SAMPLE, PITCH MOD and SAMPLE MOD fully left: you should hear the input unchanged.

To go back: the same steps with Bastl's official Wave Bard or FX Wizard file. Your Wave Bard samples are stored
separately and are not touched, but Wave Bard settings may reset when you switch firmware.

## Controls (names printed on the Wave Bard panel)

| Panel name | BRK Repeat | What it does |
|---|---|---|
| **PITCH** knob | **TIME** | Length of one cycle (the stretch of time that gets sliced). Synced, in ten steps from left to right: 1/16, 1/8, dotted 1/8, 1/4, dotted 1/4, 1/2, dotted 1/2, 1 bar, 1.5 bars, 2 bars. A length that does not fit the memory (about 0.56 s) is halved until it fits, so it stays on the beat (at 110 bpm: up to a 1/4 note). Free: 12.5 ms to 0.56 s. |
| **SAMPLE** knob | **REPEATS** | How many slices the cycle is cut into, 1 to 16. 1 = the live sound. Higher = faster stutters. |
| **LENGTH** knob | **POSITION** | Which slice repeats. Left = the first, right = the last. |
| **PITCH MOD** knob | **BRAEK** | Random gaps (silence) and jumps to other slices. |
| **SAMPLE MOD** knob | **WONK** | Random speed and direction per repeat: backwards, half, double, a fifth up... |
| SHIFT + **SAMPLE MOD** | **DAMAGE** | Lower sample rate and bit depth of the repeats. |
| SHIFT + **LENGTH** | **MIX** | Dry / wet. Starts fully wet (REPEATS 1 is the live sound anyway). |
| SHIFT + **SAMPLE** | **CLOCK** | Five zones, left to right: FREE time (LED 2 glows purple), then synced with **1**, **2**, **4** or **24** clock pulses per beat. Starts at 1 (a 1/4-note clock). LED 2 blinks green on every beat, so you can check it. |
| **MODE** button, held | **HOLD** | Freezes at once and loops the current cycle while you hold it; lets go at once. |
| SHIFT + tap **MODE** | **FREEZE** on / off | Holds the current cycle and loops it. Starts and stops at the end of a cycle, so it stays in time. Turning TIME, REPEATS and POSITION while frozen re-slices the frozen loop. |
| LFO, LFO MOD, tempo, input / output level, clock in / out | as in Bastl's firmwares | SHIFT + LFO = tempo, SHIFT + PITCH MOD = input level, SHIFT + PITCH = output level. |

## Inputs and outputs

| Panel jack | BRK Repeat |
|---|---|
| PITCH CV | adds to TIME |
| NOTE | adds to REPEATS |
| LENGTH CV | adds to POSITION |
| SAMPLE CV | adds to BRAEK |
| BANK CV | adds to WONK |
| TRIG in, RESET in | not used yet (they carry internal gates when unpatched) |
| GATE out | a short pulse at every repeat |
| Clock / sync in | tempo and beat position for synced TIME. With nothing patched, the Citadel's own tempo (SHIFT + LFO) is used. |

## LEDs
- **LED 1**: green = live sound, orange = repeating, blue = frozen, red = hold. It flashes brighter at every repeat.
- **LED 2**: white flash at every cycle start, dim green blink on every beat of the clock. Purple glow = free time.

## Patch ideas
- **Beat-repeat fill:** synced, TIME 1/4, REPEATS 8. Hold MODE on the last beat of a phrase.
- **Glitch drums:** REPEATS 4 to 8, BRAEK and WONK about 10 o'clock, a little DAMAGE.
- **Roll that speeds up:** a rising envelope into NOTE (REPEATS) from 1 to 16.
- **Pick the slice from a sequencer:** CV into LENGTH CV (POSITION).
