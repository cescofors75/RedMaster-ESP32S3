#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "SPIMaster.h"
#include "SampleManager.h"
#include "Sequencer.h"
#include "PatternBank.h"
#include "WebInterface.h"
#include "MIDIController.h"
#include "SysLog.h"
#if ENABLE_PHYSICAL_BUTTONS
#include "PhysControlButtons.h"
#endif

#ifndef ENABLE_PHYSICAL_BUTTONS
#define ENABLE_PHYSICAL_BUTTONS 1
#endif

// LED RGB integrado ESP32-S3
#define RGB_LED_PIN  48
#define RGB_LED_NUM  1



// --- WiFi: Red Doméstica (modo STA) ---
// Pon aquí tu SSID y contraseña WiFi de casa.
// Si se deja vacío (""), usará solo modo AP (red propia RED808).
#define HOME_WIFI_SSID     ""       // vacío = solo modo AP (RED808)
#define HOME_WIFI_PASS     ""   

#define HOME_WIFI_TIMEOUT  12000      // ms para intentar conectar (12s)

// AP fallback (siempre disponible si STA falla)
// WPA2 con password: aunque parezca contradictorio, en Windows 10/11 una red
// ABIERTA falla al asociar ("No es posible conectarse a esta red") por la
// detección de portal cautivo. WPA2 es MÁS compatible y sólo añade ~100ms de
// handshake. La velocidad la dan canal 11 + sin auth HTTP + orden de arranque.
#define AP_SSID     "RED808"
#define AP_PASSWORD "red808esp32"


// Daisy-first workflow: samples se gestionan desde SD en Daisy vía comandos SD_*.
// La precarga local desde LittleFS quedó eliminada (Daisy carga directamente).

#ifndef RED808_MASTER_SPI_TRIGGER_TEST
#define RED808_MASTER_SPI_TRIGGER_TEST 0
#endif

#ifndef RED808_MASTER_UART0_DEBUG
#define RED808_MASTER_UART0_DEBUG 0
#endif

#if RED808_MASTER_UART0_DEBUG
HardwareSerial debugUart(0);
#define DBG_PRINTLN(msg) debugUart.println(msg)
#define DBG_PRINTF(...) debugUart.printf(__VA_ARGS__)
#else
#define DBG_PRINTLN(msg) do {} while (0)
#define DBG_PRINTF(...) do {} while (0)
#endif

// --- OBJETOS GLOBALES ---
// NOTE: Sequencer's large pattern arrays (~229 KB) are allocated from PSRAM
// via ps_calloc() inside the Sequencer constructor — see Sequencer.cpp.
SPIMaster spiMaster;
SampleManager sampleManager;
Sequencer sequencer;
WebInterface webInterface;
MIDIController midiController;
Adafruit_NeoPixel rgbLed(RGB_LED_NUM, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

#if ENABLE_PHYSICAL_BUTTONS
// ── Botones físicos con LED RGB ──────────────────────────────────
PhysControlButtons ctrlButtons;
bool gMultiviewActive = false;   // estado actual del panel multiview
// Estado de FX master para toggles desde botones físicos
bool gDelayActive    = false;
bool gReverbActive   = false;
bool gChorusActive   = false;
bool gPhaserActive   = false;
bool gFlangerActive  = false;
bool gCompActive     = false;
bool gTremoloActive  = false;
bool gLimiterActive  = true;
bool gDistActive     = false;
#endif
// Track synth engine map for sequencer tracks:
// -1 = sample, 0 = 808, 1 = 909, 2 = 505, 3 = 303, 4=WT, 5=SH101, 6=FM, 7=PHYS, 8=NOISE
// volatile: written from Core0 (WS handler), read from Core1 (stepCallback)
volatile int8_t gTrackSynthEngine[16] = {
    -1, -1, -1, -1,
    -1, -1, -1, -1,
    -1, -1, -1, -1,
    -1, -1, -1, -1
};

void setTrackSynthEngine(int track, int8_t engine) {
    if (track < 0 || track >= 16) return;
    if (engine < -1 || engine > 8) return;  // -1, 0..8 valid engine IDs
    gTrackSynthEngine[track] = engine;
    // Memory barrier to ensure Core1 sees the write immediately
    __asm__ __volatile__("memw" ::: "memory");
}

void setAllTrackSynthEngines(int8_t engine) {
    if (engine < -1 || engine > 8) return;  // Daisy supports 0..8
    for (int i = 0; i < 16; i++) {
        gTrackSynthEngine[i] = engine;
    }
}

int8_t getTrackSynthEngine(int track) {
    if (track < 0 || track >= 16) return -1;
    return gTrackSynthEngine[track];
}

static const uint8_t PAD_303_NOTES[16] = {
    48, 50, 52, 53, 55, 57, 59, 60,
    62, 64, 65, 67, 69, 71, 72, 74
};

static bool   gSeqMelodicHeld[16] = {false};
static int8_t gSeqMelodicHeldEngine[16] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                           -1, -1, -1, -1, -1, -1, -1, -1};

// Protege gSeqMelodicHeld*/gPadTrigOff*: se mutan desde Core0 (web/MIDI via
// triggerPadWithLED; comando stop de WebInterface llama
// releaseSequencerMelodicHolds desde AsyncTCP) y Core1 (callbacks del
// sequencer, drainPadTriggerAutoOff en loop()). El read-modify-write sin
// proteccion podia mandar NoteOffs espurios o dejar voces colgadas.
// Las llamadas SPI se hacen SIEMPRE fuera de la seccion critica.
static portMUX_TYPE gPadNoteMux = portMUX_INITIALIZER_UNLOCKED;

/* ─────────────────────────────────────────────────────────────────────────
 *  Pad-trigger auto note-off (web UI / MIDI / live pad taps).
 *  triggerPadWithLED() fires CMD_SYNTH_NOTE_ON_EX for engines >= 4
 *  (WT/SH101/FM2/PHYS/NOISE) but the call site never schedules the
 *  matching NoteOff. On the polyphonic WT this stacks voices on every tap;
 *  on the mono engines (SH101/FM2) the release tail of the previous voice
 *  bleeds into the new attack — what shows up as "se solapan / patrulla de
 *  caballos". We mirror the per-track release the sequencer callback
 *  already does, but for live triggers.
 * ─────────────────────────────────────────────────────────────────────── */
static uint32_t gPadTrigOffAtMs[16]   = {0};
static int8_t   gPadTrigOffEngine[16] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                          -1, -1, -1, -1, -1, -1, -1, -1};
static uint8_t  gPadTrigOffNote[16]   = {0};
static const uint32_t kPadTrigAutoOffMs = 220;

static void releasePadTriggerNote(int track) {
    if (track < 0 || track >= 16) return;
    // Reclamar la entrada atomicamente: solo UN contexto (Core0 trigger o
    // Core1 drain) obtiene el par engine/note y manda el NoteOff.
    portENTER_CRITICAL(&gPadNoteMux);
    int8_t eng = gPadTrigOffEngine[track];
    uint8_t note = gPadTrigOffNote[track];
    gPadTrigOffAtMs[track]   = 0;
    gPadTrigOffEngine[track] = -1;
    portEXIT_CRITICAL(&gPadNoteMux);
    if (eng < 0) return;
    if (eng == 3) {
        spiMaster.synth303NoteOff();
    } else if (eng >= 4 && eng <= 8) {
        spiMaster.synthNoteOff((uint8_t)eng, (uint8_t)track, note);
    }
}

static void schedulePadTriggerAutoOff(int track, int8_t engine, uint8_t note) {
    if (track < 0 || track >= 16) return;
    portENTER_CRITICAL(&gPadNoteMux);
    gPadTrigOffEngine[track] = engine;
    gPadTrigOffNote[track]   = note;
    gPadTrigOffAtMs[track]   = millis() + kPadTrigAutoOffMs;
    portEXIT_CRITICAL(&gPadNoteMux);
}

static void drainPadTriggerAutoOff() {
    uint32_t now = millis();
    for (int t = 0; t < 16; t++) {
        if (!gPadTrigOffAtMs[t]) continue;
        if ((int32_t)(now - gPadTrigOffAtMs[t]) >= 0) {
            releasePadTriggerNote(t);
        }
    }
}

