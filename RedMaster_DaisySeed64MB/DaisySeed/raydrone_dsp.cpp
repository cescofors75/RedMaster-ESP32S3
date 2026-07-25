#include "raydrone_dsp.h"

#include <math.h>
#include <string.h>

namespace
{
constexpr float kPi             = 3.14159265358979323846f;
constexpr float kTwoPi          = 6.28318530717958647692f;
constexpr float kGolden         = 0.61803398875f;
constexpr float kCombFeedback   = 0.982f;
constexpr float kMicroDetune    = 0.005f;
constexpr float kControlSeconds = 0.020f;
/* Glass grains ring 2.5x longer; cap their live population before they
 * monopolise the same audio callback that renders the drum engine. */
constexpr uint8_t kGlassVoiceLimit = 32u;

static uint32_t PackWord(const RaydroneConfigPayload& config, uint8_t first)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&config);
    return static_cast<uint32_t>(bytes[first])
           | (static_cast<uint32_t>(bytes[first + 1u]) << 8u)
           | (static_cast<uint32_t>(bytes[first + 2u]) << 16u)
           | (static_cast<uint32_t>(bytes[first + 3u]) << 24u);
}

static void UnpackWords(uint32_t word0,
                        uint32_t word1,
                        uint32_t word2,
                        RaydroneConfigPayload& config)
{
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&config);
    bytes[0] = static_cast<uint8_t>(word0);
    bytes[1] = static_cast<uint8_t>(word0 >> 8u);
    bytes[2] = static_cast<uint8_t>(word0 >> 16u);
    bytes[3] = static_cast<uint8_t>(word0 >> 24u);
    bytes[4] = static_cast<uint8_t>(word1);
    bytes[5] = static_cast<uint8_t>(word1 >> 8u);
    bytes[6] = static_cast<uint8_t>(word1 >> 16u);
    bytes[7] = static_cast<uint8_t>(word1 >> 24u);
    bytes[8] = static_cast<uint8_t>(word2);
}
} // namespace

RaydroneDsp::RaydroneDsp()
: storage_{nullptr, 0u, nullptr, nullptr, 0u, nullptr, nullptr, 0u},
  sampleRate_(static_cast<float>(kNominalSampleRate)),
  tapeLength_(0u),
  tapeWrite_(0u),
  tapeSeen_(0u),
  reflectionWrite_(0u),
  chorusWrite_(0u),
  ready_(false),
  qualityShed_(false),
  blockFrames_(0u),
  activeCount_(0u),
  freeCount_(0u),
  blockCharacter_(RAYDRONE_DEFAULT_CHARACTER * 0.01f),
  blockMotion_(RAYDRONE_DEFAULT_MOTION * 0.01f),
  blockSpace_(RAYDRONE_DEFAULT_SPACE * 0.01f),
  blockApertureSeconds_(0.0f),
  blockGrainSeconds_(0.08f),
  blockGrainGain_(0.5f),
  blockBaseDensity_(140.0f),
  blockInsertMix_(0.0f),
  blockWetVolume_(RAYDRONE_DEFAULT_VOLUME * 0.01f),
  blockEvolution_(RAYDRONE_DEFAULT_EVOLUTION * 0.01f),
  blockRecursiveGain_(0.0f),
  blockFocusMinSeconds_(0.20f),
  blockFocusMaxSeconds_(1.80f),
  blockFocusSpread_(0.22f),
  blockFocusDrift_(0.28f),
  blockFocusBirthRate_(0.35f),
  blockHarmonicBlend_(0.0f),
  blockShimmerChance_(0.0f),
  blockBounceChance_(0.0f),
  blockFocusDepth_(0u),
  blockBounceDepth_(0u),
  reflectionToneL_(0.0f),
  reflectionToneR_(0.0f),
  spawnAccumulator_(0.0f),
  qmcPhase_(0.17320508f),
  motionPhase_(0.0f),
  chorusPhase_(0.0f),
  focusBirthAccumulator_(0.0f),
  chorusWet_(false),
  fociInitialized_(false),
  harmonicIndex_(0u),
  rngState_(0x6D2B79F5u),
  smoothingCoefficient_(0.001f),
  activeSmoothed_(0.0f),
  characterSmoothed_(RAYDRONE_DEFAULT_CHARACTER * 0.01f),
  motionSmoothed_(RAYDRONE_DEFAULT_MOTION * 0.01f),
  spaceSmoothed_(RAYDRONE_DEFAULT_SPACE * 0.01f),
  volumeSmoothed_(RAYDRONE_DEFAULT_VOLUME * 0.01f),
  mixSmoothed_(RAYDRONE_DEFAULT_MIX * 0.01f),
  evolutionSmoothed_(RAYDRONE_DEFAULT_EVOLUTION * 0.01f),
  activeTarget_(0.0f),
  characterTarget_(RAYDRONE_DEFAULT_CHARACTER * 0.01f),
  motionTarget_(RAYDRONE_DEFAULT_MOTION * 0.01f),
  spaceTarget_(RAYDRONE_DEFAULT_SPACE * 0.01f),
  volumeTarget_(RAYDRONE_DEFAULT_VOLUME * 0.01f),
  mixTarget_(RAYDRONE_DEFAULT_MIX * 0.01f),
  evolutionTarget_(RAYDRONE_DEFAULT_EVOLUTION * 0.01f),
  materialTarget_(RAYDRONE_DEFAULT_MATERIAL),
  configActive_(false),
  recursiveEnvelope_(0.0f),
  recursiveFocus_(0.5f),
  recursiveDirection_(1.0f),
  pendingRevision_(0u),
  pendingWord0_(0u),
  pendingWord1_(0u),
  pendingWord2_(0u),
  appliedRevision_(0u)
{
    memset(voices_, 0, sizeof(voices_));
    memset(active_, 0, sizeof(active_));
    memset(free_, 0, sizeof(free_));
    memset(hann_, 0, sizeof(hann_));
    memset(foci_, 0, sizeof(foci_));
    memset(comb110_, 0, sizeof(comb110_));
    memset(comb165_, 0, sizeof(comb165_));
    memset(comb220_, 0, sizeof(comb220_));
    memset(combLength_, 0, sizeof(combLength_));
    memset(combWrite_, 0, sizeof(combWrite_));
    memset(reflectionDelay_, 0, sizeof(reflectionDelay_));
}

float RaydroneDsp::Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

float RaydroneDsp::Clamp01(float value)
{
    return Clamp(value, 0.0f, 1.0f);
}

float RaydroneDsp::TriangularBipolar(float phase)
{
    phase -= floorf(phase);
    return phase < 0.5f ? phase * 4.0f - 1.0f
                        : 3.0f - phase * 4.0f;
}

float RaydroneDsp::TriangularInverse(float u)
{
    u = Clamp01(u);
    if(u < 0.5f)
        return sqrtf(2.0f * u) - 1.0f;
    return 1.0f - sqrtf(2.0f - 2.0f * u);
}

