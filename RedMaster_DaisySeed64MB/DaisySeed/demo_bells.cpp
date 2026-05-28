/* ═══════════════════════════════════════════════════════════════════
 *  demo_bells.cpp  —  "The Bells" standalone test  (Jeff Mills 1992)
 * ───────────────────────────────────────────────────────────────────
 *  Prueba la Daisy Seed SIN ESP32, SIN SD, SIN SPI.
 *  Solo síntesis pura: TR-909 + FM2Op bells.
 *
 *  BPM : 150  |  48 kHz  |  128 samples/block
 *  Key : La menor  (A3 = MIDI 57,  A5 = MIDI 81)
 *
 *  Patrón de batería (16 pasos = 1 compás):
 *    BD  paso  0, 4, 8,12   — 4-on-the-floor, drive máximo
 *    OH  paso  2, 6,10,14   — offbeat 8ths,   decay totalmente abierto
 *    CP  paso  4,12         — tiempos 2 y 4,  enterrado -14 dB
 *    RS  paso  0, 2, 4,...  — todas las corcheas, fade-in en compás 8+
 *
 *  Patrón de bells (32 pasos = 2 compases):
 *    Bell grave  (A3)  paso  0         — cada 2 compases
 *    Bell aguda  (A5)  paso  0, 16     — cada compás (siempre encendida)
 *
 *  LED:  parpadea en cada negra (pasos 0, 4, 8, 12)
 *
 *  Build:  make DEMO_BELLS=1
 * ═══════════════════════════════════════════════════════════════════ */

#include "daisy_seed.h"

/* ── Fast math: sinf parabólico + expf bit-trick (solo para synth) ── */
static inline float __fast_sinf(float x) {
    float p = x * 0.15915494f;
    p -= (float)(int)p;
    if (p < 0.0f) p += 1.0f;
    float v = 2.0f * p - 1.0f;
    float y = 4.0f * v * (1.0f - fabsf(v));
    return -(0.225f * (y * fabsf(y) - y) + y);
}
static inline float __fast_expf(float x) {
    union { float f; int32_t i; } u;
    u.i = (int32_t)(12102203.0f * x) + 1065353216;
    return (u.i > 0) ? u.f : 0.0f;
}
#define sinf(x) __fast_sinf(x)
#define expf(x) __fast_expf(x)
#include "synth/tr909.h"
#include "synth/fm2op.h"
#undef sinf
#undef expf

using namespace daisy;

/* ─────────────────────────────────────────────────────────────────
 *  Constantes de tiempo
 * ───────────────────────────────────────────────────────────────── */
static constexpr float    kSR        = 48000.0f;
static constexpr uint32_t kBlockSize = 128u;
static constexpr float    kBpm       = 150.0f;
/* 16th note = 60 / (BPM*4) * SR  →  4800 muestras a 150 BPM */
static constexpr uint32_t kStepLen   = 4800u;

/* NoteOff de bells 500 ms después del NoteOn
 * → envelope portador en ~0.88 al soltar, luego cRel lo lleva a 0 */
static constexpr int32_t kBellOffMs = (int32_t)(kSR * 0.5f);   /* 24000 */

/* ─────────────────────────────────────────────────────────────────
 *  Patrones bitmask (bit N = paso N)
 * ───────────────────────────────────────────────────────────────── */
/* Batería — 16 pasos (1 compás) */
static constexpr uint16_t kPatBD = 0x1111u;  /* 0,4,8,12          */
static constexpr uint16_t kPatOH = 0x4444u;  /* 2,6,10,14         */
static constexpr uint16_t kPatCP = 0x1010u;  /* 4,12              */
static constexpr uint16_t kPatRS = 0x5555u;  /* 0,2,4,6,8,10,12,14 */

/* Bells — 32 pasos (2 compases) */
static constexpr uint32_t kPatBL = 0x00000001u;  /* paso 0 (cada 2 compases) */
static constexpr uint32_t kPatBH = 0x00010001u;  /* pasos 0 y 16 (cada compás) */

/* ─────────────────────────────────────────────────────────────────
 *  Globales (accedidos solo desde AudioCallback)
 * ───────────────────────────────────────────────────────────────── */
