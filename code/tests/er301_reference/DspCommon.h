// BRK-ER301 - small DSP helpers shared by the ER-301 ports of LAZERS.
//
// Ported from LAZERS 2.0.0 (BRAEK's own code, PLUGIN-WORKFLOW plugins/lazers/engine/Dsp.h and Engine.cpp; see
// PROVENANCE.md).  Changes for the ER-301: C++11 (the ER-301 SDK compiler is gcc 4.9), and roundEven / truncTo0 written
// with floor / ceil only, so the package needs nothing from the firmware's maths library beyond the basic functions.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace brk {

static const int KS = 32;                 // engine block, as in LAZERS (v12.6 ksmps 32)
typedef double Blk[KS];

struct Rates {
    double sr, kr, invSr, invKr;
    Rates() { set(48000); }
    void set(double s) { sr = s; kr = s / KS; invSr = 1.0 / s; invKr = 1.0 / kr; }
};

// clamp that sends anything outside the range - including NaN - to an edge (NaN goes to the lower edge)
inline double clampLo(double x, double lo, double hi) {
    if (lo >= hi) return 0.5 * (lo + hi);
    if (x >= lo && x <= hi) return x;
    return x > hi ? hi : lo;
}
// floor, same result as std::floor, without a maths-library call when x fits in 32 bits.  The ER-301's CPU (ARMv7) has
// no floor instruction, so std::floor is a library call; the engine needs about 10 to 17 of them per sample.
inline double ffloor(double x) {
    if (x > -2147483648.0 && x < 2147483647.0) {
        double d = (double)(int32_t)x;    // rounds towards 0
        return d > x ? d - 1.0 : d;
    }
    return std::floor(x);
}
// floor as an int32_t index (x must fit in 32 bits, as every history index does)
inline int32_t ifloor(double x) {
    int32_t i = (int32_t)x;
    return (double)i > x ? i - 1 : i;
}
// round to nearest, ties to even (same results as LAZERS' std::nearbyint in the default rounding mode)
inline double roundEven(double x) {
    if (!(std::fabs(x) < 2147483647.0)) return 0.0;
    double f = ffloor(x), d = x - f;
    if (d > 0.5) return f + 1.0;
    if (d < 0.5) return f;
    return (((int32_t)f) & 1) ? f + 1.0 : f;
}
inline double truncTo0(double x) { return x < 0 ? -ffloor(-x) : ffloor(x); }
inline double fracPart(double x) { return x - truncTo0(x); }

// sin(g x 1.5707963) and cos(g x 1.5707963) for g in 0..1 (the LAZERS crossfade gains), from a 1024-step table with
// linear interpolation: at most 3e-7 away from std::sin / std::cos (-130 dB), exact at 0 and 1, and much cheaper on the
// ER-301 than two library calls per sample.  g outside 0..1 is clamped (every caller clamps it first anyway).
struct QuarterSine {
    static const int N = 1024;
    double s[N + 2], c[N + 2];
    QuarterSine() {
        for (int i = 0; i <= N; i++) { double a = (double)i / N * 1.5707963; s[i] = std::sin(a); c[i] = std::cos(a); }
        s[N + 1] = s[N]; c[N + 1] = c[N];
    }
    inline void at(double g, double& sn, double& cs) const {
        if (!(g > 0)) { sn = s[0]; cs = c[0]; return; }
        if (g >= 1) { sn = s[N]; cs = c[N]; return; }
        double x = g * N;
        int i = (int)x;
        double f = x - i;
        sn = s[i] + (s[i + 1] - s[i]) * f;
        cs = c[i] + (c[i + 1] - c[i]) * f;
    }
};

// Random numbers: 64-bit linear congruential generator with Knuth's MMIX constants, top 53 bits (as LAZERS).
struct Random {
    uint64_t s;
    Random() : s(1) {}
    void seed(uint64_t v) { s = v * 2 + 1; next(); }
    uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s; }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
    double range(double lo, double hi) { return lo + uniform() * (hi - lo); }
};

// control-rate glide with a half-value time (one step per block)
struct Glide {
    double c, y;
    Glide() : c(0), y(0) {}
    void start(const Rates& r, double halfTime, double initial = 0.0) { c = std::pow(0.5, r.invKr / halfTime); y = initial; }
    double next(double target) { y = (1.0 - c) * target + c * y; return y; }
};

// sample-rate reduction: hold a sample, take a new one every 'step' samples (fractional steps allowed)
struct Downsampler {
    // the sample count is a double (exact up to 2^53 samples, about 6000 years at 48 kHz): an int64_t count needed a
    // library call per sample to compare with 'next' on the ER-301
    double next, held, count;
    Downsampler() : next(0), held(0), count(0) {}
    void process(double* io, double step) {
        for (int n = 0; n < KS; n++) {
            if (next < count) { next += step; held = io[n]; }
            io[n] = held;
            count += 1.0;
        }
    }
};

// change detector for control values (starts from 0)
struct ChangeDetect {
    double last;
    ChangeDetect() : last(0) {}
    bool operator()(double v) { if (v != last) { last = v; return true; } return false; }
};

// SYNC note lengths in beats (LAZERS giSyncT): 1/16, 1/8, dotted 1/8, 1/4, dotted 1/4, 1/2, dotted 1/2, 1, 1.5, 2 bars
static const double kSyncT[16] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 0, 0, 0, 0, 0, 0};
inline double tabK(const double* t, int len, double ndx) {    // table read, index clamped
    int32_t i = (int32_t)std::floor(ndx);
    if (i >= len) i = len - 1; else if (i < 0) i = 0;
    return t[i];
}

} // namespace brk
