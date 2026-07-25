/*
 * SPIMaster.h
 * RED808 SPI Master — Controls STM32 Audio Slave
 * Replaces AudioEngine for all audio DSP operations
 */

#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <Arduino.h>
#include "protocol.h"
#include <freertos/semphr.h>
#include <freertos/queue.h>

// SPI fire-and-forget queue — Core0 (WS/WiFi) → Core1 (SPI task) dispatch.
// Prevents the async WebSocket handler from blocking on SPI mutex.
// 544 = enough to hold a CleanTrack/Sample data chunk: 256 samples * int16_t = 512 +
// SampleDataHeader (~16) + slack. With queue depth 64 ⇒ ~35 KB RAM.
static constexpr uint16_t SPI_QUEUE_PAYLOAD_MAX = 544;
struct SpiQueuedCmd {
    uint8_t  cmd;
    uint16_t payloadLen;
    uint8_t  payload[SPI_QUEUE_PAYLOAD_MAX];
};

// Audio constants (shared with STM32)
#define SAMPLE_RATE 48000
#define MAX_VOICES 10

// ═══════════════════════════════════════════════════════
// SPI TRANSPORT PINS (ESP32-S3 → Daisy/STM32)
// ═══════════════════════════════════════════════════════
// Bus HSPI (SPI3) — separado del display ST7789 que usa FSPI (SPI2)
// Wiring master (ESP32) -> slave (Daisy SPI1) — pines oficiales Electrosmith:
//   GPIO7 = CS  → Daisy D7  (PG10, SPI1_NSS)  — lado derecho pin 8
//   GPIO4 = SCK → Daisy D8  (PG11, SPI1_SCK)  — lado derecho pin 9
//   GPIO6 = MISO← Daisy D9  (PB4,  SPI1_MISO) — lado derecho pin 10
//   GPIO5 = MOSI→ Daisy D10 (PB5,  SPI1_MOSI) — lado derecho pin 11
// SPI mode 0, MSB first. Primer salto conservador para acelerar uploads.

#define DAISY_SPI_CS               7
#define DAISY_SPI_SCK              4
#define DAISY_SPI_MOSI             5
#define DAISY_SPI_MISO             6
#define DAISY_SPI_CLOCK_HZ    1000000UL   /* 1MHz — la Daisy drena su RXFIFO (16B) por polling cada ~100us (TIM6 10kHz + inline en audio); el maximo sostenible es ~1.28MHz. A mas velocidad (p.ej. 4MHz) los paquetes >16B (upload de patron/sample) desbordan el FIFO y se pierden bytes: secuenciador corrupto y stems que no suben. NO subir sin acelerar tambien el drenado en la Daisy. */
#define DAISY_SPI_RESPONSE_GAP_US  2000   /* 2ms — gap entre TX y RX en sendAndReceive; suficiente para Daisy main loop */

// Audio constants (mirrored from old AudioEngine for compatibility)
static constexpr int MAX_AUDIO_TRACKS = 16;
static constexpr int MAX_PADS = 24;

// Filter types (keep compatible with old AudioEngine enums for WebInterface)
enum FilterType {
    FILTER_NONE = 0,
    FILTER_LOWPASS = 1,
    FILTER_HIGHPASS = 2,
    FILTER_BANDPASS = 3,
    FILTER_NOTCH = 4,
    FILTER_ALLPASS = 5,
    FILTER_PEAKING = 6,
    FILTER_LOWSHELF = 7,
    FILTER_HIGHSHELF = 8,
    FILTER_RESONANT = 9,
    FILTER_LADDER = 10,       // Moog Ladder 24dB/oct
    FILTER_SVF_LP = 11,       // State Variable LP
    FILTER_SVF_HP = 12,       // State Variable HP
    FILTER_SVF_BP = 13,       // State Variable BP
    FILTER_COMB = 14,         // Comb filter resonator
    // Legacy pad-FX pseudo-filter IDs
    FILTER_REVERSE = 17,
    FILTER_HALFSPEED = 18,
    FILTER_STUTTER = 19
};

// Distortion modes
enum DistortionMode {
    DIST_SOFT = 0,
    DIST_HARD = 1,
    DIST_TUBE = 2,
    DIST_FUZZ = 3
};

// Filter preset structure (for UI compatibility)
struct FilterPreset {
    FilterType type;
    float cutoff;
    float resonance;
    float gain;
    const char* name;
};

