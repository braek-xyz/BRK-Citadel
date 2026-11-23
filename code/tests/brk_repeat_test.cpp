// Host tests for the BRK Repeat engine (runs on a normal computer, not the Citadel).
// Build (from the repository root):
//   g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I code/src -I code/tests/er301_reference
//       code/tests/brk_repeat_test.cpp code/tests/er301_reference/RefWrap.cpp -o brk_repeat_test && ./brk_repeat_test
// Prints "N checks, 0 failed" and exits 0 when everything passed.
//
// The main checks run the Citadel engine (fixed point) next to the ER-301 engine (doubles, BRK-ER301 dsp/, copied
// into code/tests/er301_reference) on the same input and controls, and compare the outputs sample by sample.
#include "apps/BrkRepeat/RepeatEngine.hpp"
#include "RefWrap.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <vector>

static int checks = 0, failed = 0;
static void check(bool ok, const char *what)
{
    checks++;
    if (!ok)
        failed++;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static const double SR = 44000;
using Core32 = brk::RepeatCore<32>;
using Core48 = brk::RepeatCore<48>;
enum
{
    MIX = Core32::MIX, TIME = Core32::TIME, REPEATS = Core32::REPEATS, POSITION = Core32::POSITION,
    BRAEK = Core32::BRAEK, WONK = Core32::WONK, DAMAGE = Core32::DAMAGE, SYNC = Core32::SYNC,
    FREEZE = Core32::FREEZE, FREEZEHOLD = Core32::FREEZEHOLD, HOLD = Core32::HOLD
};

static int16_t gL[Core32::kBufferLen], gR[Core32::kBufferLen];

// test input: drum-like bursts (decaying noise + tone) every 0.3 s, plus a quiet sine, different per channel
struct Sig
{
    uint32_t rnd = 12345;
    int64_t n = 0;
    double next(int ch)
    {
        rnd = rnd * 1664525u + 1013904223u;
        const double noise = ((rnd >> 8) / 8388608.0 - 1.0);
        const double t = std::fmod(n / SR, 0.3);
        const double env = std::exp(-t * 18);
        const double tone = std::sin(2 * M_PI * (ch ? 180 : 120) * n / SR);
        return 0.45 * env * (0.6 * noise + 0.8 * tone) + 0.05 * std::sin(2 * M_PI * 440 * n / SR + ch);
    }
};

struct Stats
{
    double errSq = 0, refSq = 0, maxErr = 0;
    long big = 0, total = 0;
    double rel() const { return refSq > 0 ? std::sqrt(errSq / refSq) : std::sqrt(errSq); }
    double bigFrac() const { return total ? (double)big / total : 0; }
};

template <class Core> constexpr int blockOf();
template <> constexpr int blockOf<Core32>() { return 32; }
template <> constexpr int blockOf<Core48>() { return 48; }

// Runs both engines for 'seconds'; 'ctl' sets the controls of both before each 96-sample stretch
// (96 = a whole number of 32- and 48-sample blocks, so both engines see every change at the same sample).
template <class Core>
static Stats compare(double seconds, const std::function<void(double t, std::function<void(int, double)>)> &ctl,
                     double bpm = 0, double *wetRms = nullptr)
{
    static Core c;
    c.Init(gL, gR, SR);
    RefEngine ref(SR);
    Sig sig;
    Stats st;
    const int N = 96;
    const long total = (long)(seconds * SR / N);
    double ts = 0, wetSq = 0;
    for (long b = 0; b < total; b++)
    {
        const double t = (double)b * N / SR;
        ctl(t, [&](int id, double v) { c.SetParam(id, v); ref.setParam(id, v); });
        double iL[N], iR[N], rL[N], rR[N];
        int32_t qL[N], qR[N], oL[N], oR[N];
        for (int n = 0; n < N; n++)
        {
            iL[n] = sig.next(0);
            iR[n] = sig.next(1);
            sig.n++;
            qL[n] = (int32_t)std::lround(iL[n] * 32768);
            qR[n] = (int32_t)std::lround(iR[n] * 32768);
            // feed the reference exactly the 16-bit input the Citadel engine gets
            iL[n] = qL[n] / 32768.0;
            iR[n] = qR[n] / 32768.0;
        }
        for (int k = 0; k < N; k += RefEngine::KS)
        {
            ref.setTransport(bpm, ts + (double)k / SR);
            ref.process(iL + k, iR + k, rL + k, rR + k);
        }
        for (int k = 0; k < N; k += blockOf<Core>())
        {
            c.SetTransport(bpm, ts + (double)k / SR);
            c.ProcessBlock(qL + k, qR + k, oL + k, oR + k);
        }
        ts += (double)N / SR;
        if (t < 0.05)
            continue; // start-up
        for (int n = 0; n < N; n++)
        {
            for (int ch = 0; ch < 2; ch++)
            {
                const double r = ch ? rR[n] : rL[n];
                const double o = (ch ? oR[n] : oL[n]) / 32768.0;
                const double e = std::fabs(o - r);
                st.errSq += e * e;
                st.refSq += r * r;
                wetSq += o * o;
                if (e > st.maxErr)
                    st.maxErr = e;
                if (e > 0.01)
                    st.big++;
                st.total++;
            }
        }
    }
    if (wetRms)
        *wetRms = std::sqrt(wetSq / (st.total ? st.total : 1));
    return st;
}

static void report(const char *name, const Stats &s, double relMax = 0.02, double bigMax = 0.002)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: error %.4f%% of signal (%.1f dB), %.3f%% samples off by > -40 dB, max %.4f",
                  name, 100 * s.rel(), 20 * std::log10(s.rel() + 1e-12), 100 * s.bigFrac(), s.maxErr);
    check(s.rel() < relMax && s.bigFrac() < bigMax, buf);
}