void RaydroneDsp::CompilerBarrier()
{
#if defined(__arm__) || defined(__thumb__)
    __asm volatile("dmb" ::: "memory");
#else
    __asm volatile("" ::: "memory");
#endif
}

void RaydroneDsp::Init(float sampleRate, const Storage& storage)
{
    storage_    = storage;
    sampleRate_ = (isfinite(sampleRate) && sampleRate > 1000.0f)
                      ? sampleRate
                      : static_cast<float>(kNominalSampleRate);

    uint32_t requiredTape = static_cast<uint32_t>(sampleRate_ * kTapeSeconds + 0.5f);
    tapeLength_ = storage_.tapeCapacity < requiredTape
                      ? storage_.tapeCapacity
                      : requiredTape;

    const uint32_t requiredReflection
        = static_cast<uint32_t>(sampleRate_ * 0.071f + 2.0f);
    const uint32_t requiredChorus
        = static_cast<uint32_t>(sampleRate_ * 0.022f + 4.0f);

    ready_ = storage_.tape != nullptr
             && tapeLength_ >= static_cast<uint32_t>(sampleRate_ * 2.0f)
             && storage_.reflectionL != nullptr
             && storage_.reflectionR != nullptr
             && storage_.reflectionCapacity >= requiredReflection
             && storage_.chorusL != nullptr
             && storage_.chorusR != nullptr
             && storage_.chorusCapacity >= requiredChorus;

    smoothingCoefficient_
        = 1.0f - expf(-1.0f / (sampleRate_ * kControlSeconds));

    for(uint16_t i = 0u; i < kHannSize; ++i)
    {
        const float phase = static_cast<float>(i)
                            / static_cast<float>(kHannSize - 1u);
        hann_[i] = 0.5f - 0.5f * cosf(kTwoPi * phase);
    }

    combLength_[0] = static_cast<uint16_t>(
        Clamp(floorf(sampleRate_ / 110.0f + 0.5f), 1.0f, kComb110Storage));
    combLength_[1] = static_cast<uint16_t>(
        Clamp(floorf(sampleRate_ / 164.814f + 0.5f), 1.0f, kComb165Storage));
    combLength_[2] = static_cast<uint16_t>(
        Clamp(floorf(sampleRate_ / 220.0f + 0.5f), 1.0f, kComb220Storage));
    const float reflectionSeconds[4] = {0.017f, 0.029f, 0.043f, 0.071f};
    for(uint8_t i = 0u; i < 4u; ++i)
        reflectionDelay_[i] = static_cast<uint32_t>(
            reflectionSeconds[i] * sampleRate_ + 0.5f);

    if(storage_.tape != nullptr && storage_.tapeCapacity > 0u)
        memset(storage_.tape, 0, storage_.tapeCapacity * sizeof(float));
    if(storage_.reflectionL != nullptr && storage_.reflectionCapacity > 0u)
        memset(storage_.reflectionL,
               0,
               storage_.reflectionCapacity * sizeof(float));
    if(storage_.reflectionR != nullptr && storage_.reflectionCapacity > 0u)
        memset(storage_.reflectionR,
               0,
               storage_.reflectionCapacity * sizeof(float));
    if(storage_.chorusL != nullptr && storage_.chorusCapacity > 0u)
        memset(storage_.chorusL, 0, storage_.chorusCapacity * sizeof(float));
    if(storage_.chorusR != nullptr && storage_.chorusCapacity > 0u)
        memset(storage_.chorusR, 0, storage_.chorusCapacity * sizeof(float));

    ClearRuntime();
    StageDefaults();
    LatchPendingConfig(true);

    /* Defaults should be available immediately; only activation is ramped. */
    characterSmoothed_ = characterTarget_;
    motionSmoothed_    = motionTarget_;
    spaceSmoothed_     = spaceTarget_;
    volumeSmoothed_    = volumeTarget_;
    mixSmoothed_       = mixTarget_;
    activeSmoothed_    = 0.0f;
}

void RaydroneDsp::ClearVoices()
{
    activeCount_ = 0u;
    freeCount_   = kMaxVoices;
    for(uint8_t i = 0u; i < kMaxVoices; ++i)
    {
        free_[i] = i;
        voices_[i].position        = 0.0f;
        voices_[i].age             = 0.0f;
        voices_[i].inverseDuration = 1.0f;
        voices_[i].gain            = 0.0f;
        voices_[i].step            = 1.0f;
        voices_[i].panL            = 0.70710678f;
        voices_[i].panR            = 0.70710678f;
        voices_[i].tone            = 0.0f;
        voices_[i].material        = 0u;
        voices_[i].depth           = 0u;
    }
}

void RaydroneDsp::ClearFoci()
{
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        foci_[i].position     = 0.0f;
        foci_[i].velocity     = 0.0f;
        foci_[i].weight       = 0.0f;
        foci_[i].targetWeight = 0.0f;
        foci_[i].age          = 0.0f;
        foci_[i].ttl          = 0.0f;
        foci_[i].depth        = 0u;
        foci_[i].active       = false;
    }
    focusBirthAccumulator_ = 0.0f;
    fociInitialized_       = false;
}

void RaydroneDsp::ClearRuntime()
{
    tapeWrite_        = 0u;
    tapeSeen_         = 0u;
    reflectionWrite_  = 0u;
    chorusWrite_      = 0u;
    reflectionToneL_  = 0.0f;
    reflectionToneR_  = 0.0f;
    spawnAccumulator_ = 0.0f;
    qmcPhase_         = 0.17320508f;
    motionPhase_      = 0.0f;
    chorusPhase_      = 0.0f;
    chorusWet_        = false;
    harmonicIndex_    = 0u;
    rngState_         = 0x6D2B79F5u;
    qualityShed_      = false;
    blockFrames_      = 0u;
    blockCharacter_   = characterSmoothed_;
    blockMotion_      = motionSmoothed_;
    blockSpace_       = spaceSmoothed_;
    blockApertureSeconds_ = 0.0f;
    blockGrainSeconds_ = 0.08f;
    blockGrainGain_    = 0.5f;
    blockBaseDensity_  = 140.0f;
    blockInsertMix_    = 0.0f;
    blockWetVolume_    = volumeSmoothed_;
    blockEvolution_    = evolutionSmoothed_;
    blockRecursiveGain_ = 0.0f;
    blockFocusMinSeconds_ = 0.20f;
    blockFocusMaxSeconds_ = 1.80f;
    blockFocusSpread_     = 0.22f;
    blockFocusDrift_      = 0.28f;
    blockFocusBirthRate_  = 0.35f;
    blockHarmonicBlend_   = 0.0f;
    blockShimmerChance_   = 0.0f;
    blockBounceChance_    = 0.0f;
    blockFocusDepth_      = 0u;
    blockBounceDepth_     = 0u;
    recursiveEnvelope_ = 0.0f;
    recursiveFocus_    = 0.5f;
    recursiveDirection_ = 1.0f;
    memset(comb110_, 0, sizeof(comb110_));
    memset(comb165_, 0, sizeof(comb165_));
    memset(comb220_, 0, sizeof(comb220_));
    memset(combWrite_, 0, sizeof(combWrite_));
    ClearVoices();
    ClearFoci();
}

