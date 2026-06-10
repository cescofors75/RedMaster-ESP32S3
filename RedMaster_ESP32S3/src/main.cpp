#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "SPIMaster.h"
#include "SampleManager.h"
#include "Sequencer.h"
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
static volatile bool   _pendingDsqPlay = false;    // arrancar Daisy tras upload (ordenado)

void dsqUploadPatternDeferred(int pattern) {
    if (pattern < 0) pattern = 0;
    pattern %= DSQ_PATTERNS;
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelect = true;
    _pendingDsqUpload = (int8_t)pattern;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Solo selecciona patrón en Daisy (1 cmd SPI). NO reuploadea steps.
// Usar cuando los patrones ya están en Daisy y solo queremos cambiar el activo.
void dsqSelectPatternDeferred(int pattern) {
    if (pattern < 0) pattern = 0;
    pattern %= DSQ_PATTERNS;
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelectOnly = (int8_t)pattern;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Sube patrón y al terminar arranca Daisy. Garantiza orden upload → select → play.
void dsqUploadAndPlayDeferred(int pattern) {
    if (pattern < 0) pattern = 0;
    pattern %= DSQ_PATTERNS;
    portENTER_CRITICAL(&_pendingDsqMux);
    _pendingDsqSelect = true;
    _pendingDsqUpload = (int8_t)pattern;
    _pendingDsqPlay   = true;
    portEXIT_CRITICAL(&_pendingDsqMux);
}

// Número real de patrones inline definidos en setup (HIP HOP, TECHNO, DnB,
// BREAK, HOUSE, TRAP). Subir sólo estos al boot evita saturar la SPI con
// 10 patrones vacíos y reduce el riesgo de perder comandos a la Daisy.
#define DSQ_PATTERNS_INLINE 6

// Helper: convierte un patrón del Sequencer ESP32 → DsqStepPkt y lo sube a Daisy
void dsqUploadPattern(int pattern) {
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
        sequencer.snapshotTrackForUpload(pattern, trk, clampedLen, snap);
        for (int s = 0; s < clampedLen; s++) {
            pkt[s].active      = snap[s].active ? 1 : 0;
            pkt[s].velocity    = snap[s].velocity;
            pkt[s].noteLenDiv  = snap[s].noteLenDiv;
            pkt[s].probability = snap[s].probability;
        }
        if (!spiMaster.dsqUploadTrack((uint8_t)pattern, (uint8_t)trk, pkt, (uint8_t)clampedLen)) {
            Serial.printf("[DSQ] upload track %d pattern %d failed\n", trk, pattern);
        }
        // 8 ms entre tracks: deja a la Daisy procesar el packet anterior
        // y vaciar su SPI RX ring antes del siguiente. Evita spiRingDrops y
        // tracks parciales (síntoma: patrones con pistas vacías).
        vTaskDelay(pdMS_TO_TICKS(8));
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
                    (uint8_t)pattern, (uint8_t)trk, (uint8_t)s,
                    ce, ch, re, rv, ve, vl
                );
            }
        }
    }
}

