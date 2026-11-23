/*
 * BRK Repeat: beat repeater / micro sound repeater app for the Bastl Citadel (and Kastle 2).
 * Copyright (c) 2026 BRAEK. MIT License (see LICENSE in the repository root).
 * Built on Bastl Instruments' Kastle 2 framework (MIT).  The sound is BRAEK's LAZERS REPEAT, as in the ER-301
 * "brk repeat" unit (RepeatEngine.hpp).
 *
 * Controls (Wave Bard panel names in brackets):
 *   TIME [PITCH], REPEATS [SAMPLE], POSITION [LENGTH], BRAEK [PITCH MOD], WONK [SAMPLE MOD]
 *   SHIFT + [SAMPLE MOD] = DAMAGE, SHIFT + [LENGTH] = MIX,
 *   SHIFT + [SAMPLE] = CLOCK: FREE time, or synced with 1, 2, 4 or 24 clock pulses per beat
 *   MODE button held = HOLD; SHIFT + tap MODE = FREEZE on / off
 *   LFO, tempo, input / output level and the clock input work as in Bastl's firmwares.
 */

#include "AppBrkRepeat.hpp"
#include <cmath>
#include "common/core/Kastle2.hpp"
#include "common/utils.hpp"

using namespace kastle2;

// Audio memory for the repeater: static, so nothing is allocated while audio runs.
static int16_t g_buf_left[brk::RepeatEngine::kBufferLen];
static int16_t g_buf_right[brk::RepeatEngine::kBufferLen];

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
    // default: synced, one clock pulse per beat (a 1/4-note clock)
    pots_[Pot::CLOCK] = FancyPot::Create({.pot = Hardware::Pot::POT_6,
                                          .layer = Hardware::Layer::SHIFT,
                                          .initial_value = (POT_MAX * 3) / (2 * kClockZones)});
    for (auto &pot : pots_)
    {
        pot->Init(AUDIO_LOOP_RATE);
    }

    engine_.Init(g_buf_left, g_buf_right, static_cast<double>(SAMPLE_RATE));
    // free TIME: the whole knob sweeps 12.5 ms .. the longest cycle (engine TIME is 12.5 ms x 320^time)
    free_time_max_ = std::log(engine_.MaxCycleSeconds() / 0.0125) / std::log(320.0);
    freeze_toggle_ = false;
    mode_was_pressed_ = false;
    mode_used_with_shift_ = false;
    hold_engaged_ = false;
    pulse_count_ = 0;
    have_beat_ = false;
    beats_ = 0;
    samples_since_beat_ = 0;
    beat_period_ = 0;

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

// One audio loop of the clock: pulses -> beats (every pulses_per_beat_ pulses), the beat length in samples and the
// song position.  Same idea as the ER-301 unit: the position moves on smoothly between beats, but never past the next
// beat, so if the clock stops the position stops too (and after 0.25 s SYNC runs on at the last tempo).
void AppBrkRepeat::ClockTick()
{
    Clock &clock = Kastle2::base.GetClock();
    if (clock.IsNowReset())
    {
        pulse_count_ = 0;
        have_beat_ = false;
        beats_ = 0;
        samples_since_beat_ = 0;
    }
    if (clock.IsNowTrigger())
    {
        if (pulse_count_ % static_cast<uint32_t>(pulses_per_beat_) == 0)
        {
            if (have_beat_)
            {
                const double lo = SAMPLE_RATE * 60.0 / 400.0, hi = SAMPLE_RATE * 60.0 / 20.0;
                if (samples_since_beat_ >= lo && samples_since_beat_ <= hi)
                {
                    beat_period_ = samples_since_beat_;
                }
                beats_ += 1;
            }
            have_beat_ = true;
            samples_since_beat_ = 0;
            beat_blink_ = 12;
        }
        pulse_count_++;
    }
}

