#ifndef RED808_RAYDRONE_DSP_H
#define RED808_RAYDRONE_DSP_H

#include <stddef.h>
#include <stdint.h>

#include "../../shared/raydrone_protocol.h"

/*
 * Fixed-memory live-input Raydrone engine for the Daisy Seed.
 *
 * The engine owns no large buffers. The application supplies SDRAM storage so
 * initialization and the audio callback never allocate. Control is transferred
 * through a two-word seqlock and latched once at the beginning of each block.
 */
class RaydroneDsp
{
  public:
    static constexpr uint32_t kNominalSampleRate       = 48000u;
    static constexpr uint32_t kTapeSeconds             = 4u;
    static constexpr uint32_t kTapeSamples             = kNominalSampleRate * kTapeSeconds;
    static constexpr uint32_t kReflectionBufferSamples = 4096u;
    static constexpr uint32_t kChorusBufferSamples     = 2048u;
    /* Web can allocate 512 voices. On the H750, 48 preserve the default Web
     * scene (~28 simultaneous grains) plus headroom for recursive bounces. */
    static constexpr uint8_t  kMaxVoices               = 48u;

    struct Storage
    {
        float*   tape;
        uint32_t tapeCapacity;
        float*   reflectionL;
        float*   reflectionR;
        uint32_t reflectionCapacity;
        float*   chorusL;
        float*   chorusR;
        uint32_t chorusCapacity;
    };

    RaydroneDsp();

    void Init(float sampleRate, const Storage& storage);

    /* Main-loop writer. Returns false for an unsupported wire version. */
    bool StageConfig(const RaydroneConfigPayload& config);
    void StageDefaults();

    /*
     * Audio-thread reader. Call once per block, before the first Process().
     * routed is the master-FX graph route; qualityShed lowers the voice/tap
     * budget while preserving the live tape and a continuous output.
     */
    void BeginBlock(bool routed, bool qualityShed, size_t blockFrames);

    /*
     * Captures input unconditionally and returns the smoothed dry/wet insert.
     * inputL/inputR are passed by value so output may alias the caller's L/R.
     */
    void Process(float inputL, float inputR, float& outputL, float& outputR);

    uint8_t ActiveVoiceCount() const { return activeCount_; }
    bool    IsReady() const { return ready_; }

  private:
    static constexpr uint16_t kHannSize       = 1024u;
    static constexpr uint16_t kComb110Storage = 512u;
    static constexpr uint16_t kComb165Storage = 384u;
    static constexpr uint16_t kComb220Storage = 256u;
    static constexpr uint8_t  kMaxFoci         = 16u;

    struct Voice
    {
        float   position;
        float   age;
        float   inverseDuration;
        float   gain;
        float   step;
        float   panL;
        float   panR;
        float   tone;
        uint8_t material;
        uint8_t depth;
    };

    struct Focus
    {
        float   position;
        float   velocity;
        float   weight;
        float   targetWeight;
        float   age;
        float   ttl;
        uint8_t depth;
        bool    active;
    };

    static float Clamp(float value, float low, float high);
    static float Clamp01(float value);
    static float TriangularBipolar(float phase);
    static float TriangularInverse(float u);
    static void  CompilerBarrier();

    void ClearRuntime();
    void ClearVoices();
    void ClearFoci();
    void InitFoci();
    void UpdateFoci(float deltaSeconds);
    void BirthFocus();
    int8_t PickFocus(float& positionSeconds, float& apertureScale);
    void LatchPendingConfig(bool routed);
    void ApplyControlTargets(const RaydroneConfigPayload& config, bool routed);
    void SmoothControls(float coefficient);

    float Random01();
    float NextQmc();
    float WrapTapePosition(float position) const;
    float ReadTapeLinear(float position) const;
    float ReadTapeCatmull(float position) const;
    float ReadCircularCatmull(const float* buffer,
                              uint32_t     capacity,
                              float        position) const;
    float ReadCircularLinear(const float* buffer,
                             uint32_t     capacity,
                             float        position) const;
    float HannAt(float phase) const;