class SPIMaster {
public:
    SPIMaster();
    ~SPIMaster();
    
    // Initialization
    bool begin();
    
    // ══════════════════════════════════════════════════
    // TRIGGERS (replaces AudioEngine trigger methods)
    // ══════════════════════════════════════════════════
    void triggerSampleSequencer(int padIndex, uint8_t velocity, uint8_t trackVolume = 100, uint32_t maxSamples = 0);
    void triggerSampleLive(int padIndex, uint8_t velocity);
    void triggerSample(int padIndex, uint8_t velocity);  // Alias for triggerSampleLive
    void stopSample(int padIndex);
    void stopAll();
    void triggerSidechain(int sourceTrack);              // Manually fire sidechain ducking
    bool triggerBulk(const TriggerSeqPayload* triggers, uint8_t count); // Up to 16 simultaneous
    
    // ══════════════════════════════════════════════════
    // VOLUME CONTROL
    // ══════════════════════════════════════════════════
    void setMasterVolume(uint8_t volume);
    uint8_t getMasterVolume();
    void setSequencerVolume(uint8_t volume);
    uint8_t getSequencerVolume();
    void setLiveVolume(uint8_t volume);
    uint8_t getLiveVolume();
    void setTrackVolume(int track, uint8_t volume);
    void setLivePitchShift(float pitch);
    float getLivePitchShift();
    void setTempo(float bpm);
    
    // ══════════════════════════════════════════════════
    // GLOBAL FILTER (legacy FX chain)
    // ══════════════════════════════════════════════════
    void setFilterType(FilterType type);
    void setFilterCutoff(float cutoff);
    void setFilterResonance(float resonance);
    void setBitDepth(uint8_t bits);
    void setDistortion(float amount);
    void setDistortionMode(DistortionMode mode);
    void setSampleRateReduction(uint32_t rate);
    
    // ══════════════════════════════════════════════════
    // MASTER EFFECTS
    // ══════════════════════════════════════════════════
    // Delay/Echo
    void setDelayActive(bool active);
    void setDelayTime(float ms);
    void setDelayFeedback(float feedback);
    void setDelayMix(float mix);
    
    // Phaser
    void setPhaserActive(bool active);
    void setPhaserRate(float hz);
    void setPhaserDepth(float depth);
    void setPhaserFeedback(float feedback);
    
    // Flanger
    void setFlangerActive(bool active);
    void setFlangerRate(float hz);
    void setFlangerDepth(float depth);
    void setFlangerFeedback(float feedback);
    void setFlangerMix(float mix);
    
    // Compressor/Limiter
    void setCompressorActive(bool active);
    void setCompressorThreshold(float threshold);
    void setCompressorRatio(float ratio);
    void setCompressorAttack(float ms);
    void setCompressorRelease(float ms);
    void setCompressorMakeupGain(float db);
    
    // ══════════════════════════════════════════════════
    // PER-TRACK FILTER
    // ══════════════════════════════════════════════════
    bool setTrackFilter(int track, FilterType type, float cutoff = 1000.0f, float resonance = 1.0f, float gain = 0.0f);
    void clearTrackFilter(int track);
    FilterType getTrackFilter(int track);
    int getActiveTrackFiltersCount();
    
    // ══════════════════════════════════════════════════
    // PER-PAD FILTER
    // ══════════════════════════════════════════════════
    bool setPadFilter(int pad, FilterType type, float cutoff = 1000.0f, float resonance = 1.0f, float gain = 0.0f);
    void clearPadFilter(int pad);
    FilterType getPadFilter(int pad);
    int getActivePadFiltersCount();
    
    // ══════════════════════════════════════════════════
    // PER-PAD / PER-TRACK FX
    // ══════════════════════════════════════════════════
    void setPadDistortion(int pad, float amount, DistortionMode mode = DIST_SOFT);
    void setPadBitCrush(int pad, uint8_t bits);
    void clearPadFX(int pad);
    void setTrackDistortion(int track, float amount, DistortionMode mode = DIST_SOFT);
    void setTrackBitCrush(int track, uint8_t bits);
    void clearTrackFX(int track);
    
