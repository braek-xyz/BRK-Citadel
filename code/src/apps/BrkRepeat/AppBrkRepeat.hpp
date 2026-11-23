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
        SYNC,     // SHIFT + POT_6: left half = synced to the clock, right half = free time
        COUNT
    };

    // CV inputs (names as on the Wave Bard panel)
    static constexpr Hardware::AnalogInput CV_TIME = Hardware::AnalogInput::PITCH_1;     // PITCH CV
    static constexpr Hardware::AnalogInput CV_REPEATS = Hardware::AnalogInput::PITCH_2;  // NOTE
    static constexpr Hardware::AnalogInput CV_POSITION = Hardware::AnalogInput::PARAM_3; // LENGTH CV
    static constexpr Hardware::AnalogInput CV_BRAEK = Hardware::AnalogInput::PARAM_1;    // SAMPLE CV
    static constexpr Hardware::AnalogInput CV_WONK = Hardware::AnalogInput::MODE;        // BANK CV

    /// Synced cycle lengths in quarter-note beats, Q8 (256 = one beat): 1/32 note .. 2 bars
    static constexpr int32_t kSyncBeatsQ8[] = {32, 64, 128, 256, 512, 1024, 2048};
    static constexpr int32_t kSyncSteps = 7;

    int32_t PotWithCv(Pot pot, Hardware::AnalogInput cv, bool pitch_input);
    int32_t ComputeCycleLength(int32_t time, bool synced, int32_t &beats_q8);

    bool inited_ = false;
    EnumArray<Pot, std::unique_ptr<FancyPot>> pots_;
    brk::RepeatEngine engine_;
    brk::RepeatEngine::Params params_;

    // UI state
    bool freeze_toggle_ = false;
    bool mode_was_pressed_ = false;
    bool mode_used_with_shift_ = false;
    int32_t beat_count_ = 0;
    int32_t cycle_beats_q8_ = 256;
    bool synced_ = true;

    // shown by the UI loop (set in the audio loop)
    volatile uint32_t cycle_blink_ = 0;
    volatile uint32_t slice_blink_ = 0;
    volatile uint8_t state_ = 0;
};

} // namespace kastle2