    void    CaptureInput(float mono);
    uint8_t AllocateVoice(uint8_t voiceLimit);
    void    ReleaseActiveVoice(uint8_t activeIndex);
    void    SpawnVoice(float apertureSeconds,
                       float grainSeconds,
                       float gain,
                       float width,
                       uint8_t material,
                       uint8_t voiceLimit);
    void    PlaceVoice(float position,
                       float grainSeconds,
                       float gain,
                       float width,
                       uint8_t material,
                       uint8_t voiceLimit,
                       uint8_t depth);
    void    SpawnBounce(float endPosition,
                        uint8_t remainingDepth,
                        float grainSeconds,
                        float gain,
                        float width,
                        uint8_t material,
                        uint8_t voiceLimit);
    float   NextHarmonicRatio();
    float   ApplyMaterial(Voice& voice, float sample);
    void    ProcessReflections(float inputL,
                               float inputR,
                               float wet,
                               uint8_t material,
                               float& outputL,
                               float& outputR);
    void    ProcessChorus(float inputL,
                          float inputR,
                          float motion,
                          float& outputL,
                          float& outputR);
    void    TrimVoices(uint8_t voiceLimit);

    Storage  storage_;
    float    sampleRate_;
    uint32_t tapeLength_;
    uint32_t tapeWrite_;
    uint32_t tapeSeen_;
    uint32_t reflectionWrite_;
    uint32_t chorusWrite_;
    bool     ready_;
    bool     qualityShed_;
    size_t   blockFrames_;

    Voice   voices_[kMaxVoices];
    uint8_t active_[kMaxVoices];
    uint8_t free_[kMaxVoices];
    uint8_t activeCount_;
    uint8_t freeCount_;

    /* Values derived once per audio block; Process() is the sample hot path. */
    float blockCharacter_;
    float blockMotion_;
    float blockSpace_;
    float blockApertureSeconds_;
    float blockGrainSeconds_;
    float blockGrainGain_;
    float blockBaseDensity_;
    float blockInsertMix_;
    float blockWetVolume_;
    float blockEvolution_;
    float blockRecursiveGain_;
    float blockFocusMinSeconds_;
    float blockFocusMaxSeconds_;
    float blockFocusSpread_;
    float blockFocusDrift_;
    float blockFocusBirthRate_;
    float blockHarmonicBlend_;
    float blockShimmerChance_;
    float blockBounceChance_;
    uint8_t blockFocusDepth_;
    uint8_t blockBounceDepth_;

    float hann_[kHannSize];
    Focus foci_[kMaxFoci];

    float    comb110_[kComb110Storage];
    float    comb165_[kComb165Storage];
    float    comb220_[kComb220Storage];
    uint16_t combLength_[3];
    uint16_t combWrite_[3];
    uint32_t reflectionDelay_[4];

    float reflectionToneL_;
    float reflectionToneR_;
    float spawnAccumulator_;
    float qmcPhase_;
    float motionPhase_;
    float chorusPhase_;
    float focusBirthAccumulator_;
    bool  chorusWet_;
    bool  fociInitialized_;
    uint8_t harmonicIndex_;
    uint32_t rngState_;

    float smoothingCoefficient_;
    float activeSmoothed_;
    float characterSmoothed_;
    float motionSmoothed_;
    float spaceSmoothed_;
    float volumeSmoothed_;
    float mixSmoothed_;
    float evolutionSmoothed_;

    float activeTarget_;
    float characterTarget_;
    float motionTarget_;
    float spaceTarget_;
    float volumeTarget_;
    float mixTarget_;
    float evolutionTarget_;
    uint8_t materialTarget_;
    bool    configActive_;
    float   recursiveEnvelope_;
    float   recursiveFocus_;
    float   recursiveDirection_;

    volatile uint32_t pendingRevision_;
    volatile uint32_t pendingWord0_;
    volatile uint32_t pendingWord1_;
    volatile uint32_t pendingWord2_;
    uint32_t          appliedRevision_;
};

#endif
