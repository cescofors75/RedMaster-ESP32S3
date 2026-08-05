#include "aurora_engine.h"
#include "Effects/chorus.h"
#include "Effects/flanger.h"
#include "Effects/overdrive.h"
#include "Effects/phaser.h"
#include "Filters/ladder.h"
#include "Filters/svf.h"
#include "reverbsc.h"

#include <cmath>

namespace aurora
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;

constexpr float kLoopBeats[AuroraEngine::kLoopLengthCount]
    = {1.0f, 2.0f, 4.0f, 8.0f};

} // namespace

float AuroraEngine::Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

float AuroraEngine::Sanitize(float value)
{
    // Evita que un NaN/Inf de una cola DSP contamine el resto de la cadena.
    return value == value && fabsf(value) < 16.0f ? value : 0.0f;
}

float AuroraEngine::OutputLimit(float value)
{
    const float magnitude = fabsf(value);
    if(magnitude <= 0.90f)
        return value;

    // Conserva intacta la zona musical y comprime progresivamente solo picos.
    const float excess = magnitude - 0.90f;
    const float limited = 0.90f + 0.10f * excess / (0.10f + excess);
    return value < 0.0f ? -limited : limited;
}

float AuroraEngine::EqualPowerWindow(float phase)
{
    const float sine = sinf(kPi * Clamp(phase, 0.0f, 1.0f));
    return sine * sine;
}

void AuroraEngine::Init(float sample_rate,
                        float* memory_l,
                        float* memory_r,
                        uint32_t memory_samples,
                        const FxModules& fx)
{
    sample_rate_    = sample_rate;
    memory_l_       = memory_l;
    memory_r_       = memory_r;
    memory_samples_ = memory_samples;
    fx_             = fx;

    write_position_   = 0;
    recorded_samples_ = 0;
    loop_start_       = 0;
    loop_length_      = 0;
    captured_         = false;
    playback_active_  = false;
    capture_gain_     = 0.0f;
    bypass_mix_       = 1.0f;
    sample_counter_   = 0;
    beat_phase_       = 0.0f;

    if(fx_.reverb != nullptr)
    {
        fx_.reverb->Init(sample_rate_);
    }
    if(fx_.chorus != nullptr)
    {
        fx_.chorus->Init(sample_rate_);
        fx_.chorus->SetPan(0.08f, 0.92f);
    }
    if(fx_.phaser != nullptr)
    {
        fx_.phaser->Init(sample_rate_);
        fx_.phaser->SetPoles(4);
    }
    if(fx_.flanger_l != nullptr)
    {
        fx_.flanger_l->Init(sample_rate_);
    }
    if(fx_.flanger_r != nullptr)
    {
        fx_.flanger_r->Init(sample_rate_);
    }
    if(fx_.drive_l != nullptr)
        fx_.drive_l->Init();
    if(fx_.drive_r != nullptr)
        fx_.drive_r->Init();
    if(fx_.ladder_l != nullptr)
    {
        fx_.ladder_l->Init(sample_rate_);
        fx_.ladder_l->SetFilterMode(daisysp::LadderFilter::FilterMode::LP24);
    }
    if(fx_.ladder_r != nullptr)
    {
        fx_.ladder_r->Init(sample_rate_);
        fx_.ladder_r->SetFilterMode(daisysp::LadderFilter::FilterMode::LP24);
    }
    if(fx_.svf_l != nullptr)
        fx_.svf_l->Init(sample_rate_);
    if(fx_.svf_r != nullptr)
        fx_.svf_r->Init(sample_rate_);

    UpdateFxParameters();

    grains_[0].age = 0.0f;
    grains_[1].age = 0.5f;
    ResetGrain(grains_[0], true);
    ResetGrain(grains_[1], true);
}

bool AuroraEngine::HasUsableHistory() const
{
    return recorded_samples_ >= kMinLoopSamples;
}

float AuroraEngine::CapturedSeconds() const
{
    return sample_rate_ > 0.0f ? loop_length_ / sample_rate_ : 0.0f;
}

void AuroraEngine::SetCapture(bool enabled)
{
    if(enabled == captured_)
        return;

    if(enabled)
        CaptureNow();
    else
        captured_ = false;
}

