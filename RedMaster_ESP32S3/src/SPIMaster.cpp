/*
 * SPIMaster.cpp
 * RED808 SPI Master — Full implementation
 * Sends commands to STM32 Audio Slave via SPI
 */

#include "SPIMaster.h"
#include <SPI.h>
#include <esp_task_wdt.h>

static constexpr uint8_t CLEAN_TRACK_COUNT = 4;
static constexpr uint8_t CLEAN_TRACK_SLOT_BASE = MAX_PADS;
static constexpr uint16_t DAISY_SPI_PACKET_GAP_US = 10;

static bool cleanTrackToSampleSlot(int trackIndex, uint8_t& outSlot) {
    if (trackIndex < 0 || trackIndex >= CLEAN_TRACK_COUNT) return false;
    outSlot = (uint8_t)(CLEAN_TRACK_SLOT_BASE + trackIndex);
    return true;
}

// Bus HSPI (SPI3) — separado del display ST7789 que usa FSPI/SPI2
static SPIClass daisySpi(HSPI);

// ─── SPI command name lookup removed (was only used for spiLogCallback debug)
// ─── Use cmd hex value directly in logs (e.g. 0xEE = PING)

// ═══════════════════════════════════════════════════════
// FILTER PRESETS (static, for UI compatibility)
// ═══════════════════════════════════════════════════════
static const FilterPreset filterPresets[] = {
    {FILTER_NONE,       0,    0,    0, "None"},
    {FILTER_LOWPASS,    1000, 1.0f, 0, "Low Pass"},
    {FILTER_HIGHPASS,   1000, 1.0f, 0, "High Pass"},
    {FILTER_BANDPASS,   1000, 1.0f, 0, "Band Pass"},
    {FILTER_NOTCH,      1000, 1.0f, 0, "Notch"},
    {FILTER_ALLPASS,    1000, 1.0f, 0, "All Pass"},
    {FILTER_PEAKING,    1000, 1.0f, 6, "Peaking EQ"},
    {FILTER_LOWSHELF,   200,  0.7f, 6, "Low Shelf"},
    {FILTER_HIGHSHELF,  4000, 0.7f, 6, "High Shelf"},
    {FILTER_RESONANT,   800,  8.0f, 0, "Resonant"},
    {FILTER_REVERSE,    0,    0,    0, "Reverse"},
    {FILTER_HALFSPEED,  0,    0,    0, "Half Speed"},
    {FILTER_STUTTER,    0,    0,    0, "Stutter"}
};

// ═══════════════════════════════════════════════════════
// CONSTRUCTOR / DESTRUCTOR
// ═══════════════════════════════════════════════════════

SPIMaster::SPIMaster() : seqNumber(0), spiErrorCount(0), stm32Connected(false), spiMutex(nullptr), spiCmdQueue(nullptr), spiLogCallback(nullptr) {
    spiMutex = xSemaphoreCreateMutex();
    // Initialize cached state
    cachedMasterVolume = 100;
    cachedSeqVolume = 100;
    cachedLiveVolume = 100;
    cachedLivePitch = 1.0f;
    
    // New FX cached state
    cachedReverbActive = false;
    cachedReverbFeedback = 0.85f;
    cachedReverbLpFreq = 8000.0f;
    cachedReverbMix = 0.3f;
    cachedChorusActive = false;
    cachedChorusRate = 0.5f;
    cachedChorusDepth = 0.5f;
    cachedChorusMix = 0.4f;
    cachedTremoloActive = false;
    cachedTremoloRate = 4.0f;
    cachedTremoloDepth = 0.7f;
    cachedWaveFolderGain = 1.0f;
    cachedLimiterActive = false;
    cachedAutoWahActive = false;
    cachedAutoWahLevel = 80;
    cachedAutoWahMix = 50;
    cachedStereoWidth = 100;
    cachedEarlyRefActive = false;
    cachedEarlyRefMix = 30;
    cachedDelayStereoMode = 0;
    cachedChorusStereoMode = 0;
    memset(cachedChokeGroup, 0, sizeof(cachedChokeGroup));
    memset(&cachedStatus, 0, sizeof(cachedStatus));
    cachedSynthActiveMask16 = 0x01FF; // all 9 engines
    
    for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
        cachedTrackFilter[i] = FILTER_NONE;
        trackFilterActive[i] = false;
        cachedTrackEchoActive[i] = false;
        cachedTrackFlangerActive[i] = false;
        cachedTrackCompActive[i] = false;
        cachedTrackReverbSend[i] = 0;
        cachedTrackDelaySend[i] = 0;
        cachedTrackChorusSend[i] = 0;
        cachedTrackPan[i] = 0;
        cachedTrackMute[i] = false;
        cachedTrackSolo[i] = false;
        cachedTrackPeaks[i] = 0.0f;
    }
    
    for (int i = 0; i < MAX_PADS; i++) {
        cachedPadFilter[i] = FILTER_NONE;
        padFilterActive[i] = false;
        cachedPadLoop[i] = false;
    }
    
    cachedMasterPeak = 0.0f;
    lastPeakRequest = 0;
    lastStatusPoll = 0;
    eventCallback = nullptr;
    eventUserData = nullptr;
    
    memset(txBuffer, 0, sizeof(txBuffer));
    memset(rxBuffer, 0, sizeof(rxBuffer));
}

SPIMaster::~SPIMaster() {
    daisySpi.end();
    if (spiMutex) {
        vSemaphoreDelete(spiMutex);
        spiMutex = nullptr;
    }
    if (spiCmdQueue) {
        vQueueDelete(spiCmdQueue);
        spiCmdQueue = nullptr;
    }
}

// ═══════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════

bool SPIMaster::begin() {
    pinMode(DAISY_SPI_CS, OUTPUT);
    digitalWrite(DAISY_SPI_CS, HIGH);
    daisySpi.begin(DAISY_SPI_SCK, DAISY_SPI_MISO, DAISY_SPI_MOSI, DAISY_SPI_CS);

    if (!spiCmdQueue) {
        // Queue depth 64 × ~547B = ~35 KB. Sufficient for clean-track stream
        // chunks (4 per pump tick) plus normal control traffic.
        spiCmdQueue = xQueueCreate(64, sizeof(SpiQueuedCmd));
    }

    
    // Try to connect to Daisy
    uint32_t rtt;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (ping(rtt)) {
            stm32Connected = true;
            return true;
        }
        delay(200);
    }

    // Intencionadamente devolvemos true aunque la Daisy no responda: el
    // dispositivo debe arrancar igualmente (UI/web operativas) y process()
    // reintenta la conexion cada 3s. El caller debe consultar isConnected().
    Serial.println("[SPI] WARN: Daisy no responde al boot (10 pings) - seguira reintentando");
    return true;
}

// ═══════════════════════════════════════════════════════
// SPI LOW-LEVEL
// ═══════════════════════════════════════════════════════

uint16_t SPIMaster::crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// ── Core0→Core1 queue drain ─ called at start of every process() tick ──────
void SPIMaster::drainCmdQueue() {
    if (!spiCmdQueue) return;
    SpiQueuedCmd env;
    // Cap per tick. Each sendCommandDirect blocks ~4ms at 1MHz, so a big cap (32)
    // means a ~128ms burst that hogs Core1 for the whole duration of a bulk
    // sample transfer (~16s) and starves the idle/other tasks → reset. Keep the
    // burst short so Core1 yields (process() does vTaskDelay(1) after) and the
    // watchdog stays fed. Normal control traffic rarely queues >8 at once.
    int processed = 0;
    while (processed < 8 && xQueueReceive(spiCmdQueue, &env, 0) == pdTRUE) {
        sendCommandDirect(env.cmd, env.payload, env.payloadLen);
        processed++;
    }
}

// ── High-level sendCommand: enqueues from Core0, sends directly from Core1 ───
bool SPIMaster::sendCommand(uint8_t cmd, const void* payload, uint16_t payloadLen) {
    // Core0 (WiFi/WS task): enqueue for Core1 to send — never block WS handler
    if (xPortGetCoreID() == 0 && spiCmdQueue && payloadLen <= SPI_QUEUE_PAYLOAD_MAX) {
        SpiQueuedCmd env;
        env.cmd = cmd;
        env.payloadLen = (uint16_t)payloadLen;
        if (payload && payloadLen > 0) memcpy(env.payload, payload, payloadLen);
        // Non-blocking enqueue; if queue full, DROP command (never fall through
        // to sendCommandDirect which would block Core0 on spiMutex and risk WDT)
        BaseType_t queued = xQueueSend(spiCmdQueue, &env, pdMS_TO_TICKS(5));
        if (queued == pdTRUE) return true;

        spiErrorCount++;
        // Nota: NO usar Serial.printf aquí: estamos en Core0 (WS handler) y bloquearía WiFi.
        // syslog() es no bloqueante (UDP) si está disponible.

        if (cmd == CMD_TRIGGER_SEQ || cmd == CMD_TRIGGER_LIVE || cmd == CMD_DSQ_CONTROL ||
            cmd == CMD_DSQ_SET_STEP || cmd == CMD_DSQ_SELECT_PATTERN) {
            vTaskDelay(pdMS_TO_TICKS(1));
            queued = xQueueSend(spiCmdQueue, &env, pdMS_TO_TICKS(10));
            if (queued == pdTRUE) return true;
        }
        return false;
    }
    return sendCommandDirect(cmd, payload, payloadLen);
}