bool RaydroneDsp::StageConfig(const RaydroneConfigPayload& source)
{
    if(source.version != RAYDRONE_CONFIG_VERSION)
        return false;

    RaydroneConfigPayload config = source;
    config.flags &= RAYDRONE_FLAG_ACTIVE;
    if(config.material >= RAYDRONE_MATERIAL_COUNT)
        config.material = RAYDRONE_MATERIAL_COUNT - 1u;
    if(config.character > RAYDRONE_CHARACTER_SAFE_MAX) config.character = RAYDRONE_CHARACTER_SAFE_MAX;
    if(config.motion > 100u) config.motion = 100u;
    if(config.space > 100u) config.space = 100u;
    if(config.volume > 100u) config.volume = 100u;
    if(config.mix > 100u) config.mix = 100u;
    if(config.evolution > 100u) config.evolution = 100u;

    uint32_t revision = pendingRevision_;
    if((revision & 1u) != 0u)
        ++revision;

    pendingRevision_ = revision + 1u;
    CompilerBarrier();
    pendingWord0_ = PackWord(config, 0u);
    pendingWord1_ = PackWord(config, 4u);
    pendingWord2_ = static_cast<uint32_t>(config.evolution);
    CompilerBarrier();
    pendingRevision_ = revision + 2u;
    return true;
}

void RaydroneDsp::StageDefaults()
{
    RaydroneConfigPayload config;
    config.version   = RAYDRONE_CONFIG_VERSION;
    config.flags     = 0u;
    config.material  = RAYDRONE_DEFAULT_MATERIAL;
    config.character = RAYDRONE_DEFAULT_CHARACTER;
    config.motion    = RAYDRONE_DEFAULT_MOTION;
    config.space     = RAYDRONE_DEFAULT_SPACE;
    config.volume    = RAYDRONE_DEFAULT_VOLUME;
    config.mix       = RAYDRONE_DEFAULT_MIX;
    config.evolution = RAYDRONE_DEFAULT_EVOLUTION;
    StageConfig(config);
}

void RaydroneDsp::ApplyControlTargets(const RaydroneConfigPayload& config,
                                      bool routed)
{
    configActive_   = (config.flags & RAYDRONE_FLAG_ACTIVE) != 0u;
    activeTarget_   = (configActive_ && routed) ? 1.0f : 0.0f;
    materialTarget_ = config.material < RAYDRONE_MATERIAL_COUNT
                          ? config.material
                          : static_cast<uint8_t>(RAYDRONE_MATERIAL_COUNT - 1u);
    characterTarget_ = Clamp(config.character * 0.01f, 0.0f, RAYDRONE_CHARACTER_SAFE_MAX * 0.01f);
    motionTarget_    = Clamp01(config.motion * 0.01f);
    spaceTarget_     = Clamp01(config.space * 0.01f);
    volumeTarget_    = Clamp01(config.volume * 0.01f);
    mixTarget_       = Clamp01(config.mix * 0.01f);
    evolutionTarget_  = Clamp01(config.evolution * 0.01f);
}

void RaydroneDsp::LatchPendingConfig(bool routed)
{
    const uint32_t revisionBefore = pendingRevision_;
    if((revisionBefore & 1u) == 0u && revisionBefore != appliedRevision_)
    {
        CompilerBarrier();
        const uint32_t word0 = pendingWord0_;
        const uint32_t word1 = pendingWord1_;
        const uint32_t word2 = pendingWord2_;
        CompilerBarrier();
        const uint32_t revisionAfter = pendingRevision_;
        if(revisionBefore == revisionAfter && (revisionAfter & 1u) == 0u)
        {
            RaydroneConfigPayload config;
            UnpackWords(word0, word1, word2, config);
            if(config.version == RAYDRONE_CONFIG_VERSION)
            {
                ApplyControlTargets(config, routed);
                appliedRevision_ = revisionAfter;
                return;
            }
        }
    }

    /* Route changes do not need a new payload revision. */
    activeTarget_ = (configActive_ && routed) ? 1.0f : 0.0f;
}