static DaisySeed    hw;
static TR909::Kit   drum;
static FM2Op::Synth bellLow;    /* A3 = MIDI 57,  220 Hz */
static FM2Op::Synth bellHigh;   /* A5 = MIDI 81,  880 Hz */

static uint32_t stepAccum  = 0u;
static uint8_t  step16     = 0u;   /* 0..15 */
static uint8_t  bellStep   = 0u;   /* 0..31 */
static uint32_t barCount   = 0u;
static bool     rideOn     = false;
static int32_t  bellLowOff  = -1;
static int32_t  bellHighOff = -1;

/* ─────────────────────────────────────────────────────────────────
 *  TriggerStep — dispara todos los instrumentos del paso actual
 * ───────────────────────────────────────────────────────────────── */
static void TriggerStep()
{
    /* ── Drums ── */
    if ((kPatBD >> step16) & 1u)
        drum.Trigger(TR909::INST_KICK, 1.0f);

    if ((kPatOH >> step16) & 1u)
        drum.Trigger(TR909::INST_HIHAT_O, 0.92f);

    if ((kPatCP >> step16) & 1u)
        drum.Trigger(TR909::INST_CLAP, 0.50f);

    if (rideOn && ((kPatRS >> step16) & 1u))
        drum.Trigger(TR909::INST_RIDE, 0.62f);

    /* ── Bells (2-bar loop) ── */
    if ((kPatBL >> bellStep) & 1u) {
        bellLow.NoteOn(57, 0.82f);
        bellLowOff = kBellOffMs;
    }
    if ((kPatBH >> bellStep) & 1u) {
        bellHigh.NoteOn(81, 0.68f);
        bellHighOff = kBellOffMs;
    }

    /* LED on en negras (pasos 0,4,8,12), off en corcheas intermedias */
    hw.SetLed((step16 & 3u) == 0u);

    /* Avanzar contadores */
    step16   = (step16   + 1u) % 16u;
    bellStep = (bellStep + 1u) % 32u;
    if (step16 == 0u) {
        barCount++;
        if (!rideOn && barCount >= 8u)
            rideOn = true;   /* Ride entra en corcheas a partir del compás 8 */
    }
}

/* ─────────────────────────────────────────────────────────────────
 *  AudioCallback
 *  SINE_TEST=1 → tono 440 Hz puro, sin síntesis (diagnóstico)
 * ───────────────────────────────────────────────────────────────── */
#if SINE_TEST
void AudioCallback(AudioHandle::InputBuffer,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    static float sinePhase = 0.0f;
    static uint32_t blinkAccum = 0u;
    blinkAccum += (uint32_t)size;
    if (blinkAccum >= 24000u) {          /* parpadeo cada 0.5s */
        blinkAccum -= 24000u;
        static bool ledState = false;
        ledState = !ledState;
        hw.SetLed(ledState);
    }
    for (size_t i = 0u; i < size; i++) {
        sinePhase += 440.0f / kSR;
        if (sinePhase >= 1.0f) sinePhase -= 1.0f;
        /* sinf estándar — NO fast math — para descartar problemas de DSP */
        float s = 0.5f * sinf(6.283185307f * sinePhase);
        out[0][i] = s;
        out[1][i] = s;
    }
}
#else
void AudioCallback(AudioHandle::InputBuffer,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    /* NoteOff de bells: contar muestras desde el trigger */
    if (bellLowOff > 0) {
        bellLowOff -= (int32_t)size;
        if (bellLowOff <= 0) { bellLow.NoteOff();  bellLowOff  = -1; }
    }
    if (bellHighOff > 0) {
        bellHighOff -= (int32_t)size;
        if (bellHighOff <= 0) { bellHigh.NoteOff(); bellHighOff = -1; }
    }

    /* Secuenciador de pasos */
    stepAccum += (uint32_t)size;
    while (stepAccum >= kStepLen) {
        stepAccum -= kStepLen;
        TriggerStep();
    }

    /* Renderizado muestra a muestra */
    for (size_t i = 0u; i < size; i++) {
        float drm = drum.Process();
        float bel = bellLow.Process() + bellHigh.Process();

        float mix = drm * 0.82f + bel * 0.30f;
        mix = mix > 0.95f ? 0.95f : (mix < -0.95f ? -0.95f : mix);

        out[0][i] = mix;
        out[1][i] = mix;
    }
}
#endif

