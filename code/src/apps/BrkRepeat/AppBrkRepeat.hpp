/*
 * BRK Repeat: beat repeater / micro sound repeater app for the Bastl Citadel (and Kastle 2).
 * Copyright (c) 2026 BRAEK. MIT License (see LICENSE in the repository root).
 * Built on Bastl Instruments' Kastle 2 framework (MIT).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "common/core/App.hpp"
#include "common/core/Hardware.hpp"
#include "common/core/Kastle2.hpp"
#include "common/controls/FancyPot.hpp"
#include "common/dsp/math/qmath.hpp"
#include "common/EnumTools.hpp"
#include "common/fastcode.hpp"
// the engine's per-block code runs from RAM (the Pico SDK puts .time_critical code in RAM)
#define BRK_FAST __attribute__((section(".time_critical.brk_repeat"))) __attribute__((noinline))
#include "apps/BrkRepeat/RepeatEngine.hpp"

namespace kastle2
{

class AppBrkRepeat : public virtual App
{
public:
    void Init();
    void DeInit();
    FASTCODE void AudioLoop(q15_t *input, q15_t *output, size_t size);
    void UiLoop();
    void MemoryInitialization() {}
    void MidiCallback([[maybe_unused]] midi::Message *msg) {}

    /// Community app ID (see APP_LIST.md): 0x42 = BRK Repeat
    uint8_t GetId() { return 0x42; }

private:
    enum class Pot
    {
        TIME,     // POT_5 (Wave Bard: PITCH)
        REPEATS,  // POT_6 (SAMPLE)
        POSITION, // POT_4 (LENGTH)
        BRAEK,    // POT_1 (PITCH MOD)
        WONK,     // POT_2 (SAMPLE MOD)
        DAMAGE,   // SHIFT + POT_2
        MIX,      // SHIFT + POT_4
        CLOCK,    // SHIFT + POT_6: FREE | 1 | 2 | 4 | 24 clock pulses per beat
        COUNT
    };

    // CV inputs (names as on the Wave Bard panel)
    static constexpr Hardware::AnalogInput CV_TIME = Hardware::AnalogInput::PITCH_1;     // PITCH CV
    static constexpr Hardware::AnalogInput CV_REPEATS = Hardware::AnalogInput::PITCH_2;  // NOTE
    static constexpr Hardware::AnalogInput CV_POSITION = Hardware::AnalogInput::PARAM_3; // LENGTH CV
    static constexpr Hardware::AnalogInput CV_BRAEK = Hardware::AnalogInput::PARAM_1;    // SAMPLE CV
    static constexpr Hardware::AnalogInput CV_WONK = Hardware::AnalogInput::MODE;        // BANK CV

    /// SHIFT + SAMPLE zones: 0 = FREE time, then clock pulses per beat
    static constexpr int32_t kClockZones = 5;
    static constexpr int32_t kPulsesPerBeat[kClockZones] = {0, 1, 2, 4, 24};

    using Engine = brk::RepeatEngine;
    static constexpr int KS = 48;
    static_assert(AUDIO_BUFFER_SIZE == KS, "the engine runs one block per audio loop");

    int32_t PotWithCv(Pot pot, Hardware::AnalogInput cv, bool pitch_input);
    static double Unit(int32_t v) { return static_cast<double>(v) / static_cast<double>(POT_MAX); }
    void ClockTick();

    bool inited_ = false;
    EnumArray<Pot, std::unique_ptr<FancyPot>> pots_;
    Engine engine_;
    double free_time_max_ = 1.0; ///< TIME value (engine units) of the longest free cycle

    // buttons
    bool freeze_toggle_ = false;
    bool mode_was_pressed_ = false;
    bool mode_used_with_shift_ = false;
    bool hold_engaged_ = false;

    // clock -> tempo and song position (as the ER-301 unit does it, in whole audio blocks)
    int32_t pulses_per_beat_ = 1;
    uint32_t pulse_count_ = 0;
    bool have_beat_ = false;
    double beats_ = 0;
    double samples_since_beat_ = 0;
    double beat_period_ = 0; ///< samples per beat, 0 = not known yet

    // deinterleaved audio
    int32_t in_l_[KS], in_r_[KS], out_l_[KS], out_r_[KS];

    // shown by the UI loop (set in the audio loop)
    volatile uint32_t cycle_blink_ = 0;
    volatile uint32_t slice_blink_ = 0;
    volatile uint32_t beat_blink_ = 0;
    volatile uint8_t state_ = 0; // 0 live, 1 repeating, 2 frozen, 3 hold
    volatile bool synced_ = true;
};

} // namespace kastle2