int main()
{
    using F = std::function<void(int, double)>;

    // 1. MIX 0: the input passes through exactly
    {
        static Core48 c;
        c.Init(gL, gR, SR);
        Sig sig;
        bool same = true;
        for (int b = 0; b < 4000; b++)
        {
            int32_t L[48], R[48], oL[48], oR[48];
            for (int n = 0; n < 48; n++)
            {
                L[n] = (int32_t)std::lround(sig.next(0) * 32768);
                R[n] = (int32_t)std::lround(sig.next(1) * 32768);
                sig.n++;
            }
            c.SetParam(MIX, 0);
            c.SetParam(REPEATS, 8);
            c.ProcessBlock(L, R, oL, oR);
            for (int n = 0; n < 48; n++)
                same = same && oL[n] == L[n] && oR[n] == R[n];
        }
        check(same, "MIX 0 passes the input through unchanged");
    }

    // 2. same sound as the ER-301 engine (blocks of 32, as on the ER-301)
    auto basic = [](double, F s) { s(MIX, 1); s(TIME, 0.5); s(REPEATS, 4); s(POSITION, 0.34); };
    report("free 4 repeats", compare<Core32>(4, basic));
    report("one repeat (passes input)", compare<Core32>(3, [](double, F s) { s(MIX, 1); s(TIME, 0.3); }));
    report("16 repeats, short", compare<Core32>(3, [](double, F s) { s(MIX, 1); s(TIME, 0.12); s(REPEATS, 16); s(POSITION, 0.7); }));
    report("BRAEK + WONK", compare<Core32>(6, [](double, F s) {
               s(MIX, 1); s(TIME, 0.55); s(REPEATS, 6); s(POSITION, 0.2); s(BRAEK, 0.5); s(WONK, 0.8); }));
    report("WONK only (speeds and reverse)", compare<Core32>(6, [](double, F s) {
               s(MIX, 1); s(TIME, 0.6); s(REPEATS, 3); s(WONK, 1); }));
    report("SYNC 110 bpm, 1/8 notes", compare<Core32>(6, [](double, F s) {
               s(MIX, 1); s(SYNC, 1); s(TIME, 0.15); s(REPEATS, 2); s(POSITION, 1); }, 110));
    report("SYNC 110 bpm, 1/4 note, 8 repeats, WONK", compare<Core32>(6, [](double, F s) {
               s(MIX, 1); s(SYNC, 1); s(TIME, 0.35); s(REPEATS, 8); s(WONK, 0.5); s(BRAEK, 0.3); }, 110));
    report("FREEZE on, then off", compare<Core32>(6, [](double t, F s) {
               s(MIX, 1); s(TIME, 0.5); s(REPEATS, 2); s(FREEZE, (t > 1.0 && t < 3.5) ? 1 : 0); }));
    report("HOLD presses", compare<Core32>(6, [](double t, F s) {
               s(MIX, 0.8); s(TIME, 0.45); s(REPEATS, 4); s(HOLD, 1);
               double ph = std::fmod(t, 1.0);
               s(FREEZEHOLD, ph > 0.4 && ph < 0.75 ? 1 : 0); }));
    report("knobs moving (TIME, REPEATS, POSITION)", compare<Core32>(8, [](double t, F s) {
               s(MIX, 1); s(TIME, 0.3 + 0.3 * std::sin(t * 0.7)); s(REPEATS, 1 + std::floor(8 * (0.5 + 0.5 * std::sin(t * 1.3))));
               s(POSITION, 0.5 + 0.5 * std::sin(t * 0.4)); s(BRAEK, t > 4 ? 0.4 : 0); }));
    report("DAMAGE 0.5", compare<Core32>(4, [](double, F s) {
               s(MIX, 1); s(TIME, 0.5); s(REPEATS, 4); s(DAMAGE, 0.5); }));
    // other DAMAGE settings hold a sample for a fractional count; where that count lands exactly on a whole sample the
    // two engines can round it to neighbouring samples, so only the overall sound is compared
    report("DAMAGE 0.6 (overall)", compare<Core32>(4, [](double, F s) {
               s(MIX, 1); s(TIME, 0.5); s(REPEATS, 4); s(DAMAGE, 0.6); }), 0.1, 0.02);
    report("MIX half, FREEZE with MIX 0", compare<Core32>(5, [](double t, F s) {
               s(MIX, t < 2 ? 0.5 : 0); s(TIME, 0.4); s(REPEATS, 3); s(FREEZE, t > 2.5 ? 1 : 0); }));

    // 3. the Citadel's 48-sample blocks give the same sound as 32 (steady controls)
    report("48-sample blocks: free 4 repeats", compare<Core48>(4, basic));
    report("48-sample blocks: SYNC + WONK", compare<Core48>(6, [](double, F s) {
               s(MIX, 1); s(SYNC, 1); s(TIME, 0.35); s(REPEATS, 8); s(WONK, 0.5); s(BRAEK, 0.3); }, 110));
    report("48-sample blocks: FREEZE", compare<Core48>(6, [](double t, F s) {
               s(MIX, 1); s(TIME, 0.5); s(REPEATS, 2); s(FREEZE, (t > 1.0 && t < 3.5) ? 1 : 0); }), 0.05, 0.01);

    // 4. SYNC notes longer than the memory are halved and stay on the beat
    {
        static Core48 c;
        c.Init(gL, gR, SR);
        c.SetParam(SYNC, 1);
        c.SetParam(TIME, 0.95); // 2 bars
        c.SetParam(MIX, 1);
        int32_t z[48] = {}, o[48], o2[48];
        double ts = 0;
        int cycles = 0;
        long lastCycle = -1, len = 0;
        for (long b = 0; b < 6000; b++)
        {
            c.SetTransport(110, ts);
            c.ProcessBlock(z, z, o, o2);
            ts += 48 / SR;
            if (c.TakeCycleFlag())
            {
                if (lastCycle >= 0)
                    len = (b - lastCycle) * 48;
                lastCycle = b;
                cycles++;
            }
        }
        // 110 bpm: one beat = 24000 samples; the longest cycle is about 24700, so 2 bars -> 1 beat
        char buf[160];
        std::snprintf(buf, sizeof buf, "SYNC 2 bars at 110 bpm halves to %.3g beats (cycle about %ld samples, max %.0f)",
                      c.SyncBeats(), len, c.MaxCycleSeconds() * SR);
        check(c.SyncBeats() == 1.0 && cycles > 5 && std::labs(len - 24000) <= 48, buf);
        c.SetTransport(60, ts);
        c.ProcessBlock(z, z, o, o2);
        check(c.SyncBeats() == 0.5, "SYNC 2 bars at 60 bpm halves to 1/8 note");
    }

    // 5. Citadel clock: every pulse is a beat; TIME steps are fractions / multiples of the pulse.  A 1/16 clock at
    //    110 bpm (one pulse every 6000 samples): the middle of TIME repeats every pulse, and cycles land on pulses.
    {
        static const double pulses[11] = {1.0 / 8, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0, 1.5, 2.0, 3.0, 4.0, 8.0};
        struct Case { int step; long expect; };
        const Case cases[] = {{5, 6000}, {3, 3000}, {7, 12000}, {1, 1500}, {10, 24000}};
        for (const Case &cs : cases)
        {
            static Core48 c;
            c.Init(gL, gR, SR);
            c.SetSyncTable(pulses, 11);
            c.SetTempoRange(15, 30000);
            c.SetParam(SYNC, 1);
            c.SetParam(MIX, 1);
            c.SetParam(TIME, (cs.step + 0.5) / 11);
            const double period = 6000; // samples per pulse (a multiple of 48, as the Citadel measures it)
            int32_t z[48] = {}, o[48], o2[48];
            long since = 0, beats = 0, last = -1, len = 0, offBeat = 0, cyc = 0;
            for (long b = 0; b < 8000; b++)
            {
                if (since >= period)
                {
                    since = 0;
                    beats++;
                }
                const double frac = std::min(1.0, since / period);
                c.SetTransport(60.0 * SR / period, (beats + frac) * period / SR);
                c.ProcessBlock(z, z, o, o2);
                since += 48;
                if (c.TakeCycleFlag())
                {
                    const long at = b * 48;
                    if (last >= 0)
                        len = at - last;
                    last = at;
                    if (b > 2000)
                    {
                        cyc++;
                        // the cycle starts within one block of a multiple of the cycle length on the pulse grid
                        const long ph = at % cs.expect;
                        if (std::min(ph, cs.expect - ph) > 200)
                            offBeat++;
                    }
                }
            }
            char buf[200];
            std::snprintf(buf, sizeof buf, "1/16 clock, TIME step %d: cycle %ld samples (expect %ld), %ld of %ld cycle starts off the grid",
                          cs.step, len, cs.expect, offBeat, cyc);
            check(std::labs(len - cs.expect) <= 48 && cyc > 3 && offBeat == 0, buf);
        }
    }

    // 6. fuzz: random controls, loud input; must stay in range (sanitizers watch the memory)
    {
        static Core48 c;
        c.Init(gL, gR, SR);
        uint32_t r = 777;
        auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (r >> 8) / 16777216.0; };
        bool ok = true;
        double ts = 0;
        for (long b = 0; b < 40000; b++)
        {
            if (b % 37 == 0)
            {
                c.SetParam(MIX, rnd());
                c.SetParam(TIME, rnd());
                c.SetParam(REPEATS, 1 + 15 * rnd());
                c.SetParam(POSITION, rnd());
                c.SetParam(BRAEK, rnd());
                c.SetParam(WONK, rnd());
                c.SetParam(DAMAGE, rnd());
                c.SetParam(SYNC, rnd() > 0.5);
                c.SetParam(FREEZE, rnd() > 0.7);
                c.SetParam(HOLD, rnd() > 0.6);
                c.SetParam(FREEZEHOLD, rnd() > 0.5);
            }
            int32_t L[48], R[48], oL[48], oR[48];
            for (int n = 0; n < 48; n++)
            {
                L[n] = (int32_t)(rnd() * 80000 - 40000); // beyond 16 bits on purpose
                R[n] = -L[n];
            }
            c.SetTransport(b % 5000 < 4000 ? 40 + 300 * rnd() * (b % 3 == 0) + 100 : 0, ts);
            ts += 48 / SR;
            c.ProcessBlock(L, R, oL, oR);
            for (int n = 0; n < 48; n++)
                ok = ok && oL[n] >= -32768 && oL[n] <= 32767 && oR[n] >= -32768 && oR[n] <= 32767;
        }
        check(ok, "fuzz: 40000 blocks of random controls, output stays in range");
    }

    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