// ── Raw SPI send (always executes synchronously) ─────────────────────────────
bool SPIMaster::sendCommandDirect(uint8_t cmd, const void* payload, uint16_t payloadLen) {
    SPIPacketHeader header;
    header.magic = SPI_MAGIC_CMD;
    header.cmd = cmd;
    header.length = payloadLen;
    header.checksum = (payload && payloadLen > 0) ? crc16((const uint8_t*)payload, payloadLen) : 0;

    // Acquire mutex (thread safety Core0 ↔ Core1)
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
        return false;
    }
    // seqNumber se incrementa BAJO el mutex: se escribe desde ambos nucleos y
    // un ++ concurrente duplicaba/saltaba secuencias, rompiendo la verificacion
    // de secuencia del slave.
    header.sequence = seqNumber++;

    const uint16_t totalLen = sizeof(SPIPacketHeader) + payloadLen;
    if (totalLen > sizeof(txBuffer)) {
        xSemaphoreGive(spiMutex);
        return false;
    }

    memcpy(txBuffer, &header, sizeof(SPIPacketHeader));
    if (payload && payloadLen > 0) {
        memcpy(txBuffer + sizeof(SPIPacketHeader), payload, payloadLen);
    }

    // Per-command SPI clock. Sample/clean-track DATA streams are paced (inter-
    // packet gap + queue), so they run reliably at the higher 4MHz the system
    // 1MHz uniforme para todos los comandos: el RXFIFO hardware de la Daisy
    // (SPI1, 16 bytes) se desborda a 4MHz — TIM6 drena cada 100µs pero el FIFO
    // se llena en 32µs. Los bytes perdidos corrompen CMD_SAMPLE_BEGIN (campo
    // totalSamples), el CRC falla, el comando se descarta, padLoading[pad] nunca
    // se activa y sampleLoaded queda false. NO subir a >1MHz sin acelerar el
    // drenado TIM6 en la Daisy (ARR < 32µs a 4MHz, actualmente ARR=9999=100µs).
    uint32_t clockHz = DAISY_SPI_CLOCK_HZ;
    daisySpi.beginTransaction(SPISettings(clockHz, MSBFIRST, SPI_MODE0));
    digitalWrite(DAISY_SPI_CS, LOW);
    daisySpi.transferBytes(txBuffer, rxBuffer, totalLen);
    digitalWrite(DAISY_SPI_CS, HIGH);
    daisySpi.endTransaction();

    /* Inter-packet gap: dar tiempo a la Daisy para drenar RXFIFO
     * y procesar el paquete anterior en su main loop.            */
    delayMicroseconds(DAISY_SPI_PACKET_GAP_US);
    
    xSemaphoreGive(spiMutex);

    if (spiLogCallback && cmd != CMD_GET_PEAKS && cmd != CMD_GET_CPU_LOAD
                       && cmd != CMD_PING    && cmd != CMD_GET_STATUS
                       && cmd != CMD_GET_VOICES) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"spi_log\",\"cmd\":\"0x%02X\",\"seq\":%d,\"len\":%d,\"ms\":%lu}",
            (unsigned)cmd, (int)header.sequence,
            (int)payloadLen, (unsigned long)millis());
        spiLogCallback(buf);
    }
    return true;
}

bool SPIMaster::sendAndReceive(uint8_t cmd, const void* payload, uint16_t payloadLen,
                                void* response, uint16_t responseLen) {
    SPIPacketHeader header;
    header.magic = SPI_MAGIC_CMD;
    header.cmd = cmd;
    header.length = payloadLen;
    header.checksum = (payload && payloadLen > 0) ? crc16((const uint8_t*)payload, payloadLen) : 0;

    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    header.sequence = seqNumber++;  // bajo mutex (ver sendCommandDirect)

    const uint16_t totalLen = sizeof(SPIPacketHeader) + payloadLen;
    if (totalLen > sizeof(txBuffer)) {
        xSemaphoreGive(spiMutex);
        return false;
    }

    memcpy(txBuffer, &header, sizeof(SPIPacketHeader));
    if (payload && payloadLen > 0) {
        memcpy(txBuffer + sizeof(SPIPacketHeader), payload, payloadLen);
    }

    daisySpi.beginTransaction(SPISettings(DAISY_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(DAISY_SPI_CS, LOW);
    daisySpi.transferBytes(txBuffer, rxBuffer, totalLen);
    digitalWrite(DAISY_SPI_CS, HIGH);
    daisySpi.endTransaction();

    delayMicroseconds(DAISY_SPI_RESPONSE_GAP_US);

    // Read response header + payload (segunda transacción CS)
    SPIPacketHeader respHeader = {};
    bool success = false;
    uint8_t attempts = 0;
    // 8 attempts × ~800µs = ≤6.4ms max block time from Core0 WS handler
    static constexpr uint8_t kMaxAttempts = 8;

    while (!success && attempts < kMaxAttempts) {
        attempts++;
        memset(&respHeader, 0, sizeof(respHeader));
        memset(txBuffer, 0xFF, sizeof(SPIPacketHeader));

        daisySpi.beginTransaction(SPISettings(DAISY_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
        digitalWrite(DAISY_SPI_CS, LOW);
        daisySpi.transferBytes(txBuffer, reinterpret_cast<uint8_t*>(&respHeader), sizeof(SPIPacketHeader));

        const bool headerOk = (respHeader.magic == SPI_MAGIC_RESP) &&
                              (respHeader.cmd == cmd) &&
                              (respHeader.length <= responseLen) &&
                              (respHeader.length <= (SPI_MAX_PAYLOAD - sizeof(SPIPacketHeader)));

        if (headerOk) {
            if (respHeader.length > 0 && response) {
                memset(txBuffer, 0xFF, respHeader.length);
                daisySpi.transferBytes(txBuffer, static_cast<uint8_t*>(response), respHeader.length);
            }
            success = true;
        }

        digitalWrite(DAISY_SPI_CS, HIGH);
        daisySpi.endTransaction();

        if (!success) {
            delayMicroseconds(800);
        }
    }

    if (!success) {
        spiErrorCount++;
    }

    xSemaphoreGive(spiMutex);

    if (spiLogCallback && cmd != CMD_GET_PEAKS && cmd != CMD_GET_CPU_LOAD
                       && cmd != CMD_PING    && cmd != CMD_GET_STATUS
                       && cmd != CMD_GET_VOICES) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"spi_log\",\"cmd\":\"0x%02X\",\"seq\":%d,\"ok\":%s,\"try\":%d,\"ms\":%lu}",
            (unsigned)cmd, (int)header.sequence,
            success ? "true" : "false", (int)attempts, (unsigned long)millis());
        spiLogCallback(buf);
    }

    return success;
}

// ═══════════════════════════════════════════════════════
// PROCESS (called from task loop)
// ═══════════════════════════════════════════════════════

void SPIMaster::process() {
    // ── 1. Drain commands queued from Core0 (WS/WiFi task) ──
    drainCmdQueue();

    // Backlog en la cola = transfer bulk en curso (sample/clean-track/DSQ).
    // Saltar el polling de telemetria mientras tanto: cada round-trip de
    // peaks/status retiene spiMutex ~0.8-6ms y compite con el drenado de
    // chunks, alargando los uploads y arriesgando drops.
    const bool bulkBacklog = spiCmdQueue && uxQueueMessagesWaiting(spiCmdQueue) > 0;

    // ── 2. Keepalive PING every 2s (with RTT tracking for telemetry) ──
    static uint32_t lastHeartbeat = 0;
    if (stm32Connected && !bulkBacklog && (millis() - lastHeartbeat > 2000)) {
        uint32_t rttUs = 0;
        if (ping(rttUs)) {
            lastPingRttMs = (float)rttUs / 1000.0f;
        }
        lastHeartbeat = millis();
    }

    // ── 3. Poll audio peaks every 200ms (was 120ms — reduces SPI mutex hold time) ──
    static uint32_t lastPeakPoll = 0;
    if (stm32Connected && !bulkBacklog && (millis() - lastPeakPoll > 200)) {
        requestPeaks();
        lastPeakPoll = millis();
    }

    // ── 4. Refresh status for /adm telemetry every 3s (immediate on first run) ──
    static bool firstStatusPoll = true;
    if ((firstStatusPoll || millis() - lastStatusPoll > 3000) && !bulkBacklog) {
        firstStatusPoll = false;
        if (stm32Connected) {
            requestStatus();
            drainEvents();
            // Also refresh SD status cache
            SdStatusResponse sdTmp;
            if (sendAndReceive(CMD_SD_STATUS, nullptr, 0, &sdTmp, sizeof(sdTmp))) {
                cachedSdStatus = sdTmp;
                cachedSdStatusValid = true;
            }
        }
        lastStatusPoll = millis();
    }

    // ── 5. Reconnect if link dropped ──
    if (!stm32Connected) {
        static uint32_t lastRetry = 0;
        if (millis() - lastRetry > 3000) {
            uint32_t rtt;
            if (ping(rtt)) stm32Connected = true;
            lastRetry = millis();
        }
    }
}

// ═══════════════════════════════════════════════════════
// TRIGGERS
// ═══════════════════════════════════════════════════════

void SPIMaster::triggerSampleSequencer(int padIndex, uint8_t velocity, uint8_t trackVolume, uint32_t maxSamples) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    TriggerSeqPayload payload;
    payload.padIndex = (uint8_t)padIndex;
    payload.velocity = velocity;
    payload.trackVolume = trackVolume;
    payload.reserved = 0;
    payload.maxSamples = maxSamples;
    
    sendCommand(CMD_TRIGGER_SEQ, &payload, sizeof(payload));
}

void SPIMaster::triggerSampleLive(int padIndex, uint8_t velocity) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    TriggerLivePayload payload;
    payload.padIndex = (uint8_t)padIndex;
    payload.velocity = velocity;
    
    sendCommand(CMD_TRIGGER_LIVE, &payload, sizeof(payload));
}

void SPIMaster::triggerSample(int padIndex, uint8_t velocity) {
    triggerSampleLive(padIndex, velocity);
}

void SPIMaster::stopSample(int padIndex) {
    uint8_t pad = (uint8_t)padIndex;
    sendCommand(CMD_TRIGGER_STOP, &pad, 1);
}

void SPIMaster::stopAll() {
    sendCommand(CMD_TRIGGER_STOP_ALL, nullptr, 0);
}

void SPIMaster::triggerSidechain(int sourceTrack) {
    if (sourceTrack < 0 || sourceTrack >= MAX_AUDIO_TRACKS) return;
    SidechainTriggerPayload p = {(uint8_t)sourceTrack, 0};
    sendCommand(CMD_TRIGGER_SIDECHAIN, &p, sizeof(p));
}