void AuroraEngine::ToggleCapture()
{
    SetCapture(!captured_);
}

void AuroraEngine::CaptureNow()
{
    if(!HasUsableHistory() || memory_samples_ == 0)
        return;

    const float beats       = kLoopBeats[loop_length_index_];
    const float beat_samples = sample_rate_ * 60.0f / tempo_bpm_;
    uint32_t desired
        = static_cast<uint32_t>(beat_samples * beats + 0.5f);
    if(desired > memory_samples_)
        desired = memory_samples_;
    if(desired > recorded_samples_)
        desired = recorded_samples_;
    if(desired < kMinLoopSamples)
        return;

    loop_length_ = desired;
    loop_start_  = (write_position_ + memory_samples_ - loop_length_)
                  % memory_samples_;
    overdub_position_ = 0.0f;
    voice_phase_a_    = 0.0f;
    voice_phase_b_    = 0.0f;
    ResetGrain(grains_[0], true);
    grains_[0].age = 0.0f;
    ResetGrain(grains_[1], true);
    grains_[1].age = 0.5f;

    captured_        = true;
    playback_active_ = true;
}

void AuroraEngine::ClearHistory()
{
    captured_         = false;
    recorded_samples_ = 0;
    write_position_   = 0;
    tap_armed_        = false;
}

void AuroraEngine::SetScene(Scene scene)
{
    requested_scene_ = scene;
    if(requested_scene_ != active_scene_)
        scene_fading_out_ = true;
}

void AuroraEngine::SetSceneIndex(uint8_t index)
{
    SetScene(static_cast<Scene>(index % kSceneCount));
}

void AuroraEngine::StepScene(int increment)
{
    int next = static_cast<int>(requested_scene_) + increment;
    while(next < 0)
        next += kSceneCount;
    next %= kSceneCount;
    SetSceneIndex(static_cast<uint8_t>(next));
}

void AuroraEngine::SetLoopLengthIndex(uint8_t index)
{
    loop_length_index_ = index % kLoopLengthCount;
}

void AuroraEngine::StepLoopLength(int increment)
{
    int next = static_cast<int>(loop_length_index_) + increment;
    while(next < 0)
        next += kLoopLengthCount;
    loop_length_index_ = static_cast<uint8_t>(next % kLoopLengthCount);
}

void AuroraEngine::SetCharacter(float normalized)
{
    character_target_ = Clamp(normalized, 0.0f, 1.0f);
}

void AuroraEngine::SetIntensity(float normalized)
{
    intensity_target_ = Clamp(normalized, 0.0f, 1.0f);
}

void AuroraEngine::SetEvolve(bool enabled)
{
    evolve_ = enabled;
}

void AuroraEngine::SetBypass(bool enabled)
{
    bypassed_ = enabled;
}

void AuroraEngine::ToggleBypass()
{
    bypassed_ = !bypassed_;
}

void AuroraEngine::SetTempo(float bpm)
{
    tempo_bpm_ = Clamp(bpm, 40.0f, 240.0f);
}

void AuroraEngine::TapTempo()
{
    if(tap_armed_)
    {
        const uint64_t elapsed = sample_counter_ - last_tap_sample_;
        const float min_samples = sample_rate_ * 0.25f; // 240 BPM
        const float max_samples = sample_rate_ * 1.50f; // 40 BPM
        if(elapsed >= min_samples && elapsed <= max_samples)
        {
            const float tapped = 60.0f * sample_rate_ / elapsed;
            // Un poco de inercia evita que un tap imperfecto mueva todo el loop.
            SetTempo(tempo_bpm_ * 0.25f + tapped * 0.75f);
        }
    }

    last_tap_sample_ = sample_counter_;
    tap_armed_       = true;
    beat_phase_      = 0.0f;
}

void AuroraEngine::Record(float input_l, float input_r)
{
    memory_l_[write_position_] = input_l;
    memory_r_[write_position_] = input_r;
    write_position_ = (write_position_ + 1u) % memory_samples_;
    if(recorded_samples_ < memory_samples_)
        ++recorded_samples_;
}

