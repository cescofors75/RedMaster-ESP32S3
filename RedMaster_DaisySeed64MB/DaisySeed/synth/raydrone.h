/* ═══════════════════════════════════════════════════════════════════
 *  raydrone.h  --  RayDrone granular render engine  (v1.0, Daisy port)
 * ─────────────────────────────────────────────────────────────────
 *  Port del motor RayDrone (Rust/WASM, cescofors75/RayDrone) al
 *  firmware RED808 de la Daisy Seed, ampliado para drones y shimmer
 *  de gama alta.
 *
 *  METODOLOGIA (identica al WASM):
 *    El sonido no se sintetiza desde un oscilador: se *renderiza*.
 *    Se lanzan N rayos por segundo hacia un punto de foco dentro de
 *    una fuente (sample en SDRAM o fuente armonica interna). Cada
 *    rayo lee un grano con dispersion triangular alrededor del foco
 *    (la "profundidad de campo" optica). La convergencia estadistica
 *    de la nube ES el drone: no existe en la fuente, emerge.
 *
 *      apertura estrecha  → lectura casi tonal, el sample se reconoce
 *      apertura amplia    → desenfoque profundo, textura atmosferica
 *
 *  QUE APORTA ESTE PORT SOBRE EL MOTOR WASM ORIGINAL:
 *    1. MATERIALES REALES. El WASM aplica un one-pole por grano
 *       (metal/madera/cristal/agua/plasma). Aqui ese shaper por grano
 *       se conserva, pero ademas la nube pasa por un BANCO MODAL de
 *       8 modos estereo afinado a la nota, con ratios y Q propios de
 *       cada material (barra metalica, membrana Bessel, barra de
 *       madera tipo marimba, cuerda armonica, cristal/hielo de Q
 *       altisimo...). El material deja de ser un color y pasa a ser
 *       la resonancia del cuerpo que suena.
 *    2. SHIMMER DE VERDAD. El WASM solo tiene OCT = probabilidad de
 *       que un grano lea a 2x. Eso es una capa de octava, no shimmer.
 *       Aqui hay un lazo de difusion (4 allpass + 2 comb por canal)
 *       con DOS pitch-shifters (+12 y +19) DENTRO de la
 *       realimentacion: la octava se re-inyecta y se acumula, y la
 *       cola asciende sola. El OCT por grano se mantiene como capa
 *       extra.
 *    3. VARIACION. Cuatro LFOs a frecuencias mutuamente irracionales
 *       (nunca se repite el patron) + un motor de mutacion que cada
 *       cierto tiempo re-tira un parametro dentro de limites y hace
 *       glide hasta el nuevo valor. El drone nunca se queda quieto.
 *    4. VENTANA MORFABLE. Tres tablas (percusiva / Hann / meseta) con
 *       crossfade continuo: mismo cloud, texturas radicalmente
 *       distintas.
 *    5. FUENTE INTERNA. Si no hay sample cargado el motor genera su
 *       propia fuente armonica afinada (A1, 12 parciales con deriva
 *       de fase), asi el engine suena siempre y suena afinado.
 *
 *  SE CONSERVA DEL ORIGINAL:
 *    - Dispersion triangular inversa tri_inv() sobre el foco.
 *    - Muestreo QMC: aureo (baja discrepancia), estratificado, random.
 *    - Constelacion de focos RECURSIVA (modo ambient): semillas
 *      inmortales que engendran hijos auto-similares con offset, peso
 *      y vida que encogen por nivel. Es la "autoevaluacion con
 *      recursividad" del motor original.
 *    - Rebotes de grano (un grano al morir puede relanzar otro desde
 *      su posicion final, con profundidad decreciente).
 *    - Voicings en entonacion justa por grano (bate mucho menos que
 *      el temperamento igual cuando solapan cientos de granos).
 *    - Aberracion cromatica: 3 bandas con apertura distinta.
 *    - Rayos inteligentes: rejection sampling ∝ energia de la fuente.
 *    - Autoevolucion del foco realimentada por la envolvente de salida.
 *
 *  Engine ID: 9  (SYNTH_ENGINE_RAYDRONE)
 *
 *  48 kHz  float32  C++14  header-only  sin asignacion dinamica
 * ═══════════════════════════════════════════════════════════════════ */
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef RAYDRONE_TWOPI
#define RAYDRONE_TWOPI 6.283185307179586f
#endif

/* Barrera de compilador. Cortex-M es de un solo nucleo: para ordenar
 * escrituras frente a una ISR basta con impedir que el compilador las
 * reordene, no hace falta una barrera de memoria de verdad. */
#define RAYDRONE_BARRIER() __atomic_signal_fence(__ATOMIC_SEQ_CST)

/* DSY_SDRAM_BSS solo existe compilando para la Daisy. Fuera (tests de
 * host) se define vacio para que el header siga siendo compilable. */
#ifndef DSY_SDRAM_BSS
#define RAYDRONE_LOCAL_SDRAM_DEF 1
#define DSY_SDRAM_BSS
#endif

