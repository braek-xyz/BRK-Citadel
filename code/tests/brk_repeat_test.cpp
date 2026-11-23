// Host tests for the BRK Repeat engine (runs on a normal computer, not the Citadel).
// Build: g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I code/src code/tests/brk_repeat_test.cpp -o /tmp/brt && /tmp/brt
#include "apps/BrkRepeat/RepeatEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

using brk::RepeatEngine;
static int checks = 0, failed = 0;
#define CHECK(c, msg)                                   \
    do                                                  \
    {                                                   \
        checks++;                                       \
        if (!(c))                                       \
        {                                               \
            failed++;                                   \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        }                                               \
    } while (0)

static int16_t bufL[RepeatEngine::kBufLen], bufR[RepeatEngine::kBufLen];
static const size_t kBlock = 48;

// deterministic test signal: a ramp-ish signal that is different at every sample
static int16_t sig(int64_t n, int ch) { return static_cast<int16_t>(((n * 37 + ch * 1000) % 20000) - 10000); }

static void run(RepeatEngine &e, const RepeatEngine::Params &p, int64_t &n, size_t blocks,
                std::vector<int16_t> *outL = nullptr, std::vector<int16_t> *inL = nullptr)
{
    int16_t in[2 * kBlock], out[2 * kBlock];
    for (size_t b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            in[2 * i] = sig(n + i, 0);
            in[2 * i + 1] = sig(n + i, 1);
        }
        e.SetParams(p);
        e.Process(in, out, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            if (outL)
                outL->push_back(out[2 * i]);
            if (inL)
                inL->push_back(in[2 * i]);
        }
        n += kBlock;
    }
}