bool SPIMaster::triggerBulk(const TriggerSeqPayload* triggers, uint8_t count) {
    if (!triggers || count == 0 || count > 16) return false;
    BulkTriggersPayload p = {};
    p.count = count;
    p.reserved = 0;
    memcpy(p.triggers, triggers, count * sizeof(TriggerSeqPayload));
    // Only send header + actual trigger data (not the full 16-slot array)
    uint16_t payloadLen = (uint16_t)(2 + count * sizeof(TriggerSeqPayload));
    return sendCommand(CMD_BULK_TRIGGERS, &p, payloadLen);
}

// ═══════════════════════════════════════════════════════
// VOLUME CONTROL
// ═══════════════════════════════════════════════════════

void SPIMaster::setMasterVolume(uint8_t volume) {
    cachedMasterVolume = volume;
    VolumePayload p = {volume};
    sendCommand(CMD_MASTER_VOLUME, &p, sizeof(p));
}

uint8_t SPIMaster::getMasterVolume() {
    return cachedMasterVolume;
}

void SPIMaster::setSequencerVolume(uint8_t volume) {
    cachedSeqVolume = volume;
    VolumePayload p = {volume};
    sendCommand(CMD_SEQ_VOLUME, &p, sizeof(p));
}

uint8_t SPIMaster::getSequencerVolume() {
    return cachedSeqVolume;
}

void SPIMaster::setLiveVolume(uint8_t volume) {
    cachedLiveVolume = volume;
    VolumePayload p = {volume};
    sendCommand(CMD_LIVE_VOLUME, &p, sizeof(p));
}

uint8_t SPIMaster::getLiveVolume() {
    return cachedLiveVolume;
}

void SPIMaster::setTrackVolume(int track, uint8_t volume) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    TrackVolumePayload p = {(uint8_t)track, volume};
    sendCommand(CMD_TRACK_VOLUME, &p, sizeof(p));
}

void SPIMaster::setLivePitchShift(float pitch) {
    cachedLivePitch = constrain(pitch, 0.25f, 3.0f);
    PitchPayload p = {cachedLivePitch};
    sendCommand(CMD_LIVE_PITCH, &p, sizeof(p));
}

void SPIMaster::setTempo(float bpm) {
    float clamped = constrain(bpm, 40.0f, 300.0f);
    FloatPayload p = {clamped};
    sendCommand(CMD_TEMPO, &p, sizeof(p));
}

float SPIMaster::getLivePitchShift() {
    return cachedLivePitch;
}

// ═══════════════════════════════════════════════════════
// GLOBAL FILTER
// ═══════════════════════════════════════════════════════

void SPIMaster::setFilterType(FilterType type) {
    GlobalFilterPayload p = {};
    p.filterType = (uint8_t)type;
    sendCommand(CMD_FILTER_SET, &p, sizeof(p));
}

void SPIMaster::setFilterCutoff(float cutoff) {
    FloatPayload p = {cutoff};
    sendCommand(CMD_FILTER_CUTOFF, &p, sizeof(p));
}

void SPIMaster::setFilterResonance(float resonance) {
    FloatPayload p = {resonance};
    sendCommand(CMD_FILTER_RESONANCE, &p, sizeof(p));
}

void SPIMaster::setBitDepth(uint8_t bits) {
    sendCommand(CMD_FILTER_BITDEPTH, &bits, 1);
}

void SPIMaster::setDistortion(float amount) {
    FloatPayload p = {amount};
    sendCommand(CMD_FILTER_DISTORTION, &p, sizeof(p));
}

void SPIMaster::setDistortionMode(DistortionMode mode) {
    uint8_t m = (uint8_t)mode;
    sendCommand(CMD_FILTER_DIST_MODE, &m, 1);
}

void SPIMaster::setSampleRateReduction(uint32_t rate) {
    Uint32Payload p = {rate};
    sendCommand(CMD_FILTER_SR_REDUCE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER EFFECTS — DELAY
// ═══════════════════════════════════════════════════════

void SPIMaster::setDelayActive(bool active) {
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_DELAY_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setDelayTime(float ms) {
    FloatPayload p = {ms};
    sendCommand(CMD_DELAY_TIME, &p, sizeof(p));
}

void SPIMaster::setDelayFeedback(float feedback) {
    FloatPayload p = {feedback};
    sendCommand(CMD_DELAY_FEEDBACK, &p, sizeof(p));
}

void SPIMaster::setDelayMix(float mix) {
    FloatPayload p = {mix};
    sendCommand(CMD_DELAY_MIX, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER EFFECTS — PHASER
// ═══════════════════════════════════════════════════════

void SPIMaster::setPhaserActive(bool active) {
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_PHASER_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setPhaserRate(float hz) {
    FloatPayload p = {hz};
    sendCommand(CMD_PHASER_RATE, &p, sizeof(p));
}

void SPIMaster::setPhaserDepth(float depth) {
    FloatPayload p = {depth};
    sendCommand(CMD_PHASER_DEPTH, &p, sizeof(p));
}

void SPIMaster::setPhaserFeedback(float feedback) {
    FloatPayload p = {feedback};
    sendCommand(CMD_PHASER_FEEDBACK, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER EFFECTS — FLANGER
// ═══════════════════════════════════════════════════════

void SPIMaster::setFlangerActive(bool active) {
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_FLANGER_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setFlangerRate(float hz) {
    FloatPayload p = {hz};
    sendCommand(CMD_FLANGER_RATE, &p, sizeof(p));
}

void SPIMaster::setFlangerDepth(float depth) {
    FloatPayload p = {depth};
    sendCommand(CMD_FLANGER_DEPTH, &p, sizeof(p));
}

void SPIMaster::setFlangerFeedback(float feedback) {
    FloatPayload p = {feedback};
    sendCommand(CMD_FLANGER_FEEDBACK, &p, sizeof(p));
}

void SPIMaster::setFlangerMix(float mix) {
    FloatPayload p = {mix};
    sendCommand(CMD_FLANGER_MIX, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER EFFECTS — COMPRESSOR
// ═══════════════════════════════════════════════════════

void SPIMaster::setCompressorActive(bool active) {
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_COMP_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setCompressorThreshold(float threshold) {
    FloatPayload p = {threshold};
    sendCommand(CMD_COMP_THRESHOLD, &p, sizeof(p));
}

void SPIMaster::setCompressorRatio(float ratio) {
    FloatPayload p = {ratio};
    sendCommand(CMD_COMP_RATIO, &p, sizeof(p));
}

void SPIMaster::setCompressorAttack(float ms) {
    FloatPayload p = {ms};
    sendCommand(CMD_COMP_ATTACK, &p, sizeof(p));
}

void SPIMaster::setCompressorRelease(float ms) {
    FloatPayload p = {ms};
    sendCommand(CMD_COMP_RELEASE, &p, sizeof(p));
}

void SPIMaster::setCompressorMakeupGain(float db) {
    FloatPayload p = {db};
    sendCommand(CMD_COMP_MAKEUP, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// PER-TRACK FILTER
// ═══════════════════════════════════════════════════════

bool SPIMaster::setTrackFilter(int track, FilterType type, float cutoff, float resonance, float gain) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
    
    cachedTrackFilter[track] = type;
    trackFilterActive[track] = (type != FILTER_NONE);
    
    TrackFilterPayload p = {};
    p.track = (uint8_t)track;
    p.filterType = (uint8_t)type;
    p.cutoff = cutoff;
    p.resonance = resonance;
    p.gain = gain;
    
    sendCommand(CMD_TRACK_FILTER, &p, sizeof(p));
    return true;
}

void SPIMaster::clearTrackFilter(int track) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    cachedTrackFilter[track] = FILTER_NONE;
    trackFilterActive[track] = false;
    
    uint8_t t = (uint8_t)track;
    sendCommand(CMD_TRACK_CLEAR_FILTER, &t, 1);
}

FilterType SPIMaster::getTrackFilter(int track) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return FILTER_NONE;
    return cachedTrackFilter[track];
}

int SPIMaster::getActiveTrackFiltersCount() {
    int count = 0;
    for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
        if (trackFilterActive[i]) count++;
    }
    return count;
}

// ═══════════════════════════════════════════════════════
// PER-PAD FILTER
// ═══════════════════════════════════════════════════════

bool SPIMaster::setPadFilter(int pad, FilterType type, float cutoff, float resonance, float gain) {
    if (pad < 0 || pad >= MAX_PADS) return false;
    
    cachedPadFilter[pad] = type;
    padFilterActive[pad] = (type != FILTER_NONE);
    
    PadFilterPayload p = {};
    p.pad = (uint8_t)pad;
    p.filterType = (uint8_t)type;
    p.cutoff = cutoff;
    p.resonance = resonance;
    p.gain = gain;
    
    sendCommand(CMD_PAD_FILTER, &p, sizeof(p));
    return true;
}

void SPIMaster::clearPadFilter(int pad) {
    if (pad < 0 || pad >= MAX_PADS) return;
    
    cachedPadFilter[pad] = FILTER_NONE;
    padFilterActive[pad] = false;
    
    uint8_t p = (uint8_t)pad;
    sendCommand(CMD_PAD_CLEAR_FILTER, &p, 1);
}

FilterType SPIMaster::getPadFilter(int pad) {
    if (pad < 0 || pad >= MAX_PADS) return FILTER_NONE;
    return cachedPadFilter[pad];
}

int SPIMaster::getActivePadFiltersCount() {
    int count = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (padFilterActive[i]) count++;
    }
    return count;
}

// ═══════════════════════════════════════════════════════
// PER-PAD / PER-TRACK FX
// ═══════════════════════════════════════════════════════

void SPIMaster::setPadDistortion(int pad, float amount, DistortionMode mode) {
    if (pad < 0 || pad >= MAX_PADS) return;
    
    PadDistortionPayload p = {};
    p.pad = (uint8_t)pad;
    p.distMode = (uint8_t)mode;
    p.amount = amount;
    
    sendCommand(CMD_PAD_DISTORTION, &p, sizeof(p));
}

void SPIMaster::setPadBitCrush(int pad, uint8_t bits) {
    if (pad < 0 || pad >= MAX_PADS) return;
    
    PadBitCrushPayload p = {(uint8_t)pad, bits};
    sendCommand(CMD_PAD_BITCRUSH, &p, sizeof(p));
}

void SPIMaster::clearPadFX(int pad) {
    if (pad < 0 || pad >= MAX_PADS) return;
    
    uint8_t p = (uint8_t)pad;
    sendCommand(CMD_PAD_CLEAR_FX, &p, 1);
}

void SPIMaster::setTrackDistortion(int track, float amount, DistortionMode mode) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    PadDistortionPayload p = {};
    p.pad = (uint8_t)track;
    p.distMode = (uint8_t)mode;
    p.amount = amount;
    
    sendCommand(CMD_TRACK_DISTORTION, &p, sizeof(p));
}

void SPIMaster::setTrackBitCrush(int track, uint8_t bits) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    PadBitCrushPayload p = {(uint8_t)track, bits};
    sendCommand(CMD_TRACK_BITCRUSH, &p, sizeof(p));
}

void SPIMaster::clearTrackFX(int track) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    uint8_t t = (uint8_t)track;
    sendCommand(CMD_TRACK_CLEAR_FX, &t, 1);
}

// ═══════════════════════════════════════════════════════
// PER-TRACK LIVE FX (Echo, Flanger, Compressor)
// ═══════════════════════════════════════════════════════

void SPIMaster::setTrackEcho(int track, bool active, float time, float feedback, float mix) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    cachedTrackEchoActive[track] = active;
    
    TrackEchoPayload p = {};
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.time = time;
    p.feedback = feedback;
    p.mix = mix;
    
    sendCommand(CMD_TRACK_ECHO, &p, sizeof(p));
}

void SPIMaster::setTrackFlanger(int track, bool active, float rate, float depth, float feedback) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    cachedTrackFlangerActive[track] = active;
    
    TrackFlangerPayload p = {};
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.rate = rate;
    p.depth = depth;
    p.feedback = feedback;
    
    sendCommand(CMD_TRACK_FLANGER_FX, &p, sizeof(p));
}

void SPIMaster::setTrackCompressor(int track, bool active, float threshold, float ratio) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    cachedTrackCompActive[track] = active;
    
    TrackCompressorPayload p = {};
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.threshold = threshold;
    p.ratio = ratio;
    
    sendCommand(CMD_TRACK_COMPRESSOR, &p, sizeof(p));
}

void SPIMaster::clearTrackLiveFX(int track) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    
    cachedTrackEchoActive[track] = false;
    cachedTrackFlangerActive[track] = false;
    cachedTrackCompActive[track] = false;
    
    uint8_t t = (uint8_t)track;
    sendCommand(CMD_TRACK_CLEAR_LIVE, &t, 1);
}

bool SPIMaster::getTrackEchoActive(int track) const {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
    return cachedTrackEchoActive[track];
}

bool SPIMaster::getTrackFlangerActive(int track) const {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
    return cachedTrackFlangerActive[track];
}

bool SPIMaster::getTrackCompressorActive(int track) const {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return false;
    return cachedTrackCompActive[track];
}

// ═══════════════════════════════════════════════════════
// PER-TRACK FX SEND LEVELS
// ═══════════════════════════════════════════════════════

void SPIMaster::setTrackReverbSend(int track, uint8_t level) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackReverbSend[track] = level;
    TrackSendPayload p = {};
    p.track = (uint8_t)track;
    p.sendLevel = level;
    sendCommand(CMD_TRACK_REVERB_SEND, &p, sizeof(p));
}

void SPIMaster::setTrackDelaySend(int track, uint8_t level) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackDelaySend[track] = level;
    TrackSendPayload p = {};
    p.track = (uint8_t)track;
    p.sendLevel = level;
    sendCommand(CMD_TRACK_DELAY_SEND, &p, sizeof(p));
}

