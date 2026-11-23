# BRK Repeat for the Citadel

A beat repeater / micro sound repeater. It keeps a short memory of the incoming sound and plays a slice of it back
over and over, in time with your clock. Add random gaps and jumps (BRAEK), random speed and direction (WONK) and
crunch (DAMAGE), or freeze a loop. Same idea as the REPEAT effect in LAZERS.

**Version 0.1, first test build.** Not yet tried on hardware.

## Install
1. Download `brk-citadel-firmware` from the latest green run of "Build firmware" on GitHub (Actions tab), unzip it.
2. Power off, connect the Citadel by USB-C, hold **SHIFT** while powering on: it appears as a USB drive.
3. Drag **`kastle2-brk-repeat.uf2`** onto the drive. It restarts on its own.

To go back: the same steps with Bastl's official Wave Bard or FX Wizard file. Your Wave Bard samples are stored
separately and are not touched, but Wave Bard settings may reset when you switch firmware.

## Controls (names printed on the Wave Bard panel)

| Panel name | BRK Repeat | What it does |
|---|---|---|
| **PITCH** knob | **TIME** | Length of one cycle (the stretch of time that gets sliced). Synced: 1/32 note, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars (halved if it does not fit the 1.1 s memory). Free: 12.5 ms to 1.1 s. |
| **SAMPLE** knob | **REPEATS** | How many slices the cycle is cut into, 1 to 16. 1 = the live sound. Higher = faster stutters. |
| **LENGTH** knob | **POSITION** | Which slice repeats. Left = the first, right = the last. |
| **PITCH MOD** knob | **BRAEK** | Random gaps (silence) and jumps to other slices. |
| **SAMPLE MOD** knob | **WONK** | Random speed and direction per repeat: backwards, half, double, a fifth up... |
| SHIFT + **SAMPLE MOD** | **DAMAGE** | Lower sample rate and bit depth of the repeats. |
| SHIFT + **LENGTH** | **MIX** | Dry / wet. Starts fully wet (REPEATS 1 is the live sound anyway). |
| SHIFT + **SAMPLE** | **SYNC / FREE** | Left half: TIME follows the clock. Right half: free time (LED 2 glows purple). |
| **MODE** button, held | **HOLD** | Instantly loops the last slice you heard, as long as you hold it. |
| SHIFT + tap **MODE** | **FREEZE** on / off | Holds the current cycle and loops it. Starts and stops at a cycle start, so it stays in time. |
| LFO, LFO MOD, tempo, input / output level, clock in / out | as in Bastl's firmwares | SHIFT + LFO = tempo, SHIFT + PITCH MOD = input level, SHIFT + PITCH = output level. |

## Inputs and outputs

| Panel jack | BRK Repeat |
|---|---|
| PITCH CV | adds to TIME |
| NOTE | adds to REPEATS |
| LENGTH CV | adds to POSITION |
| SAMPLE CV | adds to BRAEK |
| BANK CV | adds to WONK |
| TRIG in | HOLD while the gate is high |
| RESET in | FREEZE while the gate is high |
| GATE out | a short pulse at every repeat |
| Clock / sync in | tempo; cycles start on the beat when synced |

## LEDs
- **LED 1**: green = live sound, orange = repeating, blue = frozen, red = hold. It flashes brighter at every repeat.
- **LED 2**: white flash at every cycle start. Purple glow = free time.

## Patch ideas
- **Beat-repeat fill:** synced, TIME 1/2, REPEATS 8. Gate into TRIG on the last beat of every 4 bars.
- **Glitch drums:** REPEATS 4, BRAEK and WONK about 10 o'clock, a little DAMAGE.
- **Roll that speeds up:** a rising envelope into NOTE (REPEATS) from 1 to 16.
- **Pick the slice from a sequencer:** CV into LENGTH CV (POSITION).