    // Per-track live FX (echo, flanger, compressor)
    void setTrackEcho(int track, bool active, float time = 100.0f, float feedback = 40.0f, float mix = 50.0f);
    void setTrackFlanger(int track, bool active, float rate = 50.0f, float depth = 50.0f, float feedback = 30.0f);
    void setTrackCompressor(int track, bool active, float threshold = -20.0f, float ratio = 4.0f);
    void clearTrackLiveFX(int track);
    bool getTrackEchoActive(int track) const;
    bool getTrackFlangerActive(int track) const;
    bool getTrackCompressorActive(int track) const;

    // Per-track FX send levels (to master bus FX)
    void setTrackReverbSend(int track, uint8_t level);   // 0-100
    void setTrackDelaySend(int track, uint8_t level);    // 0-100
    void setTrackChorusSend(int track, uint8_t level);   // 0-100

    // Per-track mixer controls
    void setTrackPan(int track, int8_t pan);             // -100..+100
    void setTrackMute(int track, bool mute);
    void setTrackSolo(int track, bool solo);
    uint8_t getTrackReverbSend(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackReverbSend[track] : 0; }
    uint8_t getTrackDelaySend(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackDelaySend[track] : 0; }
    uint8_t getTrackChorusSend(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackChorusSend[track] : 0; }
    int8_t getTrackPan(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackPan[track] : 0; }
    bool getTrackMute(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackMute[track] : false; }
    bool getTrackSolo(int track) const { return (track >= 0 && track < MAX_AUDIO_TRACKS) ? cachedTrackSolo[track] : false; }

    // Per-track extended FX
    void setTrackPhaser(int track, bool active, float rate = 1.0f, float depth = 50.0f, float feedback = 50.0f);
    void setTrackTremolo(int track, bool active, float rate = 4.0f, float depth = 50.0f, uint8_t wave = 0, uint8_t target = 0);
    void setTrackPitch(int track, int16_t cents);        // -1200..+1200
    void setTrackGate(int track, bool active, float thresholdDb = -40.0f, float attackMs = 1.0f, float releaseMs = 50.0f);
    void setTrackEqLow(int track, int8_t gainDb);        // -12..+12
    void setTrackEqMid(int track, int8_t gainDb);        // -12..+12
    void setTrackEqHigh(int track, int8_t gainDb);       // -12..+12
    void setTrackEq(int track, int8_t low, int8_t mid, int8_t high); // all-in-one
    
    // ══════════════════════════════════════════════════
    // MASTER FX — REVERB (DaisySP ReverbSc)
    // ══════════════════════════════════════════════════
    void setReverbActive(bool active);
    void setReverbFeedback(float feedback);   // 0.0-1.0 (room size / decay)
    void setReverbLpFreq(float hz);           // 200-12000 Hz (damp / color)
    void setReverbMix(float mix);             // 0.0-1.0 (dry/wet)
    void setReverb(bool active, float feedback = 0.85f, float lpFreq = 8000.0f, float mix = 0.3f); // all-in-one
    bool isReverbActive() const { return cachedReverbActive; }

    // ══════════════════════════════════════════════════
    // MASTER FX — CHORUS (DaisySP Chorus)
    // ══════════════════════════════════════════════════
    void setChorusActive(bool active);
    void setChorusRate(float hz);             // 0.1-10.0 Hz
    void setChorusDepth(float depth);         // 0.0-1.0
    void setChorusMix(float mix);             // 0.0-1.0
    void setChorus(bool active, float rate = 0.5f, float depth = 0.5f, float mix = 0.4f); // all-in-one
    bool isChorusActive() const { return cachedChorusActive; }

    // ══════════════════════════════════════════════════
    // MASTER FX — TREMOLO (DaisySP Tremolo)
    // ══════════════════════════════════════════════════
    void setTremoloActive(bool active);
    void setTremoloRate(float hz);            // 0.1-20.0 Hz
    void setTremoloDepth(float depth);        // 0.0-1.0
    void setTremolo(bool active, float rate = 4.0f, float depth = 0.7f); // all-in-one
    bool isTremoloActive() const { return cachedTremoloActive; }

    // ══════════════════════════════════════════════════
    // MASTER FX — WAVEFOLDER + LIMITER
    // ══════════════════════════════════════════════════
    void setWaveFolderGain(float gain);       // 1.0-10.0 (1.0=off, >1=fold)
    void setLimiterActive(bool active);       // Brick-wall 0dBFS limiter
    bool isLimiterActive() const { return cachedLimiterActive; }