void SPIMaster::setTrackChorusSend(int track, uint8_t level) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackChorusSend[track] = level;
    TrackSendPayload p = {};
    p.track = (uint8_t)track;
    p.sendLevel = level;
    sendCommand(CMD_TRACK_CHORUS_SEND, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// PER-TRACK MIXER (PAN, MUTE, SOLO)
// ═══════════════════════════════════════════════════════

void SPIMaster::setTrackPan(int track, int8_t pan) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackPan[track] = pan;
    TrackPanPayload p = {};
    p.track = (uint8_t)track;
    p.pan = pan;
    sendCommand(CMD_TRACK_PAN, &p, sizeof(p));
}

void SPIMaster::setTrackMute(int track, bool mute) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackMute[track] = mute;
    TrackMuteSoloPayload p = {};
    p.track = (uint8_t)track;
    p.enabled = mute ? 1 : 0;
    sendCommand(CMD_TRACK_MUTE, &p, sizeof(p));
}

void SPIMaster::setTrackSolo(int track, bool solo) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    cachedTrackSolo[track] = solo;
    TrackMuteSoloPayload p = {};
    p.track = (uint8_t)track;
    p.enabled = solo ? 1 : 0;
    sendCommand(CMD_TRACK_SOLO, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// PER-TRACK EXTENDED FX (phaser, tremolo, pitch, gate, EQ)
// ═══════════════════════════════════════════════════════

void SPIMaster::setTrackPhaser(int track, bool active, float rate, float depth, float feedback) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    TrackFlangerPayload p = {};  // reuse same struct layout
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.rate = rate;
    p.depth = depth;
    p.feedback = feedback;
    sendCommand(CMD_TRACK_PHASER, &p, sizeof(p));
}

void SPIMaster::setTrackTremolo(int track, bool active, float rate, float depth, uint8_t wave, uint8_t target) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    struct __attribute__((packed)) { uint8_t track; uint8_t active; uint8_t wave; uint8_t target; float rate; float depth; } p = {};
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.wave = wave;
    p.target = target;
    p.rate = rate;
    p.depth = depth;
    sendCommand(CMD_TRACK_TREMOLO, &p, sizeof(p));
}

void SPIMaster::setTrackPitch(int track, int16_t cents) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    struct __attribute__((packed)) { uint8_t track; uint8_t reserved; int16_t cents; } p = {};
    p.track = (uint8_t)track;
    p.cents = cents;
    sendCommand(CMD_TRACK_PITCH, &p, sizeof(p));
}

void SPIMaster::setTrackGate(int track, bool active, float thresholdDb, float attackMs, float releaseMs) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    TrackGatePayload p = {};
    p.track = (uint8_t)track;
    p.active = active ? 1 : 0;
    p.threshold = thresholdDb;
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    sendCommand(CMD_TRACK_GATE, &p, sizeof(p));
}

void SPIMaster::setTrackEqLow(int track, int8_t gainDb) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    struct __attribute__((packed)) { uint8_t track; int8_t gain; } p = {};
    p.track = (uint8_t)track;
    p.gain = gainDb;
    sendCommand(CMD_TRACK_EQ_LOW, &p, sizeof(p));
}

void SPIMaster::setTrackEqMid(int track, int8_t gainDb) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    struct __attribute__((packed)) { uint8_t track; int8_t gain; } p = {};
    p.track = (uint8_t)track;
    p.gain = gainDb;
    sendCommand(CMD_TRACK_EQ_MID, &p, sizeof(p));
}

void SPIMaster::setTrackEqHigh(int track, int8_t gainDb) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    struct __attribute__((packed)) { uint8_t track; int8_t gain; } p = {};
    p.track = (uint8_t)track;
    p.gain = gainDb;
    sendCommand(CMD_TRACK_EQ_HIGH, &p, sizeof(p));
}

void SPIMaster::setTrackEq(int track, int8_t low, int8_t mid, int8_t high) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return;
    TrackEqPayload p = {};
    p.track = (uint8_t)track;
    p.gainLow = low;
    p.gainMid = mid;
    p.gainHigh = high;
    // Send as 3 individual commands (no single CMD for full EQ band set)
    setTrackEqLow(track, low);
    setTrackEqMid(track, mid);
    setTrackEqHigh(track, high);
}

// ═══════════════════════════════════════════════════════
// SIDECHAIN
// ═══════════════════════════════════════════════════════

void SPIMaster::setSidechain(bool active, int sourceTrack, uint16_t destinationMask,
                              float amount, float attackMs, float releaseMs, float knee) {
    SidechainPayload p = {};
    p.active = active ? 1 : 0;
    p.sourceTrack = (uint8_t)sourceTrack;
    p.destMask = destinationMask;
    p.amount = amount;
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    p.knee = knee;
    
    sendCommand(CMD_SIDECHAIN_SET, &p, sizeof(p));
}

void SPIMaster::clearSidechain() {
    sendCommand(CMD_SIDECHAIN_CLEAR, nullptr, 0);
}

// ═══════════════════════════════════════════════════════
// PAD CONTROL (loop, reverse, pitch, stutter)
// ═══════════════════════════════════════════════════════

void SPIMaster::setPadLoop(int padIndex, bool enabled) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    cachedPadLoop[padIndex] = enabled;
    
    PadLoopPayload p = {(uint8_t)padIndex, (uint8_t)(enabled ? 1 : 0)};
    sendCommand(CMD_PAD_LOOP, &p, sizeof(p));
    
}

bool SPIMaster::isPadLooping(int padIndex) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return false;
    return cachedPadLoop[padIndex];
}

void SPIMaster::setReverseSample(int padIndex, bool reverse) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    PadReversePayload p = {(uint8_t)padIndex, (uint8_t)(reverse ? 1 : 0)};
    sendCommand(CMD_PAD_REVERSE, &p, sizeof(p));
}

void SPIMaster::setTrackPitchShift(int padIndex, float pitch) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    PadPitchPayload p = {};
    p.pad = (uint8_t)padIndex;
    p.pitch = pitch;
    
    sendCommand(CMD_PAD_PITCH, &p, sizeof(p));
}

