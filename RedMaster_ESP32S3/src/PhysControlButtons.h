#pragma once
/*
 * PhysControlButtons.h  v2.0 — configurable via web UI
 * =====================================================
 * 4 botones táctiles con LEDs WS2812B (NeoPixel).
 *
 * Pines (por defecto — hardcoded):
 *   GPIO 10 → Botón 0     GPIO 11 → Botón 1
 *   GPIO 12 → Botón 2     GPIO 13 → Botón 3
 *   GPIO 14 → DIN cadena WS2812B (4 píxeles, uno por botón)
 *
 * INPUT_PULLDOWN: HIGH = pulsado (ver begin() en el .cpp).
 * El color y la función de cada botón son configurables desde la web.
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <functional>

// ── Pines ─────────────────────────────────────────────────────────────────
#define CTRL_BTN_0       10
#define CTRL_BTN_1       11
#define CTRL_BTN_2       12
#define CTRL_BTN_3       13
#define CTRL_LED_PIN     14
#define CTRL_LED_COUNT   4
#define CTRL_DEBOUNCE_MS 50
#define CTRL_FLASH_MS   180
#define CTRL_BRIGHTNESS  80   // 0-255

// ── Colores por defecto (0xRRGGBB) ────────────────────────────────────────
#define CTRL_CLR_RED     0xFF0000UL
#define CTRL_CLR_GREEN   0x00FF00UL
#define CTRL_CLR_CYAN    0x00FFFFFFL
#define CTRL_CLR_ORANGE  0xFF5500UL
#define CTRL_CLR_PURPLE  0xAA00FFUL
#define CTRL_CLR_BLUE    0x0088FFUL
#define CTRL_CLR_YELLOW  0xFFDD00UL
#define CTRL_CLR_OFF     0x000000UL

// ══════════════════════════════════════════════════════════════════════════
//  IDs de funciones asignables a cada botón
//  (deben coincidir con BTN_ACTIONS en app.js)
// ══════════════════════════════════════════════════════════════════════════
#define BTN_FUNC_NONE           0
// ── Transporte ──────────────────────────────────────────────────────
#define BTN_FUNC_PLAY_PAUSE     1
#define BTN_FUNC_STOP           2
#define BTN_FUNC_NEXT_PATTERN   3
#define BTN_FUNC_PREV_PATTERN   4
#define BTN_FUNC_TAP_TEMPO      5
#define BTN_FUNC_NEXT_PAT_PLAY  6   // avanzar patrón y reproducir
#define BTN_FUNC_PREV_PAT_PLAY  7   // retroceder patrón y reproducir
// ── Navegación ──────────────────────────────────────────────────────
#define BTN_FUNC_MULTIVIEW      8
// ── Volumen ─────────────────────────────────────────────────────────
#define BTN_FUNC_MASTER_VOL_UP  10
#define BTN_FUNC_MASTER_VOL_DN  11
#define BTN_FUNC_LIVE_VOL_UP    12
#define BTN_FUNC_LIVE_VOL_DN    13
// ── Tempo ───────────────────────────────────────────────────────────
#define BTN_FUNC_TEMPO_UP1      20
#define BTN_FUNC_TEMPO_DN1      21
#define BTN_FUNC_TEMPO_UP5      22
#define BTN_FUNC_TEMPO_DN5      23
// ── FX Master (toggle on/off) ────────────────────────────────────────
#define BTN_FUNC_DELAY_TOGGLE   30
#define BTN_FUNC_REVERB_TOGGLE  31
#define BTN_FUNC_CHORUS_TOGGLE  32
#define BTN_FUNC_PHASER_TOGGLE  33
#define BTN_FUNC_FLANGER_TOGGLE 34
#define BTN_FUNC_COMP_TOGGLE    35
#define BTN_FUNC_TREMOLO_TOGGLE 36
#define BTN_FUNC_LIMITER_TOGGLE 37
#define BTN_FUNC_DIST_TOGGLE    38
// ── Filtro global ────────────────────────────────────────────────────
#define BTN_FUNC_FILTER_CYCLE   40
#define BTN_FUNC_CUTOFF_UP      41
#define BTN_FUNC_CUTOFF_DN      42
#define BTN_FUNC_RES_UP         43
#define BTN_FUNC_RES_DN         44
// ── Mute/Solo ───────────────────────────────────────────────────────
#define BTN_FUNC_MUTE_ALL       50
#define BTN_FUNC_UNMUTE_ALL     51
// ── Patrón ──────────────────────────────────────────────────────────
#define BTN_FUNC_PAT_LEN_CYCLE  60   // cicla 16→32→64
#define BTN_FUNC_PATTERN_0      61
#define BTN_FUNC_PATTERN_1      62
#define BTN_FUNC_PATTERN_2      63
#define BTN_FUNC_PATTERN_3      64
#define BTN_FUNC_PATTERN_4      65
#define BTN_FUNC_PATTERN_5      66
#define BTN_FUNC_PATTERN_6      67
#define BTN_FUNC_PATTERN_7      68
// ── Live Pads (disparar pad 0-15) ───────────────────────────
#define BTN_FUNC_LIVE_PAD_0     70
#define BTN_FUNC_LIVE_PAD_1     71
#define BTN_FUNC_LIVE_PAD_2     72
#define BTN_FUNC_LIVE_PAD_3     73
#define BTN_FUNC_LIVE_PAD_4     74
#define BTN_FUNC_LIVE_PAD_5     75
#define BTN_FUNC_LIVE_PAD_6     76
#define BTN_FUNC_LIVE_PAD_7     77
#define BTN_FUNC_LIVE_PAD_8     78
#define BTN_FUNC_LIVE_PAD_9     79
#define BTN_FUNC_LIVE_PAD_10    80
#define BTN_FUNC_LIVE_PAD_11    81
#define BTN_FUNC_LIVE_PAD_12    82
#define BTN_FUNC_LIVE_PAD_13    83
#define BTN_FUNC_LIVE_PAD_14    84
#define BTN_FUNC_LIVE_PAD_15    85
// ── XTRA Pads (disparar xtra 0-7, índices 16-23) ───────────────
#define BTN_FUNC_XTRA_PAD_0     90
#define BTN_FUNC_XTRA_PAD_1     91
#define BTN_FUNC_XTRA_PAD_2     92
#define BTN_FUNC_XTRA_PAD_3     93
#define BTN_FUNC_XTRA_PAD_4     94
#define BTN_FUNC_XTRA_PAD_5     95
#define BTN_FUNC_XTRA_PAD_6     96
#define BTN_FUNC_XTRA_PAD_7     97

// ══════════════════════════════════════════════════════════════════════════
//  Configuración de un botón
// ══════════════════════════════════════════════════════════════════════════
struct BtnCfg {
    uint8_t  funcId   = BTN_FUNC_NONE;
    uint32_t colorOff = CTRL_CLR_RED;    // LED cuando está "desactivado"
    uint32_t colorOn  = CTRL_CLR_GREEN;  // LED cuando está "activado"
    char     label[20] = "BUTTON";
};

// ══════════════════════════════════════════════════════════════════════════
//  Clase principal
// ══════════════════════════════════════════════════════════════════════════
class PhysControlButtons {
public:
    PhysControlButtons();

    /** Inicializa pines y LEDs. Llamar en setup() DESPUÉS de ctrlButtons.setCfg(). */
    void begin();

    /**
     * Leer botones + actualizar LEDs. Llamar en el loop/task (no bloqueante).
     * IMPORTANTE: llamar desde el MISMO CORE que maneja rgbLed para evitar
     * conflictos de temporización RMT (Core 0 / systemTask).
     */
    void update();

    // ── Configuración de botones ───────────────────────────────────────
    void setCfg(int idx, const BtnCfg& cfg);
    const BtnCfg& getCfg(int idx) const { return _cfg[idx]; }

    /** Actualiza el LED de un botón según si está activo o no. */
    void setLedState(int idx, bool active);

    /** Destella un LED brevemente (regresa a su color base después). */
    void flashLed(int idx, uint32_t color = CTRL_CLR_ORANGE);

    /** Fuerza actualización inmediata de todos los LEDs (basándonos en _baseColor). */
    void refreshLeds();

    // ── Callback genérico ─────────────────────────────────────────────
    /**
     * Se llama cuando se pulsa un botón.
     * Parámetros: índice del botón (0-3), funcId asignado.
     */
    std::function<void(int btnIdx, uint8_t funcId)> onAction;

private:
    Adafruit_NeoPixel _leds;

    BtnCfg _cfg[4];

    struct BtnState {
        uint8_t  pin;
        bool     lastRead;
        bool     stable;
        bool     pressed;
        uint32_t debounceEnd;
    } _btn[4];

    uint32_t _baseColor[CTRL_LED_COUNT];
    bool     _flashActive[CTRL_LED_COUNT];
    uint32_t _flashEnd[CTRL_LED_COUNT];

    void _scanButton(int idx);
    void _processFlashes();
};