    // MASTER FX — RAYDRONE
    // The complete 9-byte payload is sent atomically so Daisy never observes
    // a partially-updated parameter set.
    bool setRaydroneConfig(const RaydroneConfigPayload& config);
    bool updateRaydroneConfig(const RaydroneConfigPayload& patch,
                              uint8_t updateMask);
    RaydroneConfigPayload getRaydroneConfig() const;

    // ══════════════════════════════════════════════════
    // MASTER FX ROUTING
    // ══════════════════════════════════════════════════
    void setMasterFxRoute(uint8_t fxId, bool connected);

    // ══════════════════════════════════════════════════
    // NEW MASTER FX — Auto-Wah, Stereo Width, Tape Stop,
    //   Beat Repeat, Delay Stereo, Chorus Stereo, Early Ref
    // ══════════════════════════════════════════════════
    void setAutoWahActive(bool active);
    void setAutoWahLevel(uint8_t level);      // 0-127
    void setAutoWahMix(uint8_t mix);          // 0-100
    void setStereoWidth(uint8_t width);       // 0-200
    void setTapeStop(uint8_t mode);           // 0=off, 1=stop, 2=start
    void setBeatRepeat(uint8_t division);     // 0=off, 1,2,4,8,16,32
    void setDelayStereo(uint8_t mode);        // 0=mono, 1=ping-pong
    void setChorusStereo(uint8_t mode);       // 0=mono, 1=stereo
    void setEarlyRefActive(bool active);
    void setEarlyRefMix(uint8_t mix);         // 0-100

    // ══════════════════════════════════════════════════
    // CHOKE GROUPS
    // ══════════════════════════════════════════════════
    void setChokeGroup(uint8_t pad, uint8_t group); // pad=0-15, group=0-8
    uint8_t getChokeGroup(uint8_t pad) const { return pad < MAX_AUDIO_TRACKS ? cachedChokeGroup[pad] : 0; }

    // ══════════════════════════════════════════════════
    // SONG MODE (chain upload + control)
    // ══════════════════════════════════════════════════
    bool songUpload(const SongEntry* entries, uint8_t count);
    bool songControl(uint8_t action);         // 0=stop, 1=play, 2=reset
    bool songGetPos(uint8_t& outIdx, uint8_t& outPattern, uint8_t& outRepeat);

    // ══════════════════════════════════════════════════
    // PER-TRACK LFO CONFIG (sent to Daisy)
    // ══════════════════════════════════════════════════
    void setTrackLfoConfig(uint8_t track, uint8_t wave, uint8_t target,
                           uint16_t rateCentiHz, uint16_t depthMilli);

    // ══════════════════════════════════════════════════
    // SIDECHAIN
    // ══════════════════════════════════════════════════
    void setSidechain(bool active, int sourceTrack, uint16_t destinationMask,
                      float amount, float attackMs, float releaseMs, float knee);
    void clearSidechain();
    
    // ══════════════════════════════════════════════════
    // PAD CONTROL (loop, reverse, pitch, stutter)
    // ══════════════════════════════════════════════════
    void setPadLoop(int padIndex, bool enabled);
    bool isPadLooping(int padIndex);
    void setReverseSample(int padIndex, bool reverse);
    void setTrackPitchShift(int padIndex, float pitch);
    void setStutter(int padIndex, bool active, int intervalMs = 100);
    
    // ══════════════════════════════════════════════════
    // SAMPLE TRANSFER (ESP32 PSRAM → STM32)
    // ══════════════════════════════════════════════════
    bool setSampleBuffer(int padIndex, int16_t* buffer, uint32_t length);
    bool transferSample(int padIndex, int16_t* buffer, uint32_t numSamples);
    bool beginSampleStream(int padIndex, uint32_t numSamples);
    bool writeSampleStreamData(int padIndex, const int16_t* samples, uint16_t numSamples, uint32_t startSample);
    bool endSampleStream(int padIndex, bool ok, uint32_t totalSamples);
    bool beginCleanTrackStream(int trackIndex, uint32_t numSamples);
    bool writeCleanTrackStreamData(int trackIndex, const int16_t* samples, uint16_t numSamples, uint32_t startSample);
    bool endCleanTrackStream(int trackIndex, bool ok, uint32_t totalSamples);
    bool setCleanTrackActive(int trackIndex, bool active);
    bool setCleanTrackMute(int trackIndex, bool muted);
    void unloadSample(int padIndex);
    void unloadAllSamples();