void SPIMaster::setStutter(int padIndex, bool active, int intervalMs) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return;
    
    PadStutterPayload p = {};
    p.pad = (uint8_t)padIndex;
    p.active = active ? 1 : 0;
    p.intervalMs = (uint16_t)intervalMs;
    
    sendCommand(CMD_PAD_STUTTER, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// SAMPLE TRANSFER
// ═══════════════════════════════════════════════════════

bool SPIMaster::setSampleBuffer(int padIndex, int16_t* buffer, uint32_t length) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return false;
    
    // Transfer sample data to STM32
    if (buffer && length > 0) {
        return transferSample(padIndex, buffer, length);
    } else {
        // Unload
        unloadSample(padIndex);
        return true;
    }
}

bool SPIMaster::transferSample(int padIndex, int16_t* buffer, uint32_t numSamples) {
    if (!buffer || numSamples == 0) return false;

    uint32_t totalBytes = numSamples * sizeof(int16_t);

    // Reliable, self-pacing enqueue. sendCommand() from Core0 DROPS the command
    // when the SPI queue is full, and at 1MHz the link is the bottleneck for a
    // bulk sample transfer — so the queue fills and chunks get dropped silently.
    // A dropped CMD_SAMPLE_DATA leaves stale SDRAM in the Daisy buffer (noise at
    // that position); many drops on a big sample corrupt it and the unpaced
    // flood can wedge the transfer. Retry until the command is actually queued,
    // yielding so Core1 drains the queue and watchdog/other tasks stay fed.
    // Abort cleanly if the link is genuinely stalled for several seconds.
    auto enqueue = [&](uint8_t c, const void* pl, uint16_t plen) -> bool {
        uint32_t tries = 0;
        while (!sendCommand(c, pl, plen)) {
            if (++tries > 1500) return false;   // queue stuck ~3s → link dead
            vTaskDelay(pdMS_TO_TICKS(2));
            esp_task_wdt_reset();
        }
        return true;
    };

    // 1. BEGIN
    SampleBeginPayload beginP = {};
    beginP.padIndex = (uint8_t)padIndex;
    beginP.bitsPerSample = 16;
    beginP.sampleRate = SAMPLE_RATE;
    beginP.totalBytes = totalBytes;
    beginP.totalSamples = numSamples;

    if (!enqueue(CMD_SAMPLE_BEGIN, &beginP, sizeof(beginP))) return false;
    delayMicroseconds(200);  // Give STM32 time to allocate

    // 2. DATA chunks (max 512 bytes = 256 samples per chunk)
    const uint16_t CHUNK_BYTES = 512;
    uint32_t offset = 0;
    uint32_t chunkCount = 0;

    // Build data packet: SampleDataHeader + raw audio data
    uint8_t dataPkt[8 + CHUNK_BYTES];

    while (offset < totalBytes) {
        uint16_t chunkSize = (uint16_t)min((uint32_t)CHUNK_BYTES, totalBytes - offset);

        SampleDataHeader* hdr = (SampleDataHeader*)dataPkt;
        hdr->padIndex = (uint8_t)padIndex;
        hdr->reserved = 0;
        hdr->chunkSize = chunkSize;
        hdr->offset = offset;

        memcpy(dataPkt + sizeof(SampleDataHeader), ((uint8_t*)buffer) + offset, chunkSize);

        // Never drop a data chunk — that is what produced noise tails and
        // corruption/resets on larger samples.
        if (!enqueue(CMD_SAMPLE_DATA, dataPkt, sizeof(SampleDataHeader) + chunkSize)) {
            // Link stalled mid-transfer: tell the Daisy to discard the partial
            // load (best effort) so the slot doesn't stay stuck "loading".
            SampleEndPayload abortP = {};
            abortP.padIndex = (uint8_t)padIndex;
            abortP.status = 1;
            sendCommand(CMD_SAMPLE_END, &abortP, sizeof(abortP));
            return false;
        }

        offset += chunkSize;
        chunkCount++;

        // Yield periodically: keeps the cmd queue small (so Core1's drain bursts
        // stay short) and gives this task + the watchdog room during the long
        // transfer of a large sample at 1MHz. ~1s added over a 2MB sample.
        if ((chunkCount & 7) == 0) {
            esp_task_wdt_reset();
            vTaskDelay(1);
        }
    }

    // 3. END
    SampleEndPayload endP = {};
    endP.padIndex = (uint8_t)padIndex;
    endP.status = 0;
    endP.checksum = crc16((uint8_t*)buffer, totalBytes > 65535 ? 65535 : (uint16_t)totalBytes);

    enqueue(CMD_SAMPLE_END, &endP, sizeof(endP));

    // Da tiempo a la Daisy para finalizar el buffer tras CMD_SAMPLE_END.
    // Sin este delay, samples grandes (>32KB) producen ruido al disparar
    // inmediatamente porque la STM32 aún está procesando los últimos chunks.
    uint32_t waitMs = totalBytes < 32768 ? 60 : (totalBytes < 131072 ? 120 : 200);
    vTaskDelay(pdMS_TO_TICKS(waitMs));

    return true;
}

bool SPIMaster::beginSampleStream(int padIndex, uint32_t numSamples) {
    if (padIndex < 0 || padIndex >= MAX_PADS || numSamples == 0) return false;

    SampleBeginPayload beginP = {};
    beginP.padIndex = (uint8_t)padIndex;
    beginP.bitsPerSample = 16;
    beginP.sampleRate = SAMPLE_RATE;
    beginP.totalBytes = numSamples * sizeof(int16_t);
    beginP.totalSamples = numSamples;
    return sendCommand(CMD_SAMPLE_BEGIN, &beginP, sizeof(beginP));
}

bool SPIMaster::writeSampleStreamData(int padIndex, const int16_t* samples, uint16_t numSamples, uint32_t startSample) {
    if (padIndex < 0 || padIndex >= MAX_PADS || !samples || numSamples == 0) return false;
    const uint16_t chunkBytes = numSamples * sizeof(int16_t);
    if (chunkBytes > 512) return false;

    uint8_t dataPkt[sizeof(SampleDataHeader) + 512];
    SampleDataHeader* hdr = (SampleDataHeader*)dataPkt;
    hdr->padIndex = (uint8_t)padIndex;
    hdr->reserved = 0;
    hdr->chunkSize = chunkBytes;
    hdr->offset = startSample * sizeof(int16_t);
    memcpy(dataPkt + sizeof(SampleDataHeader), samples, chunkBytes);
    return sendCommand(CMD_SAMPLE_DATA, dataPkt, sizeof(SampleDataHeader) + chunkBytes);
}

bool SPIMaster::endSampleStream(int padIndex, bool ok, uint32_t totalSamples) {
    if (padIndex < 0 || padIndex >= MAX_PADS) return false;

    SampleEndPayload endP = {};
    endP.padIndex = (uint8_t)padIndex;
    endP.status = ok ? 0 : 1;
    endP.checksum = 0;
    bool sent = sendCommand(CMD_SAMPLE_END, &endP, sizeof(endP));
    uint32_t totalBytes = totalSamples * sizeof(int16_t);
    uint32_t waitMs = totalBytes < 32768 ? 60 : (totalBytes < 131072 ? 120 : 200);
    vTaskDelay(pdMS_TO_TICKS(waitMs));
    return sent;
}

void SPIMaster::unloadSample(int padIndex) {
    SampleUnloadPayload p = {(uint8_t)padIndex};
    sendCommand(CMD_SAMPLE_UNLOAD, &p, sizeof(p));
}

void SPIMaster::unloadAllSamples() {
    sendCommand(CMD_SAMPLE_UNLOAD_ALL, nullptr, 0);
}

// ═══════════════════════════════════════════════════════
// DAISY SD CARD FILE SYSTEM
// ═══════════════════════════════════════════════════════

bool SPIMaster::sdListFolders(SdFolderListResponse& out) {
    return sendAndReceive(CMD_SD_LIST_FOLDERS, nullptr, 0, &out, sizeof(out));
}

bool SPIMaster::sdListFiles(const char* folder, SdFileListResponse& out) {
    SdListFilesPayload p = {};
    strncpy(p.folderName, folder, sizeof(p.folderName) - 1);
    return sendAndReceive(CMD_SD_LIST_FILES, &p, sizeof(p), &out, sizeof(out));
}

bool SPIMaster::sdGetFileInfo(const char* folder, const char* file, SdFileInfoResponse& out) {
    SdFileInfoPayload p = {};
    p.padIndex = 0;
    strncpy(p.folderName, folder, sizeof(p.folderName) - 1);
    strncpy(p.fileName, file, sizeof(p.fileName) - 1);
    return sendAndReceive(CMD_SD_FILE_INFO, &p, sizeof(p), &out, sizeof(out));
}

bool SPIMaster::sdLoadSample(int padIndex, const char* folder, const char* file) {
    SdLoadSamplePayload p = {};
    strncpy(p.folderName, folder, sizeof(p.folderName) - 1);
    strncpy(p.fileName, file, sizeof(p.fileName) - 1);
    p.padIndex = (uint8_t)padIndex;
    return sendCommand(CMD_SD_LOAD_SAMPLE, &p, sizeof(p));
}

bool SPIMaster::sdLoadKit(const char* kitName, uint8_t startPad, uint8_t maxPads) {
    SdLoadKitPayload p = {};
    strncpy(p.kitName, kitName, sizeof(p.kitName) - 1);
    p.startPad = startPad;
    p.maxPads = maxPads;
    return sendCommand(CMD_SD_LOAD_KIT, &p, sizeof(p));
}

bool SPIMaster::sdGetKitList(SdKitListResponse& out) {
    return sendAndReceive(CMD_SD_KIT_LIST, nullptr, 0, &out, sizeof(out));
}

bool SPIMaster::sdGetStatus(SdStatusResponse& out) {
    return sendAndReceive(CMD_SD_STATUS, nullptr, 0, &out, sizeof(out));
}

