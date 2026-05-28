/* ═══════════════════════════════════════════════════════════════════
 *  DEMO BELLS — "The Bells" (Jeff Mills) tribute · Daisy Seed standalone
 * ─────────────────────────────────────────────────────────────────
 *  Recreación del clásico techno de Detroit, mostrando el potencial del
 *  RED808 sin ESP32. Suena solo al arrancar y evoluciona como el track
 *  original: NO hay fills programados — la energía se dirige muteando y
 *  desmuteando bucles (el "truco real" de Mills en la mesa).
 *
 *  Motores (todos ya en el repo):
 *    · TR-909 (tr909.h)  → kick distorsionado al máximo, open-hat con
 *                          decay abierto que se derrama, ride en corcheas
 *                          (entra en fade), clap con delay enterrado.
 *    · FM 2-op (fm2op.h) → las bells: ring modulation + detune fino para
 *                          balancear sidebands → timbre metálico. Motif en
 *                          La menor, bell grave que se mutea/desmutea,
 *                          bell aguda siempre encendida.
 *    · TB-303 (tb303.h)  → bassline chuffy en 16 corcheas (entra tarde,
 *                          como en el original ~bar 65).
 *
 *  Build:   build_daisy.ps1 -DemoBells   (o make DEMO_BELLS=1)
 *           → build/DemoBells.bin
 *  Flash:   flash_bells.ps1   (o flash_daisy.ps1 -DemoBells)
 *           NO necesita samples.bin (síntesis pura).
 * ═══════════════════════════════════════════════════════════════════ */

#include "daisy_seed.h"
#define USE_DAISYSP_LGPL
#include "daisysp.h"
#include <math.h>

/* ── Fast math para los motores de síntesis (igual que el firmware
 *    principal): sinf parabólico + expf bit-trick. Solo afecta a los
 *    headers de synth incluidos a continuación. Evita underruns con
 *    6 voces FM + 909 + 303 simultáneos. ── */
static inline float __fast_sinf(float x) {
    float phase = x * 0.15915494f;
    phase -= (float)(int)(phase);
    if(phase < 0.0f) phase += 1.0f;
    float p = 2.0f * phase - 1.0f;
    float y = 4.0f * p * (1.0f - fabsf(p));
    return -(0.225f * (y * fabsf(y) - y) + y);
}
static inline float __fast_expf(float x) {
    union { float f; int32_t i; } v;
    v.i = (int32_t)(12102203.0f * x) + 1065353216;
    return (v.i > 0) ? v.f : 0.0f;
}
#define sinf(x) __fast_sinf(x)
#define expf(x) __fast_expf(x)

#include "synth/tr909.h"
#include "synth/tb303.h"
#include "synth/fm2op.h"

#undef sinf
#undef expf

using namespace daisy;
using namespace daisysp;

DaisySeed hw;

static constexpr float  SAMPLE_RATE = 48000.0f;
static constexpr size_t AUDIO_BLOCK = 48;

/* ═══════════════════════════════════════════════════════════════════
 *  MOTORES (en SDRAM — los kits son grandes)
 * ═══════════════════════════════════════════════════════════════════ */
static TR909::Kit   drums;          /* no SDRAM: constructors run before hw.Init() */
static TB303::Synth bass;
DSY_SDRAM_BSS static ReverbSc     reverb;

/* Clap delay (enterrado en el mix) — buffer en SDRAM */
static constexpr size_t CLAP_DLY_SIZE = 24000;   /* 0.5 s @ 48k */
DSY_SDRAM_BSS static float clapDlyBuf[CLAP_DLY_SIZE];
static size_t clapDlyWp   = 0;
static float  clapDlyFb   = 0.45f;

/* ═══════════════════════════════════════════════════════════════════
 *  BELLS — polifonía FM con ring modulation
 * ═══════════════════════════════════════════════════════════════════ */
static constexpr int NUM_BELLS = 6;
static FM2Op::Synth bells[NUM_BELLS];
static uint8_t      bellNext = 0;

/* Preset de campana metálica: dos sines en ring-mod, ratio inarmónico,
 * detune fino para balancear sidebands. Decay largo (la cola se derrama). */