void AuroraEngine::Overdub(float input_l, float input_r)
{
    if(!captured_ || !evolve_ || loop_length_ == 0)
        return;

    const uint32_t local = static_cast<uint32_t>(overdub_position_);
    const uint32_t index = (loop_start_ + local) % memory_samples_;

    // Repaint de ganancia constante: evoluciona sin acumular nivel ni saturar.
    memory_l_[index] = memory_l_[index] * 0.90f + input_l * 0.10f;
    memory_r_[index] = memory_r_[index] * 0.90f + input_r * 0.10f;

    overdub_position_ += 1.0f;
    if(overdub_position_ >= loop_length_)
        overdub_position_ -= loop_length_;
}

float AuroraEngine::WrapLocal(float position) const
{
    if(loop_length_ == 0)
        return 0.0f;
    while(position < 0.0f)
        position += loop_length_;
    while(position >= loop_length_)
        position -= loop_length_;
    return position;
}

StereoFrame AuroraEngine::ReadLocal(float position) const
{
    position = WrapLocal(position);
    const uint32_t base = static_cast<uint32_t>(position);
    const float fraction = position - base;

    const uint32_t i0 = (loop_start_ + base) % memory_samples_;
    const uint32_t i1 = (i0 + 1u) % memory_samples_;

    StereoFrame frame;
    frame.l = memory_l_[i0] + (memory_l_[i1] - memory_l_[i0]) * fraction;
    frame.r = memory_r_[i0] + (memory_r_[i1] - memory_r_[i0]) * fraction;
    return frame;
}

StereoFrame AuroraEngine::ReadSeamless(float phase, float rate) const
{
    if(loop_length_ < kMinLoopSamples)
        return {0.0f, 0.0f};

    const float crossfade = Clamp(loop_length_ * 0.0125f, 96.0f, 512.0f);
    const float travel    = loop_length_ - crossfade;
    StereoFrame first;
    StereoFrame second;

    if(rate >= 0.0f)
    {
        const float position = crossfade + phase * travel;
        first = ReadLocal(position);
        if(position < loop_length_ - crossfade)
            return first;

        const float blend
            = (position - (loop_length_ - crossfade)) / crossfade;
        second = ReadLocal(position - (loop_length_ - crossfade));
        return {first.l + (second.l - first.l) * blend,
                first.r + (second.r - first.r) * blend};
    }

    const float position = (loop_length_ - crossfade) - phase * travel;
    first = ReadLocal(position);
    if(position >= crossfade)
        return first;

    const float blend = (crossfade - position) / crossfade;
    second = ReadLocal((loop_length_ - crossfade) + position);
    return {first.l + (second.l - first.l) * blend,
            first.r + (second.r - first.r) * blend};
}

void AuroraEngine::AdvancePhase(float& phase, float rate) const
{
    const float crossfade = Clamp(loop_length_ * 0.0125f, 96.0f, 512.0f);
    const float travel    = loop_length_ - crossfade;
    if(travel <= 1.0f)
        return;

    phase += fabsf(rate) / travel;
    while(phase >= 1.0f)
        phase -= 1.0f;
}

uint32_t AuroraEngine::NextRandom()
{
    random_state_ = random_state_ * 1664525u + 1013904223u;
    return random_state_;
}

float AuroraEngine::RandomUnit()
{
    return static_cast<float>((NextRandom() >> 8) & 0x00FFFFFFu)
           / 16777216.0f;
}

void AuroraEngine::ResetGrain(Grain& grain, bool initial)
{
    grain.start = RandomUnit() * (loop_length_ > 0 ? loop_length_ : 1.0f);
    const float choices[7] = {-1.0f, 0.5f, 0.75f, 1.0f, 1.0f, 1.0f, 1.25f};
    const uint32_t choice = NextRandom() % 7u;
    grain.rate = choices[choice];
    grain.pan  = RandomUnit() * 2.0f - 1.0f;
    if(!initial)
        grain.age = 0.0f;
}