// CORE 1: Sequencer UI + SPI Master
// El secuenciador corre en Daisy Seed; aquí solo actualizamos estado UI y LFO.
void spiAudioTask(void *pvParameters) {
    esp_task_wdt_add(NULL);  // subscribe to TWDT

    // Subir sólo los patrones inline reales (no los 16 slots completos).
    // 20ms de gap entre patrones para que la Daisy procese cada lote
    // antes de empezar el siguiente — evita que comandos posteriores caigan
    // al vacío y dejen slots con datos parciales (sintoma: patrones que no suenan).
    for (int pat = 0; pat < DSQ_PATTERNS_INLINE; pat++) {
        dsqUploadPattern(pat);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    // Seleccionar patrón activo en Daisy
    spiMaster.dsqSelectPattern((uint8_t)sequencer.getCurrentPattern());
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
            dsqUploadPattern(pat);
            if (selectAfterUpload) {
                spiMaster.dsqSelectPattern((uint8_t)pat);
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
            spiMaster.dsqSelectPattern((uint8_t)selOnly);
        }

        sequencer.update();   // Mantiene internos del secuenciador (beat UI, song mode)
        spiMaster.process();

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

        esp_task_wdt_reset();
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
        esp_task_wdt_reset();
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

    // Compressor: suave glue para pegar la mezcla
    spiMaster.setCompressorActive(true);
    spiMaster.setCompressorThreshold(-12.0f);
    spiMaster.setCompressorRatio(2.5f);
    spiMaster.setCompressorAttack(10.0f);
    spiMaster.setCompressorRelease(100.0f);
    spiMaster.setCompressorMakeupGain(2.0f);

    // Reverb: room sutil para dar espacio
    spiMaster.setReverb(true, 0.45f, 6000.0f, 0.12f);  // feedback=0.45, lpFreq=6kHz, mix=12%

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

    // Reverb sends: snare/clap con room, kick seco
    spiMaster.setTrackReverbSend(1, 18);   // SD: algo de room
    spiMaster.setTrackReverbSend(5, 22);   // CP: clap con reverb
    spiMaster.setTrackReverbSend(3, 15);   // OH: un toque
    spiMaster.setTrackReverbSend(4, 25);   // CY: más espacio

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
    syslog("BOOT", "SPI Master OK");
    
    
    showLoadingSamplesLED();
    delay(300);

    // 3. Sample Manager (modo Daisy-first: sin precarga local en boot)
    sampleManager.begin();
    syslog("BOOT", "SampleManager OK, heap=%u", ESP.getFreeHeap());

    // Daisy-first: cargar kit por defecto desde SD del Daisy.
    // Reintenta hasta 3 veces para asegurar que la Daisy recibe el comando
    // (acabamos de despertar, la cola SPI puede estar caliente).
    {
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
    }

    // 4. Sequencer Setup
    // El secuenciador de la Daisy dispara SAMPLES.
    // El ESP32 se encarga de disparar SYNTH TRIGGERS en paralelo.

    // Step callback: cuando un track usa synth engine, enviar synthTrigger por SPI
    sequencer.setStepCallback([](int track, uint8_t velocity, uint8_t trackVolume, uint32_t noteLenSamples) {
        int8_t engine = getTrackSynthEngine(track);
        if (engine < 0) return;  // sampler → la Daisy ya lo dispara internamente
        // Synth engine: el ESP32 envía el trigger por SPI
        float scaled = (velocity / 127.0f) * (trackVolume / 100.0f);
        uint8_t synthVel = (uint8_t)constrain((int)roundf(scaled * 127.0f), 1, 127);
        // Melodic engines (303/WTOSC/SH101/FM2Op): use per-step note if available
        if (engine >= 3) {
            int pat = sequencer.getCurrentPattern();
            int step = sequencer.getCurrentStep();
            uint8_t flags = sequencer.getStepFlags(pat, track, step);
            bool accent = (flags & 0x01) != 0;
            bool slide  = (flags & 0x02) != 0;
            bool anyNote = false;
            // Reclamo atomico del hold previo (compite con el comando stop de
            // la web, que llama releaseSequencerMelodicHolds desde AsyncTCP).
            portENTER_CRITICAL(&gPadNoteMux);
            bool prevHeld = gSeqMelodicHeld[track];
            int8_t prevEngine = gSeqMelodicHeldEngine[track];
            if (prevHeld) {
                gSeqMelodicHeld[track] = false;
                gSeqMelodicHeldEngine[track] = -1;
            }
            portEXIT_CRITICAL(&gPadNoteMux);
            if (prevHeld) {
                if (prevEngine == 3) spiMaster.synth303NoteOff();
                else if (prevEngine >= 4 && prevEngine <= 8) spiMaster.synthNoteOff((uint8_t)prevEngine, (uint8_t)track);
            }
            for (int voice = 0; voice < MELODY_STEP_VOICES; voice++) {
                uint8_t note = sequencer.getStepNoteVoice(pat, track, step, voice);
                if (note == 0) continue;
                anyNote = true;
                spiMaster.synthNoteOnEx((uint8_t)engine, note, synthVel, accent, slide);
            }
            if (!anyNote) return;
            portENTER_CRITICAL(&gPadNoteMux);
            gSeqMelodicHeld[track] = true;
            gSeqMelodicHeldEngine[track] = engine;
            portEXIT_CRITICAL(&gPadNoteMux);
        } else {
            // Percussion engines (808/909/505): trigger by instrument
            spiMaster.synthTrigger((uint8_t)engine, (uint8_t)track, synthVel);
        }
    });

    // Callback para sincronización en tiempo real con la web
    sequencer.setStepChangeCallback([](int newStep) {
        releaseSequencerMelodicHolds();
        webInterface.broadcastStep(newStep);
    });
    // Callback para cambio de patrón en song mode
    sequencer.setPatternChangeCallback([](int newPattern, int songLength) {
        webInterface.broadcastSongPattern(newPattern, songLength);
    });
    sequencer.setTempo(110); // BPM inicial
    spiMaster.setTempo(110.0f); // Sync BPM to Daisy transport
    applyProfessionalMixBaseline();
    
    // === PATRÓN 0: HIP HOP BOOM BAP (16 tracks) ===
    sequencer.selectPattern(0);
    sequencer.setStep(0, 0, true);   // BD: Kick en 1
    sequencer.setStep(0, 3, true);   // BD: Kick ghost
    sequencer.setStep(0, 10, true);  // BD: Kick sincopado
    sequencer.setStep(1, 4, true);   // SD: Snare en 2
    sequencer.setStep(1, 12, true);  // SD: Snare en 4
    for(int i=0; i<16; i+=2) sequencer.setStep(2, i, true); // CH: patrón cerrado
    sequencer.setStep(3, 6, true);   // OH: para swing
    sequencer.setStep(3, 14, true);  // OH: al final
    sequencer.setStep(5, 4, true);   // CP: Clap doblando snare
    sequencer.setStep(5, 12, true);
    sequencer.setStep(6, 7, true);   // RS: Rimshot fill
    sequencer.setStep(7, 5, true);   // CB: Cowbell groove
    sequencer.setStep(7, 13, true);  // CB: Cowbell extra
    sequencer.setStep(4, 15, true);  // CY: Cymbal crash final
    // Extra percusión para ocupar más tracks (8-15)
    sequencer.setStep(8, 2, true);   // LT
    sequencer.setStep(8, 11, true);
    sequencer.setStep(9, 6, true);   // MT
    sequencer.setStep(10, 14, true); // HT
    sequencer.setStep(11, 1, true);  // MA
    sequencer.setStep(11, 9, true);
    sequencer.setStep(12, 3, true);  // CL
    sequencer.setStep(12, 15, true);
    sequencer.setStep(13, 10, true); // HC
    sequencer.setStep(14, 12, true); // MC
    sequencer.setStep(15, 0, true);  // LC
    
    // === PATRÓN 1: TECHNO DETROIT (16 tracks) ===
    sequencer.selectPattern(1);
    for(int i=0; i<16; i+=4) sequencer.setStep(0, i, true); // BD: Four on the floor
    sequencer.setStep(1, 4, true);   // SD: Snare en 2
    sequencer.setStep(1, 12, true);  // SD: Snare en 4
    for(int i=0; i<16; i++) sequencer.setStep(2, i, true);  // CH: 16th hi-hats
    sequencer.setStep(3, 8, true);   // OH: en medio
    sequencer.setStep(5, 4, true);   // CP: Clap capa snare
    sequencer.setStep(5, 8, true);
    sequencer.setStep(5, 12, true);
    sequencer.setStep(6, 7, true);   // RS: Rim accent
    sequencer.setStep(6, 11, true);
    sequencer.setStep(6, 15, true);
    sequencer.setStep(12, 3, true);  // CL: Claves offbeat
    sequencer.setStep(12, 7, true);
    sequencer.setStep(12, 11, true);
    sequencer.setStep(12, 15, true);
    sequencer.setStep(4, 0, true);   // CY: Cymbal intro
    sequencer.setStep(4, 8, true);   // CY: medio
    // Toms/congas para expandir instrumentación completa
    sequencer.setStep(8, 8, true);   // LT
    sequencer.setStep(9, 10, true);  // MT
    sequencer.setStep(10, 14, true); // HT
    sequencer.setStep(11, 3, true);  // MA
    sequencer.setStep(11, 11, true);
    sequencer.setStep(13, 6, true);  // HC
    sequencer.setStep(14, 13, true); // MC
    sequencer.setStep(15, 15, true); // LC
    sequencer.setStepCutoffLock(1, 0, true, 340);
    sequencer.setStepCutoffLock(1, 4, true, 950);
    sequencer.setStepCutoffLock(1, 8, true, 2600);
    sequencer.setStepCutoffLock(1, 12, true, 7000);
    sequencer.setStepReverbSendLock(1, 4, true, 35);
    sequencer.setStepReverbSendLock(1, 12, true, 55);
    sequencer.setStepVolumeLock(1, 4, true, 112);
    sequencer.setStepVolumeLock(1, 12, true, 120);
    
    // === PATRÓN 2: DRUM & BASS (16 tracks) ===
    sequencer.selectPattern(2);
    sequencer.setStep(0, 0, true);   // BD: Kick doble
    sequencer.setStep(0, 2, true);
    sequencer.setStep(0, 10, true);  // BD: Kick sincopado
    sequencer.setStep(1, 4, true);   // SD: Snare break
    sequencer.setStep(1, 7, true);   // SD: Snare ghost
    sequencer.setStep(1, 10, true);
    sequencer.setStep(1, 12, true);
    for(int i=0; i<16; i++) sequencer.setStep(2, i, true);  // CH: constante
    sequencer.setStep(3, 6, true);   // OH: textura
    sequencer.setStep(3, 10, true);
    sequencer.setStep(3, 14, true);
    sequencer.setStep(5, 4, true);   // CP: Clap layers
    sequencer.setStep(5, 8, true);
    sequencer.setStep(5, 12, true);
    sequencer.setStep(4, 0, true);   // CY: Cymbal intro
    sequencer.setStep(4, 8, true);
    sequencer.setStep(4, 15, true);
    // Más capas para 16 tracks
    sequencer.setStep(7, 3, true);   // CB
    sequencer.setStep(7, 11, true);
    sequencer.setStep(8, 5, true);   // LT
    sequencer.setStep(9, 9, true);   // MT
    sequencer.setStep(10, 13, true); // HT
    sequencer.setStep(11, 2, true);  // MA
    sequencer.setStep(11, 6, true);
    sequencer.setStep(12, 1, true);  // CL
    sequencer.setStep(12, 9, true);
    sequencer.setStep(13, 7, true);  // HC
    sequencer.setStep(14, 14, true); // MC
    sequencer.setStep(15, 12, true); // LC
    
    // === PATRÓN 3: LATIN PERCUSSION (16 tracks) ===
    sequencer.selectPattern(3);
    sequencer.setStep(0, 0, true);   // BD: Kick
    sequencer.setStep(0, 8, true);
    sequencer.setStep(1, 4, true);   // SD: Snare
    sequencer.setStep(1, 12, true);
    sequencer.setStep(2, 0, true);   // CH: Hi-hat pattern
    sequencer.setStep(2, 2, true);
    sequencer.setStep(2, 4, true);
    sequencer.setStep(2, 6, true);
    sequencer.setStep(2, 8, true);
    sequencer.setStep(2, 10, true);
    sequencer.setStep(2, 12, true);
    sequencer.setStep(2, 14, true);
    sequencer.setStep(7, 1, true);   // CB: Cowbell clave
    sequencer.setStep(7, 5, true);
    sequencer.setStep(7, 9, true);
    sequencer.setStep(7, 13, true);
    sequencer.setStep(12, 0, true);  // CL: Claves pattern
    sequencer.setStep(12, 3, true);
    sequencer.setStep(12, 6, true);
    sequencer.setStep(12, 10, true);
    sequencer.setStep(13, 2, true);  // HC: Hi Conga
    sequencer.setStep(13, 7, true);
    sequencer.setStep(13, 11, true);
    sequencer.setStep(14, 4, true);  // MC: Mid Conga
    sequencer.setStep(14, 9, true);
    sequencer.setStep(14, 14, true);
    sequencer.setStep(15, 0, true);  // LC: Low Conga
    sequencer.setStep(15, 6, true);
    sequencer.setStep(15, 12, true);
    sequencer.setStep(11, 1, true);  // MA: Maracas shuffle
    sequencer.setStep(11, 3, true);
    sequencer.setStep(11, 5, true);
    sequencer.setStep(11, 7, true);
    sequencer.setStep(11, 9, true);
    sequencer.setStep(11, 11, true);
    sequencer.setStep(11, 13, true);
    sequencer.setStep(11, 15, true);
    // Refuerzo de kit completo (cymbal/toms)
    sequencer.setStep(3, 15, true);  // OH cierre
    sequencer.setStep(4, 0, true);   // CY
    sequencer.setStep(4, 8, true);
    sequencer.setStep(8, 4, true);   // LT
    sequencer.setStep(9, 8, true);   // MT
    sequencer.setStep(10, 12, true); // HT
    
    // === PATRÓN 4: CHICAGO HOUSE (16 tracks) ===
    sequencer.selectPattern(4);
    for(int i=0; i<16; i+=4) sequencer.setStep(0, i, true); // BD: Four on floor
    sequencer.setStep(1, 4, true);   // SD: Snare 2 y 4
    sequencer.setStep(1, 12, true);
    for(int i=2; i<16; i+=4) sequencer.setStep(2, i, true); // CH: offbeat
    sequencer.setStep(3, 6, true);   // OH: sincopado
    sequencer.setStep(3, 10, true);
    sequencer.setStep(3, 14, true);
    sequencer.setStep(5, 4, true);   // CP: Clap dobla snare
    sequencer.setStep(5, 8, true);
    sequencer.setStep(5, 12, true);
    sequencer.setStep(6, 1, true);   // RS: Rim house
    sequencer.setStep(6, 5, true);
    sequencer.setStep(6, 9, true);
    sequencer.setStep(6, 13, true);
    sequencer.setStep(4, 0, true);   // CY: Cymbal intro
    sequencer.setStep(4, 8, true);
    // Añadir instrumentos restantes para full groove 16-track
    sequencer.setStep(7, 3, true);   // CB
    sequencer.setStep(7, 11, true);
    sequencer.setStep(8, 6, true);   // LT
    sequencer.setStep(9, 10, true);  // MT
    sequencer.setStep(10, 14, true); // HT
    sequencer.setStep(11, 1, true);  // MA
    sequencer.setStep(11, 5, true);
    sequencer.setStep(12, 7, true);  // CL
    sequencer.setStep(13, 9, true);  // HC
    sequencer.setStep(14, 13, true); // MC
    sequencer.setStep(15, 15, true); // LC

    // === PATRÓN 5: CINEMATIC HYBRID (16 tracks) ===
    sequencer.selectPattern(5);
    sequencer.setStep(0, 0, true);
    sequencer.setStep(0, 7, true);
    sequencer.setStep(0, 10, true);
    sequencer.setStep(0, 14, true);
    sequencer.setStep(1, 4, true);
    sequencer.setStep(1, 11, true);
    sequencer.setStep(1, 12, true);
    for (int i = 0; i < 16; i += 2) sequencer.setStep(2, i, true);
    sequencer.setStep(2, 13, true);
    sequencer.setStep(2, 15, true);
    sequencer.setStep(3, 3, true);
    sequencer.setStep(3, 9, true);
    sequencer.setStep(3, 15, true);
    sequencer.setStep(4, 0, true);
    sequencer.setStep(4, 15, true);
    sequencer.setStep(5, 4, true);
    sequencer.setStep(5, 12, true);
    sequencer.setStep(6, 6, true);
    sequencer.setStep(6, 14, true);
    sequencer.setStep(7, 5, true);
    sequencer.setStep(7, 13, true);
    sequencer.setStep(8, 2, true);
    sequencer.setStep(8, 10, true);
    sequencer.setStep(9, 8, true);
    sequencer.setStep(10, 12, true);
    sequencer.setStep(11, 1, true);
    sequencer.setStep(11, 5, true);
    sequencer.setStep(11, 9, true);
    sequencer.setStep(11, 13, true);
    sequencer.setStep(12, 7, true);
    sequencer.setStep(12, 15, true);
    sequencer.setStep(13, 3, true);
    sequencer.setStep(13, 11, true);
    sequencer.setStep(14, 6, true);
    sequencer.setStep(14, 14, true);
    sequencer.setStep(15, 0, true);
    sequencer.setStep(15, 8, true);
    // Professional transitions through automation locks
    sequencer.setStepCutoffLock(0, 0, true, 320);
    sequencer.setStepCutoffLock(0, 8, true, 1400);
    sequencer.setStepCutoffLock(0, 12, true, 4200);
    sequencer.setStepCutoffLock(0, 15, true, 9000);
    sequencer.setStepReverbSendLock(1, 4, true, 22);
    sequencer.setStepReverbSendLock(1, 12, true, 45);
    sequencer.setStepReverbSendLock(4, 0, true, 60);
    sequencer.setStepReverbSendLock(4, 15, true, 70);
    sequencer.setStepVolumeLock(2, 0, true, 105);
    sequencer.setStepVolumeLock(2, 8, true, 118);
    sequencer.setStepVolumeLock(2, 15, true, 112);

    // ── DINÁMICAS DE VELOCIDAD: ghost notes, acentos, humanización ──────────
    // Patrón 0: Hip Hop Boom Bap — groove con ghosts
    sequencer.setStepVelocity(0, 0, 0, 120);   // BD: fuerte
    sequencer.setStepVelocity(0, 0, 3, 75);    // BD: ghost suave
    sequencer.setStepVelocity(0, 0, 10, 100);  // BD: sincopado medio
    sequencer.setStepVelocity(0, 1, 4, 115);   // SD: accent
    sequencer.setStepVelocity(0, 1, 12, 110);  // SD
    // CH: alternar acentos 8ths
    for(int i=0; i<16; i+=2) sequencer.setStepVelocity(0, 2, i, (i%4==0) ? 100 : 70);
    sequencer.setStepVelocity(0, 3, 6, 85);    // OH
    sequencer.setStepVelocity(0, 3, 14, 75);   // OH suave
    sequencer.setStepVelocity(0, 5, 4, 105);   // CP
    sequencer.setStepVelocity(0, 5, 12, 100);  // CP
    sequencer.setStepVelocity(0, 6, 7, 80);    // RS ghost
    sequencer.setStepVelocity(0, 7, 5, 70);    // CB suave
    sequencer.setStepVelocity(0, 7, 13, 65);   // CB ghost

    // Patrón 1: Techno — mecánico pero con groove sutil
    sequencer.setStepVelocity(1, 0, 0, 127);   // BD: fuerte
    sequencer.setStepVelocity(1, 0, 4, 120);
    sequencer.setStepVelocity(1, 0, 8, 125);
    sequencer.setStepVelocity(1, 0, 12, 118);
    // CH 16ths: patrón de acentos dinámico
    for(int i=0; i<16; i++) sequencer.setStepVelocity(1, 2, i, (i%4==0) ? 110 : (i%2==0) ? 85 : 60);

    // Patrón 2: DnB — agresivo con ghosts
    sequencer.setStepVelocity(2, 0, 0, 127);   // BD: máximo impacto
    sequencer.setStepVelocity(2, 0, 2, 100);   // BD: segundo golpe más suave
    sequencer.setStepVelocity(2, 0, 10, 110);
    sequencer.setStepVelocity(2, 1, 4, 120);   // SD
    sequencer.setStepVelocity(2, 1, 7, 65);    // SD: ghost note
    sequencer.setStepVelocity(2, 1, 10, 90);
    sequencer.setStepVelocity(2, 1, 12, 115);
    // CH: rápido con dinámicas
    for(int i=0; i<16; i++) sequencer.setStepVelocity(2, 2, i, (i%4==0) ? 100 : (i%2==0) ? 80 : 55);

    // Patrón 3: Latin — orgánico con acentos de clave
    sequencer.setStepVelocity(3, 7, 1, 90);    // CB: clave
    sequencer.setStepVelocity(3, 7, 5, 100);
    sequencer.setStepVelocity(3, 7, 9, 95);
    sequencer.setStepVelocity(3, 7, 13, 85);
    sequencer.setStepVelocity(3, 12, 0, 110);  // CL: acentuada
    sequencer.setStepVelocity(3, 12, 3, 95);
    sequencer.setStepVelocity(3, 12, 6, 100);
    sequencer.setStepVelocity(3, 12, 10, 90);
    // Congas: dinámica natural
    sequencer.setStepVelocity(3, 13, 2, 95);   // HC
    sequencer.setStepVelocity(3, 13, 7, 110);  // HC: acento
    sequencer.setStepVelocity(3, 13, 11, 85);
    sequencer.setStepVelocity(3, 14, 4, 90);   // MC
    sequencer.setStepVelocity(3, 14, 9, 105);
    sequencer.setStepVelocity(3, 14, 14, 80);
    sequencer.setStepVelocity(3, 15, 0, 100);  // LC
    sequencer.setStepVelocity(3, 15, 6, 110);  // LC: acento
    sequencer.setStepVelocity(3, 15, 12, 95);
    // Maracas: shake alternado
    for(int i=1; i<16; i+=2) sequencer.setStepVelocity(3, 11, i, (i%4==1) ? 80 : 55);

    sequencer.selectPattern(0); // Empezar con Hip Hop
    // sequencer.start(); // DISABLED: User must press PLAY

    // 5. WiFi: STA (casa) + AP (RED808 fallback)
    
    showWiFiLED();
    delay(500);
    
    if (webInterface.begin(AP_SSID, AP_PASSWORD,
                           HOME_WIFI_SSID, HOME_WIFI_PASS,
                           HOME_WIFI_TIMEOUT)) {
        showWebServerLED();
        delay(500);
    }

        // Pattern bank autoload disabled: using built-in 6 sampler patterns (HIP HOP, TECHNO, DnB, BREAK, HOUSE, TRAP)
        syslog("BOOT", "Pattern bank autoload disabled: using built-in 6 sampler patterns");

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
                int next = (sequencer.getCurrentPattern() + 1) % DSQ_PATTERNS;
                sequencer.selectPattern(next);
                spiMaster.dsqSelectPattern((uint8_t)next);
                if (funcId == BTN_FUNC_NEXT_PAT_PLAY) { sequencer.start(); spiMaster.dsqControl(1); }
                ctrlButtons.flashLed(btnIdx);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"nextPattern\",\"pattern\":%d}", next);
                webInterface.broadcastRaw(buf);
                break;
            }
            case BTN_FUNC_PREV_PATTERN:
            case BTN_FUNC_PREV_PAT_PLAY: {
                int prev = (sequencer.getCurrentPattern() + DSQ_PATTERNS - 1) % DSQ_PATTERNS;
                sequencer.selectPattern(prev);
                spiMaster.dsqSelectPattern((uint8_t)prev);
                if (funcId == BTN_FUNC_PREV_PAT_PLAY) { sequencer.start(); spiMaster.dsqControl(1); }
                ctrlButtons.flashLed(btnIdx);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"prevPattern\",\"pattern\":%d}", prev);
                webInterface.broadcastRaw(buf);
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
                sequencer.selectPattern(pIdx);
                spiMaster.dsqSelectPattern((uint8_t)pIdx);
                ctrlButtons.flashLed(btnIdx, CTRL_CLR_CYAN);
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"physButton\",\"action\":\"nextPattern\",\"pattern\":%d}", pIdx);
                webInterface.broadcastRaw(buf);
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
    esp_task_wdt_init(&twdtConfig);

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
    esp_task_wdt_reset();   // feed TWDT every iteration
    drainPadTriggerAutoOff(); // release expired pad-trigger voices
    vTaskDelay(pdMS_TO_TICKS(100)); // loop() no hace nada crítico
}