void RaydroneDsp::BeginBlock(bool routed,
                             bool qualityShed,
                             size_t blockFrames)
{
    LatchPendingConfig(routed);
    qualityShed_ = qualityShed;
    blockFrames_ = blockFrames;

    /*
     * Controls are delivered over SPI/UDP at human-rate. Smooth once per
     * audio block instead of once per sample; the scaled coefficient preserves
     * the original ~20 ms time constant while removing six feedback updates,
     * clamps and target loads from every sample in the hot path.
     */
    const float frames = blockFrames_ > 0u
                             ? static_cast<float>(blockFrames_)
                             : 1.0f;
    const float blockCoefficient
        = Clamp(frames * smoothingCoefficient_, 0.0f, 1.0f);
    SmoothControls(blockCoefficient);

    /* P4 intentionally exposes CHARACTER as a safe 0..35 macro. Normalize
     * that complete hardware range to the 0..1 curve used by the WASM
     * engine; treating 35 as 0.35 was making Daisy permanently timid. */
    blockCharacter_ = Clamp01(
        characterSmoothed_ / (RAYDRONE_CHARACTER_SAFE_MAX * 0.01f));
    const float motionRaw = Clamp01(motionSmoothed_);
    blockMotion_ = motionRaw < 0.02f ? 0.0f : motionRaw;
    blockSpace_ = Clamp01(spaceSmoothed_);
    blockApertureSeconds_
        = fminf(0.32f, 0.02f + blockCharacter_ * blockCharacter_ * 0.6f);
    const float rawGrainSeconds = 0.08f + 0.12f * blockCharacter_;
    blockGrainSeconds_ = fminf(0.20f, rawGrainSeconds);
    blockEvolution_ = Clamp01(evolutionSmoothed_);
    blockRecursiveGain_ = Clamp01(
        blockEvolution_ * (0.34f + recursiveEnvelope_ * 0.66f));
    /* P4 has one macro where the WASM page has separate POWER voicing,
     * shimmer, bounce and ambient controls. Morph that macro into the same
     * complete scene instead of reducing it to a barely audible LFO. */
    blockHarmonicBlend_ = Clamp01(blockEvolution_ * 2.2f);
    blockShimmerChance_ = 0.18f * blockHarmonicBlend_;
    blockBounceChance_ = blockEvolution_ > 0.001f
                             ? 0.35f + 0.45f * blockEvolution_
                             : 0.0f;
    blockBounceDepth_ = blockEvolution_ <= 0.001f
                            ? 0u
                            : (blockEvolution_ < 0.62f
                                   ? 1u
                                   : (blockEvolution_ < 0.90f ? 2u : 3u));
    blockFocusDepth_ = blockEvolution_ <= 0.001f
                           ? 0u
                           : (blockEvolution_ < 0.82f ? 2u : 3u);
    blockFocusSpread_ = 0.22f + 0.10f * blockEvolution_;
    blockFocusDrift_ = 0.28f + 0.22f * blockEvolution_;
    blockFocusBirthRate_ = 0.35f + 0.65f * blockEvolution_;

    blockBaseDensity_ = 140.0f + 260.0f * blockCharacter_;
    const float materialDuration = materialTarget_ == 3u ? 2.5f : 1.0f;
    const float concurrency
        = fmaxf(1.0f,
                blockBaseDensity_ * rawGrainSeconds * materialDuration);
    blockGrainGain_ = fminf(0.5f, 1.5f / sqrtf(concurrency));
    blockInsertMix_ = Clamp01(activeSmoothed_ * mixSmoothed_);
    blockWetVolume_ = Clamp01(volumeSmoothed_);
    blockApertureSeconds_ = fminf(
        0.40f, blockApertureSeconds_ * (1.0f + blockRecursiveGain_ * 0.8f));

    const float historySeconds
        = static_cast<float>(tapeLength_) / sampleRate_;
    const float baseLookBehind = Clamp(
        fmaxf(0.12f,
              blockApertureSeconds_
                  + blockGrainSeconds_ * materialDuration * 2.1f
                  + 0.035f),
        0.0f,
        historySeconds * 0.45f);
    if(blockEvolution_ <= 0.001f)
    {
        blockFocusMinSeconds_ = baseLookBehind;
        blockFocusMaxSeconds_ = baseLookBehind;
    }
    else
    {
        blockFocusMinSeconds_ = fmaxf(
            baseLookBehind,
            blockApertureSeconds_
                + blockGrainSeconds_ * materialDuration * 2.1f
                + 0.05f);
        blockFocusMaxSeconds_ = fminf(historySeconds * 0.45f, 1.80f);
        if(blockFocusMaxSeconds_ < blockFocusMinSeconds_ + 0.05f)
            blockFocusMaxSeconds_ = blockFocusMinSeconds_ + 0.05f;
    }

    if(blockEvolution_ > 0.0001f)
    {
        /* Match the WASM auto-evolution sweep: signal energy accelerates a
         * bounded traversal instead of merely changing one tiny offset. */
        const float evolutionStep = blockEvolution_
                                    * (0.0004f
                                       + recursiveEnvelope_ * 0.0018f);
        recursiveFocus_ += recursiveDirection_ * evolutionStep;
        if(recursiveFocus_ >= 1.0f)
        {
            recursiveFocus_ = 1.0f;
            recursiveDirection_ = -1.0f;
        }
        else if(recursiveFocus_ <= 0.0f)
        {
            recursiveFocus_ = 0.0f;
            recursiveDirection_ = 1.0f;
        }
    }
    UpdateFoci(frames / sampleRate_);

    const uint8_t blockVoiceLimit
        = qualityShed_ || materialTarget_ == 3u ? kGlassVoiceLimit : kMaxVoices;
    if(activeCount_ > blockVoiceLimit)
        TrimVoices(blockVoiceLimit);
}

void RaydroneDsp::SmoothControls(float coefficient)
{
    activeSmoothed_ += (activeTarget_ - activeSmoothed_) * coefficient;
    characterSmoothed_
        += (characterTarget_ - characterSmoothed_) * coefficient;
    motionSmoothed_ += (motionTarget_ - motionSmoothed_) * coefficient;
    spaceSmoothed_ += (spaceTarget_ - spaceSmoothed_) * coefficient;
    volumeSmoothed_ += (volumeTarget_ - volumeSmoothed_) * coefficient;
    mixSmoothed_ += (mixTarget_ - mixSmoothed_) * coefficient;
    evolutionSmoothed_ += (evolutionTarget_ - evolutionSmoothed_) * coefficient;
}

float RaydroneDsp::Random01()
{
    uint32_t x = rngState_;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    rngState_ = x;
    return static_cast<float>(x & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float RaydroneDsp::NextQmc()
{
    qmcPhase_ += kGolden;
    if(qmcPhase_ >= 1.0f)
        qmcPhase_ -= 1.0f;
    return qmcPhase_;
}

void RaydroneDsp::InitFoci()
{
    ClearFoci();
    const float span = blockFocusMaxSeconds_ - blockFocusMinSeconds_;
    if(span <= 0.001f)
        return;

    constexpr uint8_t kSeedCount = 3u;
    for(uint8_t i = 0u; i < kSeedCount; ++i)
    {
        float u = 0.5f + static_cast<float>(i) * kGolden;
        u -= floorf(u);
        Focus& focus      = foci_[i];
        focus.position    = blockFocusMinSeconds_ + u * span;
        focus.velocity    = (Random01() - 0.5f) * 2.0f
                            * blockFocusDrift_ * span * 0.02f;
        focus.weight       = 1.0f;
        focus.targetWeight = 1.0f;
        focus.age          = 0.0f;
        focus.ttl          = -1.0f;
        focus.depth        = blockFocusDepth_;
        focus.active       = true;
    }
    fociInitialized_ = true;
}

void RaydroneDsp::BirthFocus()
{
    int8_t slot = -1;
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        if(!foci_[i].active)
        {
            slot = static_cast<int8_t>(i);
            break;
        }
    }
    if(slot < 0)
        return;

    float parentWeight = 0.0f;
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        if(foci_[i].active && foci_[i].depth > 0u)
            parentWeight += foci_[i].weight;
    }
    if(parentWeight <= 0.0001f)
        return;

    float selector = Random01() * parentWeight;
    int8_t parent = -1;
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        if(!foci_[i].active || foci_[i].depth == 0u)
            continue;
        selector -= foci_[i].weight;
        if(selector <= 0.0f)
        {
            parent = static_cast<int8_t>(i);
            break;
        }
    }
    if(parent < 0)
        return;

    const Focus& source = foci_[static_cast<uint8_t>(parent)];
    const uint8_t level = static_cast<uint8_t>(
        blockFocusDepth_ - source.depth + 1u);
    float shrink = 1.0f;
    for(uint8_t i = 0u; i < level; ++i)
        shrink *= 0.55f;

    const float span = blockFocusMaxSeconds_ - blockFocusMinSeconds_;
    Focus& child = foci_[static_cast<uint8_t>(slot)];
    const float offset = (Random01() - 0.5f) * 2.0f
                         * blockFocusSpread_ * span * shrink;
    child.position = Clamp(source.position + offset,
                           blockFocusMinSeconds_,
                           blockFocusMaxSeconds_);
    child.velocity = (Random01() - 0.5f) * 2.0f
                     * blockFocusDrift_ * span * 0.02f
                     * (1.0f + shrink);
    child.weight       = 0.0f;
    child.targetWeight = source.weight * 0.72f;
    child.age          = 0.0f;
    child.ttl          = 12.0f * shrink;
    child.depth        = static_cast<uint8_t>(source.depth - 1u);
    child.active       = true;
}