void releaseSequencerMelodicHolds() {
    for (int track = 0; track < 16; track++) {
        // Reclamo atomico por track; el NoteOff (SPI) va fuera del mux.
        portENTER_CRITICAL(&gPadNoteMux);
        bool held = gSeqMelodicHeld[track];
        int8_t engine = gSeqMelodicHeldEngine[track];
        if (held) {
            gSeqMelodicHeld[track] = false;
            gSeqMelodicHeldEngine[track] = -1;
        }
        portEXIT_CRITICAL(&gPadNoteMux);
        if (!held) continue;
        if (engine == 3) {
            spiMaster.synth303NoteOff();
        } else if (engine >= 4 && engine <= 8) {
            spiMaster.synthNoteOff((uint8_t)engine, (uint8_t)track);
        }
    }
}

// Colores por instrumento - 16 instrumentos RGB (formato 0xRRGGBB estándar)
const uint32_t instrumentColors[16] = {
    0xFF0000,  // 0: BD (KICK) - Rojo
    0xFFA500,  // 1: SD (SNARE) - Naranja
    0xFFFF00,  // 2: CH (CL-HAT) - Amarillo
    0x00FFFF,  // 3: OH (OP-HAT) - Cian
    0xE6194B,  // 4: CY (CYMBAL) - Carmín
    0xFF00FF,  // 5: CP (CLAP) - Magenta
    0x00FF00,  // 6: RS (RIMSHOT) - Verde
    0xF58231,  // 7: CB (COWBELL) - Naranja oscuro
    0x911EB4,  // 8: LT (LOW TOM) - Púrpura
    0x46F0F0,  // 9: MT (MID TOM) - Turquesa
    0xF032E6,  // 10: HT (HIGH TOM) - Rosa
    0xBCF60C,  // 11: MA (MARACAS) - Lima
    0x38CEFF,  // 12: CL (CLAVES) - Azul claro
    0xFABEBE,  // 13: HC (HI CONGA) - Rosa pálido
    0x008080,  // 14: MC (MID CONGA) - Teal
    0x484DFF   // 15: LC (LOW CONGA) - Azul índigo
};

// === FUNCIONES DE SECUENCIA LED ===
void showBootLED() {
    // Púrpura BRILLANTE: Inicio del sistema
    rgbLed.setBrightness(255);
    rgbLed.setPixelColor(0, 0xFF00FF); // Magenta más brillante que púrpura
    rgbLed.show();
}

void showLoadingSamplesLED() {
    // Amarillo BRILLANTE: Cargando samples
    rgbLed.setBrightness(255);
    rgbLed.setPixelColor(0, 0xFFFF00);
    rgbLed.show();
}

void showWiFiLED() {
    // Azul BRILLANTE: WiFi activándose
    rgbLed.setBrightness(255);
    rgbLed.setPixelColor(0, 0x0080FF);
    rgbLed.show();
}

void showWebServerLED() {
    // Verde BRILLANTE: Servidor web listo
    rgbLed.setBrightness(255);
    rgbLed.setPixelColor(0, 0x00FF00);
    rgbLed.show();
}

void showReadyLED() {
    // Blanco brillante: Sistema listo
    rgbLed.setPixelColor(0, 0xFFFFFF);
    rgbLed.setBrightness(255);
    rgbLed.show();
    delay(2000); // 2 segundos para ver que está listo
    // Apagar
    rgbLed.clear();
    rgbLed.show();
}

// Variables para control del LED RGB fade
volatile uint8_t ledBrightness = 0;
volatile bool ledFading = false;
volatile bool ledMonoMode = false;

void setLedMonoMode(bool enabled) {
    ledMonoMode = enabled;
}

// --- TASKS (CORE PINNING) ---

// ── Triggers eliminados: el secuenciador ahora corre en Daisy Seed ──
// Daisy dispara los samples sample-accurately internamente (DsqFireStep).
// ESP32 solo sube el patrón una vez y envía CMD_DSQ_CONTROL play/stop.

// ── Deferred upload: Core0 sets flag, Core1 executes ──
static portMUX_TYPE _pendingDsqMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int8_t _pendingDsqUpload = -1;   // pattern index, -1 = idle
static volatile bool   _pendingDsqSelect = false;
static volatile int8_t _pendingDsqSelectOnly = -1; // pattern index for select-only path
static volatile int8_t _pendingDsqQueue = -1;      // pattern index for next bar
static volatile uint8_t _pendingDsqQueueBars = 0;
static volatile uint8_t _pendingNativeTransitions = 0;
static volatile bool _pendingDemoSetLaunch = false;
static volatile bool _nativeDemoSetActive = false;
static volatile bool _pendingDsqQueueCancel = false;
static volatile bool   _pendingDsqPlay = false;    // arrancar Daisy tras upload (ordenado)
static int16_t _daisySlotMasterPattern[DSQ_PATTERNS];
static uint32_t _daisySlotLastUse[DSQ_PATTERNS] = {};
static uint32_t _daisyResidentClock = 0;
static int8_t _activeDaisySlot = -1;

static int clampMasterPattern(int pattern) {
    return constrain(pattern, 0, MAX_PATTERNS - 1);
}