namespace RayDrone {

/* ─────────────────────────────────────────────────────────────────
 *  Constantes de dimensionado
 * ───────────────────────────────────────────────────────────────── */
static constexpr int   kMaxGrains      = 96;    /* techo absoluto de voces granulares  */
static constexpr int   kMaxFoci        = 24;    /* nodos del arbol de focos            */
static constexpr int   kWindowSize     = 1024;  /* resolucion de las tablas de ventana */
static constexpr int   kScaleCap       = 12;    /* grados de voicing simultaneos       */
static constexpr int   kEnergyBins     = 256;   /* mapa de energia para rayos smart    */
static constexpr int   kModes          = 8;     /* modos del banco material            */
/* Trim del banco modal: con el se ajusta el nivel del camino con
 * material al del camino seco, para que cambiar de material no sea un
 * salto de volumen. */
static constexpr float kBankGain       = 0.112f;
/* Ganancia del camino seco. Calibrada junto a kBankGain para que activar
 * un material cambie el TIMBRE y no el volumen. */
static constexpr float kDryGain        = 3.20f;
/* Anti-denormal. En cuanto la nube deja un hueco, los modos de Q alto y
 * los combs del espacio caen al rango subnormal; ahi el FPU deja de
 * resolver en hardware y el coste por muestra se multiplica por diez —
 * medido: 6,3 us/muestra frente a 0,49 con flush-to-zero. Inyectar una
 * senal minuscula de signo alterno mantiene los estados en rango normal
 * y esta 360 dB por debajo de la senal, asi que no se oye. Se hace en el
 * DSP a proposito: no depende de que el FPU tenga FZ activado. */
static constexpr float kAntiDenormal   = 1.0e-18f;
static constexpr int   kNumAllpass     = 4;     /* difusion por canal                  */
static constexpr int   kNumComb        = 2;     /* cola por canal                      */

/* Longitud de la fuente armonica interna: 2 s @48k. */
static constexpr uint32_t kInternalLen = 96000;

/* Lineas de retardo del lazo de shimmer/difusion (en samples @48k). */
static constexpr int kApLen[kNumAllpass]   = { 331, 449, 613, 787 };
static constexpr int kCombLen[kNumComb]    = { 2111, 2683 };
static constexpr int kApSpread[kNumAllpass]= {  23,  31,  17,  41 }; /* offset canal R */
static constexpr int kCombSpread[kNumComb] = {  67,  89 };
static constexpr int kShiftLen             = 4096; /* ventana del pitch shifter        */

/* ─────────────────────────────────────────────────────────────────
 *  Utilidades numericas
 * ───────────────────────────────────────────────────────────────── */

/* Propia, no M_PI: math.h no la garantiza en modo estricto ANSI. */
static constexpr float kPi = 3.14159265358979f;

static inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float Sanitize(float v) {
    return isfinite(v) ? v : 0.0f;
}

/** Aplasta a cero lo que ya no es audible.
 *
 *  Los filtros de un polo del motor (el `tone` del material y el `lp` de
 *  la aberracion, uno de cada POR GRANO) decaen exponencialmente en los
 *  huecos de la nube y acaban en el rango subnormal. Ahi el FPU deja de
 *  resolver la operacion en hardware y el coste se dispara: medido sobre
 *  este motor, 6,2 us por muestra frente a 0,49 con flush-to-zero, es
 *  decir 12x, y creciendo con el numero de granos vivos. El umbral esta
 *  500 dB por debajo de fondo de escala, asi que no se puede oir.
 *
 *  El firmware ya fuerza FZ+DN en el FPSCR al entrar en el callback de
 *  audio, asi que en la Daisy esto es cinturon y tirantes; se hace igual
 *  en el DSP para que el motor rinda lo mismo en un host sin flush (y
 *  para que medirlo fuera del firmware de un numero honesto). */
static constexpr float kFlushFloor = 1.0e-25f;
static inline float Flush(float v) {
    return (v < kFlushFloor && v > -kFlushFloor) ? 0.0f : v;
}

/** Soft-clip cubico (misma curva que el core Rust: satura a ±2/3). */
static inline float Soft(float x) {
    if(x >  1.0f) return  0.6666667f;
    if(x < -1.0f) return -0.6666667f;
    return x - x * x * x * (1.0f / 3.0f);
}

/** CDF inversa de una triangular simetrica en [-1,1].
 *  Es la dispersion de granos alrededor del foco: densidad maxima en
 *  el foco y caida lineal hacia los bordes = "profundidad de campo".
 *  Misma forma que tri_inv() del core Rust. */
static inline float TriInv(float u) {
    if(u < 0.5f) return -1.0f + sqrtf(2.0f * u);
    return 1.0f - sqrtf(2.0f * (1.0f - u));
}

/** xorshift32: el mismo PRNG que el motor WASM, reproducible por semilla. */
static inline uint32_t Xorshift32(uint32_t& s) {
    uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
}

/** Uniforme en [0,1). */
static inline float Rng01(uint32_t& s) {
    return (float)Xorshift32(s) * (1.0f / 4294967296.0f);
}

/** 2^(semitonos/12). Solo se llama al nacer un grano (unas decenas por
 *  segundo), nunca por muestra, asi que el powf no molesta aqui. */
static inline float SemitoneRatio(float semitones) {
    return powf(2.0f, semitones * (1.0f / 12.0f));
}

/* ─────────────────────────────────────────────────────────────────
 *  Voicings en entonacion justa
 * ─────────────────────────────────────────────────────────────────
 *  Un drone granulado de una sola fuente no tiene armonia: todos los
 *  granos leen la misma altura. Dando a cada rayo un ratio de una
 *  voicing consonante, la nube se apila en un acorde afinado.
 *  Los ratios son JUSTOS (fracciones de enteros pequenos) a proposito:
 *  baten muchisimo menos que el temperamento igual cuando solapan
 *  decenas de granos, que es justo lo que hace que un drone sostenido
 *  suene puro en vez de turbio.
 * ───────────────────────────────────────────────────────────────── */
enum Chord : uint8_t {
    CHORD_UNISON = 0,
    CHORD_OCTAVES,
    CHORD_POWER,       /* sub · root · quinta · octava · octava+quinta */
    CHORD_MAJOR,
    CHORD_MINOR,
    CHORD_FIFTHS,
    CHORD_SUS2,
    CHORD_PENTATONIC,
    CHORD_MAJOR_SCALE,
    CHORD_MINOR_SCALE,
    CHORD_COUNT
};

/** Rellena `out` con los ratios de la voicing. Devuelve cuantos. */
static inline uint8_t ChordRatios(uint8_t preset, float* out) {
    switch(preset)
    {
        case CHORD_OCTAVES: {
            const float r[] = { 0.5f, 1.0f, 2.0f };
            for(int i = 0; i < 3; ++i) out[i] = r[i];
            return 3;
        }
        case CHORD_POWER: {
            const float r[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
            for(int i = 0; i < 5; ++i) out[i] = r[i];
            return 5;
        }
        case CHORD_MAJOR: {
            const float r[] = { 1.0f, 5.0f/4.0f, 3.0f/2.0f, 2.0f };
            for(int i = 0; i < 4; ++i) out[i] = r[i];
            return 4;
        }
        case CHORD_MINOR: {
            const float r[] = { 1.0f, 6.0f/5.0f, 3.0f/2.0f, 2.0f };
            for(int i = 0; i < 4; ++i) out[i] = r[i];
            return 4;
        }
        case CHORD_FIFTHS: {
            const float r[] = { 1.0f, 3.0f/2.0f, 9.0f/4.0f, 3.0f };
            for(int i = 0; i < 4; ++i) out[i] = r[i];
            return 4;
        }
        case CHORD_SUS2: {
            const float r[] = { 1.0f, 9.0f/8.0f, 3.0f/2.0f, 2.0f };
            for(int i = 0; i < 4; ++i) out[i] = r[i];
            return 4;
        }
        case CHORD_PENTATONIC: {
            const float r[] = { 1.0f, 9.0f/8.0f, 5.0f/4.0f, 3.0f/2.0f, 5.0f/3.0f, 2.0f };
            for(int i = 0; i < 6; ++i) out[i] = r[i];
            return 6;
        }
        case CHORD_MAJOR_SCALE: {
            const float r[] = { 1.0f, 9.0f/8.0f, 5.0f/4.0f, 4.0f/3.0f,
                                3.0f/2.0f, 5.0f/3.0f, 15.0f/8.0f, 2.0f };
            for(int i = 0; i < 8; ++i) out[i] = r[i];
            return 8;
        }
        case CHORD_MINOR_SCALE: {
            const float r[] = { 1.0f, 9.0f/8.0f, 6.0f/5.0f, 4.0f/3.0f,
                                3.0f/2.0f, 8.0f/5.0f, 9.0f/5.0f, 2.0f };
            for(int i = 0; i < 8; ++i) out[i] = r[i];
            return 8;
        }
        default:
            out[0] = 1.0f;
            return 1;
    }
}

/* ─────────────────────────────────────────────────────────────────
 *  Materiales
 * ─────────────────────────────────────────────────────────────────
 *  Cada material define (a) un shaper barato por grano — heredado del
 *  motor WASM — y (b) un banco modal de 8 resonadores afinado a la
 *  nota. Los ratios modales son los del cuerpo fisico real:
 *
 *    METAL   barra libre-libre: 1, 2.76, 5.40, 8.93...  (inarmonico)
 *    GLASS   copa/cristal: casi la barra pero con Q mucho mas alto
 *    WOOD    barra de marimba afinada: 1, 3.93, 10.9... decae rapido
 *    WATER   cuasi-armonico con deriva lenta de los parciales altos
 *    PLASMA  ratios que caminan aleatoriamente + no linealidad
 *    STONE   denso y oscuro, Q bajo, se apaga enseguida
 *    SKIN    membrana circular (ceros de Bessel): 1, 1.59, 2.14, 2.30
 *    STRING  armonico puro 1..8, Q alto → cuerda frotada
 *    ICE     inarmonico muy agudo y Q extremo → brillo cristalino
 * ───────────────────────────────────────────────────────────────── */
enum Material : uint8_t {
    MAT_NONE = 0,
    MAT_METAL,
    MAT_WOOD,
    MAT_GLASS,
    MAT_WATER,
    MAT_PLASMA,
    MAT_STONE,
    MAT_SKIN,
    MAT_STRING,
    MAT_ICE,
    MAT_COUNT
};

struct MaterialSpec {
    float ratio[kModes];   /* parciales relativos a la fundamental        */
    /* Peso de cada modo, con la inclinacion espectral YA aplicada. Era
     * amp[m] * ratio[m]^(-1.35 + bright*1.15), calculado en cada
     * UpdateModes() — es decir ocho powf por note-on. Como solo depende
     * de constantes del material, se hornea aqui: se ahorra el powf y,
     * sobre todo, permite que el motor Rust (no_std, sin pow) use
     * exactamente los mismos numeros. */
    float weight[kModes];
    float q;               /* Q base (mayor = cola mas larga)             */
    float absorb;          /* absorcion de la camara de rayos             */
};

static inline const MaterialSpec& MaterialTable(uint8_t m) {
    /* Tablas estaticas: se resuelven en flash, no cuestan RAM. */
    static const MaterialSpec kSpecs[MAT_COUNT] = {
        /* MAT_NONE — armonico plano, banco practicamente transparente */
        { { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f },
          { 1.000000f, 0.292194f, 0.140846f, 0.085378f, 0.057455f, 0.039907f, 0.030987f, 0.023949f },
          22.0f, 0.16f },
        /* MAT_METAL — barra libre-libre */
        { { 1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.82f, 31.87f },
          { 1.000000f, 0.568687f, 0.369827f, 0.244954f, 0.164842f, 0.111306f, 0.075281f, 0.048722f },
          120.0f, 0.3f },
        /* MAT_WOOD — barra de marimba afinada, decae deprisa */
        { { 1.0f, 3.93f, 10.88f, 21.3f, 34.8f, 51.6f, 71.5f, 94.6f },
          { 1.000000f, 0.123804f, 0.022300f, 0.005854f, 0.001994f, 0.000911f, 0.000333f, 0.000127f },
          34.0f, 0.055f },
        /* MAT_GLASS — copa: casi barra pero Q altisimo */
        { { 1.0f, 2.71f, 5.15f, 8.43f, 12.5f, 17.4f, 23.1f, 29.6f },
          { 1.000000f, 0.553101f, 0.384188f, 0.279037f, 0.210453f, 0.156337f, 0.115937f, 0.081810f },
          210.0f, 0.2f },
        /* MAT_WATER — cuasi-armonico, parciales altos a la deriva */
        { { 1.0f, 2.0f, 3.0f, 4.2f, 5.4f, 6.9f, 8.3f, 10.1f },
          { 1.000000f, 0.334561f, 0.165506f, 0.094795f, 0.055733f, 0.032262f, 0.019768f, 0.011492f },
          48.0f, 0.11f },
        /* MAT_PLASMA — inarmonico irregular + no linealidad fuerte */
        { { 1.0f, 2.31f, 3.87f, 6.13f, 9.02f, 12.71f, 17.33f, 23.02f },
          { 1.000000f, 0.452160f, 0.286182f, 0.178527f, 0.114205f, 0.074267f, 0.047377f, 0.029179f },
          64.0f, 0.38f },
        /* MAT_STONE — denso, oscuro, Q bajo */
        { { 1.0f, 2.1f, 3.4f, 4.9f, 6.8f, 9.1f, 11.6f, 14.5f },
          { 1.000000f, 0.243718f, 0.083583f, 0.033236f, 0.013432f, 0.006209f, 0.002719f, 0.001064f },
          18.0f, 0.16f },
        /* MAT_SKIN — membrana circular (ceros de Bessel) */
        { { 1.0f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f, 3.155f, 3.5f },
          { 1.000000f, 0.433731f, 0.238442f, 0.204342f, 0.139374f, 0.099808f, 0.073661f, 0.049656f },
          28.0f, 0.13f },
        /* MAT_STRING — armonico puro, Q alto */
        { { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f },
          { 1.000000f, 0.504551f, 0.303005f, 0.198333f, 0.135200f, 0.093643f, 0.066492f, 0.046816f },
          150.0f, 0.09f },
        /* MAT_ICE — inarmonico agudo, Q extremo */
        { { 1.0f, 3.01f, 6.02f, 9.44f, 14.1f, 19.8f, 26.3f, 33.9f },
          { 1.000000f, 0.664163f, 0.498882f, 0.389864f, 0.301090f, 0.228397f, 0.171879f, 0.122554f },
          260.0f, 0.24f },
    };
    return kSpecs[m < MAT_COUNT ? m : 0];
}

/* ─────────────────────────────────────────────────────────────────
 *  Grano (rayo)
 * ───────────────────────────────────────────────────────────────── */
struct Grain {
    float    pos;      /* posicion de lectura en la fuente (samples)  */
    float    step;     /* velocidad de lectura (octava·voicing·detune)*/
    float    age;      /* edad en samples                            */
    float    invDur;   /* 1/duracion → fase de ventana = age·invDur   */
    float    gain;
    float    lp;       /* estado del filtro de banda (aberracion)     */
    float    tone;     /* estado del shaper de material               */
    float    panL, panR;
    uint8_t  band;     /* 0=graves 1=medios 2=agudos                  */
    uint8_t  depth;    /* rebotes restantes                          */
    bool     active;
};

/* Un modo del banco material: resonador de 2 polos (forma directa). */
struct Mode {
    float a1, a2, b0;
    float y1L, y2L, y1R, y2R;
    void Reset() { y1L = y2L = y1R = y2R = 0.0f; }
};

/* ═════════════════════════════════════════════════════════════════
 *  Engine
 * ─────────────────────────────────────────────────────────────────
 *  OJO — ningun miembro lleva inicializador por defecto, y es a
 *  proposito. La instancia son ~334 KB y tiene que vivir en SDRAM
 *  (DSY_SDRAM_BSS). Si la clase tuviera inicializadores de miembro,
 *  el compilador generaria un constructor que corre en
 *  __libc_init_array, es decir ANTES de que hw.Init() levante el
 *  controlador de SDRAM: escribirian sobre memoria que todavia no
 *  existe. Sin inicializadores la clase es trivialmente construible,
 *  se queda en .bss y no se toca nada hasta que Init() se llama a
 *  mano, ya con la SDRAM en pie.
 *
 *  Consecuencia: Init() DEBE dejar fijado todo el estado, porque la
 *  SDRAM de la Daisy tampoco se pone a cero al arrancar. El banco de
 *  pruebas de host lo comprueba llenando el objeto de basura antes de
 *  llamar a Init().
 * ═════════════════════════════════════════════════════════════════ */
class Engine
{
  public:
    /* ── Ciclo de vida ─────────────────────────────────────────── */

    void Init(float sampleRate)
    {
        sr_       = (sampleRate > 1000.0f) ? sampleRate : 48000.0f;
        invSr_    = 1.0f / sr_;
        rng_      = 0x1234'5678u;
        seed_     = 0x1234'5678u;

        BuildWindows();
        BuildInternalSource();

        /* Fuente por defecto: la armonica interna, para que el engine
         * suene aunque no haya ningun sample cargado en el pool. */
        srcData_ = internalSource_;
        srcLen_  = kInternalLen;
        srcSr_   = sr_;
        srcIsInternal_ = true;
        BuildEnergyMap(srcData_, srcLen_);

        /* Defaults: un drone de pad ancho y lento, listo para sonar. */
        density_      = 90.0f;
        grainDur_     = 0.42f;
        focus_        = 0.35f;
        aperture_     = 0.30f;
        sampleMode_   = 1;      /* aureo: la mejor convergencia       */
        chord_        = CHORD_POWER;
        pitchSemi_    = 0.0f;
        octProb_      = 0.18f;
        width_        = 0.85f;
        material_     = MAT_GLASS;
        materialAmt_  = 0.55f;
        shimmerAmt_   = 0.45f;
        shimmerTilt_  = 0.30f;  /* 0 = solo +12, 1 = solo +19         */
        spaceWet_     = 0.55f;
        spaceSize_    = 0.72f;
        damping_      = 0.42f;
        variation_    = 0.35f;
        varRate_      = 0.30f;
        fociDepth_    = 2;
        fociSeeds_    = 3;
        fociDrift_    = 0.30f;
        fociSpread_   = 0.25f;
        bounceDepth_  = 1;
        bounceProb_   = 0.35f;
        windowShape_  = 0.50f;
        aberration_   = 0.22f;
        drive_        = 0.10f;
        smartRays_    = 1;
        feedback_     = 0.30f;
        cutoff_       = 9000.0f;
        resonance_    = 0.15f;
        volume_       = 0.80f;

        noteRatio_    = 1.0f;
        noteHz_        = 55.0f;
        noteVel_       = 1.0f;
        active_        = false;
        grainCap_      = 48;   /* techo prudente; main.cpp lo ajusta por CPU */
        modeNorm_      = 0.0f;
        scaleLen_      = 1;
        svF_ = 0.5f; svQ_ = 1.0f; aLow_ = 0.06f; aHigh_ = 0.30f;

        Reset();
        RebuildChord();
        UpdateModes();
        UpdateFilter();
    }

    /** Limpia todo el estado sonoro. No toca los parametros. */
    void Reset()
    {
        for(int i = 0; i < kMaxGrains; ++i) grains_[i].active = false;
        nActive_ = 0;
        nFree_   = kMaxGrains;
        for(int i = 0; i < kMaxGrains; ++i) free_[i] = (uint16_t)(kMaxGrains - 1 - i);

        spawnAcc_ = 0.0f;
        qmcPos_   = 0.5f;
        qmcPitch_ = 0.5f;
        stratI_   = 0;
        scaleI_   = 0;
        env_      = 0.0f;
        evo_      = 0.5f;
        evoDir_   = 1.0f;
        fociAcc_  = 0.0f;
        fociDirty_= true;

        for(int i = 0; i < kMaxFoci; ++i) {
            foci_[i].active = false;
            foci_[i].w = foci_[i].wTarget = 0.0f;
        }

        memset(apL_, 0, sizeof(apL_));
        memset(apR_, 0, sizeof(apR_));
        memset(combL_, 0, sizeof(combL_));
        memset(combR_, 0, sizeof(combR_));
        memset(apIdx_, 0, sizeof(apIdx_));
        memset(combIdx_, 0, sizeof(combIdx_));
        memset(combLpL_, 0, sizeof(combLpL_));
        memset(combLpR_, 0, sizeof(combLpR_));
        memset(shiftBufL_, 0, sizeof(shiftBufL_));
        memset(shiftBufR_, 0, sizeof(shiftBufR_));
        shiftW_ = 0;
        shiftPhase12_ = 0.0f;
        shiftPhase19_ = 0.0f;

        for(int m = 0; m < kModes; ++m) modes_[m].Reset();

        denorm_ = kAntiDenormal;
        dcXL_ = dcYL_ = dcXR_ = dcYR_ = 0.0f;
        svLpL_ = svBpL_ = svLpR_ = svBpR_ = 0.0f;
        for(int i = 0; i < 4; ++i) lfoPhase_[i] = 0.25f * (float)i;
        mutateAcc_ = 0.0f;
        for(int i = 0; i < kNumMutTargets; ++i) { mutOff_[i] = 0.0f; mutTarget_[i] = 0.0f; }
        modDensity_ = modFocus_ = modAperture_ = 0.0f;
        modDur_ = modPitch_ = modMaterial_ = 0.0f;
        for(int i = 0; i < kMaxGrains; ++i) {
            activeIdx_[i] = 0;
            grains_[i].pos = grains_[i].step = grains_[i].age = 0.0f;
            grains_[i].invDur = 1.0f; grains_[i].gain = 0.0f;
            grains_[i].lp = grains_[i].tone = 0.0f;
            grains_[i].panL = grains_[i].panR = 0.707f;
            grains_[i].band = 1; grains_[i].depth = 0;
        }
        for(int i = 0; i < kMaxFoci; ++i) {
            foci_[i].pos = foci_[i].vel = foci_[i].age = 0.0f;
            foci_[i].ttl = -1.0f; foci_[i].depth = 0;
        }
        rng_ = seed_;
    }

    /* ── Fuente ────────────────────────────────────────────────── */

    /** Apunta el motor a un sample del pool SDRAM.
     *  `data == nullptr` o `len < 4` vuelve a la fuente interna.
     *
     *  Se llama desde el hilo principal mientras la ISR de audio esta
     *  leyendo la fuente, asi que el cambio va ORDENADO: primero se
     *  anula la longitud (SampleAt corta en seco y devuelve silencio),
     *  despues se mueve el puntero y se recalcula el mapa de energia, y
     *  la longitud nueva se publica la ULTIMA. Con eso la ISR nunca ve
     *  un puntero nuevo con la longitud vieja, que es como se leeria
     *  fuera del sample. El mapa de energia (16 K lecturas) queda fuera
     *  de cualquier seccion critica: no se puede hacer con las
     *  interrupciones apagadas sin cortar el audio. */
    void SetSource(const int16_t* data, uint32_t len, float sourceSr)
    {
        const bool internal = (data == nullptr || len < 4);
        const int16_t* d = internal ? internalSource_ : data;
        const uint32_t n = internal ? kInternalLen : len;

        srcLen_ = 0;                       /* la ISR deja de leer ya */
        RAYDRONE_BARRIER();
        srcData_ = d;
        srcSr_   = internal ? sr_ : ((sourceSr > 1000.0f) ? sourceSr : sr_);
        srcIsInternal_ = internal;
        BuildEnergyMap(d, n);
        RAYDRONE_BARRIER();
        srcLen_ = n;                       /* ahora ya es coherente */
        fociDirty_ = true;
    }

    /** Fuente activa, para que el firmware pueda detectar que el pad que
     *  la alimenta se ha recargado y el puntero ha quedado obsoleto. */
    const int16_t* SourceData() const { return srcData_; }
    uint32_t       SourceLen()  const { return srcLen_; }

    void UseInternalSource() { SetSource(nullptr, 0, sr_); }
    bool UsingInternalSource() const { return srcIsInternal_; }

    /* ── Nota ──────────────────────────────────────────────────── */

    /** Arranca (o re-afina) el drone. La nota fija tanto la
     *  transposicion de lectura como la fundamental del banco modal. */
    void NoteOn(uint8_t midiNote, float velocity)
    {
        noteHz_ = 440.0f * powf(2.0f, ((float)midiNote - 69.0f) / 12.0f);
        /* Fuente interna = A1 (55 Hz). Sample externo = C4 neutro. */
        const float refHz = srcIsInternal_ ? 55.0f : 261.6256f;
        noteRatio_ = noteHz_ / refHz;
        noteVel_   = Clamp(velocity, 0.05f, 1.0f);
        if(!active_) { fociDirty_ = true; }
        active_ = true;
        UpdateModes();
    }

    /** El drone se sostiene: NoteOff solo deja de sembrar granos, la
     *  nube y la cola de shimmer se apagan solas. */
    void NoteOff() { active_ = false; }

    bool IsActive() const
    {
        return active_ || nActive_ > 0 || env_ > 0.0002f;
    }

    /* ── Bloque: llamar una vez por bloque de audio ────────────── */

    /** Avanza los procesos lentos (constelacion de focos, LFOs de
     *  variacion, mutacion). `frames` = tamano del bloque. */
    void Tick(int frames)
    {
        const float dt = (float)frames * invSr_;
        UpdateFoci(dt);
        UpdateVariation(dt);
    }

    /* ── Sample: el bucle caliente ─────────────────────────────── */

    void Process(float& outL, float& outR)
    {
        /* 1 · Sembrado de rayos ─────────────────────────────────── */
        if(active_ && srcLen_ >= 4) {
            const float dens = Clamp(density_ * (1.0f + modDensity_), 1.0f, 800.0f);
            spawnAcc_ += dens * invSr_;
            int guard = 0;
            while(spawnAcc_ >= 1.0f && guard < 8) {
                Spawn();
                spawnAcc_ -= 1.0f;
                ++guard;
            }
            if(spawnAcc_ > 8.0f) spawnAcc_ = 0.0f; /* densidad absurda: no acumular */
        }

        /* 2 · Nube: solo se recorren los granos vivos ───────────── */
        float accL = 0.0f, accR = 0.0f;
        int k = 0;
        while(k < nActive_) {
            const int i = activeIdx_[k];
            Grain& g = grains_[i];
            const float ph = g.age * g.invDur;
            if(ph >= 1.0f) {
                /* El grano muere. Puede rebotar: relanza otro desde su
                 * posicion final con un salto pequeno y depth-1. Es la
                 * recursion "de rayo" del motor original. */
                const uint8_t dep = g.depth;
                const uint8_t bnd = g.band;
                const float   end = g.pos;
                g.active = false;
                --nActive_;
                activeIdx_[k] = activeIdx_[nActive_];
                free_[nFree_++] = (uint16_t)i;
                if(dep > 0 && Rng01(rng_) < bounceProb_) {
                    const float jitter = (Rng01(rng_) - 0.5f) * 0.12f * srcSr_;
                    Place(Clamp(end + jitter, 0.0f, MaxPos()), bnd, (uint8_t)(dep - 1));
                }
                continue; /* activeIdx_[k] es otro grano: no avanzar k */
            }

            const float raw = SampleAt(g.pos);
            const float s   = MaterialShaper(g, BandFilter(g, raw)) * WindowAt(ph) * g.gain;
            accL += s * g.panL;
            accR += s * g.panR;
            g.pos += g.step;
            g.age += 1.0f;
            ++k;
        }

        /* Inyeccion anti-denormal: alterna de signo cada muestra y se
         * propaga por banco modal, filtro y red de espacio, que son las
         * partes con memoria larga. */
        denorm_ = -denorm_;
        accL += denorm_;
        accR -= denorm_;

        /* 3 · Banco modal: el cuerpo del material ───────────────── */
        float mL = accL, mR = accR;
        if(materialAmt_ > 0.001f && material_ != MAT_NONE) {
            float rL = 0.0f, rR = 0.0f;
            for(int m = 0; m < kModes; ++m) {
                Mode& md = modes_[m];
                const float yL = Flush(md.b0 * accL + md.a1 * md.y1L + md.a2 * md.y2L);
                md.y2L = md.y1L; md.y1L = yL;
                const float yR = Flush(md.b0 * accR + md.a1 * md.y1R + md.a2 * md.y2R);
                md.y2R = md.y1R; md.y1R = yR;
                rL += yL * modeAmp_[m];
                rR += yR * modeAmp_[m];
            }
            /* El banco se SUMA al directo, no lo sustituye: el material
             * anade cuerpo resonante en vez de convertirse en un filtro
             * que apaga la nube. El directo solo cede un 30 % para hacer
             * sitio. */
            const float amt = materialAmt_;
            mL = accL * (1.0f - 0.30f * amt) + Soft(rL * modeNorm_) * amt;
            mR = accR * (1.0f - 0.30f * amt) + Soft(rR * modeNorm_) * amt;
        }

        /* 4 · Filtro state-variable (color global) ──────────────── */
        float fL = mL, fR = mR;
        if(cutoff_ < 15500.0f || resonance_ > 0.02f) {
            svBpL_ = Flush(svBpL_ + svF_ * (mL - svLpL_ - svQ_ * svBpL_));
            svLpL_ = Flush(svLpL_ + svF_ * svBpL_);
            svBpR_ = Flush(svBpR_ + svF_ * (mR - svLpR_ - svQ_ * svBpR_));
            svLpR_ = Flush(svLpR_ + svF_ * svBpR_);
            fL = svLpL_;
            fR = svLpR_;
        }

        /* 5 · Espacio + shimmer ────────────────────────────────── */
        float sL = fL, sR = fR;
        if(spaceWet_ > 0.001f) Space(fL, fR, sL, sR);

        /* 6 · Drive + salida ───────────────────────────────────── */
        float oL = sL, oR = sR;
        if(drive_ > 0.001f) {
            const float d = 1.0f + drive_ * 7.0f;
            oL = sL + (Soft(sL * d) - sL) * drive_;
            oR = sR + (Soft(sR * d) - sR) * drive_;
        }

        /* DC blocker (~10 Hz): la nube acumula offset. */
        const float yL = Flush(oL - dcXL_ + 0.9986f * dcYL_);
        dcXL_ = oL; dcYL_ = yL;
        const float yR = Flush(oR - dcXR_ + 0.9986f * dcYR_);
        dcXR_ = oR; dcYR_ = yR;

        const float gv = volume_ * noteVel_;
        outL = Sanitize(Soft(yL * gv));
        outR = Sanitize(Soft(yR * gv));

        /* Envolvente lenta: alimenta la autoevolucion del foco. */
        const float a = fabsf(outL) + fabsf(outR);
        env_ += (a * 0.5f - env_) * 0.0005f;
        if(feedback_ > 0.0f) {
            evo_ += evoDir_ * feedback_ * (0.0000004f + env_ * 0.0000018f);
            if(evo_ >= 1.0f) { evo_ = 1.0f; evoDir_ = -1.0f; }
            if(evo_ <= 0.0f) { evo_ = 0.0f; evoDir_ =  1.0f; }
        }
    }

    /* ── Parametros ────────────────────────────────────────────── */

    /** IDs de CMD_SYNTH_PARAM (engine 9). Ver README/protocolo. */
    enum ParamId : uint8_t {
        P_DENSITY = 0,   /* granos/s          [1..600]      */
        P_GRAIN_DUR,     /* s                 [0.01..2.0]   */
        P_FOCUS,         /* posicion norm.    [0..1]        */
        P_APERTURE,      /* desenfoque        [0..1]        */
        P_SAMPLE_MODE,   /* 0=rnd 1=aureo 2=estratificado   */
        P_CHORD,         /* voicing           [0..9]        */
        P_PITCH,         /* semitonos         [-24..24]     */
        P_OCT_PROB,      /* granos a 2x       [0..1]        */
        P_WIDTH,         /* estereo           [0..1]        */
        P_MATERIAL,      /* material          [0..9]        */
        P_MATERIAL_AMT,  /* mezcla del banco  [0..1]        */
        P_SHIMMER,       /* shimmer en el lazo[0..1]        */
        P_SHIMMER_TILT,  /* 0=+12  1=+19      [0..1]        */
        P_SPACE,         /* wet de la camara  [0..1]        */
        P_SPACE_SIZE,    /* tamano/cola       [0..1]        */
        P_DAMPING,       /* tono de la cola   [0..1]        */
        P_VARIATION,     /* profundidad       [0..1]        */
        P_VAR_RATE,      /* velocidad         [0..1]        */
        P_FOCI_DEPTH,    /* recursion         [0..4]        */
        P_FOCI_SEEDS,    /* semillas          [1..8]        */
        P_FOCI_DRIFT,    /* deriva            [0..1]        */
        P_FOCI_SPREAD,   /* dispersion hijos  [0..1]        */
        P_BOUNCE_DEPTH,  /* rebotes de grano  [0..3]        */
        P_BOUNCE_PROB,   /* prob. de rebote   [0..1]        */
        P_WINDOW,        /* 0=perc 0.5=hann 1=meseta        */
        P_ABERRATION,    /* aberracion cromatica [0..1]     */
        P_DRIVE,         /* saturacion        [0..1]        */
        P_SMART,         /* rayos inteligentes 0/1          */
        P_FEEDBACK,      /* autoevolucion     [0..1]        */
        P_CUTOFF,        /* Hz                [50..16000]   */
        P_RESONANCE,     /* Q                 [0..0.95]     */
        P_VOLUME,        /* [0..1]                          */
        P_SEED,          /* semilla del PRNG (entero)       */
        P_COUNT
    };

    void SetParam(uint8_t id, float v)
    {
        if(!isfinite(v)) return;
        switch(id)
        {
            case P_DENSITY:      density_     = Clamp(v, 1.0f, 600.0f); break;
            case P_GRAIN_DUR:    grainDur_    = Clamp(v, 0.01f, 2.0f);  break;
            case P_FOCUS:        focus_       = Clamp(v, 0.0f, 1.0f);   break;
            case P_APERTURE:     aperture_    = Clamp(v, 0.0f, 1.0f);   break;
            case P_SAMPLE_MODE:  sampleMode_  = (uint8_t)Clamp(v, 0.0f, 2.0f); break;
            case P_CHORD:
                chord_ = (uint8_t)Clamp(v, 0.0f, (float)(CHORD_COUNT - 1));
                RebuildChord();
                break;
            case P_PITCH:        pitchSemi_   = Clamp(v, -24.0f, 24.0f); break;
            case P_OCT_PROB:     octProb_     = Clamp(v, 0.0f, 1.0f);   break;
            case P_WIDTH:        width_       = Clamp(v, 0.0f, 1.0f);   break;
            case P_MATERIAL:
                material_ = (uint8_t)Clamp(v, 0.0f, (float)(MAT_COUNT - 1));
                UpdateModes();
                break;
            case P_MATERIAL_AMT: materialAmt_ = Clamp(v, 0.0f, 1.0f);   break;
            case P_SHIMMER:      shimmerAmt_  = Clamp(v, 0.0f, 1.0f);   break;
            case P_SHIMMER_TILT: shimmerTilt_ = Clamp(v, 0.0f, 1.0f);   break;
            case P_SPACE:        spaceWet_    = Clamp(v, 0.0f, 1.0f);   break;
            case P_SPACE_SIZE:   spaceSize_   = Clamp(v, 0.0f, 1.0f);   break;
            case P_DAMPING:      damping_     = Clamp(v, 0.0f, 1.0f);   break;
            case P_VARIATION:    variation_   = Clamp(v, 0.0f, 1.0f);   break;
            case P_VAR_RATE:     varRate_     = Clamp(v, 0.0f, 1.0f);   break;
            case P_FOCI_DEPTH:
                fociDepth_ = (uint8_t)Clamp(v, 0.0f, 4.0f);
                fociDirty_ = true;
                break;
            case P_FOCI_SEEDS:
                fociSeeds_ = (uint8_t)Clamp(v, 1.0f, 8.0f);
                fociDirty_ = true;
                break;
            case P_FOCI_DRIFT:   fociDrift_   = Clamp(v, 0.0f, 1.0f);   break;
            case P_FOCI_SPREAD:  fociSpread_  = Clamp(v, 0.0f, 1.0f);   break;
            case P_BOUNCE_DEPTH: bounceDepth_ = (uint8_t)Clamp(v, 0.0f, 3.0f); break;
            case P_BOUNCE_PROB:  bounceProb_  = Clamp(v, 0.0f, 1.0f);   break;
            case P_WINDOW:       windowShape_ = Clamp(v, 0.0f, 1.0f);   break;
            case P_ABERRATION:   aberration_  = Clamp(v, 0.0f, 1.0f);   break;
            case P_DRIVE:        drive_       = Clamp(v, 0.0f, 1.0f);   break;
            case P_SMART:        smartRays_   = (v > 0.5f) ? 1 : 0;     break;
            case P_FEEDBACK:     feedback_    = Clamp(v, 0.0f, 1.0f);   break;
            case P_CUTOFF:
                cutoff_ = Clamp(v, 50.0f, 16000.0f);
                UpdateFilter();
                break;
            case P_RESONANCE:
                resonance_ = Clamp(v, 0.0f, 0.95f);
                UpdateFilter();
                break;
            case P_VOLUME:       volume_      = Clamp(v, 0.0f, 1.0f);   break;
            case P_SEED: {
                const uint32_t s = (uint32_t)Clamp(v, 1.0f, 4294967000.0f);
                seed_ = s ? s : 1u;
                rng_  = seed_;
                fociDirty_ = true;
                break;
            }
            default: break;
        }
    }

    float GetParam(uint8_t id) const
    {
        switch(id)
        {
            case P_DENSITY:      return density_;
            case P_GRAIN_DUR:    return grainDur_;
            case P_FOCUS:        return focus_;
            case P_APERTURE:     return aperture_;
            case P_SAMPLE_MODE:  return (float)sampleMode_;
            case P_CHORD:        return (float)chord_;
            case P_PITCH:        return pitchSemi_;
            case P_OCT_PROB:     return octProb_;
            case P_WIDTH:        return width_;
            case P_MATERIAL:     return (float)material_;
            case P_MATERIAL_AMT: return materialAmt_;
            case P_SHIMMER:      return shimmerAmt_;
            case P_SHIMMER_TILT: return shimmerTilt_;
            case P_SPACE:        return spaceWet_;
            case P_SPACE_SIZE:   return spaceSize_;
            case P_DAMPING:      return damping_;
            case P_VARIATION:    return variation_;
            case P_VAR_RATE:     return varRate_;
            case P_FOCI_DEPTH:   return (float)fociDepth_;
            case P_FOCI_SEEDS:   return (float)fociSeeds_;
            case P_FOCI_DRIFT:   return fociDrift_;
            case P_FOCI_SPREAD:  return fociSpread_;
            case P_BOUNCE_DEPTH: return (float)bounceDepth_;
            case P_BOUNCE_PROB:  return bounceProb_;
            case P_WINDOW:       return windowShape_;
            case P_ABERRATION:   return aberration_;
            case P_DRIVE:        return drive_;
            case P_SMART:        return (float)smartRays_;
            case P_FEEDBACK:     return feedback_;
            case P_CUTOFF:       return cutoff_;
            case P_RESONANCE:    return resonance_;
            case P_VOLUME:       return volume_;
            case P_SEED:         return (float)seed_;
            default:             return 0.0f;
        }
    }

    /** Techo de granos vivos. main.cpp lo baja cuando el bus de audio
     *  entra en modo shed para no desbordar el callback. */
    void SetGrainCap(int cap)
    {
        grainCap_ = (cap < 4) ? 4 : (cap > kMaxGrains ? kMaxGrains : cap);
    }

    int  ActiveGrains() const { return nActive_; }

    /* ── Presets de fabrica ────────────────────────────────────── */

    static constexpr uint8_t kPresetCount = 8;

    /** 8 escenas listas. Cada una fija TODOS los parametros para que
     *  cambiar de preset sea determinista, no dependa del estado. */
    void ApplyPreset(uint8_t id)
    {
        struct P { uint8_t id; float v; };
        switch(id % kPresetCount)
        {
            default:
            case 0: /* Glass Cathedral — pad cristalino, shimmer amplio */
                Load(52.f, .42f, .35f, .30f, 1, CHORD_POWER, 0.f, .18f, .90f,
                     MAT_GLASS, .60f, .55f, .35f, .62f, .78f, .38f,
                     .35f, .28f, 2, 3, .30f, .25f, 1, .35f, .55f, .22f, .08f,
                     1, .30f, 9500.f, .12f, .95f);
                break;
            case 1: /* Iron Bloom — metal grave, lento, muy fisico */
                Load(42.f, .70f, .28f, .22f, 1, CHORD_FIFTHS, -12.f, .10f, .80f,
                     MAT_METAL, .72f, .30f, .15f, .50f, .85f, .55f,
                     .45f, .18f, 3, 4, .40f, .35f, 2, .45f, .70f, .30f, .18f,
                     1, .45f, 6200.f, .22f, .68f);
                break;
            case 2: /* Ice Field — agudo, brillante, shimmer +19 */
                Load(80.f, .28f, .55f, .45f, 1, CHORD_PENTATONIC, 12.f, .35f, 1.f,
                     MAT_ICE, .55f, .70f, .75f, .68f, .80f, .22f,
                     .55f, .45f, 3, 4, .55f, .45f, 1, .30f, .40f, .35f, .05f,
                     1, .40f, 13000.f, .10f, .85f);
                break;
            case 3: /* Wood Chapel — calido, corto, organico */
                Load(120.f, .22f, .40f, .35f, 2, CHORD_MAJOR, 0.f, .08f, .70f,
                     MAT_WOOD, .65f, .22f, .20f, .45f, .55f, .60f,
                     .40f, .35f, 2, 3, .35f, .30f, 1, .40f, .35f, .18f, .12f,
                     1, .25f, 5200.f, .18f, .85f);
                break;
            case 4: /* Deep Membrane — sub, oscuro, casi ritmico */
                Load(38.f, .95f, .20f, .18f, 2, CHORD_OCTAVES, -12.f, .05f, .55f,
                     MAT_SKIN, .70f, .15f, .10f, .38f, .60f, .70f,
                     .50f, .15f, 2, 2, .25f, .20f, 2, .55f, .25f, .12f, .25f,
                     0, .55f, 3200.f, .30f, .72f);
                break;
            case 5: /* Bowed String — cuerda frotada continua */
                Load(135.f, .35f, .45f, .20f, 1, CHORD_MINOR, 0.f, .12f, .75f,
                     MAT_STRING, .68f, .35f, .25f, .48f, .70f, .40f,
                     .30f, .22f, 1, 2, .20f, .18f, 0, .0f, .60f, .15f, .10f,
                     1, .20f, 8200.f, .20f, .62f);
                break;
            case 6: /* Plasma Storm — inestable, agresivo, muy variable */
                Load(120.f, .16f, .50f, .65f, 0, CHORD_MINOR_SCALE, 0.f, .45f, 1.f,
                     MAT_PLASMA, .75f, .60f, .55f, .72f, .90f, .30f,
                     .85f, .70f, 4, 5, .80f, .65f, 3, .60f, .20f, .55f, .35f,
                     0, .80f, 11000.f, .35f, .80f);
                break;
            case 7: /* Stone Well — pozo de piedra, mate y profundo */
                Load(52.f, .55f, .30f, .40f, 2, CHORD_SUS2, -7.f, .06f, .65f,
                     MAT_STONE, .62f, .20f, .12f, .58f, .95f, .72f,
                     .38f, .12f, 2, 3, .30f, .28f, 2, .50f, .50f, .20f, .15f,
                     1, .35f, 4200.f, .25f, .78f);
                break;
        }
    }

  private:
    /* ── Focos: la constelacion recursiva ──────────────────────── */
    struct Focus {
        float   pos;      /* segundos dentro de la fuente        */
        float   vel;      /* deriva (s/s)                        */
        float   w;        /* peso actual (con fade)              */
        float   wTarget;
        float   age;
        float   ttl;      /* < 0 = inmortal (semilla)            */
        uint8_t depth;    /* niveles de recursion restantes      */
        bool    active;
    };

    static constexpr int kNumMutTargets = 6;

    /* ── Tablas y buffers ──────────────────────────────────────── */
    float window_[3][kWindowSize];
    float energy_[kEnergyBins];
    float energyMax_;

    Grain    grains_[kMaxGrains];
    uint16_t activeIdx_[kMaxGrains];
    uint16_t free_[kMaxGrains];
    int      nActive_, nFree_;
    int      grainCap_;

    Focus foci_[kMaxFoci];

    float   scale_[kScaleCap];
    uint8_t scaleLen_;

    Mode  modes_[kModes];
    float modeAmp_[kModes];
    float modeNorm_;

    /* Red de difusion + shimmer. Los buffers grandes viven en SDRAM. */
    float apL_[kNumAllpass][1024];
    float apR_[kNumAllpass][1024];
    float combL_[kNumComb][4096];
    float combR_[kNumComb][4096];
    int   apIdx_[kNumAllpass][2];
    int   combIdx_[kNumComb][2];
    float combLpL_[kNumComb], combLpR_[kNumComb];
    float shiftBufL_[kShiftLen], shiftBufR_[kShiftLen];
    int   shiftW_;
    float shiftPhase12_, shiftPhase19_;

    /* Fuente armonica interna (SDRAM: 96000 int16 = 187 KB). */
    int16_t internalSource_[kInternalLen];

    const int16_t* srcData_;
    uint32_t       srcLen_;
    float          srcSr_;
    bool           srcIsInternal_;

    /* ── Estado ────────────────────────────────────────────────── */
    float sr_, invSr_;
    uint32_t rng_, seed_;
    float spawnAcc_;
    float qmcPos_, qmcPitch_;
    uint32_t stratI_, scaleI_;
    float env_, evo_, evoDir_;
    float fociAcc_;
    bool  fociDirty_;
    float noteRatio_, noteHz_, noteVel_;
    bool  active_;

    float denorm_;
    float dcXL_, dcYL_, dcXR_, dcYR_;
    float svLpL_, svBpL_, svLpR_, svBpR_;
    float svF_, svQ_;
    float aLow_, aHigh_;

    float lfoPhase_[4];
    float mutateAcc_;
    float mutOff_[kNumMutTargets];
    float mutTarget_[kNumMutTargets];
    float modDensity_, modFocus_, modAperture_;
    float modDur_, modPitch_, modMaterial_;

    /* ── Parametros ────────────────────────────────────────────── */
    float   density_, grainDur_;
    float   focus_, aperture_;
    uint8_t sampleMode_, chord_;
    float   pitchSemi_, octProb_, width_;
    uint8_t material_;
    float   materialAmt_;
    float   shimmerAmt_, shimmerTilt_;
    float   spaceWet_, spaceSize_, damping_;
    float   variation_, varRate_;
    uint8_t fociDepth_, fociSeeds_;
    float   fociDrift_, fociSpread_;
    uint8_t bounceDepth_;
    float   bounceProb_;
    float   windowShape_, aberration_, drive_;
    uint8_t smartRays_;
    float   feedback_;
    float   cutoff_, resonance_, volume_;

    /* ═════════════════════════════════════════════════════════════
     *  Construccion de tablas
     * ═══════════════════════════════════════════════════════════ */

    void BuildWindows()
    {
        for(int i = 0; i < kWindowSize; ++i) {
            const float t = (float)i / (float)(kWindowSize - 1);

            /* 0 · Percusiva: ataque inmediato, caida exponencial. Da
             *    grano "puntual", textura de lluvia / chispa. */
            window_[0][i] = expf(-5.5f * t) * (1.0f - expf(-60.0f * t));

            /* 1 · Hann: la ventana clasica del granular, la mas suave.
             *    Solapando muchos granos da una nube sin costuras. */
            window_[1][i] = 0.5f * (1.0f - cosf(RAYDRONE_TWOPI * t));

            /* 2 · Meseta (Tukey, 22 % de taper): mucho mas cuerpo
             *    sostenido, el drone gana masa y pierde grano. */
            const float taper = 0.22f;
            float w;
            if(t < taper)          w = 0.5f * (1.0f - cosf(kPi * t / taper));
            else if(t > 1.0f - taper) w = 0.5f * (1.0f - cosf(kPi * (1.0f - t) / taper));
            else                   w = 1.0f;
            window_[2][i] = w;
        }
    }

    /** Fuente armonica interna: 2 s de un complejo de 12 parciales
     *  sobre A1 (55 Hz), con deriva de fase lenta e inarmonicidad muy
     *  leve para que no suene a oscilador digital. Cada parcial se
     *  genera con un oscilador "magic circle" (dos multiplicaciones
     *  por muestra, sin llamar a sinf 1,1 M de veces). */
    void BuildInternalSource()
    {
        static constexpr float baseHz = 55.0f;
        static constexpr int   nPart  = 12;

        for(uint32_t i = 0; i < kInternalLen; ++i) internalSource_[i] = 0;

        float peak = 0.0f;
        /* Sumar los 12 parciales sobre un buffer float de 2 s costaria
         * 384 KB extra; en su lugar se acumula por bloques pequenos en
         * una ventana temporal de la pila. */
        static constexpr uint32_t kChunk = 512;
        float chunk[kChunk];

        /* Estado del oscilador de cada parcial (magic circle). */
        float sx[nPart], sy[nPart], eps[nPart], amp[nPart];
        uint32_t r = 0x9E3779B9u;
        for(int p = 0; p < nPart; ++p) {
            /* Inarmonicidad tipo cuerda: f_n = n·f0·sqrt(1 + B·n²). */
            const float n = (float)(p + 1);
            const float B = 0.00004f;
            const float hz = baseHz * n * sqrtf(1.0f + B * n * n);
            const float w  = RAYDRONE_TWOPI * hz / sr_;
            eps[p] = 2.0f * sinf(0.5f * w);
            const float ph0 = Rng01(r) * RAYDRONE_TWOPI;
            sx[p] = cosf(ph0);
            sy[p] = sinf(ph0);
            amp[p] = 1.0f / (n * (1.0f + 0.15f * n));
        }

        for(uint32_t base = 0; base < kInternalLen; base += kChunk) {
            const uint32_t n = (kInternalLen - base < kChunk) ? (kInternalLen - base) : kChunk;
            for(uint32_t j = 0; j < n; ++j) chunk[j] = 0.0f;
            for(int p = 0; p < nPart; ++p) {
                float x = sx[p], y = sy[p];
                const float e = eps[p], a = amp[p];
                for(uint32_t j = 0; j < n; ++j) {
                    x -= e * y;
                    y += e * x;
                    chunk[j] += a * y;
                }
                sx[p] = x; sy[p] = y;
            }
            for(uint32_t j = 0; j < n; ++j) {
                const float v = chunk[j];
                const float av = v < 0 ? -v : v;
                if(av > peak) peak = av;
                /* Guardamos provisionalmente escalado a 1/4 de fondo de
                 * escala; se re-normaliza en la segunda pasada. */
                internalSource_[base + j] = (int16_t)Clamp(v * 8000.0f, -32000.f, 32000.f);
            }
        }

        /* Normalizado a -3 dBFS. */
        if(peak > 0.0001f) {
            const float g = (23000.0f / (peak * 8000.0f));
            if(g < 0.999f || g > 1.001f) {
                for(uint32_t i = 0; i < kInternalLen; ++i) {
                    internalSource_[i] = (int16_t)Clamp((float)internalSource_[i] * g,
                                                        -32000.f, 32000.f);
                }
            }
        }
    }

    /** Mapa de energia de la fuente: los "rayos inteligentes" lo usan
     *  como distribucion de importancia (rejection sampling), asi los
     *  granos caen donde realmente hay senal en vez de en silencio. */
    void BuildEnergyMap(const int16_t* data, uint32_t len)
    {
        for(int b = 0; b < kEnergyBins; ++b) energy_[b] = 0.0f;
        energyMax_ = 0.000001f;
        if(data == nullptr || len < 4) return;

        const uint32_t per = len / kEnergyBins;
        if(per == 0) return;
        /* Submuestreo: 64 lecturas por bin bastan para la envolvente y
         * mantiene el coste acotado aunque el sample sea de 24 MB. */
        const uint32_t stride = (per > 64) ? (per / 64) : 1;
        for(int b = 0; b < kEnergyBins; ++b) {
            uint32_t start = (uint32_t)b * per;
            float acc = 0.0f;
            int   cnt = 0;
            for(uint32_t i = start; i < start + per && i < len; i += stride) {
                const float v = (float)data[i] * (1.0f / 32768.0f);
                acc += v * v;
                ++cnt;
            }
            const float e = cnt ? sqrtf(acc / (float)cnt) : 0.0f;
            energy_[b] = e;
            if(e > energyMax_) energyMax_ = e;
        }
    }

    void RebuildChord() { scaleLen_ = ChordRatios(chord_, scale_); }

    /** Recalcula los coeficientes del banco modal para el material y
     *  la nota actuales. Un modo = resonador de 2 polos:
     *      y[n] = b0·x[n] + a1·y[n-1] + a2·y[n-2]
     *  con a1 = 2·R·cos(w), a2 = -R², R = exp(-pi·f/(Q·sr)).
     *
     *  GANANCIA — el punto delicado. Un modo de Q alto es un filtro
     *  de banda estrechisima: excitado con una nube granular de banda
     *  ancha deja pasar una fraccion minuscula de la energia. Con la
     *  normalizacion "pico unidad" de libro (b0 = (1-R²)·sin w) los
     *  ocho modos juntos devuelven ~1 % del nivel de entrada y el
     *  material se convierte en un filtro que apaga el motor.
     *  Aqui se normaliza por POTENCIA: para ruido blanco un resonador
     *  de pico unidad y ancho de banda B entrega sqrt(pi·B/sr) del
     *  nivel de entrada, asi que se compensa con sqrt(sr/(pi·B)) y se
     *  divide por sqrt(nModos) para que la suma incoherente de los
     *  ocho vuelva a nivel unidad. El tope de compensacion evita que
     *  un modo de Q extremo excitado por una fuente tonal (que SI cae
     *  justo en su banda) reviente el bus.
     *
     *  Sin powf: los pesos espectrales vienen horneados en la tabla. */
    void UpdateModes()
    {
        const MaterialSpec& sp = MaterialTable(material_);
        const float nyq = sr_ * 0.47f;
        float ampSq = 0.0f;

        for(int m = 0; m < kModes; ++m) {
            const float f = noteHz_ * sp.ratio[m];
            /* Un parcial por encima de Nyquist se pliega: mejor
             * silenciar el modo que dejar que aliasee. */
            if(f >= nyq || f < 15.0f) {
                modes_[m].b0 = 0.0f;
                modes_[m].a1 = 0.0f;
                modes_[m].a2 = 0.0f;
                modeAmp_[m]  = 0.0f;
                modes_[m].Reset();
                continue;
            }
            const float q  = Clamp(sp.q * (0.45f + 0.75f * materialAmt_), 2.0f, 400.0f);
            const float w  = RAYDRONE_TWOPI * f / sr_;
            const float rr = Clamp(expf(-kPi * f / (q * sr_)), 0.0f, 0.99995f);
            modes_[m].a1 = 2.0f * rr * cosf(w);
            modes_[m].a2 = -rr * rr;

            /* Pico unidad. */
            const float b0Peak = (1.0f - rr * rr) * sinf(w);
            /* Compensacion por potencia (ancho de banda -3 dB = f/Q). */
            const float bw     = f / q;
            const float makeup = Clamp(sqrtf(sr_ / (kPi * bw)), 1.0f, 500.0f);
            modes_[m].b0 = b0Peak * makeup;

            modeAmp_[m] = sp.weight[m];
            ampSq += modeAmp_[m] * modeAmp_[m];
        }
        /* Suma incoherente: la norma es la raiz de la suma de cuadrados. */
        modeNorm_ = (ampSq > 1e-8f) ? (kBankGain / sqrtf(ampSq)) : 0.0f;
    }

    void UpdateFilter()
    {
        const float f = Clamp(cutoff_, 50.0f, sr_ * 0.42f);
        svF_ = Clamp(2.0f * sinf(kPi * f / sr_), 0.0f, 0.99f);
        svQ_ = Clamp(1.0f - resonance_ * 0.98f, 0.02f, 1.0f);
        aLow_  = Clamp(RAYDRONE_TWOPI * 500.0f  / sr_, 0.0f, 0.99f);
        aHigh_ = Clamp(RAYDRONE_TWOPI * 2500.0f / sr_, 0.0f, 0.99f);
    }

    /* ═════════════════════════════════════════════════════════════
     *  Lectura de la fuente
     * ═══════════════════════════════════════════════════════════ */

    float MaxPos() const { return (srcLen_ >= 4) ? (float)(srcLen_ - 3) : 0.0f; }

    /** Catmull-Rom cubico sobre la fuente int16. Menos aliasing que
     *  lineal cuando se lee a velocidad != 1 (octava, voicing, detune). */
    inline float SampleAt(float pos) const
    {
        if(srcLen_ < 4 || !(pos >= 0.0f)) return 0.0f;
        const uint32_t i = (uint32_t)pos;
        if(i + 2 >= srcLen_ || i < 1) return 0.0f;
        const float frac = pos - (float)i;
        const float k = 1.0f / 32768.0f;
        const float s0 = (float)srcData_[i - 1] * k;
        const float s1 = (float)srcData_[i]     * k;
        const float s2 = (float)srcData_[i + 1] * k;
        const float s3 = (float)srcData_[i + 2] * k;
        const float a = -0.5f * s0 + 1.5f * s1 - 1.5f * s2 + 0.5f * s3;
        const float b = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
        const float c = -0.5f * s0 + 0.5f * s2;
        return ((a * frac + b) * frac + c) * frac + s1;
    }

    /** Ventana morfable: crossfade continuo entre percusiva, Hann y
     *  meseta segun `windowShape_`. Dos lecturas + un lerp. */
    inline float WindowAt(float ph) const
    {
        int idx = (int)(ph * (float)(kWindowSize - 1));
        if(idx < 0) idx = 0;
        if(idx >= kWindowSize) idx = kWindowSize - 1;
        const float s = windowShape_ * 2.0f;
        if(s <= 1.0f) {
            return window_[0][idx] + (window_[1][idx] - window_[0][idx]) * s;
        }
        const float t = s - 1.0f;
        return window_[1][idx] + (window_[2][idx] - window_[1][idx]) * t;
    }

    /** Aberracion cromatica: cada banda ve la fuente con un filtro
     *  distinto, igual que una lente descompone la luz. */
    inline float BandFilter(Grain& g, float x) const
    {
        if(aberration_ <= 0.0f) return x;
        if(g.band == 0) { g.lp = Flush(g.lp + aLow_  * (x - g.lp)); return g.lp; }
        if(g.band == 2) { g.lp = Flush(g.lp + aHigh_ * (x - g.lp)); return x - g.lp; }
        return x;
    }

    /** Shaper barato por grano — el color inmediato del material,
     *  heredado del motor WASM y ampliado a los materiales nuevos.
     *  El cuerpo resonante lo pone el banco modal, no esto. */
    inline float MaterialShaper(Grain& g, float x) const
    {
        if(materialAmt_ <= 0.0f || material_ == MAT_NONE) return x;
        float shaped;
        switch(material_)
        {
            case MAT_METAL: { const float d = x - g.tone; g.tone += 0.12f * d;
                              shaped = x + d * 1.6f; } break;
            case MAT_WOOD:  { g.tone += 0.045f * (x - g.tone);
                              shaped = g.tone * 0.86f + x * 0.14f; } break;
            case MAT_GLASS: { g.tone += 0.22f * (x - g.tone);
                              shaped = x * 0.82f + g.tone * 0.42f; } break;
            case MAT_WATER: { g.tone += 0.09f * (x - g.tone);
                              const float rip = TriInv(fmodf(g.age * 0.00037f, 1.0f));
                              shaped = x * (0.82f + 0.18f * rip) + g.tone * 0.35f; } break;
            /* Plasma: no linealidad fuerte pero ACOTADA. El x - x³ del
             * motor WASM se invierte de signo en cuanto |x| > 1,15, lo
             * que mete un pliegue asimetrico y offset de continua en la
             * nube. Soft() da la misma aspereza sin salirse. */
            case MAT_PLASMA:  shaped = Soft(x * 2.0f) * 1.05f; break;
            case MAT_STONE: { g.tone += 0.030f * (x - g.tone);
                              shaped = g.tone * 0.94f + x * 0.06f; } break;
            case MAT_SKIN:  { g.tone += 0.055f * (x - g.tone);
                              shaped = g.tone * 0.78f + Soft(x * 1.4f) * 0.22f; } break;
            case MAT_STRING:{ const float d = x - g.tone; g.tone += 0.16f * d;
                              shaped = g.tone + d * 0.55f; } break;
            case MAT_ICE:   { const float d = x - g.tone; g.tone += 0.34f * d;
                              shaped = x * 0.7f + d * 2.1f; } break;
            default:          shaped = x; break;
        }
        g.tone = Flush(g.tone);
        return x + (shaped - x) * materialAmt_;
    }

    /* ═════════════════════════════════════════════════════════════
     *  Muestreo QMC
     * ═══════════════════════════════════════════════════════════ */

    /** Siguiente u ∈ [0,1) segun el modo:
     *    1 aureo         → secuencia de baja discrepancia (converge 1/N)
     *    2 estratificado → 17 estratos round-robin
     *    otro            → random puro (converge 1/√N) */
    inline float NextU()
    {
        static constexpr float kGolden = 0.618034f;
        static constexpr uint32_t kStrata = 17;
        if(sampleMode_ == 1) {
            qmcPos_ += kGolden;
            if(qmcPos_ >= 1.0f) qmcPos_ -= 1.0f;
            return qmcPos_;
        }
        if(sampleMode_ == 2) {
            const float u = ((float)stratI_ + Rng01(rng_)) / (float)kStrata;
            stratI_ = (stratI_ + 1) % kStrata;
            return u;
        }
        return Rng01(rng_);
    }

    /** Grado de la voicing para este grano. Usa la secuencia R2 (raiz
     *  de la constante plastica) para que la eleccion de grado tampoco
     *  se agrupe. */
    inline float ScaleRatio()
    {
        if(scaleLen_ == 0) return 1.0f;
        static constexpr float kR2Alpha = 0.7548777f;
        int idx;
        if(sampleMode_ == 1) {
            qmcPitch_ += kR2Alpha;
            if(qmcPitch_ >= 1.0f) qmcPitch_ -= 1.0f;
            idx = (int)(qmcPitch_ * (float)scaleLen_);
        } else if(sampleMode_ == 2) {
            idx = (int)(scaleI_ % scaleLen_);
            ++scaleI_;
        } else {
            idx = (int)(Rng01(rng_) * (float)scaleLen_);
        }
        if(idx >= scaleLen_) idx = scaleLen_ - 1;
        if(idx < 0) idx = 0;
        return scale_[idx];
    }

    /* ═════════════════════════════════════════════════════════════
     *  Constelacion de focos (recursion)
     * ═══════════════════════════════════════════════════════════ */

    float FocusSpan() const
    {
        return (srcLen_ >= 4 && srcSr_ > 0.0f) ? ((float)srcLen_ / srcSr_) : 0.0f;
    }

    /** (Re)coloca las semillas por baja discrepancia a lo largo de la
     *  fuente: maxima dispersion, sin solapes. Inmortales, peso pleno. */
    void ReinitFoci()
    {
        const float span = FocusSpan();
        for(int i = 0; i < kMaxFoci; ++i) { foci_[i].active = false; foci_[i].w = 0.0f; }
        const int ns = (fociSeeds_ < kMaxFoci) ? fociSeeds_ : kMaxFoci;
        for(int i = 0; i < ns; ++i) {
            const float g = 0.5f + (float)i * 0.618034f;
            const float u = g - (float)(uint32_t)g;
            foci_[i].pos     = u * span;
            foci_[i].vel     = (Rng01(rng_) - 0.5f) * fociDrift_ * span * 0.02f;
            foci_[i].w       = 1.0f;
            foci_[i].wTarget = 1.0f;
            foci_[i].age     = 0.0f;
            foci_[i].ttl     = -1.0f;
            foci_[i].depth   = fociDepth_;
            foci_[i].active  = true;
        }
        fociAcc_   = 0.0f;
        fociDirty_ = false;
    }

    /** Nace un foco hijo. El padre se elige ponderado por peso entre
     *  los que aun tienen niveles. El hijo cae cerca, con offset, peso
     *  y vida que encogen por nivel → arbol auto-similar. */
    void BirthFocus()
    {
        int slot = -1;
        for(int i = 0; i < kMaxFoci; ++i) if(!foci_[i].active) { slot = i; break; }
        if(slot < 0) return;

        float sum = 0.0f;
        for(int i = 0; i < kMaxFoci; ++i)
            if(foci_[i].active && foci_[i].depth > 0) sum += foci_[i].w;
        if(sum <= 0.0f) return;

        float r = Rng01(rng_) * sum;
        int parent = -1;
        for(int i = 0; i < kMaxFoci; ++i) {
            if(foci_[i].active && foci_[i].depth > 0) {
                r -= foci_[i].w;
                if(r <= 0.0f) { parent = i; break; }
            }
        }
        if(parent < 0) return;

        const int level = (int)fociDepth_ - (int)foci_[parent].depth + 1;
        float shrink = 1.0f;
        for(int l = 0; l < level && l < 8; ++l) shrink *= 0.55f;

        const float span = FocusSpan();
        const float off  = (Rng01(rng_) - 0.5f) * 2.0f * fociSpread_ * span * shrink;
        Focus& c = foci_[slot];
        c.pos     = Clamp(foci_[parent].pos + off, 0.0f, span);
        c.vel     = (Rng01(rng_) - 0.5f) * fociDrift_ * span * 0.02f * (1.0f + shrink);
        c.w       = 0.0f;                       /* fade-in */
        c.wTarget = foci_[parent].w * 0.72f;    /* los hijos, mas discretos */
        c.depth   = (uint8_t)(foci_[parent].depth - 1);
        c.age     = 0.0f;
        c.ttl     = 12.0f * shrink;             /* vida finita, corta a escala fina */
        c.active  = true;
    }

    void UpdateFoci(float dt)
    {
        const float span = FocusSpan();
        if(span <= 0.0f) return;
        if(fociDirty_) ReinitFoci();

        const float maxv = fociDrift_ * span * 0.03f;
        const float fade = (dt / 1.5f < 1.0f) ? (dt / 1.5f) : 1.0f;

        for(int i = 0; i < kMaxFoci; ++i) {
            Focus& f = foci_[i];
            if(!f.active) continue;
            f.age += dt;
            /* Re-aleatorizar la deriva ~cada 3 s: movimiento browniano
             * lento, no una rampa. */
            if(Rng01(rng_) < dt / 3.0f) f.vel = (Rng01(rng_) - 0.5f) * 2.0f * maxv;
            float pos = f.pos + f.vel * dt;
            if(pos < 0.0f)      { pos = -pos;             f.vel = -f.vel; }
            else if(pos > span) { pos = 2.0f * span - pos; f.vel = -f.vel; }
            f.pos = Clamp(pos, 0.0f, span);

            if(f.ttl >= 0.0f && f.age > f.ttl - 1.5f) f.wTarget = 0.0f;
            f.w += (f.wTarget - f.w) * fade;
            if(f.ttl >= 0.0f && f.age > f.ttl && f.w < 0.002f) { f.active = false; f.w = 0.0f; }
        }

        if(fociDepth_ > 0) {
            /* Tasa de nacimientos ligada a la variacion: mas variacion,
             * constelacion mas viva. */
            const float rate = 0.15f + variation_ * 0.85f;
            fociAcc_ += rate * dt;
            int guard = 0;
            while(fociAcc_ >= 1.0f && guard < 4) { BirthFocus(); fociAcc_ -= 1.0f; ++guard; }
        }
    }

    /** Elige foco ponderado por peso. -1 = no hay constelacion. */
    int PickFocus()
    {
        float sum = 0.0f;
        for(int i = 0; i < kMaxFoci; ++i) if(foci_[i].active) sum += foci_[i].w;
        if(sum <= 0.0f) return -1;
        float r = Rng01(rng_) * sum;
        for(int i = 0; i < kMaxFoci; ++i) {
            if(foci_[i].active) {
                r -= foci_[i].w;
                if(r <= 0.0f) return i;
            }
        }
        return -1;
    }

    /* ═════════════════════════════════════════════════════════════
     *  Variacion: LFOs irracionales + mutacion
     * ═══════════════════════════════════════════════════════════ */

    /** Cuatro LFOs a frecuencias mutuamente irracionales (no comparten
     *  periodo comun → el patron no se repite nunca) mas un motor de
     *  mutacion que cada cierto tiempo re-tira un parametro y hace
     *  glide hasta el nuevo valor. */
    void UpdateVariation(float dt)
    {
        static constexpr float kBaseHz[4] = { 0.0370f, 0.0613f, 0.0917f, 0.1471f };
        const float speed = 0.25f + varRate_ * 3.0f;
        float lfo[4];
        for(int i = 0; i < 4; ++i) {
            lfoPhase_[i] += kBaseHz[i] * speed * dt;
            if(lfoPhase_[i] >= 1.0f) lfoPhase_[i] -= (float)(int)lfoPhase_[i];
            lfo[i] = sinf(RAYDRONE_TWOPI * lfoPhase_[i]);
        }

        /* Mutacion: re-tirar un objetivo cada 4..30 s segun varRate_. */
        const float period = 30.0f - varRate_ * 26.0f;
        mutateAcc_ += dt;
        if(mutateAcc_ >= period) {
            mutateAcc_ = 0.0f;
            const int t = (int)(Rng01(rng_) * (float)kNumMutTargets);
            if(t >= 0 && t < kNumMutTargets)
                mutTarget_[t] = (Rng01(rng_) * 2.0f - 1.0f) * variation_;
        }
        /* Glide de 3 s hacia el objetivo: la mutacion nunca salta. */
        const float glide = (dt / 3.0f < 1.0f) ? (dt / 3.0f) : 1.0f;
        for(int i = 0; i < kNumMutTargets; ++i)
            mutOff_[i] += (mutTarget_[i] - mutOff_[i]) * glide;

        const float v = variation_;
        modDensity_  = lfo[0] * v * 0.55f + mutOff_[0] * 0.60f;
        modFocus_    = lfo[1] * v * 0.30f + mutOff_[1] * 0.35f;
        modAperture_ = lfo[2] * v * 0.45f + mutOff_[2] * 0.50f;
        modDur_      = lfo[3] * v * 0.35f + mutOff_[3] * 0.40f;
        modPitch_    = lfo[2] * v * 0.10f + mutOff_[4] * 0.14f;
        modMaterial_ = lfo[0] * v * 0.20f + mutOff_[5] * 0.25f;
    }

    /* ═════════════════════════════════════════════════════════════
     *  Sembrado de granos
     * ═══════════════════════════════════════════════════════════ */

    inline float EnergyAt(float sec) const
    {
        const float span = FocusSpan();
        if(span <= 0.0f) return 0.0f;
        int b = (int)(sec / span * (float)kEnergyBins);
        if(b < 0) b = 0;
        if(b >= kEnergyBins) b = kEnergyBins - 1;
        return energy_[b];
    }

    int AllocGrain()
    {
        if(nFree_ > 0 && nActive_ < grainCap_) {
            const int slot = free_[--nFree_];
            activeIdx_[nActive_++] = (uint16_t)slot;
            return slot;
        }
        /* Sin slots: robar el grano MAS APAGADO (fase mas avanzada → la
         * cola de la ventana ya esta cerca de 0) en vez del mas viejo.
         * Robo sin click. */
        if(nActive_ == 0) return -1;
        int bestK = 0;
        float bestPh = -1.0f;
        for(int k = 0; k < nActive_; ++k) {
            const Grain& g = grains_[activeIdx_[k]];
            const float ph = g.age * g.invDur;
            if(ph > bestPh) { bestPh = ph; bestK = k; }
        }
        return activeIdx_[bestK];
    }

    /** Coloca un grano. Decide octava, transposicion, paneo y duracion. */
    void Place(float pos, uint8_t band, uint8_t depth)
    {
        /* El cristal alarga el grano: es lo que le da la cola cantada. */
        const float matDur = (material_ == MAT_GLASS || material_ == MAT_ICE)
                             ? (1.0f + materialAmt_ * 0.6f) : 1.0f;
        const float durS = Clamp(grainDur_ * (1.0f + modDur_), 0.005f, 3.0f);
        const float durSamp = durS * sr_ * matDur;
        if(durSamp < 1.0f) return;

        const float detune = 1.0f + (Rng01(rng_) - 0.5f) * 0.005f; /* ±0,25 % lush */
        const float ratio  = ScaleRatio();
        const float oct    = (Rng01(rng_) < octProb_) ? 2.0f : 1.0f;
        const float plasma = (material_ == MAT_PLASMA)
                             ? (1.0f + (Rng01(rng_) - 0.5f) * materialAmt_ * 0.04f) : 1.0f;
        const float pitchM = SemitoneRatio(pitchSemi_ + modPitch_ * 2.0f);
        const float step = oct * ratio * detune * plasma * pitchM * noteRatio_
                           * (srcSr_ / sr_);

        const float pan  = (Rng01(rng_) * 2.0f - 1.0f) * width_;
        const int slot = AllocGrain();
        if(slot < 0) return;

        /* Compensacion de solapamiento. Sin esto el nivel de salida
         * sube con la densidad y el motor entra en el limitador en
         * cuanto se pide una nube espesa. Los granos son mutuamente
         * incoherentes, asi que su suma crece como sqrt(N): dividir
         * por sqrt(solapamiento esperado) deja el nivel plano mientras
         * se barre la densidad, que es justo lo que se quiere de un
         * mando de densidad. */
        const float overlap = Clamp(density_ * durS * matDur, 1.0f, (float)kMaxGrains);
        /* El tope evita que con solapamiento ~1 (densidad muy baja y
         * granos muy cortos) un unico grano entre al bus a 4x. */
        const float gain    = Clamp(1.25f * kDryGain / sqrtf(overlap), 0.0f, 1.0f);

        Grain& g = grains_[slot];
        g.pos    = pos;
        g.step   = Clamp(step, 0.02f, 16.0f);
        g.age    = 0.0f;
        g.invDur = 1.0f / durSamp;
        g.gain   = gain;
        g.lp     = 0.0f;
        g.tone   = 0.0f;
        g.panL   = sqrtf((1.0f - pan) * 0.5f);
        g.panR   = sqrtf((1.0f + pan) * 0.5f);
        g.band   = band;
        g.depth  = depth;
        g.active = true;
    }

    void Spawn()
    {
        if(srcLen_ < 4) return;

        /* Banda del rayo (aberracion cromatica): 40/40/20. */
        const float rb = Rng01(rng_);
        const uint8_t band = (rb < 0.4f) ? 0 : (rb < 0.8f ? 1 : 2);
        float scale = 1.0f;
        if(aberration_ > 0.0f) {
            if(band == 0)      scale = 1.0f + aberration_ * 2.2f;
            else if(band == 2) scale = Clamp(1.0f - aberration_ * 0.72f, 0.08f, 1.0f);
        }

        const float span = FocusSpan();
        if(span <= 0.0f) return;

        /* Foco efectivo: en modo constelacion, uno de los focos vivos
         * (ponderado por peso) con apertura mas cerrada cuanto mas fino
         * es el nivel — auto-similar. Si no, el foco unico con su
         * autoevolucion acotada por feedback_. */
        float effFocus, effAp;
        const float apBase = Clamp(aperture_ * (1.0f + modAperture_), 0.0f, 2.0f);
        const int fsel = (fociDepth_ > 0 || fociSeeds_ > 1) ? PickFocus() : -1;
        if(fsel >= 0) {
            const int level = (int)fociDepth_ - (int)foci_[fsel].depth;
            float lvl = 1.0f;
            for(int l = 0; l < level && l < 8; ++l) lvl *= 0.7f;
            effFocus = Clamp(foci_[fsel].pos, 0.0f, span);
            effAp    = apBase * span * 0.5f * (1.0f + feedback_ * env_ * 8.0f) * lvl;
        } else {
            const float f0 = focus_ + modFocus_ + (evo_ - 0.5f) * feedback_ * 0.45f;
            effFocus = Clamp(f0, 0.0f, 1.0f) * span;
            effAp    = apBase * span * 0.5f * (1.0f + feedback_ * env_ * 8.0f);
        }

        float offSec = effFocus + TriInv(NextU()) * effAp * scale;

        /* Rayos inteligentes: rejection ∝ energia → los granos caen
         * donde hay senal, no en el silencio del sample. */
        if(smartRays_ && !srcIsInternal_) {
            for(int t = 0; t < 6; ++t) {
                if(EnergyAt(offSec) >= energyMax_ * Rng01(rng_)) break;
                offSec = effFocus + TriInv(NextU()) * effAp * scale;
            }
        }

        const float pos = Clamp(offSec * srcSr_, 1.0f, MaxPos());
        Place(pos, band, bounceDepth_);
    }

    /* ═════════════════════════════════════════════════════════════
     *  Espacio: difusion + shimmer
     * ═════════════════════════════════════════════════════════════
     *  Red de 4 allpass (difusion) + 2 comb (cola) por canal. Dentro
     *  de la realimentacion hay DOS pitch-shifters, +12 y +19, cuya
     *  mezcla la decide shimmerTilt_. Como la octava vuelve a entrar
     *  en el lazo, cada pasada sube otra octava: la cola asciende sola
     *  y se convierte en una nube que crece. Eso es shimmer de verdad,
     *  no una capa de octava.
     * ═══════════════════════════════════════════════════════════ */

    /** Lee la linea de shimmer con dos cabezas desfasadas media ventana
     *  y las cruza con una cosinusoide, de forma que la cabeza que esta
     *  a punto de dar el salto de wrap siempre pasa por ganancia cero.
     *  Es el pitch shifter granular clasico: barato y sin artefactos
     *  audibles dentro de una cola difusa.
     *
     *  `phase` es el retardo (en samples) de la cabeza A. Avanza a
     *  (rate-1) por muestra: con rate=2 el cabezal retrocede al doble de
     *  velocidad → una octava arriba; con rate=3, octava+quinta (+19). */
    inline void ShimmerRead(float phase, float& oL, float& oR) const
    {
        static constexpr float kLen  = (float)kShiftLen;
        static constexpr float kHalf = kLen * 0.5f;

        float dA = phase;
        float dB = phase + kHalf;
        if(dB >= kLen) dB -= kLen;

        /* gA = 0 cuando la cabeza A acaba de saltar, 1 en el centro. */
        const float gA = 0.5f - 0.5f * cosf(RAYDRONE_TWOPI * (dA * (1.0f / kLen)));
        const float gB = 1.0f - gA;

        const int   iA = (int)dA;
        const float fA = dA - (float)iA;
        const int   iB = (int)dB;
        const float fB = dB - (float)iB;

        /* Indices hacia atras desde la ultima muestra escrita. */
        const int base = shiftW_ - 1 + kShiftLen;
        const int a0 = (base - iA)     & (kShiftLen - 1);
        const int a1 = (base - iA - 1) & (kShiftLen - 1);
        const int b0 = (base - iB)     & (kShiftLen - 1);
        const int b1 = (base - iB - 1) & (kShiftLen - 1);

        const float aL = shiftBufL_[a0] + (shiftBufL_[a1] - shiftBufL_[a0]) * fA;
        const float bL = shiftBufL_[b0] + (shiftBufL_[b1] - shiftBufL_[b0]) * fB;
        const float aR = shiftBufR_[a0] + (shiftBufR_[a1] - shiftBufR_[a0]) * fA;
        const float bR = shiftBufR_[b0] + (shiftBufR_[b1] - shiftBufR_[b0]) * fB;

        oL = aL * gA + bL * gB;
        oR = aR * gA + bR * gB;
    }

    /** Avanza una fase de shimmer y la envuelve en [0, kShiftLen).
     *
     *  La posicion leida es escritura - retardo. La escritura avanza a
     *  1 muestra por muestra, luego para que la lectura avance a `rate`
     *  el retardo tiene que MENGUAR a (rate - 1) por muestra. Con el
     *  signo al reves y rate = 2 el retardo crece a la misma velocidad
     *  que la escritura, la cabeza se queda clavada en una muestra y en
     *  vez de una octava arriba sale continua. */
    static inline void AdvanceShimmer(float& phase, float rate)
    {
        phase -= (rate - 1.0f);
        if(phase >= (float)kShiftLen) phase -= (float)kShiftLen;
        if(phase < 0.0f)              phase += (float)kShiftLen;
    }

    void Space(float inL, float inR, float& outL, float& outR)
    {
        const MaterialSpec& sp = MaterialTable(material_);

        /* Tamano → longitud efectiva de los combs; damping → tono. */
        const float sizeK = 0.45f + spaceSize_ * 0.55f;
        const float damp  = Clamp(damping_ * 0.75f + sp.absorb * 0.35f, 0.0f, 0.94f);
        const float fb    = Clamp(0.62f + spaceSize_ * 0.34f, 0.0f, 0.965f);

        /* Realimentacion con shimmer: lo que vuelve al lazo es la mezcla
         * de la cola con su version transpuesta. */
        float shL = 0.0f, shR = 0.0f;
        if(shimmerAmt_ > 0.001f) {
            float a12L, a12R, a19L, a19R;
            ShimmerRead(shiftPhase12_, a12L, a12R);   /* +12 */
            ShimmerRead(shiftPhase19_, a19L, a19R);   /* +19 */
            AdvanceShimmer(shiftPhase12_, 2.0f);
            AdvanceShimmer(shiftPhase19_, 3.0f);
            shL = a12L + (a19L - a12L) * shimmerTilt_;
            shR = a12R + (a19R - a12R) * shimmerTilt_;
        }

        /* Normalizacion de entrada a la red. Un comb realimentado a `fb`
         * tiene ganancia continua 1/(1-fb): con fb = 0,96 son +28 dB.
         * Sin compensarlo, subir el tamano sube el volumen y el motor
         * vive dentro del limitador. Escalando la entrada por (1-fb) el
         * nivel de la cola queda plano mientras se barre el tamano.
         *
         * El retorno del shimmer entra por el MISMO escalado: si no, la
         * ganancia del lazo (shimmer → combs → shimmer) pasa de 1 y la
         * red se autooscila. Con 0,55·1,6 = 0,88 se queda por debajo de
         * la unidad incluso con shimmer al maximo. */
        const float inScale = (1.0f - fb) * 1.6f;
        /* Los combs y allpass son lazos aparte: si la entrada al espacio
         * llega exactamente a cero (bypass del banco, silencio total) se
         * quedan sin la inyeccion del bus, asi que se siembran aqui. */
        float xL = (inL + shL * shimmerAmt_ * 0.55f) * inScale + denorm_;
        float xR = (inR + shR * shimmerAmt_ * 0.55f) * inScale - denorm_;

        /* Difusion: 4 allpass en serie por canal, con longitudes
         * ligeramente distintas entre L y R para que la cola no colapse
         * a mono. */
        for(int a = 0; a < kNumAllpass; ++a) {
            int lenL = (int)((float)kApLen[a] * sizeK);
            int lenR = (int)((float)(kApLen[a] + kApSpread[a]) * sizeK);
            if(lenL < 8)    lenL = 8;
            if(lenL > 1023) lenL = 1023;
            if(lenR < 8)    lenR = 8;
            if(lenR > 1023) lenR = 1023;

            int& iL = apIdx_[a][0];
            if(iL >= lenL) iL = 0;
            const float bL = apL_[a][iL];
            const float yL = -xL + bL;
            apL_[a][iL] = Flush(xL + bL * 0.5f);
            if(++iL >= lenL) iL = 0;
            xL = yL;

            int& iR = apIdx_[a][1];
            if(iR >= lenR) iR = 0;
            const float bR = apR_[a][iR];
            const float yR = -xR + bR;
            apR_[a][iR] = Flush(xR + bR * 0.5f);
            if(++iR >= lenR) iR = 0;
            xR = yR;
        }

        /* Cola: 2 combs realimentados con lowpass en el lazo. */
        float tL = 0.0f, tR = 0.0f;
        for(int c = 0; c < kNumComb; ++c) {
            int lenL = (int)((float)kCombLen[c] * sizeK);
            int lenR = (int)((float)(kCombLen[c] + kCombSpread[c]) * sizeK);
            if(lenL < 32)   lenL = 32;
            if(lenL > 4095) lenL = 4095;
            if(lenR < 32)   lenR = 32;
            if(lenR > 4095) lenR = 4095;

            int& iL = combIdx_[c][0];
            if(iL >= lenL) iL = 0;
            const float yL = combL_[c][iL];
            combLpL_[c] = Flush(yL * (1.0f - damp) + combLpL_[c] * damp);
            combL_[c][iL] = xL + combLpL_[c] * fb;
            if(++iL >= lenL) iL = 0;
            tL += yL;

            int& iR = combIdx_[c][1];
            if(iR >= lenR) iR = 0;
            const float yR = combR_[c][iR];
            combLpR_[c] = Flush(yR * (1.0f - damp) + combLpR_[c] * damp);
            combR_[c][iR] = xR + combLpR_[c] * fb;
            if(++iR >= lenR) iR = 0;
            tR += yR;
        }
        tL *= 0.5f;
        tR *= 0.5f;

        /* La cola alimenta al pitch shifter del proximo sample: por eso
         * cada vuelta sube otra octava y el shimmer crece. */
        shiftBufL_[shiftW_] = Soft(tL);
        shiftBufR_[shiftW_] = Soft(tR);
        shiftW_ = (shiftW_ + 1) & (kShiftLen - 1);

        outL = inL + (tL - inL) * spaceWet_;
        outR = inR + (tR - inR) * spaceWet_;
    }

    /* ── Carga de preset ───────────────────────────────────────── */
    void Load(float dens, float dur, float foc, float ap, uint8_t mode, uint8_t chord,
              float pitch, float octp, float wid, uint8_t mat, float matAmt,
              float shim, float shTilt, float space, float spSize, float damp,
              float varAmt, float varRt, uint8_t fDepth, uint8_t fSeeds,
              float fDrift, float fSpread, uint8_t bDepth, float bProb,
              float win, float aber, float drv, uint8_t smart, float fb,
              float cut, float res, float vol)
    {
        density_ = dens;      grainDur_ = dur;      focus_ = foc;
        aperture_ = ap;       sampleMode_ = mode;   chord_ = chord;
        pitchSemi_ = pitch;   octProb_ = octp;      width_ = wid;
        material_ = mat;      materialAmt_ = matAmt;
        shimmerAmt_ = shim;   shimmerTilt_ = shTilt;
        spaceWet_ = space;    spaceSize_ = spSize;  damping_ = damp;
        variation_ = varAmt;  varRate_ = varRt;
        fociDepth_ = fDepth;  fociSeeds_ = fSeeds;
        fociDrift_ = fDrift;  fociSpread_ = fSpread;
        bounceDepth_ = bDepth; bounceProb_ = bProb;
        windowShape_ = win;   aberration_ = aber;   drive_ = drv;
        smartRays_ = smart;   feedback_ = fb;
        cutoff_ = cut;        resonance_ = res;    volume_ = vol;
        RebuildChord();
        UpdateModes();
        UpdateFilter();
        fociDirty_ = true;
    }
};

} /* namespace RayDrone */

#ifdef RAYDRONE_LOCAL_SDRAM_DEF
#undef DSY_SDRAM_BSS
#undef RAYDRONE_LOCAL_SDRAM_DEF
#endif