StereoFrame AuroraEngine::ProcessDust()
{
    StereoFrame output = {0.0f, 0.0f};
    const float grain_samples = sample_rate_ * 0.085f;
    const float scatter       = 0.48f;

    for(uint32_t i = 0; i < 2; ++i)
    {
        Grain& grain = grains_[i];
        const float window = EqualPowerWindow(grain.age);
        const float position
            = grain.start + grain.rate * grain.age * grain_samples;
        const StereoFrame source = ReadLocal(position);

        const float pan = grain.pan * scatter;
        const float left_gain  = 0.5f * (1.0f - pan);
        const float right_gain = 0.5f * (1.0f + pan);
        const float mono = 0.5f * (source.l + source.r);
        output.l += window * (source.l * 0.55f + mono * left_gain * 0.9f);
        output.r += window * (source.r * 0.55f + mono * right_gain * 0.9f);

        grain.age += 1.0f / grain_samples;
        if(grain.age >= 1.0f)
        {
            grain.age -= 1.0f;
            ResetGrain(grain, true);
        }
    }

    return output;
}

StereoFrame AuroraEngine::ProcessMemory()
{
    if(!playback_active_ || loop_length_ < kMinLoopSamples)
        return {0.0f, 0.0f};

    if(active_scene_ == Scene::Ritual)
    {
        const StereoFrame stable = ReadSeamless(voice_phase_a_, 1.0f);
        AdvancePhase(voice_phase_a_, 1.0f);
        const StereoFrame grains = ProcessDust();
        const float blend = 0.62f;
        return {stable.l + (grains.l - stable.l) * blend,
                stable.r + (grains.r - stable.r) * blend};
    }

    switch(active_scene_)
    {
        case Scene::Ocean:
        {
            // Repeticion estable: el usuario ya no tiene que buscar 1x.
            const StereoFrame stable = ReadSeamless(voice_phase_a_, 1.0f);
            AdvancePhase(voice_phase_a_, 1.0f);
            return stable;
        }

        case Scene::Pulse:
        {
            const StereoFrame forward = ReadSeamless(voice_phase_a_, 1.0f);
            const StereoFrame reverse = ReadSeamless(voice_phase_b_, -1.0f);
            AdvancePhase(voice_phase_a_, 1.0f);
            AdvancePhase(voice_phase_b_, -1.0f);
            const float blend = 0.55f;
            return {forward.l + (reverse.l - forward.l) * blend,
                    forward.r + (reverse.r - forward.r) * blend};
        }

        case Scene::Halo:
        {
            // Halo aplica el flanger a la cadena live completa; la memoria se
            // mantiene estable antes de entrar en ese módulo.
            const StereoFrame fundamental = ReadSeamless(voice_phase_a_, 1.0f);
            AdvancePhase(voice_phase_a_, 1.0f);
            return fundamental;
        }

        case Scene::Ritual: return {0.0f, 0.0f};
    }

    return {0.0f, 0.0f};
}

StereoFrame AuroraEngine::ProcessReverb(float input_l, float input_r)
{
    if(fx_.reverb == nullptr)
        return {0.0f, 0.0f};

    StereoFrame output = {0.0f, 0.0f};
    fx_.reverb->Process(input_l, input_r, &output.l, &output.r);
    output.l = Sanitize(output.l);
    output.r = Sanitize(output.r);
    return output;
}