    // ══════════════════════════════════════════════════
    // DAISY SD CARD FILE SYSTEM
    //   Read kit folders/files from Daisy's SD card.
    //   Daisy loads WAVs directly from SD → SDRAM.
    // ══════════════════════════════════════════════════
    bool sdListFolders(SdFolderListResponse& out);        // Get kit folder names
    bool sdListFiles(const char* folder, SdFileListResponse& out); // Get WAV files in folder
    bool sdGetFileInfo(const char* folder, const char* file, SdFileInfoResponse& out);
    bool sdLoadSample(int padIndex, const char* folder, const char* file); // Load WAV → pad slot
    bool sdLoadKit(const char* kitName, uint8_t startPad = 0, uint8_t maxPads = 16); // Load all WAVs in kit
    bool sdGetKitList(SdKitListResponse& out);            // Get available kit names
    bool sdGetStatus(SdStatusResponse& out);              // SD card health + loaded info
    void sdUnloadKit();                                    // Free all SDRAM pad slots
    bool sdGetLoadedKit(SdStatusResponse& out);           // What's currently loaded?
    void sdAbortLoad();                                    // Cancel ongoing load
    
    // ══════════════════════════════════════════════════
    // STATUS / METERING
    // ══════════════════════════════════════════════════
    float getTrackPeak(int track);
    float getMasterPeak();
    void getTrackPeaks(float* outPeaks, int count);
    bool requestPeaks();               // Request peaks from slave (updates cache)
    bool requestActiveVoices();        // Request active voice count from slave
    bool requestCpuLoad();             // Request CPU % from slave
    bool requestStatus();              // Request full StatusResponse from slave
    bool setPerformanceStressMode(bool enabled, bool resetMetrics = false);
    int getActiveVoices();             // Returns cached value
    float getCpuLoad();                // Returns cached value
    float getCpuPeak() const { return (float)cachedStatus.cpuPeakPercent; }
    bool isPerformanceStressMode() const { return cachedStatus.perfStressMode != 0; }
    bool getStatusSnapshot(StatusResponse& out); // Fills struct with latest cached status
    bool ping(uint32_t& roundtripUs);
    void resetDSP();

    // ══════════════════════════════════════════════════
    // EVENT SYSTEM (Daisy notifications)
    // ══════════════════════════════════════════════════
    bool requestEvents(EventsResponse& out);    // Poll CMD_GET_EVENTS (0xE4)
    bool drainEvents();                          // Auto-drain all pending events
    uint8_t getPendingEventCount() const { return cachedStatus.evtCount; }
    bool hasSdCard() const { return cachedStatus.sdPresent != 0; }
    const char* getCurrentKitName() const { return cachedStatus.currentKitName; }
    uint8_t getTotalPadsLoaded() const { return cachedStatus.totalPadsLoaded; }
    uint16_t getPadsLoadedMask() const { return cachedStatus.padsLoadedMask; }
    uint8_t getXtraPadsMask() const { return cachedStatus.xtraPadsMask; }

    // Event callback (optional — set to receive events as they arrive)
    typedef void (*EventCallback)(const NotifyEvent& event, void* userData);
    void setEventCallback(EventCallback cb, void* userData = nullptr) {
        eventCallback = cb;
        eventUserData = userData;
    }
    
    // ══════════════════════════════════════════════════
    // SYNTH ENGINES (Daisy onboard: TR-808/909/505 + TB-303)
    // ══════════════════════════════════════════════════
    void synthTrigger(uint8_t engine, uint8_t instrument, uint8_t velocity);
    void synthParam(uint8_t engine, uint8_t instrument, uint8_t paramId, float value);
    void synth303NoteOn(uint8_t midiNote, bool accent, bool slide);
    void synth303NoteOff();
    void synth303Param(uint8_t paramId, float value);
    void synthNoteOnEx(uint8_t engine, uint8_t midiNote, uint8_t velocity, bool accent = false, bool slide = false);
    void synthNoteOff(uint8_t engine, uint8_t track, uint8_t note = 0xFF);
    void synthSetActive(uint8_t engineMask);
    void synthSetActive16(uint16_t engineMask16); // 2-byte mask for 9 engines
    void synthPreset(uint8_t engine, uint8_t preset);
    uint16_t getSynthActiveMask16() const { return cachedSynthActiveMask16; }
    uint8_t getSynthActiveMask() const { return (uint8_t)(cachedSynthActiveMask16 & 0xFF); }

