// BRK-ER301 - brk repeat engine (see RepeatEngine.h).
//
// PORT NOTES.  struct Repeat below is LAZERS 2.0.0 engine/Engine.cpp, struct Engine::Master::Repeat (BRAEK's own code),
// copied line for line so the sound stays the same.  Differences from LAZERS:
//   1. History is stored as float (LAZERS: double) and its length follows the sample rate (smallest power of two that
//      holds 10 s; LAZERS: 1048576).  The longest cycle stays min(4 s, 0.4 x history) as in LAZERS: 4 s at 48 kHz.
//   2. The LAZERS idle self-heal (gkHeal) is not part of this unit: it is always 0.
//   3. The random stream is the unit's own (seed 1234, as LAZERS seeds its event stream).
//   4. The guard point after the history is kept equal to point 0 (LAZERS leaves it 0, which gives a rare one-sample
//      dip when a read crosses the end of the history).
//   5. All per-block scratch arrays are members, not stack locals (the ER-301 audio task stack is small).
//   6. roundEven / truncTo0 use floor / ceil (DspCommon.h).  C++11.
//   7. CPU (0.1.5): floor is ffloor / ifloor (DspCommon.h, same results without a library call on the ER-301), and the
//      crossfade gains sin / cos (g x 1.5707963) come from a table (QuarterSine, within 3e-7).  Fast HOLD changes keep
//      the engine crossfading all the time, which made those calls the biggest cost.
//   8. CPU (0.2.0): clamps, floors, min and the DAMAGE rounding are done without floating-point compares (each compare
//      is a slow VFP-to-core transfer on the ER-301), divisions by the history length / fade / crossfade lengths are
//      multiplications, aTp / aSp is worked out once per sample (not three times), and processFloat() scrubs the
//      ER-301's float signals on NEON.  Output matches 0.1.5 to within 1e-12 (235 dB below the signal).
// Everything else (clock, slicing, BRAEK, WONK, DAMAGE, FREEZE / HOLD, SYNC, crossfades) is unchanged.
#include "RepeatEngine.h"
#include <algorithm>
#include <cstring>
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace brk {

namespace {
enum {
    P_R_MIX = R_MIX, P_R_TIME = R_TIME, P_R_REPEATS = R_REPEATS, P_R_POSITION = R_POSITION, P_R_BRAEK = R_BRAEK,
    P_R_WONK = R_WONK, P_R_DAMAGE = R_DAMAGE, P_R_SYNC = R_SYNC, P_R_FREEZE = R_FREEZE, P_R_FREEZEHOLD = R_FREEZEHOLD,
    P_R_HOLD = R_HOLD
};
typedef RepeatTransport Transport;

// history length: smallest power of two holding 10 s (at least 2^16)
inline double historyLength(double sr) {
    double n = 65536;
    while (n < 10.0 * sr && n < 8388608) n *= 2;
    return n;
}

// brk port (0.2.0), CPU: on the ER-301's CPU every floating-point compare costs a slow transfer to the integer core,
// and LAZERS' clamps and floors use 2..4 of them each, 60 per sample in all.  These do the same without compares.
// 0..1 clamp: exact for whole numbers (every clamp whose result must be exactly 0 or 1 gets whole numbers), within
// 2e-16 otherwise (the gains).
inline double clamp01(double x) { return 0.5 * (std::fabs(x) - std::fabs(x - 1.0) + 1.0); }
// lo..hi clamp, exact for whole numbers
inline double clampW(double x, double lo, double hi) { return 0.5 * (std::fabs(x - lo) - std::fabs(x - hi) + lo + hi); }
// smaller of two (within 2e-16 relative)
inline double minA(double a, double b) { return 0.5 * (a + b - std::fabs(a - b)); }
// floor of x >= 0 (below 2^31): truncation is floor there, exactly
inline double tfloor(double x) { return (double)(int32_t)x; }
// floor of a history position / history length (-2^20 < x < 2^30).  Exact for whole-sample positions; for others it
// can round up by one within 2^-32 of the next whole number, which only moves a read from the end of the history to
// its start (the same point: the guard point equals point 0).
inline double bfloor(double x) { return (double)(int32_t)(x + 1048576.0) - 1048576.0; }
// round to nearest, ties to even (|x| < 2^51), as roundEven but without compares: adding and taking away 1.5 x 2^52
// leaves the double rounded to a whole number by the CPU itself (the empty asm keeps the compiler from cancelling it)
inline double rintFast(double x) {
    double t = x + 6755399441055744.0;
#if defined(__arm__)
    __asm__("" : "+w"(t));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__("" : "+x"(t));
#else
    volatile double v = t; t = v;
#endif
    return t - 6755399441055744.0;
}
} // namespace

// =====================================================================================================================
// LAZERS 2.0.0 REPEAT + FREEZE (Repeater + RdMap UDOs)
// =====================================================================================================================
struct RepeatEngine::Repeat {
    float* BL = nullptr;             // history (giHist + guard point); float storage on the ER-301 (LAZERS: double).
    float* BR = nullptr;             // The memory is given to the engine (RepeatEngine::prepare), never allocated here.
    double giHist = 1048576, giFade = 0;   // giHist is set from the sample rate in init() (power of two)
    double iMaxP = 0, iX = 0, iPre = 0, iMinL = 0, iMaxL = 0;
    double kBoot = 1, kG = 0, kW = 0, kT = 0, kLc = 0, kLe = 0, kNc = 1, kSc = 0, kLnomC = 0, kCarry = 0, kRc = 0,
           kPassS = 1, kPassC = 1, kFzOn = 0, kFzA = 0, kFzStop = 0, kSpEnd = 0, kFza = 0, kFzG = 0, kFzX = 0,
           kMc = 0, kMtrC = 0, kJmpC = 0, kRjC = 0, kSpdC = 1, kSilC = 0, kJmpN = 0, kRjN = 0, kSpdN = 1, kSilN = 0,
           kXP = 1, kXlaw = 0, kTo = 0, kLoE = 0, kLoC = 0, kSo = 0, kMo = 0, kSpdo = 1, kSilo = 0, kOLive = 1,
           kAo = 0, kao = 0, kXso = 0, kPasso = 1, kSince = 0, kMovT = 0, kTsX = 0;
    // variables that keep their value from earlier blocks when a branch does not set them (as in v12.6)
    double kTnew2 = 0, kBnd1 = 0, kFzAn = 0, kLnew = 0, kTnew = 0, kHq = 0, kFzNx = 0, kPassEo = 0, kLiveO = 0,
           kLiveN = 0, kTb = 0, kHp = 0, kE = 0, kR1 = 0, kR2 = 0, kR3 = 0, kR4 = 0, kInc = 0, kSteps = 0, kXs = 0,
           kWadv = 0;
    // brk port: every per-block scratch array lives here, not on the stack.  The ER-301 audio task has a small stack
    // (SYS/BIOS default), LAZERS' ~12 KB of local arrays would overflow it.  Same names as the LAZERS locals.
    Blk aU, aS, aX, aC1, aAbs, aG, aI1, aI2, aL1, aR1, aL2, aR2,
        aI, aT, aK, aTp, aSp, aLp, aLh, aNew, aM, aSpd, aSil, aW, aWe, aWi, aOL0, aOR0,
        aPs1, aPs0, vL, vR, aA, aa, aQ, aIA, aNg, aQo, aIAo, aNgo, aNL, aNR, aNW, aWetL, aWetR, aTo, aSo, aMo, aSpo, aLo, aAo, aao, aOL, aOR, aOW;
    Blk ioInL, ioInR, ioOutL, ioOutR;   // RepeatEngine::process in / out
    ChangeDetect tsChanged, fzChanged;
    Glide pHlP, pMixS;
    Downsampler foldL, foldR;
    double kReqOut = 0;
    QuarterSine qs;   // brk port: crossfade gain table (built once, when the engine is made)

    void init(const Rates& r) {
        // giHist is set by RepeatEngine::prepare together with BL / BR
        std::memset(BL, 0, sizeof(float) * ((size_t)giHist + 1)); std::memset(BR, 0, sizeof(float) * ((size_t)giHist + 1));
        giFade = 0.003 * r.sr;
        { double b = 0.4 * giHist / r.sr; iMaxP = (b < 4 ? b : 4); }
        iX = roundEven(0.004 * r.sr);
        iPre = roundEven(0.0035 * r.sr);
        iMinL = roundEven(0.0125 * r.sr);
        iMaxL = roundEven(iMaxP * r.sr);
        kBoot = 1; kG = 0; kW = 0; kT = 0; kLc = iMinL; kLe = iMinL; kNc = 1; kSc = iMinL; kLnomC = iMinL; kCarry = 0;
        kRc = 0; kPassS = 1; kPassC = 1; kFzOn = 0; kFzA = 0; kFzStop = 0; kSpEnd = 0; kFza = 0; kFzG = 0; kFzX = 0;
        kMc = 0; kMtrC = 0; kJmpC = 0; kRjC = 0; kSpdC = 1; kSilC = 0; kJmpN = 0; kRjN = 0; kSpdN = 1; kSilN = 0;
        kXP = 1; kXlaw = 0; kTo = 0; kLoE = iMinL; kLoC = iMinL; kSo = iMinL; kMo = 0; kSpdo = 1; kSilo = 0;
        kOLive = 1; kAo = 0; kao = 0; kXso = 0; kPasso = 1; kSince = 0; kMovT = 0; kTsX = 0;
        tsChanged = ChangeDetect(); fzChanged = ChangeDetect();
        pHlP.start(r, 0.05);
        pMixS.start(r, 0.02);
        foldL = Downsampler(); foldR = Downsampler();
    }

    inline void tableiRaw(const float* t, const double* ndxf, double* out) const {
        const float* f = t;
        const int32_t mask = (int32_t)giHist - 1;
        // brk port (0.2.0): the index is wrapped (0..giHist) by every caller; one history length is added so it is
        // never below 0 and truncation is floor (the fraction stays within 2.4e-10)
        for (int n = 0; n < KS; n++) {
            double tmp = ndxf[n] + giHist;
            int32_t ndx = (int32_t)tmp;
            double frac = tmp - ndx;
            ndx &= mask;
            double x1 = f[ndx], x2 = f[ndx + 1];
            out[n] = x1 + (x2 - x1) * frac;
        }
    }

    // brk port (0.2.0): aQ = aTp / aSp (the caller has it); aIA = 1 / |aSpd| and aNg = (aSpd < 0 ? 1 : 0) from a few
    // per-block values; no compares
    void rdMap(const double* aTp, const double* aSp, const double* aQ, const double* aM, const double* aSpd,
               const double* aIA, const double* aNg, const double* aLc, const double* aA, const double* aa, double kXsI, double* aOL,
               double* aOR, double* aWin) {
        // (scratch blocks are members: see 'scratch' above)
        const double invHist = 1.0 / giHist;   // exact (a power of two)
        for (int n = 0; n < KS; n++) {
            double aRp = tfloor(aQ[n]);        // aTp >= 0, aSp > 0
            aU[n] = aTp[n] - aRp * aSp[n];
            double aUph = aQ[n] - aRp;         // = aU / aSp
            aAbs[n] = std::fabs(aSpd[n]);
            double v = aAbs[n] * aUph;
            double aF = v - tfloor(v);
            aS[n] = aF + aNg[n] * (1 - 2 * aF);   // LAZERS: aNeg = clamp(-aSpd x 1e6, 0, 1), 0 or 1 (|aSpd| >= 0.5)
            aX[n] = (aM[n] + aS[n]) * aSp[n];
            aC1[n] = aA[n] - aa[n] + aX[n];
        }
        if (kXsI > 0) {
            // (scratch blocks are members: see 'scratch' above)
            const double invXs = 1.0 / kXsI;
            for (int n = 0; n < KS; n++) {
                aG[n] = clamp01((aX[n] - aa[n] + kXsI) * invXs);
                double aC2 = aC1[n] - aLc[n];
                aI1[n] = aC1[n] - giHist * bfloor(aC1[n] * invHist);
                aI2[n] = aC2 - giHist * bfloor(aC2 * invHist);
            }
            tableiRaw(BL, aI1, aL1); tableiRaw(BR, aI1, aR1);
            tableiRaw(BL, aI2, aL2); tableiRaw(BR, aI2, aR2);
            for (int n = 0; n < KS; n++) {
                double aG1, aG2;
                qs.at(aG[n], aG2, aG1);   // brk port: table for sin / cos (aG[n] x 1.5707963)
                aOL[n] = aL1[n] * aG1 + aL2[n] * aG2;
                aOR[n] = aR1[n] * aG1 + aR2[n] * aG2;
            }
        } else {
            // (scratch blocks are members: see 'scratch' above)
            for (int n = 0; n < KS; n++) {
                double g = clamp01((aX[n] - aa[n] - 0.5) * 1000000);
                double aC = aC1[n] - g * aLc[n];
                aI[n] = aC - giHist * bfloor(aC * invHist);
            }
            tableiRaw(BL, aI, aOL); tableiRaw(BR, aI, aOR);
        }
        const double invFade = 1.0 / giFade;
        for (int n = 0; n < KS; n++) {
            double aDR = minA(aSp[n] - aU[n], aU[n]);
            double aDS = minA(1 - aS[n], aS[n]) * aSp[n] * aIA[n];
            aWin[n] = clamp01(minA(aDS, aDR) * invFade);
        }
    }

    void process(const Rates& r, Random& g, const double* P, const Transport& tr, double gkHeal,
                 const double* aInL, const double* aInR, double* aOutL, double* aOutR) {
        const double ksmps = KS, sr = r.sr;
        double kMix = P[P_R_MIX], kTime = P[P_R_TIME], kRep = P[P_R_REPEATS], kTrv = P[P_R_POSITION],
               kBrk = P[P_R_BRAEK], kBnd = P[P_R_WONK], kCor = P[P_R_DAMAGE], kFrz = P[P_R_FREEZE],
               kFrzH = P[P_R_FREEZEHOLD], kHold = P[P_R_HOLD], kSyn = P[P_R_SYNC], kBpm = tr.bpm,
               kTs = tr.timeSec;
        double kMov = tsChanged(kTs) ? 1 : 0;
        kSince = (kMov == 1 ? 0 : kSince + ksmps / sr);
        kMovT = (kMov == 1 ? 0.25 : kMovT - ksmps / sr);
        kTsX = kTs + kSince;
        double kBpmU = (kBpm > 20 && kBpm < 400 ? kBpm : 120);

        double kIx = clampLo(truncTo0(kTime * 9.999), 0, 9);
        double kBeats = tabK(kSyncT, 16, kIx);
        double kPerS = (kSyn >= 0.5 ? kBeats * 60 / kBpmU : 0.0125 * std::exp(std::log(320.0) * kTime));
        double kPerL = clampLo(kPerS, 0.0125, iMaxP);
        double kLnomE = kPerL * sr;
        double kNn = clampLo(roundEven(kRep), 1, 16);
        double kHoldM = (kHold >= 0.5 ? 1 : 0);
        double kReqH = (kFrzH >= 0.5 || kFrz >= 0.5 ? 1 : 0);
        double kReq = (kHoldM == 1 ? kReqH : (kFrz >= 0.5 ? 1 : 0));
        double kSyncOK = (kSyn >= 0.5 && kMovT > 0 && kPerS <= iMaxP ? 1 : 0);
        double kBB = (kBnd <= 0.001 && kBrk <= 0.001 ? 1 : 0);
        kReqOut = kReq;

        if (kBoot == 1) {
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
        if (kXP >= 1 && kChN + kChL + kChM + kChF > 0) {
            kTo = kT; kLoE = kLe; kLoC = kLc; kSo = kSc; kMo = kMc; kSpdo = kSpdC; kSilo = kSilC;
            kOLive = 1 - kFzOn; kAo = kFzA; kao = kFza; kXso = kFzX * kFzOn;
            kFzNx = (kHoldM == 0 ? kReq : kFzOn);
            kPassEo = kPassC * (kNc == 1 && kFzNx == 0 && kBB == 1 ? 1 : 0);
            kPasso = (kT >= kLe * 0.5 ? kPassEo : kPassS);
            kXP = 0;
            kXlaw = 0;
            if (kChL == 1) {
                kLnew = clampLo(roundEven(kLnomE), iMinL, iMaxL);
                if (kSyncOK == 1) {
                    kHq = kTsX * kBpmU / (60 * kBeats) + iPre / kLnew;
                    kTnew = roundEven((kHq - ffloor(kHq)) * kLnew);
                } else {
                    kTnew = roundEven(kT / kLe * kLnew);
                }
                kTnew = clampLo(kTnew, 0, kLnew - 1);
                if (kFzOn == 1) {
                    kFza = kTnew - (kG - kFzG);
                    kFza = kFza - kLnew * ffloor(kFza / kLnew);
                }
                if (kSyncOK == 0 && std::fabs(kLnew - kLoE) < 0.1 * kLoE) kXlaw = 1;
                kT = kTnew; kLc = kLnew; kLe = kLnew; kLnomC = kLnomE;
            }
            kNc = kNn;
            kSc = kLc / kNc;
            kMtrC = roundEven(kTrv * (kNc - 1));
            kMc = (kJmpC == 1 && kBrk > 0.001 ? truncTo0(kRjC * kNc) : kMtrC);
            kMc = clampLo(kMc, 0, kNc - 1);
            if (kChF == 1) {
                if (kReq == 1) {
                    kFzOn = 1; kFzA = kW; kFzStop = kW + iX; kFzG = kG;
                    kFza = kT - kLc * ffloor(kT / kLc);
                    kFzX = iX;
                    if (kChN + kChL + kChM == 0 && ffloor(kT / kSc) != kMc) kXP = 1;
                } else {
                    kFzOn = 0; kW = kFzA; kSpEnd = kFzA + iX;
                }
            }
            kPassC = (kNc == 1 && kFzOn == 0 && kBB == 1 ? 1 : 0);
            kPassS = kPassS * kPassC;
            kLiveO = (kOLive == 1 && ffloor(kTo / kSo) == kMo && kSpdo == 1 ? 1 : 0);
            kLiveN = (kFzOn == 0 && ffloor(kT / kSc) == kMc && kSpdC == 1 ? 1 : 0);
            if (kLiveO == 1 && kLiveN == 1) kXlaw = 1;
            kRc = (kT > 0 ? ffloor((kT - 1) / kSc) : -1);
        }

        double kCross = (kT + ksmps > kLe ? 1 : 0);
        double kJ = kLe - kT;
        double kFzNext = (kHoldM == 0 ? kReq : kFzOn);
        double kLn = kLc;
        if (kCross == 1) {
            if (kSyncOK == 1) {
                kTb = kTsX + kJ / sr;
                kHp = kTb * kBpmU / (60 * kBeats) + iPre / kLnomE;
                kE = kHp - roundEven(kHp);
                kLn = roundEven(kLnomE * (1 - kE));
            } else {
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

        // ---- per-sample clock ----
        // (scratch blocks are members: see 'scratch' above)
        double kMbN = roundEven(kTrv * (kNc - 1));
        double kMn = (kBrk > 0.001 && kJmpN == 1 ? truncTo0(kRjN * kNc) : kMbN);
        kMn = clampLo(kMn, 0, kNc - 1);
        // brk port (0.2.0): the clamps below get whole numbers (kT, kLe, kRc, kMc, kMn, kNc are whole), so the
        // compare-free clamps are exact; aTp >= 0 so truncation is floor.  aQ and aIA are kept for rdMap.
        const double mx = kNc - 1;
        const double iaC = 1.0 / std::fabs(kSpdC), iaN = 1.0 / std::fabs(kSpdN);
        const double ngC = (kSpdC < 0 ? 1 : 0), ngN = (kSpdN < 0 ? 1 : 0);
        if (kCross == 1) {
            for (int n = 0; n < KS; n++) {
                double aIdx = n;
                aT[n] = kT + aIdx;
                aK[n] = clamp01(aT[n] - kLe + 1);
                aTp[n] = aT[n] - aK[n] * kLe;
                aSp[n] = kSc + aK[n] * (kSn - kSc);
                aLp[n] = kLc + aK[n] * (kLn - kLc);
                aLh[n] = kLe + aK[n] * (kLn - kLe);
            }
        } else {
            // no cycle end in this block (kT + 32 <= kLe): aK is 0 for every sample, exactly
            for (int n = 0; n < KS; n++) {
                double aIdx = n;
                aT[n] = kT + aIdx;
                aK[n] = 0;
                aTp[n] = aT[n];
                aSp[n] = kSc;
                aLp[n] = kLc;
                aLh[n] = kLe;
            }
        }
        for (int n = 0; n < KS; n++) {
            aQ[n] = aTp[n] / aSp[n];
            double aRx = tfloor(aQ[n]);
            aNew[n] = clamp01(aK[n] * 1000 + aRx - kRc);
            aM[n] = clampW(kMc + aNew[n] * (kMn - kMc), 0, mx);   // (mx = 0 gives 0, as LAZERS' mx / 2)
            aSpd[n] = kSpdC + aNew[n] * (kSpdN - kSpdC);
            aIA[n] = iaC + aNew[n] * (iaN - iaC);
            aNg[n] = ngC + aNew[n] * (ngN - ngC);
            aSil[n] = kSilC + aNew[n] * (kSilN - kSilC);
        }

        // ---- record ----
        if (kRel == 1) kSpEnd = kFzA + iX;
        double kJ0 = (kRel == 1 ? kJ : 0);
        double kWb = (kRel == 1 ? kFzA : kW);
        // (scratch blocks are members: see 'scratch' above)
        for (int n = 0; n < KS; n++) aW[n] = kWb + (double)n - kJ0;
        if (kFzOn == 0) {
            for (int n = 0; n < KS; n++) aWe[n] = 1;
            kWadv = ksmps;
        } else if (kRel == 1) {
            for (int n = 0; n < KS; n++) aWe[n] = aK[n];
            kWadv = ksmps - kJ;
        } else {
            for (int n = 0; n < KS; n++) aWe[n] = clamp01(kFzStop - aW[n]);
            kWadv = clampLo(kFzStop - kW, 0, ksmps);
        }
        double kHw = 1 - gkHeal;
        // brk port (0.2.0): write positions are whole samples, so this wrap is exact and aWi is a whole number in
        // 0..giHist-1 (the mask only guards)
        const double invHist = 1.0 / giHist;
        for (int n = 0; n < KS; n++) aWi[n] = aW[n] - giHist * bfloor(aW[n] * invHist);
        double kSpl = (kWb < kSpEnd ? 1 : 0);
        const double invX = 1.0 / iX;
        const int32_t wMask = (int32_t)giHist - 1;
        auto wIdx = [&](double x) { return (int32_t)x & wMask; };
        if (kWadv >= ksmps && kSpl == 0) {
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); BL[i] = aInL[n] * kHw; }
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); BR[i] = aInR[n] * kHw; }
        } else if (kWadv > 0) {
            // (scratch blocks are members: see 'scratch' above)
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); aOL0[n] = BL[i]; }
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); aOR0[n] = BR[i]; }
            for (int n = 0; n < KS; n++) {
                double ph = clamp01((aW[n] - kSpEnd + iX) * invX);
                qs.at(ph, aPs1[n], aPs0[n]);   // brk port: table for sin / cos (ph x 1.5707963)
            }
            for (int n = 0; n < KS; n++) vL[n] = (aInL[n] * kHw * aPs1[n] + aOL0[n] * aPs0[n]) * aWe[n] + aOL0[n] * (1 - aWe[n]);
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); BL[i] = vL[n]; }
            for (int n = 0; n < KS; n++) vR[n] = (aInR[n] * kHw * aPs1[n] + aOR0[n] * aPs0[n]) * aWe[n] + aOR0[n] * (1 - aWe[n]);
            for (int n = 0; n < KS; n++) { int32_t i = wIdx(aWi[n]); BR[i] = vR[n]; }
        }

        // brk port fix: keep the guard point equal to point 0, so a read across the end of the history does not
        // interpolate towards 0 (LAZERS leaves it at 0: a rare one-sample dip once per history wrap)
        BL[(size_t)giHist] = BL[0]; BR[(size_t)giHist] = BR[0];

        // ---- play: the current mapping ----
        // (scratch blocks are members: see 'scratch' above)
        if (kFzOn == 0) {
            kFzAn = kW + kJ;
            for (int n = 0; n < KS; n++) {
                aA[n] = aW[n] + kEng * aK[n] * (kFzAn - aW[n]);
                aa[n] = aTp[n] - kEng * aK[n] * aTp[n];
            }
            kXs = 0;
        } else if (kRel == 1) {
            for (int n = 0; n < KS; n++) {
                aA[n] = kFzA + aK[n] * (aW[n] - kFzA);
                aa[n] = kFza + aK[n] * (aTp[n] - kFza);
            }
            kXs = 0;
        } else {
            for (int n = 0; n < KS; n++) { aA[n] = kFzA; aa[n] = kFza; }
            kXs = kFzX;
        }
        rdMap(aTp, aSp, aQ, aM, aSpd, aIA, aNg, aLp, aA, aa, kXs, aNL, aNR, aNW);
        for (int n = 0; n < KS; n++) {
            double aHalf = clamp01((aTp[n] - aLh[n] * 0.5) * 1000000);
            double aPS = kPassS + aK[n] * (kPassE - kPassS);
            double aPE = kPassE + aK[n] * (kPassN - kPassE);
            double aPass = aPS + aHalf * (aPE - aPS);
            aNW[n] = (aNW[n] + aPass * (1 - aNW[n])) * (1 - aSil[n]);
        }
        double kHlP = pHlP.next(gkHeal);
        double kGh = 1 - kHlP;
        // (scratch blocks are members: see 'scratch' above)
        if (kXP < 1) {
            // (scratch blocks are members: see 'scratch' above)
            const double iaO = 1.0 / std::fabs(kSpdo), ngO = (kSpdo < 0 ? 1 : 0);
            for (int n = 0; n < KS; n++) {
                double t = kTo + (double)n;           // >= 0
                aTo[n] = t - kLoE * tfloor(t / kLoE);
                aQo[n] = aTo[n] / kSo;
                aIAo[n] = iaO; aNgo[n] = ngO;
                aSo[n] = kSo; aMo[n] = kMo; aSpo[n] = kSpdo; aLo[n] = kLoC;
                if (kOLive == 1) { aAo[n] = aW[n]; aao[n] = aTo[n]; }
                else { aAo[n] = kAo; aao[n] = kao; }
            }
            rdMap(aTo, aSo, aQo, aMo, aSpo, aIAo, aNgo, aLo, aAo, aao, kXso, aOL, aOR, aOW);
            for (int n = 0; n < KS; n++) {
                aOW[n] = (aOW[n] + kPasso * (1 - aOW[n])) * (1 - kSilo);
                double aXp = clamp01(kXP + (double)n * invX);
                double sXp, cXp;
                qs.at(aXp, sXp, cXp);   // brk port: table for sin / cos (aXp x 1.5707963)
                double aGn = sXp * (1 - kXlaw) + aXp * kXlaw;
                double aGo = cXp * (1 - kXlaw) + (1 - aXp) * kXlaw;
                aWetL[n] = (aNL[n] * aNW[n] * aGn + aOL[n] * aOW[n] * aGo) * kGh;
                aWetR[n] = (aNR[n] * aNW[n] * aGn + aOR[n] * aOW[n] * aGo) * kGh;
            }
            kXP = kXP + ksmps / iX;
            kTo = kTo + ksmps;
            kTo = kTo - kLoE * ffloor(kTo / kLoE);
        } else {
            for (int n = 0; n < KS; n++) { aWetL[n] = aNL[n] * aNW[n] * kGh; aWetR[n] = aNR[n] * aNW[n] * kGh; }
        }

        // ---- advance the clock state ----
        double kTl = kT + ksmps - 1;
        double kRold = kRc;
        if (kTl >= kLe) {
            kTnew2 = kTl + 1 - kLe;
            kRc = ffloor((kTl - kLe) / kSn);
            kBnd1 = 1;
            if (kEng == 1) {
                kFzOn = 1; kFzA = kW + kJ; kFzStop = kFzA + iX; kFza = 0; kFzG = kG + kJ; kFzX = iX;
            } else if (kRel == 1) {
                kFzOn = 0;
            }
            kLc = kLn; kLe = kLn; kSc = kSn; kPassS = kPassE; kPassC = kPassN; kT = kTnew2;
        } else {
            kRc = ffloor(kTl / kSc);
            kBnd1 = (kRc != kRold ? 1 : 0);
            kT = kTl + 1;
        }
        kW = kWb + kWadv;
        kG = kG + ksmps;
        if (kBnd1 == 1) {
            kMc = kMn; kJmpC = kJmpN; kRjC = kRjN; kSpdC = kSpdN; kSilC = kSilN; kMtrC = kMbN;
            kR1 = g.range(0, 1);
            kJmpN = (kR1 < kBrk ? 1 : 0);
            kRjN = g.range(0, 0.9999);
            kR2 = g.range(0, 1);
            kSilN = (kR2 < kBrk * 0.9 ? 1 : 0);
            kR3 = g.range(0, 1);
            if (kR3 < kBnd) {
                kR4 = g.range(0, 1);
                if (kR4 < 0.2) kSpdN = -1;
                else if (kR4 < 0.4) kSpdN = 0.5;
                else if (kR4 < 0.55) kSpdN = 2;
                else if (kR4 < 0.7) kSpdN = -0.5;
                else if (kR4 < 0.85) kSpdN = 1.5;
                else kSpdN = -2;
            } else {
                kSpdN = 1;
            }
        }
        if (kBrk <= 0.001) { kSilN = 0; kJmpN = 0; }
        if (kBnd <= 0.001) kSpdN = 1;

        // ---- DAMAGE: sample-rate + bit reduction of the wet signal ----
        if (kCor > 0.02) {
            kInc = 1 + kCor * kCor * 24;
            kSteps = std::exp(std::log(2.0) * (16 - 12 * kCor));
            foldL.process(aWetL, kInc);
            foldR.process(aWetR, kInc);
            // brk port (0.2.0): rintFast = roundEven here (|wet x steps| < 2^21); x (1 / steps) is within 1e-16
            const double invSteps = 1.0 / kSteps;
            for (int n = 0; n < KS; n++) aWetL[n] = rintFast(aWetL[n] * kSteps) * invSteps;
            for (int n = 0; n < KS; n++) aWetR[n] = rintFast(aWetR[n] * kSteps) * invSteps;
        }

        // ---- mix (engaging FREEZE with MIX at 0 jumps to fully wet) ----
        double kMixE = (kReq == 1 && kMix < 0.02 ? 1 : kMix);
        double kMixS = pMixS.next(kMixE);
        fzChanged(kFzOn);
        for (int n = 0; n < KS; n++) {
            aOutL[n] = aInL[n] * (1 - kMixS) + aWetL[n] * kMixS;
            aOutR[n] = aInR[n] * (1 - kMixS) + aWetR[n] * kMixS;
        }
    }
};

