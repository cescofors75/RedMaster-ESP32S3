#include "daisy_pod.h"

#include "src/aurora_engine.h"
#include "Effects/chorus.h"
#include "Effects/flanger.h"
#include "Effects/overdrive.h"
#include "Effects/phaser.h"
#include "Filters/ladder.h"
#include "Filters/svf.h"
#include "reverbsc.h"

#include <cmath>
#include <cstdint>

using namespace daisy;
using aurora::AuroraEngine;
using aurora::FxModules;
using aurora::Scene;
using aurora::StereoFrame;
using daisysp::Chorus;
using daisysp::Flanger;
using daisysp::LadderFilter;
using daisysp::Overdrive;
using daisysp::Phaser;
using daisysp::ReverbSc;
using daisysp::Svf;

namespace
{
constexpr float    kSampleRate       = 48000.0f;
constexpr uint32_t kMemorySeconds    = 12;
constexpr uint32_t kMemorySamples    = static_cast<uint32_t>(kSampleRate) * kMemorySeconds;

DaisyPod     hardware;
AuroraEngine engine;

// La SDRAM no se limpia al arrancar: el motor solo lee la parte que ya grabo.
DSY_SDRAM_BSS static float memory_l[kMemorySamples];
DSY_SDRAM_BSS static float memory_r[kMemorySamples];
DSY_SDRAM_BSS static ReverbSc spacious_reverb;
DSY_SDRAM_BSS static Chorus ocean_chorus;
DSY_SDRAM_BSS static Phaser pulse_phaser;
DSY_SDRAM_BSS static Flanger halo_flanger_l;
DSY_SDRAM_BSS static Flanger halo_flanger_r;
static Overdrive ritual_drive_l;
static Overdrive ritual_drive_r;
static LadderFilter color_ladder_l;
static LadderFilter color_ladder_r;
static Svf color_svf_l;
static Svf color_svf_r;

// Mailbox MIDI. El audio consume contadores monotonicamente; asi no se pierde
// un gesto si llega justo entre la lectura y el borrado de un flag.
volatile uint32_t midi_capture_toggle_seq = 0;
volatile uint32_t midi_capture_gate_seq   = 0;
volatile uint8_t  midi_capture_gate       = 0;
volatile uint32_t midi_clear_seq          = 0;
volatile uint32_t midi_bypass_toggle_seq  = 0;
volatile uint32_t midi_bypass_gate_seq    = 0;
volatile uint8_t  midi_bypass_gate        = 0;

volatile uint32_t midi_scene_seq = 0;
volatile uint8_t  midi_scene     = 0;
volatile uint32_t midi_loop_seq  = 0;
volatile uint8_t  midi_loop      = 2;

volatile uint32_t midi_character_seq = 0;
volatile float    midi_character     = 0.5f;
volatile uint32_t midi_intensity_seq = 0;
volatile float    midi_intensity     = 0.35f;
volatile bool     midi_evolve     = false;

volatile uint32_t midi_tempo_seq = 0;
volatile float    midi_tempo     = 120.0f;

float Clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

void ShowStartupAnimation()
{
    constexpr float colors[4][3]
        = {{0.0f, 0.55f, 1.0f},
           {1.0f, 0.05f, 0.45f},
           {1.0f, 0.55f, 0.0f},
           {0.55f, 0.0f, 1.0f}};

    for(uint32_t i = 0; i < 4; ++i)
    {
        hardware.led1.Set(colors[i][0], colors[i][1], colors[i][2]);
        hardware.led2.Set(colors[(i + 1) % 4][0],
                          colors[(i + 1) % 4][1],
                          colors[(i + 1) % 4][2]);
        hardware.UpdateLeds();
        hardware.DelayMs(80);
    }

    // Firma visual v4.1 estable: dos pulsos blancos largos.
    for(uint32_t i = 0; i < 2; ++i)
    {
        hardware.led1.Set(0.72f, 0.72f, 0.72f);
        hardware.led2.Set(0.72f, 0.72f, 0.72f);
        hardware.UpdateLeds();
        hardware.DelayMs(220);
        hardware.ClearLeds();
        hardware.UpdateLeds();
        hardware.DelayMs(100);
    }
    hardware.ClearLeds();
    hardware.UpdateLeds();
}

void ApplyMidiMailbox(float knob_character, float knob_intensity)
{
    static uint32_t capture_toggle_seen = 0;
    static uint32_t capture_gate_seen   = 0;
    static uint32_t clear_seen          = 0;
    static uint32_t bypass_toggle_seen  = 0;
    static uint32_t bypass_gate_seen    = 0;
    static uint32_t scene_seen          = 0;
    static uint32_t loop_seen           = 0;
    static uint32_t character_seen      = 0;
    static uint32_t intensity_seen      = 0;
    static uint32_t tempo_seen          = 0;

    static bool  character_owned_by_midi = false;
    static bool  intensity_owned_by_midi = false;
    static float character_anchor        = 0.0f;
    static float intensity_anchor        = 0.0f;

    const uint32_t capture_toggle_now = midi_capture_toggle_seq;
    if(capture_toggle_now != capture_toggle_seen)
    {
        if(((capture_toggle_now - capture_toggle_seen) & 1u) != 0u)
            engine.ToggleCapture();
        capture_toggle_seen = capture_toggle_now;
    }

    if(midi_capture_gate_seq != capture_gate_seen)
    {
        capture_gate_seen = midi_capture_gate_seq;
        engine.SetCapture(midi_capture_gate != 0);
    }

    if(midi_clear_seq != clear_seen)
    {
        clear_seen = midi_clear_seq;
        engine.ClearHistory();
    }

    const uint32_t bypass_toggle_now = midi_bypass_toggle_seq;
    if(bypass_toggle_now != bypass_toggle_seen)
    {
        if(((bypass_toggle_now - bypass_toggle_seen) & 1u) != 0u)
            engine.ToggleBypass();
        bypass_toggle_seen = bypass_toggle_now;
    }

    if(midi_bypass_gate_seq != bypass_gate_seen)
    {
        bypass_gate_seen = midi_bypass_gate_seq;
        engine.SetBypass(midi_bypass_gate != 0);
    }

    if(midi_scene_seq != scene_seen)
    {
        scene_seen = midi_scene_seq;
        engine.SetSceneIndex(midi_scene);
    }

    if(midi_loop_seq != loop_seen)
    {
        loop_seen = midi_loop_seq;
        engine.SetLoopLengthIndex(midi_loop);
    }

    if(midi_character_seq != character_seen)
    {
        character_seen          = midi_character_seq;
        character_owned_by_midi = true;
        character_anchor        = knob_character;
        engine.SetCharacter(midi_character);
    }

    if(midi_intensity_seq != intensity_seen)
    {
        intensity_seen          = midi_intensity_seq;
        intensity_owned_by_midi = true;
        intensity_anchor        = knob_intensity;
        engine.SetIntensity(midi_intensity);
    }

    // Soft takeover: el CC conserva el control hasta mover fisicamente el knob.
    if(character_owned_by_midi)
    {
        if(fabsf(knob_character - character_anchor) > 0.025f)
            character_owned_by_midi = false;
    }
    if(!character_owned_by_midi)
        engine.SetCharacter(knob_character);

    if(intensity_owned_by_midi)
    {
        if(fabsf(knob_intensity - intensity_anchor) > 0.025f)
            intensity_owned_by_midi = false;
    }
    if(!intensity_owned_by_midi)
        engine.SetIntensity(knob_intensity);

    if(midi_tempo_seq != tempo_seen)
    {
        tempo_seen = midi_tempo_seq;
        engine.SetTempo(midi_tempo);
    }
}

void UpdateControls()
{
    hardware.ProcessAllControls();

    const float knob_character = hardware.GetKnobValue(DaisyPod::KNOB_1);
    const float knob_intensity = hardware.GetKnobValue(DaisyPod::KNOB_2);
    ApplyMidiMailbox(knob_character, knob_intensity);

    const int encoder_increment = hardware.encoder.Increment();
    if(encoder_increment != 0)
        engine.StepScene(encoder_increment);

    if(hardware.button1.RisingEdge())
        engine.ToggleCapture();

    if(hardware.button2.RisingEdge() && !engine.IsCaptured())
        engine.TapTempo();

    engine.SetEvolve((engine.IsCaptured() && hardware.button2.Pressed())
                     || midi_evolve);

    // Un clic, una funcion: bypass. La longitud queda fija a 4 beats.
    if(hardware.encoder.FallingEdge())
        engine.ToggleBypass();
}

void UpdateLeds()
{
    constexpr float scene_colors[4][3]
        = {{0.0f, 0.55f, 1.0f},
           {1.0f, 0.04f, 0.40f},
           {1.0f, 0.52f, 0.0f},
           {0.52f, 0.0f, 1.0f}};

    if(engine.IsBypassed())
    {
        hardware.led1.Set(0.18f, 0.0f, 0.0f);
        hardware.led2.Set(0.18f, 0.0f, 0.0f);
        hardware.UpdateLeds();
        return;
    }

    const uint8_t scene = static_cast<uint8_t>(engine.RequestedScene());
    const float beat_pulse = 0.82f + 0.18f * (1.0f - engine.BeatPhase());
    const float filter_level = 0.22f + 0.78f * engine.Character();
    hardware.led1.Set(scene_colors[scene][0] * beat_pulse * filter_level,
                      scene_colors[scene][1] * beat_pulse * filter_level,
                      scene_colors[scene][2] * beat_pulse * filter_level);

    if(engine.IsEvolving())
    {
        hardware.led2.Set(0.75f, 0.75f, 0.75f);
    }
    else if(engine.IsCaptured())
    {
        const float amount_level = 0.18f + 0.72f * engine.Intensity();
        hardware.led2.Set(amount_level, amount_level * 0.24f, 0.0f);
    }
    else
    {
        const float meter = Clamp01(engine.InputLevel() * 1.8f);
        const float amount_level = 0.04f + 0.78f * engine.Intensity();
        hardware.led2.Set(meter * 0.18f, amount_level, meter * 0.05f);
    }

    hardware.UpdateLeds();
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    UpdateControls();

    for(size_t i = 0; i < size; ++i)
    {
        const StereoFrame frame = engine.Process(in[0][i], in[1][i]);
        out[0][i] = frame.l;
        out[1][i] = frame.r;
    }

    static uint8_t led_divider = 0;
    if(++led_divider >= 4)
    {
        led_divider = 0;
        UpdateLeds();
    }
}

void HandleMidiEvent(MidiEvent& event)
{
    switch(event.type)
    {
        case NoteOn:
        {
            const NoteOnEvent note = event.AsNoteOn();
            if(note.velocity == 0)
            {
                if(note.note == 62)
                    midi_evolve = false;
                break;
            }
            if(note.note == 60)
                ++midi_capture_toggle_seq;
            else if(note.note == 62)
                midi_evolve = true;
            else if(note.note == 64)
                ++midi_clear_seq;
            break;
        }

        case NoteOff:
        {
            const NoteOffEvent note = event.AsNoteOff();
            if(note.note == 62)
                midi_evolve = false;
            break;
        }

        case ControlChange:
        {
            const ControlChangeEvent cc = event.AsControlChange();
            const float normalized = cc.value / 127.0f;
            switch(cc.control_number)
            {
                case 20:
                    midi_character = normalized;
                    ++midi_character_seq;
                    break;
                case 21:
                    midi_intensity = normalized;
                    ++midi_intensity_seq;
                    break;
                case 22:
                    midi_scene = static_cast<uint8_t>(normalized * 3.999f);
                    ++midi_scene_seq;
                    break;
                case 23:
                    midi_loop = static_cast<uint8_t>(normalized * 3.999f);
                    ++midi_loop_seq;
                    break;
                case 64:
                    midi_capture_gate = cc.value >= 64 ? 1 : 0;
                    ++midi_capture_gate_seq;
                    break;
                case 65: midi_evolve = cc.value >= 64; break;
                case 66:
                    midi_bypass_gate = cc.value >= 64 ? 1 : 0;
                    ++midi_bypass_gate_seq;
                    break;
                default: break;
            }
            break;
        }

        case ProgramChange:
        {
            const ProgramChangeEvent program = event.AsProgramChange();
            midi_scene = program.program % AuroraEngine::kSceneCount;
            ++midi_scene_seq;
            break;
        }

        case SystemRealTime:
        {
            static uint32_t last_clock_ms = 0;
            static float    clock_interval_ms = 0.0f;

            if(event.srt_type == TimingClock)
            {
                const uint32_t now = System::GetNow();
                if(last_clock_ms != 0)
                {
                    const uint32_t elapsed = now - last_clock_ms;
                    if(elapsed >= 5 && elapsed <= 100)
                    {
                        if(clock_interval_ms == 0.0f)
                            clock_interval_ms = elapsed;
                        else
                            clock_interval_ms += 0.12f
                                                 * (elapsed - clock_interval_ms);
                        midi_tempo = 60000.0f / (24.0f * clock_interval_ms);
                        ++midi_tempo_seq;
                    }
                }
                last_clock_ms = now;
            }
            break;
        }

        default: break;
    }
}

} // namespace

int main(void)
{
    hardware.Init(true);
    hardware.SetAudioBlockSize(48);
    ShowStartupAnimation();

    FxModules fx = {};
    fx.reverb   = &spacious_reverb;
    fx.chorus   = &ocean_chorus;
    fx.phaser   = &pulse_phaser;
    fx.flanger_l = &halo_flanger_l;
    fx.flanger_r = &halo_flanger_r;
    fx.drive_l  = &ritual_drive_l;
    fx.drive_r  = &ritual_drive_r;
    fx.ladder_l = &color_ladder_l;
    fx.ladder_r = &color_ladder_r;
    fx.svf_l    = &color_svf_l;
    fx.svf_r    = &color_svf_r;

    engine.Init(hardware.AudioSampleRate(),
                memory_l,
                memory_r,
                kMemorySamples,
                fx);

    hardware.StartAdc();
    hardware.midi.StartReceive();
    hardware.StartAudio(AudioCallback);

    while(true)
    {
        hardware.midi.Listen();
        while(hardware.midi.HasEvents())
        {
            MidiEvent event = hardware.midi.PopEvent();
            HandleMidiEvent(event);
        }
        hardware.DelayMs(1);
    }
}