static void ApplyBellPreset(FM2Op::Synth& v)
{
    v.params.algo     = 2;        /* RING MODULATION (C * M)            */
    v.params.ratio    = 1.41f;    /* √2 → inarmónico → metálico         */
    v.params.index    = 1.0f;     /* en ring-mod actúa como mezcla mod  */
    v.params.feedback = 0.0f;
    v.params.detune   = 4.0f;     /* detune fino → sidebands batiendo   */
    v.params.velSens  = 0.4f;

    /* Carrier: golpe instantáneo, cola larga de campana */
    v.params.cAtk = 0.001f;
    v.params.cDec = 2.6f;
    v.params.cSus = 0.0f;
    v.params.cRel = 1.4f;

    /* Modulator: sostiene el timbre metálico durante la cola */
    v.params.mAtk = 0.001f;
    v.params.mDec = 2.2f;
    v.params.mSus = 0.2f;
    v.params.mRel = 1.0f;

    v.params.volume = 0.55f;
}

static void BellNoteOn(uint8_t midiNote, float vel)
{
    FM2Op::Synth& v = bells[bellNext];
    ApplyBellPreset(v);
    v.NoteOn(midiNote, vel);
    bellNext = (uint8_t)((bellNext + 1) % NUM_BELLS);
}

/* ═══════════════════════════════════════════════════════════════════
 *  SECUENCIADOR  — 16 semicorcheas por compás
 * ═══════════════════════════════════════════════════════════════════ */
static constexpr float BPM  = 132.0f;            /* tempo Mills          */
static const uint32_t kStepSamples =
    (uint32_t)(SAMPLE_RATE * 60.0f / BPM / 4.0f); /* semicorchea         */

static uint32_t stepCounter = 0;   /* samples dentro del step actual     */
static int      step16      = 0;   /* 0..15 dentro del compás            */
static uint32_t barCounter  = 0;   /* compás absoluto                    */

/* ── Motif de las bells (La menor) ── 16 pasos, 0 = silencio ──
 *  Posiciones según la grilla del patrón:
 *    Bell ALTA (siempre ON): pasos 0, 3, 5, 8, 11, 13
 *    Bell BAJA (muted/unmuted por Mills): pasos 0, 6, 11
 *           paso:  1   e   +   a   2   e   +   a   3   e   +   a   4   e   +   a
 *           idx:   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15 */
static const uint8_t kBellHi[16] = {
    81, 0, 0, 76,  0, 79, 0, 0,  81, 0, 0, 84,  0, 83, 0, 0
};  /* A5 . . E5 . G5 . . A5 . . C6 . B5 . .   (ostinato hipnótico) */

static const uint8_t kBellLo[16] = {
    69, 0, 0, 0,  0, 0, 64, 0,  0, 0, 0, 72,  0, 0, 0, 0
};  /* A4 . . .  . . E4 .  . . . C5 . . . .    (refuerzo grave)     */

/* ── Bassline 303 (La menor) ── 16 notas steady, ataque "chuffy" ──
 *  La grilla muestra 16 celdas uniformes → ostinato de semicorcheas
 *  sobre la tónica (A1), con acento en cada tiempo. */
static const uint8_t kBassNotes[16] = {
    33, 33, 33, 33,  33, 33, 33, 33,
    33, 33, 33, 33,  33, 33, 33, 33
};
static const uint8_t kBassAccent[16] = {
    1, 0, 0, 0,  1, 0, 0, 0,
    1, 0, 0, 0,  1, 0, 0, 0
};

/* ═══════════════════════════════════════════════════════════════════
 *  ARREGLO  — el "truco Mills": todo se controla con mute/unmute.
 *  Devuelve qué elementos suenan en un compás dado. La estructura
 *  hace build-up y luego entra en un loop con variaciones de mute.
 * ═══════════════════════════════════════════════════════════════════ */
struct Arrangement {
    bool kick, openHat, ride, clap, bellHi, bellLo, bass;
};

static Arrangement GetArrangement(uint32_t bar)
{
    Arrangement a = {false,false,false,false,false,false,false};

    /* Tras el build-up inicial (32 compases), entra en loop de 32 */
    uint32_t b = (bar < 32) ? bar : (32 + ((bar - 32) % 32));

    /* ── Build-up ── */
    a.kick    = (b >= 0);
    a.ride    = (b >= 4);                 /* ride entra en fade            */
    a.openHat = (b >= 8);
    a.bellHi  = (b >= 12);                /* bell aguda: a partir de aquí  */
    a.clap    = (b >= 16);
    a.bellLo  = (b >= 16);
    a.bass    = (b >= 24);                /* bassline entra tarde          */

    /* ── Truco Mills: en el loop, la bell grave entra y sale cada 4
     *    compases; el kick se "abre" quitando hi-hat puntualmente ── */
    if(bar >= 32){
        uint32_t loopBar = (bar - 32) % 32;
        a.bellLo  = (loopBar % 8) < 4;    /* grave aparece/desaparece      */
        a.openHat = (loopBar % 16) < 12;  /* breakdown de hats             */
        if((loopBar % 16) >= 12) a.bass = true;  /* bass solo en breakdown */
    }
    return a;
}