void RaydroneDsp::UpdateFoci(float deltaSeconds)
{
    if(blockEvolution_ <= 0.001f
       || blockFocusMaxSeconds_ <= blockFocusMinSeconds_)
    {
        if(fociInitialized_)
            ClearFoci();
        return;
    }
    if(!fociInitialized_)
        InitFoci();
    if(!fociInitialized_)
        return;

    const float dt = Clamp(deltaSeconds, 0.0f, 0.1f);
    const float span = blockFocusMaxSeconds_ - blockFocusMinSeconds_;
    const float maximumVelocity = blockFocusDrift_ * span * 0.03f;
    const float fade = fminf(1.0f, dt / 1.5f);

    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        Focus& focus = foci_[i];
        if(!focus.active)
            continue;
        focus.age += dt;
        if(focus.ttl < 0.0f)
            focus.depth = blockFocusDepth_;
        if(Random01() < dt / 3.0f)
            focus.velocity = (Random01() - 0.5f) * 2.0f
                             * maximumVelocity;

        float position = focus.position + focus.velocity * dt;
        if(position < blockFocusMinSeconds_)
        {
            position = blockFocusMinSeconds_
                       + (blockFocusMinSeconds_ - position);
            focus.velocity = -focus.velocity;
        }
        else if(position > blockFocusMaxSeconds_)
        {
            position = blockFocusMaxSeconds_
                       - (position - blockFocusMaxSeconds_);
            focus.velocity = -focus.velocity;
        }
        focus.position = Clamp(position,
                               blockFocusMinSeconds_,
                               blockFocusMaxSeconds_);

        if(focus.ttl >= 0.0f && focus.age > focus.ttl - 1.5f)
            focus.targetWeight = 0.0f;
        focus.weight += (focus.targetWeight - focus.weight) * fade;
        if(focus.ttl >= 0.0f
           && focus.age > focus.ttl
           && focus.weight < 0.002f)
        {
            focus.active = false;
            focus.weight = 0.0f;
        }
    }

    if(blockFocusDepth_ > 0u)
    {
        focusBirthAccumulator_ += blockFocusBirthRate_ * dt;
        uint8_t guard = 0u;
        while(focusBirthAccumulator_ >= 1.0f && guard++ < 4u)
        {
            BirthFocus();
            focusBirthAccumulator_ -= 1.0f;
        }
    }
}

