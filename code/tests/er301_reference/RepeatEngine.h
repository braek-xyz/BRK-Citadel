// BRK-ER301 - brk repeat engine: the LAZERS 2.0.0 REPEAT + FREEZE effect as a stand-alone stereo effect.
//
// Plain C++11, no dependencies, no ER-301 code.  Ported from LAZERS 2.0.0 (BRAEK's own code,
// PLUGIN-WORKFLOW plugins/lazers/engine/Engine.cpp, struct Engine::Master::Repeat); the DSP is unchanged, see
// RepeatEngine.cpp for the list of differences.  Works in blocks of KS = 32 samples like LAZERS.
//
// Memory: the engine allocates nothing big itself.  The caller gives it the history memory (memoryNeeded() floats, about
// 4 MB at 48 kHz); on the ER-301 that comes from the firmware's BigHeap, like the core Delay unit's buffers.
// Real-time rules: process() never allocates, locks or touches files.
#pragma once
#include "DspCommon.h"
#include <cstddef>

namespace brk {

// controls, with the LAZERS raw ranges
enum RepeatParam {
    R_MIX,          // 0..1     incoming vs repeats                       (default 0)
    R_TIME,         // 0..1     repeat cycle 12.5..320 ms, or note length (default 0.5)
    R_REPEATS,      // 1..16    slices per cycle                          (default 1)
    R_POSITION,     // 0..1     which slice repeats                       (default 0)
    R_BRAEK,        // 0..1     random gaps and jumps                     (default 0)
    R_WONK,         // 0..1     random speed / direction per repeat       (default 0)
    R_DAMAGE,       // 0..1     sample-rate and bit reduction             (default 0)
    R_SYNC,         // 0 / 1    TIME follows tempo                        (default 0)
    R_FREEZE,       // 0 / 1    loop the current cycle                    (default 0)
    R_FREEZEHOLD,   // 0 / 1    momentary FREEZE (used in HOLD mode)      (default 0)
    R_HOLD,         // 0 / 1    HOLD mode: FREEZE starts / stops at once  (default 0)
    R_COUNT
};

struct RepeatTransport {
    double bpm = 0;       // 0 = no tempo (120 is used)
    double timeSec = 0;   // song position in seconds; SYNC locks to it while it keeps moving
};

class RepeatEngine {
public:
    RepeatEngine();
    ~RepeatEngine();
    static size_t memoryNeeded(double sampleRate);   // floats of history memory (10 s per channel + guard points)
    // false (and the engine passes sound through unchanged) if memory is null or too small
    bool prepare(double sampleRate, float* memory, size_t floats);
    void reset();                       // silence, rewind
    void setParam(int id, double v) { if (id >= 0 && id < R_COUNT) prm[id] = v; }
    double param(int id) const { return prm[id]; }
    void setTransport(const RepeatTransport& t) { tr = t; }
    // one block of KS samples; in and out may be the same arrays
    void process(const double* inL, const double* inR, double* outL, double* outR);
    // the same with float in / out (the ER-301's signals); the in / out scrub runs 4 samples at a time on NEON
    void processFloat(const float* inL, const float* inR, float* outL, float* outR);
    bool frozen() const;
    double maxCycleSeconds() const;

private:
    struct Repeat;
    Rates rates;
    Random rng;
    double prm[R_COUNT];
    RepeatTransport tr;
    Repeat* rep;
    float* mem;
    size_t memFloats;
    RepeatEngine(const RepeatEngine&);
    RepeatEngine& operator=(const RepeatEngine&);
};

} // namespace brk
