/*
 * BRK Repeat engine for the RP2040 (Bastl Citadel / Kastle 2).
 * Copyright (c) 2026 BRAEK. MIT License (see LICENSE in the repository root).
 *
 * This is BRAEK's LAZERS 2.0.0 REPEAT + FREEZE effect, as ported to the ER-301 in BRK-ER301 (dsp/RepeatEngine.cpp,
 * "brk repeat"), ported once more for a CPU with no floating-point unit and 264 kB of RAM.  Plain C++, no Kastle 2 or
 * Pico SDK code, so the same file runs in the host tests next to the ER-301 engine.
 *
 * How the port is split
 * - Once per block (KS samples): the LAZERS control logic, line for line, in double (cycle clock, slicing, SYNC phase,
 *   BRAEK / WONK random choices, FREEZE / HOLD, crossfade state).  It runs about 900 times a second on the Citadel.
 * - Every sample: integer and fixed-point only.  Positions are whole samples; slice lengths are Q16 (1/65536 sample);
 *   gains are Q15 with 32768 = 1.0; the history is 16-bit stereo.
 *
 * Differences from the ER-301 engine (everything else is the same):
 *   1. History: 50000 samples per channel, 16-bit (ER-301: 10 s, float).  The longest cycle is therefore
 *      (50000 - 2 x 4 ms - 2 x KS - 64) / 2 samples, about 0.56 s at 44 kHz (ER-301: 4 s).
 *   2. SYNC: a note length longer than the longest cycle is halved until it fits (ER-301: SYNC turns off and the cycle
 *      is clamped).  So SYNC always stays on the beat.
 *   3. Per-sample maths in fixed point (results within a few 16-bit steps of the double engine; see the host test).
 *      The two 1e6-steep LAZERS switches (read from the previous cycle; second half of a cycle) are plain compares.
 *   4. DAMAGE bit reduction rounds halves away from zero (LAZERS: to even).
 *   5. The LAZERS idle self-heal is not part of it (as on the ER-301).
 *   6. The SYNC length table and the accepted tempo range can be changed (SetSyncTable, SetTempoRange): the
 *      Citadel counts every clock pulse as a beat, so a 1/16 clock at 150 bpm is 600 "beats" a minute.
 *
 * Real-time rules: ProcessBlock() never allocates, locks or touches flash or files.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef BRK_FAST
#define BRK_FAST
#endif
// The per-block function is large (the LAZERS control logic); it is compiled for size and stays in flash, while the
// per-sample read (rdMap) can be put in RAM with BRK_FAST.
#if defined(__GNUC__) && !defined(__clang__)
#define BRK_COMPACT __attribute__((optimize("Os")))
#define BRK_HOT __attribute__((optimize("O2")))
#define BRK_INLINE __attribute__((always_inline)) inline
#else
#define BRK_COMPACT
#define BRK_HOT
#define BRK_INLINE inline
#endif

namespace brk
{

// ---- helpers, as in BRK-ER301 dsp/DspCommon.h -------------------------------------------------------------------
inline double clampLo(double x, double lo, double hi)
{
    if (lo >= hi)
        return 0.5 * (lo + hi);
    if (x >= lo && x <= hi)
        return x;
    return x > hi ? hi : lo;
}
inline double ffloor(double x)
{
    if (x > -2147483648.0 && x < 2147483647.0)
    {
        double d = (double)(int32_t)x;
        return d > x ? d - 1.0 : d;
    }
    return std::floor(x);
}
inline double roundEven(double x)
{
    if (!(std::fabs(x) < 2147483647.0))
        return 0.0;
    double f = ffloor(x), d = x - f;
    if (d > 0.5)
        return f + 1.0;
    if (d < 0.5)
        return f;
    return (((int32_t)f) & 1) ? f + 1.0 : f;
}
inline double truncTo0(double x) { return x < 0 ? -ffloor(-x) : ffloor(x); }

struct Random // 64-bit LCG, Knuth MMIX constants, top 53 bits (as LAZERS)
{
    uint64_t s = 1;
    void seed(uint64_t v) { s = v * 2 + 1; next(); }
    uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s; }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double lo, double hi) { return lo + uniform() * (hi - lo); }
};

struct ChangeDetect
{
    double last = 0;
    bool operator()(double v)
    {
        if (v != last)
        {
            last = v;
            return true;
        }
        return false;
    }
};

// SYNC note lengths in beats (LAZERS giSyncT): 1/16, 1/8, 1/8., 1/4, 1/4., 1/2, 1/2., 1 bar, 1.5 bars, 2 bars
static constexpr double kSyncT[10] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0};

// ---- the engine ----------------------------------------------------------------------------------------------------
template <int KS>
class RepeatCore
{
public:
    enum Param
    {
        MIX,        // 0..1    incoming vs repeats
        TIME,       // 0..1    free: 12.5 ms x 320^time; sync: note length by tenths (see kSyncT)
        REPEATS,    // 1..16   slices per cycle
        POSITION,   // 0..1    which slice repeats
        BRAEK,      // 0..1    random gaps and jumps
        WONK,       // 0..1    random speed / direction per repeat
        DAMAGE,     // 0..1    sample-rate and bit reduction
        SYNC,       // 0 / 1   TIME follows tempo
        FREEZE,     // 0 / 1   loop the current cycle (starts / stops at a cycle end)
        FREEZEHOLD, // 0 / 1   momentary FREEZE (HOLD mode)
        HOLD,       // 0 / 1   HOLD mode: FREEZE starts / stops at once
        COUNT
    };

    static constexpr int32_t kHist = 50000;         ///< history samples per channel
    static constexpr size_t kBufferLen = kHist + 1; ///< int16 per channel the caller gives (one guard point)

    void Init(int16_t *left, int16_t *right, double sample_rate)
    {
        BL = left;
        BR = right;
        sr_ = sample_rate;
        static const double def[COUNT] = {0, 0.5, 1, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < COUNT; i++)
            prm_[i] = def[i];
        for (int i = 0; i <= kTab; i++)
            sinTab_[i] = (uint16_t)std::lround(std::sin((double)i / kTab * 1.5707963) * 32768.0);
        sinTab_[kTab + 1] = sinTab_[kTab];
        Reset();
    }

    /// Silence and rewind (not for the audio loop: clears 200 kB)
    void Reset()
    {
        rng_.seed(1234);
        std::memset(BL, 0, sizeof(int16_t) * kBufferLen);
        std::memset(BR, 0, sizeof(int16_t) * kBufferLen);
        const double sr = sr_;
        giFade = 0.003 * sr;
        fadeI_ = (int32_t)std::lround(giFade);
        iX = roundEven(0.004 * sr);
        iPre = roundEven(0.0035 * sr);
        iMinL = roundEven(0.0125 * sr);
        iMaxL = ffloor((kHist - 2 * iX - 2 * KS - 64) / 2);
        iMaxP = iMaxL / sr;
        kBoot = 1; kG = 0; kW = 0; kT = 0; kLc = iMinL; kLe = iMinL; kNc = 1; kSc = iMinL; kLnomC = iMinL; kCarry = 0;
        kRc = 0; kPassS = 1; kPassC = 1; kFzOn = 0; kFzA = 0; kFzStop = 0; kSpEnd = 0; kFza = 0; kFzG = 0; kFzX = 0;
        kMc = 0; kMtrC = 0; kJmpC = 0; kRjC = 0; kSpdC = 1; kSilC = 0; kJmpN = 0; kRjN = 0; kSpdN = 1; kSilN = 0;
        kXP = 1; kXlaw = 0; kTo = 0; kLoE = iMinL; kLoC = iMinL; kSo = iMinL; kMo = 0; kSpdo = 1; kSilo = 0;
        kOLive = 1; kAo = 0; kao = 0; kXso = 0; kPasso = 1; kSince = 0; kMovT = 0; kTsX = 0;
        kTnew2 = kBnd1 = kFzAn = kLnew = kTnew = kHq = kFzNx = kPassEo = kLiveO = kLiveN = kTb = kHp = kE = 0;
        kR1 = kR2 = kR3 = kR4 = kXs = kWadv = 0;
        tsChanged_ = ChangeDetect();
        fzChanged_ = ChangeDetect();
        mixC_ = std::pow(0.5, (KS / sr) / 0.02);
        mixY_ = 0;
        dsNext_ = 0;
        dsCount_ = 0;
        dsHeldL_ = dsHeldR_ = 0;
        timeCached_ = -1;
        corCached_ = -1;
        beatsUsed_ = 1;
        cycleFlag_ = sliceFlag_ = false;
    }

    void SetParam(int id, double v)
    {
        if (id >= 0 && id < COUNT)
            prm_[id] = v;
    }
    double GetParam(int id) const { return prm_[id]; }
    /// Tempos accepted from SetTransport (others count as no tempo: 120 is used).  LAZERS / ER-301: 20..400.
    void SetTempoRange(double lo, double hi)
    {
        bpmLo_ = lo;
        bpmHi_ = hi;
    }
    /// SYNC lengths in beats that TIME chooses from (in equal steps); default: the LAZERS table kSyncT (10 notes).
    /// The table must stay valid while the engine runs.
    void SetSyncTable(const double *beats, int count)
    {
        if (beats && count > 0)
        {
            syncT_ = beats;
            syncN_ = count;
        }
    }
    /// bpm 0 = no tempo (120 is used); timeSec = song position, SYNC locks to it while it keeps moving
    void SetTransport(double bpm, double time_sec)
    {
        bpm_ = bpm;
        ts_ = time_sec;
    }

    bool Frozen() const { return kFzOn >= 0.5; }
    /// true while the input passes straight through (one slice, nothing random, not frozen)
    bool Passing() const { return kPassS >= 0.5 && kFzOn < 0.5; }
    double MaxCycleSeconds() const { return iMaxP; }
    /// the SYNC note length in beats after halving
    double SyncBeats() const { return beatsUsed_; }
    bool TakeCycleFlag()
    {
        bool f = cycleFlag_;
        cycleFlag_ = false;
        return f;
    }
    bool TakeSliceFlag()
    {
        bool f = sliceFlag_;
        sliceFlag_ = false;
        return f;
    }

    /// One block of KS samples per channel, 16-bit values (32768 = 1.0) in int32.  in and out may be the same arrays.
    BRK_COMPACT void ProcessBlock(const int32_t *inL, const int32_t *inR, int32_t *outL, int32_t *outR)
    {
        for (int n = 0; n < KS; n++)
        {
            xL_[n] = clamp16(inL[n]);
            xR_[n] = clamp16(inR[n]);
        }
        const double ksmps = KS, sr = sr_;
        const double *P = prm_;
        double kMix = P[MIX], kTime = P[TIME], kRep = P[REPEATS], kTrv = P[POSITION], kBrk = P[BRAEK],
               kBnd = P[WONK], kCor = P[DAMAGE], kFrz = P[FREEZE], kFrzH = P[FREEZEHOLD], kHold = P[HOLD],
               kSyn = P[SYNC], kBpm = bpm_, kTs = ts_;
        double kMov = tsChanged_(kTs) ? 1 : 0;
        kSince = (kMov == 1 ? 0 : kSince + ksmps / sr);
        kMovT = (kMov == 1 ? 0.25 : kMovT - ksmps / sr);
        kTsX = kTs + kSince;
        double kBpmU = (kBpm > bpmLo_ && kBpm < bpmHi_ ? kBpm : 120);

        double kIx = clampLo(truncTo0(kTime * (syncN_ - 0.001)), 0, syncN_ - 1);
        double kBeats = syncT_[(int)kIx];
        if (kSyn >= 0.5)
        {
            // port: halve a note that is too long for the memory (stays on the beat)
            while (kBeats * 60 / kBpmU > iMaxP && kBeats > 0.07)
                kBeats *= 0.5;
        }
        beatsUsed_ = kBeats;
        if (kTime != timeCached_)
        {
            timeCached_ = kTime;
            freePer_ = 0.0125 * std::exp(std::log(320.0) * kTime);
        }
        double kPerS = (kSyn >= 0.5 ? kBeats * 60 / kBpmU : freePer_);
        double kPerL = clampLo(kPerS, 0.0125, iMaxP);
        double kLnomE = kPerL * sr;
        double kNn = clampLo(roundEven(kRep), 1, 16);
        double kHoldM = (kHold >= 0.5 ? 1 : 0);
        double kReqH = (kFrzH >= 0.5 || kFrz >= 0.5 ? 1 : 0);
        double kReq = (kHoldM == 1 ? kReqH : (kFrz >= 0.5 ? 1 : 0));
        double kSyncOK = (kSyn >= 0.5 && kMovT > 0 && kPerS <= iMaxP ? 1 : 0);
        double kBB = (kBnd <= 0.001 && kBrk <= 0.001 ? 1 : 0);

        if (kBoot == 1)
        {
            kBoot = 0;
            kLc = roundEven(kLnomE);
            kLe = kLc;
            kLnomC = kLnomE;
            kNc = kNn;
            kSc = kLc / kNc;
            kMtrC = roundEven(kTrv * (kNc - 1));
            kMc = kMtrC;
            kPassC = (kNc == 1 && kBB == 1 ? 1 : 0);
            kPassS = kPassC;
            kRc = -1;
        }

        double kChN = (kNn != kNc ? 1 : 0);
        double kChL = (std::fabs(kLnomE - kLnomC) > 0.01 * kLnomC ? 1 : 0);
        double kMtr = roundEven(kTrv * (kNn - 1));
        double kChM = (kMtr != kMtrC ? 1 : 0);
        double kChF = (kHoldM == 1 && kReq != kFzOn ? 1 : 0);
        if (kXP >= 1 && kChN + kChL + kChM + kChF > 0)
        {
            kTo = kT; kLoE = kLe; kLoC = kLc; kSo = kSc; kMo = kMc; kSpdo = kSpdC; kSilo = kSilC;
            kOLive = 1 - kFzOn; kAo = kFzA; kao = kFza; kXso = kFzX * kFzOn;
            kFzNx = (kHoldM == 0 ? kReq : kFzOn);
            kPassEo = kPassC * (kNc == 1 && kFzNx == 0 && kBB == 1 ? 1 : 0);
            kPasso = (kT >= kLe * 0.5 ? kPassEo : kPassS);
            kXP = 0;
            kXlaw = 0;
            if (kChL == 1)
            {
                kLnew = clampLo(roundEven(kLnomE), iMinL, iMaxL);
                if (kSyncOK == 1)
                {
                    kHq = kTsX * kBpmU / (60 * kBeats) + iPre / kLnew;
                    kTnew = roundEven((kHq - ffloor(kHq)) * kLnew);
                }
                else
                {
                    kTnew = roundEven(kT / kLe * kLnew);
                }
                kTnew = clampLo(kTnew, 0, kLnew - 1);
                if (kFzOn == 1)
                {
                    kFza = kTnew - (kG - kFzG);
                    kFza = kFza - kLnew * ffloor(kFza / kLnew);
                }
                if (kSyncOK == 0 && std::fabs(kLnew - kLoE) < 0.1 * kLoE)
                    kXlaw = 1;
                kT = kTnew; kLc = kLnew; kLe = kLnew; kLnomC = kLnomE;
            }
            kNc = kNn;
            kSc = kLc / kNc;
            kMtrC = roundEven(kTrv * (kNc - 1));
            kMc = (kJmpC == 1 && kBrk > 0.001 ? truncTo0(kRjC * kNc) : kMtrC);
            kMc = clampLo(kMc, 0, kNc - 1);
            if (kChF == 1)
            {
                if (kReq == 1)
                {
                    kFzOn = 1; kFzA = kW; kFzStop = kW + iX; kFzG = kG;
                    kFza = kT - kLc * ffloor(kT / kLc);
                    kFzX = iX;
                    if (kChN + kChL + kChM == 0 && ffloor(kT / kSc) != kMc)
                        kXP = 1;
                }
                else
                {
                    kFzOn = 0; kW = kFzA; kSpEnd = kFzA + iX;
                }
            }
            kPassC = (kNc == 1 && kFzOn == 0 && kBB == 1 ? 1 : 0);
            kPassS = kPassS * kPassC;
            kLiveO = (kOLive == 1 && ffloor(kTo / kSo) == kMo && kSpdo == 1 ? 1 : 0);
            kLiveN = (kFzOn == 0 && ffloor(kT / kSc) == kMc && kSpdC == 1 ? 1 : 0);
            if (kLiveO == 1 && kLiveN == 1)
                kXlaw = 1;
            kRc = (kT > 0 ? ffloor((kT - 1) / kSc) : -1);
        }

        double kCross = (kT + ksmps > kLe ? 1 : 0);
        double kJ = kLe - kT;
        double kFzNext = (kHoldM == 0 ? kReq : kFzOn);
        double kLn = kLc;
        if (kCross == 1)
        {
            if (kSyncOK == 1)
            {
                kTb = kTsX + kJ / sr;
                kHp = kTb * kBpmU / (60 * kBeats) + iPre / kLnomE;
                kE = kHp - roundEven(kHp);
                kLn = roundEven(kLnomE * (1 - kE));
            }
            else
            {
                kLn = roundEven(kLnomE + kCarry);
                kCarry = kLnomE + kCarry - kLn;
            }
            kLn = clampLo(kLn, iMinL, iMaxL);
        }
        double kSn = kLn / kNc;
        double kPassN = (kNc == 1 && kFzNext == 0 && kBB == 1 ? 1 : 0);
        double kPassE = kPassC * kPassN;
        double kEng = (kCross == 1 && kFzOn == 0 && kFzNext == 1 ? 1 : 0);
        double kRel = (kCross == 1 && kFzOn == 1 && kFzNext == 0 ? 1 : 0);

        // ---- per-sample clock (integers) ----
        double kMbN = roundEven(kTrv * (kNc - 1));
        double kMn = (kBrk > 0.001 && kJmpN == 1 ? truncTo0(kRjN * kNc) : kMbN);
        kMn = clampLo(kMn, 0, kNc - 1);
        {
            const int32_t iT = (int32_t)kT, iLe = (int32_t)kLe, iLc = (int32_t)kLc, iLn = (int32_t)kLn;
            const uint32_t sc16 = q16(kSc), sn16 = q16(kSn);
            const int32_t iRc = (int32_t)kRc, mx = (int32_t)kNc - 1, iMc = (int32_t)kMc, iMn = (int32_t)kMn;
            const int32_t hC = halfSpeed(kSpdC), hN = halfSpeed(kSpdN);
            const int32_t ngC = kSpdC < 0, ngN = kSpdN < 0, slC = kSilC >= 0.5, slN = kSilN >= 0.5;
            const bool cross = kCross == 1;
            for (int n = 0; n < KS; n++)
            {
                const int32_t t = iT + n;
                const int32_t k = (cross && t >= iLe) ? 1 : 0;
                K_[n] = k;
                Tp_[n] = t - k * iLe;
                Sp_[n] = k ? sn16 : sc16;
                Lp_[n] = k ? iLn : iLc;
                Lh_[n] = k ? iLn : iLe;
                const uint32_t tp16 = (uint32_t)Tp_[n] << 16;
                const uint32_t rx = tp16 / Sp_[n];
                U_[n] = tp16 - rx * Sp_[n];
                const int32_t nw = (k || (int32_t)rx - iRc >= 1) ? 1 : 0;
                int32_t m = nw ? iMn : iMc;
                M_[n] = m < 0 ? 0 : (m > mx ? mx : m);
                H_[n] = nw ? hN : hC;
                Ng_[n] = nw ? ngN : ngC;
                Sil_[n] = nw ? slN : slC;
            }
        }

        // ---- record ----
        if (kRel == 1)
            kSpEnd = kFzA + iX;
        double kJ0 = (kRel == 1 ? kJ : 0);
        double kWb = (kRel == 1 ? kFzA : kW);
        double kWe0 = 1; // 1: every sample written; 0: aK decides (release); -1: up to kFzStop (frozen)
        if (kFzOn == 0)
        {
            kWadv = ksmps;
        }
        else if (kRel == 1)
        {
            kWe0 = 0;
            kWadv = ksmps - kJ;
        }
        else
        {
            kWe0 = -1;
            kWadv = clampLo(kFzStop - kW, 0, ksmps);
        }
        const int32_t wmod = histMod(kWb - kJ0); // history index of aW[0]
        double kSpl = (kWb < kSpEnd ? 1 : 0);
        if (kWadv >= ksmps && kSpl == 0)
        {
            int32_t i = wmod;
            for (int n = 0; n < KS; n++)
            {
                BL[i] = (int16_t)xL_[n];
                BR[i] = (int16_t)xR_[n];
                if (++i == kHist)
                    i = 0;
            }
        }
        else if (kWadv > 0)
        {
            const int32_t iXi = (int32_t)iX;
            const int32_t ph0 = clampBig(kWb - kJ0 - kSpEnd + iX); // aW - kSpEnd + iX at n = 0
            const int32_t stop0 = clampBig(kFzStop - (kWb - kJ0)); // kFzStop - aW at n = 0
            int32_t i = wmod;
            for (int n = 0; n < KS; n++)
            {
                const bool we = kWe0 > 0 ? true : (kWe0 == 0 ? K_[n] != 0 : (stop0 - n >= 1));
                if (we)
                {
                    const int32_t d = ph0 + n;
                    const int32_t g = d <= 0 ? 0 : (d >= iXi ? 32768 : (int32_t)(((uint32_t)d << 15) / (uint32_t)iXi));
                    int32_t s1, s0;
                    sinCos(g, s1, s0);
                    BL[i] = (int16_t)clamp16((xL_[n] * s1 + BL[i] * s0) >> 15);
                    BR[i] = (int16_t)clamp16((xR_[n] * s1 + BR[i] * s0) >> 15);
                }
                if (++i == kHist)
                    i = 0;
            }
        }
        BL[kHist] = BL[0];
        BR[kHist] = BR[0];

        // ---- play: the current mapping ----
        if (kFzOn == 0)
        {
            kFzAn = kW + kJ;
            const int32_t anMod = histMod(kFzAn);
            const bool eng = kEng == 1;
            int32_t i = wmod;
            for (int n = 0; n < KS; n++)
            {
                const bool e = eng && K_[n];
                Am_[n] = e ? anMod : i;
                AA_[n] = e ? 0 : Tp_[n];
                if (++i == kHist)
                    i = 0;
            }
            kXs = 0;
        }
        else if (kRel == 1)
        {
            const int32_t aMod = histMod(kFzA), ia = (int32_t)kFza;
            int32_t i = wmod;
            for (int n = 0; n < KS; n++)
            {
                Am_[n] = K_[n] ? i : aMod;
                AA_[n] = K_[n] ? Tp_[n] : ia;
                if (++i == kHist)
                    i = 0;
            }
            kXs = 0;
        }
        else
        {
            const int32_t aMod = histMod(kFzA), ia = (int32_t)kFza;
            for (int n = 0; n < KS; n++)
            {
                Am_[n] = aMod;
                AA_[n] = ia;
            }
            kXs = kFzX;
        }
        rdMap(U_, Sp_, M_, H_, Ng_, Lp_, Am_, AA_, (int32_t)kXs, NL_, NR_, NW_);
        {
            const int32_t pS = kPassS >= 0.5, pE = kPassE >= 0.5, pN = kPassN >= 0.5;
            for (int n = 0; n < KS; n++)
            {
                const bool half = 2 * Tp_[n] > Lh_[n];
                const int32_t ps = K_[n] ? pE : pS;
                const int32_t pe = K_[n] ? pN : pE;
                const int32_t pass = half ? pe : ps;
                NW_[n] = Sil_[n] ? 0 : (pass ? 32768 : NW_[n]);
            }
        }
        if (kXP < 1)
        {
            // the old mapping, crossfaded out over 4 ms
            const int32_t iTo = (int32_t)kTo, iLoE = (int32_t)kLoE, iLoC = (int32_t)kLoC, iMo = (int32_t)kMo;
            const uint32_t so16 = q16(kSo);
            const int32_t hO = halfSpeed(kSpdo), ngO = kSpdo < 0;
            const bool live = kOLive == 1;
            const int32_t aoMod = histMod(kAo), iao = (int32_t)kao;
            int32_t i = wmod;
            for (int n = 0; n < KS; n++)
            {
                int32_t t = iTo + n;
                while (t >= iLoE)
                    t -= iLoE;
                const uint32_t tp16 = (uint32_t)t << 16;
                const uint32_t rx = tp16 / so16;
                U_[n] = tp16 - rx * so16;
                Sp_[n] = so16;
                M_[n] = iMo;
                H_[n] = hO;
                Ng_[n] = ngO;
                Lp_[n] = iLoC;
                Am_[n] = live ? i : aoMod;
                AA_[n] = live ? t : iao;
                if (++i == kHist)
                    i = 0;
            }
            rdMap(U_, Sp_, M_, H_, Ng_, Lp_, Am_, AA_, (int32_t)kXso, OL_, OR_, OW_);
            const int32_t xp0 = (int32_t)std::lround(kXP * 65536.0), xpStep = (int32_t)std::lround(65536.0 / iX);
            const bool lin = kXlaw >= 0.5;
            const int32_t silo = kSilo >= 0.5, passo = kPasso >= 0.5;
            for (int n = 0; n < KS; n++)
            {
                int32_t x = xp0 + n * xpStep;
                x = x < 0 ? 0 : (x > 65536 ? 65536 : x);
                const int32_t g = x >> 1;
                int32_t gn, go;
                if (lin)
                {
                    gn = g;
                    go = 32768 - g;
                }
                else
                {
                    sinCos(g, gn, go);
                }
                const int32_t ow = silo ? 0 : (passo ? 32768 : OW_[n]);
                WL_[n] = (((NL_[n] * NW_[n]) >> 15) * gn >> 15) + (((OL_[n] * ow) >> 15) * go >> 15);
                WR_[n] = (((NR_[n] * NW_[n]) >> 15) * gn >> 15) + (((OR_[n] * ow) >> 15) * go >> 15);
            }
            kXP = kXP + ksmps / iX;
            kTo = kTo + ksmps;
            kTo = kTo - kLoE * ffloor(kTo / kLoE);
        }
        else
        {
            for (int n = 0; n < KS; n++)
            {
                WL_[n] = (NL_[n] * NW_[n]) >> 15;
                WR_[n] = (NR_[n] * NW_[n]) >> 15;
            }
        }

        // ---- advance the clock state ----
        double kTl = kT + ksmps - 1;
        double kRold = kRc;
        if (kTl >= kLe)
        {
            kTnew2 = kTl + 1 - kLe;
            kRc = ffloor((kTl - kLe) / kSn);
            kBnd1 = 1;
            if (kEng == 1)
            {
                kFzOn = 1; kFzA = kW + kJ; kFzStop = kFzA + iX; kFza = 0; kFzG = kG + kJ; kFzX = iX;
            }
            else if (kRel == 1)
            {
                kFzOn = 0;
            }
            kLc = kLn; kLe = kLn; kSc = kSn; kPassS = kPassE; kPassC = kPassN; kT = kTnew2;
            cycleFlag_ = true;
        }
        else
        {
            kRc = ffloor(kTl / kSc);
            kBnd1 = (kRc != kRold ? 1 : 0);
            kT = kTl + 1;
        }
        kW = kWb + kWadv;
        kG = kG + ksmps;
        if (kBnd1 == 1)
        {
            sliceFlag_ = true;
            kMc = kMn; kJmpC = kJmpN; kRjC = kRjN; kSpdC = kSpdN; kSilC = kSilN; kMtrC = kMbN;
            kR1 = rng_.range(0, 1);
            kJmpN = (kR1 < kBrk ? 1 : 0);
            kRjN = rng_.range(0, 0.9999);
            kR2 = rng_.range(0, 1);
            kSilN = (kR2 < kBrk * 0.9 ? 1 : 0);
            kR3 = rng_.range(0, 1);
            if (kR3 < kBnd)
            {
                kR4 = rng_.range(0, 1);
                if (kR4 < 0.2) kSpdN = -1;
                else if (kR4 < 0.4) kSpdN = 0.5;
                else if (kR4 < 0.55) kSpdN = 2;
                else if (kR4 < 0.7) kSpdN = -0.5;
                else if (kR4 < 0.85) kSpdN = 1.5;
                else kSpdN = -2;
            }
            else
            {
                kSpdN = 1;
            }
        }
        if (kBrk <= 0.001)
        {
            kSilN = 0;
            kJmpN = 0;
        }
        if (kBnd <= 0.001)
            kSpdN = 1;

        // ---- DAMAGE: sample-rate + bit reduction of the wet signal ----
        if (kCor > 0.02)
        {
            if (kCor != corCached_)
            {
                corCached_ = kCor;
                dsStep_ = (int32_t)std::lround((1 + kCor * kCor * 24) * 65536.0);
                const double steps = std::exp(std::log(2.0) * (16 - 12 * kCor));
                const double q = 32768.0 / steps; // one quantiser step in 16-bit units
                qStep8_ = q > 1.0 ? (int32_t)std::lround(q * 256.0) : 0;
            }
            for (int n = 0; n < KS; n++)
            {
                if (dsNext_ < dsCount_)
                {
                    dsNext_ += dsStep_;
                    dsHeldL_ = WL_[n];
                    dsHeldR_ = WR_[n];
                }
                WL_[n] = dsHeldL_;
                WR_[n] = dsHeldR_;
                dsCount_ += 65536;
            }
            dsNext_ -= dsCount_; // keep the counters small (only their difference matters)
            dsCount_ = 0;
            if (qStep8_ > 0)
            {
                const int32_t q = qStep8_, h = q >> 1;
                for (int n = 0; n < KS; n++)
                {
                    WL_[n] = quant(WL_[n], q, h);
                    WR_[n] = quant(WR_[n], q, h);
                }
            }
        }

        // ---- mix (engaging FREEZE with MIX at 0 jumps to fully wet) ----
        double kMixE = (kReq == 1 && kMix < 0.02 ? 1 : kMix);
        mixY_ = (1.0 - mixC_) * kMixE + mixC_ * mixY_;
        fzChanged_(kFzOn);
        int32_t m = (int32_t)std::lround(mixY_ * 32768.0);
        m = m < 0 ? 0 : (m > 32768 ? 32768 : m);
        for (int n = 0; n < KS; n++)
        {
            const int32_t l = xL_[n], r = xR_[n];
            outL[n] = clamp16(l + (((clamp16(WL_[n]) - l) * m) >> 15));
            outR[n] = clamp16(r + (((clamp16(WR_[n]) - r) * m) >> 15));
        }
    }

private:
    static constexpr int kTab = 1024;

    static inline int32_t clamp16(int32_t x) { return x < -32768 ? -32768 : (x > 32767 ? 32767 : x); }
    static inline uint32_t q16(double x) { return (uint32_t)(x * 65536.0 + 0.5); }
    static inline int32_t halfSpeed(double spd) { return (int32_t)(std::fabs(spd) * 2 + 0.5); } // |speed| x 2: 1..4
    static inline int32_t clampBig(double x)
    {
        return x < -1073741824.0 ? -1073741824 : (x > 1073741824.0 ? 1073741824 : (int32_t)x);
    }
    static inline int32_t histMod(double x) // x is a whole number
    {
        int32_t r = (int32_t)((int64_t)x % kHist);
        return r < 0 ? r + kHist : r;
    }
    static BRK_INLINE int32_t wrapH(int32_t i)
    {
        while (i < 0)
            i += kHist;
        while (i >= kHist)
            i -= kHist;
        return i;
    }
    static inline int32_t quant(int32_t x, int32_t q, int32_t h)
    {
        const int32_t v = x * 256;
        const int32_t k = (v >= 0 ? v + h : v - h) / q;
        return (k * q) >> 8;
    }
    // sin and cos of g x 90 degrees, g in Q15 (0..32768), results Q15
    BRK_INLINE void sinCos(int32_t g, int32_t &s, int32_t &c) const
    {
        const int32_t i = g >> 5, f = g & 31;
        s = sinTab_[i] + (((sinTab_[i + 1] - sinTab_[i]) * f) >> 5);
        const int32_t gc = 32768 - g, j = gc >> 5, fc = gc & 31;
        c = sinTab_[j] + (((sinTab_[j + 1] - sinTab_[j]) * fc) >> 5);
    }
    static BRK_INLINE int32_t rd(const int16_t *B, int32_t i, int32_t xf)
    {
        const int32_t a = B[i], b = B[i + 1];
        return a + (((b - a) * (xf >> 2)) >> 14);
    }

    // LAZERS RdMap: where in the history each output sample is read, and its window (fade at slice edges).
    // U: position in the slice (Q16), Sp: slice length (Q16), M: slice number, H: |speed| x 2, Ng: backwards,
    // Lc: cycle length, Am: history index of the mapping's anchor, aa: anchor offset, xs: freeze seam crossfade.
    BRK_FAST BRK_HOT void rdMap(const uint32_t *U, const uint32_t *Sp, const int32_t *M, const int32_t *H, const int32_t *Ng,
                        const int32_t *Lc, const int32_t *Am, const int32_t *aa, int32_t xs, int32_t *oL, int32_t *oR,
                        int32_t *win) const
    {
        const uint32_t fade16 = (uint32_t)fadeI_ << 16;
        const int32_t xs16 = xs << 16;
        for (int n = 0; n < KS; n++)
        {
            const uint32_t sp = Sp[n], u = U[n];
            uint32_t p;
            switch (H[n])
            {
            case 1: p = u >> 1; break;
            case 3: p = u + (u >> 1); break;
            case 4: p = u << 1; break;
            default: p = u; break;
            }
            if (p >= sp)
                p -= sp;
            const uint32_t s = Ng[n] ? sp - p : p;
            const int32_t x16 = M[n] * (int32_t)sp + (int32_t)s;
            const int32_t xi = x16 >> 16, xf = x16 & 0xFFFF;
            const int32_t d = x16 - (aa[n] << 16);
            const int32_t base = Am[n] - aa[n] + xi;
            if (xs > 0)
            {
                const int32_t dd = d + xs16;
                const int32_t g = dd <= 0 ? 0 : (dd >= xs16 ? 32768 : (int32_t)(((uint32_t)dd / (uint32_t)xs) >> 1));
                int32_t g2, g1;
                sinCos(g, g2, g1);
                const int32_t i1 = wrapH(base), i2 = wrapH(base - Lc[n]);
                oL[n] = (rd(BL, i1, xf) * g1 + rd(BL, i2, xf) * g2) >> 15;
                oR[n] = (rd(BR, i1, xf) * g1 + rd(BR, i2, xf) * g2) >> 15;
            }
            else
            {
                const int32_t i = wrapH(base - (d > 32768 ? Lc[n] : 0));
                oL[n] = rd(BL, i, xf);
                oR[n] = rd(BR, i, xf);
            }
            const uint32_t dr = (sp - u < u) ? sp - u : u;
            uint32_t ds = (sp - s < s) ? sp - s : s;
            switch (H[n])
            {
            case 1: ds <<= 1; break;
            case 3: ds = (ds << 1) / 3; break;
            case 4: ds >>= 1; break;
            default: break;
            }
            const uint32_t mm = ds < dr ? ds : dr;
            win[n] = mm >= fade16 ? 32768 : (int32_t)((mm / (uint32_t)fadeI_) >> 1);
        }
    }

    int16_t *BL = nullptr, *BR = nullptr;
    double sr_ = 44000;
    double prm_[COUNT] = {};
    double bpm_ = 0, ts_ = 0;
    Random rng_;
    uint16_t sinTab_[kTab + 2] = {};

    double giFade = 0, iMaxP = 0, iX = 0, iPre = 0, iMinL = 0, iMaxL = 0;
    int32_t fadeI_ = 132;
    double kBoot = 1, kG = 0, kW = 0, kT = 0, kLc = 0, kLe = 0, kNc = 1, kSc = 0, kLnomC = 0, kCarry = 0, kRc = 0,
           kPassS = 1, kPassC = 1, kFzOn = 0, kFzA = 0, kFzStop = 0, kSpEnd = 0, kFza = 0, kFzG = 0, kFzX = 0,
           kMc = 0, kMtrC = 0, kJmpC = 0, kRjC = 0, kSpdC = 1, kSilC = 0, kJmpN = 0, kRjN = 0, kSpdN = 1, kSilN = 0,
           kXP = 1, kXlaw = 0, kTo = 0, kLoE = 0, kLoC = 0, kSo = 0, kMo = 0, kSpdo = 1, kSilo = 0, kOLive = 1,
           kAo = 0, kao = 0, kXso = 0, kPasso = 1, kSince = 0, kMovT = 0, kTsX = 0;
    double kTnew2 = 0, kBnd1 = 0, kFzAn = 0, kLnew = 0, kTnew = 0, kHq = 0, kFzNx = 0, kPassEo = 0, kLiveO = 0,
           kLiveN = 0, kTb = 0, kHp = 0, kE = 0, kR1 = 0, kR2 = 0, kR3 = 0, kR4 = 0, kXs = 0, kWadv = 0;
    ChangeDetect tsChanged_, fzChanged_;
    double mixC_ = 0, mixY_ = 0;
    double bpmLo_ = 20, bpmHi_ = 400;
    const double *syncT_ = kSyncT;
    int syncN_ = 10;
    double timeCached_ = -1, freePer_ = 0.0125, corCached_ = -1, beatsUsed_ = 1;
    int32_t dsNext_ = 0, dsCount_ = 0, dsStep_ = 65536, dsHeldL_ = 0, dsHeldR_ = 0, qStep8_ = 0;
    bool cycleFlag_ = false, sliceFlag_ = false;

    // per-block scratch (members: keeps the audio stack small)
    int32_t xL_[KS], xR_[KS], K_[KS], Tp_[KS], Lp_[KS], Lh_[KS], M_[KS], H_[KS], Ng_[KS], Sil_[KS], Am_[KS], AA_[KS];
    uint32_t U_[KS], Sp_[KS];
    int32_t NL_[KS], NR_[KS], NW_[KS], OL_[KS], OR_[KS], OW_[KS], WL_[KS], WR_[KS];
};

/// The Citadel runs whole 48-sample audio blocks
using RepeatEngine = RepeatCore<48>;

} // namespace brk