    // ══════════════════════════════════════════════════
    // DAISY SEQUENCER (sequencer runs on Daisy Seed)
    // ══════════════════════════════════════════════════
    // Upload all steps for one track/pattern (caller builds DsqStepPkt[] from Sequencer)
    bool dsqUploadTrack(uint8_t pattern, uint8_t track,
                        const DsqStepPkt* steps, uint8_t stepCount);
    // Update a single step
    bool dsqSetStep(uint8_t pattern, uint8_t track, uint8_t step,
                    bool active, uint8_t velocity, uint8_t noteLenDiv, uint8_t probability);
    // Playback control: 0=stop 1=play 2=reset
    bool dsqControl(uint8_t mode);
    // Select active pattern
    bool dsqSelectPattern(uint8_t pattern);
    // Set pattern length (16/32/64)
    bool dsqSetLength(uint8_t length);
    // Mute/unmute a track
    bool dsqSetMute(uint8_t track, bool muted);
    // Swing amount 0-100
    bool dsqSetSwing(uint8_t amount);
    // Humanize amount: timing ms + velocity percent
    bool dsqSetHumanize(uint8_t timingMs, uint8_t velocityAmount);
    // Per-step parameter lock
    bool dsqSetParamLock(uint8_t pattern, uint8_t track, uint8_t step,
                         bool cutoffEn, uint16_t cutoffHz,
                         bool reverbEn, uint8_t reverbSend,
                         bool volEn, uint8_t volume);
    // Store melodic notes and accent/slide flags in a resident Daisy step
    bool dsqSetStepNotes(uint8_t pattern, uint8_t track, uint8_t step,
                         uint8_t flags, const uint8_t notes[4]);
    // Query sequencer position (blocking round-trip)
    bool dsqGetPos(uint8_t& outStep, uint8_t& outPattern, bool& outPlaying);
    // Set engine for a track in Daisy sequencer (-1=sampler, 0=808, 1=909, 2=505, 3=303)
    bool dsqSetTrackEngine(uint8_t track, int8_t engine);

    // ══════════════════════════════════════════════════
    // FILTER PRESETS (static, for UI)
    // ══════════════════════════════════════════════════
    static const FilterPreset* getFilterPreset(FilterType type);
    static const char* getFilterName(FilterType type);
    
    // Connection status
    bool isConnected() const { return stm32Connected; }
    uint32_t getSPIErrors() const { return spiErrorCount; }
    float getLastPingMs() const { return lastPingRttMs; }
    bool getCachedSdStatus(SdStatusResponse& out) const { if (!cachedSdStatusValid) return false; out = cachedSdStatus; return true; }

    // ══════════════════════════════════════════════════
    // SPI LOG CALLBACK (WebSocket diagnostics)
    // ══════════════════════════════════════════════════
    // Called after each SPI transaction (except CMD_GET_PEAKS spam).
    // JSON: {"type":"spi_log","name":"PING","cmd":238,"seq":3,"len":4,"ok":true,"try":1,"ms":12345}
    typedef void (*SpiLogCallback)(const char* json);
    void setSpiLogCallback(SpiLogCallback cb) { spiLogCallback = cb; }
    
    // Process (called from task loop — handles IRQ, peak polling)
    void process();
    
private:
    uint16_t seqNumber;          // solo se muta bajo spiMutex
    uint32_t spiErrorCount;

    // Estado cacheado del filtro/FX master. setFilterType() envía el payload
    // COMPLETO (CMD_FILTER_SET, 20 bytes): mandarlo con ceros aplastaba
    // cutoff→20Hz y bitDepth→0 en la Daisy (silencio total al elegir un
    // filtro de OUT en el patchbay). Los setters individuales actualizan la
    // caché para que el próximo setFilterType conserve el estado real.
    float    cachedFilterCutoff   = 20000.0f;  // neutro: filtro abierto
    float    cachedFilterQ        = 0.707f;
    uint8_t  cachedBitDepth       = 16;        // 16 = bitcrush off
    uint8_t  cachedDistMode       = 0;
    float    cachedDistortion     = 0.0f;
    uint32_t cachedSrReduce       = 0;
    // volatile: escrito desde Core1 (ping/requestPeaks en process) y leido
    // desde Core0 via isConnected().
    volatile bool stm32Connected;
    float lastPingRttMs = -1.0f;
    
