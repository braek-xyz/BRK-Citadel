/*
 * BRK Repeat engine: beat repeater / micro sound repeater for the RP2040 (Bastl Citadel / Kastle 2).
 * Copyright (c) 2026 BRAEK. MIT License (see LICENSE in the repository root).
 *
 * Plain C++ (no Kastle 2 / Pico SDK dependency), integer and fixed-point maths only (the RP2040 has no FPU).
 * Design follows BRAEK's LAZERS REPEAT + FREEZE (cycle, repeats, position, braek, wonk, damage, freeze, hold),
 * rewritten for a small memory and integer maths.
 *
 * How it works
 * - Audio is recorded into a ring buffer all the time (except while frozen / held).
 * - Time is cut into CYCLES of length L samples; each cycle is cut into R SLICES of length S = L / R.
 * - In every slice we play the audio that belongs to slice number M of the current cycle (POSITION). Slice k of a
 *   cycle with M == k is just the live sound, so R = 1 (or M == k) passes the input through unchanged.
 * - BRAEK: per slice, a chance to jump to a random slice or to be silent. WONK: per slice, a chance to play at another
 *   speed and/or backwards. Random choices are made one slice ahead so fades can be planned.
 * - FREEZE: at the next cycle start, stop recording and keep looping the cycle that just finished. Releases at a
 *   cycle start too. HOLD: instantly loop the last slice heard (S samples), until released.
 * - DAMAGE: sample-rate and bit reduction of the repeats.
 * - Fades of about 3 ms are applied at every slice edge that is not continuous, so there are no clicks.
 *
 * Real-time rules: no allocation, no locks, no floating point in Process().
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brk
{

class RepeatEngine
{
public:
    static constexpr int32_t kBufLen = 50000;   ///< Samples per channel (about 1.1 s at 44 kHz). Must fit RAM with Bastl's framework (FX Wizard uses 2 x 50800).
    static constexpr int32_t kFade = 132;       ///< Fade length at slice edges, samples (3 ms).
    static constexpr int32_t kMinCycle = 550;   ///< Shortest cycle, samples (12.5 ms).
    static constexpr int32_t kMaxCycle = kBufLen - 2 * kFade; ///< Longest cycle, samples.
    static constexpr int32_t kMaxRepeats = 16;
    static constexpr int32_t kOne = 256;        ///< Speed 1.0 in Q8.

    enum class State
    {
        LIVE,      ///< Passing the input (nothing repeating right now)
        REPEATING, ///< Repeating slices
        FROZEN,    ///< FREEZE active
        HOLD       ///< HOLD active
    };

    struct Params
    {
        int32_t cycle_len = 22000; ///< Cycle length in samples (clamped to kMinCycle..kMaxCycle)
        int32_t repeats = 1;       ///< 1..16
        int32_t position = 0;      ///< 0..4095: which slice repeats
        int32_t braek = 0;         ///< 0..4095: random gaps and jumps
        int32_t wonk = 0;          ///< 0..4095: random speed / direction
        int32_t damage = 0;        ///< 0..4095: rate and bit reduction of the repeats
        int32_t mix = 32767;       ///< 0..32767: dry / wet
        bool freeze = false;       ///< FREEZE request (applied at the next cycle start)
        bool hold = false;         ///< HOLD (applied at once)
    };

    /// @param left, right buffers of kBufLen samples each, owned by the caller (static memory).
    void Init(int16_t *left, int16_t *right)
    {
        buf_[0] = left;
        buf_[1] = right;
        for (int32_t i = 0; i < kBufLen; i++)
        {
            left[i] = 0;
            right[i] = 0;
        }
        w_ = 0;
        t_ = 0;
        L_ = 22000;
        R_ = 1;
        S_ = L_;
        cs_ = 0;
        k_ = 0;
        cur_ = Slice{};
        next_ = Slice{};
        edge_in_cont_ = true;
        cont_ = true;
        next_k_ = 0;
        frozen_ = false;
        writing_ = true;
        hold_ = false;
        hold_start_ = 0;
        hold_len_ = kMinCycle;
        hold_pos_ = 0;
        hold_x_ = 0;
        mix_ = 32768;
        dmg_acc_ = 0;
        dmg_hold_[0] = dmg_hold_[1] = 0;
        rng_ = 0x9E3779B9u;
        force_boundary_ = false;
        cycle_flag_ = false;
        slice_flag_ = false;
    }

    /// Call once per audio block, before Process().
    void SetParams(const Params &p)
    {
        p_ = p;
        if (p_.cycle_len < kMinCycle)
            p_.cycle_len = kMinCycle;
        if (p_.cycle_len > kMaxCycle)
            p_.cycle_len = kMaxCycle;
        if (p_.repeats < 1)
            p_.repeats = 1;
        if (p_.repeats > kMaxRepeats)
            p_.repeats = kMaxRepeats;
    }

    /// Start a new cycle now (clock / reset). Ignored if a cycle started very recently.
    void Realign()
    {
        if (t_ >= kFade)
            force_boundary_ = true;
    }

    /// Interleaved stereo in / out (in[2*i] = left, in[2*i+1] = right), Q15 values in int16_t or int32_t.
    /// in and out may be the same buffer.
    template <typename T>
    void Process(const T *in, T *out, size_t n)
    {
        // mix: ramp across the block to avoid zipper noise; FREEZE / HOLD are always fully wet (as in LAZERS)
        // Q15 with 32768 = fully wet, so a full mix is bit-exact
        const int32_t mix_target = (frozen_ || hold_ || p_.hold || p_.mix >= 32767) ? 32768 : (p_.mix < 0 ? 0 : p_.mix);
        const int32_t mix_start = mix_;
        const int32_t mix_step = (mix_target - mix_start) / static_cast<int32_t>(n == 0 ? 1 : n);

        // DAMAGE settings (per block)
        const int32_t d = p_.damage;
        const bool dmg_on = d > 80;
        const int32_t dmg_inc = kOne + static_cast<int32_t>((static_cast<int64_t>(d) * d * 24 * kOne) / (4095 * 4095));
        const int32_t dmg_bits = (d * 12) / 4095;
        const int32_t dmg_mask = ~((1 << dmg_bits) - 1);

        // HOLD edge: start (instant). Release: crossfade back.
        if (p_.hold && !hold_)
        {
            hold_ = true;
            hold_len_ = S_ < kMinCycle ? kMinCycle : S_;
            if (hold_len_ > kMaxCycle)
                hold_len_ = kMaxCycle;
            hold_start_ = Wrap(w_ - hold_len_);
            hold_pos_ = 0;
            writing_ = false;
        }
        else if (!p_.hold && hold_)
        {
            hold_ = false;
            if (!frozen_)
                writing_ = true;
        }

        for (size_t i = 0; i < n; i++)
        {
            const int32_t inL = Sat16(static_cast<int32_t>(in[2 * i]));
            const int32_t inR = Sat16(static_cast<int32_t>(in[2 * i + 1]));

            // ---- cycle / slice clock ----
            if (t_ >= L_ || force_boundary_)
                CycleStart();
            int32_t k = t_ / S_;
            if (k >= R_)
                k = R_ - 1;
            if (k != k_)
                SliceStart(k);
            const int32_t s = t_ - k_ * S_; // position inside the slice

            // ---- record (before reading, so a live slice reads this very sample) ----
            if (writing_)
            {
                buf_[0][w_] = static_cast<int16_t>(inL);
                buf_[1][w_] = static_cast<int16_t>(inR);
                w_ = Wrap(w_ + 1);
            }

            // ---- normal path (slices) ----
            int32_t wetL, wetR;
            ReadSlice(s, wetL, wetR);

            // ---- HOLD path, crossfaded in / out over kFade samples ----
            if (hold_ || hold_x_ > 0)
            {
                int32_t hL, hR;
                const int32_t idx = Wrap(hold_start_ + hold_pos_);
                const int32_t env = EdgeEnv(hold_pos_, hold_len_, true, true);
                hL = (buf_[0][idx] * env) >> 15;
                hR = (buf_[1][idx] * env) >> 15;
                hold_pos_++;
                if (hold_pos_ >= hold_len_)
                    hold_pos_ = 0;
                if (hold_ && hold_x_ < kFade)
                    hold_x_++;
                else if (!hold_ && hold_x_ > 0)
                    hold_x_--;
                const int32_t g = (hold_x_ * 32768) / kFade;
                wetL = ((hL * g) + (wetL * (32768 - g))) >> 15;
                wetR = ((hR * g) + (wetR * (32768 - g))) >> 15;
            }

            // ---- DAMAGE ----
            if (dmg_on)
            {
                dmg_acc_ += kOne;
                if (dmg_acc_ >= dmg_inc)
                {
                    dmg_acc_ -= dmg_inc;
                    dmg_hold_[0] = wetL & dmg_mask;
                    dmg_hold_[1] = wetR & dmg_mask;
                }
                wetL = dmg_hold_[0];
                wetR = dmg_hold_[1];
            }

            // ---- mix ----
            const int32_t m = mix_start + mix_step * static_cast<int32_t>(i);
            int32_t oL = ((inL * (32768 - m)) + (wetL * m)) >> 15;
            int32_t oR = ((inR * (32768 - m)) + (wetR * m)) >> 15;
            out[2 * i] = static_cast<T>(Sat16(oL));
            out[2 * i + 1] = static_cast<T>(Sat16(oR));

            t_++;
        }
        mix_ = mix_target;
    }

    // ---- status, for LEDs and outputs (read and cleared by the UI) ----
    bool TakeCycleFlag()
    {
        const bool f = cycle_flag_;
        cycle_flag_ = false;
        return f;
    }
    bool TakeSliceFlag()
    {
        const bool f = slice_flag_;
        slice_flag_ = false;
        return f;
    }
    State GetState() const
    {
        if (hold_)
            return State::HOLD;
        if (frozen_)
            return State::FROZEN;
        return cur_.live ? State::LIVE : State::REPEATING;
    }
    int32_t GetCycleLength() const { return L_; }

private:
    struct Slice
    {
        int32_t m = 0;       ///< which slice of the cycle to read
        int32_t speed = kOne; ///< Q8, negative = backwards
        bool silent = false;
        bool jumped = false; ///< BRAEK jump: m was chosen at random
        bool live = true;    ///< continuous with the input (reads its own time, speed 1, not silent, not frozen)
    };

    static inline int32_t Wrap(int32_t x)
    {
        while (x < 0)
            x += kBufLen;
        while (x >= kBufLen)
            x -= kBufLen;
        return x;
    }

    static inline int16_t Sat16(int32_t x)
    {
        return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
    }

    inline uint32_t Rand()
    {
        uint32_t x = rng_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng_ = x;
        return x;
    }
    /// Random number 0..4095
    inline int32_t Rand12() { return static_cast<int32_t>(Rand() >> 20); }

    /// Fade envelope (Q15, 32768 = 1.0 so full level is bit-exact) for position s in a slice of length len.
    static inline int32_t EdgeEnv(int32_t s, int32_t len, bool fade_in, bool fade_out)
    {
        int32_t f = kFade;
        if (2 * f > len)
            f = len / 2;
        if (f < 1)
            return 32768;
        int32_t e = 32768;
        if (fade_in && s < f)
            e = (s * 32768) / f;
        if (fade_out && (len - 1 - s) < f)
        {
            const int32_t e2 = ((len - 1 - s) * 32768) / f;
            if (e2 < e)
                e = e2;
        }
        return e;
    }

    int32_t PositionSlice(int32_t R) const
    {
        // 0..4095 -> 0..R-1, rounded
        return (p_.position * (R - 1) + 2047) / 4095;
    }

    /// Decide what slice k (of R) will play.
    Slice MakeSlice(int32_t k, int32_t R, bool will_be_frozen)
    {
        Slice sl;
        sl.m = PositionSlice(R);
        if (p_.braek > 16)
        {
            sl.jumped = Rand12() < p_.braek;
            if (sl.jumped)
                sl.m = static_cast<int32_t>((static_cast<uint32_t>(Rand12()) * static_cast<uint32_t>(R)) >> 12);
            if (Rand12() < (p_.braek * 9) / 10)
                sl.silent = true;
        }
        if (p_.wonk > 16 && Rand12() < p_.wonk)
        {
            const int32_t r = Rand12();
            if (r < 819)
                sl.speed = -kOne; // backwards
            else if (r < 1638)
                sl.speed = kOne / 2; // half speed
            else if (r < 2253)
                sl.speed = 2 * kOne; // double speed
            else if (r < 2867)
                sl.speed = -kOne / 2; // half speed backwards
            else if (r < 3481)
                sl.speed = (3 * kOne) / 2; // a fifth up
            else
                sl.speed = -2 * kOne; // double speed backwards
        }
        sl.live = !will_be_frozen && sl.m == k && sl.speed == kOne && !sl.silent;
        return sl;
    }

    void CycleStart()
    {
        force_boundary_ = false;
        cycle_flag_ = true;
        const int32_t prev_cs = cs_;
        const int32_t prev_L = L_;

        // FREEZE engage / release, at the cycle start
        if (p_.freeze && !frozen_)
        {
            frozen_ = true;
            writing_ = false;
            // loop the cycle that just finished: from the previous cycle start to the write head
            (void)prev_L;
            cs_ = prev_cs;
            L_ = Wrap(w_ - prev_cs);
            if (L_ < kMinCycle)
                L_ = kMinCycle;
            if (L_ > kMaxCycle)
                L_ = kMaxCycle;
        }
        else if (!p_.freeze && frozen_)
        {
            frozen_ = false;
            if (!hold_)
                writing_ = true;
            cs_ = w_;
            L_ = p_.cycle_len;
        }
        else if (frozen_)
        {
            // stay on the same frozen cycle
        }
        else
        {
            cs_ = w_;
            L_ = p_.cycle_len;
        }
        R_ = p_.repeats;
        if (R_ > L_ / (2 * kFade))
            R_ = L_ / (2 * kFade) < 1 ? 1 : L_ / (2 * kFade); // keep slices long enough for the fades
        S_ = L_ / R_;
        L_ = S_ * R_; // whole slices only
        t_ = 0;
        k_ = -1;
        SliceStart(0);
    }

    void SliceStart(int32_t k)
    {
        slice_flag_ = true;
        // The edge into this slice was decided when it was planned (cont_), so both sides agree on the fade.
        edge_in_cont_ = cont_;
        if (next_k_ == k)
        {
            cur_ = next_;
            if (k == 0)
            {
                // the number of slices may have changed at the cycle start: re-aim the slice number
                if (!cur_.jumped)
                    cur_.m = PositionSlice(R_);
                if (cur_.m > R_ - 1)
                    cur_.m = R_ - 1;
                cur_.live = !frozen_ && cur_.m == 0 && cur_.speed == kOne && !cur_.silent;
            }
        }
        else
        {
            cur_ = MakeSlice(k, R_, frozen_);
        }
        k_ = k;
        // plan the next slice now, so the current one can fade out if needed
        next_k_ = (k + 1 < R_) ? k + 1 : 0;
        next_ = MakeSlice(next_k_, R_, next_k_ == 0 ? p_.freeze : frozen_);
        cont_ = cur_.live && next_.live;
    }

    /// Read the normal (slice) path at position s of the current slice.
    inline void ReadSlice(int32_t s, int32_t &outL, int32_t &outR)
    {
        if (cur_.silent)
        {
            outL = 0;
            outR = 0;
            return;
        }
        int32_t idx0, frac;
        const int32_t S = S_;
        if (cur_.speed == kOne)
        {
            idx0 = cur_.m * S + s;
            frac = 0;
        }
        else
        {
            const int32_t sp = cur_.speed < 0 ? -cur_.speed : cur_.speed;
            int32_t pos = s * sp; // Q8 within the slice
            const int32_t span = S << 8;
            while (pos >= span)
                pos -= span;
            if (cur_.speed < 0)
                pos = ((S - 1) << 8) - pos;
            idx0 = cur_.m * S + (pos >> 8);
            frac = pos & 0xFF;
        }

        // idx0 is relative to the cycle start. Was it recorded yet in this cycle?
        int32_t base = cs_;
        if (!frozen_)
        {
            const int32_t written = Wrap(w_ - cs_); // samples recorded since this cycle started
            if (idx0 >= written)
            {
                // not recorded yet: take it from the previous cycle (one cycle earlier)
                base = (2 * L_ <= kBufLen) ? cs_ - L_ : cs_;
            }
        }
        const int32_t a = Wrap(base + idx0);
        const int32_t b = (a + 1 >= kBufLen) ? 0 : a + 1;
        int32_t l = buf_[0][a];
        int32_t r = buf_[1][a];
        if (frac)
        {
            l += ((buf_[0][b] - l) * frac) >> 8;
            r += ((buf_[1][b] - r) * frac) >> 8;
        }
        if (!(cur_.live && edge_in_cont_ && cont_))
        {
            const bool fin = !(cur_.live && edge_in_cont_);
            const bool fout = !cont_;
            const int32_t env = EdgeEnv(s, S, fin, fout);
            l = (l * env) >> 15;
            r = (r * env) >> 15;
        }
        outL = l;
        outR = r;
    }

    int16_t *buf_[2] = {nullptr, nullptr};
    Params p_;
    int32_t w_ = 0;          ///< write index
    int32_t t_ = 0;          ///< position in the cycle
    int32_t L_ = 22000;      ///< cycle length
    int32_t R_ = 1;          ///< repeats (slices per cycle)
    int32_t S_ = 22000;      ///< slice length
    int32_t cs_ = 0;         ///< write index at the cycle start
    int32_t k_ = 0;          ///< current slice number
    int32_t next_k_ = 0;
    Slice cur_, next_;
    bool edge_in_cont_ = true; ///< the edge into the current slice is continuous (no fade-in)
    bool cont_ = true;         ///< the edge out of the current slice is continuous (no fade-out)
    bool frozen_ = false;
    bool writing_ = true;
    bool hold_ = false;
    int32_t hold_start_ = 0, hold_len_ = kMinCycle, hold_pos_ = 0, hold_x_ = 0;
    int32_t mix_ = 32768;
    int32_t dmg_acc_ = 0;
    int32_t dmg_hold_[2] = {0, 0};
    uint32_t rng_ = 0x9E3779B9u;
    bool force_boundary_ = false;
    bool cycle_flag_ = false;
    bool slice_flag_ = false;
};

} // namespace brk