void SPIMaster::sdUnloadKit() {
    sendCommand(CMD_SD_UNLOAD_KIT, nullptr, 0);
}

bool SPIMaster::sdGetLoadedKit(SdStatusResponse& out) {
    return sendAndReceive(CMD_SD_GET_LOADED, nullptr, 0, &out, sizeof(out));
}

void SPIMaster::sdAbortLoad() {
    sendCommand(CMD_SD_ABORT, nullptr, 0);
}

// ═══════════════════════════════════════════════════════
// STATUS / METERING
// ═══════════════════════════════════════════════════════

float SPIMaster::getTrackPeak(int track) {
    if (track < 0 || track >= MAX_AUDIO_TRACKS) return 0.0f;
    return cachedTrackPeaks[track];
}

float SPIMaster::getMasterPeak() {
    return cachedMasterPeak;
}

void SPIMaster::getTrackPeaks(float* outPeaks, int count) {
    int n = min(count, (int)MAX_AUDIO_TRACKS);
    memcpy(outPeaks, cachedTrackPeaks, n * sizeof(float));
}

bool SPIMaster::requestPeaks() {
    PeaksResponse resp;
    if (sendAndReceive(CMD_GET_PEAKS, nullptr, 0, &resp, sizeof(resp))) {
        memcpy(cachedTrackPeaks, resp.trackPeaks, sizeof(cachedTrackPeaks));
        cachedMasterPeak = resp.masterPeak;
        /* Si la comunicación funciona, confirmar conexión aunque el boot ping fallara */
        if (!stm32Connected) stm32Connected = true;
        return true;
    }
    return false;
}

int SPIMaster::getActiveVoices() {
    return (int)cachedStatus.activeVoices;
}

float SPIMaster::getCpuLoad() {
    return (float)cachedStatus.cpuLoadPercent;
}

bool SPIMaster::ping(uint32_t& roundtripUs) {
    PingPayload pingP = {(uint32_t)micros()};
    PongResponse pong;
    
    uint32_t start = micros();
    if (sendAndReceive(CMD_PING, &pingP, sizeof(pingP), &pong, sizeof(pong))) {
        roundtripUs = micros() - start;
        if (!stm32Connected) stm32Connected = true;  // auto-reconnect si el boot ping falló
        return true;
    }
    return false;
}

void SPIMaster::resetDSP() {
    sendCommand(CMD_RESET, nullptr, 0);
    
    // Reset cached state
    for (int i = 0; i < MAX_AUDIO_TRACKS; i++) {
        cachedTrackFilter[i] = FILTER_NONE;
        trackFilterActive[i] = false;
        cachedTrackEchoActive[i] = false;
        cachedTrackFlangerActive[i] = false;
        cachedTrackCompActive[i] = false;
        cachedTrackPeaks[i] = 0.0f;
    }
    for (int i = 0; i < MAX_PADS; i++) {
        cachedPadFilter[i] = FILTER_NONE;
        padFilterActive[i] = false;
        cachedPadLoop[i] = false;
    }
    cachedMasterPeak = 0.0f;
    cachedReverbActive = false;
    cachedChorusActive = false;
    cachedTremoloActive = false;
    cachedWaveFolderGain = 1.0f;
    cachedLimiterActive = false;
    memset(&cachedStatus, 0, sizeof(cachedStatus));
    
}

// ═══════════════════════════════════════════════════════
// MASTER FX — REVERB
// ═══════════════════════════════════════════════════════