void AuroraEngine::UpdateFxParameters()
{
    const float c = character_;
    const float i = intensity_;

    switch(active_scene_)
    {
        case Scene::Ocean: // Chorus y LadderFilter oficiales de DaisySP.
        {
            if(fx_.chorus != nullptr)
            {
                fx_.chorus->SetLfoDepth(0.22f + c * 0.68f,
                                        0.28f + c * 0.62f);
                fx_.chorus->SetLfoFreq(0.07f + c * 0.24f,
                                       0.09f + c * 0.29f);
                fx_.chorus->SetDelayMs(7.0f + c * 13.0f,
                                       10.0f + c * 15.0f);
                fx_.chorus->SetFeedback(0.08f + c * 0.28f);
            }
            const float cutoff = 240.0f * powf(70.0f, c);
            const float resonance = 0.18f + (1.0f - c) * 0.62f;
            if(fx_.ladder_l != nullptr)
            {
                fx_.ladder_l->SetFreq(cutoff);
                fx_.ladder_l->SetRes(resonance);
                fx_.ladder_l->SetInputDrive(1.0f + i * 0.65f);
                fx_.ladder_l->SetPassbandGain(0.30f);
            }
            if(fx_.ladder_r != nullptr)
            {
                fx_.ladder_r->SetFreq(cutoff * 1.015f);
                fx_.ladder_r->SetRes(resonance);
                fx_.ladder_r->SetInputDrive(1.0f + i * 0.65f);
                fx_.ladder_r->SetPassbandGain(0.30f);
            }
            break;
        }

        case Scene::Pulse: // Pulse: phaser profundo y pico SVF movil.
        {
            const float center = 140.0f * powf(25.0f, c);
            if(fx_.phaser != nullptr)
            {
                fx_.phaser->SetFreq(center);
                fx_.phaser->SetLfoFreq(0.08f + c * 0.48f);
                fx_.phaser->SetLfoDepth(0.40f + i * 0.42f);
                fx_.phaser->SetFeedback(0.20f + c * 0.42f);
            }
            const float filter_center = 220.0f * powf(35.0f, c);
            if(fx_.svf_l != nullptr)
            {
                fx_.svf_l->SetFreq(filter_center);
                fx_.svf_l->SetRes(0.30f + i * 0.55f);
                fx_.svf_l->SetDrive(0.18f + i * 0.42f);
            }
            if(fx_.svf_r != nullptr)
            {
                fx_.svf_r->SetFreq(filter_center * 1.025f);
                fx_.svf_r->SetRes(0.30f + i * 0.55f);
                fx_.svf_r->SetDrive(0.18f + i * 0.42f);
            }
            break;
        }

        case Scene::Halo: // Flanger estéreo y filtro de aire.
        {
            if(fx_.flanger_l != nullptr)
            {
                fx_.flanger_l->SetLfoFreq(0.07f + c * 0.72f);
                fx_.flanger_l->SetLfoDepth(0.25f + i * 0.58f);
                fx_.flanger_l->SetFeedback(0.10f + c * 0.48f);
                fx_.flanger_l->SetDelay(0.22f + c * 0.52f);
            }
            if(fx_.flanger_r != nullptr)
            {
                fx_.flanger_r->SetLfoFreq(0.09f + c * 0.77f);
                fx_.flanger_r->SetLfoDepth(0.28f + i * 0.55f);
                fx_.flanger_r->SetFeedback(0.12f + c * 0.46f);
                fx_.flanger_r->SetDelay(0.25f + c * 0.48f);
            }
            const float air_cutoff = 260.0f * powf(12.0f, c);
            if(fx_.svf_l != nullptr)
            {
                fx_.svf_l->SetFreq(air_cutoff);
                fx_.svf_l->SetRes(0.18f + i * 0.28f);
                fx_.svf_l->SetDrive(0.10f);
            }
            if(fx_.svf_r != nullptr)
            {
                fx_.svf_r->SetFreq(air_cutoff * 1.03f);
                fx_.svf_r->SetRes(0.18f + i * 0.28f);
                fx_.svf_r->SetDrive(0.10f);
            }
            break;
        }

        case Scene::Ritual: // Ritual: drive compensado y ladder caliente.
        {
            if(fx_.drive_l != nullptr)
                fx_.drive_l->SetDrive(0.08f + c * 0.76f);
            if(fx_.drive_r != nullptr)
                fx_.drive_r->SetDrive(0.08f + c * 0.76f);
            const float cutoff = 420.0f * powf(24.0f, c);
            const float resonance = 0.28f + (1.0f - c) * 0.52f;
            if(fx_.ladder_l != nullptr)
            {
                fx_.ladder_l->SetFreq(cutoff);
                fx_.ladder_l->SetRes(resonance);
                fx_.ladder_l->SetInputDrive(1.0f + c * 2.0f);
                fx_.ladder_l->SetPassbandGain(0.24f);
            }
            if(fx_.ladder_r != nullptr)
            {
                fx_.ladder_r->SetFreq(cutoff * 0.985f);
                fx_.ladder_r->SetRes(resonance);
                fx_.ladder_r->SetInputDrive(1.0f + c * 2.0f);
                fx_.ladder_r->SetPassbandGain(0.24f);
            }
            break;
        }
    }

    if(fx_.reverb != nullptr)
    {
        float feedback = 0.78f;
        float lowpass  = 8500.0f;
        if(active_scene_ == Scene::Ocean)
        {
            feedback = 0.70f + i * 0.12f;
            lowpass  = 6500.0f + c * 4500.0f;
        }
        else if(active_scene_ == Scene::Pulse)
        {
            feedback = 0.68f + i * 0.10f;
            lowpass  = 7200.0f;
        }
        else if(active_scene_ == Scene::Halo)
        {
            feedback = 0.76f + i * 0.12f;
            lowpass  = 7800.0f + c * 3500.0f;
        }
        else
        {
            feedback = 0.65f + i * 0.10f;
            lowpass  = 5600.0f + c * 2800.0f;
        }
        fx_.reverb->SetFeedback(feedback);
        fx_.reverb->SetLpFreq(lowpass);
    }
}