int main()
{
    // 1. Repeats 1, nothing else: output is exactly the input
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 11000;
        int64_t n = 0;
        std::vector<int16_t> o, i;
        run(e, p, n, 3000, &o, &i);
        size_t diff = 0;
        for (size_t k = 0; k < o.size(); k++)
            diff += (o[k] != i[k]);
        CHECK(diff == 0, "repeats 1 passes the input unchanged");
    }
    // 2. Repeats 4, position 0: every slice of a cycle repeats slice 0 (away from the fades)
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 4800; // 100 blocks
        p.repeats = 4;      // slice = 1200
        int64_t n = 0;
        std::vector<int16_t> o, i;
        run(e, p, n, 1000, &o, &i);
        // the first cycle start after sample 0 is at 4800 (initial cycle is 22000 -> first boundary at 22000)
        const size_t c0 = 22000;
        bool ok = true;
        for (int c = 0; c < 5; c++)
            for (int k = 1; k < 4; k++)
                for (int s = 200; s < 1000; s++)
                {
                    size_t at = c0 + c * 4800 + k * 1200 + s;
                    size_t src = c0 + c * 4800 + s;
                    if (o[at] != i[src])
                        ok = false;
                }
        CHECK(ok, "repeats 4 / position 0 repeats the first slice");
        bool ok0 = true;
        for (int c = 1; c < 5; c++)
            for (int s = 200; s < 1000; s++)
                if (o[c0 + c * 4800 + s] != i[c0 + c * 4800 + s])
                    ok0 = false;
        CHECK(ok0, "slice 0 is the live sound");
    }
    // 3. Position last slice: slice 0 must come from the previous cycle (already recorded)
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 4800;
        p.repeats = 4;
        p.position = 4095;
        int64_t n = 0;
        std::vector<int16_t> o, i;
        run(e, p, n, 1000, &o, &i);
        const size_t c0 = 22000;
        bool ok = true;
        for (int c = 2; c < 5; c++)
            for (int s = 200; s < 1000; s++)
            {
                size_t at = c0 + c * 4800 + s;               // slice 0 of cycle c
                size_t src = c0 + (c - 1) * 4800 + 3600 + s; // slice 3 of cycle c-1
                if (o[at] != i[src])
                    ok = false;
            }
        CHECK(ok, "not-yet-recorded slice is read from the previous cycle");
    }
    // 4. FREEZE: after it engages, the output repeats with the cycle length
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 4800;
        int64_t n = 0;
        run(e, p, n, 500);
        p.freeze = true;
        std::vector<int16_t> o;
        run(e, p, n, 1000, &o);
        CHECK(e.GetState() == RepeatEngine::State::FROZEN, "freeze engages");
        bool ok = true;
        for (size_t k = 20000; k + 4800 < o.size(); k++)
            if (o[k] != o[k + 4800])
                ok = false;
        CHECK(ok, "frozen output loops with the cycle length");
        p.freeze = false;
        std::vector<int16_t> o2, i2;
        run(e, p, n, 500, &o2, &i2);
        CHECK(e.GetState() == RepeatEngine::State::LIVE, "freeze releases");
        bool live = true;
        for (size_t k = 12000; k < o2.size(); k++)
            if (o2[k] != i2[k])
                live = false;
        CHECK(live, "after release the sound is live again");
    }
    // 5. HOLD: output loops with the slice length
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 9600;
        p.repeats = 8; // slice 1200
        int64_t n = 0;
        run(e, p, n, 1000);
        p.hold = true;
        std::vector<int16_t> o;
        run(e, p, n, 300, &o);
        CHECK(e.GetState() == RepeatEngine::State::HOLD, "hold engages");
        bool ok = true;
        for (size_t k = 2000; k + 1200 < o.size(); k++)
            if (o[k] != o[k + 1200])
                ok = false;
        CHECK(ok, "held output loops with the slice length");
    }
    // 6. Fuzz: random settings, clock realigns, freeze / hold switching. Sanitizers catch out-of-range access.
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        int64_t n = 0;
        srand(1);
        int16_t in[2 * kBlock], out[2 * kBlock];
        int32_t peak = 0;
        for (int b = 0; b < 200000; b++)
        {
            if (b % 97 == 0)
            {
                p.cycle_len = rand() % 70000;
                p.repeats = rand() % 18;
                p.position = rand() % 4096;
                p.braek = rand() % 4096;
                p.wonk = rand() % 4096;
                p.damage = rand() % 4096;
                p.mix = rand() % 32768;
            }
            if (b % 53 == 0)
                p.freeze = rand() % 2;
            if (b % 31 == 0)
                p.hold = (rand() % 5) == 0;
            if (b % 41 == 0)
                e.Realign();
            for (size_t i = 0; i < kBlock; i++)
            {
                in[2 * i] = (rand() % 65536) - 32768;
                in[2 * i + 1] = (rand() % 65536) - 32768;
            }
            e.SetParams(p);
            e.Process(in, out, kBlock);
            for (size_t i = 0; i < 2 * kBlock; i++)
                peak = std::max<int32_t>(peak, std::abs(out[i]));
            n += kBlock;
        }
        CHECK(peak <= 32768, "fuzz: output stays in range");
    }
    // 7. Clicks: with repeats 4 and wonk / braek, a quiet sine must not get jumps bigger than a few steps
    {
        RepeatEngine e;
        e.Init(bufL, bufR);
        RepeatEngine::Params p;
        p.cycle_len = 8800;
        p.repeats = 4;
        p.position = 1500;
        p.braek = 1500;
        p.wonk = 2000;
        int16_t in[2 * kBlock], out[2 * kBlock];
        int64_t n = 0;
        int32_t prev = 0, worst = 0;
        for (int b = 0; b < 20000; b++)
        {
            for (size_t i = 0; i < kBlock; i++)
            {
                int16_t v = static_cast<int16_t>(8000 * std::sin(2 * M_PI * 110.0 * (n + i) / 44000.0));
                in[2 * i] = v;
                in[2 * i + 1] = v;
            }
            e.SetParams(p);
            e.Process(in, out, kBlock);
            for (size_t i = 0; i < kBlock; i++)
            {
                if (b > 500)
                    worst = std::max(worst, std::abs(out[2 * i] - prev));
                prev = out[2 * i];
            }
            n += kBlock;
        }
        // a 110 Hz sine at 8000 moves at most about 126 per sample; at double speed about 252
        std::printf("largest sample-to-sample step: %d\n", worst);
        CHECK(worst < 600, "no clicks at slice edges");
    }
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