static Arrangement curArr = {true,false,false,false,false,false,false};
static float ledLevel = 0.0f;

/* Ride fade-in: gana volumen durante los primeros compases tras entrar */
static float rideGain = 0.0f;

static void SequencerTick()
{
    /* Al inicio de cada compás, recalcular arreglo y aplicar mutes 909 */
    if(step16 == 0){
        curArr = GetArrangement(barCounter);

        drums.SetMute(TR909::INST_KICK,    !curArr.kick);
        drums.SetMute(TR909::INST_HIHAT_O, !curArr.openHat);
        drums.SetMute(TR909::INST_RIDE,    !curArr.ride);
        drums.SetMute(TR909::INST_CLAP,    !curArr.clap);
    }

    const float vAcc = 1.0f;
    const float vReg = 0.7f;

    /* ── KICK 4-on-the-floor (distorsionado al máximo) ── */
    if(curArr.kick && (step16 % 4) == 0)
        drums.Trigger(TR909::INST_KICK, 1.0f);

    /* ── OPEN HAT en los offbeats (decay abierto → se derrama) ── */
    if(curArr.openHat && (step16 % 4) == 2)
        drums.Trigger(TR909::INST_HIHAT_O, 0.7f);

    /* ── RIDE en corcheas, con fade-in ── */
    if(curArr.ride && (step16 % 2) == 0){
        rideGain += (1.0f - rideGain) * 0.02f;   /* sube lento → fade     */
        drums.SetVolume(TR909::INST_RIDE, 0.55f * rideGain);
        drums.Trigger(TR909::INST_RIDE, 0.6f);
    }

    /* ── CLAP en el backbeat (2 y 4) → va al delay, enterrado ── */
    if(curArr.clap && (step16 == 4 || step16 == 12))
        drums.Trigger(TR909::INST_CLAP, 0.85f);

    /* ── BELLS ── alta siempre ON; baja se mutea/desmutea (truco Mills) ── */
    if(curArr.bellHi){
        uint8_t hi = kBellHi[step16];
        if(hi != 0){
            float vel = ((step16 % 4) == 0) ? 0.9f : 0.65f;
            BellNoteOn(hi, vel);
        }
    }
    if(curArr.bellLo){
        uint8_t lo = kBellLo[step16];
        if(lo != 0)
            BellNoteOn(lo, 0.7f);
    }

    /* ── BASS 303: 16 corcheas con ataque chuffy ── */
    if(curArr.bass){
        uint8_t note = kBassNotes[step16];
        bool    acc  = kBassAccent[step16] != 0;
        bool    slide = (step16 % 4) == 3;   /* slides ocasionales         */
        bass.NoteOn(note, acc, slide);
    }

    ledLevel = (step16 % 4 == 0) ? 1.0f : 0.4f;

    /* Avance del step */
    step16++;
    if(step16 >= 16){
        step16 = 0;
        barCounter++;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  AUDIO CALLBACK
 * ═══════════════════════════════════════════════════════════════════ */
void AudioCallback(AudioHandle::InputBuffer  /*in*/,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    for(size_t i = 0; i < size; i++)
    {
        if(stepCounter == 0) SequencerTick();
        stepCounter++;
        if(stepCounter >= kStepSamples) stepCounter = 0;

        /* ── 909 drums (kick lleva su propio drive/limiter interno) ── */
        float drumMix = drums.Process();

        /* ── Delay (lee retrasado, realimenta una fracción). Alimentamos
         *    una porción del mix de percusión → el clap del backbeat queda
         *    "enterrado" en ecos, como en el original. ── */
        float dlyOut = clapDlyBuf[clapDlyWp];
        /* Inyectamos una porción del mix de percusión (queda "enterrado") */
        clapDlyBuf[clapDlyWp] = drumMix * 0.18f + dlyOut * clapDlyFb;
        clapDlyWp = (clapDlyWp + 1) % CLAP_DLY_SIZE;

        /* ── Bells (suma de voces) ── */
        float bellMix = 0.0f;
        for(int v = 0; v < NUM_BELLS; v++)
            bellMix += bells[v].Process();
        bellMix *= 0.5f;

        /* ── Bass 303 ── */
        float bassMix = bass.Process() * 0.9f;

        /* ── Mezcla seca ── */
        float dry = drumMix * 0.9f + bellMix + bassMix + dlyOut * 0.5f;

        /* ── Reverb estéreo (sobre todo para las bells) ── */
        float wetL, wetR;
        reverb.Process(bellMix + dlyOut * 0.4f, bellMix + dlyOut * 0.4f,
                       &wetL, &wetR);

        float outL = dry + wetL * 0.45f;
        float outR = dry + wetR * 0.45f;

        /* Soft clip de seguridad */
        outL = tanhf(outL * 0.7f);
        outR = tanhf(outR * 0.7f);

        out[0][i] = outL;
        out[1][i] = outR;
    }

    /* LED al pulso */
    ledLevel *= 0.85f;
    hw.SetLed(ledLevel > 0.2f);
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main()
{
    /* FPU Flush-to-Zero + Default-NaN (evita denormales en la cola reverb) */
    __asm volatile("VMRS r0, FPSCR\n"
                   "ORR  r0, r0, #(1<<24)|(1<<25)\n"
                   "VMSR FPSCR, r0" ::: "r0");
    *(volatile uint32_t*)0xE000EF3Cu |= (1u << 24) | (1u << 25);

    hw.Init();
    hw.SetAudioBlockSize(AUDIO_BLOCK);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    /* ── 909: kit techno, kick distorsionado al máximo ── */
    drums.Init(SAMPLE_RATE);
    drums.kick.SetDrive(1.0f);        /* clipeo extremo — sonido Mills     */
    drums.kick.SetDecay(0.55f);
    drums.kick.SetCompression(1.0f);  /* exagera el noise floor            */
    drums.hihatO.SetDecay(1.6f);      /* decay abierto → se derrama        */
    drums.SetVolume(TR909::INST_KICK,    1.3f);
    drums.SetVolume(TR909::INST_HIHAT_O, 0.6f);
    drums.SetVolume(TR909::INST_CLAP,    0.5f);   /* enterrado en el mix    */
    drums.SetMasterVolume(0.9f);

    /* ── Bells FM ── */
    for(int v = 0; v < NUM_BELLS; v++){
        bells[v].Init(SAMPLE_RATE);
        ApplyBellPreset(bells[v]);
    }

    /* ── 303 bass chuffy ── */
    bass.Init(SAMPLE_RATE);
    bass.SetWaveform(TB303::WAVE_SAW);
    bass.SetCutoff(420.0f);
    bass.SetResonance(0.82f);
    bass.SetEnvMod(0.8f);
    bass.SetDecay(0.12f);             /* corto → ataque "chuffy"/hi-hat-ish */
    bass.SetAccent(0.85f);
    bass.SetOverdrive(0.5f);
    bass.SetVolume(0.8f);

    /* ── Reverb: cola larga, agudos amortiguados ── */
    reverb.Init(SAMPLE_RATE);
    reverb.SetFeedback(0.82f);
    reverb.SetLpFreq(8500.0f);

    /* Limpiar buffer del delay */
    for(size_t i = 0; i < CLAP_DLY_SIZE; i++) clapDlyBuf[i] = 0.0f;

    hw.StartAudio(AudioCallback);

    while(1) System::Delay(100);
}

/* ── Fault handler: SOS para diferenciar de cuelgues silenciosos ── */
static void FaultDelay(uint32_t ms)
{
    volatile uint32_t cycles = ms * 240000u;
    while(cycles--) __asm volatile("nop");
}
static void FaultSosLoop(void)
{
    __disable_irq();
    while(1)
    {
        for(int i = 0; i < 3; i++){ hw.SetLed(true); FaultDelay(120); hw.SetLed(false); FaultDelay(120); }
        FaultDelay(250);
        for(int i = 0; i < 3; i++){ hw.SetLed(true); FaultDelay(450); hw.SetLed(false); FaultDelay(150); }
        FaultDelay(250);
        for(int i = 0; i < 3; i++){ hw.SetLed(true); FaultDelay(120); hw.SetLed(false); FaultDelay(120); }
        FaultDelay(1500);
    }
}
extern "C" void HardFault_Handler(void) { FaultSosLoop(); }
extern "C" void MemManage_Handler(void) { FaultSosLoop(); }
extern "C" void BusFault_Handler(void)  { FaultSosLoop(); }
extern "C" void UsageFault_Handler(void){ FaultSosLoop(); }