StereoFrame AuroraEngine::ProcessLiveFx(float input_l,
                                        float input_r,
                                        const StereoFrame& memory)
{
    if(++fx_parameter_divider_ >= 256)
    {
        fx_parameter_divider_ = 0;
        UpdateFxParameters();
    }

    StereoFrame source = {input_l * 0.72f + memory.l * 0.70f,
                          input_r * 0.72f + memory.r * 0.70f};
    source.l = Sanitize(source.l);
    source.r = Sanitize(source.r);

    switch(active_scene_)
    {
        case Scene::Ocean:
        {
            StereoFrame wet = source;
            if(fx_.chorus != nullptr)
            {
                const float mono = 0.5f * (source.l + source.r);
                fx_.chorus->Process(mono);
                wet.l = Sanitize(fx_.chorus->GetLeft()) * 1.55f
                        + source.l * 0.16f;
                wet.r = Sanitize(fx_.chorus->GetRight()) * 1.55f
                        + source.r * 0.16f;
            }
            if(fx_.ladder_l != nullptr)
                wet.l = Sanitize(fx_.ladder_l->Process(wet.l));
            if(fx_.ladder_r != nullptr)
                wet.r = Sanitize(fx_.ladder_r->Process(wet.r));
            return wet;
        }

        case Scene::Pulse:
        {
            const float mid  = 0.5f * (source.l + source.r);
            const float side = 0.5f * (source.l - source.r);
            float phased = mid;
            if(fx_.phaser != nullptr)
                phased = Sanitize(fx_.phaser->Process(mid) * 0.25f);
            StereoFrame wet = {phased + side * 0.35f,
                               phased - side * 0.35f};
            if(fx_.svf_l != nullptr)
            {
                fx_.svf_l->Process(wet.l);
                wet.l = Sanitize(fx_.svf_l->Notch() * 0.62f
                                 + fx_.svf_l->Band() * 0.54f);
            }
            if(fx_.svf_r != nullptr)
            {
                fx_.svf_r->Process(wet.r);
                wet.r = Sanitize(fx_.svf_r->Notch() * 0.62f
                                 + fx_.svf_r->Band() * 0.54f);
            }
            return wet;
        }

        case Scene::Halo:
        {
            StereoFrame wet = source;
            if(fx_.flanger_l != nullptr)
                wet.l = Sanitize(fx_.flanger_l->Process(source.l));
            if(fx_.flanger_r != nullptr)
                wet.r = Sanitize(fx_.flanger_r->Process(source.r));
            if(fx_.svf_l != nullptr)
            {
                fx_.svf_l->Process(wet.l);
                wet.l = Sanitize(fx_.svf_l->High() * 0.42f
                                 + fx_.svf_l->Notch() * 0.64f);
            }
            if(fx_.svf_r != nullptr)
            {
                fx_.svf_r->Process(wet.r);
                wet.r = Sanitize(fx_.svf_r->High() * 0.42f
                                 + fx_.svf_r->Notch() * 0.64f);
            }
            return wet;
        }

        case Scene::Ritual:
        {
            StereoFrame wet = source;
            if(fx_.drive_l != nullptr)
                wet.l = Sanitize(fx_.drive_l->Process(wet.l));
            if(fx_.drive_r != nullptr)
                wet.r = Sanitize(fx_.drive_r->Process(wet.r));
            if(fx_.ladder_l != nullptr)
                wet.l = Sanitize(fx_.ladder_l->Process(wet.l));
            if(fx_.ladder_r != nullptr)
                wet.r = Sanitize(fx_.ladder_r->Process(wet.r));
            return wet;
        }
    }

    return source;
}