// =====================================================================================================================
RepeatEngine::RepeatEngine() : rep(new Repeat()), mem(nullptr), memFloats(0) {
    static const double def[R_COUNT] = {0, 0.5, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < R_COUNT; i++) prm[i] = def[i];
}
RepeatEngine::~RepeatEngine() { delete rep; }

size_t RepeatEngine::memoryNeeded(double sampleRate) {
    double sr = sampleRate > 1000 ? sampleRate : 48000;
    return 2 * ((size_t)historyLength(sr) + 1);
}

bool RepeatEngine::prepare(double sampleRate, float* memory, size_t floats) {
    rates.set(sampleRate > 1000 ? sampleRate : 48000);
    const size_t h = (size_t)historyLength(rates.sr);
    if (memory == nullptr || floats < 2 * (h + 1)) { mem = nullptr; memFloats = 0; return false; }
    mem = memory; memFloats = floats;
    rep->giHist = (double)h;
    rep->BL = mem;
    rep->BR = mem + h + 1;
    reset();
    return true;
}

void RepeatEngine::reset() {
    rng.seed(1234);
    if (mem) rep->init(rates);
}

void RepeatEngine::process(const double* inL, const double* inR, double* outL, double* outR) {
    if (!mem) {   // no memory: pass the input through
        for (int n = 0; n < KS; n++) { outL[n] = inL[n]; outR[n] = inR[n]; }
        return;
    }
    for (int n = 0; n < KS; n++) {      // LAZERS scrub: anything not finite or louder than 16 becomes 0
        double l = inL[n], r = inR[n];
        rep->ioInL[n] = (l <= 16 && l >= -16) ? l : 0;
        rep->ioInR[n] = (r <= 16 && r >= -16) ? r : 0;
    }
    rep->process(rates, rng, prm, tr, 0.0, rep->ioInL, rep->ioInR, rep->ioOutL, rep->ioOutR);
    for (int n = 0; n < KS; n++) {
        double l = rep->ioOutL[n], r = rep->ioOutR[n];
        outL[n] = (l <= 16 && l >= -16) ? l : 0;
        outR[n] = (r <= 16 && r >= -16) ? r : 0;
    }
}