FASTCODE void AppBrkRepeat::AudioLoop(q15_t *input, q15_t *output, size_t size)
{
    if (!inited_ || size != KS)
    {
        return;
    }

    for (auto &pot : pots_)
    {
        pot->Process();
    }

    // ---- clock ----
    const int32_t zone = constrain((pots_[Pot::CLOCK]->GetValue() * kClockZones) / (POT_MAX + 1), 0, kClockZones - 1);
    synced_ = zone > 0;
    if (synced_ && kPulsesPerBeat[zone] != pulses_per_beat_)
    {
        pulses_per_beat_ = kPulsesPerBeat[zone];
        pulse_count_ = 0;
        have_beat_ = false;
    }
    ClockTick();
    double bpm = 0, song_pos = 0;
    if (beat_period_ > 0)
    {
        bpm = 60.0 * SAMPLE_RATE / beat_period_;
        double frac = samples_since_beat_ / beat_period_;
        if (frac > 1)
        {
            frac = 1;
        }
        song_pos = (beats_ + frac) * beat_period_ / SAMPLE_RATE;
    }
    engine_.SetTransport(bpm, song_pos);

    // ---- controls ----
    const int32_t time = PotWithCv(Pot::TIME, CV_TIME, true);
    if (synced_)
    {
        // ten note lengths: 1/16, 1/8, 1/8., 1/4, 1/4., 1/2, 1/2., 1 bar, 1.5 bars, 2 bars (halved if too long)
        const int32_t note = constrain((time * 10) / (POT_MAX + 1), 0, 9);
        engine_.SetParam(Engine::TIME, (note + 0.5) / 10.0);
    }
    else
    {
        engine_.SetParam(Engine::TIME, Unit(time) * free_time_max_);
    }
    engine_.SetParam(Engine::SYNC, synced_ ? 1 : 0);
    engine_.SetParam(Engine::REPEATS, 1.0 + 15.0 * Unit(PotWithCv(Pot::REPEATS, CV_REPEATS, true)));
    engine_.SetParam(Engine::POSITION, Unit(PotWithCv(Pot::POSITION, CV_POSITION, false)));
    engine_.SetParam(Engine::BRAEK, Unit(PotWithCv(Pot::BRAEK, CV_BRAEK, false)));
    engine_.SetParam(Engine::WONK, Unit(PotWithCv(Pot::WONK, CV_WONK, false)));
    engine_.SetParam(Engine::DAMAGE, Unit(Kastle2::base.GetLfoMod().AdjustedPotValue(pots_[Pot::DAMAGE].get())));
    engine_.SetParam(Engine::MIX, Unit(Kastle2::base.GetLfoMod().AdjustedPotValue(pots_[Pot::MIX].get())));

    // ---- buttons: MODE held = HOLD, SHIFT + tap MODE = FREEZE toggle ----
    const bool shift = Kastle2::hw.Pressed(Hardware::Button::SHIFT);
    const bool mode = Kastle2::hw.Pressed(Hardware::Button::MODE);
    if (mode && shift)
    {
        mode_used_with_shift_ = true;
    }
    if (!mode && mode_was_pressed_)
    {
        if (mode_used_with_shift_)
        {
            freeze_toggle_ = !freeze_toggle_;
        }
        mode_used_with_shift_ = false;
    }
    mode_was_pressed_ = mode;
    // TRIG and RESET are not used: on the Citadel they are normalled to internal gates when nothing is patched,
    // which switched HOLD on by itself (first hardware test, 2026-11-23).
    // HOLD as on the ER-301: freezes at once while held, releases at once; the engine stays in HOLD mode until the
    // hold-freeze has ended.
    const bool hold_pressed = mode && !mode_used_with_shift_;
    if (hold_pressed)
    {
        hold_engaged_ = true;
    }
    else if (!engine_.Frozen())
    {
        hold_engaged_ = false;
    }
    engine_.SetParam(Engine::HOLD, (hold_pressed || hold_engaged_) ? 1 : 0);
    engine_.SetParam(Engine::FREEZE, freeze_toggle_ ? 1 : 0);
    engine_.SetParam(Engine::FREEZEHOLD, hold_pressed ? 1 : 0);

    // ---- audio ----
    for (int i = 0; i < KS; i++)
    {
        in_l_[i] = input[2 * i];
        in_r_[i] = input[2 * i + 1];
    }
    engine_.ProcessBlock(in_l_, in_r_, out_l_, out_r_);
    for (int i = 0; i < KS; i++)
    {
        output[2 * i] = out_l_[i];
        output[2 * i + 1] = out_r_[i];
    }
    samples_since_beat_ += KS;

    // ---- gate out and LED info ----
    if (engine_.TakeCycleFlag())
    {
        cycle_blink_ = 20; // audio loops (about 22 ms)
    }
    else if (cycle_blink_ > 0)
    {
        cycle_blink_ = cycle_blink_ - 1;
    }
    if (engine_.TakeSliceFlag())
    {
        slice_blink_ = 10;
    }
    else if (slice_blink_ > 0)
    {
        slice_blink_ = slice_blink_ - 1;
    }
    if (beat_blink_ > 0)
    {
        beat_blink_ = beat_blink_ - 1;
    }
    // GATE out: a short pulse at every slice
    Kastle2::hw.SetGateOut(slice_blink_ > 5);
    state_ = hold_pressed ? 3 : (engine_.Frozen() ? 2 : (engine_.Passing() ? 0 : 1));
}

void AppBrkRepeat::UiLoop()
{
    // Read the knobs (Bastl's FancyPot only updates its value here; first hardware test found the knobs dead without it)
    for (auto &pot : pots_)
    {
        pot->ReadValue();
    }

    if (Kastle2::hw.GetLayer() == Hardware::Layer::NORMAL || Kastle2::hw.GetLayer() == Hardware::Layer::MODE)
    {
        uint32_t color;
        switch (state_)
        {
        case 0:
            color = WS2812::DARK_GREEN;
            break;
        case 1:
            color = WS2812::ORANGE;
            break;
        case 2:
            color = WS2812::BLUE;
            break;
        default:
            color = WS2812::RED;
            break;
        }
        if (slice_blink_ == 0)
        {
            color = WS2812::ApplyBrightness(color, 96);
        }
        Kastle2::hw.SetLed(Hardware::Led::LED_1, color);
        // LED 2: white flash at each cycle start, dim green on each beat of the clock; purple while FREE time
        uint32_t c2;
        if (cycle_blink_ > 0)
        {
            c2 = WS2812::WHITE;
        }
        else if (!synced_)
        {
            c2 = WS2812::ApplyBrightness(WS2812::PURPLE, 64);
        }
        else if (beat_blink_ > 0)
        {
            c2 = WS2812::ApplyBrightness(WS2812::GREEN, 64);
        }
        else
        {
            c2 = WS2812::NONE;
        }
        Kastle2::hw.SetLed(Hardware::Led::LED_2, c2);
    }
}