    // TX/RX buffers
    uint8_t txBuffer[SPI_MAX_PAYLOAD];
    uint8_t rxBuffer[SPI_MAX_PAYLOAD];
    
    // Cached state (so getters don't need SPI round-trip)
    uint8_t cachedMasterVolume;
    uint8_t cachedSeqVolume;
    uint8_t cachedLiveVolume;
    float cachedLivePitch;

    // New FX cached state
    bool  cachedReverbActive;
    float cachedReverbFeedback;
    float cachedReverbLpFreq;
    float cachedReverbMix;
    bool  cachedChorusActive;
    float cachedChorusRate;
    float cachedChorusDepth;
    float cachedChorusMix;
    bool  cachedTremoloActive;
    float cachedTremoloRate;
    float cachedTremoloDepth;
    float cachedWaveFolderGain;
    bool  cachedLimiterActive;
    RaydroneConfigPayload cachedRaydroneConfig;

    // New FX cached state
    bool    cachedAutoWahActive;
    uint8_t cachedAutoWahLevel;
    uint8_t cachedAutoWahMix;
    uint8_t cachedStereoWidth;
    bool    cachedEarlyRefActive;
    uint8_t cachedEarlyRefMix;
    uint8_t cachedDelayStereoMode;
    uint8_t cachedChorusStereoMode;
    uint8_t cachedChokeGroup[MAX_AUDIO_TRACKS]; // group per pad (0=none, 1-8)

    // Status cache (54 bytes V2)
    StatusResponse cachedStatus;

    // SD status cache (polled periodically on Core1)
    SdStatusResponse cachedSdStatus;
    bool cachedSdStatusValid = false;

    // Synth engine active mask — 16-bit for 9 engines
    uint16_t cachedSynthActiveMask16;
    
    // Per-track/pad cached filter state
    FilterType cachedTrackFilter[MAX_AUDIO_TRACKS];
    bool trackFilterActive[MAX_AUDIO_TRACKS];
    FilterType cachedPadFilter[MAX_PADS];
    bool padFilterActive[MAX_PADS];
    
    // Per-track live FX cached state
    bool cachedTrackEchoActive[MAX_AUDIO_TRACKS];
    bool cachedTrackFlangerActive[MAX_AUDIO_TRACKS];
    bool cachedTrackCompActive[MAX_AUDIO_TRACKS];

    // Per-track mixer cached state
    uint8_t cachedTrackReverbSend[MAX_AUDIO_TRACKS];
    uint8_t cachedTrackDelaySend[MAX_AUDIO_TRACKS];
    uint8_t cachedTrackChorusSend[MAX_AUDIO_TRACKS];
    int8_t  cachedTrackPan[MAX_AUDIO_TRACKS];
    bool    cachedTrackMute[MAX_AUDIO_TRACKS];
    bool    cachedTrackSolo[MAX_AUDIO_TRACKS];
    
    // Pad loop cached state
    bool cachedPadLoop[MAX_PADS];
    
    // Event callback
    EventCallback eventCallback;
    void* eventUserData;
    uint32_t lastStatusPoll;

    // Peak levels (updated by requestPeaks)
    float cachedTrackPeaks[MAX_AUDIO_TRACKS];
    float cachedMasterPeak;
    uint32_t lastPeakRequest;
    
    // SPI mutex for thread safety (Core0 triggers vs Core1 process)
    SemaphoreHandle_t spiMutex;
    // Serializes Raydrone read/merge/write across AsyncTCP, UDP and Core1.
    mutable SemaphoreHandle_t raydroneConfigMutex;
    // Core0 → Core1 fire-and-forget command queue (avoids blocking WS handler)
    QueueHandle_t     spiCmdQueue;

    // SPI log callback (diagnostics via WebSocket admin panel)
    SpiLogCallback spiLogCallback;

    // SPI low-level
    bool sendCommand(uint8_t cmd, const void* payload, uint16_t payloadLen);
    bool sendCommandDirect(uint8_t cmd, const void* payload, uint16_t payloadLen);
    bool replayRaydroneConfigDirect();
    void drainCmdQueue();   // Called from Core1 process() loop
    bool sendAndReceive(uint8_t cmd, const void* payload, uint16_t payloadLen,
                        void* response, uint16_t responseLen);
    uint16_t crc16(const uint8_t* data, uint16_t len);
};

#endif // SPI_MASTER_H