float AuroraEngine::ProcessDcBlock(float input, uint8_t channel)
{
    const float output
        = input - dc_input_[channel] + 0.995f * dc_output_[channel];
    dc_input_[channel]  = input;
    dc_output_[channel] = output;
    return output;
}

StereoFrame AuroraEngine::Process(float input_l, float input_r)
{
    input_l = ProcessDcBlock(input_l, 0);
    input_r = ProcessDcBlock(input_r, 1);

    const float peak = fmaxf(fabsf(input_l), fabsf(input_r));
    const float envelope_rate = peak > input_level_ ? 0.08f : 0.0008f;
    input_level_ += envelope_rate * (peak - input_level_);

    character_ += 0.0015f * (character_target_ - character_);
    intensity_ += 0.0012f * (intensity_target_ - intensity_);

    beat_phase_ += tempo_bpm_ / (60.0f * sample_rate_);
    if(beat_phase_ >= 1.0f)
        beat_phase_ -= 1.0f;

    if(scene_fading_out_)
    {
        scene_gain_ -= 1.0f / 1024.0f;
        if(scene_gain_ <= 0.0f)
        {
            scene_gain_       = 0.0f;
            active_scene_     = requested_scene_;
            scene_fading_out_ = false;
            voice_phase_b_    = voice_phase_a_;
            ResetGrain(grains_[0], true);
            grains_[0].age = 0.0f;
            ResetGrain(grains_[1], true);
            grains_[1].age = 0.5f;
        }
    }
    else if(scene_gain_ < 1.0f)
    {
        scene_gain_ += 1.0f / 1024.0f;
        if(scene_gain_ > 1.0f)
            scene_gain_ = 1.0f;
    }

    const float capture_target = captured_ ? 1.0f : 0.0f;
    capture_gain_ += 0.0025f * (capture_target - capture_gain_);

    if(!captured_)
        Record(input_l, input_r);

    StereoFrame memory = ProcessMemory();
    memory.l *= capture_gain_;
    memory.r *= capture_gain_;
    Overdub(input_l, input_r);

    if(!captured_ && capture_gain_ < 0.0001f)
        playback_active_ = false;

    const float effect_mix = intensity_;
    StereoFrame wet = ProcessLiveFx(input_l, input_r, memory);
    wet.l *= scene_gain_;
    wet.r *= scene_gain_;

    float reverb_send = 0.42f;
    if(active_scene_ == Scene::Pulse)
        reverb_send = 0.30f;
    else if(active_scene_ == Scene::Halo)
        reverb_send = 0.92f;
    else if(active_scene_ == Scene::Ritual)
        reverb_send = 0.24f;
    const StereoFrame reverb
        = ProcessReverb(wet.l * effect_mix * reverb_send,
                        wet.r * effect_mix * reverb_send);

    StereoFrame processed;
    const float dry_gain    = 1.0f - effect_mix * 0.78f;
    const float wet_gain    = effect_mix * 0.92f;
    const float reverb_gain = effect_mix * 0.54f;
    processed.l = input_l * dry_gain + wet.l * wet_gain
                  + reverb.l * reverb_gain;
    processed.r = input_r * dry_gain + wet.r * wet_gain
                  + reverb.r * reverb_gain;

    const float bypass_target = bypassed_ ? 0.0f : 1.0f;
    bypass_mix_ += 0.0015f * (bypass_target - bypass_mix_);

    StereoFrame output;
    const float safe_l = OutputLimit(processed.l);
    const float safe_r = OutputLimit(processed.r);
    output.l = input_l + (safe_l - input_l) * bypass_mix_;
    output.r = input_r + (safe_r - input_r) * bypass_mix_;

    ++sample_counter_;
    return output;
}

} // namespace aurora