int dsqGetResidentSlot(int masterPattern) {
    masterPattern = clampMasterPattern(masterPattern);
    int slot = -1;
    portENTER_CRITICAL(&_pendingDsqMux);
    for (int i = 0; i < DSQ_PATTERNS; ++i) {
        if (_daisySlotMasterPattern[i] == masterPattern) {
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&_pendingDsqMux);
    return slot;
}

void dsqUploadPatternDeferred(int pattern) {
    pattern = clampMasterPattern(pattern);
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelect = true;
    _pendingDsqUpload = (int8_t)pattern;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Solo selecciona patrón en Daisy (1 cmd SPI). NO reuploadea steps.
// Usar cuando los patrones ya están en Daisy y solo queremos cambiar el activo.
void dsqSelectPatternDeferred(int pattern) {
    pattern = clampMasterPattern(pattern);
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelectOnly = (int8_t)pattern;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

void dsqPreparePatternDeferred(int pattern) {
    pattern = clampMasterPattern(pattern);
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelect = false;
    _pendingDsqUpload = (int8_t)pattern;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

void dsqQueuePatternDeferred(int pattern, uint8_t bars) {
    pattern = clampMasterPattern(pattern);
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqQueue = (int8_t)pattern;
    _pendingDsqQueueBars = constrain(bars, 0, 16);
    _pendingNativeTransitions = bars > 0 ? 2 : 1;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

void dsqCancelPatternQueueDeferred() {
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqQueue = -1;
    _pendingDsqQueueBars = 0;
    _pendingNativeTransitions = 0;
    _pendingDsqQueueCancel = true;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

static bool dsqConsumeNativeTransition() {
    bool native = false;
    portENTER_CRITICAL(&_pendingDsqMux);
    if (_nativeDemoSetActive) {
        native = true;
    } else if (_pendingNativeTransitions > 0) {
        _pendingNativeTransitions = (uint8_t)(_pendingNativeTransitions - 1u);
        native = true;
    }
    portEXIT_CRITICAL(&_pendingDsqMux);
    return native;
}

void dsqLaunchDemoSetDeferred() {
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDemoSetLaunch = true;
    _pendingNativeTransitions = 0;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Sube patrón y al terminar arranca Daisy. Garantiza orden upload → select → play.
void dsqUploadAndPlayDeferred(int pattern) {
    pattern = clampMasterPattern(pattern);
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelect = true;
    _pendingDsqUpload = (int8_t)pattern;
    _pendingDsqPlay   = true;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Convierte un patrón de la biblioteca Master en un slot físico de la Daisy.
// Nunca se entrega un índice Master directamente al protocolo de 16 slots.
static void dsqUploadPatternToSlot(int masterPattern, int daisySlot) {
    masterPattern = clampMasterPattern(masterPattern);
    daisySlot = constrain(daisySlot, 0, DSQ_PATTERNS - 1);
    const int stepCount = sequencer.getPatternLength();  // longitud global
    const int clampedLen = (stepCount >= 64) ? 64 : (stepCount >= 32) ? 32 : 16;
    DsqStepPkt pkt[DSQ_MAX_STEPS];
    // static: dsqUploadPattern solo se invoca desde spiAudioTask (Core1), nunca
    // de forma reentrante, asi que un buffer estatico evita ~700B de stack.
    static StepUploadData snap[STEPS_PER_PATTERN];
    spiMaster.dsqSetLength((uint8_t)clampedLen);
    vTaskDelay(pdMS_TO_TICKS(5));
    for (int trk = 0; trk < DSQ_TRACKS; trk++) {
        // Copia consistente de la pista bajo un solo lock. Antes se leia con
        // getters sin lock mientras la web editaba el patron, produciendo
        // uploads desgarrados (sintoma: pistas/patrones parciales).
        sequencer.snapshotTrackForUpload(masterPattern, trk, clampedLen, snap);
        for (int s = 0; s < clampedLen; s++) {
            pkt[s].active      = snap[s].active ? 1 : 0;
            pkt[s].velocity    = snap[s].velocity;
            const uint8_t length = snap[s].noteLenDiv & 0x0F;
            const uint8_t ratchet = constrain(snap[s].ratchet, 1, 4);
            pkt[s].noteLenDiv  = length | ((ratchet - 1) << 4);
            pkt[s].probability = snap[s].probability;
        }
        if (!spiMaster.dsqUploadTrack((uint8_t)daisySlot, (uint8_t)trk, pkt, (uint8_t)clampedLen)) {
            Serial.printf("[DSQ] upload track %d master %d -> slot %d failed\n", trk, masterPattern, daisySlot);
        }
        // 8 ms entre tracks: deja a la Daisy procesar el packet anterior
        // y vaciar su SPI RX ring antes del siguiente. Evita spiRingDrops y
        // tracks parciales (síntoma: patrones con pistas vacías).
        vTaskDelay(pdMS_TO_TICKS(8));
        // Las notas/flags melódicas viajan aparte para conservar el paquete
        // histórico de 4 bytes. Daisy limpia las notas al recibir la pista;
        // solo enviamos los steps que realmente contienen información musical.
        for (int s = 0; s < clampedLen; s++) {
            bool hasNotes = false;
            for (int v = 0; v < MELODY_STEP_VOICES; v++) {
                if (snap[s].noteVoices[v] != 0) { hasNotes = true; break; }
            }
            if (hasNotes || snap[s].flags != 0) {
                spiMaster.dsqSetStepNotes((uint8_t)daisySlot, (uint8_t)trk,
                                          (uint8_t)s, snap[s].flags,
                                          snap[s].noteVoices);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        // Param locks: usar las flags 'enabled' reales del snapshot. Antes se
        // inferia del valor (ch!=1000, rv!=0, vl!=0), lo que descartaba un
        // cutoff legitimo de 1000 Hz o un volumen 0 (bug M15).
        for (int s = 0; s < clampedLen; s++) {
            bool     ce = snap[s].cutoffEn;
            uint16_t ch = snap[s].cutoffHz;
            bool     re = snap[s].reverbEn;
            uint8_t  rv = snap[s].reverbSend;
            bool     ve = snap[s].volumeEn;
            uint8_t  vl = snap[s].volume;
            if (ce || re || ve) {
                spiMaster.dsqSetParamLock(
                    (uint8_t)daisySlot, (uint8_t)trk, (uint8_t)s,
                    ce, ch, re, rv, ve, vl
                );
            }
        }
    }
}

static int chooseDaisySlot(int masterPattern) {
    const int resident = dsqGetResidentSlot(masterPattern);
    if (resident >= 0) return resident;
    for (int i = 0; i < DSQ_PATTERNS; ++i) {
        if (_daisySlotMasterPattern[i] < 0) return i;
    }
    int oldestSlot = (_activeDaisySlot == 0) ? 1 : 0;
    for (int i = 0; i < DSQ_PATTERNS; ++i) {
        if (i == _activeDaisySlot) continue;
        if (_daisySlotLastUse[i] < _daisySlotLastUse[oldestSlot]) oldestSlot = i;
    }
    return oldestSlot;
}

static int ensurePatternResident(int masterPattern, bool forceUpload) {
    masterPattern = clampMasterPattern(masterPattern);
    int slot = dsqGetResidentSlot(masterPattern);
    if (slot < 0) slot = chooseDaisySlot(masterPattern);
    if (forceUpload || _daisySlotMasterPattern[slot] != masterPattern) {
        dsqUploadPatternToSlot(masterPattern, slot);
        portENTER_CRITICAL(&_pendingDsqMux);
        _daisySlotMasterPattern[slot] = masterPattern;
        portEXIT_CRITICAL(&_pendingDsqMux);
    }
    _daisySlotLastUse[slot] = ++_daisyResidentClock;
    return slot;
}

static void applyPatternPerformance(int masterPattern) {
    PatternMetadata metadata{};
    if (!sequencer.getPatternMetadata(masterPattern, metadata)) return;
    if (metadata.recommendedBpm >= 40 && metadata.recommendedBpm <= 240) {
        sequencer.setTempo((float)metadata.recommendedBpm);
        spiMaster.setTempo((float)metadata.recommendedBpm);
    }
    sequencer.setHumanize(metadata.humanizeTimingMs, metadata.humanizeVelocity);
    spiMaster.dsqSetSwing(metadata.swing);
    spiMaster.dsqSetHumanize(metadata.humanizeTimingMs, metadata.humanizeVelocity);

    BuiltinPatternSoundProfile sound{};
    if (getBuiltinPatternSoundProfile(masterPattern, sound)) {
        for (int engine = 0; engine < BUILTIN_ENGINE_COUNT; engine++) {
            spiMaster.synthPreset((uint8_t)engine, sound.presets[engine]);
        }
        for (int track = 0; track < MAX_TRACKS; track++) {
            setTrackSynthEngine(track, sound.engines[track]);
            spiMaster.dsqSetTrackEngine((uint8_t)track, sound.engines[track]);
            spiMaster.dsqSetMute((uint8_t)track, false);
        }
    }
}

// API histórica: ahora fuerza la actualización del slot residente correcto.
void dsqUploadPattern(int masterPattern) {
    ensurePatternResident(masterPattern, true);
}

// CORE 1: Sequencer UI + SPI Master
// El secuenciador corre en Daisy Seed; aquí solo actualizamos estado UI y LFO.
// En Arduino/IDF algunas tareas (y loopTask en ciertas configuraciones) ya no
// quedan suscritas al TWDT. reset() desde una tarea ajena no alimenta nada y
// además escribe "task not found" por UART, lo que roba tiempo al Wi-Fi.
static inline void feedTaskWdtIfSubscribed() {
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

void spiAudioTask(void *pvParameters) {
    esp_task_wdt_add(NULL);  // subscribe to TWDT

    for (int slot = 0; slot < DSQ_PATTERNS; ++slot) {
        _daisySlotMasterPattern[slot] = -1;
        _daisySlotLastUse[slot] = 0;
    }

    // El banco activo ocupa sus slots residentes al arrancar (20 en el banco
    // factory; 16 si LittleFS falta y seguimos con el fallback integrado).
    // 20ms de gap entre patrones para que la Daisy procese cada lote
    // antes de empezar el siguiente — evita que comandos posteriores caigan
    // al vacío y dejen slots con datos parciales (sintoma: patrones que no suenan).
    int startupPatternCount = getConfiguredPatternCount();
    if (startupPatternCount <= 0) startupPatternCount = BUILTIN_PATTERN_COUNT;
    startupPatternCount = constrain(startupPatternCount, 1, DSQ_PATTERNS);
    for (int pat = 0; pat < startupPatternCount; pat++) {
        dsqUploadPatternToSlot(pat, pat);
        _daisySlotMasterPattern[pat] = pat;
        _daisySlotLastUse[pat] = ++_daisyResidentClock;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    // Seleccionar patrón activo en Daisy
    const int initialMasterPattern = sequencer.getCurrentPattern();
    _activeDaisySlot = ensurePatternResident(initialMasterPattern, false);
    spiMaster.dsqSelectPattern((uint8_t)_activeDaisySlot);
    applyPatternPerformance(initialMasterPattern);
    // Sync tempo inicial a secuenciador Daisy
    spiMaster.setTempo((float)sequencer.getTempo());

    // ── Sync synth engines al arrancar ──────────
    for (int t = 0; t < 16; t++) {
        spiMaster.dsqSetTrackEngine((uint8_t)t, gTrackSynthEngine[t]);
        // Asegurar que NINGUN track quede muteado al arrancar: la Daisy
        // ya rutea por dsqTrackEngine[] (samplers/synths) y el mute es estado
        // de usuario. Mutear aqui por engine dejaba al secuenciador silencioso
        // cuando un track cambiaba a sampler (-1) tras boot.
        spiMaster.dsqSetMute((uint8_t)t, false);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    while (true) {
        // ── Check deferred pattern upload from Core0 ──
        int8_t pat;
        bool selectAfterUpload;
        bool playAfterUpload;
        portENTER_CRITICAL(&_pendingDsqMux);
        pat = _pendingDsqUpload;
        if (pat >= 0) {
            _pendingDsqUpload = -1;
            selectAfterUpload = _pendingDsqSelect;
            _pendingDsqSelect = false;
            playAfterUpload = _pendingDsqPlay;
            _pendingDsqPlay = false;
        } else {
            selectAfterUpload = false;
            playAfterUpload = false;
        }
        portEXIT_CRITICAL(&_pendingDsqMux);
        if (pat >= 0) {
            const int slot = ensurePatternResident(pat, true);
            if (selectAfterUpload) {
                spiMaster.dsqSelectPattern((uint8_t)slot);
                _activeDaisySlot = slot;
                applyPatternPerformance(pat);
            }
            if (playAfterUpload) {
                spiMaster.dsqControl(1);  // PLAY al final, en orden garantizado
            }
        }

        // ── Select-only path (1 cmd SPI, no reupload) ──
        int8_t selOnly;
        portENTER_CRITICAL(&_pendingDsqMux);
        selOnly = _pendingDsqSelectOnly;
        _pendingDsqSelectOnly = -1;
        portEXIT_CRITICAL(&_pendingDsqMux);
        if (selOnly >= 0) {
            const int slot = ensurePatternResident(selOnly, false);
            spiMaster.dsqSelectPattern((uint8_t)slot);
            _activeDaisySlot = slot;
            applyPatternPerformance(selOnly);
        }

        // Native Daisy queue: target is made resident first, then Daisy stores
        // the slot and commits it sample-accurately before firing step 0.
        int8_t queuedMaster;
        uint8_t queuedBars;
        portENTER_CRITICAL(&_pendingDsqMux);
        queuedMaster = _pendingDsqQueue;
        queuedBars = _pendingDsqQueueBars;
        _pendingDsqQueue = -1;
        _pendingDsqQueueBars = 0;
        portEXIT_CRITICAL(&_pendingDsqMux);
        if (queuedMaster >= 0) {
            const int slot = ensurePatternResident(queuedMaster, false);
            spiMaster.dsqQueuePattern((uint8_t)slot, queuedBars);
        }
        bool cancelQueue = false;
        portENTER_CRITICAL(&_pendingDsqMux);
        cancelQueue = _pendingDsqQueueCancel;
        _pendingDsqQueueCancel = false;
        portEXIT_CRITICAL(&_pendingDsqMux);
        if (cancelQueue) spiMaster.dsqCancelPatternQueue();

        bool launchDemo = false;
        portENTER_CRITICAL(&_pendingDsqMux);
        launchDemo = _pendingDemoSetLaunch;
        _pendingDemoSetLaunch = false;
        portEXIT_CRITICAL(&_pendingDsqMux);
        if (launchDemo) {
            struct DemoScene { uint8_t pattern; uint8_t bars; };
            // Complete ~80-second audition: every factory scene exactly once,
            // ordered as an energy curve rather than by genre folder.
            static constexpr DemoScene demo20[FACTORY_PATTERN_COUNT] = {
                {11,2}, {8,2},  {3,2},  {16,2}, {2,2},
                {7,2},  {13,2}, {12,2}, {0,2},  {4,2},
                {10,2}, {9,2},  {1,2},  {6,2},  {14,2},
                {15,2}, {17,1}, {5,2},  {18,1}, {19,4}
            };
            Sequencer::SongChainEntry local[FACTORY_PATTERN_COUNT] = {};
            SongEntry daisy[FACTORY_PATTERN_COUNT] = {};
            uint8_t count = 0;
            int configured = getConfiguredPatternCount();
            if (configured <= 0) configured = BUILTIN_PATTERN_COUNT;
            sequencer.stop();
            spiMaster.dsqControl(0);
            for (uint8_t i = 0; i < FACTORY_PATTERN_COUNT; ++i) {
                if (demo20[i].pattern >= configured) continue;
                local[count].pattern = demo20[i].pattern;
                local[count].repeats = demo20[i].bars;
                // Force-refresh every scene from the active S3 bank, then send
                // physical resident slots to Daisy's sample-accurate song chain.
                daisy[count].pattern = (uint8_t)ensurePatternResident(demo20[i].pattern, true);
                daisy[count].repeats = demo20[i].bars;
                ++count;
            }
            if (count > 0) {
                sequencer.songChainUpload(local, count);
                spiMaster.songUpload(daisy, count);
                applyPatternPerformance(local[0].pattern);
                portENTER_CRITICAL(&_pendingDsqMux);
                _nativeDemoSetActive = true;
                portEXIT_CRITICAL(&_pendingDsqMux);
                sequencer.songChainPlay();
                spiMaster.songControl(1);
            }
        }

        sequencer.update();   // Mantiene internos del secuenciador (beat UI, song mode)
        if (_nativeDemoSetActive && !sequencer.isSongChainActive()) {
            portENTER_CRITICAL(&_pendingDsqMux);
            _nativeDemoSetActive = false;
            portEXIT_CRITICAL(&_pendingDsqMux);
        }
        spiMaster.process();

        // Daisy is the audible clock. Poll its real sample-accurate position
        // and publish only changes; P4 no longer has to wait for the slower
        // logical S3 clock or the multi-packet pattern payload.
        static uint32_t lastDaisyPosPollMs = 0;
        static int lastDaisyUiStep = -1;
        static int lastDaisyUiPattern = -1;
        static bool lastDaisyUiPlaying = false;
        const uint32_t posNowMs = millis();
        if (posNowMs - lastDaisyPosPollMs >= 20) {
            lastDaisyPosPollMs = posNowMs;
            uint8_t daisyStep = 0, daisySlot = 0;
            bool daisyPlaying = false;
            if (spiMaster.dsqGetPos(daisyStep, daisySlot, daisyPlaying)) {
                int masterPattern = -1;
                if (daisySlot < DSQ_PATTERNS) {
                    portENTER_CRITICAL(&_pendingDsqMux);
                    masterPattern = _daisySlotMasterPattern[daisySlot];
                    portEXIT_CRITICAL(&_pendingDsqMux);
                }
                if (masterPattern < 0 || masterPattern >= MAX_PATTERNS - 4)
                    masterPattern = sequencer.getPerformancePattern();
                if ((int)daisyStep != lastDaisyUiStep ||
                    masterPattern != lastDaisyUiPattern ||
                    daisyPlaying != lastDaisyUiPlaying) {
                    lastDaisyUiStep = daisyStep;
                    lastDaisyUiPattern = masterPattern;
                    lastDaisyUiPlaying = daisyPlaying;
                    webInterface.publishDaisyTransport(daisyStep, masterPattern, daisyPlaying);
                }
            }
        }

#if RED808_MASTER_SPI_TRIGGER_TEST
        static uint32_t lastDiagTriggerMs = 0;
        static uint8_t diagPad = 0;
        uint32_t nowMs = millis();
        if (nowMs - lastDiagTriggerMs >= 1000) {
            lastDiagTriggerMs = nowMs;
            uint32_t pingUs = 0;
            bool pingOk = spiMaster.ping(pingUs);
            spiMaster.triggerSampleLive(diagPad, 120);
            spiMaster.synthTrigger(0, diagPad, 120);
            rgbLed.setPixelColor(0, (diagPad & 1) ? 0x00FF00 : 0xFF0000);
            rgbLed.show();
            DBG_PRINTF("[SPI_DIAG] pad=%u ping=%u rtt_us=%lu errors=%lu\n",
                       (unsigned)diagPad,
                       pingOk ? 1U : 0U,
                       (unsigned long)pingUs,
                       (unsigned long)spiMaster.getSPIErrors());
            diagPad = (diagPad + 1) & 0x03;
        }
#endif

        feedTaskWdtIfSubscribed();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// CORE 0: WiFi, Web Server, MIDI, LED (Prioridad Media)
void systemTask(void *pvParameters) {
    esp_task_wdt_add(NULL);  // subscribe to TWDT

    uint32_t lastLedUpdate = 0;
#if ENABLE_PHYSICAL_BUTTONS
    uint32_t lastBtnUpdate  = 0;
#endif
    
    while (true) {
        midiController.update();
        webInterface.update();
        webInterface.handleUdp();
#if ENABLE_PHYSICAL_BUTTONS
        // Botones físicos — Core 0 (mismo que rgbLed para evitar conflicto RMT)
        if (millis() - lastBtnUpdate >= 5) {
            lastBtnUpdate = millis();
            ctrlButtons.update();
        }
#endif
        // Fade out del LED después de trigger
        if (ledFading && millis() - lastLedUpdate > 20) {
            lastLedUpdate = millis();
            if (ledBrightness > 10) {
                ledBrightness -= 8;
                rgbLed.setBrightness(ledBrightness);
                rgbLed.show();
            } else {
                rgbLed.clear();
                rgbLed.show();
                ledFading = false;
                ledBrightness = 0;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(2)); // 500Hz system loop - mínima latencia WiFi
        feedTaskWdtIfSubscribed();
    }
}

// onStepTrigger eliminado: el secuenciador corre en Daisy Seed.
// Los triggers sample se disparan en DsqFireStep() dentro del AudioCallback.
// Los synth triggers (808/909/505 sintéticos) siguen siendo controlados por la UI.

// Función para triggers manuales desde live pads (web interface)
// Esta SÍ enciende el LED RGB
void triggerPadWithLED(int track, uint8_t velocity) {
    DBG_PRINTF("[TRIG] pad=%d vel=%d connected=%d\n", track, velocity, (int)spiMaster.isConnected());
    int8_t engine = getTrackSynthEngine(track);
    if (track >= 0 && track < 16 && engine >= 0 && engine <= 8) {
        uint8_t liveVol = spiMaster.getLiveVolume();
        float scaled = (velocity / 127.0f) * (liveVol / 100.0f);
        uint8_t synthVelocity = (uint8_t)constrain((int)roundf(scaled * 127.0f), 1, 127);
        if (engine == 3) {
            uint8_t midiNote = PAD_303_NOTES[track];
            // Cancel any previous voice on this pad so rapid taps don't
            // bleed release tails into the new attack.
            if (gPadTrigOffAtMs[track]) releasePadTriggerNote(track);
            spiMaster.synth303NoteOn(midiNote, false, false);
            schedulePadTriggerAutoOff(track, 3, midiNote);
        } else if (engine >= 4) {
            uint8_t midiNote = PAD_303_NOTES[track];
            // Same protection for WT/SH101/FM2/PHYS/NOISE — the
            // CMD_SYNTH_NOTE_ON_EX handler on Daisy never auto-releases,
            // so we must release the previous voice ourselves and arm
            // an off for this one. Without this, WT's poly voices stack
            // forever and the mono engines bleed release tails.
            if (gPadTrigOffAtMs[track]) releasePadTriggerNote(track);
            spiMaster.synthNoteOnEx((uint8_t)engine, midiNote, synthVelocity, false, false);
            schedulePadTriggerAutoOff(track, engine, midiNote);
        } else {
            spiMaster.synthTrigger((uint8_t)engine, (uint8_t)track, synthVelocity);
        }
    } else {
        spiMaster.triggerSampleLive(track, velocity);
    }
    // Iluminar LED RGB con color del instrumento (solo pads principales 0-15)
    if (track >= 0 && track < 16) {
        uint32_t color = ledMonoMode ? 0xFF0000 : instrumentColors[track];
        ledBrightness = 255;
        ledFading = true;
        rgbLed.setBrightness(ledBrightness);
        rgbLed.setPixelColor(0, color);
        rgbLed.show();
    }
}

static void applyProfessionalMixBaseline() {
    // Global post-master defaults: warm, punchy 808 mix
    spiMaster.setMasterVolume(100);
    spiMaster.setSequencerVolume(95);
    spiMaster.setLiveVolume(100);
    spiMaster.setLivePitchShift(1.0f);

    spiMaster.setFilterType(FILTER_NONE);
    spiMaster.setDelayActive(false);
    spiMaster.setPhaserActive(false);
    spiMaster.setFlangerActive(false);
    spiMaster.setTremoloActive(false);
    spiMaster.setWaveFolderGain(1.0f);

    // Arranque neutro: ningún FX colorea el primer sonido. El músico activa
    // cada proceso desde la UI y todos los niveles empiezan en cero.
    spiMaster.setCompressorActive(false);
    spiMaster.setReverb(false, 0.0f, 200.0f, 0.0f);

    // Chorus off, limiter on
    spiMaster.setChorusActive(false);
    spiMaster.setLimiterActive(true);

    for (int track = 0; track < 16; track++) {
        spiMaster.clearTrackFilter(track);
        spiMaster.clearTrackFX(track);
        spiMaster.clearTrackLiveFX(track);
        spiMaster.setTrackDelaySend(track, 0);
        spiMaster.setTrackChorusSend(track, 0);
        spiMaster.setTrackMute(track, false);
        spiMaster.setTrackSolo(track, false);
        spiMaster.setTrackEq(track, 0, 0, 0);
        spiMaster.setTrackPan(track, 0);
        spiMaster.setTrackReverbSend(track, 0);
        sequencer.setTrackVolume(track, 100);
    }

    // ── Professional 808 mix: niveles y panorámica por instrumento ──
    // track 0=BD, 1=SD, 2=CH, 3=OH, 4=CY, 5=CP, 6=RS, 7=CB
    // track 8=LT, 9=MT, 10=HT, 11=MA, 12=CL, 13=HC, 14=MC, 15=LC
    spiMaster.setTrackVolume(0, 110);   // BD: prominente
    spiMaster.setTrackVolume(1, 100);   // SD
    spiMaster.setTrackVolume(2, 80);    // CH: sutil
    spiMaster.setTrackVolume(3, 75);    // OH: suave
    spiMaster.setTrackVolume(4, 70);    // CY: de fondo
    spiMaster.setTrackVolume(5, 95);    // CP
    spiMaster.setTrackVolume(6, 85);    // RS
    spiMaster.setTrackVolume(7, 80);    // CB

    // Panorámica estéreo para anchura
    spiMaster.setTrackPan(2, -15);    // CH: ligeramente izq
    spiMaster.setTrackPan(3,  20);    // OH: ligeramente der
    spiMaster.setTrackPan(7,  25);    // CB: derecha
    spiMaster.setTrackPan(8, -30);    // LT: izquierda
    spiMaster.setTrackPan(9, -10);    // MT: casi centro izq
    spiMaster.setTrackPan(10, 15);    // HT: centro der
    spiMaster.setTrackPan(13,  35);   // HC: derecha
    spiMaster.setTrackPan(14, -20);   // MC: izquierda
    spiMaster.setTrackPan(15, -35);   // LC: izquierda

    for (int pad = 0; pad < 24; pad++) {
        spiMaster.clearPadFilter(pad);
        spiMaster.clearPadFX(pad);
    }
}

void setup() {
    Serial.begin(115200);
#if RED808_MASTER_UART0_DEBUG
    debugUart.begin(115200, SERIAL_8N1, -1, -1);
    delay(50);
    DBG_PRINTLN("[BOOT] UART0 debug online");
#endif
    rgbLed.begin();
    rgbLed.setBrightness(255);
    showBootLED();
    delay(500);

    // ── Reset reason logging ──
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("[BOOT] Reset reason: %d\n", (int)reason);
    DBG_PRINTF("[BOOT] Reset reason: %d\n", (int)reason);

    // 1. Filesystem
    if (!LittleFS.begin(true)) {
        rgbLed.setPixelColor(0, 0xFF0000);
        rgbLed.show();
        delay(3000);
        ESP.restart();
    }

    syslogBegin();

    // Register shutdown handler to capture crash info
    esp_register_shutdown_handler([]() {
        syslogPanic("shutdown/panic handler fired");
    });

    // Decode reset reason
    const char* rstName = "unknown";
    switch ((int)reason) {
        case 1:  rstName = "POWERON";   break;
        case 3:  rstName = "SW";        break;
        case 4:  rstName = "PANIC";     break;
        case 5:  rstName = "INT_WDT";   break;
        case 6:  rstName = "TASK_WDT";  break;
        case 7:  rstName = "WDT";       break;
        case 8:  rstName = "DEEPSLEEP"; break;
        case 12: rstName = "BROWNOUT";  break;
        case 14: rstName = "USB";       break;
        case 15: rstName = "JTAG";      break;
    }
    syslog("BOOT", "Reset reason: %d (%s)", (int)reason, rstName);
    syslog("BOOT", "Heap: free=%u psram=%u/%u",
           ESP.getFreeHeap(), (uint32_t)ESP.getFreePsram(), (uint32_t)ESP.getPsramSize());

    // NVS ya esta disponible en setup: cargar mapeos MIDI persistidos aqui.
    midiController.loadMappings();

    // 2. SPI Master — connects to STM32 for audio DSP
    if (!spiMaster.begin()) {
        syslog("BOOT", "SPI Master init FAILED — restarting");
        rgbLed.setPixelColor(0, 0xFF0000);
        rgbLed.show();
        delay(3000);
        ESP.restart();
    }
    if (spiMaster.isConnected()) {
        syslog("BOOT", "SPI Master OK - Daisy connected");
    } else {
        syslog("BOOT", "SPI bus initialized, Daisy OFFLINE - audio unavailable until reconnect");
    }
    
    
    showLoadingSamplesLED();
    delay(300);

    // 3. Sample Manager (modo Daisy-first: sin precarga local en boot)
    sampleManager.begin();
    syslog("BOOT", "SampleManager OK, heap=%u", ESP.getFreeHeap());

    // Preparar la carga del kit, pero ejecutarla DESPUES de levantar WiFi/web.
    // En el peor caso esta verificacion espera 8 s; no debe ocultar el SSID ni
    // bloquear la primera pantalla de una demo.
    auto loadDefaultKitAfterNetwork = []() {
        const char* defaultKit = "RED 808 KARZ";
        bool sdOk = false;
        int loadedMainPads = -1;
        SdStatusResponse sdStatus = {};
        for (int attempt = 0; attempt < 3; attempt++) {
            if (spiMaster.sdGetStatus(sdStatus)) {
                sdOk = true;
                if (sdStatus.present) {
                    loadedMainPads = 0;
                    for (int i = 0; i < 16; i++) {
                        if (sdStatus.samplesLoaded & (1UL << i)) loadedMainPads++;
                    }
                    syslog("BOOT", "Daisy SD: present=1 loadedPads=%d (attempt %d)",
                           loadedMainPads, attempt + 1);
                    break;
                } else {
                    syslog("BOOT", "Daisy SD not present (attempt %d)", attempt + 1);
                }
            } else {
                syslog("BOOT", "Daisy sdGetStatus FAILED (attempt %d)", attempt + 1);
            }
            delay(150);
        }

        if (!sdOk) {
            syslog("BOOT", "WARN: Daisy SD status unreachable, kit NOT loaded");
        } else if (!sdStatus.present) {
            syslog("BOOT", "WARN: Daisy SD missing, kit NOT loaded");
        } else if (loadedMainPads >= 16) {
            syslog("BOOT", "Daisy already has 16 pads, skip kit load");
        } else {
            // Necesitamos cargar / recargar el kit. Reintenta hasta 3 veces.
            bool sent = false;
            for (int attempt = 0; attempt < 3 && !sent; attempt++) {
                sent = spiMaster.sdLoadKit(defaultKit, 0, 16);
                if (!sent) {
                    syslog("BOOT", "sdLoadKit enqueue FAILED (attempt %d)", attempt + 1);
                    delay(100);
                }
            }
            if (sent) {
                syslog("BOOT", "sdLoadKit '%s' sent OK (had %d/16)", defaultKit, loadedMainPads);
                const uint32_t loadStart = millis();
                int lastLoaded = loadedMainPads < 0 ? 0 : loadedMainPads;
                while (millis() - loadStart < 8000) {
                    delay(250);
                    SdStatusResponse pollStatus = {};
                    if (!spiMaster.sdGetStatus(pollStatus) || !pollStatus.present) {
                        continue;
                    }
                    int pollLoaded = 0;
                    for (int i = 0; i < 16; i++) {
                        if (pollStatus.samplesLoaded & (1UL << i)) pollLoaded++;
                    }
                    if (pollLoaded != lastLoaded) {
                        syslog("BOOT", "Default kit loading: %d/16 pads", pollLoaded);
                        lastLoaded = pollLoaded;
                    }
                    if (pollLoaded >= 16) {
                        syslog("BOOT", "Default kit ready: 16/16 pads loaded");
                        break;
                    }
                }
                if (lastLoaded < 16) {
                    syslog("BOOT", "WARN: default kit not fully loaded after timeout (%d/16)", lastLoaded);
                }
            } else {
                syslog("BOOT", "ERROR: sdLoadKit could not be enqueued");
            }
        }

        // Sin fallback a engine 808 para pads sin sample. Los pads vacios
        // quedan en sampler (-1) y simplemente no suenan en el secuenciador
        // (DsqFireStep los salta). La UI muestra que pads tienen sample
        // cargado y cuales no, asi el comportamiento es predecible: pulsar
        // play sobre un pad vacio = silencio (no synth fantasma).
        {
            SdStatusResponse finalStatus = {};
            uint16_t loadedMask = 0;
            if (spiMaster.sdGetStatus(finalStatus) && finalStatus.present) {
                loadedMask = (uint16_t)(finalStatus.samplesLoaded & 0xFFFF);
            }
            int missingCount = 0;
            char missingList[96] = {};
            size_t mlPos = 0;
            for (int t = 0; t < 16; t++) {
                if (!(loadedMask & (1u << t))) {
                    missingCount++;
                    if (mlPos < sizeof(missingList) - 4) {
                        int w = snprintf(missingList + mlPos, sizeof(missingList) - mlPos,
                                         "%s%d", mlPos == 0 ? "" : ",", t);
                        if (w > 0) mlPos += (size_t)w;
                    }
                }
            }
            if (missingCount > 0) {
                syslog("BOOT",
                       "WARN: %d pad(s) sin sample tras sdLoadKit (pads: %s) - silencioso en DSQ",
                       missingCount, missingList);
            }
        }
    };

    // 4. Sequencer Setup
    // Daisy es la única autoridad temporal: dispara samples y sintetizadores
    // dentro del callback de audio. Repetirlos desde este callback del Master
    // producía ataques dobles, flams y notas fuera de fase.
    sequencer.setStepCallback(nullptr);

    // Callback para sincronización en tiempo real con la web
    sequencer.setStepChangeCallback([](int newStep) {
        (void)newStep;
        releaseSequencerMelodicHolds();
    });
    // Callback para cambio de patrón en song mode
    sequencer.setPatternChangeCallback([](int newPattern, int songLength) {
        const bool daisyAlreadyQueued = dsqConsumeNativeTransition();
        // Scratch scenes MAX-4..MAX-1 power DROP/BUILD/FILL/VAR but are deliberately
        // invisible to controllers: the header keeps showing the song scene.
        if (newPattern < MAX_PATTERNS - 4) {
            if (!daisyAlreadyQueued) dsqSelectPatternDeferred(newPattern);
            else applyPatternPerformance(newPattern);
            webInterface.broadcastSongPattern(newPattern, songLength);
        }
    });
    sequencer.setTempo(110); // BPM inicial
    spiMaster.setTempo(110.0f); // Sync BPM to Daisy transport
    applyProfessionalMixBaseline();
    
    initializeProfessionalPatternBank(sequencer);

    // sequencer.start(); // DISABLED: User must press PLAY

    // 5. WiFi: STA (casa) + AP (RED808 fallback)
    
    showWiFiLED();
    delay(50);
    
    if (webInterface.begin(AP_SSID, AP_PASSWORD,
                           HOME_WIFI_SSID, HOME_WIFI_PASS,
                           HOME_WIFI_TIMEOUT)) {
        showWebServerLED();
        delay(50);
    }

    // AsyncWebServer ya puede entregar la portada mientras la Daisy termina
    // de verificar/cargar los samples del kit por defecto.
    loadDefaultKitAfterNetwork();

    // Cargar el banco factory de 20 patrones desde LittleFS encima del banco
    // integrado de 16. Si el archivo no existe (fs sin subir), se mantiene
    // el integrado como fallback seguro.
    {
        String bankErr;
        if (webInterface.loadPatternBank("20_patrones_factory_daisy", &bankErr)) {
            syslog("BOOT", "Pattern bank '20 Patrones Factory Daisy' cargado desde LittleFS");
        } else {
            syslog("BOOT", "Banco JSON no cargado (%s) - usando 16 integrados",
                   bankErr.c_str());
        }
    }

    webInterface.setMIDIController(&midiController);
    midiController.setMessageCallback([](const MIDIMessage& msg) {
        webInterface.broadcastMIDIMessage(msg);
        if (msg.type == MIDI_NOTE_ON && msg.data2 > 0) {
            int8_t pad = midiController.getMappedPad(msg.data1);
            if (pad >= 0) {
                triggerPadWithLED(pad, msg.data2);
            }
        }
    });
    midiController.setDeviceCallback([](bool connected, const MIDIDeviceInfo& info) {
        webInterface.broadcastMIDIDeviceStatus(connected, info);
    });
    midiController.begin();

#if ENABLE_PHYSICAL_BUTTONS
    // Callback: WebInterface notifica cuando llega POST /api/buttons
    // Aplica nueva config en tiempo real sin reiniciar
    webInterface.setBtnConfigCallback([](const String& json) {
        StaticJsonDocument<1024> doc;
        if (deserializeJson(doc, json)) return;
        // Acepta array plano [...] o {"buttons":[...]}
        JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["buttons"].as<JsonArray>();
        if (arr.isNull()) return;
        for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
            BtnCfg cfg;
            cfg.funcId   = arr[i]["funcId"]   | (uint8_t)ctrlButtons.getCfg(i).funcId;
            cfg.colorOff = arr[i]["colorOff"]  | (uint32_t)CTRL_CLR_RED;
            cfg.colorOn  = arr[i]["colorOn"]   | (uint32_t)CTRL_CLR_GREEN;
            const char* lbl = arr[i]["label"];
            if (lbl) strncpy(cfg.label, lbl, 19);
            cfg.label[19] = '\0';
            ctrlButtons.setCfg(i, cfg);
        }
    });

    // --- BOTONES FÍSICOS CON LED RGB ---
    // Cargar configuración guardada antes de begin()
    {
        File f;
        if (LittleFS.exists("/buttons.json")) {
            f = LittleFS.open("/buttons.json", "r");
        }
        if (f) {
            StaticJsonDocument<1024> doc;
            if (!deserializeJson(doc, f)) {
                // Acepta array plano [...] o {"buttons":[...]}
                JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["buttons"].as<JsonArray>();
                if (!arr.isNull()) {
                    for (int i = 0; i < 4 && i < (int)arr.size(); i++) {
                        BtnCfg cfg;
                        cfg.funcId   = arr[i]["funcId"]   | (uint8_t)ctrlButtons.getCfg(i).funcId;
                        cfg.colorOff = arr[i]["colorOff"]  | (uint32_t)CTRL_CLR_RED;
                        cfg.colorOn  = arr[i]["colorOn"]   | (uint32_t)CTRL_CLR_GREEN;
                        const char* lbl = arr[i]["label"];
                        if (lbl) strncpy(cfg.label, lbl, 19);
                        cfg.label[19] = '\0';
                        ctrlButtons.setCfg(i, cfg);
                    }
                }
            }
            f.close();
        }
    }
    ctrlButtons.begin();

    // Callback genérico — dispatcher de todas las funciones
    ctrlButtons.onAction = [](int btnIdx, uint8_t funcId) {
        char buf[128];
        switch (funcId) {
            /* ── Transporte ── */
            case BTN_FUNC_PLAY_PAUSE: {
                bool nowPlaying = !sequencer.isPlaying();
                if (nowPlaying) { sequencer.start(); spiMaster.dsqControl(1); }
                else            { sequencer.stop();  spiMaster.dsqControl(0); }
                ctrlButtons.setLedState(btnIdx, nowPlaying);
                webInterface.broadcastSequencerState();
                break;
            }
            case BTN_FUNC_STOP:
                sequencer.stop(); spiMaster.dsqControl(0);
                ctrlButtons.setLedState(btnIdx, false);
                webInterface.broadcastSequencerState();
                break;
            case BTN_FUNC_NEXT_PATTERN:
            case BTN_FUNC_NEXT_PAT_PLAY: {
                int patternCount = max(1, (int)getConfiguredPatternCount());
                int next = (sequencer.getCurrentPattern() + 1) % patternCount;
                sequencer.songChainStop();
                spiMaster.songControl(0);
                sequencer.selectPattern(next);
                dsqSelectPatternDeferred(next);
                if (funcId == BTN_FUNC_NEXT_PAT_PLAY) { sequencer.start(); spiMaster.dsqControl(1); }
                ctrlButtons.flashLed(btnIdx);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"nextPattern\",\"pattern\":%d}", next);
                webInterface.broadcastRaw(buf);
                webInterface.broadcastUdpSongPattern(next, 1);
                break;
            }
            case BTN_FUNC_PREV_PATTERN:
            case BTN_FUNC_PREV_PAT_PLAY: {
                int patternCount = max(1, (int)getConfiguredPatternCount());
                int prev = (sequencer.getCurrentPattern() + patternCount - 1) % patternCount;
                sequencer.songChainStop();
                spiMaster.songControl(0);
                sequencer.selectPattern(prev);
                dsqSelectPatternDeferred(prev);
                if (funcId == BTN_FUNC_PREV_PAT_PLAY) { sequencer.start(); spiMaster.dsqControl(1); }
                ctrlButtons.flashLed(btnIdx);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"prevPattern\",\"pattern\":%d}", prev);
                webInterface.broadcastRaw(buf);
                webInterface.broadcastUdpSongPattern(prev, 1);
                break;
            }
            case BTN_FUNC_TAP_TEMPO:
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_YELLOW);
                // Tap tempo no implementado aquí (requiere multipress timing)
                break;
            /* ── Navegación ── */
            case BTN_FUNC_MULTIVIEW:
                gMultiviewActive = !gMultiviewActive;
                ctrlButtons.setLedState(btnIdx, gMultiviewActive);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"multiview\",\"active\":%s}",
                    gMultiviewActive ? "true" : "false");
                webInterface.broadcastRaw(buf);
                break;
            /* ── Volumen ── */
            case BTN_FUNC_MASTER_VOL_UP: {
                uint8_t v = min(150, (int)spiMaster.getMasterVolume() + 5);
                spiMaster.setMasterVolume(v);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_GREEN);
                break;
            }
            case BTN_FUNC_MASTER_VOL_DN: {
                uint8_t v = (uint8_t)max(0, (int)spiMaster.getMasterVolume() - 5);
                spiMaster.setMasterVolume(v);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_RED);
                break;
            }
            case BTN_FUNC_LIVE_VOL_UP: {
                uint8_t v = min(180, (int)spiMaster.getLiveVolume() + 5);
                spiMaster.setLiveVolume(v);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_GREEN);
                break;
            }
            case BTN_FUNC_LIVE_VOL_DN: {
                uint8_t v = (uint8_t)max(0, (int)spiMaster.getLiveVolume() - 5);
                spiMaster.setLiveVolume(v);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_RED);
                break;
            }
            /* ── Tempo ── */
            case BTN_FUNC_TEMPO_UP1: spiMaster.setTempo(sequencer.getTempo() + 1);  sequencer.setTempo(sequencer.getTempo() + 1);  ctrlButtons.flashLed(btnIdx, CTRL_CLR_YELLOW); break;
            case BTN_FUNC_TEMPO_DN1: spiMaster.setTempo(max(20.f, sequencer.getTempo() - 1)); sequencer.setTempo(max(20.f, sequencer.getTempo() - 1)); ctrlButtons.flashLed(btnIdx, CTRL_CLR_YELLOW); break;
            case BTN_FUNC_TEMPO_UP5: spiMaster.setTempo(sequencer.getTempo() + 5);  sequencer.setTempo(sequencer.getTempo() + 5);  ctrlButtons.flashLed(btnIdx, CTRL_CLR_YELLOW); break;
            case BTN_FUNC_TEMPO_DN5: spiMaster.setTempo(max(20.f, sequencer.getTempo() - 5)); sequencer.setTempo(max(20.f, sequencer.getTempo() - 5)); ctrlButtons.flashLed(btnIdx, CTRL_CLR_YELLOW); break;
            /* ── FX Master toggles ── */
            case BTN_FUNC_DELAY_TOGGLE:   gDelayActive   = !gDelayActive;   spiMaster.setDelayActive(gDelayActive);    ctrlButtons.setLedState(btnIdx, gDelayActive);   break;
            case BTN_FUNC_REVERB_TOGGLE:  gReverbActive  = !gReverbActive;  spiMaster.setReverbActive(gReverbActive);  ctrlButtons.setLedState(btnIdx, gReverbActive);  break;
            case BTN_FUNC_CHORUS_TOGGLE:  gChorusActive  = !gChorusActive;  spiMaster.setChorusActive(gChorusActive);  ctrlButtons.setLedState(btnIdx, gChorusActive);  break;
            case BTN_FUNC_PHASER_TOGGLE:  gPhaserActive  = !gPhaserActive;  spiMaster.setPhaserActive(gPhaserActive);  ctrlButtons.setLedState(btnIdx, gPhaserActive);  break;
            case BTN_FUNC_FLANGER_TOGGLE: gFlangerActive = !gFlangerActive; spiMaster.setFlangerActive(gFlangerActive); ctrlButtons.setLedState(btnIdx, gFlangerActive); break;
            case BTN_FUNC_COMP_TOGGLE:    gCompActive    = !gCompActive;    spiMaster.setCompressorActive(gCompActive);  ctrlButtons.setLedState(btnIdx, gCompActive);    break;
            case BTN_FUNC_TREMOLO_TOGGLE: gTremoloActive = !gTremoloActive; spiMaster.setTremoloActive(gTremoloActive); ctrlButtons.setLedState(btnIdx, gTremoloActive); break;
            case BTN_FUNC_LIMITER_TOGGLE: gLimiterActive = !gLimiterActive; spiMaster.setLimiterActive(gLimiterActive); ctrlButtons.setLedState(btnIdx, gLimiterActive); break;
            case BTN_FUNC_DIST_TOGGLE:    gDistActive    = !gDistActive; ctrlButtons.setLedState(btnIdx, gDistActive); break;
            /* ── Mute/Solo ── */
            case BTN_FUNC_MUTE_ALL:
                for (int t=0;t<16;t++) spiMaster.setTrackMute(t, true);
                ctrlButtons.setLedState(btnIdx, true);
                break;
            case BTN_FUNC_UNMUTE_ALL:
                for (int t=0;t<16;t++) spiMaster.setTrackMute(t, false);
                ctrlButtons.setLedState(btnIdx, false);
                break;
            /* ── Longitud patrón ── */
            case BTN_FUNC_PAT_LEN_CYCLE: {
                int cur = sequencer.getPatternLength();
                int next = (cur == 16) ? 32 : (cur == 32) ? 64 : 16;
                sequencer.setPatternLength(next);
                spiMaster.dsqSetLength((uint8_t)next);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_PURPLE);
                break;
            }
            /* ── Ir a patrón N ── */
            case BTN_FUNC_PATTERN_0: case BTN_FUNC_PATTERN_1: case BTN_FUNC_PATTERN_2:
            case BTN_FUNC_PATTERN_3: case BTN_FUNC_PATTERN_4: case BTN_FUNC_PATTERN_5:
            case BTN_FUNC_PATTERN_6: case BTN_FUNC_PATTERN_7: {
                int pIdx = funcId - BTN_FUNC_PATTERN_0;
                sequencer.songChainStop();
                spiMaster.songControl(0);
                sequencer.selectPattern(pIdx);
                dsqSelectPatternDeferred(pIdx);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_CYAN);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"nextPattern\",\"pattern\":%d}", pIdx);
                webInterface.broadcastRaw(buf);
                webInterface.broadcastUdpSongPattern(pIdx, 1);
                break;
            }
            /* ── Live Pads (disparo directo vía SPI) ── */
            case BTN_FUNC_LIVE_PAD_0:  case BTN_FUNC_LIVE_PAD_1:  case BTN_FUNC_LIVE_PAD_2:
            case BTN_FUNC_LIVE_PAD_3:  case BTN_FUNC_LIVE_PAD_4:  case BTN_FUNC_LIVE_PAD_5:
            case BTN_FUNC_LIVE_PAD_6:  case BTN_FUNC_LIVE_PAD_7:  case BTN_FUNC_LIVE_PAD_8:
            case BTN_FUNC_LIVE_PAD_9:  case BTN_FUNC_LIVE_PAD_10: case BTN_FUNC_LIVE_PAD_11:
            case BTN_FUNC_LIVE_PAD_12: case BTN_FUNC_LIVE_PAD_13: case BTN_FUNC_LIVE_PAD_14:
            case BTN_FUNC_LIVE_PAD_15: {
                uint8_t padIdx = (uint8_t)(funcId - BTN_FUNC_LIVE_PAD_0);
                spiMaster.triggerSampleLive(padIdx, 127);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_GREEN);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"triggerPad\",\"pad\":%d}", padIdx);
                webInterface.broadcastRaw(buf);
                break;
            }
            /* ── XTRA Pads (índices 16-23) ── */
            case BTN_FUNC_XTRA_PAD_0: case BTN_FUNC_XTRA_PAD_1: case BTN_FUNC_XTRA_PAD_2:
            case BTN_FUNC_XTRA_PAD_3: case BTN_FUNC_XTRA_PAD_4: case BTN_FUNC_XTRA_PAD_5:
            case BTN_FUNC_XTRA_PAD_6: case BTN_FUNC_XTRA_PAD_7: {
                uint8_t padIdx = (uint8_t)(16 + (funcId - BTN_FUNC_XTRA_PAD_0));
                spiMaster.triggerSampleLive(padIdx, 127);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_ORANGE);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"triggerPad\",\"pad\":%d}", padIdx);
                webInterface.broadcastRaw(buf);
                break;
            }
        }
    };
#endif

    // --- SD EVENT CALLBACK (Daisy → WebSocket) ---
    spiMaster.setEventCallback([](const NotifyEvent& evt, void* /*ud*/) {
        StaticJsonDocument<256> doc;
        doc["type"]     = "sdEvent";
        doc["event"]    = evt.type;       // EVT_SD_*
        doc["padCount"] = evt.padCount;
        doc["maskLo"]   = evt.padMaskLo;
        doc["maskHi"]   = evt.padMaskHi;
        doc["maskXtra"] = evt.padMaskXtra;
        doc["name"]     = String(evt.name);
        String out;
        serializeJson(doc, out);
        webInterface.broadcastRaw(out.c_str());
    });

    // ── Task Watchdog: 10s timeout, panic on timeout ──
    // IMPORTANTE: inicializar ANTES de crear las tasks. spiAudioTask (prio 24) y
    // systemTask (prio 5) tienen mayor prioridad que loopTask (prio 1, donde corre
    // setup()), por lo que arrancan de inmediato y llaman esp_task_wdt_add() en su
    // entrada. Si el TWDT no esta inicializado en ese momento, el add() devuelve
    // ESP_ERR_INVALID_STATE y la task NUNCA queda suscrita — dejando el nucleo de
    // audio/SPI sin watchdog (justo lo contrario del diseno de recuperacion).
    // La carga de samples (que tardaba) ya ocurrio antes en setup(), asi que
    // arrancar el watchdog aqui no provoca un reset espurio durante el boot.
    esp_task_wdt_config_t twdtConfig = {};
    twdtConfig.timeout_ms = 10000;
    twdtConfig.idle_core_mask = 0;
    twdtConfig.trigger_panic = true;
    // Arduino/IDF normally creates TWDT before setup(). Try that existing
    // instance first: calling init() first emits an ESP-IDF error even if we
    // immediately reconfigure it successfully afterwards.
    esp_err_t twdtErr = esp_task_wdt_reconfigure(&twdtConfig);
    if (twdtErr == ESP_ERR_INVALID_STATE) {
        twdtErr = esp_task_wdt_init(&twdtConfig);
    }
    if (twdtErr != ESP_OK) {
        syslog("BOOT", "WARN: TWDT setup failed: %s", esp_err_to_name(twdtErr));
    }

    // --- DUAL-CORE TASKS ---

    // CORE 1: SPI Audio Task (Sequencer + SPI) - Prioridad máxima
    xTaskCreatePinnedToCore(
        spiAudioTask,
        "SPIAudioTask",
        20480,  // 20KB stack — patrón upload en boot + DSQ upload (16 tracks × 8 patrones)
        NULL,
        24,     // Prioridad máxima
        NULL,
        1       // CORE 1: Sequencer + SPI Master
    );
    
    // CORE 0: System Task - Prioridad media
    xTaskCreatePinnedToCore(
        systemTask,
        "SystemTask",
        24576,  // 24KB stack - WiFi/JSON/UDP (increased for safety)
        NULL,
        5,
        NULL,
        0       // CORE 0: WiFi, Web, MIDI, LED
    );

    // El TWDT ya quedo inicializado antes de crear las tasks (ver arriba).
    esp_task_wdt_add(NULL);  // subscribe loopTask (setup corre aqui)

    Serial.println("=== RED808 BOOT COMPLETE ===");
    syslog("BOOT", "COMPLETE heap=%u min=%u block=%u psram=%u samples=%d",
           ESP.getFreeHeap(), ESP.getMinFreeHeap(),
           (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (uint32_t)ESP.getFreePsram(), sampleManager.getLoadedSamplesCount());
    Serial.printf("[BOOT] Heap: free=%u min=%u largest_block=%u\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("[BOOT] PSRAM: free=%u / %u\n",
                  (uint32_t)ESP.getFreePsram(), (uint32_t)ESP.getPsramSize());
    Serial.printf("[BOOT] Samples loaded: %d\n", sampleManager.getLoadedSamplesCount());

    showReadyLED();
}

void loop() {
    feedTaskWdtIfSubscribed();  // loopTask puede no estar suscrito por Arduino
    drainPadTriggerAutoOff(); // release expired pad-trigger voices
    vTaskDelay(pdMS_TO_TICKS(100)); // loop() no hace nada crítico
}
