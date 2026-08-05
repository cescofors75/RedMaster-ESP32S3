#pragma once

#include <cstddef>
#include <cstdint>

namespace daisysp
{
class ReverbSc;
class Chorus;
class Phaser;
class Flanger;
class Overdrive;
class LadderFilter;
class Svf;
}

namespace aurora
{

enum class Scene : uint8_t
{
    Ocean = 0,
    Pulse,
    Halo,
    Ritual,
};

struct StereoFrame
{
    float l;
    float r;
};

struct FxModules
{
    daisysp::ReverbSc*     reverb;
    daisysp::Chorus*       chorus;
    daisysp::Phaser*       phaser;
    daisysp::Flanger*      flanger_l;
    daisysp::Flanger*      flanger_r;
    daisysp::Overdrive*    drive_l;
    daisysp::Overdrive*    drive_r;
    daisysp::LadderFilter* ladder_l;
    daisysp::LadderFilter* ladder_r;
    daisysp::Svf*          svf_l;
    daisysp::Svf*          svf_r;
};

class AuroraEngine
{
  public:
    static constexpr uint8_t kSceneCount      = 4;
    static constexpr uint8_t kLoopLengthCount = 4;

    void Init(float sample_rate,
              float* memory_l,
              float* memory_r,
              uint32_t memory_samples,
              const FxModules& fx);

    StereoFrame Process(float input_l, float input_r);

    void SetCapture(bool enabled);
    void ToggleCapture();
    void ClearHistory();

    void SetScene(Scene scene);
    void SetSceneIndex(uint8_t index);
    void StepScene(int increment);

    void SetLoopLengthIndex(uint8_t index);
    void StepLoopLength(int increment = 1);

    void SetCharacter(float normalized);
    void SetIntensity(float normalized);
    void SetEvolve(bool enabled);
    void SetBypass(bool enabled);
    void ToggleBypass();

    void SetTempo(float bpm);
    void TapTempo();

    bool    IsCaptured() const { return captured_; }
    bool    IsEvolving() const { return evolve_; }
    bool    IsBypassed() const { return bypassed_; }
    bool    HasUsableHistory() const;
    Scene   ActiveScene() const { return active_scene_; }
    Scene   RequestedScene() const { return requested_scene_; }
    uint8_t LoopLengthIndex() const { return loop_length_index_; }
    float   Tempo() const { return tempo_bpm_; }
    float   BeatPhase() const { return beat_phase_; }
    float   InputLevel() const { return input_level_; }
    float   Character() const { return character_; }
    float   Intensity() const { return intensity_; }
    float   CapturedSeconds() const;

  private:
    static constexpr uint32_t kMinLoopSamples = 2048;
    struct Grain
    {
        float age;
        float start;
        float rate;
        float pan;
    };

    static float Clamp(float value, float low, float high);
    static float Sanitize(float value);
    static float OutputLimit(float value);
    static float EqualPowerWindow(float phase);

    void        CaptureNow();
    void        Record(float input_l, float input_r);
    void        Overdub(float input_l, float input_r);
    StereoFrame ProcessMemory();
    StereoFrame ProcessDust();
    StereoFrame ReadSeamless(float phase, float rate) const;
    StereoFrame ReadLocal(float position) const;
    void        AdvancePhase(float& phase, float rate) const;
    float       WrapLocal(float position) const;

    void        ResetGrain(Grain& grain, bool initial = false);
    uint32_t    NextRandom();
    float       RandomUnit();

    StereoFrame ProcessReverb(float input_l, float input_r);
    StereoFrame ProcessLiveFx(float input_l,
                              float input_r,
                              const StereoFrame& memory);
    void        UpdateFxParameters();
    float       ProcessDcBlock(float input, uint8_t channel);

    float*   memory_l_       = nullptr;
    float*   memory_r_       = nullptr;
    uint32_t memory_samples_ = 0;
    uint32_t write_position_ = 0;
    uint32_t recorded_samples_ = 0;

    uint32_t loop_start_  = 0;
    uint32_t loop_length_ = 0;
    float    overdub_position_ = 0.0f;

    float sample_rate_   = 48000.0f;
    float tempo_bpm_     = 120.0f;
    float beat_phase_    = 0.0f;
    uint64_t sample_counter_ = 0;
    uint64_t last_tap_sample_ = 0;
    bool     tap_armed_       = false;

    Scene active_scene_    = Scene::Ocean;
    Scene requested_scene_ = Scene::Ocean;
    bool  scene_fading_out_ = false;
    float scene_gain_       = 1.0f;

    uint8_t loop_length_index_ = 2;
    float   character_target_  = 0.5f;
    float   character_         = 0.5f;
    float   intensity_target_  = 0.35f;
    float   intensity_         = 0.35f;

    bool  captured_       = false;
    bool  playback_active_ = false;
    bool  evolve_          = false;
    bool  bypassed_        = false;
    float capture_gain_    = 0.0f;
    float bypass_mix_      = 1.0f;

    float voice_phase_a_ = 0.0f;
    float voice_phase_b_ = 0.0f;
    Grain grains_[2];
    uint32_t random_state_ = 0xA17E5EEDu;

    float dc_input_[2]  = {0.0f, 0.0f};
    float dc_output_[2] = {0.0f, 0.0f};
    float input_level_  = 0.0f;

    FxModules fx_ = {};
    uint8_t   fx_parameter_divider_ = 0;
};

} // namespace aurora
