// Host-test wrapper around the ER-301 brk repeat engine (BRK-ER301 dsp/, BRAEK's own code, copied unchanged into
// this folder).  Its namespace is renamed so it can sit next to the Citadel engine in one test program.
#define brk brk_ref
#include "RepeatEngine.cpp"
#undef brk
#include "RefWrap.hpp"
#include <vector>

struct RefEngine::Impl {
    brk_ref::RepeatEngine e;
    std::vector<float> mem;
};
RefEngine::RefEngine(double sr) : p(new Impl) {
    p->mem.assign(brk_ref::RepeatEngine::memoryNeeded(sr), 0.0f);
    p->e.prepare(sr, p->mem.data(), p->mem.size());
}
RefEngine::~RefEngine() { delete p; }
void RefEngine::setParam(int id, double v) { p->e.setParam(id, v); }
void RefEngine::setTransport(double bpm, double ts) {
    brk_ref::RepeatTransport t; t.bpm = bpm; t.timeSec = ts; p->e.setTransport(t);
}
void RefEngine::process(const double *iL, const double *iR, double *oL, double *oR) { p->e.process(iL, iR, oL, oR); }
bool RefEngine::frozen() const { return p->e.frozen(); }