int8_t RaydroneDsp::PickFocus(float& positionSeconds,
                              float& apertureScale)
{
    float totalWeight = 0.0f;
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        if(foci_[i].active)
            totalWeight += foci_[i].weight;
    }
    if(totalWeight <= 0.0001f)
        return -1;

    float selector = Random01() * totalWeight;
    for(uint8_t i = 0u; i < kMaxFoci; ++i)
    {
        if(!foci_[i].active)
            continue;
        selector -= foci_[i].weight;
        if(selector <= 0.0f)
        {
            positionSeconds = foci_[i].position;
            const uint8_t level = blockFocusDepth_ >= foci_[i].depth
                                      ? static_cast<uint8_t>(
                                            blockFocusDepth_ - foci_[i].depth)
                                      : 0u;
            apertureScale = 1.0f;
            for(uint8_t depth = 0u; depth < level; ++depth)
                apertureScale *= 0.70f;
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

float RaydroneDsp::WrapTapePosition(float position) const
{
    const float length = static_cast<float>(tapeLength_);
    /* Grain steps are bounded to roughly one sample, so a branch is enough;
     * the old while-loops were needlessly present in the per-voice hot path. */
    if(position < 0.0f) position += length;
    else if(position >= length) position -= length;
    return position;
}

float RaydroneDsp::ReadTapeCatmull(float position) const
{
    return ReadCircularCatmull(storage_.tape, tapeLength_, position);
}

float RaydroneDsp::ReadTapeLinear(float position) const
{
    /*
     * This is the hot path: every active grain reads the tape once per
     * sample. Catmull-Rom costs four external-SDRAM reads plus a dozen
     * multiplies; linear interpolation costs two reads and is already
     * band-limited by the grain Hann window. Keep Catmull for the short
     * chorus line, but use the cheaper interpolator for live grains.
     */
    if(storage_.tape == nullptr || tapeLength_ < 2u)
        return 0.0f;

    if(position < 0.0f) position += static_cast<float>(tapeLength_);
    else if(position >= static_cast<float>(tapeLength_))
        position -= static_cast<float>(tapeLength_);

    const uint32_t i0 = static_cast<uint32_t>(position);
    const uint32_t i1 = i0 + 1u < tapeLength_ ? i0 + 1u : 0u;
    const float fraction = position - static_cast<float>(i0);
    const float a = storage_.tape[i0];
    const float b = storage_.tape[i1];
    return a + (b - a) * fraction;
}

float RaydroneDsp::ReadCircularCatmull(const float* buffer,
                                       uint32_t     capacity,
                                       float        position) const
{
    if(buffer == nullptr || capacity < 4u)
        return 0.0f;

    if(position < 0.0f) position += static_cast<float>(capacity);
    else if(position >= static_cast<float>(capacity))
        position -= static_cast<float>(capacity);

    const int32_t i1 = static_cast<int32_t>(position);
    int32_t i0 = i1 - 1;
    int32_t i2 = i1 + 1;
    int32_t i3 = i1 + 2;
    if(i0 < 0) i0 += static_cast<int32_t>(capacity);
    if(i2 >= static_cast<int32_t>(capacity)) i2 -= static_cast<int32_t>(capacity);
    if(i3 >= static_cast<int32_t>(capacity)) i3 -= static_cast<int32_t>(capacity);
    if(i3 >= static_cast<int32_t>(capacity)) i3 -= static_cast<int32_t>(capacity);

    const float y0 = buffer[i0];
    const float y1 = buffer[i1];
    const float y2 = buffer[i2];
    const float y3 = buffer[i3];
    const float t  = position - static_cast<float>(i1);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f
           * ((2.0f * y1) + (-y0 + y2) * t
              + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
              + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
}

float RaydroneDsp::ReadCircularLinear(const float* buffer,
                                      uint32_t     capacity,
                                      float        position) const
{
    if(buffer == nullptr || capacity < 2u)
        return 0.0f;
    if(position < 0.0f) position += static_cast<float>(capacity);
    else if(position >= static_cast<float>(capacity))
        position -= static_cast<float>(capacity);
    const uint32_t i0 = static_cast<uint32_t>(position);
    const uint32_t i1 = i0 + 1u < capacity ? i0 + 1u : 0u;
    const float fraction = position - static_cast<float>(i0);
    return buffer[i0] + (buffer[i1] - buffer[i0]) * fraction;
}

float RaydroneDsp::HannAt(float phase) const
{
    phase = Clamp01(phase);
    const float position = phase * static_cast<float>(kHannSize - 1u);
    const uint16_t index = static_cast<uint16_t>(position);
    /* A nearest-bin lookup avoids another interpolation in the voice loop. */
    const uint16_t next = index + 1u < kHannSize
                              ? static_cast<uint16_t>(index + 1u)
                              : index;
    return hann_[position - static_cast<float>(index) >= 0.5f ? next : index];
}

void RaydroneDsp::CaptureInput(float mono)
{
    if(!ready_)
        return;

    /*
     * Match the AudioWorklet live layer: the raw mono drives both paths, while
     * the comb write and final 78/22 tape sample are clamped independently.
     * Pre-clamping here discarded legitimate hot-bus transients.
     */
    mono = isfinite(mono) ? mono : 0.0f;
    float excitation = 0.0f;

    float* combBuffers[3] = {comb110_, comb165_, comb220_};
    for(uint8_t combIndex = 0u; combIndex < 3u; ++combIndex)
    {
        float* buffer = combBuffers[combIndex];
        const uint16_t write = combWrite_[combIndex];
        const float delayed = buffer[write];
        buffer[write] = Clamp(mono + delayed * kCombFeedback, -1.0f, 1.0f);
        uint16_t next = static_cast<uint16_t>(write + 1u);
        if(next >= combLength_[combIndex])
            next = 0u;
        combWrite_[combIndex] = next;
        excitation += delayed;
    }
    excitation *= 1.0f / 3.0f;

    const float liveSource = Clamp(mono * 0.78f + excitation * 0.22f,
                                   -1.0f,
                                   1.0f);
    storage_.tape[tapeWrite_] = liveSource;
    ++tapeWrite_;
    if(tapeWrite_ >= tapeLength_)
        tapeWrite_ = 0u;
    if(tapeSeen_ < tapeLength_)
        ++tapeSeen_;
}

uint8_t RaydroneDsp::AllocateVoice(uint8_t voiceLimit)
{
    if(activeCount_ < voiceLimit && freeCount_ > 0u)
    {
        const uint8_t slot = free_[--freeCount_];
        active_[activeCount_++] = slot;
        return slot;
    }

    uint8_t bestActive = 0u;
    float bestPhase = -1.0f;
    const uint8_t searchCount
        = activeCount_ < voiceLimit ? activeCount_ : voiceLimit;
    for(uint8_t i = 0u; i < searchCount; ++i)
    {
        const Voice& voice = voices_[active_[i]];
        const float phase = voice.age * voice.inverseDuration;
        if(phase > bestPhase)
        {
            bestPhase  = phase;
            bestActive = i;
        }
    }
    return active_[bestActive];
}

void RaydroneDsp::ReleaseActiveVoice(uint8_t activeIndex)
{
    const uint8_t slot = active_[activeIndex];
    --activeCount_;
    active_[activeIndex] = active_[activeCount_];
    free_[freeCount_++] = slot;
}

void RaydroneDsp::TrimVoices(uint8_t voiceLimit)
{
    /*
     * Load shedding must take effect in the block where it is requested.
     * Retire the grains closest to the end of their Hann window first; simply
     * waiting for long Glass grains could otherwise keep 96 voices alive for
     * nearly half a second after the CPU governor trips.
     */
    while(activeCount_ > voiceLimit)
    {
        uint8_t oldest = 0u;
        float   phase  = -1.0f;
        for(uint8_t i = 0u; i < activeCount_; ++i)
        {
            const Voice& voice = voices_[active_[i]];
            const float candidate = voice.age * voice.inverseDuration;
            if(candidate > phase)
            {
                phase  = candidate;
                oldest = i;
            }
        }
        ReleaseActiveVoice(oldest);
    }
}

void RaydroneDsp::SpawnVoice(float apertureSeconds,
                             float grainSeconds,
                             float gain,
                             float width,
                             uint8_t material,
                             uint8_t voiceLimit)
{
    if(tapeLength_ < 4u || voiceLimit == 0u)
        return;
    if(activeCount_ > voiceLimit)
        return;

    float focusSeconds = 0.5f
                         * (blockFocusMinSeconds_ + blockFocusMaxSeconds_);
    float apertureScale = 1.0f;
    if(PickFocus(focusSeconds, apertureScale) < 0
       && blockEvolution_ > 0.0001f)
    {
        focusSeconds = blockFocusMinSeconds_
                       + recursiveFocus_
                             * (blockFocusMaxSeconds_
                                - blockFocusMinSeconds_);
    }
    const float effectiveAperture = apertureSeconds * apertureScale;
    const float focus = static_cast<float>(tapeWrite_)
                        - focusSeconds * sampleRate_;
    const float dispersion = TriangularInverse(NextQmc())
                             * effectiveAperture * sampleRate_;
    PlaceVoice(focus + dispersion,
               grainSeconds,
               gain,
               width,
               material,
               voiceLimit,
               blockBounceDepth_);
}

float RaydroneDsp::NextHarmonicRatio()
{
    if(blockHarmonicBlend_ <= 0.0001f
       || Random01() >= blockHarmonicBlend_)
        return 1.0f;

    /* The same POWER family used by the WASM demo:
     * sub · root · fifth · octave · octave+fifth. Round-robin allocation is
     * the hardware equivalent of its stratified pitch sampling. */
    static const float kPowerRatios[5] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
    float ratio = kPowerRatios[harmonicIndex_];
    harmonicIndex_ = static_cast<uint8_t>((harmonicIndex_ + 1u) % 5u);
    /* Keep the live-tape read safely behind its writer: shimmer the lower
     * POWER degrees, which still yields octave, fifth-high and double octave
     * without producing the web engine's occasional 6x read head. */
    if(ratio <= 1.5f && Random01() < blockShimmerChance_)
        ratio *= 2.0f;
    return ratio;
}

void RaydroneDsp::PlaceVoice(float position,
                             float grainSeconds,
                             float gain,
                             float width,
                             uint8_t material,
                             uint8_t voiceLimit,
                             uint8_t depth)
{
    const uint8_t slot = AllocateVoice(voiceLimit);
    Voice& voice = voices_[slot];
    const float materialDuration = material == 3u ? 2.5f : 1.0f;
    const float durationSamples
        = fmaxf(1.0f, grainSeconds * sampleRate_ * materialDuration);
    const float detune = 1.0f + (Random01() - 0.5f) * kMicroDetune;
    const float plasmaDetune
        = material == 5u ? 1.0f + (Random01() - 0.5f) * 0.04f : 1.0f;
    const float harmonicRatio = NextHarmonicRatio();
    const float pan = (Random01() * 2.0f - 1.0f) * Clamp01(width);

    voice.position        = WrapTapePosition(position);
    voice.age             = 0.0f;
    voice.inverseDuration = 1.0f / durationSamples;
    voice.gain            = gain;
    voice.step            = detune * plasmaDetune * harmonicRatio;
    voice.panL            = sqrtf((1.0f - pan) * 0.5f);
    voice.panR            = sqrtf((1.0f + pan) * 0.5f);
    voice.tone            = 0.0f;
    voice.material        = material;
    voice.depth           = depth;
}

void RaydroneDsp::SpawnBounce(float endPosition,
                              uint8_t remainingDepth,
                              float grainSeconds,
                              float gain,
                              float width,
                              uint8_t material,
                              uint8_t voiceLimit)
{
    float ageSamples = static_cast<float>(tapeWrite_) - endPosition;
    if(ageSamples < 0.0f)
        ageSamples += static_cast<float>(tapeLength_);
    float ageSeconds = ageSamples / sampleRate_;
    ageSeconds += (Random01() - 0.5f) * 0.10f;
    ageSeconds = Clamp(ageSeconds,
                       blockFocusMinSeconds_,
                       blockFocusMaxSeconds_);
    const float position = static_cast<float>(tapeWrite_)
                           - ageSeconds * sampleRate_;
    PlaceVoice(position,
               grainSeconds,
               gain,
               width,
               material,
               voiceLimit,
               remainingDepth);
}

float RaydroneDsp::ApplyMaterial(Voice& voice, float sample)
{
    float shaped = sample;
    switch(voice.material)
    {
        case 1u:
        {
            const float differential = sample - voice.tone;
            voice.tone += 0.12f * differential;
            shaped = sample + differential * 1.6f;
            break;
        }
        case 2u:
            voice.tone += 0.045f * (sample - voice.tone);
            shaped = voice.tone * 0.86f + sample * 0.14f;
            break;
        case 3u:
            voice.tone += 0.22f * (sample - voice.tone);
            shaped = sample * 0.82f + voice.tone * 0.42f;
            break;
        case 4u:
        {
            voice.tone += 0.09f * (sample - voice.tone);
            float waterPhase = voice.age * 0.00037f;
            waterPhase -= floorf(waterPhase);
            shaped = sample
                         * (0.82f + 0.18f * TriangularInverse(waterPhase))
                     + voice.tone * 0.35f;
            break;
        }
        case 5u:
            shaped = sample + (sample - sample * sample * sample) * 0.75f;
            break;
        default:
            break;
    }
    return shaped;
}

void RaydroneDsp::ProcessReflections(float inputL,
                                     float inputR,
                                     float wet,
                                     uint8_t material,
                                     float& outputL,
                                     float& outputR)
{
    const uint32_t capacity = storage_.reflectionCapacity;
    if(capacity == 0u)
    {
        outputL = inputL;
        outputR = inputR;
        return;
    }

    if(wet <= 0.00001f)
    {
        storage_.reflectionL[reflectionWrite_] = inputL;
        storage_.reflectionR[reflectionWrite_] = inputR;
        if(++reflectionWrite_ >= capacity) reflectionWrite_ = 0u;
        reflectionToneL_ *= 0.995f;
        reflectionToneR_ *= 0.995f;
        outputL = inputL;
        outputR = inputR;
        return;
    }

    static const float pathGains[4]   = {0.44f, 0.31f, 0.22f, 0.15f};
    /* At low SPACE the late taps contribute very little but still cost four
     * external-SDRAM reads per sample. Spend the full four-tap budget only
     * once the user asks for a genuinely wide room. */
    const uint8_t pathCount = qualityShed_ || wet < 0.18f ? 2u : 4u;
    float rayL = 0.0f;
    float rayR = 0.0f;

    for(uint8_t path = 0u; path < pathCount; ++path)
    {
        uint32_t delay = reflectionDelay_[path];
        if(delay >= capacity) delay = capacity - 1u;
        const uint32_t read = reflectionWrite_ >= delay
                                  ? reflectionWrite_ - delay
                                  : reflectionWrite_ + capacity - delay;
        const float cross = (path & 1u) == 0u ? 0.18f : 0.42f;
        const float delayedL = storage_.reflectionL[read];
        const float delayedR = storage_.reflectionR[read];
        rayL += (delayedL * (1.0f - cross) + delayedR * cross)
                * pathGains[path];
        rayR += (delayedR * (1.0f - cross) + delayedL * cross)
                * pathGains[path];
    }

    float absorb = 0.16f;
    switch(material)
    {
        case 1u: absorb = 0.30f; break;
        case 2u: absorb = 0.055f; break;
        case 3u: absorb = 0.20f; break;
        case 4u: absorb = 0.11f; break;
        case 5u: absorb = 0.38f; break;
        default: break;
    }
    reflectionToneL_ += absorb * (rayL - reflectionToneL_);
    reflectionToneR_ += absorb * (rayR - reflectionToneR_);

    if(material == 1u || material == 5u)
    {
        rayL += rayL - reflectionToneL_;
        rayR += rayR - reflectionToneR_;
    }
    else if(material == 2u || material == 4u)
    {
        rayL = rayL * 0.45f + reflectionToneL_ * 0.55f;
        rayR = rayR * 0.45f + reflectionToneR_ * 0.55f;
    }

    const float feedback = Clamp(
        wet * (0.18f + wet * 0.24f) * (1.0f + blockRecursiveGain_ * 0.65f),
        0.0f, 0.46f);
    storage_.reflectionL[reflectionWrite_]
        = Clamp(inputL + rayL * feedback, -4.0f, 4.0f);
    storage_.reflectionR[reflectionWrite_]
        = Clamp(inputR + rayR * feedback, -4.0f, 4.0f);
    if(++reflectionWrite_ >= capacity) reflectionWrite_ = 0u;

    outputL = inputL + (rayL - inputL) * wet;
    outputR = inputR + (rayR - inputR) * wet;
}

void RaydroneDsp::ProcessChorus(float inputL,
                                float inputR,
                                float motion,
                                float& outputL,
                                float& outputR)
{
    const uint32_t capacity = storage_.chorusCapacity;
    const float wet = qualityShed_ ? 0.0f : 0.22f * Clamp01(motion);
    if(capacity == 0u)
    {
        outputL = inputL;
        outputR = inputR;
        return;
    }

    if(wet <= 0.00001f)
    {
        /* Motion is normally zero. Do not perform two SDRAM writes per audio
         * sample just to advance a bypassed chorus line. A one-time clear on
         * the falling edge removes the old tail before a later activation. */
        if(chorusWet_)
        {
            memset(storage_.chorusL, 0, capacity * sizeof(float));
            memset(storage_.chorusR, 0, capacity * sizeof(float));
            chorusWrite_ = 0u;
            chorusPhase_ = 0.0f;
            chorusWet_ = false;
        }
        outputL = inputL;
        outputR = inputR;
        return;
    }

    chorusWet_ = true;
    storage_.chorusL[chorusWrite_] = inputL;
    storage_.chorusR[chorusWrite_] = inputR;

    /*
     * Basic workspace changes only chorus wet. Its advanced defaults remain
     * fixed at 0.25 Hz and 8 ms, with one shared tap for both channels.
     */
    const float tri = chorusPhase_ < 0.5f
                          ? chorusPhase_ * 2.0f
                          : 2.0f - chorusPhase_ * 2.0f;
    const float delay = (0.014f + tri * 0.008f) * sampleRate_;
    const float read = static_cast<float>(chorusWrite_) - delay;
    const float chorusL
        = ReadCircularLinear(storage_.chorusL, capacity, read);
    const float chorusR
        = ReadCircularLinear(storage_.chorusR, capacity, read);

    if(++chorusWrite_ >= capacity) chorusWrite_ = 0u;
    chorusPhase_ += 0.25f / sampleRate_;
    if(chorusPhase_ >= 1.0f)
        chorusPhase_ -= 1.0f;
    outputL = inputL + (chorusL - inputL) * wet;
    outputR = inputR + (chorusR - inputR) * wet;
}

void RaydroneDsp::Process(float inputL,
                          float inputR,
                          float& outputL,
                          float& outputR)
{
    const float dryL = isfinite(inputL) ? inputL : 0.0f;
    const float dryR = isfinite(inputR) ? inputR : 0.0f;
    CaptureInput((dryL + dryR) * 0.5f);

    if(!ready_)
    {
        outputL = dryL;
        outputR = dryR;
        return;
    }

    const float insertMix = blockInsertMix_;
    if(activeTarget_ <= 0.0f && activeSmoothed_ < 0.00001f)
    {
        if(activeCount_ != 0u)
            ClearVoices();
        spawnAccumulator_ = 0.0f;
        /*
         * Keep auxiliary rings moving while bypassed. Zeros replace every old
         * reflection in about 85 ms and every chorus sample in about 43 ms, so
         * a later activation cannot resurrect a frozen tail.
         */
        float discardL;
        float discardR;
        ProcessReflections(0.0f,
                           0.0f,
                           0.0f,
                           materialTarget_,
                           discardL,
                           discardR);
        ProcessChorus(0.0f,
                      0.0f,
                      0.0f,
                      discardL,
                      discardR);
        outputL = dryL;
        outputR = dryR;
        return;
    }

    const float motion = blockMotion_;
    const float space = blockSpace_;
    const float apertureSeconds = blockApertureSeconds_;
    const float grainSeconds = blockGrainSeconds_;
    const float grainGain = blockGrainGain_;
    const float recursiveWidth = fmaxf(0.78f * space,
                                       0.75f * blockHarmonicBlend_);
    const float motionRate = 0.1f + 0.6f * motion;
    const float motionDepth = 0.45f * motion;
    /* Motion=0 is the default and needs no per-sample phase/floorf work. */
    float density = blockBaseDensity_;
    if(motion > 0.0f)
    {
        const float motionValue = TriangularBipolar(motionPhase_);
        density *= fmaxf(0.0f, 1.0f + motionValue * motionDepth);
    }
    const uint8_t voiceLimit
        = qualityShed_ || materialTarget_ == 3u ? kGlassVoiceLimit : kMaxVoices;
    if(qualityShed_)
        density *= 0.55f;

    const uint32_t requiredHistory
        = static_cast<uint32_t>(blockFocusMaxSeconds_ * sampleRate_)
          + static_cast<uint32_t>(blockFrames_);
    const bool liveReady = tapeSeen_ >= requiredHistory;

    if(activeTarget_ > 0.0f && liveReady)
    {
        spawnAccumulator_ += density / sampleRate_;
        while(spawnAccumulator_ >= 1.0f)
        {
            SpawnVoice(apertureSeconds,
                       grainSeconds,
                       grainGain,
                       recursiveWidth,
                       materialTarget_,
                       voiceLimit);
            spawnAccumulator_ -= 1.0f;
        }
    }
    else
    {
        spawnAccumulator_ = 0.0f;
    }

    float wetL = 0.0f;
    float wetR = 0.0f;
    uint8_t activeIndex = 0u;
    while(activeIndex < activeCount_)
    {
        Voice& voice = voices_[active_[activeIndex]];
        const float phase = voice.age * voice.inverseDuration;
        if(phase >= 1.0f)
        {
            const float endPosition = voice.position;
            const uint8_t remainingDepth = voice.depth;
            ReleaseActiveVoice(activeIndex);
            if(remainingDepth > 0u
               && liveReady
               && Random01() < blockBounceChance_)
            {
                SpawnBounce(endPosition,
                            static_cast<uint8_t>(remainingDepth - 1u),
                            grainSeconds,
                            grainGain,
                            recursiveWidth,
                            materialTarget_,
                            voiceLimit);
            }
            continue;
        }

        const float raw = ReadTapeLinear(voice.position);
        const float sample = ApplyMaterial(voice, raw)
                             * HannAt(phase) * voice.gain;
        wetL += sample * voice.panL;
        wetR += sample * voice.panR;
        voice.position = WrapTapePosition(voice.position + voice.step);
        voice.age += 1.0f;
        ++activeIndex;
    }

    float reflectedL;
    float reflectedR;
    const float reflectionWet = fmaxf(0.72f * space,
                                      0.42f * blockHarmonicBlend_);
    ProcessReflections(wetL,
                       wetR,
                       reflectionWet,
                       materialTarget_,
                       reflectedL,
                       reflectedR);

    float chorusedL;
    float chorusedR;
    ProcessChorus(reflectedL,
                  reflectedR,
                  motion,
                  chorusedL,
                  chorusedR);

    if(motion > 0.0f)
    {
        motionPhase_ += motionRate / sampleRate_;
        if(motionPhase_ >= 1.0f)
            motionPhase_ -= 1.0f;
    }

    const float wetVolume = blockWetVolume_;
    chorusedL = Clamp(chorusedL * wetVolume, -4.0f, 4.0f);
    chorusedR = Clamp(chorusedR * wetVolume, -4.0f, 4.0f);
    outputL = dryL + (chorusedL - dryL) * insertMix;
    outputR = dryR + (chorusedR - dryR) * insertMix;

    /* Follow RayDrone's own wet output, excluding the parallel dry signal.
     * This makes the recursion genuinely self-referential even at low MIX. */
    if (blockEvolution_ > 0.0001f) {
        const float level = fminf(
            1.0f, 0.5f * (fabsf(chorusedL) + fabsf(chorusedR)));
        recursiveEnvelope_ += (level - recursiveEnvelope_) * 0.0025f;
    } else {
        recursiveEnvelope_ *= 0.9975f;
    }
}