// LAZERS scrub on floats: anything not finite or louder than 16 becomes 0
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
static inline void scrub16(const float* in, float* out) {
    const float32x4_t lim = vdupq_n_f32(16.0f);
    for (int n = 0; n < KS; n += 4) {
        float32x4_t x = vld1q_f32(in + n);
        uint32x4_t ok = vcleq_f32(vabsq_f32(x), lim);   // false for NaN
        vst1q_f32(out + n, vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(x), ok)));
    }
}
#else
static inline void scrub16(const float* in, float* out) {
    for (int n = 0; n < KS; n++) { float x = in[n]; out[n] = (x <= 16 && x >= -16) ? x : 0; }
}
#endif

void RepeatEngine::processFloat(const float* inL, const float* inR, float* outL, float* outR) {
    if (!mem) {
        for (int n = 0; n < KS; n++) { outL[n] = inL[n]; outR[n] = inR[n]; }
        return;
    }
    float sL[KS], sR[KS];
    scrub16(inL, sL); scrub16(inR, sR);
    for (int n = 0; n < KS; n++) { rep->ioInL[n] = sL[n]; rep->ioInR[n] = sR[n]; }
    rep->process(rates, rng, prm, tr, 0.0, rep->ioInL, rep->ioInR, rep->ioOutL, rep->ioOutR);
    for (int n = 0; n < KS; n++) { sL[n] = (float)rep->ioOutL[n]; sR[n] = (float)rep->ioOutR[n]; }
    scrub16(sL, outL); scrub16(sR, outR);
}

bool RepeatEngine::frozen() const { return mem && rep->kFzOn >= 0.5; }
double RepeatEngine::maxCycleSeconds() const { return rep->iMaxP; }

} // namespace brk