/* ─────────────────────────────────────────────────────────────────
 *  main
 * ───────────────────────────────────────────────────────────────── */
int main()
{
    hw.Init();

    /* ── Blink de arranque: 5 destellos rápidos confirman que el firmware corre ── */
    for (int i = 0; i < 5; i++) {
        hw.SetLed(true);
        System::Delay(80);
        hw.SetLed(false);
        System::Delay(80);
    }
    System::Delay(300);

    /* ── TR-909 Kit ── */
    drum.Init(kSR);
    drum.LoadPreset(TR909::Presets::Techno);

    /* Kick: Jeff Mills lo clipea a máximo — compresor exagera el noise floor
     * Aquí: drive=1.0 + compression alta + decay largo = mismo efecto */
    drum.kick.drive       = 1.0f;
    drum.kick.compression = 0.85f;
    drum.kick.decay       = 0.90f;
    drum.kick.pitch       = 44.0f;
    drum.kick.pitchAmt    = 14.0f;
    drum.SetVolume(TR909::INST_KICK, 1.35f);

    /* Open HiHat: decay totalmente abierto, se derrama sobre el beat */
    drum.hihatO.decay = 1.80f;
    drum.hihatO.tone  = 0.65f;
    drum.SetVolume(TR909::INST_HIHAT_O, 0.78f);

    /* Clap: lleva delay en el original — aquí solo lo enterramos en el mix */
    drum.clap.decay = 0.40f;
    drum.clap.snap  = 0.70f;
    drum.SetVolume(TR909::INST_CLAP, 0.18f);

    /* Ride: 8th notes, fade-in manual desde compás 8 */
    drum.ride.decay     = 2.00f;
    drum.ride.bellDecay = 0.15f;
    drum.ride.bellAmt   = 0.40f;
    drum.ride.tone      = 0.55f;
    drum.SetVolume(TR909::INST_RIDE, 0.52f);

    /* ── FM Bell grave — A3 (220 Hz)
     *  Ratio 14: el 14º armónico FM crea los sidebands metálicos
     *  característicos de las bells de techno clásico.
     *  Detune +4 cents: desfase fino para los sidebands (ring-mod casero).
     *  NoteOff a 500ms → release de 1.5s desde ~0.88 de amplitud. ── */
    bellLow.Init(kSR);
    bellLow.params.ratio    = 14.0f;
    bellLow.params.index    = 3.0f;
    bellLow.params.feedback = 0.10f;
    bellLow.params.algo     = 0;
    bellLow.params.detune   = 4.0f;
    bellLow.params.cAtk     = 0.001f;
    bellLow.params.cDec     = 4.0f;
    bellLow.params.cSus     = 0.0f;
    bellLow.params.cRel     = 1.5f;
    bellLow.params.mAtk     = 0.001f;
    bellLow.params.mDec     = 1.5f;
    bellLow.params.mSus     = 0.0f;
    bellLow.params.mRel     = 0.5f;
    bellLow.params.velSens  = 0.5f;
    bellLow.params.volume   = 0.88f;

    /* ── FM Bell aguda — A5 (880 Hz)
     *  Mismos sidebands, -4 cents: complementa el detuning de la grave.
     *  Siempre encendida (dispara en cada downbeat). ── */
    bellHigh.Init(kSR);
    bellHigh.params.ratio    = 14.0f;
    bellHigh.params.index    = 2.5f;
    bellHigh.params.feedback = 0.06f;
    bellHigh.params.algo     = 0;
    bellHigh.params.detune   = -4.0f;
    bellHigh.params.cAtk     = 0.001f;
    bellHigh.params.cDec     = 3.0f;
    bellHigh.params.cSus     = 0.0f;
    bellHigh.params.cRel     = 1.0f;
    bellHigh.params.mAtk     = 0.001f;
    bellHigh.params.mDec     = 1.0f;
    bellHigh.params.mSus     = 0.0f;
    bellHigh.params.mRel     = 0.3f;
    bellHigh.params.velSens  = 0.4f;
    bellHigh.params.volume   = 0.72f;

    hw.SetAudioBlockSize(kBlockSize);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.StartAudio(AudioCallback);

    hw.SetLed(true);

    /* Bucle principal vacío — toda la lógica corre en AudioCallback */
    while (true) {
        System::Delay(100);
    }
}
