/*
 * BRK Repeat: beat repeater / micro sound repeater app for the Bastl Citadel (and Kastle 2).
 * Copyright (c) 2026 BRAEK. MIT License (see LICENSE in the repository root).
 * Built on Bastl Instruments' Kastle 2 framework (MIT).
 *
 * Controls (Wave Bard panel names in brackets):
 *   TIME [PITCH], REPEATS [SAMPLE], POSITION [LENGTH], BRAEK [PITCH MOD], WONK [SAMPLE MOD]
 *   SHIFT + [SAMPLE MOD] = DAMAGE, SHIFT + [LENGTH] = MIX, SHIFT + [SAMPLE] = SYNC (left) / FREE (right)
 *   MODE button held = HOLD; SHIFT + tap MODE = FREEZE on / off
 *   TRIG input (gate) = HOLD; RESET input (gate) = FREEZE while high
 *   LFO, tempo, input / output level, clock and sync work as in Bastl's firmwares.
 */

#include "AppBrkRepeat.hpp"
#include <cmath>
#include "common/core/Kastle2.hpp"
#include "common/utils.hpp"

using namespace kastle2;

// Audio memory for the repeater: static, so nothing is allocated while audio runs.
static int16_t g_buf_left[brk::RepeatEngine::kBufLen];
static int16_t g_buf_right[brk::RepeatEngine::kBufLen];

void AppBrkRepeat::Init()
{
    inited_ = false;

    // We process the audio ourselves; LEDs 1 and 2 are ours in the normal layer
    Kastle2::base.SetFeatureEnabled(Base::Feature::AUDIO_CHAIN, false);
    Kastle2::base.SetFeatureEnabled(Base::Feature::GATE_INDICATION, false);
    Kastle2::base.SetFeatureEnabled(Base::Feature::GATE_OUT, false);

    pots_[Pot::TIME] = FancyPot::Create({.pot = Hardware::Pot::POT_5,
                                         .layer = Hardware::Layer::NORMAL,
                                         .initial_value = POT_HALF});
    pots_[Pot::REPEATS] = FancyPot::Create({.pot = Hardware::Pot::POT_6,
                                            .layer = Hardware::Layer::NORMAL,
                                            .initial_value = POT_MIN});
    pots_[Pot::POSITION] = FancyPot::Create({.pot = Hardware::Pot::POT_4,
                                             .layer = Hardware::Layer::NORMAL,
                                             .initial_value = POT_MIN});
    pots_[Pot::BRAEK] = FancyPot::Create({.pot = Hardware::Pot::POT_1,
                                          .layer = Hardware::Layer::NORMAL,
                                          .initial_value = POT_MIN});
    pots_[Pot::WONK] = FancyPot::Create({.pot = Hardware::Pot::POT_2,
                                         .layer = Hardware::Layer::NORMAL,
                                         .initial_value = POT_MIN});
    pots_[Pot::DAMAGE] = FancyPot::Create({.pot = Hardware::Pot::POT_2,
                                           .layer = Hardware::Layer::SHIFT,
                                           .initial_value = POT_MIN});
    pots_[Pot::MIX] = FancyPot::Create({.pot = Hardware::Pot::POT_4,
                                        .layer = Hardware::Layer::SHIFT,
                                        .initial_value = POT_MAX});
    pots_[Pot::SYNC] = FancyPot::Create({.pot = Hardware::Pot::POT_6,
                                         .layer = Hardware::Layer::SHIFT,
                                         .initial_value = POT_MIN});
    for (auto &pot : pots_)
    {
        pot->Init(AUDIO_LOOP_RATE);
    }

    engine_.Init(g_buf_left, g_buf_right);
    freeze_toggle_ = false;
    mode_was_pressed_ = false;
    mode_used_with_shift_ = false;
    beat_count_ = 0;

    inited_ = true;
}

void AppBrkRepeat::DeInit()
{
    inited_ = false;
}

int32_t AppBrkRepeat::PotWithCv(Pot pot, Hardware::AnalogInput cv, bool pitch_input)
{
    int32_t v = Kastle2::base.GetLfoMod().AdjustedPotValue(pots_[pot].get());
    int32_t c = Kastle2::hw.GetAnalogValue(cv);
    if (pitch_input)
    {
        c = (c * POT_MAX) / ADC_5V; // 0..5 V sweeps the whole knob
    }
    return constrain(v + c, POT_MIN, POT_MAX);
}

int32_t AppBrkRepeat::ComputeCycleLength(int32_t time, bool synced, int32_t &beats_q8)
{
    using E = brk::RepeatEngine;
    if (synced)
    {
        int32_t idx = (time * kSyncSteps) / (POT_MAX + 1);
        idx = constrain(idx, 0, kSyncSteps - 1);
        beats_q8 = kSyncBeatsQ8[idx];
        const int32_t beat_samples =
            static_cast<int32_t>(Kastle2::base.GetClock().GetTargetTicks()) * static_cast<int32_t>(AUDIO_BUFFER_SIZE);
        int32_t len = static_cast<int32_t>((static_cast<int64_t>(beat_samples) * beats_q8) >> 8);
        // too long for the memory: halve it (stays in time with the beat)
        while (len > E::kMaxCycle && beats_q8 > 8)
        {
            beats_q8 /= 2;
            len /= 2;
        }
        return len;
    }
    // free time: 12.5 ms .. about 1.2 s, exponential
    beats_q8 = 0;
    const float x = static_cast<float>(time) / static_cast<float>(POT_MAX);
    const float ratio = static_cast<float>(E::kMaxCycle) / static_cast<float>(E::kMinCycle);
    return static_cast<int32_t>(static_cast<float>(E::kMinCycle) * powf(ratio, x));
}

