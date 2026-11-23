/*
 * BRK Repeat main. Copyright (c) 2026 BRAEK. MIT License.
 * Same structure as Bastl's Template app (MIT).
 */
#include "common/core/Kastle2.hpp"
#include "apps/BrkRepeat/AppBrkRepeat.hpp"

using namespace kastle2;

AppBrkRepeat app;

static void process_audio(q15_t *input, q15_t *output, size_t size)
{
    app.AudioLoop(input, output, size);
}

static void midi_callback(midi::Message *msg)
{
    app.MidiCallback(msg);
}

int main()
{
    Kastle2::Init();
    Kastle2::RegisterApp(&app);
    app.Init();
    Kastle2::StartAudio(process_audio);
    Kastle2::SetAppMidiCallback(midi_callback);
    while (true)
    {
        Kastle2::ReadInputs();
        app.UiLoop();
    }
    return 0;
}
