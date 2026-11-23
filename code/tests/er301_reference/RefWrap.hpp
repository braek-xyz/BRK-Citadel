// Host-test wrapper around the ER-301 brk repeat engine (blocks of 32 samples, doubles).
#pragma once
struct RefEngine {
    static const int KS = 32;
    explicit RefEngine(double sr);
    ~RefEngine();
    void setParam(int id, double v);   // same ids as brk::RepeatCore<KS>::Param
    void setTransport(double bpm, double timeSec);
    void process(const double *inL, const double *inR, double *outL, double *outR);
    bool frozen() const;
    struct Impl;
    Impl *p;
};