FASTCODE void AppBrkRepeat::AudioLoop(q15_t *input, q15_t *output, size_t size)
{
    if (!inited_)
    {
        return;
    }

    for (auto &pot : pots_)
    {
        pot->Process();
    }

    // ---- parameters ----
    synced_ = pots_[Pot::SYNC]->GetValue() < POT_HALF;
    const int32_t time = PotWithCv(Pot::TIME, CV_TIME, true);
    // knob right = shorter cycle feels natural for "faster" in free mode; keep knob right = longer for both
    params_.cycle_len = ComputeCycleLength(time, synced_, cycle_beats_q8_);
    const int32_t rep = PotWithCv(Pot::REPEATS, CV_REPEATS, true);
    params_.repeats = 1 + (rep * 16) / (POT_MAX + 1);
    params_.position = PotWithCv(Pot::POSITION, CV_POSITION, false);
    params_.braek = PotWithCv(Pot::BRAEK, CV_BRAEK, false);
    params_.wonk = PotWithCv(Pot::WONK, CV_WONK, false);
    params_.damage = Kastle2::base.GetLfoMod().AdjustedPotValue(pots_[Pot::DAMAGE].get());
    params_.mix = (Kastle2::base.GetLfoMod().AdjustedPotValue(pots_[Pot::MIX].get()) * 32767) / POT_MAX;

    // HOLD: MODE button (not when used with SHIFT) or TRIG gate
    const bool shift = Kastle2::hw.Pressed(Hardware::Button::SHIFT);
    const bool mode = Kastle2::hw.Pressed(Hardware::Button::MODE);
    if (mode && shift)
    {
        mode_used_with_shift_ = true;
    }
    if (!mode && mode_was_pressed_)
    {
        // MODE released: a SHIFT + MODE tap toggles FREEZE
        if (mode_used_with_shift_)
        {
            freeze_toggle_ = !freeze_toggle_;
        }
        mode_used_with_shift_ = false;
    }
    mode_was_pressed_ = mode;
    params_.hold = (mode && !mode_used_with_shift_) || Kastle2::hw.GetTriggerIn();
    params_.freeze = freeze_toggle_ || Kastle2::hw.GetResetIn();

    // ---- clock: start cycles on the beat when synced ----
    Clock &clock = Kastle2::base.GetClock();
    if (clock.IsNowReset())
    {
        beat_count_ = 0;
        if (synced_)
        {
            engine_.Realign();
        }
    }
    else if (clock.IsNowTrigger())
    {
        beat_count_++;
        if (synced_)
        {
            const int32_t beats_per_cycle = cycle_beats_q8_ >> 8; // 0 when the cycle is shorter than a beat
            if (beats_per_cycle <= 1 || (beat_count_ % beats_per_cycle) == 0)
            {
                engine_.Realign();
            }
        }
    }

    // ---- audio ----
    engine_.SetParams(params_);
    engine_.Process(input, output, size);

    // ---- outputs and LED info ----
    const bool slice = engine_.TakeSliceFlag();
    const bool cycle = engine_.TakeCycleFlag();
    if (cycle)
    {
        cycle_blink_ = 20; // audio loops (about 22 ms)
    }
    else if (cycle_blink_ > 0)
    {
        cycle_blink_ = cycle_blink_ - 1;
    }
    if (slice)
    {
        slice_blink_ = 10;
    }
    else if (slice_blink_ > 0)
    {
        slice_blink_ = slice_blink_ - 1;
    }
    // GATE out: a short pulse at every repeat (slice start)
    Kastle2::hw.SetGateOut(slice_blink_ > 5);
    state_ = static_cast<uint8_t>(engine_.GetState());
}

void AppBrkRepeat::UiLoop()
{
    if (Kastle2::hw.GetLayer() == Hardware::Layer::NORMAL || Kastle2::hw.GetLayer() == Hardware::Layer::MODE)
    {
        uint32_t color;
        switch (static_cast<brk::RepeatEngine::State>(state_))
        {
        case brk::RepeatEngine::State::LIVE:
            color = WS2812::DARK_GREEN;
            break;
        case brk::RepeatEngine::State::REPEATING:
            color = WS2812::ORANGE;
            break;
        case brk::RepeatEngine::State::FROZEN:
            color = WS2812::BLUE;
            break;
        case brk::RepeatEngine::State::HOLD:
        default:
            color = WS2812::RED;
            break;
        }
        if (slice_blink_ == 0)
        {
            color = WS2812::ApplyBrightness(color, 96);
        }
        Kastle2::hw.SetLed(Hardware::Led::LED_1, color);
        // LED 2: white flash at each cycle start; purple while FREE time
        uint32_t c2 = cycle_blink_ > 0 ? WS2812::WHITE : (synced_ ? WS2812::NONE : WS2812::ApplyBrightness(WS2812::PURPLE, 64));
        Kastle2::hw.SetLed(Hardware::Led::LED_2, c2);
    }
}
