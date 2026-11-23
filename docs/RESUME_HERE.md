# Resume here

**Last session: 2026-11-23**

Done:
- Researched the platform (plan: `docs/PLAN.md`). Forked Bastl's `kastle2` as `braek-xyz/BRK-Citadel`.
- Cloud build added (`.github/workflows/build.yml`): builds Bastl's stock **FX Wizard** and **Template** `.uf2`,
  unchanged, to prove the build chain. Built in a cloud session first: exit 0, FX Wizard flash 82 %, RAM 8.5 % static.

Next:
1. BRAEK downloads `kastle2-fx-wizard.uf2` from the Actions run, flashes it, and checks it behaves like stock FX Wizard.
   (To go back to Wave Bard: Bastl's official Wave Bard file, which includes the samples.)
2. Phase 1: `dsp-research` on the repeater design for RP2040 (fixed point, memory), then agree the controls map,
   then the first playable BRK Repeat app.

Not tested: nothing has run on a Citadel yet.