void SPIMaster::setReverbActive(bool active) {
    cachedReverbActive = active;
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_REVERB_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setReverbFeedback(float feedback) {
    cachedReverbFeedback = constrain(feedback, 0.0f, 0.99f);
    FloatPayload p = {cachedReverbFeedback};
    sendCommand(CMD_REVERB_FEEDBACK, &p, sizeof(p));
}

void SPIMaster::setReverbLpFreq(float hz) {
    cachedReverbLpFreq = constrain(hz, 200.0f, 12000.0f);
    FloatPayload p = {cachedReverbLpFreq};
    sendCommand(CMD_REVERB_LPFREQ, &p, sizeof(p));
}

void SPIMaster::setReverbMix(float mix) {
    cachedReverbMix = constrain(mix, 0.0f, 1.0f);
    FloatPayload p = {cachedReverbMix};
    sendCommand(CMD_REVERB_MIX, &p, sizeof(p));
}

void SPIMaster::setReverb(bool active, float feedback, float lpFreq, float mix) {
    cachedReverbActive   = active;
    cachedReverbFeedback = constrain(feedback, 0.0f, 0.99f);
    cachedReverbLpFreq   = constrain(lpFreq, 200.0f, 12000.0f);
    cachedReverbMix      = constrain(mix, 0.0f, 1.0f);
    // Send full payload in one command
    ReverbPayload p = {};
    p.active   = active ? 1 : 0;
    p.feedback = cachedReverbFeedback;
    p.lpFreq   = cachedReverbLpFreq;
    p.mix      = cachedReverbMix;
    sendCommand(CMD_REVERB_ACTIVE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER FX — CHORUS
// ═══════════════════════════════════════════════════════

void SPIMaster::setChorusActive(bool active) {
    cachedChorusActive = active;
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_CHORUS_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setChorusRate(float hz) {
    cachedChorusRate = constrain(hz, 0.1f, 10.0f);
    FloatPayload p = {cachedChorusRate};
    sendCommand(CMD_CHORUS_RATE, &p, sizeof(p));
}

void SPIMaster::setChorusDepth(float depth) {
    cachedChorusDepth = constrain(depth, 0.0f, 1.0f);
    FloatPayload p = {cachedChorusDepth};
    sendCommand(CMD_CHORUS_DEPTH, &p, sizeof(p));
}

void SPIMaster::setChorusMix(float mix) {
    cachedChorusMix = constrain(mix, 0.0f, 1.0f);
    FloatPayload p = {cachedChorusMix};
    sendCommand(CMD_CHORUS_MIX, &p, sizeof(p));
}

void SPIMaster::setChorus(bool active, float rate, float depth, float mix) {
    cachedChorusActive = active;
    cachedChorusRate   = constrain(rate,  0.1f, 10.0f);
    cachedChorusDepth  = constrain(depth, 0.0f, 1.0f);
    cachedChorusMix    = constrain(mix,   0.0f, 1.0f);
    ChorusPayload p = {};
    p.active = active ? 1 : 0;
    p.rate   = cachedChorusRate;
    p.depth  = cachedChorusDepth;
    p.mix    = cachedChorusMix;
    sendCommand(CMD_CHORUS_ACTIVE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER FX — TREMOLO
// ═══════════════════════════════════════════════════════

void SPIMaster::setTremoloActive(bool active) {
    cachedTremoloActive = active;
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_TREMOLO_ACTIVE, &p, sizeof(p));
}

void SPIMaster::setTremoloRate(float hz) {
    cachedTremoloRate = constrain(hz, 0.1f, 20.0f);
    FloatPayload p = {cachedTremoloRate};
    sendCommand(CMD_TREMOLO_RATE, &p, sizeof(p));
}

void SPIMaster::setTremoloDepth(float depth) {
    cachedTremoloDepth = constrain(depth, 0.0f, 1.0f);
    FloatPayload p = {cachedTremoloDepth};
    sendCommand(CMD_TREMOLO_DEPTH, &p, sizeof(p));
}

void SPIMaster::setTremolo(bool active, float rate, float depth) {
    cachedTremoloActive = active;
    cachedTremoloRate   = constrain(rate,  0.1f, 20.0f);
    cachedTremoloDepth  = constrain(depth, 0.0f, 1.0f);
    TremoloPayload p = {};
    p.active = active ? 1 : 0;
    p.rate   = cachedTremoloRate;
    p.depth  = cachedTremoloDepth;
    sendCommand(CMD_TREMOLO_ACTIVE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER FX — WAVEFOLDER + LIMITER
// ═══════════════════════════════════════════════════════

void SPIMaster::setWaveFolderGain(float gain) {
    cachedWaveFolderGain = constrain(gain, 1.0f, 10.0f);
    FloatPayload p = {cachedWaveFolderGain};
    sendCommand(CMD_WAVEFOLDER_GAIN, &p, sizeof(p));
}

void SPIMaster::setLimiterActive(bool active) {
    cachedLimiterActive = active;
    BoolPayload p = {(uint8_t)(active ? 1 : 0)};
    sendCommand(CMD_LIMITER_ACTIVE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// MASTER FX ROUTING
// ═══════════════════════════════════════════════════════

void SPIMaster::setMasterFxRoute(uint8_t fxId, bool connected) {
    MasterFxRoutePayload p = { fxId, (uint8_t)(connected ? 1 : 0) };
    sendCommand(CMD_MASTER_FX_ROUTE, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// NEW MASTER FX — Auto-Wah, Stereo Width, Tape Stop,
//   Beat Repeat, Delay Stereo, Chorus Stereo, Early Ref
// ═══════════════════════════════════════════════════════

void SPIMaster::setAutoWahActive(bool active) {
    cachedAutoWahActive = active;
    uint8_t v = active ? 1 : 0;
    sendCommand(CMD_AUTOWAH_ACTIVE, &v, 1);
}

void SPIMaster::setAutoWahLevel(uint8_t level) {
    cachedAutoWahLevel = level;
    sendCommand(CMD_AUTOWAH_LEVEL, &level, 1);
}

void SPIMaster::setAutoWahMix(uint8_t mix) {
    cachedAutoWahMix = mix;
    sendCommand(CMD_AUTOWAH_MIX, &mix, 1);
}

void SPIMaster::setStereoWidth(uint8_t width) {
    cachedStereoWidth = width;
    sendCommand(CMD_STEREO_WIDTH, &width, 1);
}

void SPIMaster::setTapeStop(uint8_t mode) {
    sendCommand(CMD_TAPE_STOP, &mode, 1);
}

void SPIMaster::setBeatRepeat(uint8_t division) {
    sendCommand(CMD_BEAT_REPEAT, &division, 1);
}

void SPIMaster::setDelayStereo(uint8_t mode) {
    cachedDelayStereoMode = mode;
    sendCommand(CMD_DELAY_STEREO, &mode, 1);
}

void SPIMaster::setChorusStereo(uint8_t mode) {
    cachedChorusStereoMode = mode;
    sendCommand(CMD_CHORUS_STEREO, &mode, 1);
}

void SPIMaster::setEarlyRefActive(bool active) {
    cachedEarlyRefActive = active;
    uint8_t v = active ? 1 : 0;
    sendCommand(CMD_EARLY_REF_ACTIVE, &v, 1);
}

void SPIMaster::setEarlyRefMix(uint8_t mix) {
    cachedEarlyRefMix = mix;
    sendCommand(CMD_EARLY_REF_MIX, &mix, 1);
}

// ═══════════════════════════════════════════════════════
// CHOKE GROUPS
// ═══════════════════════════════════════════════════════

void SPIMaster::setChokeGroup(uint8_t pad, uint8_t group) {
    if (pad < MAX_AUDIO_TRACKS) cachedChokeGroup[pad] = group;
    ChokeGroupPayload p = { pad, group };
    sendCommand(CMD_CHOKE_GROUP, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// SONG MODE (chain upload + control)
// ═══════════════════════════════════════════════════════

bool SPIMaster::songUpload(const SongEntry* entries, uint8_t count) {
    if (!entries || count == 0 || count > SONG_MAX_ENTRIES) return false;
    uint8_t buf[1 + 2 * SONG_MAX_ENTRIES];
    buf[0] = count;
    memcpy(buf + 1, entries, count * sizeof(SongEntry));
    return sendCommand(CMD_SONG_UPLOAD, buf, 1 + count * 2);
}

bool SPIMaster::songControl(uint8_t action) {
    return sendCommand(CMD_SONG_CONTROL, &action, 1);
}

bool SPIMaster::songGetPos(uint8_t& outIdx, uint8_t& outPattern, uint8_t& outRepeat) {
    SongPosResponse resp = {};
    if (sendAndReceive(CMD_SONG_GET_POS, nullptr, 0, &resp, sizeof(resp))) {
        outIdx = resp.songIdx;
        outPattern = resp.currentPattern;
        outRepeat = resp.songRepeatCnt;
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════
// PER-TRACK LFO CONFIG (sent to Daisy)
// ═══════════════════════════════════════════════════════

void SPIMaster::setTrackLfoConfig(uint8_t track, uint8_t wave, uint8_t target,
                                   uint16_t rateCentiHz, uint16_t depthMilli) {
    TrackLfoConfigPayload p;
    p.track   = track;
    p.wave    = wave;
    p.target  = target;
    p.rateHi  = (uint8_t)(rateCentiHz >> 8);
    p.rateLo  = (uint8_t)(rateCentiHz & 0xFF);
    p.depthHi = (uint8_t)(depthMilli >> 8);
    p.depthLo = (uint8_t)(depthMilli & 0xFF);
    sendCommand(CMD_TRACK_LFO_CONFIG, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// STATUS QUERIES — SPI round-trips
// ═══════════════════════════════════════════════════════

bool SPIMaster::requestActiveVoices() {
    if (!stm32Connected) return false;
    VoicesResponse resp = {};
    if (sendAndReceive(CMD_GET_VOICES, nullptr, 0, &resp, sizeof(resp))) {
        cachedStatus.activeVoices = resp.activeVoices;
        return true;
    }
    return false;
}

bool SPIMaster::requestCpuLoad() {
    if (!stm32Connected) return false;
    CpuLoadResponse resp = {};
    if (sendAndReceive(CMD_GET_CPU_LOAD, nullptr, 0, &resp, sizeof(resp))) {
        cachedStatus.cpuLoadPercent = (uint8_t)constrain((int)resp.cpuLoad, 0, 100);
        cachedStatus.uptime = resp.uptime / 1000;  // ms → s
        cachedStatus.cpuAvgPercent = (uint8_t)constrain((int)resp.cpuAvg, 0, 100);
        cachedStatus.cpuPeakPercent = (uint8_t)constrain((int)resp.cpuPeak, 0, 100);
        cachedStatus.activeVoices = resp.activeVoices;
        cachedStatus.perfStressMode = resp.perfStressMode;
        cachedStatus.spiErrCnt = resp.spiErrCnt;
        cachedStatus.spiRingDrops = resp.spiRingDrops;
        return true;
    }
    return false;
}

bool SPIMaster::setPerformanceStressMode(bool enabled, bool resetMetrics) {
    if (!stm32Connected) return false;
    uint8_t mode = resetMetrics ? 2 : (enabled ? 1 : 0);
    uint8_t resp[4] = {};
    bool ok = sendAndReceive(CMD_DIAG_PERF_STRESS, &mode, 1, resp, sizeof(resp));
    if (ok) {
        cachedStatus.perfStressMode = resp[0];
        cachedStatus.cpuLoadPercent = resp[1];
        cachedStatus.cpuAvgPercent = resp[1];
        cachedStatus.cpuPeakPercent = resp[2];
        cachedStatus.activeVoices = resp[3];
    }
    return ok;
}

bool SPIMaster::requestStatus() {
    if (!stm32Connected) return false;
    StatusResponse resp = {};
    if (sendAndReceive(CMD_GET_STATUS, nullptr, 0, &resp, sizeof(resp))) {
        cachedStatus = resp;
        return true;
    }
    return false;
}

bool SPIMaster::beginCleanTrackStream(int trackIndex, uint32_t numSamples) {
    uint8_t slot = 0;
    // Do NOT gate on stm32Connected: it only turns true when the Daisy answers a
    // ping/peaks (its SPI *response* path). Sample/clean-track loads are one-way
    // commands that work even if responses are flaky — the pad loader
    // (transferSample) never checks it and loads fine. Gating here is exactly why
    // stems failed with "daisy-begin-failed" while pads loaded OK.
    if (!cleanTrackToSampleSlot(trackIndex, slot)) return false;
    SampleBeginPayload payload = {};
    payload.padIndex = slot;
    payload.bitsPerSample = 16;
    payload.sampleRate = 48000;
    payload.totalBytes = numSamples * sizeof(int16_t);
    payload.totalSamples = numSamples;
    return sendCommand(CMD_SAMPLE_BEGIN, &payload, sizeof(payload));
}

bool SPIMaster::writeCleanTrackStreamData(int trackIndex, const int16_t* samples, uint16_t numSamples, uint32_t startSample) {
    uint8_t slot = 0;
    if (!cleanTrackToSampleSlot(trackIndex, slot) || !samples || numSamples == 0) return false;
    if (numSamples > 256) numSamples = 256;
    const uint16_t chunkSize = (uint16_t)(numSamples * sizeof(int16_t));
    const uint16_t totalSize = (uint16_t)(sizeof(SampleDataHeader) + chunkSize);
    if (totalSize > sizeof(txBuffer)) return false;
    SampleDataHeader header = {};
    header.padIndex = slot;
    header.chunkSize = chunkSize;
    header.offset = startSample * sizeof(int16_t);
    memcpy(txBuffer, &header, sizeof(header));
    memcpy(txBuffer + sizeof(header), samples, chunkSize);
    return sendCommand(CMD_SAMPLE_DATA, txBuffer, totalSize);
}

bool SPIMaster::endCleanTrackStream(int trackIndex, bool ok, uint32_t totalSamples) {
    uint8_t slot = 0;
    if (!cleanTrackToSampleSlot(trackIndex, slot)) return false;
    SampleEndPayload payload = {};
    payload.padIndex = slot;
    payload.status = ok ? 0 : 1;
    payload.checksum = totalSamples;
    return sendCommand(CMD_SAMPLE_END, &payload, sizeof(payload));
}

bool SPIMaster::setCleanTrackActive(int trackIndex, bool active) {
    if (trackIndex < 0 || trackIndex >= CLEAN_TRACK_COUNT) return false;
    CleanTrackControlPayload payload = { (uint8_t)trackIndex, (uint8_t)(active ? 1 : 0) };
    return sendCommand(CMD_CLEAN_TRACK_ACTIVE, &payload, sizeof(payload));
}

bool SPIMaster::setCleanTrackMute(int trackIndex, bool muted) {
    if (trackIndex < 0 || trackIndex >= CLEAN_TRACK_COUNT) return false;
    CleanTrackControlPayload payload = { (uint8_t)trackIndex, (uint8_t)(muted ? 1 : 0) };
    return sendCommand(CMD_CLEAN_TRACK_MUTE, &payload, sizeof(payload));
}

bool SPIMaster::getStatusSnapshot(StatusResponse& out) {
    out = cachedStatus;
    return stm32Connected;
}

bool SPIMaster::requestEvents(EventsResponse& out) {
    if (!stm32Connected) return false;
    memset(&out, 0, sizeof(out));
    return sendAndReceive(CMD_GET_EVENTS, nullptr, 0, &out, sizeof(out));
}

bool SPIMaster::drainEvents() {
    if (!stm32Connected) return false;
    int totalDrained = 0;
    // Probe unconditionally up to 4 rounds — do NOT rely on cachedStatus.evtCount
    // because in normal operation requestStatus() is never called and evtCount=0.
    for (int round = 0; round < 4; round++) {
        EventsResponse evtResp = {};
        if (!requestEvents(evtResp)) break;
        if (evtResp.count == 0) break;
        for (int i = 0; i < evtResp.count; i++) {
            const NotifyEvent& evt = evtResp.events[i];
            totalDrained++;
            if (eventCallback) {
                eventCallback(evt, eventUserData);
            }
        }
    }
    return totalDrained > 0;
}


// ═══════════════════════════════════════════════════════
// FILTER PRESETS (static, for UI)
// ═══════════════════════════════════════════════════════

const FilterPreset* SPIMaster::getFilterPreset(FilterType type) {
    // El enum FilterType tiene huecos (LADDER=10, SVF=11..13, COMB=14,
    // REVERSE=17, HALFSPEED=18, STUTTER=19) pero filterPresets[] es denso.
    // Indexar por el valor del enum leia fuera de limites (tipos 13-19) o
    // devolvia el preset equivocado (10-12). Buscamos por .type.
    const size_t n = sizeof(filterPresets) / sizeof(filterPresets[0]);
    for (size_t i = 0; i < n; i++) {
        if (filterPresets[i].type == type) {
            return &filterPresets[i];
        }
    }
    return &filterPresets[0];  // "None" como fallback seguro
}

const char* SPIMaster::getFilterName(FilterType type) {
    const FilterPreset* fp = getFilterPreset(type);
    return fp ? fp->name : "Unknown";
}

// ═══════════════════════════════════════════════════════
// SYNTH ENGINES (TR-808/909/505 percussive + TB-303 bass)
// ═══════════════════════════════════════════════════════

void SPIMaster::synthTrigger(uint8_t engine, uint8_t instrument, uint8_t velocity) {
    SynthTriggerPayload p;
    p.engine     = engine;
    p.instrument = instrument;
    p.velocity   = velocity;
    sendCommand(CMD_SYNTH_TRIGGER, &p, sizeof(p));
}

void SPIMaster::synthParam(uint8_t engine, uint8_t instrument, uint8_t paramId, float value) {
    SynthParamPayload p;
    p.engine     = engine;
    p.instrument = instrument;
    p.paramId    = paramId;
    p.reserved   = 0;
    p.value      = value;
    sendCommand(CMD_SYNTH_PARAM, &p, sizeof(p));
}

void SPIMaster::synth303NoteOn(uint8_t midiNote, bool accent, bool slide) {
    SynthNoteOnPayload p;
    p.midiNote = midiNote;
    p.accent   = accent ? 1 : 0;
    p.slide    = slide  ? 1 : 0;
    sendCommand(CMD_SYNTH_NOTE_ON, &p, sizeof(p));
}

void SPIMaster::synth303NoteOff() {
    sendCommand(CMD_SYNTH_NOTE_OFF, nullptr, 0);
}

void SPIMaster::synthNoteOff(uint8_t engine, uint8_t track, uint8_t note) {
    if (note != 0xFF) {
        uint8_t payload[3] = { engine, track, note };
        sendCommand(CMD_SYNTH_NOTE_OFF, payload, sizeof(payload));
        return;
    }
    uint8_t payload[2] = { engine, track };
    sendCommand(CMD_SYNTH_NOTE_OFF, payload, sizeof(payload));
}

void SPIMaster::synth303Param(uint8_t paramId, float value) {
    Synth303ParamPayload p;
    p.paramId    = paramId;
    p.reserved[0] = p.reserved[1] = p.reserved[2] = 0;
    p.value      = value;
    sendCommand(CMD_SYNTH_303_PARAM, &p, sizeof(p));
}

void SPIMaster::synthNoteOnEx(uint8_t engine, uint8_t midiNote, uint8_t velocity, bool accent, bool slide) {
    SynthNoteOnExPayload p;
    p.engine   = engine;
    p.midiNote = midiNote;
    p.velocity = velocity;
    p.accent   = accent ? 1 : 0;
    p.slide    = slide  ? 1 : 0;
    sendCommand(CMD_SYNTH_NOTE_ON_EX, &p, sizeof(p));
}

void SPIMaster::synthSetActive(uint8_t engineMask) {
    // Legacy 1-byte: store in low byte of 16-bit mask
    SynthActivePayload p;
    p.engineMask = engineMask;
    if (sendCommand(CMD_SYNTH_ACTIVE, &p, sizeof(p))) {
        cachedSynthActiveMask16 = (cachedSynthActiveMask16 & 0xFF00) | engineMask;
    }
}

void SPIMaster::synthSetActive16(uint16_t engineMask16) {
    SynthActivePayload16 p;
    p.maskLo = (uint8_t)(engineMask16 & 0xFF);
    p.maskHi = (uint8_t)((engineMask16 >> 8) & 0xFF);
    if (sendCommand(CMD_SYNTH_ACTIVE, &p, sizeof(p))) {
        cachedSynthActiveMask16 = engineMask16;
    }
}

void SPIMaster::synthPreset(uint8_t engine, uint8_t preset) {
    SynthPresetPayload p;
    p.engine = engine;
    p.preset = preset;
    sendCommand(CMD_SYNTH_PRESET, &p, sizeof(p));
}

// ═══════════════════════════════════════════════════════
// DAISY SEQUENCER (0xD0-0xD8)
// ═══════════════════════════════════════════════════════

bool SPIMaster::dsqUploadTrack(uint8_t pattern, uint8_t track,
                               const DsqStepPkt* steps, uint8_t stepCount)
{
    if (!steps || stepCount == 0 || stepCount > DSQ_MAX_STEPS) return false;

    // Build payload: 4-byte header + stepCount * 4-byte DsqStepPkt
    uint8_t buf[4 + DSQ_MAX_STEPS * sizeof(DsqStepPkt)];
    buf[0] = pattern % DSQ_PATTERNS;
    buf[1] = track & 15;
    buf[2] = stepCount;
    buf[3] = 0; // reserved
    memcpy(buf + 4, steps, stepCount * sizeof(DsqStepPkt));
    return sendCommand(CMD_DSQ_UPLOAD_TRACK, buf, 4 + stepCount * sizeof(DsqStepPkt));
}

bool SPIMaster::dsqSetStep(uint8_t pattern, uint8_t track, uint8_t step,
                           bool active, uint8_t velocity,
                           uint8_t noteLenDiv, uint8_t probability)
{
    DsqSetStepPayload p;
    p.pattern    = pattern % DSQ_PATTERNS;
    p.track      = track & 15;
    p.step       = step < DSQ_MAX_STEPS ? step : 0;
    p.active     = active ? 1 : 0;
    p.velocity   = velocity ? velocity : 100;
    p.noteLenDiv = noteLenDiv;
    p.probability = probability ? probability : 100;
    p.reserved   = 0;
    return sendCommand(CMD_DSQ_SET_STEP, &p, sizeof(p));
}

bool SPIMaster::dsqControl(uint8_t mode) {
    uint8_t buf[1] = { mode };
    return sendCommand(CMD_DSQ_CONTROL, buf, 1);
}

bool SPIMaster::dsqSelectPattern(uint8_t pattern) {
    uint8_t buf[1] = { (uint8_t)(pattern % DSQ_PATTERNS) };
    return sendCommand(CMD_DSQ_SELECT_PATTERN, buf, 1);
}

bool SPIMaster::dsqSetLength(uint8_t length) {
    if (length != 16 && length != 32 && length != 64) return false;
    uint8_t buf[1] = { length };
    return sendCommand(CMD_DSQ_SET_LENGTH, buf, 1);
}

bool SPIMaster::dsqSetMute(uint8_t track, bool muted) {
    uint8_t buf[2] = { (uint8_t)(track & 15), static_cast<uint8_t>(muted ? 1u : 0u) };
    return sendCommand(CMD_DSQ_SET_MUTE, buf, 2);
}

bool SPIMaster::dsqSetSwing(uint8_t amount) {
    uint8_t buf[1] = { static_cast<uint8_t>(amount > 100 ? 100u : amount) };
    return sendCommand(CMD_DSQ_SET_SWING, buf, 1);
}

bool SPIMaster::dsqSetHumanize(uint8_t timingMs, uint8_t velocityAmount) {
    uint8_t buf[2] = {
        static_cast<uint8_t>(timingMs > 40 ? 40u : timingMs),
        static_cast<uint8_t>(velocityAmount > 60 ? 60u : velocityAmount)
    };
    return sendCommand(CMD_DSQ_SET_HUMANIZE, buf, 2);
}

bool SPIMaster::dsqSetParamLock(uint8_t pattern, uint8_t track, uint8_t step,
                                bool cutoffEn, uint16_t cutoffHz,
                                bool reverbEn, uint8_t reverbSend,
                                bool volEn, uint8_t volume)
{
    DsqSetParamLockPayload p;
    p.pattern   = pattern % DSQ_PATTERNS;
    p.track     = track & 15;
    p.step      = step < DSQ_MAX_STEPS ? step : 0;
    p.cutoffEn  = cutoffEn ? 1 : 0;
    p.cutoffHi  = (uint8_t)(cutoffHz >> 8);
    p.cutoffLo  = (uint8_t)(cutoffHz & 0xFF);
    p.reverbEn  = reverbEn ? 1 : 0;
    p.reverbSend = reverbSend;
    p.volEn     = volEn ? 1 : 0;
    p.volume    = volume;
    p.reserved[0] = p.reserved[1] = 0;
    return sendCommand(CMD_DSQ_SET_PARAM_LOCK, &p, sizeof(p));
}

bool SPIMaster::dsqSetStepNotes(uint8_t pattern, uint8_t track, uint8_t step,
                                uint8_t flags, const uint8_t notes[4])
{
    if (!notes || step >= DSQ_MAX_STEPS) return false;
    DsqSetStepNotesPayload p{};
    p.pattern = pattern % DSQ_PATTERNS;
    p.track = track & 15;
    p.step = step;
    p.flags = flags & 0x03;
    memcpy(p.notes, notes, sizeof(p.notes));
    return sendCommand(CMD_DSQ_SET_STEP_NOTES, &p, sizeof(p));
}

bool SPIMaster::dsqGetPos(uint8_t& outStep, uint8_t& outPattern, bool& outPlaying)
{
    DsqPosResponse r = {};
    bool ok = sendAndReceive(CMD_DSQ_GET_POS, nullptr, 0, &r, sizeof(r));
    if (ok) {
        outStep    = r.currentStep;
        outPattern = r.currentPattern;
        outPlaying = r.playing != 0;
    }
    return ok;
}

bool SPIMaster::dsqSetTrackEngine(uint8_t track, int8_t engine)
{
    uint8_t buf[2] = { (uint8_t)(track & 15), (uint8_t)engine };
    return sendCommand(CMD_DSQ_SET_TRACK_ENGINE, buf, 2);
}
