/*
 * WebInterface.cpp
 * RED808 Web Interface con WebSockets
 */

#include "WebInterface.h"
#include "SPIMaster.h"
#include "Sequencer.h"
#include "SampleManager.h"
#include "SysLog.h"
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <memory>   // std::shared_ptr (snapshot de sample en /api/sampledata)

#ifndef ENABLE_PHYSICAL_BUTTONS
#define ENABLE_PHYSICAL_BUTTONS 1
#endif

// Timeout para clientes UDP (30 segundos sin actividad)
#define UDP_CLIENT_TIMEOUT 30000

// Hardening de estabilidad WS bajo estrés
static constexpr size_t kWsMaxTextBytes = 24576;
static constexpr size_t kWsMaxBinaryBytes = 256;
static constexpr unsigned long kFastMasterCmdMinMs = 8;
static constexpr unsigned long kFastTrackCmdMinMs = 8;
static constexpr unsigned long kFastPadCmdMinMs = 8;
static constexpr unsigned long kFastVolumeCmdMinMs = 8;
static constexpr size_t kUdpMaxPacketBytes = 4096;
static constexpr int kCleanTrackCount = 4;
static constexpr const char* kCleanTracksStatePath = "/clean_tracks.json";
static constexpr const char* kCleanTracksDir = "/cleantracks";

static uint16_t le16(const uint8_t* p);
static uint32_t le32(const uint8_t* p);
static int16_t wav_s24_to_s16(const uint8_t* p);

struct CleanTrackSlotState {
  bool occupied;
  bool loaded;
  bool armed;
  bool muted;
  bool playing;
  bool movable;
  bool loadFailed;   // verify a Daisy fallo: NO reintentar automaticamente
  uint32_t sizeBytes;
  char name[24];
  char clipName[64];
  char filePath[96];
  char status[16];
};

struct CleanTrackUploadState {
  bool active;
  bool error;
  int slot;
  size_t bytesWritten;
  char filename[64];
  char filePath[96];
  char errorMsg[96];
  File file;
};

static CleanTrackSlotState s_cleanTracks[kCleanTrackCount];
static CleanTrackUploadState s_cleanTrackUpload;
static char s_cleanTrackLastInitError[96] = {};

static void sendCleanTrackInitError(AsyncWebServerRequest* request, int status,
                                    const char* reason, const char* message) {
  strlcpy(s_cleanTrackLastInitError, reason ? reason : "unknown", sizeof(s_cleanTrackLastInitError));
  StaticJsonDocument<256> response;
  response["success"] = false;
  response["reason"]  = s_cleanTrackLastInitError;
  response["message"] = message ? message : "Clean track upload failed";
  String output;
  serializeJson(response, output);
  request->send(status, "application/json", output);
}

// ── Async clean-track streaming state (pumped from update(), Core 0) ──────────
// Mirrors the pumpDaisyUpload() pattern: HTTP handler saves the file, sets
// active=true, then returns immediately.  update() calls pumpCleanTrackStream()
// every 2 ms so the SPI transfer happens without blocking the async-TCP task.
struct CleanTrackStreamState {
  bool active       = false;  // set last after all other fields are ready
  bool begun        = false;  // header parsed + beginCleanTrackStream sent
  bool error        = false;
  int  slot         = -1;
  char filePath[96] = {};
  uint16_t channels = 0;
  uint16_t bits     = 0;
  uint32_t bytesRemaining = 0;
  uint32_t totalSamples   = 0;
  uint8_t  carry[8]       = {};
  size_t   carryLen       = 0;
  uint32_t samplesSent    = 0;
  uint32_t startedMs      = 0;
  uint32_t lastProgressMs = 0;
  char errorMsg[96]       = {};
  // Phase 3 state machine — zero-blocking finalisation
  uint8_t  finalizePhase  = 0;     // 0=streaming, 1=endSent, 2=verifyWait, 3=publish, 4=done
  uint32_t finalizeStartMs = 0;
  uint32_t lastVerifyReqMs = 0;
  uint8_t  verifyAttempts  = 0;
  bool     verified        = false;
};
static CleanTrackStreamState s_cleanTrackStream;
static File                  s_cleanTrackStreamFile;   // file handle kept open between pumps
static constexpr uint32_t kCleanTrackStreamTotalTimeoutMs = 240000;
static constexpr uint32_t kCleanTrackStreamNoProgressTimeoutMs = 30000;

// ── Pre-allocated broadcast buffer in PSRAM to avoid heap fragmentation ──
// broadcastSequencerState() was the #1 cause of heap fragmentation:
// DynamicJsonDocument(8192) + String serialization = ~13KB alloc/free every cycle.
// Using static PSRAM buffers eliminates this churn entirely.
static char* _stateBuf = nullptr;
static constexpr size_t kStateBufSize = 8192;  // serialized JSON fits in ~5-6KB

// Pre-allocated PSRAM buffer for pattern JSON (selectPattern/getPattern)
static char* _patternBuf = nullptr;
static constexpr size_t kPatternBufSize = 40960;  // pattern JSON ~18-32KB (with melody data)

// Serializa el acceso a _stateBuf/_patternBuf. broadcastSequencerState() se
// invoca desde la tarea AsyncTCP (processCommand) Y desde systemTask
// (update/handleUdp); sin esto dos tareas podian serializar JSON sobre el
// mismo buffer a la vez y emitir frames corruptos. Un solo mutex para ambos
// buffers: contencion baja y sin riesgo de orden de locks.
static SemaphoreHandle_t _jsonBufMutex = nullptr;
static inline bool jsonBufLock() {
  return _jsonBufMutex && xSemaphoreTake(_jsonBufMutex, pdMS_TO_TICKS(150)) == pdTRUE;
}
static inline void jsonBufUnlock() {
  if (_jsonBufMutex) xSemaphoreGive(_jsonBufMutex);
}

// Endpoint of the UDP packet currently being processed. WiFiUDP::remoteIP()
// can become unreliable after nested command handling / additional UDP writes,
// so commands that need a direct reply use this latched endpoint.
static IPAddress s_udpReplyIp(0, 0, 0, 0);
static uint16_t  s_udpReplyPort = 0;

// PSRAM allocator for ArduinoJson — documents allocated here never touch internal heap
struct PsramAllocator {
  void* allocate(size_t size) {
    return ps_malloc(size);
  }
  void deallocate(void* ptr) {
    free(ptr);
  }
  void* reallocate(void* ptr, size_t new_size) {
    return ps_realloc(ptr, new_size);
  }
};
using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;

// Persistent JSON document in PSRAM — reused for every state broadcast.
// Allocated once in begin(), never freed. clear() between uses.
static PsramJsonDocument* _stateDoc = nullptr;

extern SPIMaster spiMaster;
extern Sequencer sequencer;
extern WebInterface webInterface;

// WAV upload header scratch in PSRAM (8MB available). Large enough to skip past
// JUNK/bext/LIST/cue chunks some DAWs place before the `data` chunk; the
// streaming path only needs to locate the `data` header to begin PCM, after
// which it uses fileOffset to place bytes. The old 4096-byte inline buffer
// rejected such WAVs ("WAV header too large") while the xtra/file path (which
// seeks the whole file on LittleFS) accepted them.
static constexpr size_t kDaisyHeaderCap = 256 * 1024;
static uint8_t* daisyUploadHeaderBuf() {
  static uint8_t* buf = (uint8_t*)ps_malloc(kDaisyHeaderCap);
  return buf;
}

struct DaisyUploadStreamState {
  int pad = -1;
  bool active = false;
  bool begun = false;
  bool error = false;
  char errorMsg[96] = "";
  uint8_t* header = nullptr;   // -> daisyUploadHeaderBuf() (PSRAM, kDaisyHeaderCap)
  size_t headerLen = 0;
  uint32_t dataPos = 0;
  uint32_t dataSize = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  size_t fileOffset = 0;
  uint8_t carry[8] = {};
  size_t carryLen = 0;
  uint32_t samplesSent = 0;
  char filename[64] = "";
};

static DaisyUploadStreamState s_daisyUpload;

static void pumpDaisyUpload();
static void pumpCleanTrackStream();
static bool queueCleanTrackStreamSlot(int slot);
static void queueNextStoredCleanTrackStream();

static void clearCleanTrackSlot(CleanTrackSlotState& slot, int index) {
  memset(&slot, 0, sizeof(slot));
  slot.armed = true;
  slot.movable = true;
  snprintf(slot.name, sizeof(slot.name), "Stem %d", index + 1);
  strncpy(slot.status, "empty", sizeof(slot.status) - 1);
}

static void initCleanTrackState() {
  for (int i = 0; i < kCleanTrackCount; ++i) {
    clearCleanTrackSlot(s_cleanTracks[i], i);
  }
  memset(&s_cleanTrackUpload, 0, sizeof(s_cleanTrackUpload));
}

static String cleanTrackSafeName(const String& input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
      out += c;
    } else {
      out += '_';
    }
  }
  if (out.length() == 0) out = "clip.wav";
  return out;
}

static bool ensureCleanTracksDir() {
  if (LittleFS.exists(kCleanTracksDir)) return true;
  return LittleFS.mkdir(kCleanTracksDir);
}

static bool saveCleanTracksStateToFs() {
  DynamicJsonDocument doc(2048);
  doc["version"] = 1;
  JsonArray tracks = doc.createNestedArray("tracks");
  for (int i = 0; i < kCleanTrackCount; ++i) {
    JsonObject track = tracks.createNestedObject();
    track["id"] = i;
    track["occupied"] = s_cleanTracks[i].occupied;
    track["loaded"] = s_cleanTracks[i].loaded;
    track["armed"] = s_cleanTracks[i].armed;
    track["muted"] = s_cleanTracks[i].muted;
    track["movable"] = s_cleanTracks[i].movable;
    track["sizeBytes"] = s_cleanTracks[i].sizeBytes;
    track["name"] = s_cleanTracks[i].name;
    track["clipName"] = s_cleanTracks[i].clipName;
    track["filePath"] = s_cleanTracks[i].filePath;
    track["status"] = s_cleanTracks[i].status;
  }
  File file = LittleFS.open(kCleanTracksStatePath, "w");
  if (!file) return false;
  serializeJson(doc, file);
  file.close();
  return true;
}

static void loadCleanTracksStateFromFs() {
  initCleanTrackState();
  if (!LittleFS.exists(kCleanTracksStatePath)) return;
  File file = LittleFS.open(kCleanTracksStatePath, "r");
  if (!file) return;
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) return;
  JsonArray tracks = doc["tracks"].as<JsonArray>();
  if (tracks.isNull()) return;
  for (JsonObject track : tracks) {
    int id = track["id"] | -1;
    if (id < 0 || id >= kCleanTrackCount) continue;
    clearCleanTrackSlot(s_cleanTracks[id], id);
    s_cleanTracks[id].occupied = track["occupied"] | false;
    s_cleanTracks[id].loaded = track["loaded"] | false;
    s_cleanTracks[id].armed = track["armed"] | true;
    s_cleanTracks[id].muted = track["muted"] | false;
    s_cleanTracks[id].movable = track["movable"] | true;
    s_cleanTracks[id].sizeBytes = track["sizeBytes"] | 0;
    strlcpy(s_cleanTracks[id].name, track["name"] | "", sizeof(s_cleanTracks[id].name));
    strlcpy(s_cleanTracks[id].clipName, track["clipName"] | "", sizeof(s_cleanTracks[id].clipName));
    strlcpy(s_cleanTracks[id].filePath, track["filePath"] | "", sizeof(s_cleanTracks[id].filePath));
    strlcpy(s_cleanTracks[id].status, track["status"] | "", sizeof(s_cleanTracks[id].status));
    if (s_cleanTracks[id].occupied && s_cleanTracks[id].filePath[0] && !LittleFS.exists(s_cleanTracks[id].filePath)) {
      clearCleanTrackSlot(s_cleanTracks[id], id);
    }
    if (s_cleanTracks[id].status[0] == 0) {
      strlcpy(s_cleanTracks[id].status, s_cleanTracks[id].occupied ? "stored" : "empty", sizeof(s_cleanTracks[id].status));
    }
  }
}

static int findFirstFreeCleanTrackSlot() {
  for (int i = 0; i < kCleanTrackCount; ++i) {
    if (!s_cleanTracks[i].occupied) return i;
  }
  // No free slot: reclaim ALL slots left occupied by a FAILED load — delete
  // their orphaned files (freeing the 11MB FS) and clear them, so failed uploads
  // never permanently block new ones or eat storage. Good (loaded) stems kept.
  int reclaimed = -1;
  for (int i = 0; i < kCleanTrackCount; ++i) {
    if (s_cleanTracks[i].loadFailed) {
      if (s_cleanTracks[i].filePath[0]) LittleFS.remove(s_cleanTracks[i].filePath);
      clearCleanTrackSlot(s_cleanTracks[i], i);
      if (reclaimed < 0) reclaimed = i;
    }
  }
  return reclaimed;
}

static bool parseWavFileHeader(File& file, uint16_t& channels, uint16_t& bits, uint32_t& dataOffset, uint32_t& dataSize, char* err, size_t errLen) {
  uint8_t header[4096] = {};
  size_t headerLen = file.read(header, sizeof(header));
  if (headerLen < 44) {
    strlcpy(err, "WAV header too small", errLen);
    return false;
  }
  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    strlcpy(err, "Not a WAV file", errLen);
    return false;
  }

  uint32_t pos = 12;
  bool fmtFound = false;
  bool dataFound = false;
  while (pos + 8 <= headerLen) {
    uint32_t chunkSize = le32(header + pos + 4);
    if (memcmp(header + pos, "fmt ", 4) == 0) {
      if (chunkSize < 16 || pos + 24 > headerLen) {
        strlcpy(err, "Invalid fmt chunk", errLen);
        return false;
      }
      const uint8_t* fmt = header + pos + 8;
      uint16_t audioFormat = le16(fmt);
      channels = le16(fmt + 2);
      bits = le16(fmt + 14);
      if (audioFormat != 1 && audioFormat != 0xFFFE) {
        strlcpy(err, "Unsupported WAV format", errLen);
        return false;
      }
      fmtFound = true;
    } else if (memcmp(header + pos, "data", 4) == 0) {
      dataOffset = pos + 8;
      dataSize = chunkSize;
      dataFound = true;
      break;
    }
    pos += 8 + chunkSize + (chunkSize & 1);
  }

  if (!fmtFound || !dataFound) {
    strlcpy(err, "Missing WAV chunks", errLen);
    return false;
  }
  if ((bits != 16 && bits != 24) || channels < 1 || channels > 2) {
    strlcpy(err, "Need mono/stereo 16/24-bit WAV", errLen);
    return false;
  }
  if (!file.seek(dataOffset, SeekSet)) {
    strlcpy(err, "Seek to WAV data failed", errLen);
    return false;
  }
  return true;
}

static bool streamWavFileToCleanTrack(int slot, const char* path, char* err, size_t errLen) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    strlcpy(err, "Failed opening clean track file", errLen);
    return false;
  }

  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  if (!parseWavFileHeader(file, channels, bits, dataOffset, dataSize, err, errLen)) {
    file.close();
    return false;
  }

  const uint32_t frameBytes = (bits / 8) * channels;
  const uint32_t totalSamples = dataSize / frameBytes;
  if (totalSamples == 0) {
    strlcpy(err, "Empty WAV data", errLen);
    file.close();
    return false;
  }
  if (!spiMaster.beginCleanTrackStream(slot, totalSamples)) {
    strlcpy(err, "Daisy clean track begin failed", errLen);
    file.close();
    return false;
  }

  uint8_t inBuf[1024] = {};
  uint8_t carry[8] = {};
  size_t carryLen = 0;
  int16_t pcm[256] = {};
  uint32_t bytesRemaining = dataSize;
  uint32_t samplesSent = 0;

  while (bytesRemaining > 0) {
    esp_task_wdt_reset();
    size_t toRead = bytesRemaining > sizeof(inBuf) ? sizeof(inBuf) : bytesRemaining;
    size_t readNow = file.read(inBuf, toRead);
    if (readNow == 0) {
      strlcpy(err, "Unexpected WAV EOF", errLen);
      spiMaster.endCleanTrackStream(slot, false, samplesSent);
      file.close();
      return false;
    }
    bytesRemaining -= (uint32_t)readNow;

    uint8_t parseBuf[sizeof(carry) + sizeof(inBuf)] = {};
    memcpy(parseBuf, carry, carryLen);
    memcpy(parseBuf + carryLen, inBuf, readNow);
    size_t parseLen = carryLen + readNow;
    size_t fullBytes = parseLen - (parseLen % frameBytes);
    size_t offset = 0;
    while (offset + frameBytes <= fullBytes) {
      uint16_t pcmCount = 0;
      while (offset + frameBytes <= fullBytes && pcmCount < 256) {
        const uint8_t* frame = parseBuf + offset;
        if (bits == 16) {
          if (channels == 1) {
            pcm[pcmCount++] = (int16_t)le16(frame);
          } else {
            int16_t l = (int16_t)le16(frame);
            int16_t r = (int16_t)le16(frame + 2);
            pcm[pcmCount++] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
          }
        } else {
          if (channels == 1) {
            pcm[pcmCount++] = wav_s24_to_s16(frame);
          } else {
            int16_t l = wav_s24_to_s16(frame);
            int16_t r = wav_s24_to_s16(frame + 3);
            pcm[pcmCount++] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
          }
        }
        offset += frameBytes;
      }
      if (pcmCount > 0 && !spiMaster.writeCleanTrackStreamData(slot, pcm, pcmCount, samplesSent)) {
        strlcpy(err, "Daisy clean track write failed", errLen);
        spiMaster.endCleanTrackStream(slot, false, samplesSent);
        file.close();
        return false;
      }
      samplesSent += pcmCount;
      esp_task_wdt_reset();
      yield();
    }

    carryLen = parseLen - fullBytes;
    if (carryLen > 0) memcpy(carry, parseBuf + fullBytes, carryLen);
  }

  file.close();
  if (!spiMaster.endCleanTrackStream(slot, true, samplesSent)) {
    strlcpy(err, "Daisy clean track end failed", errLen);
    return false;
  }
  return true;
}

static void syncCleanTrackStateFromDaisy() {
  StatusResponse status = {};
  if (!spiMaster.getStatusSnapshot(status)) return;
  for (int track = 0; track < kCleanTrackCount; ++track) {
    const bool daisyLoaded = (status.cleanTrackLoadedMask & (1u << track)) != 0;
    const bool daisyPlaying = (status.cleanTrackPlayingMask & (1u << track)) != 0;
    s_cleanTracks[track].loaded = daisyLoaded;
    s_cleanTracks[track].playing = daisyPlaying;
    if (!s_cleanTracks[track].occupied) {
      strlcpy(s_cleanTracks[track].status, "empty", sizeof(s_cleanTracks[track].status));
    } else if (daisyPlaying) {
      strlcpy(s_cleanTracks[track].status, s_cleanTracks[track].muted ? "playing-muted" : "playing", sizeof(s_cleanTracks[track].status));
    } else if (daisyLoaded) {
      strlcpy(s_cleanTracks[track].status, s_cleanTracks[track].muted ? "loaded-muted" : "loaded", sizeof(s_cleanTracks[track].status));
    } else {
      strlcpy(s_cleanTracks[track].status, "stored", sizeof(s_cleanTracks[track].status));
    }
  }
}

static bool queueCleanTrackStreamSlot(int slot) {
  if (slot < 0 || slot >= kCleanTrackCount) return false;
  if (s_cleanTrackStream.active) return false;
  if (!s_cleanTracks[slot].occupied || s_cleanTracks[slot].filePath[0] == 0) return false;

  s_cleanTracks[slot].loaded = false;
  strlcpy(s_cleanTracks[slot].status, "streaming", sizeof(s_cleanTracks[slot].status));
  s_cleanTrackStream = CleanTrackStreamState{};
  s_cleanTrackStream.slot = slot;
  strlcpy(s_cleanTrackStream.filePath, s_cleanTracks[slot].filePath, sizeof(s_cleanTrackStream.filePath));
  s_cleanTrackStream.startedMs = millis();
  s_cleanTrackStream.lastProgressMs = s_cleanTrackStream.startedMs;
  s_cleanTrackStream.active = true;
  return true;
}

static void queueNextStoredCleanTrackStream() {
  if (s_cleanTrackStream.active) return;
  for (int slot = 0; slot < kCleanTrackCount; ++slot) {
    auto& s = s_cleanTracks[slot];
    // Saltar slots marcados loadFailed: si los re-encolaramos aqui entrariamos
    // en bucle infinito tras un verify fallido (la UI se quedaba en STREAMING
    // para siempre). El usuario debe re-subir o pulsar retry para limpiar el flag.
    if (s.occupied && !s.loaded && !s.loadFailed && s.filePath[0]) {
      if (queueCleanTrackStreamSlot(slot)) return;
    }
  }
}

static void loadPersistedCleanTracksToDaisy() {
  for (int slot = 0; slot < kCleanTrackCount; ++slot) {
    if (!s_cleanTracks[slot].occupied || s_cleanTracks[slot].filePath[0] == 0) continue;
    s_cleanTracks[slot].loaded = false;
    strlcpy(s_cleanTracks[slot].status, "stored", sizeof(s_cleanTracks[slot].status));
  }
  queueNextStoredCleanTrackStream();
  spiMaster.requestStatus();
  syncCleanTrackStateFromDaisy();
}

// Page‐transition broadcast pause: set when '/' is served, cleared after 2s
static volatile unsigned long pageTransitionMs = 0;
// volatile: escritos por processCommand (corre tanto en la tarea AsyncTCP
// como en systemTask via UDP) y leidos por los builders de estado. Son
// escalares word-atomicos; volatile garantiza visibilidad entre tareas.
static volatile bool gMasterDelayActive = false;
static volatile uint8_t gGrooveSwingAmount = 0;
static volatile bool gMasterPhaserActive = false;
static volatile float gMasterDelayMix = 0.0f;
static volatile float gMasterPhaserDepth = 0.0f;
static volatile float gMasterReverbMix = 0.3f;
static volatile bool gMasterFlangerActive = false;
static volatile bool gMasterCompressorActive = false;
static volatile int gMasterFilterType = 0;
static volatile int gMasterBitCrushBits = 16;
static volatile int gMasterSampleRateReduction = SAMPLE_RATE;
static volatile float gMasterFilterCutoff = 20000.0f;
static volatile float gMasterFilterResonance = 1.0f;
static volatile float gMasterDistortion = 0.0f;
extern void dsqUploadPattern(int pattern);           // upload one pattern to Daisy sequencer (Core1 only)
extern void dsqUploadPatternDeferred(int pattern);   // safe from Core0: sets flag for Core1
extern void dsqSelectPatternDeferred(int pattern);   // select-only path (1 SPI cmd, no reupload)
extern void dsqUploadAndPlayDeferred(int pattern);   // upload + select + play, en orden garantizado

// Helper: lee todos los param locks de un step del Sequencer y los envía a Daisy
static void dsqSyncParamLock(int pat, int track, int step) {
    uint16_t ch = sequencer.getStepCutoffLock(pat, track, step);
    bool ce = (ch != 0 && ch != 1000);  // 1000 = valor default "sin lock"
    uint8_t rv = sequencer.getStepReverbSendLock(pat, track, step);
    bool re = (rv != 0);
    uint8_t vl = sequencer.getStepVolumeLock(pat, track, step);
    bool ve = (vl != 0);
    spiMaster.dsqSetParamLock((uint8_t)pat, (uint8_t)track, (uint8_t)step,
        ce, ch, re, rv, ve, vl);
}

static String normalizePatternBankPath(const String& requestPath) {
  String path = requestPath;
  path.trim();
  if (path.length() == 0) return String();
  if (path.indexOf("..") >= 0) return String();
  if (!path.startsWith("/patterns/")) {
    if (path.startsWith("/")) path = "/patterns" + path;
    else path = "/patterns/" + path;
  }
  if (!path.endsWith(".json")) path += ".json";
  return path;
}

static int normalizePatternLength(int requested) {
  if (requested <= 16) return 16;
  if (requested <= 32) return 32;
  return 64;
}

static bool loadPatternBankFromFs(const String& requestPath, String* errMsg = nullptr) {
  String path = normalizePatternBankPath(requestPath);
  if (path.length() == 0) {
    if (errMsg) *errMsg = "bad_path";
    return false;
  }
  if (!LittleFS.exists(path)) {
    if (errMsg) *errMsg = "not_found";
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    if (errMsg) *errMsg = "open_failed";
    return false;
  }

  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, f);
  f.close();
  if (error) {
    if (errMsg) *errMsg = "json_error";
    return false;
  }

  int stepCount = normalizePatternLength(doc["stepCount"] | 16);
  sequencer.setPatternLength(stepCount);

  float tempo = doc["tempo"] | 0.0f;
  if (tempo >= 40.0f && tempo <= 240.0f) {
    sequencer.setTempo(tempo);
    spiMaster.setTempo(tempo);
  }

  JsonArrayConst patterns = doc["patterns"].as<JsonArrayConst>();
  if (patterns.isNull() || patterns.size() == 0) {
    if (errMsg) *errMsg = "no_patterns";
    return false;
  }

  for (JsonObjectConst patObj : patterns) {
    int slot = patObj["slot"] | -1;
    if (slot < 0 || slot >= MAX_PATTERNS) continue;
    bool stepsData[MAX_TRACKS][STEPS_PER_PATTERN] = {};
    uint8_t velsData[MAX_TRACKS][STEPS_PER_PATTERN];
    memset(velsData, 127, sizeof(velsData));

    JsonArrayConst tracks = patObj["tracks"].as<JsonArrayConst>();
    for (JsonObjectConst trObj : tracks) {
      int track = trObj["track"] | -1;
      if (track < 0 || track >= MAX_TRACKS) continue;
      JsonArrayConst steps = trObj["steps"].as<JsonArrayConst>();
      JsonArrayConst velocities = trObj["velocities"].as<JsonArrayConst>();
      for (int step = 0; step < stepCount && step < STEPS_PER_PATTERN; step++) {
        bool active = false;
        if (!steps.isNull() && step < (int)steps.size()) {
          active = steps[step].as<int>() != 0;
        }
        stepsData[track][step] = active;
        if (!velocities.isNull() && step < (int)velocities.size()) {
          int velocity = velocities[step].as<int>();
          velsData[track][step] = constrain(velocity, 1, 127);
        }
      }
    }

    sequencer.clearPattern(slot);
    sequencer.setPatternBulk(slot, stepsData, velsData);
  }

  JsonArrayConst songChain = doc["songChain"].as<JsonArrayConst>();
  if (!songChain.isNull() && songChain.size() > 0) {
    uint8_t count = min((int)songChain.size(), (int)Sequencer::SONG_CHAIN_MAX);
    Sequencer::SongChainEntry entries[Sequencer::SONG_CHAIN_MAX] = {};
    SongEntry spiEntries[SONG_MAX_ENTRIES] = {};
    for (uint8_t i = 0; i < count; i++) {
      entries[i].pattern = constrain((int)(songChain[i]["pattern"] | 0), 0, MAX_PATTERNS - 1);
      entries[i].repeats = constrain((int)(songChain[i]["repeats"] | 1), 1, 16);
      spiEntries[i].pattern = entries[i].pattern;
      spiEntries[i].repeats = entries[i].repeats;
    }
    sequencer.songChainUpload(entries, count);
    spiMaster.songUpload(spiEntries, count);
    sequencer.songChainReset();
    spiMaster.songControl(2);
  }

  int selectPattern = doc["selectPattern"] | 0;
  selectPattern = constrain(selectPattern, 0, MAX_PATTERNS - 1);
  sequencer.selectPattern(selectPattern);
  dsqUploadPatternDeferred(selectPattern);
  return true;
}
extern SampleManager sampleManager;
extern void triggerPadWithLED(int track, uint8_t velocity);  // Función que enciende LED
extern void setLedMonoMode(bool enabled);
extern volatile int8_t gTrackSynthEngine[16];
extern void setTrackSynthEngine(int track, int8_t engine);
extern void setAllTrackSynthEngines(int8_t engine);
extern void releaseSequencerMelodicHolds();
static char gDaisyPadFiles[MAX_PADS][32];
static void clearTrackLoopStateForSynth(int track, AsyncWebSocket* ws);

static void clearTrackLoopStateForSynth(int track, AsyncWebSocket* ws) {
  if (track < 0 || track >= 16) {
    return;
  }

  const bool loopWasActive = sequencer.isLooping(track) || sequencer.isLoopPaused(track);
  if (sequencer.isLooping(track)) {
    sequencer.toggleLoop(track);
  }

  spiMaster.setPadLoop(track, false);
  spiMaster.stopSample(track);

  if (loopWasActive && ws && ws->count() > 0) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
      "{\"type\":\"loopState\",\"track\":%d,\"active\":false,\"paused\":false,\"loopType\":%d}",
      track, (int)sequencer.getLoopType(track));
    ws->textAll(buf, len);
  }
}

static void setDaisyPadFile(int padIndex, const String& fileName) {
  if (padIndex < 0 || padIndex >= MAX_PADS) return;
  strncpy(gDaisyPadFiles[padIndex], fileName.c_str(), 31);
  gDaisyPadFiles[padIndex][31] = '\0';
}

static void clearDaisyPadFiles() {
  memset(gDaisyPadFiles, 0, sizeof(gDaisyPadFiles));
}

static void clearDaisyPadFilesNotInMask(uint32_t loadedMask) {
  for (int i = 0; i < MAX_PADS; i++) {
    if ((loadedMask & (1UL << i)) == 0) {
      gDaisyPadFiles[i][0] = '\0';
    }
  }
}

static bool isSupportedSampleFile(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".raw") || lower.endsWith(".wav");
}

static const char* detectSampleFormat(const char* filename) {
  if (!filename) {
    return "";
  }
  String name = String(filename);
  if (name.endsWith(".wav") || name.endsWith(".WAV")) {
    return "wav";
  }
  if (name.endsWith(".raw") || name.endsWith(".RAW")) {
    return "raw";
  }
  return "";
}

static bool readWavInfo(File& file, uint32_t& rate, uint16_t& channels, uint16_t& bits) {
  if (!file) return false;
  file.seek(0, SeekSet);
  uint8_t header[44];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }
  channels = header[22] | (header[23] << 8);
  rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  bits = header[34] | (header[35] << 8);
  return true;
}

static void sendWebAsset(AsyncWebServerRequest *request,
                         const char* routePath,
                         const char* contentType,
                         const char* cacheControl = "no-cache") {
  String fsPath = "/web";
  fsPath += routePath;

  // Pasamos la ruta SIN .gz para que AsyncFileResponse use su detección automática:
  // si fsPath no existe pero fsPath+".gz" sí (caso normal en data_gz/web), la librería
  // abre el .gz y establece Content-Encoding: gzip, _sendContentLength=true, _chunked=false,
  // garantizando un Content-Length correcto y el header Content-Disposition sin extensión .gz.
  if (!LittleFS.exists(fsPath) && !LittleFS.exists(fsPath + ".gz")) {
    request->send(404, "text/plain", "Not found");
    return;
  }

  AsyncWebServerResponse *response = request->beginResponse(LittleFS, fsPath, contentType);
  response->addHeader("Cache-Control", cacheControl);
  response->addHeader("Vary", "Accept-Encoding");
  request->send(response);
}

static void populateStateDocument(JsonDocument& doc) {
  SdStatusResponse sdStat = {};
  bool sdOk = spiMaster.getCachedSdStatus(sdStat);  // Non-blocking: reads cache updated by Core1
  uint32_t sdLoadedMask = sdOk ? sdStat.samplesLoaded : 0;
  if (sdOk) {
    clearDaisyPadFilesNotInMask(sdLoadedMask);
  }

  int sdLoadedCount = 0;
  for (int i = 0; i < MAX_PADS; i++) {
    if (sdLoadedMask & (1UL << i)) sdLoadedCount++;
  }

  int localLoadedCount = sampleManager.getLoadedSamplesCount();
  doc["type"] = "state";
  doc["playing"] = sequencer.isPlaying();
  doc["tempo"] = sequencer.getTempo();
  doc["pattern"] = sequencer.getCurrentPattern();
  doc["step"] = sequencer.getCurrentStep();
  doc["sequencerVolume"] = spiMaster.getSequencerVolume();
  doc["liveVolume"] = spiMaster.getLiveVolume();
  doc["samplesLoaded"] = max(localLoadedCount, sdLoadedCount);
  doc["daisySamplesLoaded"] = sdLoadedCount;
  doc["memoryUsed"] = sampleManager.getTotalMemoryUsed();
  doc["psramFree"] = sampleManager.getFreePSRAM();
  doc["songMode"] = sequencer.isSongMode();
  doc["songLength"] = sequencer.getSongLength();
  doc["stepCount"] = sequencer.getPatternLength();
  doc["swing"] = gGrooveSwingAmount;
  doc["humanizeTimingMs"] = sequencer.getHumanizeTimingMs();
  doc["humanizeVelocity"] = sequencer.getHumanizeVelocityAmount();
  doc["heap"] = ESP.getFreeHeap();

  JsonArray loopActive = doc.createNestedArray("loopActive");
  JsonArray loopPaused = doc.createNestedArray("loopPaused");
  for (int track = 0; track < MAX_TRACKS; track++) {
    loopActive.add(sequencer.isLooping(track));
    loopPaused.add(sequencer.isLoopPaused(track));
  }

  JsonArray trackMuted = doc.createNestedArray("trackMuted");
  for (int track = 0; track < MAX_TRACKS; track++) {
    trackMuted.add(sequencer.isTrackMuted(track));
  }
  
  JsonArray trackVolumes = doc.createNestedArray("trackVolumes");
  for (int track = 0; track < MAX_TRACKS; track++) {
    trackVolumes.add(sequencer.getTrackVolume(track));
  }

  JsonArray trackSynthEngines = doc.createNestedArray("trackSynthEngines");
  for (int track = 0; track < MAX_TRACKS; track++) {
    trackSynthEngines.add((int)gTrackSynthEngine[track]);
  }

  // Compact samples: only send loaded sample info (not empty pads)
  JsonArray sampleArray = doc.createNestedArray("samples");
  for (int pad = 0; pad < MAX_SAMPLES; pad++) {
    bool loadedLocal = sampleManager.isSampleLoaded(pad);
    bool loadedDaisy = (sdLoadedMask & (1UL << pad)) != 0;
    bool loaded = loadedLocal || loadedDaisy;
    if (loaded) {
      JsonObject sampleObj = sampleArray.createNestedObject();
      sampleObj["pad"] = pad;
      sampleObj["loaded"] = true;
      const char* localName = sampleManager.getSampleName(pad);
      const char* daisyName = gDaisyPadFiles[pad];
      const char* finalName = (loadedLocal && localName) ? localName : daisyName;
      sampleObj["name"] = (finalName && finalName[0]) ? finalName : "";
      sampleObj["size"] = loadedLocal ? (sampleManager.getSampleLength(pad) * 2) : 0;
      sampleObj["format"] = detectSampleFormat(finalName);
    }
  }
  
  // Send pad filter states (for live pads)
  JsonArray padFilters = doc.createNestedArray("padFilters");
  for (int pad = 0; pad < 16; pad++) {
    FilterType filterType = spiMaster.getPadFilter(pad);
    padFilters.add((int)filterType);
  }
  
  // Send track filter states (for sequencer tracks)
  JsonArray trackFilters = doc.createNestedArray("trackFilters");
  for (int track = 0; track < 16; track++) {
    FilterType filterType = spiMaster.getTrackFilter(track);
    trackFilters.add((int)filterType);
  }
  
  // Daisy SD card info (explicit SD status query in strict boundary mode)
  if (sdOk) {
    doc["sdPresent"] = (bool)sdStat.present;
    doc["sdKit"] = String(sdStat.currentKit);
    doc["sdPadsLoaded"] = sdLoadedCount;
    doc["sdLoadedMask"] = sdLoadedMask;
  } else {
    doc["sdPresent"] = false;
    doc["sdKit"] = "";
    doc["sdPadsLoaded"] = 0;
    doc["sdLoadedMask"] = 0;
  }
  
  doc["lfoActive"] = 0;

  // New state: synth engine mask (16-bit for 9 engines)
  doc["synthActiveMask"] = spiMaster.getSynthActiveMask16();

  // Song Chain state
  doc["songChainActive"] = sequencer.isSongChainActive();
  doc["songChainIdx"] = sequencer.getSongChainIdx();
  doc["songChainRepeat"] = sequencer.getSongChainRepeatCnt();
  doc["songChainCount"] = sequencer.getSongChainCount();

  // Choke groups
  JsonArray chokeArr = doc.createNestedArray("chokeGroups");
  for (int i = 0; i < 16; i++) {
    chokeArr.add(spiMaster.getChokeGroup(i));
  }

  syncCleanTrackStateFromDaisy();
  JsonArray cleanTracks = doc.createNestedArray("cleanTracks");
  for (int track = 0; track < kCleanTrackCount; track++) {
    JsonObject cleanTrack = cleanTracks.createNestedObject();
    cleanTrack["id"] = track;
    cleanTrack["name"] = s_cleanTracks[track].name;
    cleanTrack["occupied"] = s_cleanTracks[track].occupied;
    cleanTrack["loaded"] = s_cleanTracks[track].loaded;
    cleanTrack["armed"] = s_cleanTracks[track].armed;
    cleanTrack["muted"] = s_cleanTracks[track].muted;
    cleanTrack["playing"] = s_cleanTracks[track].playing;
    cleanTrack["clipName"] = s_cleanTracks[track].clipName;
    cleanTrack["status"] = s_cleanTracks[track].status;
    cleanTrack["movable"] = s_cleanTracks[track].movable;
    cleanTrack["sizeBytes"] = s_cleanTracks[track].sizeBytes;
  }
}

static bool isClientReady(AsyncWebSocketClient* client) {
  return client != nullptr && client->status() == WS_CONNECTED;
}

// Cache of sample counts per family — avoid scanning filesystem in WS callback
static int cachedSampleCounts[16] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
static const char* sampleFamilies[] = {"BD", "SD", "CH", "OH", "CP", "CB", "RS", "CL", "MA", "CY", "HT", "LT", "MC", "MT", "HC", "LC"};

static void rebuildSampleCountCache() {
  for (int i = 0; i < 16; i++) {
    String path = String("/") + String(sampleFamilies[i]);
    int count = 0;
    
    File dir = LittleFS.open(path);
    if (dir && dir.isDirectory()) {
      File file = dir.openNextFile();
      while (file) {
        if (!file.isDirectory()) {
          String fullName = file.name();
          int lastSlash = fullName.lastIndexOf('/');
          String fileName = lastSlash >= 0 ? fullName.substring(lastSlash + 1) : fullName;
          if (isSupportedSampleFile(fileName)) {
            count++;
          }
        }
        file.close();
        file = dir.openNextFile();
      }
      dir.close();
    }
    cachedSampleCounts[i] = count;
  }
}

static void sendSampleCounts(AsyncWebSocketClient* client) {
  if (!client || !isClientReady(client)) {
    return;
  }
  
  // Build cache on first request
  if (cachedSampleCounts[0] < 0) {
    rebuildSampleCountCache();
  }
  
  StaticJsonDocument<512> sampleCountDoc;
  sampleCountDoc["type"] = "sampleCounts";
  
  for (int i = 0; i < 16; i++) {
    sampleCountDoc[sampleFamilies[i]] = cachedSampleCounts[i];
  }
  
  String countOutput;
  serializeJson(sampleCountDoc, countOutput);
  
  if (isClientReady(client)) {
    client->text(countOutput);
  }
}

WebInterface::WebInterface() {
  server = nullptr;
  ws = nullptr;
  initialized = false;
  midiController = nullptr;
  memset(wsReassemblySlots, 0, sizeof(wsReassemblySlots));
  for (auto& slot : wsReassemblySlots) {
    slot.clientId = 0xFFFFFFFF;
  }
  
  // Inicializar variables de rate limiting
  lastTriggerTime = 0;
  lastStepChangeTime = 0;
  lastBroadcastTime = 0;
  _staConnected = false;
  initCleanTrackState();
}

WebInterface::~WebInterface() {
  for (auto& slot : wsReassemblySlots) {
    releaseWsReassemblySlot(&slot);
  }
  if (server) {
    server->end();
  }
  if (ws) {
    ws->closeAll();
    delete ws;
    ws = nullptr;
  }
  if (server) {
    delete server;
    server = nullptr;
  }
}

bool WebInterface::begin(const char* apSsid, const char* apPassword,
                         const char* staSSID, const char* staPassword,
                         unsigned long staTimeoutMs) {
  _staConnected = false;
  loadCleanTracksStateFromFs();
  loadPersistedCleanTracksToDaisy();

  // Pad samples are no longer persisted to LittleFS: persisting 2-4MB WAVs
  // filled the 11MB partition and blocked stem uploads (fs_full), and a large
  // persisted sample could reset the S3 on boot. Remove any left over from the
  // old persistence so the filesystem is free for stems/clean tracks.
  _bootReloadCount = 0;
  _bootReloadHead  = 0;
  for (int i = 0; i < 16; i++) {
    char path[28];
    snprintf(path, sizeof(path), "/samples/pad%d.wav", i);
    if (LittleFS.exists(path)) LittleFS.remove(path);
  }

  WiFi.setSleep(false);
  WiFi.persistent(false);          // no guarda credenciales en NVS (evita flash corrupto)
  WiFi.mode(WIFI_OFF);
  delay(100);

  // ── Decide mode: STA+AP if home SSID provided, AP-only otherwise ──
  bool tryStation = (staSSID && staSSID[0] != '\0');

  if (tryStation) {
    // --- STA + AP dual mode ---
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    // Start AP first so it's always reachable
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(apSsid, apPassword, 11, 0, 4);
    delay(200);

    // Now try STA with static IP (192.168.1.80 — the "808" IP!)
    IPAddress staIP(192, 168, 1, 80);
    IPAddress staGW(192, 168, 1, 1);
    IPAddress staSN(255, 255, 255, 0);
    IPAddress staDNS(192, 168, 1, 1);
    WiFi.config(staIP, staGW, staSN, staDNS);

    WiFi.begin(staSSID, staPassword);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < staTimeoutMs) {
      delay(250);
      yield();
    }
    if (WiFi.status() == WL_CONNECTED) {
      _staConnected = true;
      // Restart AP on SAME channel as STA to eliminate radio channel switching
      // (channel hopping causes hardware interrupts that jitter Core1 audio)
      uint8_t staCh = WiFi.channel();
      if (staCh > 0 && staCh != 1) {
        WiFi.softAP(apSsid, apPassword, staCh, 0, 4);
        delay(100);
      }
      Serial.printf("[WiFi] STA connected: %s  IP: %s  ch=%d\n",
                    staSSID, WiFi.localIP().toString().c_str(), staCh);
    } else {
      Serial.printf("[WiFi] STA failed for '%s', AP-only mode\n", staSSID);
      // Keep AP running, STA just didn't connect
    }
  } else {
    // --- AP-only mode ---
    WiFi.mode(WIFI_AP);
    delay(50);

    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(apSsid, apPassword, 11, 0, 4);
    delay(200);
  }

  // Protocolo b/g/n y beacon 100ms
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_AP, &conf);
  conf.ap.beacon_interval = 100;
  esp_wifi_set_config(WIFI_IF_AP, &conf);

  WiFi.setSleep(false);
  
  // Crear servidor web
  server = new AsyncWebServer(80);
  ws = new AsyncWebSocket("/ws");

  // WebSocket handler
  ws->onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, 
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    this->onWebSocketEvent(server, client, type, arg, data, len);
  });
  
  server->addHandler(ws);
  
  // Servir archivos grandes con gzip explícito para máximo rendimiento
  server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request){
    // Pause periodic broadcasts during page transition to free TCP/Core0
    pageTransitionMs = millis();
    sendWebAsset(request, "/index.html", "text/html", "no-cache, no-store, must-revalidate");
  });

  
  server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/index.html", "text/html", "no-cache, no-store, must-revalidate");
  });
  
  server->on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/app.js", "application/javascript", "no-cache");
  });

  // Navegación común + indicador de conexión (cargado por todas las páginas)
  server->on("/nav.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/nav.js", "application/javascript", "no-cache");
  });

  server->on("/sample-editor.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/sample-editor.js", "application/javascript", "no-cache");
  });
  
  server->on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/style.css", "text/css", "no-cache");
  });

  server->on("/theme-vars.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/theme-vars.css", "text/css", "no-cache");
  });
  
  server->on("/keyboard-controls.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/keyboard-controls.js", "application/javascript", "no-cache");
  });
  
  server->on("/keyboard-styles.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/keyboard-styles.css", "text/css", "no-cache");
  });
  
  server->on("/midi-import.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/midi-import.js", "application/javascript", "no-cache");
  });
  
  server->on("/chat-agent.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/chat-agent.js", "application/javascript", "no-cache");
  });
  
  server->on("/waveform-visualizer.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/waveform-visualizer.js", "application/javascript", "no-cache");
  });

  server->on("/synth-editor.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/synth-editor.js", "application/javascript", "no-cache");
  });

  server->on("/export-pattern.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/export-pattern.js", "application/javascript", "no-cache");
  });

  server->on("/melody-editor.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/melody-editor.js", "application/javascript", "no-cache");
  });
  
  // Patchbay page
  server->on("/patchbay", HTTP_GET, [this](AsyncWebServerRequest *request){
    pageTransitionMs = millis();
    if (ws) ws->cleanupClients(0);
    sendWebAsset(request, "/patchbay.html", "text/html");
  });
  
  server->on("/patchbay.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/patchbay.css", "text/css", "no-cache");
  });
  
  server->on("/patchbay.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/patchbay.js", "application/javascript", "no-cache");
  });

  // Multiview page — redirect to .html served by serveStatic (avoids AsyncFileResponse 500 edge case)
  server->on("/multiview", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/multiview.html");
  });

  server->on("/multiview.html", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/multiview.html", "text/html");
  });

  server->on("/multiview.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/multiview.css", "text/css", "no-cache");
  });
  
  server->on("/multiview.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/multiview.js", "application/javascript", "no-cache");
  });

  // Live gesture page
  server->on("/gesture", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture.html", "text/html", "no-cache, no-store, must-revalidate");
  });

  server->on("/gesture.html", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture.html", "text/html", "no-cache, no-store, must-revalidate");
  });

  server->on("/gesture-pro", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture-pro.html", "text/html", "no-cache, no-store, must-revalidate");
  });

  server->on("/gesture-pro.html", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture-pro.html", "text/html", "no-cache, no-store, must-revalidate");
  });

  server->on("/gesture.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture.js", "application/javascript", "no-cache");
  });

  server->on("/gesture-styles.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/gesture-styles.css", "text/css", "no-cache");
  });

  // Mobile page — los assets existían en el FS pero no había ruta que los
  // sirviera (solo era accesible vía bridge externo). Con esto /mobile
  // funciona directo desde el ESP32 y entra en la navegación común.
  server->on("/mobile", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/mobile.html", "text/html", "no-cache");
  });

  server->on("/mobile.html", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/mobile.html", "text/html", "no-cache");
  });

  server->on("/mobile.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/mobile.css", "text/css", "no-cache");
  });

  server->on("/mobile.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/mobile.js", "application/javascript", "no-cache");
  });

  // Admin page
  server->on("/adm", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/admin.html", "text/html", "no-cache");
  });

  server->on("/admin.css", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/admin.css", "text/css", "no-cache");
  });

  server->on("/admin.js", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/admin.js", "application/javascript", "no-cache");
  });

  server->on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    sendWebAsset(request, "/favicon.ico", "image/x-icon", "max-age=86400, immutable");
  });

  // 404 para rutas desconocidas: evita que el navegador quede bloqueado esperando respuesta
  server->onNotFound([](AsyncWebServerRequest *request){
    request->send(404, "text/plain", "Not found");
  });
  
  // API REST
  server->on("/api/trigger", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("pad", true)) {
      int pad = request->getParam("pad", true)->value().toInt();
      triggerPadWithLED(pad, 127);  // Enciende LED RGB
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing pad parameter");
    }
  });

  server->on("/api/trigger", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("pad")) {
      int pad = request->getParam("pad")->value().toInt();
      triggerPadWithLED(pad, 127);
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing pad parameter");
    }
  });
  
  server->on("/api/tempo", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("value", true)) {
      float tempo = request->getParam("value", true)->value().toFloat();
      sequencer.setTempo(tempo);
      spiMaster.setTempo(tempo);
      request->send(200, "text/plain", "OK");
    }
  });
  
  server->on("/api/pattern", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("index", true)) {
      int pattern = request->getParam("index", true)->value().toInt();
      sequencer.selectPattern(pattern);
      // Reliability first: re-upload selected pattern so Daisy cannot stay with
      // a stale/empty slot if a previous upload was partial.
      dsqUploadPatternDeferred(pattern);
      request->send(200, "text/plain", "OK");
    }
  });
  
  server->on("/api/sequencer", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("action", true)) {
      String action = request->getParam("action", true)->value();
      if (action == "start") { sequencer.start(); dsqUploadAndPlayDeferred(sequencer.getCurrentPattern()); }
      else if (action == "stop") { sequencer.stop(); spiMaster.dsqControl(0); }
      request->send(200, "text/plain", "OK");
    }
  });
  
  server->on("/api/getPattern", HTTP_GET, [](AsyncWebServerRequest *request){
    int pattern = sequencer.getCurrentPattern();
    if (request->hasParam("index")) {
      pattern = request->getParam("index")->value().toInt();
    } else if (request->hasParam("pattern")) {
      pattern = request->getParam("pattern")->value().toInt();
    }
    if (pattern < 0 || pattern >= MAX_PATTERNS) {
      request->send(400, "application/json", "{\"error\":\"bad_pattern\"}");
      return;
    }
    DynamicJsonDocument doc(4096);
    
    for (int track = 0; track < 16; track++) {
      JsonArray trackSteps = doc.createNestedArray(String(track));
      for (int step = 0; step < sequencer.getPatternLength(); step++) {
        trackSteps.add(sequencer.getStep(pattern, track, step));
      }
    }
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  server->on("/api/patternBanks", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(2048);
    JsonArray files = doc.createNestedArray("files");
    File dir = LittleFS.open("/patterns", "r");
    if (dir) {
      File entry = dir.openNextFile();
      while (entry) {
        if (!entry.isDirectory()) {
          String name = entry.name();
          if (name.endsWith(".json")) files.add(name);
        }
        entry = dir.openNextFile();
      }
      dir.close();
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server->on("/api/patternBank/load", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!request->hasParam("file")) {
      request->send(400, "application/json", "{\"error\":\"missing_file\"}");
      return;
    }
    String err;
    String file = request->getParam("file")->value();
    if (!loadPatternBankFromFs(file, &err)) {
      String out = String("{\"error\":\"") + err + "\"}";
      request->send(400, "application/json", out);
      return;
    }
    broadcastSequencerState();
    broadcastUdpSongPattern(sequencer.getCurrentPattern(), sequencer.getSongChainCount());
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server->on("/api/p4State", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<4096> doc;
    SdStatusResponse sdStat = {};
    bool sdOk = spiMaster.getCachedSdStatus(sdStat);
    uint32_t sdLoadedMask = sdOk ? sdStat.samplesLoaded : 0;

    doc["pattern"] = sequencer.getCurrentPattern();
    doc["playing"] = sequencer.isPlaying();
    doc["tempo"] = sequencer.getTempo();
    doc["step"] = sequencer.getCurrentStep();
    doc["stepCount"] = sequencer.getPatternLength();
    doc["masterVolume"] = spiMaster.getMasterVolume();
    doc["sequencerVolume"] = spiMaster.getSequencerVolume();
    doc["liveVolume"] = spiMaster.getLiveVolume();
    doc["kit"] = sdOk ? String(sdStat.currentKit) : "";
    doc["sdPresent"] = sdOk ? (bool)sdStat.present : false;
    doc["sdLoadedMask"] = sdLoadedMask;

    JsonArray mute = doc.createNestedArray("mute");
    JsonArray solo = doc.createNestedArray("solo");
    JsonArray volumes = doc.createNestedArray("trackVolumes");
    JsonArray trackSynthEngines = doc.createNestedArray("trackSynthEngines");
    for (int track = 0; track < MAX_TRACKS; track++) {
      mute.add(sequencer.isTrackMuted(track) || spiMaster.getTrackMute(track));
      solo.add(spiMaster.getTrackSolo(track));
      volumes.add(sequencer.getTrackVolume(track));
      trackSynthEngines.add((int)gTrackSynthEngine[track]);
    }

    JsonObject fx = doc.createNestedObject("fx");
    fx["filterType"] = gMasterFilterType;
    fx["filterCutoff"] = gMasterFilterCutoff;
    fx["filterResonance"] = gMasterFilterResonance;
    fx["distortion"] = gMasterDistortion;
    fx["bitCrush"] = gMasterBitCrushBits;
    fx["sampleRate"] = gMasterSampleRateReduction;
    fx["delayActive"] = gMasterDelayActive;
    fx["delayMix"] = gMasterDelayMix;
    fx["reverbActive"] = spiMaster.isReverbActive();
    fx["reverbMix"] = gMasterReverbMix;
    fx["phaserActive"] = gMasterPhaserActive;
    fx["phaserDepth"] = gMasterPhaserDepth;

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });
  
  // Endpoint para info del sistema (para dashboard /adm)
  // ZERO SPI calls here — only reads cached data.
  // SPI polling (status/peaks/ping) runs on Core1 via SPIMaster::process().

  // === System Log: download raw ===
  server->on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists(SYSLOG_PATH)) {
      request->send(LittleFS, SYSLOG_PATH, "text/plain");
    } else {
      request->send(200, "text/plain", "(empty log)");
    }
  });

  // === System Log: clear ===
  server->on("/api/log", HTTP_DELETE, [](AsyncWebServerRequest *request){
    syslogClear();
    request->send(200, "text/plain", "log cleared");
  });

  // === Storage info: free/used/total ===
  server->on("/api/storage", HTTP_GET, [](AsyncWebServerRequest *request){
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    char buf[128];
    snprintf(buf, sizeof(buf),
      "{\"total\":%u,\"used\":%u,\"free\":%u}",
      (unsigned)total, (unsigned)used, (unsigned)(total - used));
    request->send(200, "application/json", buf);
  });

  // === MIDI preset files: list + serve ===
  server->on("/api/midi/list", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    File dir = LittleFS.open("/midi");
    bool first = true;
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        String name = f.name();
        if (name.endsWith(".mid") || name.endsWith(".midi")) {
          if (!first) json += ",";
          // Escape quotes in filename
          name.replace("\"", "\\\"");
          json += "\"" + name + "\"";
          first = false;
        }
        f = dir.openNextFile();
      }
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  server->on("/midi/*", HTTP_GET, [](AsyncWebServerRequest *request){
    String path = request->url();  // e.g. /midi/filename.mid
    // Contencion de path: solo servir desde /midi/ y rechazar traversal,
    // como ya hace /api/waveform.
    if (!path.startsWith("/midi/") || path.indexOf("..") >= 0) {
      request->send(400, "text/plain", "Bad path");
      return;
    }
    if (!LittleFS.exists(path)) {
      request->send(404, "text/plain", "Not found");
      return;
    }
    request->send(LittleFS, path, "audio/midi");
  });

  // === Melody presets: hardcoded classic patterns ===
  server->on("/api/melody/list", HTTP_GET, [](AsyncWebServerRequest *request){
    static const char MELODY_PRESETS[] PROGMEM = R"json([
      {"name":"Acid Am","notes":[45,0,57,48,45,0,57,60,45,0,57,48,45,0,60,57],"accents":[1,0,0,0,1,0,0,1,1,0,0,0,1,0,1,0],"slides":[0,0,0,1,0,0,1,0,0,0,0,1,0,0,0,1]},
      {"name":"303 Bass","notes":[36,0,36,38,36,0,41,0,36,0,36,43,36,0,41,38],"accents":[1,0,1,0,0,0,1,0,1,0,1,0,0,0,1,0],"slides":[0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1]},
      {"name":"Arpeggio","notes":[48,52,55,60,48,52,55,60,48,52,55,60,48,52,55,60],"accents":[1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0],"slides":[0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1]},
      {"name":"Detroit","notes":[48,48,0,48,60,0,48,48,0,48,55,0,48,48,0,60],"accents":[1,0,0,1,1,0,0,0,1,0,1,0,0,0,0,1],"slides":[0,1,0,0,0,0,0,1,0,0,0,0,0,1,0,0]},
      {"name":"Fm Bell","notes":[60,67,64,72,60,67,64,72,60,67,64,72,60,67,64,72],"accents":[1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0],"slides":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]},
      {"name":"Octave Riff","notes":[36,48,36,48,38,50,38,50,41,53,41,53,43,55,43,55],"accents":[1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0],"slides":[0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0]}
    ])json";
    request->send(200, "application/json", MELODY_PRESETS);
  });

  // === System Log: live viewer with auto-refresh ===
  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request){
    static const char LOG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>RED808 Log</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#111;color:#0f0;font:13px/1.4 'Courier New',monospace;height:100vh;display:flex;flex-direction:column}
#bar{background:#222;padding:6px 12px;display:flex;gap:12px;align-items:center;border-bottom:1px solid #333;flex-shrink:0}
#bar span{color:#888;font-size:12px}
#bar button{background:#333;color:#ccc;border:1px solid #555;padding:4px 12px;cursor:pointer;border-radius:3px;font-size:12px}
#bar button:hover{background:#444}
#bar button.danger{color:#f44}
#log{flex:1;overflow-y:auto;padding:8px 12px;white-space:pre-wrap;word-break:break-all}
.line-BOOT{color:#4af}.line-HEAP{color:#fa0}.line-WS{color:#e0e}.line-CMD{color:#0ff}
.line-WARN{color:#ff0;font-weight:bold}
</style></head><body>
<div id="bar">
  <span>RED808 SysLog</span>
  <button onclick="refresh()">Refresh</button>
  <button onclick="toggleAuto();" id="btnAuto">Auto: ON (3s)</button>
  <button onclick="clearLog()" class="danger">Clear Log</button>
  <span id="info"></span>
</div>
<div id="log">Loading...</div>
<script>
let auto_=true,timer,iv=3000;
const $log=document.getElementById('log'),$info=document.getElementById('info'),$btn=document.getElementById('btnAuto');
function colorize(t){return t.replace(/^(\[[\d.]+\]\[(\w+)\].*)/gm,(m,line,tag)=>'<span class="line-'+tag+'">'+line.replace(/</g,'&lt;')+'</span>');}
function refresh(){
  fetch('/api/log').then(r=>r.text()).then(t=>{
    $log.innerHTML=colorize(t);
    $log.scrollTop=$log.scrollHeight;
    $info.textContent=t.length+' bytes | '+new Date().toLocaleTimeString();
  }).catch(()=>{$info.textContent='fetch error';});
}
function toggleAuto(){auto_=!auto_;$btn.textContent='Auto: '+(auto_?'ON (3s)':'OFF');if(auto_)startAuto();else clearInterval(timer);}
function startAuto(){clearInterval(timer);timer=setInterval(refresh,iv);}
function clearLog(){if(confirm('Clear log?'))fetch('/api/log',{method:'DELETE'}).then(()=>refresh());}
refresh();if(auto_)startAuto();
</script></body></html>)rawliteral";
    request->send(200, "text/html", LOG_HTML);
  });

  server->on("/api/sysinfo", HTTP_GET, [this](AsyncWebServerRequest *request){
    StaticJsonDocument<3072> doc;
    
    // Info de memoria
    doc["heapFree"] = ESP.getFreeHeap();
    doc["heapSize"] = ESP.getHeapSize();
    doc["psramFree"] = ESP.getFreePsram();
    doc["psramSize"] = ESP.getPsramSize();
    doc["flashSize"] = ESP.getFlashChipSize();
    
    // Info de WiFi
    if (_staConnected) {
      doc["wifiMode"] = "STA+AP";
      doc["ssid"] = WiFi.SSID();
      doc["ip"] = WiFi.localIP().toString();
      doc["apIP"] = WiFi.softAPIP().toString();
    } else {
      doc["wifiMode"] = "AP";
      doc["ssid"] = WiFi.softAPSSID();
      doc["ip"] = WiFi.softAPIP().toString();
    }
    doc["channel"] = WiFi.channel();
    doc["txPower"] = "19.5dBm";
    doc["connectedStations"] = WiFi.softAPgetStationNum();
    
    // Info de WebSocket
    if (ws) {
      doc["wsClients"] = ws->count();
      JsonArray clients = doc.createNestedArray("wsClientList");
      for (auto& client : ws->getClients()) {
        JsonObject c = clients.createNestedObject();
        c["id"] = client.id();
        c["ip"] = client.remoteIP().toString();
        c["status"] = client.status();
      }
    }
    
    // Info de clientes UDP
    // Serial.printf("[sysinfo] UDP clients count: %d\n", udpClients.size()); // Comentado
    doc["udpClients"] = udpClients.size();
    JsonArray udpClientsList = doc.createNestedArray("udpClientList");
    unsigned long now = millis();
    for (const auto& pair : udpClients) {
      JsonObject c = udpClientsList.createNestedObject();
      c["ip"] = pair.second.ip.toString();
      c["port"] = pair.second.port;
      c["lastSeen"] = (now - pair.second.lastSeen) / 1000;
      c["packets"] = pair.second.packetCount;
      // Serial.printf("[sysinfo] Adding UDP client: %s:%d (packets: %d)\n", // Comentado
      //               pair.second.ip.toString().c_str(), pair.second.port, pair.second.packetCount);
    }
    
    // Info del secuenciador
    doc["tempo"] = sequencer.getTempo();
    doc["playing"] = sequencer.isPlaying();
    doc["pattern"] = sequencer.getCurrentPattern();
    doc["samplesLoaded"] = sampleManager.getLoadedSamplesCount();
    doc["memoryUsed"] = sampleManager.getTotalMemoryUsed();

    // Daisy realtime telemetry — uses cached data only (no SPI calls)
    doc["daisyConnected"] = spiMaster.isConnected();
    doc["daisyPingOk"] = spiMaster.isConnected();
    doc["daisyRttMs"] = spiMaster.getLastPingMs();
    doc["daisyVoices"] = spiMaster.getActiveVoices();
    doc["daisyCpu"] = spiMaster.getCpuLoad();
    doc["daisyCpuPeak"] = spiMaster.getCpuPeak();
    doc["daisyPerfStress"] = spiMaster.isPerformanceStressMode();
    doc["daisyMasterPeak"] = spiMaster.getMasterPeak();
    doc["daisyKit"] = String(spiMaster.getCurrentKitName());
    // Diagnostics
    StatusResponse daisyStat = {};
    spiMaster.getStatusSnapshot(daisyStat);
    doc["daisyPadsLoaded"]  = daisyStat.totalPadsLoaded;
    doc["daisyPadsMask"]    = daisyStat.padsLoadedMask;
    doc["daisySpiErrCnt"]   = (int)daisyStat.spiErrCnt;
    doc["daisySpiRingDrops"] = (int)daisyStat.spiRingDrops;
    doc["daisyMasterClip"] = daisyStat.masterClipFlag != 0;

    float peaks[16];
    spiMaster.getTrackPeaks(peaks, 16);
    JsonArray peaksArray = doc.createNestedArray("daisyTrackPeaks");
    for (int i = 0; i < 16; i++) {
      peaksArray.add(peaks[i]);
    }
    
    // Info MIDI USB
    if (midiController) {
      MIDIDeviceInfo midiInfo = midiController->getDeviceInfo();
      doc["midiEnabled"] = true;
      doc["midiConnected"] = midiInfo.connected;
      if (midiInfo.connected) {
        doc["midiDevice"] = midiInfo.deviceName;
        doc["midiVendorId"] = String(midiInfo.vendorId, HEX);
        doc["midiProductId"] = String(midiInfo.productId, HEX);
      }
    } else {
      doc["midiEnabled"] = false;
      doc["midiConnected"] = false;
    }
    
    // Uptime
    doc["uptime"] = millis();
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });
  
  // Endpoint para subir samples WAV
  server->on("/api/upload", HTTP_POST, 
    [](AsyncWebServerRequest *request){
      // No enviar respuesta aquí - se maneja en handleDaisyUpload cuando final=true
    },
    [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
      String target = "pad";
      if (request->hasParam("target")) {
        target = request->getParam("target")->value();
      } else if (request->hasParam("target", false)) {
        target = request->getParam("target", false)->value();
      }
      if (target == "cleanTrack") {
        handleCleanTrackUpload(request, filename, index, data, len, final);
      } else {
        // Pad/xtra WAV import: stream straight to the Daisy as bytes arrive
        // (handleDaisyUpload), converting to mono on the fly. We do NOT buffer
        // the whole WAV in PSRAM — an 8MB S3 can't hold a ~4MB raw file + the
        // decoded sample at once ("No PSRAM for sample"). The Daisy has 64MB and
        // stores it. The old "data chunk failed" drops are fixed by backpressure
        // in processDaisyUploadPcm + the 4MHz sample clock.
        handleDaisyUpload(request, filename, index, data, len, final);
      }
    }
  );

  server->on("/api/uploadDaisy", HTTP_POST,
    [](AsyncWebServerRequest *request){},
    [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
      handleDaisyUpload(request, filename, index, data, len, final);
    }
  );

  server->on("/api/unloadDaisy", HTTP_POST, [this](AsyncWebServerRequest *request){
    int pad = -1;
    if (request->hasParam("pad")) {
      pad = request->getParam("pad")->value().toInt();
    } else if (request->hasParam("pad", false)) {
      pad = request->getParam("pad", false)->value().toInt();
    }
    if (pad < 0 || pad >= MAX_SAMPLES) {
      request->send(400, "application/json", "{\"success\":false,\"message\":\"Invalid pad\"}");
      return;
    }
    spiMaster.unloadSample(pad);
    broadcastUploadComplete(pad, true, "Daisy sample unloaded");
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Daisy sample unloaded\"}");
  });
  
  // MIDI Mapping endpoints
  server->on("/api/midi/mapping", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!midiController) {
      request->send(503, "application/json", "{\"error\":\"MIDI disabled\"}");
      return;
    }

    StaticJsonDocument<2048> doc;
    JsonArray mappings = doc.createNestedArray("mappings");
    
    int count = 0;
    const MIDINoteMapping* maps = midiController->getAllMappings(count);
    for (int i = 0; i < count; i++) {
      if (maps[i].enabled) {
        JsonObject mapping = mappings.createNestedObject();
        mapping["note"] = maps[i].note;
        mapping["pad"] = maps[i].pad;
      }
    }
    
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });
  
  server->on("/api/midi/mapping", HTTP_POST, [this](AsyncWebServerRequest *request){}, NULL,
    [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (!midiController) {
        request->send(503, "application/json", "{\"error\":\"MIDI disabled\"}");
        return;
      }

      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, data, len);
      
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      
      // Procesar mappings
      if (doc.containsKey("note") && doc.containsKey("pad")) {
        uint8_t note = doc["note"];
        int8_t pad = doc["pad"];
        midiController->setPadMapping(pad, note);  // busca por pad, actualiza nota
        request->send(200, "application/json", "{\"success\":true}");
      } else if (doc.containsKey("reset") && doc["reset"] == true) {
        midiController->resetToDefaultMapping();
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Mapping reset to default\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
      }
    }
  );
  
  #if ENABLE_PHYSICAL_BUTTONS
  // ── GET /api/buttons — devuelve configuración guardada ─────────────────
  server->on("/api/buttons", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/buttons.json")) {
      AsyncWebServerResponse *resp = request->beginResponse(
          LittleFS, "/buttons.json", "application/json");
      resp->addHeader("Cache-Control", "no-cache");
      request->send(resp);
    } else {
      // Devolver config por defecto si no hay archivo guardado
      request->send(200, "application/json",
        "[{\"funcId\":1,\"colorOff\":16711680,\"colorOn\":65280,\"label\":\"PLAY/PAUSE\"},"
        "{\"funcId\":7,\"colorOff\":16711680,\"colorOn\":34816,\"label\":\"PREV+PLAY\"},"
        "{\"funcId\":6,\"colorOff\":16711680,\"colorOn\":34816,\"label\":\"NEXT+PLAY\"},"
        "{\"funcId\":2,\"colorOff\":16711680,\"colorOn\":16711680,\"label\":\"STOP\"}]");
    }
  });

  // ── POST /api/buttons — guarda y aplica nueva configuración ────────────
  server->on("/api/buttons", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    NULL,
    [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = String((char*)data, len);
      // Validar JSON básico
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, body)) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      // Extraer array — acepta tanto [{...}] como {"buttons":[{...}]}
      JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["buttons"].as<JsonArray>();
      if (arr.isNull()) {
        request->send(400, "application/json", "{\"error\":\"Missing buttons array\"}");
        return;
      }
      // Guardar SOLO el array plano en LittleFS
      String arrStr;
      serializeJson(arr, arrStr);
      File f = LittleFS.open("/buttons.json", "w");
      if (f) { f.print(arrStr); f.close(); }
      // Aplicar en tiempo real vía callback (pasa el array plano)
      if (_btnConfigCb) _btnConfigCb(arrStr);
      // Broadcast a todos los clientes web
      String bcast = "{\"type\":\"btnConfig\",\"buttons\":" + arrStr + "}";
      ws->textAll(bcast);
      request->send(200, "application/json", "{\"success\":true}");
    }
  );
  #endif

  // Endpoint para obtener forma de onda de un sample cargado (para visualizador)
  server->on("/api/waveform", HTTP_GET, [](AsyncWebServerRequest *request){
    // Mode 1: waveform from loaded pad (?pad=N)
    // Mode 2: waveform from file on LittleFS (?file=/family/name.wav)
    
    int points = 200;
    if (request->hasParam("points")) {
      points = request->getParam("points")->value().toInt();
      if (points < 20) points = 20;
      if (points > 400) points = 400;
    }
    
    // === MODE 2: From file path (for preview before loading) ===
    if (request->hasParam("file")) {
      String filePath = request->getParam("file")->value();
      // Higiene de ruta: rechazar traversal y rutas vacias (lectura solo dentro de LittleFS)
      if (filePath.length() == 0 || filePath.indexOf("..") >= 0) {
        request->send(400, "application/json", "{\"error\":\"bad_path\"}");
        return;
      }
      if (!filePath.startsWith("/")) filePath = "/" + filePath;
      
      File file = LittleFS.open(filePath, "r");
      if (!file) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
      }
      
      size_t fileSize = file.size();
      
      // Parse WAV header to find data chunk
      uint32_t dataOffset = 0;
      uint32_t dataSize = 0;
      uint16_t numChannels = 1;
      uint16_t bitsPerSample = 16;
      uint32_t sampleRate = SAMPLE_RATE;
      
      String fname = filePath;
      fname.toLowerCase();
      
      if (fname.endsWith(".wav")) {
        // Read WAV header
        uint8_t header[44];
        file.seek(0);
        if (file.read(header, 44) == 44 && memcmp(header, "RIFF", 4) == 0) {
          numChannels = header[22] | (header[23] << 8);
          sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
          bitsPerSample = header[34] | (header[35] << 8);
          
          // Find data chunk (with safety limit to prevent infinite loop)
          file.seek(36);
          int chunkSearchLimit = 20;  // Max 20 chunks to search
          while (file.available() >= 8 && chunkSearchLimit-- > 0) {
            char chunkId[4];
            uint32_t chunkSize;
            if (file.read((uint8_t*)chunkId, 4) != 4) break;
            if (file.read((uint8_t*)&chunkSize, 4) != 4) break;
            if (memcmp(chunkId, "data", 4) == 0) {
              dataOffset = file.position();
              dataSize = chunkSize;
              break;
            }
            if (chunkSize == 0) break;  // Prevent infinite loop on corrupted WAV
            file.seek(file.position() + chunkSize);
          }
        }
        if (dataSize == 0) {
          file.close();
          request->send(400, "application/json", "{\"error\":\"Invalid WAV\"}");
          return;
        }
      } else {
        // RAW file - whole file is data
        dataOffset = 0;
        dataSize = fileSize;
      }
      
      uint32_t bytesPerSample = bitsPerSample / 8;
      uint32_t totalSamples = dataSize / bytesPerSample;
      if (numChannels == 2) totalSamples /= 2;
      
      uint32_t samplesPerPoint = totalSamples / points;
      if (samplesPerPoint == 0) samplesPerPoint = 1;
      int actualPoints = totalSamples / samplesPerPoint;
      if (actualPoints > points) actualPoints = points;
      
      float durationMs = (totalSamples * 1000.0f) / sampleRate;
      
      // Extract filename from path
      String name = filePath;
      int lastSlash = name.lastIndexOf('/');
      if (lastSlash >= 0) name = name.substring(lastSlash + 1);
      
      // Build response while reading file in chunks
      // Pre-reserve String to avoid repeated reallocs (~14 bytes per point)
      String json;
      json.reserve(200 + actualPoints * 14);
      json = "{\"file\":\"";
      json += name;
      json += "\",\"samples\":";
      json += totalSamples;
      json += ",\"duration\":";
      json += String(durationMs, 1);
      json += ",\"rate\":";
      json += sampleRate;
      json += ",\"points\":";
      json += actualPoints;
      json += ",\"peaks\":[";
      
      // Read peaks in small chunks — heap allocated to avoid stack overflow in async handler
      const int CHUNK_SAMPLES = 256;  // Reduced for safety
      int16_t* chunkBuf = (int16_t*)malloc(CHUNK_SAMPLES * 2 * sizeof(int16_t));
      if (!chunkBuf) {
        file.close();
        request->send(500, "application/json", "{\"error\":\"Memory\"}");
        return;
      }
      
      file.seek(dataOffset);
      
      for (int p = 0; p < actualPoints; p++) {
        int16_t maxVal = 0, minVal = 0;
        uint32_t remaining = samplesPerPoint;
        
        while (remaining > 0) {
          uint32_t toRead = remaining;
          if (toRead > CHUNK_SAMPLES) toRead = CHUNK_SAMPLES;
          
          size_t bytesToRead = toRead * bytesPerSample * numChannels;
          size_t bytesRead = file.read((uint8_t*)chunkBuf, bytesToRead);
          if (bytesRead == 0) break;
          
          uint32_t samplesRead = bytesRead / (bytesPerSample * numChannels);
          
          for (uint32_t j = 0; j < samplesRead; j++) {
            int16_t s;
            if (numChannels == 2) {
              s = (chunkBuf[j * 2] / 2) + (chunkBuf[j * 2 + 1] / 2);
            } else {
              s = chunkBuf[j];
            }
            if (s > maxVal) maxVal = s;
            if (s < minVal) minVal = s;
          }
          remaining -= samplesRead;
        }
        
        if (p > 0) json += ",";
        json += "[";
        json += (int)(maxVal >> 8);
        json += ",";
        json += (int)(minVal >> 8);
        json += "]";
        
        if (p % 10 == 9) yield();
      }
      json += "]}";
      
      free(chunkBuf);
      file.close();
      request->send(200, "application/json", json);
      return;
    }
    
    // === MODE 1: From loaded pad ===
    if (!request->hasParam("pad")) {
      request->send(400, "application/json", "{\"error\":\"Missing pad or file parameter\"}");
      return;
    }
    int pad = request->getParam("pad")->value().toInt();
    if (pad < 0 || pad >= MAX_SAMPLES) {
      request->send(400, "application/json", "{\"error\":\"Invalid pad\"}");
      return;
    }
    if (!sampleManager.isSampleLoaded(pad)) {
      request->send(404, "application/json", "{\"error\":\"No sample loaded\"}");
      return;
    }
    
    // 'points' already declared at top of lambda
    
    // Get waveform peaks (pairs: max, min per point)
    int8_t* peaks = (int8_t*)malloc(points * 2);
    if (!peaks) {
      request->send(500, "application/json", "{\"error\":\"Memory\"}");
      return;
    }
    
    int actualPoints = sampleManager.getWaveformPeaks(pad, peaks, points);
    
    // Build compact JSON response
    uint32_t sampleLen = sampleManager.getSampleLength(pad);
    float durationMs = (sampleLen * 1000.0f) / SAMPLE_RATE;
    
    // Use chunked response to avoid large buffer allocation
    String json = "{\"pad\":";
    json += pad;
    json += ",\"name\":\"";
    json += sampleManager.getSampleName(pad);
    json += "\",\"samples\":";
    json += sampleLen;
    json += ",\"duration\":";
    json += String(durationMs, 1);
    json += ",\"points\":";
    json += actualPoints;
    json += ",\"peaks\":[";
    
    for (int i = 0; i < actualPoints; i++) {
      if (i > 0) json += ",";
      json += "[";
      json += (int)peaks[i * 2];      // max (positive)
      json += ",";
      json += (int)peaks[i * 2 + 1];  // min (negative)
      json += "]";
      if (i % 20 == 19) yield(); // Yield cada 20 puntos
    }
    json += "]}";
    
    free(peaks);
    request->send(200, "application/json", json);
  });

  // Endpoint para descargar audio RAW del pad cargado (PCM 16-bit mono 48000Hz)
  server->on("/api/sampledata", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("pad")) {
      request->send(400, "application/json", "{\"error\":\"Missing pad parameter\"}");
      return;
    }
    int pad = request->getParam("pad")->value().toInt();
    if (pad < 0 || pad >= MAX_SAMPLES) {
      request->send(400, "application/json", "{\"error\":\"Invalid pad\"}");
      return;
    }
    if (!sampleManager.isSampleLoaded(pad)) {
      request->send(404, "application/json", "{\"error\":\"No sample loaded\"}");
      return;
    }

    int16_t* liveBuffer = sampleManager.getSampleBuffer(pad);
    uint32_t length = sampleManager.getSampleLength(pad);
    if (!liveBuffer || length == 0) {
      request->send(404, "application/json", "{\"error\":\"Empty sample\"}");
      return;
    }

    // Build WAV header + stream PCM data
    uint32_t dataSize = length * 2; // 16-bit = 2 bytes per sample
    uint32_t fileSize = 44 + dataSize;

    // El lambda chunked vive durante muchos callbacks async. Capturar el
    // puntero vivo del SampleManager era use-after-free si el pad se
    // recargaba/recortaba/liberaba durante la descarga. Copiamos a un buffer
    // PSRAM propiedad de la respuesta: el shared_ptr se libera cuando la
    // respuesta (y su lambda) se destruyen.
    std::shared_ptr<uint8_t> snapshot((uint8_t*)ps_malloc(dataSize), free);
    if (!snapshot) {
      request->send(503, "application/json", "{\"error\":\"Insufficient PSRAM\"}");
      return;
    }
    memcpy(snapshot.get(), liveBuffer, dataSize);

    AsyncWebServerResponse *response = request->beginChunkedResponse("audio/wav",
      [snapshot, dataSize, fileSize](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        if (index >= fileSize) return 0;

        size_t written = 0;

        // Write WAV header (first 44 bytes)
        if (index < 44) {
          uint8_t header[44];
          memcpy(header, "RIFF", 4);
          uint32_t riffSize = fileSize - 8;
          memcpy(header + 4, &riffSize, 4);
          memcpy(header + 8, "WAVE", 4);
          memcpy(header + 12, "fmt ", 4);
          uint32_t fmtSize = 16;
          memcpy(header + 16, &fmtSize, 4);
          uint16_t audioFormat = 1; // PCM
          memcpy(header + 20, &audioFormat, 2);
          uint16_t numChannels = 1;
          memcpy(header + 22, &numChannels, 2);
          uint32_t sampleRate = SAMPLE_RATE;
          memcpy(header + 24, &sampleRate, 4);
          uint32_t byteRate = SAMPLE_RATE * 2;
          memcpy(header + 28, &byteRate, 4);
          uint16_t blockAlign = 2;
          memcpy(header + 32, &blockAlign, 2);
          uint16_t bitsPerSample = 16;
          memcpy(header + 34, &bitsPerSample, 2);
          memcpy(header + 36, "data", 4);
          memcpy(header + 40, &dataSize, 4);

          size_t headerBytesToCopy = 44 - (size_t)index;
          if (headerBytesToCopy > maxLen) headerBytesToCopy = maxLen;
          memcpy(buf, header + index, headerBytesToCopy);
          written += headerBytesToCopy;
        }

        // Write PCM data
        if (index + written >= 44 && written < maxLen) {
          size_t pcmOffset = (index + written) - 44;
          size_t pcmRemaining = dataSize - pcmOffset;
          size_t toCopy = maxLen - written;
          if (toCopy > pcmRemaining) toCopy = pcmRemaining;
          memcpy(buf + written, snapshot.get() + pcmOffset, toCopy);
          written += toCopy;
        }

        return written;
      }
    );

    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Content-Disposition", "inline");
    request->send(response);
  });
  
  server->begin();

  // Mutex de los buffers JSON compartidos (creado antes que cualquier uso)
  if (!_jsonBufMutex) {
    _jsonBufMutex = xSemaphoreCreateMutex();
  }
  // Pre-allocate state broadcast buffer in PSRAM (one-time, never freed)
  if (!_stateBuf) {
    _stateBuf = (char*)ps_malloc(kStateBufSize);
  }
  // Pre-allocate pattern JSON buffer in PSRAM (one-time, never freed)
  if (!_patternBuf) {
    _patternBuf = (char*)ps_malloc(kPatternBufSize);
  }
  // Pre-allocate JSON document in PSRAM (one-time, reused via clear())
  if (!_stateDoc) {
    _stateDoc = new PsramJsonDocument(8192);
  }

  // Iniciar servidor UDP
  if (udp.begin(UDP_PORT)) {
  } else {
  }
  
  initialized = true;
  return true;
}

WebInterface::WsReassemblySlot* WebInterface::findWsReassemblySlot(uint32_t clientId, bool create) {
  WsReassemblySlot* freeSlot = nullptr;
  for (auto& slot : wsReassemblySlots) {
    if (slot.clientId == clientId) {
      return &slot;
    }
    if (create && !freeSlot && slot.clientId == 0xFFFFFFFF) {
      freeSlot = &slot;
    }
  }
  if (freeSlot) {
    freeSlot->clientId = clientId;
    freeSlot->buffer = nullptr;
    freeSlot->size = 0;
  }
  return freeSlot;
}

void WebInterface::releaseWsReassemblySlot(uint32_t clientId) {
  WsReassemblySlot* slot = findWsReassemblySlot(clientId, false);
  if (slot) {
    releaseWsReassemblySlot(slot);
  }
}

void WebInterface::releaseWsReassemblySlot(WsReassemblySlot* slot) {
  if (!slot) return;
  if (slot->buffer) {
    free(slot->buffer);
  }
  slot->buffer = nullptr;
  slot->size = 0;
  slot->clientId = 0xFFFFFFFF;
}

void WebInterface::onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // ⚠️ LÍMITE DE 3 CLIENTES para estabilidad
    if (ws->count() > 3) {
      client->close(1008, "Max clients reached");
      syslog("WS", "client %u rejected (max clients)", client->id());
      return;
    }
    syslog("WS", "client %u connected (total=%d) heap=%u",
           client->id(), ws->count(), ESP.getFreeHeap());
    
    
    StaticJsonDocument<512> basicState;
    basicState["type"] = "connected";
    basicState["playing"] = sequencer.isPlaying();
    basicState["tempo"] = sequencer.getTempo();
    basicState["pattern"] = sequencer.getCurrentPattern();
    basicState["clientId"] = client->id();
    
    // Serialize to stack buffer — avoid heap String
    char connectBuf[256];
    size_t cLen = serializeJson(basicState, connectBuf, sizeof(connectBuf));
    if (isClientReady(client) && cLen > 0) {
      client->text(connectBuf, cLen);
    }
  } else if (type == WS_EVT_DISCONNECT) {
    syslog("WS", "client %u disconnected (remaining=%d) heap=%u",
           client->id(), ws->count() > 0 ? ws->count()-1 : 0, ESP.getFreeHeap());
    releaseWsReassemblySlot(client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;

    // Guard rails de payload para evitar OOM/fragmentación bajo cargas anómalas
    if (info->opcode == WS_TEXT && info->len > kWsMaxTextBytes) {
      return;
    }
    if (info->opcode == WS_BINARY && info->len > kWsMaxBinaryBytes) {
      return;
    }
    
    bool _wsFreeAfter = false;
    WsReassemblySlot* reassemblySlot = nullptr;
    auto cleanupWsReassembly = [&]() {
      if (_wsFreeAfter && reassemblySlot) {
        releaseWsReassemblySlot(reassemblySlot);
      }
    };
    
    // Safe buffer variables (used for WS_TEXT to avoid data[len]=0 overflow)
    char* safeData = nullptr;
    bool safeFreeNeeded = false;
    
    if (!(info->final && info->index == 0 && info->len == len)) {
      // Fragmented frame - reassemble chunks into complete message
      if (info->index == 0) {
        if (info->opcode == WS_TEXT && info->len > kWsMaxTextBytes) {
          return;
        }
        reassemblySlot = findWsReassemblySlot(client->id(), true);
        if (!reassemblySlot) return;
        releaseWsReassemblySlot(reassemblySlot);
        reassemblySlot = findWsReassemblySlot(client->id(), true);
        if (!reassemblySlot) return;
        if (ESP.getFreeHeap() < (uint32_t)(info->len + 4096)) {
          return;
        }
        reassemblySlot->buffer = (uint8_t*)malloc(info->len + 2); // +2 for null terminator safety
        if (!reassemblySlot->buffer) {
          releaseWsReassemblySlot(reassemblySlot);
          return;
        }
        reassemblySlot->size = info->len;
      } else {
        reassemblySlot = findWsReassemblySlot(client->id(), false);
        if (!reassemblySlot || !reassemblySlot->buffer) {
          return;
        }
      }
      if (reassemblySlot->buffer && info->index + len <= reassemblySlot->size) {
        memcpy(reassemblySlot->buffer + info->index, data, len);
      } else {
        releaseWsReassemblySlot(reassemblySlot);
        return;
      }
      if (info->final && (info->index + len) == info->len && reassemblySlot->buffer) {
        reassemblySlot->buffer[reassemblySlot->size] = 0; // Safe null-terminate
        data = reassemblySlot->buffer;
        len = reassemblySlot->size;
        _wsFreeAfter = true;
      } else {
        return; // Still accumulating chunks
      }
    }
      
    // 1. MANEJO DE BINARIO (Baja latencia para Triggers)
    if (info->opcode == WS_BINARY) {
      // Protocolo: [0x90, PAD, VEL]
      if (len == 3 && data[0] == 0x90) {
         int pad = data[1];
         int velocity = data[2];
         // Acotar como en los caminos JSON: data[1] es 0-255 y aguas abajo
         // se usa como indice/pad sin mas validacion.
         if (pad >= 0 && pad < MAX_PADS) {
           triggerPadWithLED(pad, velocity);
         }
      }
    }
    // 2. MANEJO DE TEXTO (JSON normal)
    else if (info->opcode == WS_TEXT) {
      // Reject if heap critically low — prevent crash during JSON processing
      if (ESP.getFreeHeap() < 15000) {
        cleanupWsReassembly();
        return;
      }
      
      // SAFE null-terminate: copy to buffer with extra byte instead of data[len]=0
      if (_wsFreeAfter) {
        // Already in our reassembly buffer which has +2 bytes — already null-terminated above
        safeData = (char*)data;
      } else {
        // Non-fragmented message: copy to safe buffer
        safeData = (char*)malloc(len + 1);
        if (!safeData) {
          cleanupWsReassembly();
          return;
        }
        memcpy(safeData, data, len);
        safeData[len] = 0;
        safeFreeNeeded = true;
      }
      // Point data to safe null-terminated buffer for JSON parsing
      data = (uint8_t*)safeData;
        
        // Check for bulk pattern command (needs larger JSON doc)
        if (len > 400 && strstr((char*)data, "\"setBulk\"") != nullptr) {
          syslog("CMD", "setBulk len=%u heap=%u", (unsigned)len, ESP.getFreeHeap());
          if (ESP.getFreeHeap() < 35000) {
            StaticJsonDocument<64> errDoc;
            errDoc["type"] = "error"; errDoc["msg"] = "low_heap";
            String errStr; serializeJson(errDoc, errStr);
            if (isClientReady(client)) client->text(errStr);
            if (safeFreeNeeded) free(safeData);
            cleanupWsReassembly();
            return;
          }
          PsramJsonDocument bulkDoc(32768);  // PSRAM — avoids 32KB spike on internal DRAM
          DeserializationError bulkErr = deserializeJson(bulkDoc, (char*)data);
          if (!bulkErr) {
            int pattern = bulkDoc["p"].as<int>();
            // p = -1 means current pattern (single bar import)
            if (pattern < 0) pattern = sequencer.getCurrentPattern();
            if (pattern >= 0 && pattern < MAX_PATTERNS) {
              bool stepsData[MAX_TRACKS][STEPS_PER_PATTERN] = {};
              uint8_t velsData[MAX_TRACKS][STEPS_PER_PATTERN];
              memset(velsData, 127, sizeof(velsData));
              
              JsonArray sArr = bulkDoc["s"].as<JsonArray>();
              JsonArray vArr = bulkDoc["v"].as<JsonArray>();
              
              for (int t = 0; t < 16 && t < (int)sArr.size(); t++) {
                JsonArray trackSteps = sArr[t].as<JsonArray>();
                for (int s = 0; s < STEPS_PER_PATTERN && s < (int)trackSteps.size(); s++) {
                  stepsData[t][s] = trackSteps[s].as<int>() != 0;
                }
                if (vArr && t < (int)vArr.size()) {
                  JsonArray trackVels = vArr[t].as<JsonArray>();
                  for (int s = 0; s < STEPS_PER_PATTERN && s < (int)trackVels.size(); s++) {
                    int v = trackVels[s].as<int>();
                    velsData[t][s] = (v > 0 && v <= 127) ? v : 127;
                  }
                }
              }
              
              sequencer.setPatternBulk(pattern, stepsData, velsData);
              
              // Lightweight ACK — stack buffer, no heap alloc
              char ackBuf[40];
              int ackLen = snprintf(ackBuf, sizeof(ackBuf),
                "{\"type\":\"bulkAck\",\"p\":%d}", pattern);
              if (isClientReady(client)) {
                client->text(ackBuf, ackLen);
              }
            }
          }
          // Cleanup reassembly buffer before early return
          if (_wsFreeAfter) {
            cleanupWsReassembly();
          } else if (safeFreeNeeded) {
            free(safeData);
          }
          return; // Already handled
        }

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, (char*)data);
        
        if (!error) {
          // Procesar comandos comunes primero (start, stop, tempo, etc.)
          processCommand(doc);
          
          // Comandos específicos del WebSocket que requieren respuesta
          String cmd = doc["cmd"];
          
          if (cmd == "getPattern") {
            int pattern = sequencer.getCurrentPattern();
            syslog("CMD", "getPattern idx=%d heap=%u", pattern, ESP.getFreeHeap());
            // 6 data structures × 16 tracks × 16 steps — needs ~13-15KB ArduinoJson pool
            if (ESP.getFreeHeap() < 35000) {
              StaticJsonDocument<64> err; err["type"] = "error"; err["msg"] = "low_heap";
              String errStr; serializeJson(err, errStr);
              if (isClientReady(client)) client->text(errStr);
              if (safeFreeNeeded) free(safeData);
              cleanupWsReassembly();
              return;
            }
            yield();
            int stepCount = sequencer.getPatternLength();
            // Using PSRAM allocator to avoid 32KB spike on internal DRAM heap
            PsramJsonDocument responseDoc(stepCount > 16 ? 49152 : 24576);
            responseDoc["stepCount"] = stepCount;  // 8+ structures × 16×steps values
            responseDoc["type"] = "pattern";
            responseDoc["index"] = pattern;
            
            // Send steps (active/inactive) - 16 tracks activos
            for (int track = 0; track < 16; track++) {
              JsonArray trackSteps = responseDoc.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackSteps.add(sequencer.getStep(track, step));
              }
            }
            
            // Send velocities
            JsonObject velocitiesObj = responseDoc.createNestedObject("velocities");
            for (int track = 0; track < 16; track++) {
              JsonArray trackVels = velocitiesObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackVels.add(sequencer.getStepVelocity(track, step));
              }
            }
            
            // Send note lengths (1=full, 2=half, 4=quarter, 8=eighth)
            JsonObject noteLensObj = responseDoc.createNestedObject("noteLens");
            for (int track = 0; track < 16; track++) {
              JsonArray trackNL = noteLensObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackNL.add(sequencer.getStepNoteLen(track, step));
              }
            }

            JsonObject volumeLocksObj = responseDoc.createNestedObject("volumeLocks");
            for (int track = 0; track < 16; track++) {
              JsonArray trackLocks = volumeLocksObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                if (sequencer.hasStepVolumeLock(track, step)) {
                  trackLocks.add(sequencer.getStepVolumeLock(track, step));
                } else {
                  trackLocks.add(-1);
                }
              }
            }

            JsonObject probabilitiesObj = responseDoc.createNestedObject("probabilities");
            for (int track = 0; track < 16; track++) {
              JsonArray trackProb = probabilitiesObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackProb.add(sequencer.getStepProbability(track, step));
              }
            }

            JsonObject ratchetsObj = responseDoc.createNestedObject("ratchets");
            for (int track = 0; track < 16; track++) {
              JsonArray trackRat = ratchetsObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackRat.add(sequencer.getStepRatchet(track, step));
              }
            }

            JsonObject cutoffLocksObj = responseDoc.createNestedObject("cutoffLocks");
            for (int track = 0; track < 16; track++) {
              JsonArray trackLocks = cutoffLocksObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                if (sequencer.hasStepCutoffLock(track, step)) {
                  trackLocks.add((int)sequencer.getStepCutoffLock(track, step));
                } else {
                  trackLocks.add(-1);
                }
              }
            }

            JsonObject reverbLocksObj = responseDoc.createNestedObject("reverbLocks");
            for (int track = 0; track < 16; track++) {
              JsonArray trackLocks = reverbLocksObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                if (sequencer.hasStepReverbSendLock(track, step)) {
                  trackLocks.add((int)sequencer.getStepReverbSendLock(track, step));
                } else {
                  trackLocks.add(-1);
                }
              }
            }

            // Melody: per-step MIDI notes (0 = rest)
            JsonObject notesObj = responseDoc.createNestedObject("stepNotes");
            JsonObject noteVoicesObj = responseDoc.createNestedObject("stepNoteVoices");
            for (int track = 0; track < 16; track++) {
              JsonArray trackNotes = notesObj.createNestedArray(String(track));
              JsonArray trackVoices = noteVoicesObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackNotes.add(sequencer.getStepNote(track, step));
                JsonArray stepVoices = trackVoices.createNestedArray();
                for (int voice = 0; voice < MELODY_STEP_VOICES; voice++) {
                  stepVoices.add(sequencer.getStepNoteVoice(track, step, voice));
                }
              }
            }

            // Melody: per-step flags (bit0=accent, bit1=slide)
            JsonObject flagsObj = responseDoc.createNestedObject("stepFlags");
            for (int track = 0; track < 16; track++) {
              JsonArray trackFlags = flagsObj.createNestedArray(String(track));
              for (int step = 0; step < stepCount; step++) {
                trackFlags.add(sequencer.getStepFlags(track, step));
              }
            }
            
            // Serialize to PSRAM buffer to avoid String heap fragmentation
            if (!_patternBuf) _patternBuf = (char*)ps_malloc(kPatternBufSize);
            if (_patternBuf && jsonBufLock()) {
              size_t len = serializeJson(responseDoc, _patternBuf, kPatternBufSize);
              syslog("CMD", "getPat JSON len=%u heap=%u", (unsigned)len, ESP.getFreeHeap());
              if (len > 0 && len < kPatternBufSize) {
                if (isClientReady(client)) {
                  client->text(_patternBuf, len);
                } else {
                  ws->textAll(_patternBuf, len);
                }
              }
              jsonBufUnlock();
            }
            syslog("CMD", "getPat DONE heap=%u", ESP.getFreeHeap());
          }
          else if (cmd == "init") {
            // Cliente solicita inicialización completa
            // State doc lives in PSRAM — safe to send regardless of heap
            if (isClientReady(client)) {
              sendSequencerStateToClient(client);
            }

            // Enviar estado de MIDI scan (stack buffer, no heap)
            if (midiController && isClientReady(client)) {
              StaticJsonDocument<128> midiScanDoc;
              midiScanDoc["type"] = "midiScan";
              midiScanDoc["enabled"] = midiController->isScanEnabled();
              char midiBuf[64];
              size_t mLen = serializeJson(midiScanDoc, midiBuf, sizeof(midiBuf));
              if (mLen > 0) client->text(midiBuf, mLen);
            }
          }
          else if (cmd == "getSampleCounts") {
            // Nuevo comando para obtener conteos de samples
            sendSampleCounts(client);
          }
          else if (cmd == "getSamples") {
            // Obtener lista de samples de una familia desde LittleFS
            const char* family = doc["family"];
            int padIndex = doc["pad"];

            DynamicJsonDocument responseDoc(4096);  // Heap-allocated, flexible size
            responseDoc["type"] = "sampleList";
            responseDoc["family"] = family;
            responseDoc["pad"] = padIndex;

            if (!family) {
              responseDoc.createNestedArray("samples");
              String emptyOut;
              serializeJson(responseDoc, emptyOut);
              if (isClientReady(client)) client->text(emptyOut);
              else ws->textAll(emptyOut);
              if (safeFreeNeeded) free(safeData);
              cleanupWsReassembly();
              return;
            }

            String path = String("/") + String(family);

            File dir = LittleFS.open(path, "r");

            if (dir && dir.isDirectory()) {
              JsonArray samples = responseDoc.createNestedArray("samples");
              File file = dir.openNextFile();
              int count = 0;

              while (file) {
                if (!file.isDirectory()) {
                  String filename = file.name();
                  int lastSlash = filename.lastIndexOf('/');
                  if (lastSlash >= 0) {
                    filename = filename.substring(lastSlash + 1);
                  }

                  if (isSupportedSampleFile(filename)) {
                    JsonObject sampleObj = samples.createNestedObject();
                    sampleObj["name"] = filename;
                    sampleObj["size"] = file.size();
                    const char* format = detectSampleFormat(filename.c_str());
                    sampleObj["format"] = format;
                    uint32_t rate = 0;
                    uint16_t channels = 0;
                    uint16_t bits = 0;
                    if (format && String(format) == "wav") {
                      if (readWavInfo(file, rate, channels, bits)) {
                        sampleObj["rate"] = rate;
                        sampleObj["channels"] = channels;
                        sampleObj["bits"] = bits;
                      } else {
                        sampleObj["rate"] = 0;
                        sampleObj["channels"] = 0;
                        sampleObj["bits"] = 0;
                      }
                    } else {
                      sampleObj["rate"] = SAMPLE_RATE;
                      sampleObj["channels"] = 1;
                      sampleObj["bits"] = 16;
                    }
                    count++;

                    // Yield cada 3 samples para evitar watchdog
                    if (count % 3 == 0) {
                      yield();
                    }
                  }
                }
                file.close();
                file = dir.openNextFile();
              }
              dir.close();
            } else {
              responseDoc.createNestedArray("samples");  // folder not found → empty list
            }

            String output;
            serializeJson(responseDoc, output);
            if (isClientReady(client)) {
              client->text(output);
            } else {
              ws->textAll(output);
            }
          }
          // getSamples y loadSample ahora manejados en processCommand()
          // Comandos restantes ya procesados por processCommand()
        }
      }
    // Cleanup safe buffer if we allocated one (non-fragmented path)
    if (safeFreeNeeded) {
      free(safeData);
    }
    // Cleanup reassembly buffer after processing
    cleanupWsReassembly();
  }
}

void WebInterface::broadcastSequencerState() {
  if (!initialized || !ws || ws->count() == 0) return;
  
  // Rate limiting - max 2 per second to reduce UI flickering
  static unsigned long lastBroadcast = 0;
  unsigned long now = millis();
  if (now - lastBroadcast < 500) return;
  lastBroadcast = now;
  
  // All memory lives in PSRAM — zero heap allocation in this path
  if (_stateDoc && _stateBuf && jsonBufLock()) {
    _stateDoc->clear();
    populateStateDocument(*_stateDoc);
    size_t len = serializeJson(*_stateDoc, _stateBuf, kStateBufSize);
    if (len > 0 && len < kStateBufSize) {
      ws->textAll(_stateBuf, len);
    }
    jsonBufUnlock();
  }
}

bool WebInterface::loadPatternBank(const String& file, String* errMsg) {
  if (!loadPatternBankFromFs(file, errMsg)) return false;
  broadcastSequencerState();
  broadcastUdpSongPattern(sequencer.getCurrentPattern(), sequencer.getSongChainCount());
  return true;
}

bool WebInterface::shouldSendUdpStateSync(const char* cmd) const {
  if (!cmd) return false;
  return strcmp(cmd, "hello") == 0 ||
         strcmp(cmd, "get_state") == 0 ||
         strcmp(cmd, "getState") == 0 ||
         strcmp(cmd, "selectPattern") == 0 ||
         strcmp(cmd, "start") == 0 ||
         strcmp(cmd, "stop") == 0 ||
         strcmp(cmd, "tempo") == 0 ||
         strcmp(cmd, "mute") == 0 ||
         strcmp(cmd, "solo") == 0 ||
         strcmp(cmd, "setMuteMask") == 0 ||
         strcmp(cmd, "setSoloMask") == 0 ||
         strcmp(cmd, "setTrackSolo") == 0 ||
         strcmp(cmd, "getTrackVolumes") == 0 ||
         strncmp(cmd, "setFilter", 9) == 0 ||
         strncmp(cmd, "setDelay", 8) == 0 ||
         strncmp(cmd, "setReverb", 9) == 0 ||
         strncmp(cmd, "setChorus", 9) == 0 ||
         strncmp(cmd, "setPhaser", 9) == 0 ||
         strncmp(cmd, "setFlanger", 10) == 0 ||
         strncmp(cmd, "setCompressor", 13) == 0 ||
         strcmp(cmd, "setDistortion") == 0 ||
         strcmp(cmd, "setBitCrush") == 0 ||
         strcmp(cmd, "setSampleRate") == 0 ||
         strncmp(cmd, "setTrack", 8) == 0 ||
         strncmp(cmd, "sdLoad", 6) == 0 ||
         strcmp(cmd, "sdUnloadKit") == 0 ||
         strcmp(cmd, "sdGetStatus") == 0;
}

void WebInterface::sendUdpStateSync(IPAddress ip, uint16_t port) {
  if (!initialized || ip == IPAddress(0, 0, 0, 0) || port == 0) return;

  PsramJsonDocument doc(4096);
  SdStatusResponse sdStat = {};
  bool sdOk = spiMaster.getCachedSdStatus(sdStat);
  uint32_t sdLoadedMask = sdOk ? sdStat.samplesLoaded : 0;

  doc["cmd"] = "state_sync";
  doc["pattern"] = sequencer.getCurrentPattern();
  doc["playing"] = sequencer.isPlaying();
  doc["tempo"] = sequencer.getTempo();
  doc["step"] = sequencer.getCurrentStep();
  doc["stepCount"] = sequencer.getPatternLength();
  doc["masterVolume"] = spiMaster.getMasterVolume();
  doc["sequencerVolume"] = spiMaster.getSequencerVolume();
  doc["liveVolume"] = spiMaster.getLiveVolume();
  doc["kit"] = sdOk ? String(sdStat.currentKit) : "";
  doc["sdPresent"] = sdOk ? (bool)sdStat.present : false;
  doc["sdLoadedMask"] = sdLoadedMask;

  JsonArray mute = doc.createNestedArray("mute");
  JsonArray solo = doc.createNestedArray("solo");
  JsonArray volumes = doc.createNestedArray("trackVolumes");
  JsonArray trackSynthEngines = doc.createNestedArray("trackSynthEngines");
  for (int track = 0; track < MAX_TRACKS; track++) {
    mute.add(sequencer.isTrackMuted(track) || spiMaster.getTrackMute(track));
    solo.add(spiMaster.getTrackSolo(track));
    volumes.add(sequencer.getTrackVolume(track));
    trackSynthEngines.add((int)gTrackSynthEngine[track]);
  }

  JsonObject fx = doc.createNestedObject("fx");
  fx["filterType"] = gMasterFilterType;
  fx["filterCutoff"] = gMasterFilterCutoff;
  fx["filterResonance"] = gMasterFilterResonance;
  fx["distortion"] = gMasterDistortion;
  fx["bitCrush"] = gMasterBitCrushBits;
  fx["sampleRate"] = gMasterSampleRateReduction;
  fx["delayActive"] = gMasterDelayActive;
  fx["delayMix"] = gMasterDelayMix;
  fx["phaserActive"] = gMasterPhaserActive;
  fx["phaserDepth"] = gMasterPhaserDepth;
  fx["flangerActive"] = gMasterFlangerActive;
  fx["compressorActive"] = gMasterCompressorActive;
  fx["reverbActive"] = spiMaster.isReverbActive();
  fx["reverbMix"] = gMasterReverbMix;
  fx["chorusActive"] = spiMaster.isChorusActive();

  JsonArray trackFilters = fx.createNestedArray("trackFilters");
  JsonArray trackReverbSend = fx.createNestedArray("trackReverbSend");
  JsonArray trackDelaySend = fx.createNestedArray("trackDelaySend");
  JsonArray trackChorusSend = fx.createNestedArray("trackChorusSend");
  JsonArray trackPan = fx.createNestedArray("trackPan");
  JsonArray trackEcho = fx.createNestedArray("trackEcho");
  JsonArray trackFlanger = fx.createNestedArray("trackFlanger");
  JsonArray trackCompressor = fx.createNestedArray("trackCompressor");
  for (int track = 0; track < MAX_TRACKS; track++) {
    trackFilters.add((int)spiMaster.getTrackFilter(track));
    trackReverbSend.add(spiMaster.getTrackReverbSend(track));
    trackDelaySend.add(spiMaster.getTrackDelaySend(track));
    trackChorusSend.add(spiMaster.getTrackChorusSend(track));
    trackPan.add(spiMaster.getTrackPan(track));
    trackEcho.add(spiMaster.getTrackEchoActive(track));
    trackFlanger.add(spiMaster.getTrackFlangerActive(track));
    trackCompressor.add(spiMaster.getTrackCompressorActive(track));
  }

  JsonArray samples = doc.createNestedArray("samples");
  for (int pad = 0; pad < MAX_PADS; pad++) {
    bool loadedLocal = (pad < MAX_SAMPLES) && sampleManager.isSampleLoaded(pad);
    bool loadedDaisy = (sdLoadedMask & (1UL << pad)) != 0;
    if (!loadedLocal && !loadedDaisy) continue;
    JsonObject sample = samples.createNestedObject();
    sample["pad"] = pad;
    sample["loaded"] = true;
    const char* localName = loadedLocal ? sampleManager.getSampleName(pad) : nullptr;
    const char* daisyName = gDaisyPadFiles[pad];
    const char* name = (localName && localName[0]) ? localName : daisyName;
    sample["name"] = (name && name[0]) ? name : "";
  }

  if (!_stateBuf) _stateBuf = (char*)ps_malloc(kStateBufSize);
  if (!_stateBuf) return;
  if (!jsonBufLock()) return;
  size_t len = serializeJson(doc, _stateBuf, kUdpMaxPacketBytes);
  if (len == 0 || len >= kUdpMaxPacketBytes) { jsonBufUnlock(); return; }
  udp.beginPacket(ip, port);
  udp.write((uint8_t*)_stateBuf, len);
  udp.endPacket();
  jsonBufUnlock();

  // v2.9 — also push the current authoritative melody state to this slave so
  // newly-joined or reconnected slaves immediately see the right grid/pad.
  sendMelodySyncTo(ip, port);
}

void WebInterface::broadcastUdpStateSync() {
  if (udpClients.empty()) return;
  for (auto& entry : udpClients) {
    sendUdpStateSync(entry.second.ip, entry.second.port);
    yield();
  }
}

void WebInterface::sendUdpJsonTo(IPAddress ip, uint16_t port, const JsonDocument& doc) {
  if (!initialized || ip == IPAddress(0, 0, 0, 0) || port == 0) return;
  char buf[192];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len == 0 || len >= sizeof(buf)) return;
  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)buf, len);
  udp.endPacket();
}

void WebInterface::broadcastUdpMasterFx(const char* param, bool value) {
  if (udpClients.empty() || !param || !param[0]) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "masterFx";
  doc["param"] = param;
  doc["value"] = value;
  for (auto& entry : udpClients) {
    sendUdpJsonTo(entry.second.ip, entry.second.port, doc);
    yield();
  }
}

void WebInterface::broadcastUdpMasterFx(const char* param, float value) {
  if (udpClients.empty() || !param || !param[0]) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "masterFx";
  doc["param"] = param;
  doc["value"] = value;
  for (auto& entry : udpClients) {
    sendUdpJsonTo(entry.second.ip, entry.second.port, doc);
    yield();
  }
}

void WebInterface::broadcastUdpTrackVolume(int track, int volume) {
  if (udpClients.empty() || track < 0 || track >= 16) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "trackVolumeSet";
  doc["track"] = track;
  doc["volume"] = volume;
  for (auto& entry : udpClients) {
    if (entry.second.ip == s_udpReplyIp) continue;  // skip sender (they get full state_sync)
    sendUdpJsonTo(entry.second.ip, entry.second.port, doc);
    yield();
  }
}

void WebInterface::broadcastUdpSongPattern(int pattern, int songLength) {
  if (udpClients.empty() || pattern < 0 || pattern >= MAX_PATTERNS) return;
  StaticJsonDocument<128> doc;
  doc["type"] = "songPattern";
  doc["pattern"] = pattern;
  doc["songLength"] = songLength;
  for (auto& entry : udpClients) {
    sendUdpJsonTo(entry.second.ip, entry.second.port, doc);
    yield();
  }
}

// v2.9 — Build & send melody_sync packet (engine/octave/rec/step/pad/grid)
// to a single UDP slave. Used both for broadcast and on-hello replies so any
// slave that joins/reconnects immediately sees the authoritative melody state.
void WebInterface::melodyClearGrid() {
  for (int c = 0; c < 16; c++) for (int r = 0; r < 12; r++) melodyGrid[c][r] = false;
}

void WebInterface::sendMelodySyncTo(IPAddress ip, uint16_t port) {
  // Compact stack buffer — grid is 16*12=192 chars + JSON overhead < 700 bytes.
  char buf[800];
  int n = snprintf(buf, sizeof(buf),
                   "{\"cmd\":\"melody_sync\",\"engine\":%u,\"octave\":%u,\"rec\":%d,\"step\":%u,\"pad\":%u,\"grid\":[",
                   (unsigned)melodyEngine, (unsigned)melodyOctave,
                   melodyRecActive ? 1 : 0, (unsigned)melodyStep,
                   (unsigned)melodyPad);
  if (n <= 0 || n >= (int)sizeof(buf)) return;
  for (int c = 0; c < 16; c++) {
    if (n + 2 >= (int)sizeof(buf)) return;
    if (c) buf[n++] = ',';
    buf[n++] = '[';
    for (int r = 0; r < 12; r++) {
      if (n + 2 >= (int)sizeof(buf)) return;
      if (r) buf[n++] = ',';
      buf[n++] = melodyGrid[c][r] ? '1' : '0';
    }
    if (n + 1 >= (int)sizeof(buf)) return;
    buf[n++] = ']';
  }
  if (n + 2 >= (int)sizeof(buf)) return;
  buf[n++] = ']';
  buf[n++] = '}';
  udp.beginPacket(ip, port);
  udp.write((uint8_t*)buf, (size_t)n);
  udp.endPacket();
}

void WebInterface::broadcastMelodySync() {
  if (udpClients.empty()) return;
  static unsigned long lastLog = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastLog > 5000) {
    lastLog = nowMs;
    Serial.printf("[MASTER broadcastMelodySync] clients=%u rec=%d eng=%u oct=%u pad=%u step=%u\n",
                  (unsigned)udpClients.size(), (int)melodyRecActive,
                  melodyEngine, melodyOctave, melodyPad, melodyStep);
  }
  for (auto& entry : udpClients) {
    sendMelodySyncTo(entry.second.ip, entry.second.port);
    yield();
  }
}

/* v2.6 — Push the active pattern (drum steps + selected index) to every
 * known UDP slave. Triggered whenever the active pattern changes via web/UI
 * so the LCD slaves don't show a stale pattern. */
void WebInterface::sendUdpPatternRows(IPAddress ip, uint16_t port, int patternNum) {
  if (ip == IPAddress(0, 0, 0, 0) || port == 0) return;
  if (patternNum < 0 || patternNum >= MAX_PATTERNS) return;
  int stepCount = constrain(sequencer.getPatternLength(), 16, STEPS_PER_PATTERN);
  for (int t = 0; t < MAX_TRACKS; t++) {
    char rowHex[17];
    int nibbles = (stepCount + 3) / 4;
    for (int nib = 0; nib < nibbles; nib++) {
      uint8_t value = 0;
      for (int bit = 0; bit < 4; bit++) {
        int step = nib * 4 + bit;
        if (step < stepCount && sequencer.getStep(patternNum, t, step)) value |= (1U << bit);
      }
      rowHex[nib] = (value < 10) ? ('0' + value) : ('A' + value - 10);
    }
    rowHex[nibbles] = '\0';
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
      "{\"cmd\":\"pattern_row\",\"pattern\":%d,\"track\":%d,\"stepCount\":%d,\"row\":\"%s\"}",
      patternNum, t, stepCount, rowHex);
    if (len > 0 && len < (int)sizeof(buf)) {
      udp.beginPacket(ip, port);
      udp.write((const uint8_t*)buf, len);
      udp.endPacket();
      delay(1);
    }
  }
}

void WebInterface::broadcastUdpPatternSync(int patternNum) {
  if (udpClients.empty()) return;
  for (auto& entry : udpClients) {
    sendUdpPatternRows(entry.second.ip, entry.second.port, patternNum);
    yield();
  }
}

void WebInterface::sendSequencerStateToClient(AsyncWebSocketClient* client) {
  if (!initialized || !ws || !isClientReady(client)) return;
  
  // All memory in PSRAM — zero heap allocation
  if (_stateDoc && _stateBuf) {
    _stateDoc->clear();
    populateStateDocument(*_stateDoc);
    size_t len = serializeJson(*_stateDoc, _stateBuf, kStateBufSize);
    if (len > 0 && len < kStateBufSize) {
      client->text(_stateBuf, len);
    }
  }
}

void WebInterface::broadcastPadTrigger(int pad) {
  if (!initialized || !ws || ws->count() == 0) return;
  if (ESP.getFreeHeap() < 20000) return;
  
  // Stack buffer — zero heap allocation
  char buf[48];
  int len = snprintf(buf, sizeof(buf), "{\"type\":\"pad\",\"pad\":%d}", pad);
  ws->textAll(buf, len);
}

// --- Deferred broadcast flags (written from Core1, consumed by Core0 update) ---
static volatile int _pendingBroadcastStep = -1;     // -1 = nothing pending
static volatile int _pendingSongPattern   = -1;     // -1 = nothing pending
static volatile int _pendingSongLength    = 0;

void WebInterface::broadcastStep(int step) {
  // Called from Core1 (stepChangeCallback) — do NOT touch ws here!
  // Just set the volatile flag; update() on Core0 will do the actual broadcast.
  _pendingBroadcastStep = step;
}

void WebInterface::broadcastSongPattern(int pattern, int songLength) {
  // Called from Core1 (patternChangeCallback) — do NOT touch ws here!
  // Just set the volatile flags; update() on Core0 will do the actual broadcast.
  _pendingSongLength  = songLength;
  _pendingSongPattern = pattern;   // write pattern LAST so consumer sees both
}

// ── Non-blocking pad sample transfer to the Daisy ────────────────────────────
// A large WAV over the 1MHz SPI takes ~20s; doing it in one blocking call reset
// the S3. Decode once into PSRAM, then push a few chunks per update() tick so the
// transfer never blocks this task (mirrors how stems stream).
struct PadXferState {
  bool     active  = false;
  int      pad     = -1;
  int16_t* buf     = nullptr;
  uint32_t total   = 0;     // samples
  uint32_t offset  = 0;     // samples already queued
  bool     begun   = false;
  uint32_t startMs = 0;
};
static PadXferState s_padXfer;

void WebInterface::pumpPadTransfer() {
  if (!s_padXfer.active) return;

  if (!s_padXfer.begun) {
    if (spiMaster.beginSampleStream(s_padXfer.pad, s_padXfer.total)) {
      s_padXfer.begun = true;
    } else if (millis() - s_padXfer.startMs > 4000) {
      s_padXfer.active = false;
      broadcastUploadComplete(s_padXfer.pad, false, "Daisy begin failed");
    }
    return;  // resume next tick
  }

  // Push a few chunks; stop early if the SPI queue is full (resume next tick).
  const uint32_t CH = 256;  // 256 samples = 512 bytes per chunk
  int sent = 0;
  while (s_padXfer.offset < s_padXfer.total && sent < 4) {
    uint32_t left = s_padXfer.total - s_padXfer.offset;
    uint32_t n = (left < CH) ? left : CH;
    if (!spiMaster.writeSampleStreamData(s_padXfer.pad, s_padXfer.buf + s_padXfer.offset,
                                         (uint16_t)n, s_padXfer.offset)) {
      break;  // queue full — try again next tick
    }
    s_padXfer.offset += n;
    sent++;
  }

  if (s_padXfer.offset >= s_padXfer.total) {
    int pad = s_padXfer.pad;
    spiMaster.endSampleStream(pad, true, s_padXfer.total);
    s_padXfer.active = false;
    // The Daisy now holds the sample (64MB SDRAM); free the S3-side copy so
    // PSRAM doesn't accumulate across uploads (was "No PSRAM for sample").
    sampleManager.releaseHostSample(pad);
    setTrackSynthEngine(pad, -1);
    spiMaster.dsqSetTrackEngine((uint8_t)pad, -1);
    spiMaster.dsqSetMute((uint8_t)pad, false);
    broadcastUploadComplete(pad, true, "Sample uploaded and loaded successfully");
    broadcastSequencerState();
  }
}

void WebInterface::update() {
  if (!initialized || !ws || !server) return;

  unsigned long now = millis();

  pumpDaisyUpload();
  pumpCleanTrackStream();
  pumpPadTransfer();

  // ── Deferred sample load (systemTask, Core0) ──────────────────────────────
  // Decode the uploaded WAV into PSRAM, free the raw upload buffer immediately,
  // then hand off to the non-blocking pad transfer pump. Pad WAVs are NOT
  // persisted to LittleFS anymore: that filled the 11MB FS and blocked stem
  // uploads (fs_full), and a large persisted sample could reset the S3 on boot.
  int pendPad = _pendingLoadPad;
  if (pendPad >= 0 && !s_padXfer.active) {
    _pendingLoadPad = -1;  // clear flag first
    esp_task_wdt_reset();

    bool decoded = false;
    if (_uploadBuf && _uploadBufLen > 0) {
      decoded = sampleManager.decodeSampleFromBuffer(_uploadBuf, _uploadBufLen, pendPad);
    }
    // Free the raw WAV now — only the decoded sample stays in PSRAM during the
    // (non-blocking) transfer, halving peak memory.
    if (_uploadBuf) { free(_uploadBuf); _uploadBuf = nullptr; _uploadBufLen = 0; }
    esp_task_wdt_reset();

    if (decoded) {
      s_padXfer = PadXferState{};
      s_padXfer.active  = true;
      s_padXfer.pad     = pendPad;
      s_padXfer.buf     = sampleManager.getSampleBuffer(pendPad);
      s_padXfer.total   = sampleManager.getSampleLength(pendPad);
      s_padXfer.begun   = false;
      s_padXfer.startMs = now;
      if (!s_padXfer.buf || s_padXfer.total == 0) {
        s_padXfer.active = false;
        broadcastUploadComplete(pendPad, false, "Decoded sample empty");
      }
      // success is broadcast by pumpPadTransfer() when the transfer completes
    } else {
      String errDetail = String(sampleManager.getLastParseError());
      String errMsg = errDetail.length() ? "Failed to load: " + errDetail : "Failed to load sample";
      broadcastUploadComplete(pendPad, false, errMsg);
    }
  }

  // ── Boot persistence reload: un pad por tick para no bloquear el event loop ──
  if (_bootReloadCount > 0 && _pendingLoadPad < 0 && !s_daisyUpload.active) {
    int8_t pad = _bootReloadQueue[_bootReloadHead];
    _bootReloadHead  = (uint8_t)((_bootReloadHead + 1) % 16);
    _bootReloadCount--;
    char path[28];
    snprintf(path, sizeof(path), "/samples/pad%d.wav", pad);
    if (LittleFS.exists(path)) {
      esp_task_wdt_reset();
      bool ok = sampleManager.loadSample(path, (int)pad);
      if (ok) {
        setTrackSynthEngine((int)pad, -1);
        spiMaster.dsqSetTrackEngine((uint8_t)pad, -1);
        spiMaster.dsqSetMute((uint8_t)pad, false);
      }
      esp_task_wdt_reset();
    }
  }

  // ── Consume deferred broadcasts from Core1 (thread-safe: only ws access from Core0) ──
  int step = _pendingBroadcastStep;
  if (step >= 0 && ws->count() > 0) {
    _pendingBroadcastStep = -1;
    static unsigned long lastStepBroadcast = 0;
    if (now - lastStepBroadcast >= 80 || step == 0) {
      lastStepBroadcast = now;
      char buf[32];
      int len = snprintf(buf, sizeof(buf), "{\"type\":\"step\",\"step\":%d}", step);
      ws->textAll(buf, len);
    }
  }
  int songPat = _pendingSongPattern;
  if (songPat >= 0 && ws->count() > 0) {
    _pendingSongPattern = -1;
    int songLen = _pendingSongLength;
    char buf[80];
    int len = snprintf(buf, sizeof(buf),
      "{\"type\":\"songPattern\",\"pattern\":%d,\"songLength\":%d}",
      songPat, songLen);
    ws->textAll(buf, len);
    broadcastUdpSongPattern(songPat, songLen);
  } else if (songPat >= 0) {
    _pendingSongPattern = -1;
    broadcastUdpSongPattern(songPat, _pendingSongLength);
  }

  // Skip periodic broadcasts during page transitions (2s window)
  bool pageLoading = (pageTransitionMs != 0 && (now - pageTransitionMs) < 2000);
  if (pageTransitionMs != 0 && (now - pageTransitionMs) >= 2000) {
    pageTransitionMs = 0;  // clear flag
  }
  
  // Broadcast audio levels for all WS clients (main UI + /adm)
  static unsigned long lastAudioLevels = 0;
  if (!pageLoading && now - lastAudioLevels >= 150 && ws->count() > 0) {
    lastAudioLevels = now;

    // Peak data is polled by SPIMaster::process() on Core1 —
    // we just read the cached values here (zero SPI blocking).

    if (ESP.getFreeHeap() >= 20000) {
      uint8_t levelBuf[18];
      levelBuf[0] = 0xAA;

      float peaks[16];
      spiMaster.getTrackPeaks(peaks, 16);
      for (int i = 0; i < 16; i++) {
        float p = peaks[i];
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        levelBuf[i + 1] = (uint8_t)(p * 255.0f);
      }
      float mp = spiMaster.getMasterPeak();
      if (mp < 0.0f) mp = 0.0f;
      if (mp > 1.0f) mp = 1.0f;
      levelBuf[17] = (uint8_t)(mp * 255.0f);

      ws->binaryAll(levelBuf, 18);
    }
  }
  
  // Limpiar WebSocket clients desconectados cada 2 segundos
  // Limpiar WebSocket clients desconectados cada segundo
  static unsigned long lastWsCleanup = 0;
  if (now - lastWsCleanup > 1000) {
    ws->cleanupClients(3);  // máx 3 clientes, cerrar el resto
    lastWsCleanup = now;
  }

  // Broadcast estado periódico (15s) — red de seguridad para sample info.
  // Reducido de 5s a 15s para minimizar heap churn (~13KB por llamada).
  static unsigned long lastPeriodicState = 0;
  if (!pageLoading && ws->count() > 0 && now - lastPeriodicState >= 15000) {
    lastPeriodicState = now;
    broadcastSequencerState();
  }

  static unsigned long lastUdpStateSync = 0;
  if (!pageLoading && !udpClients.empty() && now - lastUdpStateSync >= 2000) {
    lastUdpStateSync = now;
    broadcastUdpStateSync();
  }

  // v2.9 — periodic melody_sync so newly-joined slaves and any missed packet recover
  static unsigned long lastUdpMelodySync = 0;
  if (!pageLoading && !udpClients.empty() && now - lastUdpMelodySync >= 3000) {
    lastUdpMelodySync = now;
    broadcastMelodySync();
  }

  // ── Heap monitor: log cada 10s para diagnosticar fugas ──
  static unsigned long lastHeapLog = 0;
  static uint32_t minHeapSeen = 0xFFFFFFFF;
  static uint8_t lowHeapStrikes = 0;
  if (now - lastHeapLog > 10000) {
    lastHeapLog = now;
    uint32_t h = ESP.getFreeHeap();
    size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t psramFree = ESP.getFreePsram();
    if (h < minHeapSeen) minHeapSeen = h;
    
    syslog("HEAP", "free=%u min=%u block=%u psram=%u ws=%d",
           h, minHeapSeen, (uint32_t)maxBlock, psramFree, ws ? ws->count() : 0);
    
    // Warn web clients when heap is getting low
    if (h < 30000 && ws && ws->count() > 0) {
      char warnBuf[80];
      int wlen = snprintf(warnBuf, sizeof(warnBuf),
        "{\"type\":\"warning\",\"msg\":\"low_heap\",\"heap\":%u,\"block\":%u}",
        h, (uint32_t)maxBlock);
      ws->textAll(warnBuf, wlen);
    }
    
    // No reiniciar automáticamente aquí: en carga web pesada el heap puede
    // fragmentarse temporalmente y provocar bucles de reboot aunque el equipo
    // siga operativo. Dejamos aviso persistente para diagnóstico.
    if (maxBlock < 12000) {
      lowHeapStrikes++;
      syslog("HEAP", "WARNING low block=%u strike=%d/3", (uint32_t)maxBlock, lowHeapStrikes);
      if (lowHeapStrikes >= 3) {
        syslog("HEAP", "CRITICAL sustained low heap block=%u; auto-restart disabled", (uint32_t)maxBlock);
        lowHeapStrikes = 0;
      }
    } else {
      lowHeapStrikes = 0;
    }
  }
  
  // Limpiar clientes UDP inactivos cada 30 segundos
  static unsigned long lastCleanup = 0;
  if (now - lastCleanup > 30000) {
    cleanupStaleUdpClients();
    lastCleanup = now;
  }
  
  // WiFi health check cada 30 segundos (no-blocking)
  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck > 30000) {
    lastWifiCheck = now;

    if (_staConnected) {
      // ── STA+AP: check STA still connected, recover if dropped ──
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
      }
      // AP should stay alive automatically in AP_STA mode
    } else {
      // ── AP-only: verify AP is active ──
      if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("RED808", "red808esp32", 1, 0, 4);
        WiFi.setSleep(false);
      }
    }
  }
}

String WebInterface::getIP() {
  if (_staConnected && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return WiFi.softAPIP().toString();
}

// Procesar comandos JSON (compartido entre WebSocket y UDP)
void WebInterface::processCommand(const JsonDocument& doc) {
  // ── Heap guard: si queda poca memoria, descartamos el comando ──
  if (ESP.getFreeHeap() < 20000) {
    syslog("CMD", "DROPPED cmd heap=%u", ESP.getFreeHeap());
    // Avisar a la UI: comandos request/response (getPattern, etc.) quedaban
    // colgados sin respuesta y la web parecia congelada. String estatico,
    // sin allocs intermedios significativos.
    if (ws && ws->count() > 0) {
      ws->textAll("{\"type\":\"error\",\"msg\":\"low_heap\"}");
    }
    return;
  }

  String cmd = doc["cmd"];

  static unsigned long lastMasterFxCmdMs = 0;
  static unsigned long lastTrackFxCmdMs[24] = {};
  static unsigned long lastPadFxCmdMs = 0;
  static unsigned long lastVolumeCmdMs = 0;
  const unsigned long nowCmdMs = millis();

  bool isMasterFxFast =
    (cmd == "setFilter") ||
    (cmd == "setFilterCutoff") ||
    (cmd == "setFilterResonance") ||
    (cmd == "setBitCrush") ||
    (cmd == "setDistortion") ||
    (cmd == "setSampleRate") ||
    (cmd == "setDelayTime") ||
    (cmd == "setDelayFeedback") ||
    (cmd == "setDelayMix") ||
    (cmd == "setPhaserRate") ||
    (cmd == "setPhaserDepth") ||
    (cmd == "setPhaserFeedback") ||
    (cmd == "setFlangerRate") ||
    (cmd == "setFlangerDepth") ||
    (cmd == "setFlangerFeedback") ||
    (cmd == "setFlangerMix") ||
    (cmd == "setCompressorThreshold") ||
    (cmd == "setCompressorRatio") ||
    (cmd == "setCompressorAttack") ||
    (cmd == "setCompressorRelease") ||
    (cmd == "setCompressorMakeupGain");

  bool isTrackFxFast =
    (cmd == "setTrackFilter") ||
    (cmd == "setTrackDistortion") ||
    (cmd == "setTrackBitCrush") ||
    (cmd == "setTrackEcho") ||
    (cmd == "setTrackFlanger") ||
    (cmd == "setTrackCompressor");

  bool isPadFxFast =
    (cmd == "setPadFilter") ||
    (cmd == "setPadDistortion") ||
    (cmd == "setPadBitCrush");

  bool isVolumeFast =
    (cmd == "setSequencerVolume") ||
    (cmd == "setLiveVolume") ||
    (cmd == "setVolume") ||
    (cmd == "setLivePitch");

  if (isMasterFxFast) {
    if (nowCmdMs - lastMasterFxCmdMs < kFastMasterCmdMinMs) return;
    lastMasterFxCmdMs = nowCmdMs;
  } else if (isTrackFxFast) {
    int tIdx = doc.containsKey("track") ? (int)doc["track"] : -1;
    if (tIdx >= 0 && tIdx < 24) {
      if (nowCmdMs - lastTrackFxCmdMs[tIdx] < kFastTrackCmdMinMs) return;
      lastTrackFxCmdMs[tIdx] = nowCmdMs;
    }
  } else if (isPadFxFast) {
    if (nowCmdMs - lastPadFxCmdMs < kFastPadCmdMinMs) return;
    lastPadFxCmdMs = nowCmdMs;
  } else if (isVolumeFast) {
    if (nowCmdMs - lastVolumeCmdMs < kFastVolumeCmdMinMs) return;
    lastVolumeCmdMs = nowCmdMs;
  }
  
  if (cmd == "hello" || cmd == "get_state" || cmd == "getState") {
    return;
  }
  else if (cmd == "trigger") {
    int pad = doc["pad"];
    if (pad < 0 || pad >= 24) return;  // 16 sequencer + 8 XTRA
    int velocity = doc.containsKey("vel") ? doc["vel"].as<int>() : 127;
    triggerPadWithLED(pad, velocity);
    broadcastPadTrigger(pad);
  }
  else if (cmd == "setStep") {
    int track = doc["track"];
    int step = doc["step"];
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;
    bool active = doc["active"];
    bool silent = doc.containsKey("silent") && doc["silent"].as<bool>();
    uint8_t noteLen = doc.containsKey("noteLen") ? (uint8_t)doc["noteLen"].as<int>() : 1;
    if (noteLen == 0 || (noteLen != 1 && noteLen != 2 && noteLen != 4 && noteLen != 8)) noteLen = 1;
    // Support writing to a specific pattern (for multi-pattern MIDI import)
    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        int savedPattern = sequencer.getCurrentPattern();
        sequencer.selectPattern(pattern);
        sequencer.setStep(track, step, active);
        spiMaster.dsqSetStep((uint8_t)pattern, (uint8_t)track, (uint8_t)step,
            active ? 1 : 0, 100, 1, 100);
        sequencer.selectPattern(savedPattern);
        yield(); // Prevent watchdog reset during bulk import
      }
    } else {
      sequencer.setStep(track, step, active);
      sequencer.setStepNoteLen(track, step, noteLen);
      spiMaster.dsqSetStep((uint8_t)sequencer.getCurrentPattern(), (uint8_t)track, (uint8_t)step,
          active ? 1 : 0,
          sequencer.getStepVelocity(sequencer.getCurrentPattern(), track, step),
          noteLen,
          sequencer.getStepProbability(sequencer.getCurrentPattern(), track, step));
      // Only broadcast if not in silent/bulk mode
      if (!silent) {
        StaticJsonDocument<160> resp;
        resp["type"] = "stepSet";
        resp["track"] = track;
        resp["step"] = step;
        resp["active"] = active;
        resp["noteLen"] = noteLen;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
      yield();
    }
  }
  else if (cmd == "start") {
    sequencer.start();
    // Upload + select + play en orden garantizado (Core1 procesa todo seguido).
    // Necesario tras cargar MIDI, editar pasos en bulk, o cambiar engines.
    dsqUploadAndPlayDeferred(sequencer.getCurrentPattern());
    spiMaster.requestStatus();
    syncCleanTrackStateFromDaisy();
    StaticJsonDocument<96> resp;
    resp["type"] = "playState";
    resp["playing"] = true;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "stop") {
    sequencer.stop();
    spiMaster.dsqControl(0);
    spiMaster.requestStatus();
    syncCleanTrackStateFromDaisy();
    releaseSequencerMelodicHolds();
    StaticJsonDocument<96> resp;
    resp["type"] = "playState";
    resp["playing"] = false;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCleanTrackActive") {
    int track = doc["track"] | -1;
    bool active = doc["active"] | false;
    if (track >= 0 && track < kCleanTrackCount && s_cleanTracks[track].occupied) {
      s_cleanTracks[track].armed = active;
      spiMaster.setCleanTrackActive(track, active);
      saveCleanTracksStateToFs();
      spiMaster.requestStatus();
      syncCleanTrackStateFromDaisy();
      broadcastSequencerState();
    }
  }
  else if (cmd == "setCleanTrackMute") {
    int track = doc["track"] | -1;
    bool muted = doc["muted"] | false;
    if (track >= 0 && track < kCleanTrackCount && s_cleanTracks[track].occupied) {
      s_cleanTracks[track].muted = muted;
      spiMaster.setCleanTrackMute(track, muted);
      saveCleanTracksStateToFs();
      spiMaster.requestStatus();
      syncCleanTrackStateFromDaisy();
      broadcastSequencerState();
    }
  }
  else if (cmd == "clearPattern") {
    int pattern = doc.containsKey("pattern") ? doc["pattern"].as<int>() : sequencer.getCurrentPattern();
    sequencer.clearPattern(pattern);
    yield();
    char buf[64];
    int blen = snprintf(buf, sizeof(buf), "{\"type\":\"patternCleared\",\"pattern\":%d}", pattern);
    if (ws && ws->count() > 0) ws->textAll(buf, blen);
  }
  else if (cmd == "clearPatterns") {
    // Bulk clear: {cmd:"clearPatterns", from:0, to:99}
    int from = doc.containsKey("from") ? doc["from"].as<int>() : 0;
    int to   = doc.containsKey("to")   ? doc["to"].as<int>()   : from;
    if (from < 0) from = 0;
    if (to >= MAX_PATTERNS) to = MAX_PATTERNS - 1;
    syslog("CMD", "clearPatterns %d-%d heap=%u", from, to, ESP.getFreeHeap());
    for (int p = from; p <= to; p++) {
      sequencer.clearPattern(p);
      if ((p & 3) == 3) yield();   // feed WDT every 4 patterns
    }
    yield();
    char buf[80];
    int blen = snprintf(buf, sizeof(buf),
      "{\"type\":\"patternsCleared\",\"from\":%d,\"to\":%d}", from, to);
    if (ws && ws->count() > 0) ws->textAll(buf, blen);
    syslog("CMD", "clearPatterns DONE heap=%u", ESP.getFreeHeap());
  }
  else if (cmd == "setSongMode") {
    bool enabled = doc["enabled"];
    int length = doc.containsKey("length") ? doc["length"].as<int>() : 1;
    sequencer.setSongLength(length);
    sequencer.setSongMode(enabled);
    broadcastSequencerState();
  }
  else if (cmd == "tempo") {
    float tempo = doc["value"];
    sequencer.setTempo(tempo);
    spiMaster.setTempo(tempo);
    StaticJsonDocument<96> resp;
    resp["type"] = "tempoChange";
    resp["tempo"] = tempo;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setStepCount") {
    int count = doc["count"];
    if (count == 16 || count == 32 || count == 64) {
      sequencer.setPatternLength(count);
      spiMaster.dsqSetLength((uint8_t)count);
      StaticJsonDocument<96> resp;
      resp["type"] = "stepCount";
      resp["count"] = count;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "selectPattern") {
    // No encadenar `doc["a"] | doc["b"] | default`: el primer `|` de
    // ArduinoJson ya resuelve a int y el resto es OR bitwise (p.ej.
    // {"index":0,"pattern":5} daba 0|5=5). Resolver presencia explicitamente.
    int pattern;
    if (!doc["index"].isNull())        pattern = doc["index"].as<int>();
    else if (!doc["pattern"].isNull()) pattern = doc["pattern"].as<int>();
    else                               pattern = sequencer.getCurrentPattern();
    syslog("CMD", "selPat idx=%d heap=%u", pattern, ESP.getFreeHeap());
    sequencer.selectPattern(pattern);
    // Reliability first: refresh Daisy slot on selection to avoid silent
    // patterns when a previous upload did not fully commit.
    dsqUploadPatternDeferred(pattern);
    /* v2.6 — Push to UDP slaves so LCD pattern display always matches master */
    broadcastUdpPatternSync(pattern);
    if (s_udpReplyIp != IPAddress(0, 0, 0, 0) && s_udpReplyPort != 0) {
      // Also reply directly to the requester. This avoids the P4 timeout when
      // the UDP client map is still empty/stale just after a Master reboot.
      sendUdpPatternRows(s_udpReplyIp, s_udpReplyPort, pattern);
    }
    
    // broadcastSequencerState + pattern JSON — no SPI blocking now
    if (ESP.getFreeHeap() > 50000) {
      broadcastSequencerState();
      yield();
    } else if (ESP.getFreeHeap() > 35000) {
      StaticJsonDocument<128> minState;
      minState["type"] = "patternSelected";
      minState["pattern"] = pattern;
      String ms; serializeJson(minState, ms);
      if (ws) ws->textAll(ms);
    }
    syslog("CMD", "selPat bcast done heap=%u", ESP.getFreeHeap());
    
    if (ESP.getFreeHeap() < 30000) {
      syslog("CMD", "selPat SKIP pattern JSON (low heap)");
      return;
    }
    yield();
    int stepCount = sequencer.getPatternLength();
    PsramJsonDocument patternDoc(stepCount > 16 ? 32768 : 14336);
    patternDoc["type"] = "pattern";
    patternDoc["index"] = pattern;
    patternDoc["stepCount"] = stepCount;
    
    for (int track = 0; track < 16; track++) {
      JsonArray trackSteps = patternDoc.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        trackSteps.add(sequencer.getStep(track, step));
      }
    }
    
    JsonObject velocitiesObj = patternDoc.createNestedObject("velocities");
    for (int track = 0; track < 16; track++) {
      JsonArray trackVels = velocitiesObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        trackVels.add(sequencer.getStepVelocity(track, step));
      }
    }

    JsonObject volumeLocksObj = patternDoc.createNestedObject("volumeLocks");
    for (int track = 0; track < 16; track++) {
      JsonArray trackLocks = volumeLocksObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        if (sequencer.hasStepVolumeLock(track, step)) {
          trackLocks.add(sequencer.getStepVolumeLock(track, step));
        } else {
          trackLocks.add(-1);
        }
      }
    }

    JsonObject probabilitiesObj = patternDoc.createNestedObject("probabilities");
    for (int track = 0; track < 16; track++) {
      JsonArray trackProb = probabilitiesObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        trackProb.add(sequencer.getStepProbability(track, step));
      }
    }

    JsonObject ratchetsObj = patternDoc.createNestedObject("ratchets");
    for (int track = 0; track < 16; track++) {
      JsonArray trackRat = ratchetsObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        trackRat.add(sequencer.getStepRatchet(track, step));
      }
    }

    JsonObject cutoffLocksObj = patternDoc.createNestedObject("cutoffLocks");
    for (int track = 0; track < 16; track++) {
      JsonArray trackLocks = cutoffLocksObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        if (sequencer.hasStepCutoffLock(track, step)) {
          trackLocks.add((int)sequencer.getStepCutoffLock(track, step));
        } else {
          trackLocks.add(-1);
        }
      }
    }

    JsonObject reverbLocksObj = patternDoc.createNestedObject("reverbLocks");
    for (int track = 0; track < 16; track++) {
      JsonArray trackLocks = reverbLocksObj.createNestedArray(String(track));
      for (int step = 0; step < stepCount; step++) {
        if (sequencer.hasStepReverbSendLock(track, step)) {
          trackLocks.add((int)sequencer.getStepReverbSendLock(track, step));
        } else {
          trackLocks.add(-1);
        }
      }
    }
    
    // Serialize to PSRAM buffer to avoid String heap fragmentation
    if (!_patternBuf) _patternBuf = (char*)ps_malloc(kPatternBufSize);
    if (_patternBuf && jsonBufLock()) {
      size_t len = serializeJson(patternDoc, _patternBuf, kPatternBufSize);
      syslog("CMD", "selPat JSON len=%u heap=%u", (unsigned)len, ESP.getFreeHeap());
      if (len > 0 && len < kPatternBufSize && ws && ws->count() > 0) {
        ws->textAll(_patternBuf, len);
      }
      jsonBufUnlock();
    }
    syslog("CMD", "selPat DONE heap=%u", ESP.getFreeHeap());
  }
  else if (cmd == "loadSample") {
    const char* family   = doc["family"];
    const char* filename = doc["filename"];
    int padIndex = doc["pad"];
    if (padIndex < 0 || padIndex >= 16) return;
    if (!family || !filename) return;
    syslog("CMD", "loadSample pad=%d %s/%s heap=%u", padIndex, family, filename, ESP.getFreeHeap());

    yield();

    // Build LittleFS path: /<family>/<filename>
    // We transfer the audio data directly from ESP32 LittleFS → Daisy RAM via SPI.
    // This does NOT touch the Daisy QSPI flash, so the original boot sample is
    // preserved and reloads on reboot ("sin machacar el antiguo").
    String fullPath = String("/") + String(family) + "/" + String(filename);
    bool ok = sampleManager.loadSample(fullPath.c_str(), padIndex);

    if (ok) {
      // Apply trim markers if the user dragged the waveform start/end
      float trimStart = doc.containsKey("trimStart") ? (float)doc["trimStart"] : 0.0f;
      float trimEnd   = doc.containsKey("trimEnd")   ? (float)doc["trimEnd"]   : 1.0f;
      if (trimStart > 0.001f || trimEnd < 0.999f) {
        sampleManager.trimSample(padIndex, trimStart, trimEnd);
      }

      uint32_t sampleLen = sampleManager.getSampleLength(padIndex);

      StaticJsonDocument<256> responseDoc;
      responseDoc["type"]     = "sampleLoaded";
      responseDoc["pad"]      = padIndex;
      responseDoc["filename"] = filename;
      responseDoc["size"]     = sampleLen * 2;           // bytes
      responseDoc["format"]   = detectSampleFormat(filename);
      responseDoc["quality"]  = "LittleFS";

      String output;
      serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    }
  }
  // === Trim already-loaded sample ===
  else if (cmd == "trimSample") {
    int padIndex = doc["pad"];
    float trimStart = doc.containsKey("trimStart") ? (float)doc["trimStart"] : 0.0f;
    float trimEnd = doc.containsKey("trimEnd") ? (float)doc["trimEnd"] : 1.0f;
    if (padIndex >= 0 && padIndex < MAX_SAMPLES && sampleManager.isSampleLoaded(padIndex)) {
      if (sampleManager.trimSample(padIndex, trimStart, trimEnd)) {
        StaticJsonDocument<256> responseDoc;
        responseDoc["type"] = "sampleTrimmed";
        responseDoc["pad"] = padIndex;
        responseDoc["size"] = sampleManager.getSampleLength(padIndex) * 2;
        responseDoc["samples"] = sampleManager.getSampleLength(padIndex);
        String output;
        serializeJson(responseDoc, output);
        if (ws) ws->textAll(output);
      }
    }
  }
  // === XTRA PADS: list samples from /xtra folder ===
  else if (cmd == "getXtraSamples") {
    StaticJsonDocument<2048> responseDoc;
    responseDoc["type"] = "xtraSampleList";
    
    File dir = LittleFS.open("/xtra", "r");
    if (dir && dir.isDirectory()) {
      JsonArray samples = responseDoc.createNestedArray("samples");
      File file = dir.openNextFile();
      int count = 0;
      while (file) {
        if (!file.isDirectory()) {
          String filename = file.name();
          int lastSlash = filename.lastIndexOf('/');
          if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
          
          if (isSupportedSampleFile(filename)) {
            JsonObject sampleObj = samples.createNestedObject();
            sampleObj["name"] = filename;
            sampleObj["size"] = file.size();
            count++;
            if (count % 3 == 0) yield();
          }
        }
        file.close();
        file = dir.openNextFile();
      }
      dir.close();
    } else {
      // Create /xtra if it doesn't exist
      LittleFS.mkdir("/xtra");
      responseDoc.createNestedArray("samples");
    }
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  // === XTRA PADS: load sample from /xtra to a pad ===
  else if (cmd == "loadXtraSample") {
    const char* filename = doc["filename"];
    int padIndex = doc["pad"];
    if (padIndex < 16 || padIndex >= 24) return;

    String fullPath = String("/xtra/") + String(filename);
    yield();

    // Notificar al cliente que empieza la transferencia SPI a la Daisy
    {
      StaticJsonDocument<128> tDoc;
      tDoc["type"] = "xtraTransferring";
      tDoc["pad"]  = padIndex;
      String tOut; serializeJson(tDoc, tOut);
      if (ws) ws->textAll(tOut);
    }

    if (sampleManager.loadSample(fullPath.c_str(), padIndex)) {
      // Transferencia completada (ya incluye delay post-CMD_SAMPLE_END)
      StaticJsonDocument<256> responseDoc;
      responseDoc["type"]     = "xtraReady";
      responseDoc["pad"]      = padIndex;
      responseDoc["filename"] = filename;
      responseDoc["size"]     = sampleManager.getSampleLength(padIndex) * 2;
      String output; serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);

      // También enviamos sampleLoaded para compatibilidad con waveform cache etc.
      responseDoc["type"] = "sampleLoaded";
      output = ""; serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "mute") {
    int track = doc["track"];
    if (track < 0 || track >= 16) return;
    yield();
    bool muted = doc["value"];
    sequencer.muteTrack(track, muted);
    spiMaster.setTrackMute(track, muted);
    spiMaster.dsqSetMute((uint8_t)track, muted);
    StaticJsonDocument<128> muteDoc;
    muteDoc["type"] = "trackMuted";
    muteDoc["track"] = track;
    muteDoc["muted"] = muted;
    String muteOutput;
    serializeJson(muteDoc, muteOutput);
    if (ws) ws->textAll(muteOutput);
  }
  else if (cmd == "solo") {
    int track = doc["track"];
    if (track < 0 || track >= 16) return;
    bool solo = doc["value"];
    spiMaster.setTrackSolo(track, solo);
    StaticJsonDocument<128> soloDoc;
    soloDoc["type"] = "trackSolo";
    soloDoc["track"] = track;
    soloDoc["solo"] = solo;
    String soloOutput;
    serializeJson(soloDoc, soloOutput);
    if (ws) ws->textAll(soloOutput);
  }
  else if (cmd == "setMuteMask" || cmd == "setSoloMask") {
    // Atomic update of all 16 track mute/solo states in one shot, to avoid
    // the per-track UDP flood that causes flicker when many packets are
    // dropped/reordered. Accepts either a 16-bit "mask" integer or a 16-bool
    // "values" array.
    bool isSolo = (cmd == "setSoloMask");
    uint32_t mask = 0;
    bool haveMask = false;
    if (doc.containsKey("mask")) {
      mask = (uint32_t)(doc["mask"].as<uint32_t>() & 0xFFFFu);
      haveMask = true;
    } else if (doc["values"].is<JsonArrayConst>()) {
      JsonArrayConst arr = doc["values"].as<JsonArrayConst>();
      int t = 0;
      for (JsonVariantConst v : arr) {
        if (t >= 16) break;
        if (v.as<bool>()) mask |= (1u << t);
        t++;
      }
      haveMask = true;
    }
    if (!haveMask) return;
    for (int t = 0; t < 16; t++) {
      bool on = (mask >> t) & 1u;
      if (isSolo) {
        spiMaster.setTrackSolo(t, on);
      } else {
        sequencer.muteTrack(t, on);
        spiMaster.setTrackMute(t, on);
        spiMaster.dsqSetMute((uint8_t)t, on);
      }
    }
    // Broadcast a single WS update for the whole mask
    StaticJsonDocument<256> resp;
    resp["type"] = isSolo ? "trackSoloMask" : "trackMuteMask";
    resp["mask"] = mask;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "toggleLoop") {
    int track = doc["track"];
    if (track < 0 || track >= 24) return;
    yield();
    
    if (track >= 16) {
      // XTRA pads (16-23): continuous audio loop via SPIMaster → STM32
      bool newState = !spiMaster.isPadLooping(track);
      spiMaster.setPadLoop(track, newState);
      // If enabling loop, also trigger the sample so it starts playing
      if (newState) {
        spiMaster.triggerSampleLive(track, 127);
      }
      
      StaticJsonDocument<192> responseDoc;
      responseDoc["type"] = "loopState";
      responseDoc["track"] = track;
      responseDoc["active"] = newState;
      responseDoc["paused"] = false;
      responseDoc["loopType"] = 0;
      
      String output;
      serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    } else {
      // Sequencer tracks (0-15): step-based loop via Sequencer
      if (doc.containsKey("loopType")) {
        int lt = doc["loopType"];
        sequencer.setLoopType(track, (LoopType)constrain(lt, 0, 3));
      }
      
      sequencer.toggleLoop(track);
      bool loopNow = sequencer.isLooping(track);

      // For sequencer tracks, loop means retriggering on sequencer steps.
      // Do not arm Daisy's continuous pad loop here or long stems will
      // restart independently of the visible step triggers.
      spiMaster.setPadLoop(track, false);
      
      StaticJsonDocument<192> responseDoc;
      responseDoc["type"] = "loopState";
      responseDoc["track"] = track;
      responseDoc["active"] = loopNow;
      responseDoc["paused"] = sequencer.isLoopPaused(track);
      responseDoc["loopType"] = (int)sequencer.getLoopType(track);
      
      String output;
      serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    }
    yield();
  }
  else if (cmd == "setLoopType") {
    int track = doc["track"];
    int lt = doc["loopType"];
    if (track < 0 || track >= 16) return;
    yield();
    sequencer.setLoopType(track, (LoopType)constrain(lt, 0, 3));
    
    StaticJsonDocument<192> responseDoc;
    responseDoc["type"] = "loopState";
    responseDoc["track"] = track;
    responseDoc["active"] = sequencer.isLooping(track);
    responseDoc["paused"] = sequencer.isLoopPaused(track);
    responseDoc["loopType"] = lt;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
    yield();
  }
  else if (cmd == "pauseLoop") {
    int track = doc["track"];
    if (track < 0 || track >= 16) return;
    sequencer.pauseLoop(track);
    
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "loopState";
    responseDoc["track"] = track;
    responseDoc["active"] = sequencer.isLooping(track);
    responseDoc["paused"] = sequencer.isLoopPaused(track);
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setLedMonoMode") {
    bool monoMode = doc["value"];
    setLedMonoMode(monoMode);
    StaticJsonDocument<96> resp;
    resp["type"] = "ledMode"; resp["mono"] = monoMode;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFilter") {
    int type = doc["type"];
    gMasterFilterType = type;
    spiMaster.setFilterType((FilterType)type);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "filterType"; resp["value"] = type;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFilterCutoff") {
    float cutoff = doc["value"];
    gMasterFilterCutoff = cutoff;
    spiMaster.setFilterCutoff(cutoff);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "filterCutoff"; resp["value"] = cutoff;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFilterResonance") {
    float resonance = doc["value"];
    gMasterFilterResonance = resonance;
    spiMaster.setFilterResonance(resonance);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "filterResonance"; resp["value"] = resonance;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setBitCrush") {
    int bits = doc["value"];
    gMasterBitCrushBits = bits;
    spiMaster.setBitDepth(bits);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "bitCrush"; resp["value"] = bits;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("bitCrush", (float)bits);
    broadcastUdpStateSync();
  }
  else if (cmd == "setDistortion") {
    float amount = doc["value"];
    gMasterDistortion = amount;
    spiMaster.setDistortion(amount);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "distortion"; resp["value"] = amount;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("distortion", amount);
    broadcastUdpStateSync();
  }
  else if (cmd == "setDistortionMode") {
    int mode = doc["value"];
    spiMaster.setDistortionMode((DistortionMode)mode);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "distortionMode"; resp["value"] = mode;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setSampleRate") {
    int rate = doc["value"];
    gMasterSampleRateReduction = rate;
    spiMaster.setSampleRateReduction(rate);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "sampleRate"; resp["value"] = rate;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("sampleRate", (float)rate);
    broadcastUdpStateSync();
  }
  // ============= NEW: Master Effects Commands =============
  else if (cmd == "setDelayActive") {
    bool active = doc["value"];
    gMasterDelayActive = active;
    spiMaster.setDelayActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "delayActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("delayActive", active);
    broadcastUdpStateSync();
  }
  else if (cmd == "setDelayTime") {
    float ms = doc["value"];
    spiMaster.setDelayTime(ms);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "delayTime"; resp["value"] = ms;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setDelayFeedback") {
    float fb = doc["value"];
    spiMaster.setDelayFeedback(fb / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "delayFeedback"; resp["value"] = fb;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setDelayMix") {
    float mix = doc["value"];
    gMasterDelayMix = mix;
    spiMaster.setDelayMix(mix / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "delayMix"; resp["value"] = mix;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("delayMix", mix);
    broadcastUdpStateSync();
  }
  else if (cmd == "setPhaserActive") {
    bool active = doc["value"];
    gMasterPhaserActive = active;
    spiMaster.setPhaserActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "phaserActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("phaserActive", active);
    broadcastUdpStateSync();
  }
  else if (cmd == "setPhaserRate") {
    float rate = doc["value"];
    spiMaster.setPhaserRate(rate / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "phaserRate"; resp["value"] = rate;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setPhaserDepth") {
    float depth = doc["value"];
    gMasterPhaserDepth = depth;
    spiMaster.setPhaserDepth(depth / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "phaserDepth"; resp["value"] = depth;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("phaserDepth", depth);
    broadcastUdpStateSync();
  }
  else if (cmd == "setPhaserFeedback") {
    float fb = doc["value"];
    spiMaster.setPhaserFeedback(fb / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "phaserFeedback"; resp["value"] = fb;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFlangerActive") {
    bool active = doc["value"];
    gMasterFlangerActive = active;
    spiMaster.setFlangerActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "flangerActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFlangerRate") {
    float rate = doc["value"];
    spiMaster.setFlangerRate(rate / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "flangerRate"; resp["value"] = rate;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFlangerDepth") {
    float depth = doc["value"];
    spiMaster.setFlangerDepth(depth / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "flangerDepth"; resp["value"] = depth;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFlangerFeedback") {
    float fb = doc["value"];
    spiMaster.setFlangerFeedback(fb / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "flangerFeedback"; resp["value"] = fb;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setFlangerMix") {
    float mix = doc["value"];
    spiMaster.setFlangerMix(mix / 100.0f);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "flangerMix"; resp["value"] = mix;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorActive") {
    bool active = doc["value"];
    gMasterCompressorActive = active;
    spiMaster.setCompressorActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorThreshold") {
    float thresh = doc["value"];
    spiMaster.setCompressorThreshold(thresh);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorThreshold"; resp["value"] = thresh;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorRatio") {
    float ratio = doc["value"];
    spiMaster.setCompressorRatio(ratio);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorRatio"; resp["value"] = ratio;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorAttack") {
    float attack = doc["value"];
    spiMaster.setCompressorAttack(attack);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorAttack"; resp["value"] = attack;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorRelease") {
    float release = doc["value"];
    spiMaster.setCompressorRelease(release);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorRelease"; resp["value"] = release;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setCompressorMakeupGain") {
    float gain = doc["value"];
    spiMaster.setCompressorMakeupGain(gain);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "compressorMakeupGain"; resp["value"] = gain;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setReverbActive") {
    bool active = doc["value"];
    spiMaster.setReverbActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "reverbActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("reverbActive", active);
    broadcastUdpStateSync();
  }
  else if (cmd == "setReverbFeedback") {
    float v = doc["value"];
    float feedback = (v > 1.0f) ? (v / 100.0f) : v;
    spiMaster.setReverbFeedback(feedback);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "reverbFeedback"; resp["value"] = feedback;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setReverbLpFreq") {
    float hz = doc["value"];
    spiMaster.setReverbLpFreq(hz);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "reverbLpFreq"; resp["value"] = hz;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setReverbMix") {
    float v = doc["value"];
    float mix = (v > 1.0f) ? (v / 100.0f) : v;
    gMasterReverbMix = mix;
    spiMaster.setReverbMix(mix);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "reverbMix"; resp["value"] = mix;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
    broadcastUdpMasterFx("reverbMix", mix);
    broadcastUdpStateSync();
  }
  else if (cmd == "setChorusActive") {
    bool active = doc["value"];
    spiMaster.setChorusActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "chorusActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setChorusRate") {
    float rate = doc["value"];
    spiMaster.setChorusRate(rate);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "chorusRate"; resp["value"] = rate;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setChorusDepth") {
    float v = doc["value"];
    float depth = (v > 1.0f) ? (v / 100.0f) : v;
    spiMaster.setChorusDepth(depth);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "chorusDepth"; resp["value"] = depth;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setChorusMix") {
    float v = doc["value"];
    float mix = (v > 1.0f) ? (v / 100.0f) : v;
    spiMaster.setChorusMix(mix);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "chorusMix"; resp["value"] = mix;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setTremoloActive") {
    bool active = doc["value"];
    spiMaster.setTremoloActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "tremoloActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setTremoloRate") {
    float rate = doc["value"];
    spiMaster.setTremoloRate(rate);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "tremoloRate"; resp["value"] = rate;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setTremoloDepth") {
    float v = doc["value"];
    float depth = (v > 1.0f) ? (v / 100.0f) : v;
    spiMaster.setTremoloDepth(depth);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "tremoloDepth"; resp["value"] = depth;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setWavefolderGain") {
    float gain = doc["value"];
    spiMaster.setWaveFolderGain(gain);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "wavefolderGain"; resp["value"] = gain;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setLimiterActive") {
    bool active = doc["value"];
    spiMaster.setLimiterActive(active);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "limiterActive"; resp["value"] = active;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setTrackReverbSend") {
    int track = doc["track"];
    int level = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackReverbSend(track, (uint8_t)constrain(level, 0, 100));
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "reverbSend";
      resp["value"] = constrain(level, 0, 100);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackDelaySend") {
    int track = doc["track"];
    int level = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackDelaySend(track, (uint8_t)constrain(level, 0, 100));
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "delaySend";
      resp["value"] = constrain(level, 0, 100);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackChorusSend") {
    int track = doc["track"];
    int level = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackChorusSend(track, (uint8_t)constrain(level, 0, 100));
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "chorusSend";
      resp["value"] = constrain(level, 0, 100);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackPan") {
    int track = doc["track"];
    int pan = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackPan(track, (int8_t)constrain(pan, -100, 100));
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "pan";
      resp["value"] = constrain(pan, -100, 100);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackDspMute") {
    int track = doc["track"];
    bool mute = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackMute(track, mute);
      spiMaster.dsqSetMute((uint8_t)track, mute);
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "dspMute";
      resp["value"] = mute;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackSolo") {
    int track = doc["track"];
    bool solo = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackSolo(track, solo);
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "solo";
      resp["value"] = solo;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackPhaser") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      bool active = doc.containsKey("active") ? doc["active"].as<bool>() : true;
      float rate = doc.containsKey("rate") ? doc["rate"].as<float>() : 1.0f;
      float depth = doc.containsKey("depth") ? doc["depth"].as<float>() : 50.0f;
      float feedback = doc.containsKey("feedback") ? doc["feedback"].as<float>() : 50.0f;
      spiMaster.setTrackPhaser(track, active, rate, depth, feedback);
      StaticJsonDocument<192> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "phaser";
      resp["active"] = active;
      resp["rate"] = rate;
      resp["depth"] = depth;
      resp["feedback"] = feedback;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackTremolo") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      bool active = doc.containsKey("active") ? doc["active"].as<bool>() : true;
      float rate = doc.containsKey("rate") ? doc["rate"].as<float>() : 4.0f;
      float depth = doc.containsKey("depth") ? doc["depth"].as<float>() : 50.0f;
      uint8_t wave = doc.containsKey("wave") ? doc["wave"].as<uint8_t>() : 0;
      uint8_t target = doc.containsKey("target") ? doc["target"].as<uint8_t>() : 0;
      spiMaster.setTrackTremolo(track, active, rate, depth, wave, target);
      StaticJsonDocument<192> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "tremolo";
      resp["active"] = active;
      resp["rate"] = rate;
      resp["depth"] = depth;
      resp["wave"] = wave;
      resp["target"] = target;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackPitch") {
    int track = doc["track"];
    int cents = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackPitch(track, (int16_t)constrain(cents, -1200, 1200));
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "pitchCents";
      resp["value"] = constrain(cents, -1200, 1200);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackGate") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      bool active = doc.containsKey("active") ? doc["active"].as<bool>() : true;
      float threshold = doc.containsKey("threshold") ? doc["threshold"].as<float>() : -40.0f;
      float attack = doc.containsKey("attack") ? doc["attack"].as<float>() : 1.0f;
      float release = doc.containsKey("release") ? doc["release"].as<float>() : 50.0f;
      spiMaster.setTrackGate(track, active, threshold, attack, release);
      StaticJsonDocument<192> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "gate";
      resp["active"] = active;
      resp["threshold"] = threshold;
      resp["attack"] = attack;
      resp["release"] = release;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackEq") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      int low = doc.containsKey("low") ? doc["low"].as<int>() : 0;
      int mid = doc.containsKey("mid") ? doc["mid"].as<int>() : 0;
      int high = doc.containsKey("high") ? doc["high"].as<int>() : 0;
      spiMaster.setTrackEq(track, (int8_t)constrain(low, -12, 12), (int8_t)constrain(mid, -12, 12), (int8_t)constrain(high, -12, 12));
      StaticJsonDocument<192> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "eq";
      resp["low"] = constrain(low, -12, 12);
      resp["mid"] = constrain(mid, -12, 12);
      resp["high"] = constrain(high, -12, 12);
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackEqLow") {
    int track = doc["track"];
    int value = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackEqLow(track, (int8_t)constrain(value, -12, 12));
    }
  }
  else if (cmd == "setTrackEqMid") {
    int track = doc["track"];
    int value = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackEqMid(track, (int8_t)constrain(value, -12, 12));
    }
  }
  else if (cmd == "setTrackEqHigh") {
    int track = doc["track"];
    int value = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackEqHigh(track, (int8_t)constrain(value, -12, 12));
    }
  }
  // ============= Per-Pad / Per-Track FX Commands =============
  else if (cmd == "setPadDistortion") {
    int pad = doc["pad"];
    float amount = doc["amount"];
    int mode = doc.containsKey("mode") ? (int)doc["mode"] : 0;
    if (pad >= 0 && pad < 24) {
      spiMaster.setPadDistortion(pad, amount, (DistortionMode)mode);
      StaticJsonDocument<128> resp;
      resp["type"] = "padFxSet";
      resp["pad"] = pad;
      resp["fx"] = "distortion";
      resp["amount"] = amount;
      resp["mode"] = mode;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setPadBitCrush") {
    int pad = doc["pad"];
    int bits = doc["value"];
    if (pad >= 0 && pad < 24) {
      spiMaster.setPadBitCrush(pad, bits);
      StaticJsonDocument<128> resp;
      resp["type"] = "padFxSet";
      resp["pad"] = pad;
      resp["fx"] = "bitcrush";
      resp["value"] = bits;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "clearPadFX") {
    int pad = doc["pad"];
    if (pad >= 0 && pad < 24) {
      spiMaster.clearPadFX(pad);
      StaticJsonDocument<96> resp;
      resp["type"] = "padFxCleared";
      resp["pad"] = pad;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackDistortion") {
    int track = doc["track"];
    float amount = doc["amount"];
    int mode = doc.containsKey("mode") ? (int)doc["mode"] : 0;
    if (track >= 0 && track < 16) {
      spiMaster.setTrackDistortion(track, amount, (DistortionMode)mode);
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "distortion";
      resp["amount"] = amount;
      resp["mode"] = mode;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackBitCrush") {
    int track = doc["track"];
    int bits = doc["value"];
    if (track >= 0 && track < 16) {
      spiMaster.setTrackBitCrush(track, bits);
      StaticJsonDocument<128> resp;
      resp["type"] = "trackFxSet";
      resp["track"] = track;
      resp["fx"] = "bitcrush";
      resp["value"] = bits;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "clearTrackFX") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      spiMaster.clearTrackFX(track);
      StaticJsonDocument<96> resp;
      resp["type"] = "trackFxCleared";
      resp["track"] = track;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  // ============= REVERSE Command =============
  else if (cmd == "setReverse") {
    bool value = doc["value"];
    StaticJsonDocument<128> resp;
    resp["type"] = "trackFxUpdate";
    resp["fx"] = "reverse";
    resp["value"] = value;
    if (doc.containsKey("track")) {
      int track = doc["track"];
      if (track >= 0 && track < 16) {
        spiMaster.setReverseSample(track, value);
        resp["track"] = track;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    } else if (doc.containsKey("pad")) {
      int pad = doc["pad"];
      if (pad >= 0 && pad < 24) {
        spiMaster.setReverseSample(pad, value);
        resp["pad"] = pad;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    }
  }
  // ============= PITCH SHIFT Command =============
  else if (cmd == "setPitchShift") {
    float value = doc["value"];
    StaticJsonDocument<128> resp;
    resp["type"] = "trackFxUpdate";
    resp["fx"] = "pitch";
    resp["value"] = value;
    if (doc.containsKey("track")) {
      int track = doc["track"];
      if (track >= 0 && track < 16) {
        spiMaster.setTrackPitchShift(track, value);
        resp["track"] = track;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    } else if (doc.containsKey("pad")) {
      int pad = doc["pad"];
      if (pad >= 0 && pad < 24) {
        spiMaster.setTrackPitchShift(pad, value);
        resp["pad"] = pad;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    }
  }
  // ============= STUTTER Command =============
  else if (cmd == "setStutter") {
    bool value = false;
    if (doc.containsKey("value")) {
      value = doc["value"].as<bool>();
    } else if (doc.containsKey("active")) {
      value = doc["active"].as<bool>();
    }
    int interval = doc.containsKey("interval") ? (int)doc["interval"] : 100;
    StaticJsonDocument<128> resp;
    resp["type"] = "trackFxUpdate";
    resp["fx"] = "stutter";
    resp["value"] = value;
    resp["interval"] = interval;
    if (doc.containsKey("track")) {
      int track = doc["track"];
      if (track >= 0 && track < 16) {
        spiMaster.setStutter(track, value, interval);
        resp["track"] = track;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    } else if (doc.containsKey("pad")) {
      int pad = doc["pad"];
      if (pad >= 0 && pad < 24) {
        spiMaster.setStutter(pad, value, interval);
        resp["pad"] = pad;
        String out; serializeJson(resp, out);
        if (ws) ws->textAll(out);
      }
    }
  }
  // ============= PER-TRACK LIVE FX (SLAVE Controller) =============
  else if (cmd == "setTrackEcho") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      float time, feedback, mix;
      bool active;
      // One-knob mode (pot 0-127) or detailed mode
      if (doc.containsKey("value")) {
        int val = doc["value"].as<int>();
        active = val > 0;
        mix = (float)val / 127.0f * 100.0f;
        time = doc.containsKey("time") ? doc["time"].as<float>() : 100.0f;
        feedback = doc.containsKey("feedback") ? doc["feedback"].as<float>() : 40.0f;
      } else {
        active = doc["active"] | false;
        time = doc.containsKey("time") ? doc["time"].as<float>() : 100.0f;
        feedback = doc.containsKey("feedback") ? doc["feedback"].as<float>() : 40.0f;
        mix = doc.containsKey("mix") ? doc["mix"].as<float>() : 50.0f;
      }
      spiMaster.setTrackEcho(track, active, time, feedback, mix);
      StaticJsonDocument<256> resp;
      resp["type"] = "trackLiveFx";
      resp["track"] = track;
      resp["fx"] = "echo";
      resp["active"] = spiMaster.getTrackEchoActive(track);
      resp["time"] = time;
      resp["feedback"] = feedback;
      resp["mix"] = mix;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackFlanger") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      float rate, depth, feedback;
      bool active;
      if (doc.containsKey("value")) {
        int val = doc["value"].as<int>();
        active = val > 0;
        depth = (float)val / 127.0f * 100.0f;
        rate = doc.containsKey("rate") ? doc["rate"].as<float>() : 50.0f;
        feedback = doc.containsKey("feedback") ? doc["feedback"].as<float>() : 30.0f;
      } else {
        active = doc["active"] | false;
        rate = doc.containsKey("rate") ? doc["rate"].as<float>() : 50.0f;
        depth = doc.containsKey("depth") ? doc["depth"].as<float>() : 50.0f;
        feedback = doc.containsKey("feedback") ? doc["feedback"].as<float>() : 30.0f;
      }
      spiMaster.setTrackFlanger(track, active, rate, depth, feedback);
      StaticJsonDocument<256> resp;
      resp["type"] = "trackLiveFx";
      resp["track"] = track;
      resp["fx"] = "flanger";
      resp["active"] = spiMaster.getTrackFlangerActive(track);
      resp["rate"] = rate;
      resp["depth"] = depth;
      resp["feedback"] = feedback;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setTrackCompressor") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      float threshold, ratio;
      bool active;
      if (doc.containsKey("value")) {
        int val = doc["value"].as<int>();
        active = val > 0;
        threshold = -60.0f + (float)val / 127.0f * 60.0f; // 0=-60dB, 127=0dB
        ratio = doc.containsKey("ratio") ? doc["ratio"].as<float>() : 4.0f;
      } else {
        active = doc["active"] | false;
        threshold = doc.containsKey("threshold") ? doc["threshold"].as<float>() : -20.0f;
        ratio = doc.containsKey("ratio") ? doc["ratio"].as<float>() : 4.0f;
      }
      spiMaster.setTrackCompressor(track, active, threshold, ratio);
      StaticJsonDocument<256> resp;
      resp["type"] = "trackLiveFx";
      resp["track"] = track;
      resp["fx"] = "compressor";
      resp["active"] = spiMaster.getTrackCompressorActive(track);
      resp["threshold"] = threshold;
      resp["ratio"] = ratio;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setSidechainPro") {
    bool active = doc.containsKey("active") ? doc["active"].as<bool>() : true;
    int source = doc.containsKey("source") ? doc["source"].as<int>() : 0;
    float amountPct = doc.containsKey("amount") ? doc["amount"].as<float>() : 50.0f;
    float attackMs = doc.containsKey("attack") ? doc["attack"].as<float>() : 6.0f;
    float releaseMs = doc.containsKey("release") ? doc["release"].as<float>() : 180.0f;
    float knee = doc.containsKey("knee") ? doc["knee"].as<float>() : 0.4f;

    uint16_t mask = 0;
    if (doc.containsKey("destinations")) {
      JsonArrayConst dest = doc["destinations"].as<JsonArrayConst>();
      for (JsonVariantConst v : dest) {
        int t = v.as<int>();
        if (t >= 0 && t < 16 && t != source) mask |= (1U << t);
      }
    }

    spiMaster.setSidechain(active, source, mask, amountPct / 100.0f, attackMs, releaseMs, knee);

    StaticJsonDocument<256> resp;
    resp["type"] = "sidechainState";
    resp["active"] = active;
    resp["source"] = source;
    resp["mask"] = mask;
    resp["amount"] = amountPct;
    resp["attack"] = attackMs;
    resp["release"] = releaseMs;
    resp["knee"] = knee;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "clearTrackLiveFX") {
    int track = doc["track"];
    if (track >= 0 && track < 16) {
      spiMaster.clearTrackLiveFX(track);
      StaticJsonDocument<128> resp;
      resp["type"] = "trackLiveFx";
      resp["track"] = track;
      resp["fx"] = "cleared";
      resp["active"] = false;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setSequencerVolume") {
    int volume = constrain((int)doc["value"], 0, 150);
    spiMaster.setSequencerVolume(volume);
    
    // Broadcast volume change to all clients
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "state";
    responseDoc["sequencerVolume"] = volume;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setLiveVolume") {
    int volume = constrain((int)doc["value"], 0, 150);
    spiMaster.setLiveVolume(volume);
    
    // Broadcast volume change to all clients
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "state";
    responseDoc["liveVolume"] = volume;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setVolume") {
    int volume = doc["value"];
    spiMaster.setMasterVolume(volume);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "volume"; resp["value"] = volume;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "stopAllSounds") {
    spiMaster.stopAll();
    StaticJsonDocument<64> resp;
    resp["type"] = "allStopped";
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  else if (cmd == "setLivePitch") {
    float pitch = doc["pitch"].as<float>();
    pitch = constrain(pitch, 0.25f, 3.0f);
    spiMaster.setLivePitchShift(pitch);
    StaticJsonDocument<96> resp;
    resp["type"] = "masterFx"; resp["param"] = "livePitch"; resp["value"] = pitch;
    String out; serializeJson(resp, out);
    if (ws) ws->textAll(out);
  }
  // ============= NEW: Per-Track Filter Commands =============
  else if (cmd == "setTrackFilter") {
    int track = doc["track"];
    if (track < 0 || track >= 16) {
      return;
    }
    int filterType = doc.containsKey("type") ? doc["type"].as<int>() : doc["filterType"].as<int>();
    float cutoff = doc.containsKey("cutoff") ? doc["cutoff"].as<float>() : 1000.0f;
    float resonance = doc.containsKey("resonance") ? doc["resonance"].as<float>() : 1.0f;
    float gain = doc.containsKey("gain") ? doc["gain"].as<float>() : 0.0f;
    
    bool success = spiMaster.setTrackFilter(track, (FilterType)filterType, cutoff, resonance, gain);
    
    // Send response with filter parameters for UI badge
    StaticJsonDocument<256> responseDoc;
    responseDoc["type"] = "trackFilterSet";
    responseDoc["track"] = track;
    responseDoc["success"] = success;
    responseDoc["activeFilters"] = spiMaster.getActiveTrackFiltersCount();
    responseDoc["filterType"] = filterType;
    responseDoc["cutoff"] = (int)cutoff;
    responseDoc["resonance"] = resonance;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "clearTrackFilter") {
    int track = doc["track"];
    if (track < 0 || track >= 16) {
      return;
    }
    spiMaster.clearTrackFilter(track);
    
    // Send response
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "trackFilterCleared";
    responseDoc["track"] = track;
    responseDoc["activeFilters"] = spiMaster.getActiveTrackFiltersCount();
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  // ============= NEW: Per-Pad Filter Commands =============
  else if (cmd == "setPadFilter") {
    int pad = doc["pad"];
    if (pad < 0 || pad >= 24) {
      return;
    }
    int filterType = doc.containsKey("type") ? doc["type"].as<int>() : doc["filterType"].as<int>();
    float cutoff = doc.containsKey("cutoff") ? doc["cutoff"].as<float>() : 1000.0f;
    float resonance = doc.containsKey("resonance") ? doc["resonance"].as<float>() : 1.0f;
    float gain = doc.containsKey("gain") ? doc["gain"].as<float>() : 0.0f;
    
    bool success = spiMaster.setPadFilter(pad, (FilterType)filterType, cutoff, resonance, gain);
    
    // Send response
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "padFilterSet";
    responseDoc["pad"] = pad;
    responseDoc["success"] = success;
    responseDoc["activeFilters"] = spiMaster.getActivePadFiltersCount();
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "clearPadFilter") {
    int pad = doc["pad"];
    if (pad < 0 || pad >= 24) {
      return;
    }
    spiMaster.clearPadFilter(pad);
    
    // Send response
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "padFilterCleared";
    responseDoc["pad"] = pad;
    responseDoc["activeFilters"] = spiMaster.getActivePadFiltersCount();
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getFilterPresets") {
    // Return list of available filter presets
    StaticJsonDocument<512> responseDoc;
    responseDoc["type"] = "filterPresets";
    
    JsonArray presets = responseDoc.createNestedArray("presets");
    for (int i = 0; i <= 9; i++) {
      JsonObject preset = presets.createNestedObject();
      const FilterPreset* fp = SPIMaster::getFilterPreset((FilterType)i);
      preset["id"] = i;
      preset["name"] = fp->name;
      preset["cutoff"] = fp->cutoff;
      preset["resonance"] = fp->resonance;
      preset["gain"] = fp->gain;
    }
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  // ============= NEW: Step Velocity Commands =============
  else if (cmd == "setStepVelocity") {
    int track = doc["track"];
    int step = doc["step"];
    int velocity = doc["velocity"];
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) {
      return;
    }
    
    bool silent = doc.containsKey("silent") && doc["silent"].as<bool>();
    
    // Support writing to a specific pattern
    bool isBulkImport = doc.containsKey("pattern");
    if (isBulkImport) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepVelocity(pattern, track, step, velocity);
        spiMaster.dsqSetStep((uint8_t)pattern, (uint8_t)track, (uint8_t)step,
            sequencer.getStep(pattern, track, step) ? 1 : 0, (uint8_t)velocity,
            sequencer.getStepNoteLen(pattern, track, step),
            sequencer.getStepProbability(pattern, track, step));
        yield(); // Prevent watchdog reset during bulk import
      }
      return;
    } else {
      int curPat = sequencer.getCurrentPattern();
      sequencer.setStepVelocity(track, step, velocity);
      spiMaster.dsqSetStep((uint8_t)curPat, (uint8_t)track, (uint8_t)step,
          sequencer.getStep(curPat, track, step) ? 1 : 0, (uint8_t)velocity,
          sequencer.getStepNoteLen(curPat, track, step),
          sequencer.getStepProbability(curPat, track, step));
      yield();
    }
    
    // Broadcast to all clients (skip in silent/bulk mode)
    if (!silent) {
      StaticJsonDocument<128> responseDoc;
      responseDoc["type"] = "stepVelocitySet";
      responseDoc["track"] = track;
      responseDoc["step"] = step;
      responseDoc["velocity"] = velocity;
      
      String output;
      serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "getStepVelocity") {
    int track = doc["track"];
    int step = doc["step"];
    
    uint8_t velocity = sequencer.getStepVelocity(track, step);
    
    // Send response
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "stepVelocity";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["velocity"] = velocity;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepVolumeLock") {
    int track = doc["track"];
    int step = doc["step"];
    bool enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    int volume = doc.containsKey("volume") ? doc["volume"].as<int>() : 100;
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepVolumeLock(pattern, track, step, enabled, volume);
        dsqSyncParamLock(pattern, track, step);
      }
    } else {
      int curPat = sequencer.getCurrentPattern();
      sequencer.setStepVolumeLock(track, step, enabled, volume);
      dsqSyncParamLock(curPat, track, step);
    }

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepVolumeLockSet";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["volume"] = constrain(volume, 0, 150);
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepProbability") {
    int track = doc["track"];
    int step = doc["step"];
    int probability = doc.containsKey("probability") ? doc["probability"].as<int>() : 100;
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepProbability(pattern, track, step, probability);
        spiMaster.dsqSetStep((uint8_t)pattern, (uint8_t)track, (uint8_t)step,
            sequencer.getStep(pattern, track, step) ? 1 : 0,
            sequencer.getStepVelocity(pattern, track, step),
            sequencer.getStepNoteLen(pattern, track, step),
            (uint8_t)constrain(probability, 0, 100));
      }
    } else {
      int curPat = sequencer.getCurrentPattern();
      sequencer.setStepProbability(track, step, probability);
      spiMaster.dsqSetStep((uint8_t)curPat, (uint8_t)track, (uint8_t)step,
          sequencer.getStep(curPat, track, step) ? 1 : 0,
          sequencer.getStepVelocity(curPat, track, step),
          sequencer.getStepNoteLen(curPat, track, step),
          (uint8_t)constrain(probability, 0, 100));
    }

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepProbabilitySet";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["probability"] = constrain(probability, 0, 100);
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepCutoffLock") {
    int track = doc["track"];
    int step = doc["step"];
    bool enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    int cutoff = doc.containsKey("cutoff") ? doc["cutoff"].as<int>() : 1000;
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepCutoffLock(pattern, track, step, enabled, (uint16_t)constrain(cutoff, 20, 20000));
        dsqSyncParamLock(pattern, track, step);
      }
    } else {
      int curPat = sequencer.getCurrentPattern();
      sequencer.setStepCutoffLock(track, step, enabled, (uint16_t)constrain(cutoff, 20, 20000));
      dsqSyncParamLock(curPat, track, step);
    }

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepCutoffLockSet";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["cutoff"] = constrain(cutoff, 20, 20000);
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepReverbSendLock") {
    int track = doc["track"];
    int step = doc["step"];
    bool enabled = doc.containsKey("enabled") ? doc["enabled"].as<bool>() : true;
    int level = doc.containsKey("value") ? doc["value"].as<int>() : 0;
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepReverbSendLock(pattern, track, step, enabled, (uint8_t)constrain(level, 0, 100));
        dsqSyncParamLock(pattern, track, step);
      }
    } else {
      int curPat = sequencer.getCurrentPattern();
      sequencer.setStepReverbSendLock(track, step, enabled, (uint8_t)constrain(level, 0, 100));
      dsqSyncParamLock(curPat, track, step);
    }

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepReverbLockSet";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["value"] = constrain(level, 0, 100);
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepRatchet") {
    int track = doc["track"];
    int step = doc["step"];
    int ratchet = doc.containsKey("ratchet") ? doc["ratchet"].as<int>() : 1;
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.setStepRatchet(pattern, track, step, ratchet);
      }
    } else {
      sequencer.setStepRatchet(track, step, ratchet);
    }

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepRatchetSet";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["ratchet"] = constrain(ratchet, 1, 4);
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setStepNote") {
    int track = doc["track"];
    int step = doc["step"];
    int note = doc.containsKey("note") ? doc["note"].as<int>() : 0;
    int flags = doc.containsKey("flags") ? doc["flags"].as<int>() : -1;
    JsonArrayConst voices = doc["voices"].as<JsonArrayConst>();
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    if (doc.containsKey("pattern")) {
      int pattern = doc["pattern"].as<int>();
      if (pattern >= 0 && pattern < MAX_PATTERNS) {
        sequencer.clearStepNoteVoices(pattern, track, step);
        if (!voices.isNull()) {
          int voice = 0;
          for (JsonVariantConst v : voices) {
            if (voice >= MELODY_STEP_VOICES) break;
            int voiceNote = v.as<int>();
            if (voiceNote < 0) voiceNote = 0;
            if (voiceNote > 127) voiceNote = 127;
            sequencer.setStepNoteVoice(pattern, track, step, voice++, (uint8_t)voiceNote);
          }
        } else {
          sequencer.setStepNote(pattern, track, step, (uint8_t)note);
        }
        if (flags >= 0) sequencer.setStepFlags(pattern, track, step, (uint8_t)flags);
      }
    } else {
      sequencer.clearStepNoteVoices(track, step);
      if (!voices.isNull()) {
        int voice = 0;
        for (JsonVariantConst v : voices) {
          if (voice >= MELODY_STEP_VOICES) break;
          int voiceNote = v.as<int>();
          if (voiceNote < 0) voiceNote = 0;
          if (voiceNote > 127) voiceNote = 127;
          sequencer.setStepNoteVoice(track, step, voice++, (uint8_t)voiceNote);
        }
      } else {
        sequencer.setStepNote(track, step, (uint8_t)note);
      }
      if (flags >= 0) sequencer.setStepFlags(track, step, (uint8_t)flags);
    }

    bool silent = doc.containsKey("silent") && doc["silent"].as<bool>();
    if (!silent) {
      StaticJsonDocument<160> resp;
      resp["type"] = "stepNoteSet";
      resp["track"] = track;
      resp["step"] = step;
      resp["note"] = sequencer.getStepNote(track, step);
      JsonArray outVoices = resp.createNestedArray("voices");
      int respPattern = doc.containsKey("pattern") ? doc["pattern"].as<int>() : sequencer.getCurrentPattern();
      for (int voice = 0; voice < MELODY_STEP_VOICES; voice++) {
        outVoices.add(sequencer.getStepNoteVoice(respPattern, track, step, voice));
      }
      if (flags >= 0) resp["flags"] = flags;
      String out; serializeJson(resp, out);
      if (ws) ws->textAll(out);
    }
  }
  else if (cmd == "setHumanize") {
    int timing = doc.containsKey("timing") ? doc["timing"].as<int>() : 0;
    int velocity = doc.containsKey("velocity") ? doc["velocity"].as<int>() : 0;
    sequencer.setHumanize(timing, velocity);
    spiMaster.dsqSetHumanize(sequencer.getHumanizeTimingMs(), sequencer.getHumanizeVelocityAmount());

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "humanizeSet";
    responseDoc["timing"] = sequencer.getHumanizeTimingMs();
    responseDoc["velocity"] = sequencer.getHumanizeVelocityAmount();
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setSwing") {
    int amount = doc.containsKey("amount") ? doc["amount"].as<int>() :
                 (doc.containsKey("swing") ? doc["swing"].as<int>() :
                 (doc.containsKey("value") ? doc["value"].as<int>() : 0));
    gGrooveSwingAmount = constrain(amount, 0, 100);
    spiMaster.dsqSetSwing(gGrooveSwingAmount);

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "swingSet";
    responseDoc["swing"] = gGrooveSwingAmount;
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "resetGroove") {
    gGrooveSwingAmount = 0;
    sequencer.setHumanize(0, 0);
    spiMaster.dsqSetSwing(0);
    spiMaster.dsqSetHumanize(0, 0);

    StaticJsonDocument<192> responseDoc;
    responseDoc["type"] = "grooveState";
    responseDoc["swing"] = gGrooveSwingAmount;
    responseDoc["humanizeTimingMs"] = sequencer.getHumanizeTimingMs();
    responseDoc["humanizeVelocity"] = sequencer.getHumanizeVelocityAmount();
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getGrooveState") {
    StaticJsonDocument<192> responseDoc;
    responseDoc["type"] = "grooveState";
    responseDoc["swing"] = gGrooveSwingAmount;
    responseDoc["humanizeTimingMs"] = sequencer.getHumanizeTimingMs();
    responseDoc["humanizeVelocity"] = sequencer.getHumanizeVelocityAmount();
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getStepVolumeLock") {
    int track = doc["track"];
    int step = doc["step"];
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    bool enabled = sequencer.hasStepVolumeLock(track, step);
    int volume = enabled ? sequencer.getStepVolumeLock(track, step) : 0;

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepVolumeLock";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["volume"] = volume;
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getStepCutoffLock") {
    int track = doc["track"];
    int step = doc["step"];
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    bool enabled = sequencer.hasStepCutoffLock(track, step);
    int cutoff = enabled ? (int)sequencer.getStepCutoffLock(track, step) : 0;

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepCutoffLock";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["cutoff"] = cutoff;
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getStepReverbSendLock") {
    int track = doc["track"];
    int step = doc["step"];
    if (track < 0 || track >= MAX_TRACKS || step < 0 || step >= STEPS_PER_PATTERN) return;

    bool enabled = sequencer.hasStepReverbSendLock(track, step);
    int level = enabled ? (int)sequencer.getStepReverbSendLock(track, step) : 0;

    StaticJsonDocument<160> responseDoc;
    responseDoc["type"] = "stepReverbLock";
    responseDoc["track"] = track;
    responseDoc["step"] = step;
    responseDoc["enabled"] = enabled;
    responseDoc["value"] = level;
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "get_pattern") {
    int patternNum = doc.containsKey("pattern") ? doc["pattern"].as<int>() : sequencer.getCurrentPattern();
    
    if (patternNum < 0 || patternNum >= MAX_PATTERNS) return;

    // Enviar UDP de vuelta al slave (solo si es una petición UDP)
    if (s_udpReplyIp != IPAddress(0, 0, 0, 0) && s_udpReplyPort != 0) {
      sendUdpPatternRows(s_udpReplyIp, s_udpReplyPort, patternNum);
    }
  }
  // ============= NEW: Track Volume Commands =============
  else if (cmd == "setTrackVolume") {
    int track = doc["track"];
    int volume = constrain((int)doc["volume"], 0, 150);
    if (track < 0 || track >= 16) {
      return;
    }
    
    sequencer.setTrackVolume(track, volume);
    spiMaster.setTrackVolume(track, (uint8_t)constrain(volume, 0, 150));
    
    // Broadcast to all clients
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "trackVolumeSet";
    responseDoc["track"] = track;
    responseDoc["volume"] = volume;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
    broadcastUdpTrackVolume(track, volume);
  }
  else if (cmd == "getTrackVolume") {
    int track = doc["track"];
    
    uint8_t volume = sequencer.getTrackVolume(track);
    
    // Send response
    StaticJsonDocument<128> responseDoc;
    responseDoc["type"] = "trackVolume";
    responseDoc["track"] = track;
    responseDoc["volume"] = volume;
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "getTrackVolumes") {
    // Send all track volumes
    StaticJsonDocument<256> responseDoc;
    responseDoc["type"] = "trackVolumes";
    
    JsonArray volumes = responseDoc.createNestedArray("volumes");
    for (int track = 0; track < 16; track++) {
      volumes.add(sequencer.getTrackVolume(track));
    }
    
    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setTrackSynthEngine") {
    int track = doc["track"];
    int engine = doc["engine"];
    syslog("CMD", "setEngine trk=%d eng=%d heap=%u", track, engine, ESP.getFreeHeap());
    if (track < 0 || track >= 16 || engine < -1 || engine > 8) {
      return;  // Daisy supports engines 0..8
    }

    // Si el pad estaba en 303 y cambia a otro motor, detener la nota
    if (gTrackSynthEngine[track] == 3 && engine != 3) {
      spiMaster.synth303NoteOff();
    }

    if (engine >= 0) {
      clearTrackLoopStateForSynth(track, ws);
    }

    setTrackSynthEngine(track, (int8_t)engine);
    spiMaster.dsqSetTrackEngine((uint8_t)track, (int8_t)engine);
    // Asegurar que el track NO esta muteado en Daisy. El boot fallback
    // antiguo muteaba synth tracks, y eso dejaba el secuenciador silencioso
    // tras cambiar a sampler. La Daisy ya rutea por dsqTrackEngine[].
    spiMaster.dsqSetMute((uint8_t)track, false);

    // Stack buffer — avoids String heap allocation
    char buf[96];
    int len = snprintf(buf, sizeof(buf),
      "{\"type\":\"trackSynthEngineSet\",\"track\":%d,\"engine\":%d}",
      track, engine);
    if (ws && ws->count() > 0) ws->textAll(buf, len);
    syslog("CMD", "setEngine DONE trk=%d eng=%d heap=%u", track, engine, ESP.getFreeHeap());
  }
  else if (cmd == "applyKitToAllPads") {
    int engine = doc["engine"];
    syslog("CMD", "applyKitToAllPads engine=%d heap=%u", engine, ESP.getFreeHeap());
    if (engine < -1 || engine > 8) {
      return;  // Daisy supports engines 0..8
    }

    // Si algún track estaba en 303 y cambia a otro motor, detener la nota
    if (engine != 3) {
      for (int i = 0; i < 16; i++) {
        if (gTrackSynthEngine[i] == 3) {
          spiMaster.synth303NoteOff();
          break;
        }
      }
    }

    if (engine >= 0) {
      for (int i = 0; i < 16; i++) {
        clearTrackLoopStateForSynth(i, ws);
      }
    }

    setAllTrackSynthEngines((int8_t)engine);
    for (int i = 0; i < 16; i++) {
      spiMaster.dsqSetTrackEngine((uint8_t)i, (int8_t)engine);
      // Forzar unmute: limpia restos del boot que muteaba tracks por engine.
      spiMaster.dsqSetMute((uint8_t)i, false);
    }

    StaticJsonDocument<256> responseDoc;
    responseDoc["type"] = "trackSynthEngines";
    JsonArray engines = responseDoc.createNestedArray("engines");
    for (int track = 0; track < 16; track++) {
      engines.add((int)gTrackSynthEngine[track]);
    }

    String output;
    serializeJson(responseDoc, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setMidiScan") {
    bool enabled = doc["enabled"];
    if (midiController) {
      midiController->setScanEnabled(enabled);
      // Broadcast state to all clients
      StaticJsonDocument<128> responseDoc;
      responseDoc["type"] = "midiScan";
      responseDoc["enabled"] = enabled;
      String output;
      serializeJson(responseDoc, output);
      if (ws) ws->textAll(output);
    }
  }
  // ══════════════════════════════════════════════════════
  // DAISY SD CARD COMMANDS
  // ══════════════════════════════════════════════════════
  else if (cmd == "sdListKits") {
    SdKitListResponse kitList;
    if (spiMaster.sdGetKitList(kitList)) {
      DynamicJsonDocument resp(2048);
      resp["type"] = "sdKitList";
      JsonArray kits = resp.createNestedArray("kits");
      for (int i = 0; i < kitList.count && i < 16; i++) {
        kits.add(String(kitList.kits[i]));
      }
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    } else {
      StaticJsonDocument<128> resp;
      resp["type"] = "sdKitList";
      resp["error"] = "SPI timeout";
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "sdLoadKit") {
    const char* kit = doc["kit"];
    if (kit) {
      uint8_t startPad = doc.containsKey("startPad") ? doc["startPad"].as<uint8_t>() : 0;
      uint8_t maxPads  = doc.containsKey("maxPads")  ? doc["maxPads"].as<uint8_t>()  : 16;
      bool ok = spiMaster.sdLoadKit(kit, startPad, maxPads);
      if (ok) {
        clearDaisyPadFiles();
        // Asegurar que los tracks afectados quedan en modo sampler para que
        // el kit recien cargado realmente suene (el boot fallback puede
        // haber dejado tracks en engine 808 si no habia SD al arrancar).
        uint8_t lastPad = startPad + maxPads;
        if (lastPad > 16) lastPad = 16;
        for (uint8_t t = startPad; t < lastPad; t++) {
          setTrackSynthEngine(t, -1);
          spiMaster.dsqSetTrackEngine(t, -1);
          spiMaster.dsqSetMute(t, false);
        }
      }
      StaticJsonDocument<192> resp;
      resp["type"] = "sdLoadKitAck";
      resp["kit"] = kit;
      resp["ok"] = ok;
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "sdUnloadKit") {
    spiMaster.sdUnloadKit();
    clearDaisyPadFiles();
    StaticJsonDocument<64> resp;
    resp["type"] = "sdUnloadKitAck";
    resp["ok"] = true;
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "sdLoadSample") {
    int pad = doc["pad"];
    const char* folder = doc["folder"];
    const char* file   = doc["file"];
    if (folder && file && pad >= 0 && pad < 24) {
      SdFileInfoResponse info = {};
      spiMaster.sdGetFileInfo(folder, file, info);
      bool ok = spiMaster.sdLoadSample(pad, folder, file);
      if (ok) {
        setDaisyPadFile(pad, String(file));
      }
      StaticJsonDocument<192> resp;
      resp["type"] = "sdLoadSampleAck";
      resp["pad"]  = pad;
      resp["folder"] = folder;
      resp["file"] = file;
      resp["size"] = info.sizeBytes;
      resp["ok"]   = ok;
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "sdListFolders") {
    SdFolderListResponse folders;
    if (spiMaster.sdListFolders(folders)) {
      DynamicJsonDocument resp(2048);
      resp["type"] = "sdFolderList";
      JsonArray arr = resp.createNestedArray("folders");
      for (int i = 0; i < folders.count && i < 16; i++) {
        arr.add(String(folders.names[i]));
      }
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "sdListFiles") {
    const char* folder = doc["folder"];
    if (folder) {
      SdFileListResponse files;
      if (spiMaster.sdListFiles(folder, files)) {
        DynamicJsonDocument resp(4096);
        resp["type"] = "sdFileList";
        resp["folder"] = folder;
        JsonArray arr = resp.createNestedArray("files");
        for (int i = 0; i < files.count && i < 24; i++) {
          JsonObject f = arr.createNestedObject();
          f["name"] = String(files.files[i].name);
          f["size"] = files.files[i].sizeBytes;
        }
        String output;
        serializeJson(resp, output);
        if (ws) ws->textAll(output);
      }
    }
  }
  else if (cmd == "sdGetStatus") {
    SdStatusResponse sdStat;
    if (spiMaster.getCachedSdStatus(sdStat)) {
      clearDaisyPadFilesNotInMask(sdStat.samplesLoaded);
      int loadedCount = 0;
      for (int i = 0; i < MAX_PADS; i++) {
        if (sdStat.samplesLoaded & (1UL << i)) loadedCount++;
      }
      StaticJsonDocument<256> resp;
      resp["type"]      = "sdStatus";
      resp["present"]   = (bool)sdStat.present;
      resp["totalMB"]   = sdStat.totalMB;
      resp["freeMB"]    = sdStat.freeMB;
      resp["loaded"]    = loadedCount;
      resp["loadedMask"] = sdStat.samplesLoaded;
      resp["kit"]       = String(sdStat.currentKit);
      String output;
      serializeJson(resp, output);
      if (ws) ws->textAll(output);
    }
  }
  else if (cmd == "sdAbort") {
    spiMaster.sdAbortLoad();
    StaticJsonDocument<64> resp;
    resp["type"] = "sdAbortAck";
    resp["ok"]   = true;
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }
  else if (cmd == "setDaisyPerfStress") {
    bool enabled = doc["enabled"] | false;
    bool resetMetrics = doc["resetMetrics"] | false;
    bool ok = spiMaster.setPerformanceStressMode(enabled, resetMetrics);
    StaticJsonDocument<128> resp;
    resp["type"] = "daisyPerfStressAck";
    resp["ok"] = ok;
    resp["enabled"] = spiMaster.isPerformanceStressMode();
    resp["cpu"] = spiMaster.getCpuLoad();
    resp["cpuPeak"] = spiMaster.getCpuPeak();
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }
  // ══════════════════════════════════════════════════════
  // SYNTH ENGINES — TR-808/909/505 + TB-303 + WTOSC + SH-101 + FM2Op
  // ══════════════════════════════════════════════════════

  // {"cmd":"synthTrigger","engine":0,"instrument":0,"velocity":100}
  else if (cmd == "synthTrigger") {
    uint8_t engine     = doc["engine"]     | 0;
    uint8_t instrument = doc["instrument"] | 0;
    uint8_t velocity   = doc["velocity"]   | 100;
    float scaled = (velocity / 127.0f) * (spiMaster.getLiveVolume() / 100.0f);
    uint8_t outVelocity = (uint8_t)constrain((int)roundf(scaled * 127.0f), 1, 127);
    spiMaster.synthTrigger(engine, instrument, outVelocity);
  }

  // {"cmd":"synthParam","engine":0,"instrument":0,"paramId":0,"value":0.5}
  else if (cmd == "synthParam") {
    uint8_t engine     = doc["engine"]     | 0;
    uint8_t instrument = doc["instrument"] | 0;
    uint8_t paramId    = doc["paramId"]    | 0;
    float   value      = doc["value"]      | 0.5f;
    spiMaster.synthParam(engine, instrument, paramId, value);
  }

  // {"cmd":"setWtNote","track":0,"note":60}
  else if (cmd == "setWtNote") {
    int track = doc["track"] | 0;
    int note  = doc["note"]  | 60;
    if (track >= 0 && track < 16)
      spiMaster.synthParam(4, (uint8_t)track, 8, (float)note);
  }

  // {"cmd":"synth303NoteOn","note":48,"accent":false,"slide":false}
  else if (cmd == "synth303NoteOn") {
    uint8_t note   = doc["note"]   | 36;
    bool    accent = doc["accent"] | false;
    bool    slide  = doc["slide"]  | false;
    spiMaster.synth303NoteOn(note, accent, slide);
  }

  // {"cmd":"synthNoteOnEx","engine":4,"note":60,"velocity":100,"accent":false,"slide":false}
  else if (cmd == "synthNoteOnEx") {
    uint8_t engine   = doc["engine"]   | 3;
    uint8_t note     = doc["note"]     | 48;
    uint8_t velocity = doc["velocity"] | 100;
    bool    accent   = doc["accent"]   | false;
    bool    slide    = doc["slide"]    | false;
    spiMaster.synthNoteOnEx(engine, note, velocity, accent, slide);
  }

  // v2.7 — {"cmd":"melodyRecNote","engine":4,"note":60}
  // Forwarded to all UDP slaves so the S3 melody screen can capture P4 piano
  // keystrokes when its REC mode is enabled. Master itself takes no action.
  // v2.9 — Melody authoritative state. Master keeps engine/octave/grid/rec
  // and broadcasts melody_sync to ALL UDP slaves on every change so P4 piano
  // and S3 melody screen mirror each other automatically (just like pads).
  else if (cmd == "melodyRecToggle") {
    uint8_t e = doc["engine"] | melodyEngine; if (e <= 8) melodyEngine = e;
    int o     = doc["octave"] | (int)melodyOctave; if (o >= 0 && o <= 9) melodyOctave = (uint8_t)o;
    melodyRecActive = doc["active"] | !melodyRecActive;
    if (melodyRecActive) { melodyClearGrid(); melodyStep = 0; }
    broadcastMelodySync();
  }
  else if (cmd == "melodySetEngine") {
    uint8_t e = doc["engine"] | melodyEngine;
    if (e <= 8) melodyEngine = e;
    broadcastMelodySync();
  }
  else if (cmd == "melodySetOctave") {
    int o = doc["octave"] | (int)melodyOctave;
    if (o < 0) o = 0; if (o > 9) o = 9;
    melodyOctave = (uint8_t)o;
    broadcastMelodySync();
  }
  else if (cmd == "melodySetPad") {
    int p = doc["pad"] | (int)melodyPad;
    if (p >= 0 && p < 16) melodyPad = (uint8_t)p;
    broadcastMelodySync();
  }
  else if (cmd == "melodyRecNote") {
    int note = doc["note"] | -1;
    Serial.printf("[MASTER melodyRecNote] rec=%d note=%d step=%u\n", (int)melodyRecActive, note, melodyStep);
    if (note >= 0 && note <= 127 && melodyRecActive) {
      // map midi -> S3 row table: row r where (11-r) == midi%12
      int pc  = note % 12;
      int row = 11 - pc;
      int col = melodyStep;
      if (col >= 0 && col < 16 && row >= 0 && row < 12) {
        melodyGrid[col][row] = true;
        melodyStep = (uint8_t)((col + 1) % 16);
      }
      broadcastMelodySync();
    }
  }
  else if (cmd == "melodyAssign") {
    uint8_t e = doc["engine"] | melodyEngine; if (e <= 8) melodyEngine = e;
    int o     = doc["octave"] | (int)melodyOctave; if (o >= 0 && o <= 9) melodyOctave = (uint8_t)o;
    int pad = doc["pad"] | (int)melodyPad;
    if (pad >= 0 && pad < 16) {
      melodyPad = (uint8_t)pad;
      JsonArrayConst steps = doc["steps"].as<JsonArrayConst>();
      uint8_t assignedNotes[16][MELODY_STEP_VOICES] = {};
      if (!steps.isNull()) {
        melodyClearGrid();
        int col = 0;
        for (JsonVariantConst stepArr : steps) {
          if (col >= 16) break;
          if (stepArr.is<JsonArrayConst>()) {
            int voice = 0;
            for (JsonVariantConst v : stepArr.as<JsonArrayConst>()) {
              int note = v.as<int>();
              if (note < 0 || note > 127) continue;
              int row = 11 - (note % 12);
              if (row >= 0 && row < 12) melodyGrid[col][row] = true;
              if (note > 0 && voice < MELODY_STEP_VOICES) {
                assignedNotes[col][voice++] = (uint8_t)note;
              }
            }
          }
          col++;
        }
      }
      melodyPadAssigned[pad] = true;
      melodyPadEngine[pad]   = melodyEngine;
      melodyPadOctave[pad]   = melodyOctave;
      memcpy(melodyPadGrid[pad], melodyGrid, sizeof(melodyGrid));
      setTrackSynthEngine(pad, (int8_t)melodyEngine);
      spiMaster.dsqSetTrackEngine((uint8_t)pad, (int8_t)melodyEngine);
      int curPat = sequencer.getCurrentPattern();
      for (int step = 0; step < 16; step++) {
        bool active = false;
        int voice = 0;
        uint8_t primaryNote = 0;
        sequencer.clearStepNoteVoices(pad, step);
        if (!steps.isNull()) {
          for (int v = 0; v < MELODY_STEP_VOICES; v++) {
            uint8_t midi = assignedNotes[step][v];
            if (midi == 0) continue;
            if (primaryNote == 0) primaryNote = midi;
            active = true;
            sequencer.setStepNoteVoice(pad, step, voice++, midi);
          }
        } else {
          for (int row = 0; row < 12; row++) {
            if (!melodyGrid[step][row]) continue;
            active = true;
            if (voice < MELODY_STEP_VOICES) {
              int pc = 11 - row;
              int midi = ((int)melodyOctave + 1) * 12 + pc;
              if (midi < 0) midi = 0;
              if (midi > 127) midi = 127;
              if (primaryNote == 0) primaryNote = (uint8_t)midi;
              sequencer.setStepNoteVoice(pad, step, voice, (uint8_t)midi);
              voice++;
            }
          }
        }
        sequencer.setStepNote(pad, step, primaryNote);
        sequencer.setStep(pad, step, active);
        sequencer.setStepNoteLen(pad, step, 1);
        spiMaster.dsqSetStep((uint8_t)curPat, (uint8_t)pad, (uint8_t)step,
            active ? 1 : 0,
            sequencer.getStepVelocity(curPat, pad, step),
            1,
            sequencer.getStepProbability(curPat, pad, step));
      }
      Serial.printf("[MASTER melodyAssign] pad=%d eng=%u oct=%u\n", pad, melodyEngine, melodyOctave);
      broadcastMelodySync();
    }
  }
  else if (cmd == "melodyClear") {
    melodyClearGrid(); melodyStep = 0;
    broadcastMelodySync();
  }

  // {"cmd":"synth303NoteOff"}
  else if (cmd == "synth303NoteOff") {
    spiMaster.synth303NoteOff();
  }

  // {"cmd":"synthNoteOff","engine":4,"track":0}
  else if (cmd == "synthNoteOff") {
    uint8_t engine = doc["engine"] | 3;
    uint8_t track  = doc["track"]  | 0;
    uint8_t note   = doc.containsKey("note") ? (uint8_t)(doc["note"] | 0xFF) : (uint8_t)0xFF;
    if ((track < 16 || track == 0xFF) && engine <= 8) {
      Serial.printf("[MASTER synthNoteOff] engine=%u track=%u note=%u\n",
                    (unsigned)engine, (unsigned)track, (unsigned)note);
      spiMaster.synthNoteOff(engine, track, note);
    } else {
      Serial.printf("[MASTER synthNoteOff reject] engine=%u track=%u note=%u\n",
                    (unsigned)engine, (unsigned)track, (unsigned)note);
    }
  }

  // {"cmd":"synth303Param","paramId":0,"value":800.0}
  else if (cmd == "synth303Param") {
    uint8_t paramId = doc["paramId"] | 0;
    float   value   = doc["value"]   | 0.5f;
    spiMaster.synth303Param(paramId, value);
  }

  // {"cmd":"synthActive","mask":123}   (bit0=808, bit1=909, bit2=505, bit3=303, bit4=WT, bit5=SH101, bit6=FM2Op)
  // {"cmd":"synthActive","mask":511}   (9 engines, 16-bit)
  else if (cmd == "synthActive") {
    uint16_t mask = doc["mask"] | 0x01FF;
    if (mask > 0xFF) {
      spiMaster.synthSetActive16(mask);
    } else {
      spiMaster.synthSetActive((uint8_t)mask);
    }
    StaticJsonDocument<64> resp;
    resp["type"] = "synthActiveAck";
    resp["mask"] = mask;
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }

  // {"cmd":"synthPreset","engine":5,"preset":2}
  else if (cmd == "synthPreset") {
    uint8_t engine = doc["engine"] | 0;
    uint8_t preset = doc["preset"] | 0;
    spiMaster.synthPreset(engine, preset);

    StaticJsonDocument<96> resp;
    resp["type"] = "synthPresetAck";
    resp["engine"] = engine;
    resp["preset"] = preset;
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }

  // ═══════════════════════════════════════════════════
  // NEW MASTER FX
  // ═══════════════════════════════════════════════════

  // {"cmd":"setMasterFxRoute","fxId":10,"connected":true}
  else if (cmd == "setMasterFxRoute") {
    uint8_t fxId = doc["fxId"] | 0;
    bool connected = doc["connected"] | false;
    spiMaster.setMasterFxRoute(fxId, connected);
  }

  // {"cmd":"setAutoWahActive","active":true}
  else if (cmd == "setAutoWahActive") {
    bool active = doc["active"] | false;
    spiMaster.setAutoWahActive(active);
  }
  // {"cmd":"setAutoWahLevel","level":80}
  else if (cmd == "setAutoWahLevel") {
    uint8_t level = doc["level"] | 80;
    spiMaster.setAutoWahLevel(level);
  }
  // {"cmd":"setAutoWahMix","mix":50}
  else if (cmd == "setAutoWahMix") {
    uint8_t mix = doc["mix"] | 50;
    spiMaster.setAutoWahMix(mix);
  }

  // {"cmd":"setStereoWidth","width":100}
  else if (cmd == "setStereoWidth") {
    uint8_t width = doc["width"] | 100;
    spiMaster.setStereoWidth(width);
  }

  // {"cmd":"setTapeStop","mode":1}
  else if (cmd == "setTapeStop") {
    uint8_t mode = doc["mode"] | 0;
    spiMaster.setTapeStop(mode);
  }

  // {"cmd":"setBeatRepeat","division":8}
  else if (cmd == "setBeatRepeat") {
    uint8_t division = doc["division"] | 0;
    spiMaster.setBeatRepeat(division);
  }

  // {"cmd":"setDelayStereo","mode":1}
  else if (cmd == "setDelayStereo") {
    uint8_t mode = doc["mode"] | 0;
    spiMaster.setDelayStereo(mode);
  }

  // {"cmd":"setChorusStereo","mode":1}
  else if (cmd == "setChorusStereo") {
    uint8_t mode = doc["mode"] | 0;
    spiMaster.setChorusStereo(mode);
  }

  // {"cmd":"setEarlyRefActive","active":true}
  else if (cmd == "setEarlyRefActive") {
    bool active = doc["active"] | false;
    spiMaster.setEarlyRefActive(active);
  }
  // {"cmd":"setEarlyRefMix","mix":30}
  else if (cmd == "setEarlyRefMix") {
    uint8_t mix = doc["mix"] | 30;
    spiMaster.setEarlyRefMix(mix);
  }

  // ═══════════════════════════════════════════════════
  // CHOKE GROUPS
  // ═══════════════════════════════════════════════════

  // {"cmd":"setChokeGroup","pad":6,"group":1}
  else if (cmd == "setChokeGroup") {
    uint8_t pad = doc["pad"] | 0;
    uint8_t group = doc["group"] | 0;
    spiMaster.setChokeGroup(pad, group);
  }

  // ═══════════════════════════════════════════════════
  // SONG CHAIN MODE
  // ═══════════════════════════════════════════════════

  // {"cmd":"songChainUpload","chain":[{"pattern":0,"repeats":4},{"pattern":1,"repeats":2}]}
  else if (cmd == "songChainUpload") {
    JsonArrayConst arr = doc["chain"].as<JsonArrayConst>();
    if (!arr.isNull() && arr.size() > 0) {
      uint8_t count = min((int)arr.size(), (int)Sequencer::SONG_CHAIN_MAX);
      Sequencer::SongChainEntry entries[Sequencer::SONG_CHAIN_MAX];
      for (uint8_t i = 0; i < count; i++) {
        entries[i].pattern = arr[i]["pattern"] | 0;
        entries[i].repeats = arr[i]["repeats"] | 1;
      }
      sequencer.songChainUpload(entries, count);
      // Also upload to Daisy via SPI
      SongEntry spiEntries[SONG_MAX_ENTRIES];
      for (uint8_t i = 0; i < count; i++) {
        spiEntries[i].pattern = entries[i].pattern;
        spiEntries[i].repeats = entries[i].repeats;
      }
      spiMaster.songUpload(spiEntries, count);
    }
  }

  // {"cmd":"songChainControl","action":1}  0=stop, 1=play, 2=reset
  else if (cmd == "songChainControl") {
    uint8_t action = doc["action"] | 0;
    if (action == 1) {
      sequencer.songChainPlay();
      spiMaster.songControl(1);
    } else if (action == 0) {
      sequencer.songChainStop();
      spiMaster.songControl(0);
    } else if (action == 2) {
      sequencer.songChainReset();
      spiMaster.songControl(2);
    }
    broadcastSequencerState();
  }

  // {"cmd":"songGetPos"}
  else if (cmd == "songGetPos") {
    StaticJsonDocument<128> resp;
    resp["type"] = "songPos";
    resp["idx"] = sequencer.getSongChainIdx();
    resp["pattern"] = sequencer.getCurrentPattern();
    resp["repeat"] = sequencer.getSongChainRepeatCnt();
    resp["active"] = sequencer.isSongChainActive();
    String output;
    serializeJson(resp, output);
    if (ws) ws->textAll(output);
  }

  // ═══════════════════════════════════════════════════
  // PER-TRACK LFO CONFIG (Daisy-side)
  // ═══════════════════════════════════════════════════

  // {"cmd":"setTrackLfo","track":0,"wave":0,"target":3,"rate":100,"depth":500}
  else if (cmd == "setTrackLfo") {
    uint8_t track = doc["track"] | 0;
    uint8_t wave = doc["wave"] | 0;
    uint8_t target = doc["target"] | 0;
    uint16_t rate = doc["rate"] | 100;     // centésimas de Hz
    uint16_t depth = doc["depth"] | 500;   // milésimas
    spiMaster.setTrackLfoConfig(track, wave, target, rate, depth);
  }
}

void WebInterface::updateUdpClient(IPAddress ip, uint16_t port) {
  // Use fixed-size key buffer to avoid String heap allocation/fragmentation
  char key[16];
  snprintf(key, sizeof(key), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  String sKey(key);

  auto it = udpClients.find(sKey);
  if (it != udpClients.end()) {
    if (it->second.port != port) {
      it->second.port = port;
    }
    it->second.lastSeen = millis();
    it->second.packetCount++;
  } else {
    if (udpClients.size() >= 10) {
      cleanupStaleUdpClients();
      if (udpClients.size() >= 10) return;
    }
    UdpClient client;
    client.ip = ip;
    client.port = port;
    client.lastSeen = millis();
    client.packetCount = 1;
    udpClients[sKey] = client;
  }
}

// Limpiar clientes UDP inactivos
void WebInterface::cleanupStaleUdpClients() {
  unsigned long now = millis();
  auto it = udpClients.begin();
  
  int cleaned = 0;
  while (it != udpClients.end()) {
    if (now - it->second.lastSeen > UDP_CLIENT_TIMEOUT) {
      // Serial.printf("[UDP] Client timeout: %s\n", it->first.c_str()); // Comentado
      it = udpClients.erase(it);
      cleaned++;
      if (cleaned % 2 == 0) yield(); // Yield cada 2 eliminaciones
    } else {
      ++it;
    }
  }
}

// Manejar paquetes UDP entrantes (with crash protection)
void WebInterface::handleUdp() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  // ── Heap guard: skip processing if memory is critical ──
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 20000) {
    char drain[64];
    while (udp.available()) udp.read(drain, sizeof(drain));
    return;
  }

  // ── Reject oversized packets ──
  if (packetSize >= (int)kUdpMaxPacketBytes) {
    char drain[64];
    while (udp.available()) udp.read(drain, sizeof(drain));
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print("{\"s\":\"err\",\"m\":\"too_large\"}");
    udp.endPacket();
    return;
  }

  static char incomingPacket[kUdpMaxPacketBytes];
  int len = udp.read(incomingPacket, sizeof(incomingPacket) - 1);
  if (len <= 0) return;
  incomingPacket[len] = 0;

  updateUdpClient(udp.remoteIP(), udp.remotePort());

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, incomingPacket);

  if (!error) {
    const char* cmd = doc["cmd"] | "";
    bool syncAfter = shouldSendUdpStateSync(cmd);
    IPAddress remoteIp = udp.remoteIP();
    uint16_t remotePort = udp.remotePort();
    s_udpReplyIp = remoteIp;
    s_udpReplyPort = remotePort;
    processCommand(doc);
    s_udpReplyIp = IPAddress(0, 0, 0, 0);
    s_udpReplyPort = 0;
    udp.beginPacket(remoteIp, remotePort);
    udp.print("{\"s\":\"ok\"}");
    udp.endPacket();
    if (syncAfter) {
      sendUdpStateSync(remoteIp, remotePort);
    }
  } else {
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print("{\"s\":\"err\",\"m\":\"parse\"}");
    udp.endPacket();
  }
  yield();
}

// ========================================
// MIDI Functions
// ========================================

void WebInterface::setMIDIController(MIDIController* controller) {
  midiController = controller;
}

void WebInterface::broadcastMIDIMessage(const MIDIMessage& msg) {
  if (!initialized || !ws || ws->count() == 0) return;
  
  // Throttling: solo broadcast cada 100ms para baja latencia
  static uint32_t lastBroadcastTime = 0;
  static uint32_t messageCount = 0;
  uint32_t now = millis();
  
  messageCount++;
  
  // Solo enviar actualizaciones cada 100ms (máximo 10 por segundo)
  if (now - lastBroadcastTime < 100) {
    return; // Skip este broadcast
  }
  
  lastBroadcastTime = now;
  
  StaticJsonDocument<256> doc;
  doc["type"] = "midiMessage";
  
  // Message type string
  const char* msgType = "unknown";
  switch (msg.type) {
    case MIDI_NOTE_ON: msgType = "noteOn"; break;
    case MIDI_NOTE_OFF: msgType = "noteOff"; break;
    case MIDI_CONTROL_CHANGE: msgType = "cc"; break;
    case MIDI_PROGRAM_CHANGE: msgType = "program"; break;
    case MIDI_PITCH_BEND: msgType = "pitchBend"; break;
    case MIDI_AFTERTOUCH: msgType = "aftertouch"; break;
    case MIDI_CHANNEL_PRESSURE: msgType = "pressure"; break;
  }
  
  doc["messageType"] = msgType;
  doc["channel"] = msg.channel + 1; // 1-16 for display
  doc["data1"] = msg.data1;
  doc["data2"] = msg.data2;
  doc["timestamp"] = msg.timestamp;
  doc["totalMessages"] = messageCount; // Contador acumulado
  
  String output;
  serializeJson(doc, output);
  ws->textAll(output);
}

void WebInterface::broadcastMIDIDeviceStatus(bool connected, const MIDIDeviceInfo& info) {
  if (!initialized || !ws || ws->count() == 0) return;
  
  StaticJsonDocument<512> doc;
  doc["type"] = "midiDevice";
  doc["connected"] = connected;
  
  if (connected) {
    doc["deviceName"] = info.deviceName;
    doc["vendorId"] = info.vendorId;
    doc["productId"] = info.productId;
    doc["connectTime"] = info.connectTime;
  }
  
  String output;
  serializeJson(doc, output);
  ws->textAll(output);
  
}

// ========================================
// SAMPLE UPLOAD FUNCTIONS
// ========================================

// Variables estáticas para mantener estado del upload
static String uploadFilename;
static int uploadPad = -1;
static size_t uploadSize = 0;
static size_t uploadReceived = 0;
static bool uploadError = false;
static String uploadErrorMsg = "";
static int uploadLastPercent = -1;
static constexpr bool UPLOAD_PROGRESS_WS_ENABLED = false;

void WebInterface::handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    // ── Primera parte: inicializar estado ──
    uploadFilename    = filename;
    uploadReceived    = 0;
    uploadError       = false;
    uploadErrorMsg    = "";
    uploadLastPercent = -1;

    // Pad
    if (!request->hasParam("pad", false)) {
      uploadError = true; uploadErrorMsg = "Missing pad parameter";
      broadcastUploadComplete(-1, false, uploadErrorMsg); return;
    }
    uploadPad = request->getParam("pad", false)->value().toInt();
    if (uploadPad < 0 || uploadPad >= MAX_SAMPLES) {
      uploadError = true; uploadErrorMsg = "Invalid pad number";
      broadcastUploadComplete(uploadPad, false, uploadErrorMsg); return;
    }

    // Extensión
    if (!filename.endsWith(".wav") && !filename.endsWith(".WAV")) {
      uploadError = true; uploadErrorMsg = "Only WAV files are supported";
      broadcastUploadComplete(uploadPad, false, uploadErrorMsg); return;
    }

    // Tamaño
    uploadSize = request->contentLength();
    if (uploadSize == 0) {
      uploadError = true; uploadErrorMsg = "Unknown file size (Content-Length missing)";
      broadcastUploadComplete(uploadPad, false, uploadErrorMsg); return;
    }
    if (uploadSize > 8 * 1024 * 1024) {
      uploadError = true; uploadErrorMsg = "File too large (max 8MB)";
      broadcastUploadComplete(uploadPad, false, uploadErrorMsg); return;
    }

    // Liberar buffer anterior si existe
    if (_uploadBuf) { free(_uploadBuf); _uploadBuf = nullptr; _uploadBufLen = 0; }

    // Allocar en PSRAM
    _uploadBuf = (uint8_t*)heap_caps_malloc(uploadSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_uploadBuf) {
      // fallback DRAM (solo para archivos pequeños)
      _uploadBuf = (uint8_t*)heap_caps_malloc(uploadSize, MALLOC_CAP_8BIT);
    }
    if (!_uploadBuf) {
      uploadError = true; uploadErrorMsg = "No memory for upload";
      broadcastUploadComplete(uploadPad, false, uploadErrorMsg); return;
    }
    _uploadBufLen = 0;
  }

  if (uploadError) {
    if (final) request->send(400, "application/json",
      "{\"success\":false,\"message\":\"" + uploadErrorMsg + "\"}");
    return;
  }

  // ── Acumular datos en PSRAM ──
  if (_uploadBuf && len > 0) {
    size_t copy = len;
    if (_uploadBufLen + copy > uploadSize) copy = uploadSize - _uploadBufLen;
    memcpy(_uploadBuf + _uploadBufLen, data, copy);
    _uploadBufLen += copy;
    uploadReceived  = _uploadBufLen;

    int percent = (uploadSize > 0) ? (int)(uploadReceived * 100 / uploadSize) : 0;
    if (UPLOAD_PROGRESS_WS_ENABLED && percent != uploadLastPercent && percent % 25 == 0) {
      broadcastUploadProgress(uploadPad, percent);
      uploadLastPercent = percent;
    }
    yield();
  }

  // ── Upload completo ──
  if (final) {
    // Responder HTTP 200 inmediatamente — no bloquear el task de AsyncWebServer
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Uploading...\"}");

    // Activar carga diferida en update() (systemTask Core0)
    _pendingLoadPad = uploadPad;

    uploadPad = -1; uploadFilename = ""; uploadSize = 0;
    uploadReceived = 0; uploadError = false; uploadErrorMsg = "";
    uploadLastPercent = -1;
  }
}

static portMUX_TYPE s_daisyUploadMux = portMUX_INITIALIZER_UNLOCKED;
static constexpr size_t kDaisyUploadQueueBlocks = 64;
static constexpr size_t kDaisyUploadBlockSamples = 256;

struct DaisyUploadPcmBlock {
  uint16_t count = 0;
  int16_t samples[kDaisyUploadBlockSamples] = {};
};

static DaisyUploadPcmBlock s_daisyUploadQueue[kDaisyUploadQueueBlocks];
static volatile uint16_t s_daisyUploadQueueRead = 0;
static volatile uint16_t s_daisyUploadQueueWrite = 0;
static volatile uint16_t s_daisyUploadQueueCount = 0;

static void resetDaisyUploadQueue() {
  portENTER_CRITICAL(&s_daisyUploadMux);
  s_daisyUploadQueueRead = 0;
  s_daisyUploadQueueWrite = 0;
  s_daisyUploadQueueCount = 0;
  portEXIT_CRITICAL(&s_daisyUploadMux);
}

static bool pushDaisyUploadBlock(const int16_t* samples, uint16_t count) {
  if (!samples || count == 0 || count > kDaisyUploadBlockSamples) return false;

  while (true) {
    bool pushed = false;
    portENTER_CRITICAL(&s_daisyUploadMux);
    if (s_daisyUploadQueueCount < kDaisyUploadQueueBlocks) {
      DaisyUploadPcmBlock& block = s_daisyUploadQueue[s_daisyUploadQueueWrite];
      block.count = count;
      memcpy(block.samples, samples, count * sizeof(int16_t));
      s_daisyUploadQueueWrite = (uint16_t)((s_daisyUploadQueueWrite + 1) % kDaisyUploadQueueBlocks);
      s_daisyUploadQueueCount++;
      pushed = true;
    }
    portEXIT_CRITICAL(&s_daisyUploadMux);

    if (pushed) return true;
    delay(1);
    yield();
    esp_task_wdt_reset();
    if (s_daisyUpload.error) return false;
  }
}

static bool popDaisyUploadBlock(DaisyUploadPcmBlock& out) {
  bool ok = false;
  portENTER_CRITICAL(&s_daisyUploadMux);
  if (s_daisyUploadQueueCount > 0) {
    out = s_daisyUploadQueue[s_daisyUploadQueueRead];
    s_daisyUploadQueueRead = (uint16_t)((s_daisyUploadQueueRead + 1) % kDaisyUploadQueueBlocks);
    s_daisyUploadQueueCount--;
    ok = true;
  }
  portEXIT_CRITICAL(&s_daisyUploadMux);
  return ok;
}

static uint16_t le16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t wav_s24_to_s16(const uint8_t* p) {
  int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
  if (v & 0x00800000) v |= ~0x00FFFFFF;
  return (int16_t)(v >> 8);
}

static void daisyUploadError(const char* msg) {
  s_daisyUpload.error = true;
  strncpy(s_daisyUpload.errorMsg, msg, sizeof(s_daisyUpload.errorMsg) - 1);
  s_daisyUpload.errorMsg[sizeof(s_daisyUpload.errorMsg) - 1] = '\0';
}

static bool parseDaisyUploadHeader() {
  DaisyUploadStreamState& st = s_daisyUpload;
  if (st.headerLen < 44) return false;
  if (memcmp(st.header, "RIFF", 4) != 0 || memcmp(st.header + 8, "WAVE", 4) != 0) {
    daisyUploadError("Not a WAV file");
    return false;
  }

  uint32_t pos = 12;
  bool fmtFound = false;
  bool dataFound = false;
  while (pos + 8 <= st.headerLen) {
    uint32_t chunkSize = le32(st.header + pos + 4);
    if (memcmp(st.header + pos, "fmt ", 4) == 0) {
      if (chunkSize < 16 || pos + 24 > st.headerLen) return false;
      const uint8_t* fmt = st.header + pos + 8;
      uint16_t audioFormat = le16(fmt);
      st.channels = le16(fmt + 2);
      st.bits = le16(fmt + 14);
      if (audioFormat != 1 && audioFormat != 0xFFFE) {
        daisyUploadError("Unsupported WAV format");
        return false;
      }
      fmtFound = true;
    } else if (memcmp(st.header + pos, "data", 4) == 0) {
      st.dataPos = pos + 8;
      st.dataSize = chunkSize;
      dataFound = true;
      break;
    }
    pos += 8 + chunkSize + (chunkSize & 1);
  }

  if (!fmtFound || !dataFound) return false;
  if ((st.bits != 16 && st.bits != 24) || st.channels < 1 || st.channels > 2) {
    daisyUploadError("Need mono/stereo 16/24-bit WAV");
    return false;
  }

  uint32_t frameBytes = (st.bits / 8) * st.channels;
  uint32_t totalSamples = st.dataSize / frameBytes;
  if (totalSamples == 0) {
    daisyUploadError("Empty WAV data");
    return false;
  }
  st.begun = spiMaster.beginSampleStream(st.pad, totalSamples);
  if (!st.begun) daisyUploadError("Daisy stream begin failed");
  return st.begun;
}

static void processDaisyUploadPcm(const uint8_t* data, size_t len) {
  DaisyUploadStreamState& st = s_daisyUpload;
  if (st.error || !st.begun || len == 0) return;

  const size_t frameBytes = (st.bits / 8) * st.channels;
  int16_t pcm[256];
  size_t pcmCount = 0;

  auto flushPcm = [&]() {
    if (pcmCount == 0) return;
    // Backpressure instead of dropping: WiFi delivers faster than the SPI link
    // drains the queue, so wait for room rather than lose audio (which corrupted
    // the sample / "data chunk failed"). Yields so pumpDaisyUpload (Core0) drains
    // to the Daisy; TCP naturally slows the sender while we wait.
    uint32_t tries = 0;
    while (!pushDaisyUploadBlock(pcm, (uint16_t)pcmCount)) {
      if (st.error) return;
      if (++tries > 3000) { daisyUploadError("Daisy upload stalled"); return; }
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    pcmCount = 0;
  };

  size_t pos = 0;
  uint8_t frame[8];
  while (pos < len && !st.error) {
    size_t need = frameBytes - st.carryLen;
    size_t take = min(need, len - pos);
    memcpy(st.carry + st.carryLen, data + pos, take);
    st.carryLen += take;
    pos += take;
    if (st.carryLen < frameBytes) break;

    memcpy(frame, st.carry, frameBytes);
    st.carryLen = 0;
    if (st.bits == 16) {
      if (st.channels == 1) {
        pcm[pcmCount++] = (int16_t)le16(frame);
      } else {
        int16_t l = (int16_t)le16(frame);
        int16_t r = (int16_t)le16(frame + 2);
        pcm[pcmCount++] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
      }
    } else {
      if (st.channels == 1) {
        pcm[pcmCount++] = wav_s24_to_s16(frame);
      } else {
        int16_t l = wav_s24_to_s16(frame);
        int16_t r = wav_s24_to_s16(frame + 3);
        pcm[pcmCount++] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
      }
    }
    if (pcmCount == 256) flushPcm();
    if ((pos & 0x0FFF) == 0) {
      yield();
      esp_task_wdt_reset();
    }
  }
  flushPcm();
}

void WebInterface::handleDaisyUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    s_daisyUpload = DaisyUploadStreamState();
    resetDaisyUploadQueue();
    s_daisyUpload.active = true;
    s_daisyUpload.header = daisyUploadHeaderBuf();
    if (!s_daisyUpload.header) daisyUploadError("No PSRAM for WAV header buffer");
    if (request->hasParam("pad")) {
      s_daisyUpload.pad = request->getParam("pad")->value().toInt();
    } else if (request->hasParam("pad", false)) {
      s_daisyUpload.pad = request->getParam("pad", false)->value().toInt();
    } else {
      daisyUploadError("Missing pad parameter");
    }
    if (s_daisyUpload.pad < 0 || s_daisyUpload.pad >= MAX_SAMPLES) daisyUploadError("Invalid pad number");
    if (!filename.endsWith(".wav") && !filename.endsWith(".WAV")) daisyUploadError("Only WAV files are supported");
    if (request->contentLength() == 0) daisyUploadError("Unknown file size (Content-Length missing)");
    if (request->contentLength() > (8 * 1024 * 1024)) daisyUploadError("File too large (max 8MB)");
    strncpy(s_daisyUpload.filename, filename.c_str(), sizeof(s_daisyUpload.filename) - 1);
  }

  DaisyUploadStreamState& st = s_daisyUpload;
  if (!st.error && len > 0) {
    size_t chunkStart = st.fileOffset;
    st.fileOffset += len;

    if (!st.begun) {
      size_t copy = min(len, kDaisyHeaderCap - st.headerLen);
      memcpy(st.header + st.headerLen, data, copy);
      st.headerLen += copy;
      if (!parseDaisyUploadHeader() && !st.error && st.headerLen >= kDaisyHeaderCap) {
        daisyUploadError("WAV header too large");
      }
    }

    if (st.begun && !st.error) {
      size_t dataStart = 0;
      size_t dataEnd = len;
      size_t chunkEnd = chunkStart + len;
      if (chunkEnd <= st.dataPos || chunkStart >= st.dataPos + st.dataSize) {
        dataEnd = 0;
      } else {
        if (chunkStart < st.dataPos) dataStart = st.dataPos - chunkStart;
        if (chunkEnd > st.dataPos + st.dataSize) dataEnd = st.dataPos + st.dataSize - chunkStart;
      }
      if (dataEnd > dataStart) processDaisyUploadPcm(data + dataStart, dataEnd - dataStart);
    }
  }

  if (final) {
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Upload received, streaming to Daisy...\"}");
  }
}

// ── pumpCleanTrackStream ─────────────────────────────────────────────────────
// Called from update() (SystemTask, Core 0) every 2 ms.
// Streams a WAV file already saved in LittleFS to the Daisy Seed in chunks,
// without ever blocking the AsyncWebServer callback task.
static void pumpCleanTrackStream() {
  if (!s_cleanTrackStream.active) return;

  uint32_t nowMs = millis();
  if (s_cleanTrackStream.startedMs == 0) {
    s_cleanTrackStream.startedMs = nowMs;
    s_cleanTrackStream.lastProgressMs = nowMs;
  }
  if (!s_cleanTrackStream.error) {
    if ((uint32_t)(nowMs - s_cleanTrackStream.startedMs) > kCleanTrackStreamTotalTimeoutMs) {
      strlcpy(s_cleanTrackStream.errorMsg, "stream-timeout", sizeof(s_cleanTrackStream.errorMsg));
      s_cleanTrackStream.error = true;
    } else if (s_cleanTrackStream.begun
               && (uint32_t)(nowMs - s_cleanTrackStream.lastProgressMs) > kCleanTrackStreamNoProgressTimeoutMs) {
      strlcpy(s_cleanTrackStream.errorMsg, "stream-stalled", sizeof(s_cleanTrackStream.errorMsg));
      s_cleanTrackStream.error = true;
    }
  }

  // ── Phase 1: open file, parse WAV header, begin SPI stream ──────────────
  if (!s_cleanTrackStream.begun) {
    File f = LittleFS.open(s_cleanTrackStream.filePath, "r");
    if (!f) {
      strlcpy(s_cleanTrackStream.errorMsg, "open-failed", sizeof(s_cleanTrackStream.errorMsg));
      s_cleanTrackStream.error = true;
    } else {
      char herr[96] = {};
      uint16_t ch = 0, bits = 0;
      uint32_t dataOff = 0, dataSz = 0;
      if (!parseWavFileHeader(f, ch, bits, dataOff, dataSz, herr, sizeof(herr)) || dataSz == 0) {
        strlcpy(s_cleanTrackStream.errorMsg, herr[0] ? herr : "bad-header", sizeof(s_cleanTrackStream.errorMsg));
        s_cleanTrackStream.error = true;
        f.close();
      } else {
        const uint32_t frameBytes  = (bits / 8) * ch;
        const uint32_t totSamples  = dataSz / frameBytes;
        if (!spiMaster.beginCleanTrackStream(s_cleanTrackStream.slot, totSamples)) {
          strlcpy(s_cleanTrackStream.errorMsg, "daisy-begin-failed", sizeof(s_cleanTrackStream.errorMsg));
          s_cleanTrackStream.error = true;
          f.close();
        } else if (!f.seek(dataOff, SeekSet)) {
          strlcpy(s_cleanTrackStream.errorMsg, "seek-failed", sizeof(s_cleanTrackStream.errorMsg));
          spiMaster.endCleanTrackStream(s_cleanTrackStream.slot, false, 0);
          s_cleanTrackStream.error = true;
          f.close();
        } else {
          s_cleanTrackStream.channels       = ch;
          s_cleanTrackStream.bits           = bits;
          s_cleanTrackStream.bytesRemaining = dataSz;
          s_cleanTrackStream.totalSamples   = totSamples;
          s_cleanTrackStream.samplesSent    = 0;
          s_cleanTrackStream.carryLen       = 0;
          s_cleanTrackStream.begun          = true;
          s_cleanTrackStreamFile = f;  // keep file open across pumps
        }
      }
    }
    // On init error fall through to finalise below; on success return and wait for next pump
    if (!s_cleanTrackStream.error) return;
  }

  // ── Phase 2: read-decode-send up to 4 chunks per pump tick ───────────────
  if (!s_cleanTrackStream.error && s_cleanTrackStream.bytesRemaining > 0) {
    uint8_t inBuf[1024];
    uint8_t parseBuf[8 + sizeof(inBuf)];   // 8 = max carry size
    int16_t pcm[256];
    const uint32_t frameBytes = (uint32_t)(s_cleanTrackStream.bits / 8) * s_cleanTrackStream.channels;

    for (int pumps = 0; pumps < 4 && s_cleanTrackStream.bytesRemaining > 0 && !s_cleanTrackStream.error; ++pumps) {
      size_t toRead  = min((size_t)sizeof(inBuf), (size_t)s_cleanTrackStream.bytesRemaining);
      size_t readNow = s_cleanTrackStreamFile.read(inBuf, toRead);
      if (readNow == 0) {
        strlcpy(s_cleanTrackStream.errorMsg, "unexpected-eof", sizeof(s_cleanTrackStream.errorMsg));
        s_cleanTrackStream.error = true;
        break;
      }
      s_cleanTrackStream.bytesRemaining -= (uint32_t)readNow;

      memcpy(parseBuf, s_cleanTrackStream.carry, s_cleanTrackStream.carryLen);
      memcpy(parseBuf + s_cleanTrackStream.carryLen, inBuf, readNow);
      size_t parseLen  = s_cleanTrackStream.carryLen + readNow;
      size_t fullBytes = parseLen - (parseLen % frameBytes);
      size_t offset    = 0;

      while (offset + frameBytes <= fullBytes && !s_cleanTrackStream.error) {
        uint16_t pcmCount = 0;
        while (offset + frameBytes <= fullBytes && pcmCount < 256) {
          const uint8_t* frame = parseBuf + offset;
          if (s_cleanTrackStream.bits == 16) {
            if (s_cleanTrackStream.channels == 1) {
              pcm[pcmCount++] = (int16_t)le16(frame);
            } else {
              int16_t l = (int16_t)le16(frame);
              int16_t r = (int16_t)le16(frame + 2);
              pcm[pcmCount++] = (int16_t)(((int32_t)l + r) / 2);
            }
          } else {
            if (s_cleanTrackStream.channels == 1) {
              pcm[pcmCount++] = wav_s24_to_s16(frame);
            } else {
              int16_t l = wav_s24_to_s16(frame);
              int16_t r = wav_s24_to_s16(frame + 3);
              pcm[pcmCount++] = (int16_t)(((int32_t)l + r) / 2);
            }
          }
          offset += frameBytes;
        }
        if (pcmCount > 0) {
          if (!spiMaster.writeCleanTrackStreamData(s_cleanTrackStream.slot, pcm, pcmCount, s_cleanTrackStream.samplesSent)) {
            strlcpy(s_cleanTrackStream.errorMsg, "spi-write-failed", sizeof(s_cleanTrackStream.errorMsg));
            s_cleanTrackStream.error = true;
          } else {
            s_cleanTrackStream.samplesSent += pcmCount;
            s_cleanTrackStream.lastProgressMs = millis();
          }
        }
      }

      s_cleanTrackStream.carryLen = parseLen - fullBytes;
      if (s_cleanTrackStream.carryLen > 0) {
        memcpy(s_cleanTrackStream.carry, parseBuf + fullBytes, s_cleanTrackStream.carryLen);
      }
      esp_task_wdt_reset();
    }
  }

  // ── Phase 3: finalise as a non-blocking state machine ────────────────────
  // Each tick spends < 5 ms. No delay() / no synchronous SPI loops.
  bool streamDone = s_cleanTrackStream.error
                 || (s_cleanTrackStream.bytesRemaining == 0 && s_cleanTrackStream.carryLen == 0);
  if (!streamDone) return;

  // Phase transition table:
  //   0 = streaming done, ready to send CMD_SAMPLE_END
  //   1 = end sent, kick off first status request
  //   2 = waiting for status snapshot, retry every 30 ms, max 5 attempts (~150 ms)
  //   3 = publish (persist FS + broadcast WS, deferred from HTTP handler)
  //   4 = idle / queue next
  if (s_cleanTrackStream.finalizePhase == 0) {
    if (!s_cleanTrackStream.error && s_cleanTrackStream.samplesSent != s_cleanTrackStream.totalSamples) {
      strlcpy(s_cleanTrackStream.errorMsg, "sample-count-mismatch", sizeof(s_cleanTrackStream.errorMsg));
      s_cleanTrackStream.error = true;
    }
    bool ok = !s_cleanTrackStream.error;
    if (s_cleanTrackStreamFile) s_cleanTrackStreamFile.close();
    // Solo enviar endCleanTrackStream si llegamos a enviar begin a la Daisy.
    // Sin esta guarda, errores tempranos (open-failed, bad-header,
    // daisy-begin-failed) terminaban mandando un END sin BEGIN previo,
    // dejando a la Daisy en estado inconsistente y rompiendo la siguiente
    // subida (sintoma: "el primero no se inicializa").
    if (s_cleanTrackStream.begun) {
      spiMaster.endCleanTrackStream(s_cleanTrackStream.slot, ok, s_cleanTrackStream.samplesSent);
    }
    s_cleanTrackStream.finalizeStartMs = millis();
    s_cleanTrackStream.lastVerifyReqMs = 0;
    s_cleanTrackStream.verifyAttempts  = 0;
    s_cleanTrackStream.verified        = false;
    s_cleanTrackStream.finalizePhase   = ok ? 1 : 3;  // skip verify on error
    return;  // next tick continues
  }

  if (s_cleanTrackStream.finalizePhase == 1) {
    // Esperar ~250 ms tras el END para que Daisy procese CMD_SAMPLE_END
    // (drenar cola SPI con los chunks pendientes + ejecutar handler que
    // marca cleanTrackLoaded[track]=true). Sin este delay el primer
    // requestStatus se enviaba antes de que Daisy actualizase el bit.
    if ((uint32_t)(millis() - s_cleanTrackStream.finalizeStartMs) < 250) {
      return;  // wait
    }
    // Kick off first status request (async via queue; Core1 will dispatch)
    spiMaster.requestStatus();
    s_cleanTrackStream.lastVerifyReqMs = millis();
    s_cleanTrackStream.verifyAttempts  = 1;
    s_cleanTrackStream.finalizePhase   = 2;
    return;
  }

  if (s_cleanTrackStream.finalizePhase == 2) {
    int slot = s_cleanTrackStream.slot;
    StatusResponse st = {};
    if (spiMaster.getStatusSnapshot(st)) {
      if ((st.cleanTrackLoadedMask & (1u << slot)) != 0) {
        s_cleanTrackStream.verified      = true;
        s_cleanTrackStream.finalizePhase = 3;
        return;
      }
    }
    // Retry cada ~100 ms hasta 30 intentos (~3 s total). El ciclo SPI
    // requestStatus -> Daisy responde -> snapshot updated puede tardar
    // varios ticks cuando la cola SPI venia con cientos de chunks de
    // datos pendientes. 200 ms era demasiado corto y daba 'daisy-not-loaded'
    // sistematicamente.
    if ((uint32_t)(millis() - s_cleanTrackStream.lastVerifyReqMs) >= 100) {
      if (s_cleanTrackStream.verifyAttempts >= 30) {
        StatusResponse finalSt = {};
        uint32_t mask = 0;
        if (spiMaster.getStatusSnapshot(finalSt)) {
          mask = finalSt.cleanTrackLoadedMask;
        }
        syslog("STEM", "verify failed slot=%d mask=0x%02X sent=%lu total=%lu attempts=%u",
               slot, (unsigned)mask,
               (unsigned long)s_cleanTrackStream.samplesSent,
               (unsigned long)s_cleanTrackStream.totalSamples,
               s_cleanTrackStream.verifyAttempts);
        s_cleanTrackStream.finalizePhase = 3;  // give up, mark not-loaded
        return;
      }
      spiMaster.requestStatus();
      s_cleanTrackStream.lastVerifyReqMs = millis();
      s_cleanTrackStream.verifyAttempts++;
    }
    return;
  }

  if (s_cleanTrackStream.finalizePhase == 3) {
    int  slot = s_cleanTrackStream.slot;
    bool ok   = !s_cleanTrackStream.error;
    if (ok && s_cleanTrackStream.verified) {
      s_cleanTracks[slot].loaded     = true;
      s_cleanTracks[slot].loadFailed = false;
      // Auto-arm clean track tras upload exitoso para que suene al pulsar Play.
      // Antes, armed=false por defecto -> setCleanTrackActive(false) -> stem silencioso
      // a pesar del toast de éxito. El usuario espera que un stem recién subido suene.
      s_cleanTracks[slot].armed = true;
      s_cleanTracks[slot].muted = false;
      spiMaster.setCleanTrackActive(slot, true);
      spiMaster.setCleanTrackMute(slot, false);
      strlcpy(s_cleanTracks[slot].status, "loaded", sizeof(s_cleanTracks[slot].status));
    } else if (ok) {
      s_cleanTracks[slot].loaded     = false;
      s_cleanTracks[slot].loadFailed = true;  // evita re-encolado infinito
      strlcpy(s_cleanTracks[slot].status, "daisy-not-loaded", sizeof(s_cleanTracks[slot].status));
    } else {
      s_cleanTracks[slot].loaded     = false;
      s_cleanTracks[slot].loadFailed = true;
      strlcpy(s_cleanTracks[slot].status,
              s_cleanTrackStream.errorMsg[0] ? s_cleanTrackStream.errorMsg : "load-error",
              sizeof(s_cleanTracks[slot].status));
    }
    s_cleanTrackStream.finalizePhase = 4;
    // Persist + broadcast deferred to NEXT tick to keep this one short
    return;
  }

  // Phase 4: publish + reset + queue next (split work across calls)
  syncCleanTrackStateFromDaisy();
  saveCleanTracksStateToFs();
  webInterface.broadcastSequencerState();
  s_cleanTrackStream = CleanTrackStreamState{};  // reset all fields
  queueNextStoredCleanTrackStream();
  if (s_cleanTrackStream.active) {
    saveCleanTracksStateToFs();
    webInterface.broadcastSequencerState();
  }
}

static void pumpDaisyUpload() {
  if (s_daisyUpload.active && s_daisyUpload.begun && !s_daisyUpload.error) {
    DaisyUploadPcmBlock block;
    uint8_t drained = 0;
    while (drained < 6 && popDaisyUploadBlock(block)) {
      bool sent = false;
      for (int r = 0; r < 3 && !sent; r++) {
        sent = spiMaster.writeSampleStreamData(s_daisyUpload.pad, block.samples, block.count, s_daisyUpload.samplesSent);
        if (!sent) vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (!sent) {
        daisyUploadError("Daisy data chunk failed");
        break;
      }
      s_daisyUpload.samplesSent += block.count;
      drained++;
      esp_task_wdt_reset();
      yield();
    }
  }

  if (s_daisyUpload.active) {
    bool uploadFinished = (s_daisyUpload.dataSize > 0)
      && (s_daisyUpload.fileOffset >= (size_t)s_daisyUpload.dataPos + (size_t)s_daisyUpload.dataSize);
    bool queueEmpty = (s_daisyUploadQueueCount == 0);
    if (s_daisyUpload.error || (uploadFinished && queueEmpty && s_daisyUpload.carryLen == 0)) {
      bool ok = s_daisyUpload.begun && !s_daisyUpload.error && s_daisyUpload.samplesSent > 0;
      if (s_daisyUpload.begun) spiMaster.endSampleStream(s_daisyUpload.pad, ok, s_daisyUpload.samplesSent);
      if (ok) {
        // Tras subir una stem/sample a un pad del secuenciador, forzar
        // engine=-1 (sampler) en la Daisy: si el track estaba en 808 por el
        // fallback de boot (SD sin kit), el sample subido NO sonaba porque
        // el secuenciador y el trigger live seguian disparando synth808.
        if (s_daisyUpload.pad >= 0 && s_daisyUpload.pad < 16) {
          setTrackSynthEngine(s_daisyUpload.pad, -1);
          spiMaster.dsqSetTrackEngine((uint8_t)s_daisyUpload.pad, -1);
          spiMaster.dsqSetMute((uint8_t)s_daisyUpload.pad, false);
        }
        webInterface.broadcastUploadComplete(s_daisyUpload.pad, true, "Sample streamed to Daisy");
        webInterface.broadcastSequencerState();
      } else {
        const char* msg = s_daisyUpload.errorMsg[0] ? s_daisyUpload.errorMsg : "Daisy upload failed";
        webInterface.broadcastUploadComplete(s_daisyUpload.pad, false, msg);
      }
      resetDaisyUploadQueue();
      s_daisyUpload = DaisyUploadStreamState();
    }
  }
}

void WebInterface::handleCleanTrackUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    if (s_cleanTrackUpload.file) {
      s_cleanTrackUpload.file.close();
    }
    memset(&s_cleanTrackUpload, 0, sizeof(s_cleanTrackUpload));
    s_cleanTrackLastInitError[0] = 0;
    s_cleanTrackUpload.active = true;

    if (!filename.endsWith(".wav") && !filename.endsWith(".WAV")) {
      sendCleanTrackInitError(request, 400, "invalid_wav", "Only WAV files are supported");
      s_cleanTrackUpload.active = false;
      return;
    }
    if (request->contentLength() == 0 || request->contentLength() > (8 * 1024 * 1024)) {
      sendCleanTrackInitError(request, 400, "invalid_size", "Invalid file size (max 8MB)");
      s_cleanTrackUpload.active = false;
      return;
    }

    // NOTA: no rechazamos aqui aunque el pump este streaming otro stem.
    // Si lo rechazaramos con 503, el PRIMER upload del usuario tras un boot
    // (donde el pump esta cargando stems persistidos en LittleFS) fallaba con
    // "el primero no se inicializa". En su lugar guardamos el WAV en
    // LittleFS y queueNextStoredCleanTrackStream() lo recogera cuando el
    // pump termine la stream en curso.

    // Find the slot FIRST: findFirstFreeCleanTrackSlot() reclaims failed slots
    // and deletes their orphaned files, which frees FS space. Doing this before
    // the space check lets a fresh upload reuse the room left by failed ones.
    int slot = findFirstFreeCleanTrackSlot();
    if (slot < 0) {
      sendCleanTrackInitError(request, 409, "no_free_slots", "No free clean track available");
      s_cleanTrackUpload.active = false;
      return;
    }

    // Validate free FS space (uploaded file size + small margin for FS metadata).
    {
      size_t totalB = LittleFS.totalBytes();
      size_t usedB  = LittleFS.usedBytes();
      size_t freeB  = (totalB > usedB) ? (totalB - usedB) : 0;
      size_t needB  = (size_t)request->contentLength() + 8192;
      if (freeB < needB) {
        String fsMsg = String("Insufficient storage (free ") + (uint32_t)(freeB/1024) +
                       "KB, need " + (uint32_t)(needB/1024) + "KB)";
        sendCleanTrackInitError(request, 507, "fs_full", fsMsg.c_str());
        s_cleanTrackUpload.active = false;
        return;
      }
    }

    if (!ensureCleanTracksDir()) {
      sendCleanTrackInitError(request, 500, "mkdir_failed", "Failed to prepare clean track storage");
      s_cleanTrackUpload.active = false;
      return;
    }

    String safeName = cleanTrackSafeName(filename);
    String path = String(kCleanTracksDir) + "/slot_" + String(slot) + "_" + safeName;
    LittleFS.remove(path);

    s_cleanTrackUpload.slot = slot;
    strlcpy(s_cleanTrackUpload.filename, filename.c_str(), sizeof(s_cleanTrackUpload.filename));
    strlcpy(s_cleanTrackUpload.filePath, path.c_str(), sizeof(s_cleanTrackUpload.filePath));
    s_cleanTrackUpload.file = LittleFS.open(path, "w");
    if (!s_cleanTrackUpload.file) {
      sendCleanTrackInitError(request, 500, "open_failed", "Failed to create clean track file");
      s_cleanTrackUpload.active = false;
      return;
    }
  }

  if (!s_cleanTrackUpload.active || !s_cleanTrackUpload.file) {
    if (final) {
      StaticJsonDocument<256> response;
      response["success"] = false;
      response["reason"] = "not_initialized";
      response["lastInitError"] = s_cleanTrackLastInitError[0] ? s_cleanTrackLastInitError : "none";
      response["message"] = String("Clean track not initialized: ") +
                            (s_cleanTrackLastInitError[0] ? s_cleanTrackLastInitError : "unknown");
      String output;
      serializeJson(response, output);
      request->send(500, "application/json", output);
    }
    return;
  }

  if (len > 0) {
    size_t written = s_cleanTrackUpload.file.write(data, len);
    if (written != len) {
      s_cleanTrackUpload.error = true;
      strlcpy(s_cleanTrackUpload.errorMsg, "Failed writing clean track file", sizeof(s_cleanTrackUpload.errorMsg));
    } else {
      s_cleanTrackUpload.bytesWritten += written;
    }
  }

  if (final) {
    s_cleanTrackUpload.file.close();
    if (s_cleanTrackUpload.error) {
      LittleFS.remove(s_cleanTrackUpload.filePath);
      request->send(500, "application/json", "{\"success\":false,\"message\":\"Failed writing clean track file\"}");
      memset(&s_cleanTrackUpload, 0, sizeof(s_cleanTrackUpload));
      return;
    }

    int slot = s_cleanTrackUpload.slot;
    clearCleanTrackSlot(s_cleanTracks[slot], slot);
    s_cleanTracks[slot].occupied  = true;
    s_cleanTracks[slot].loaded    = false;
    s_cleanTracks[slot].armed     = true;
    s_cleanTracks[slot].muted     = false;
    s_cleanTracks[slot].movable   = true;
    s_cleanTracks[slot].sizeBytes = (uint32_t)s_cleanTrackUpload.bytesWritten;
    strlcpy(s_cleanTracks[slot].clipName, s_cleanTrackUpload.filename, sizeof(s_cleanTracks[slot].clipName));
    strlcpy(s_cleanTracks[slot].filePath, s_cleanTrackUpload.filePath, sizeof(s_cleanTracks[slot].filePath));

    // Defer SPI streaming to pumpCleanTrackStream() in update() (SystemTask, Core 0).
    // This avoids blocking the AsyncWebServer callback task for the entire stream duration,
    // which was causing "cannot connect" / connection-drop issues during large WAV uploads.
    if (s_cleanTrackStream.active) {
      // A previous stream is still in progress \u2014 mark as stored, user can reload later
      strlcpy(s_cleanTracks[slot].status, "stored", sizeof(s_cleanTracks[slot].status));
    } else {
      queueCleanTrackStreamSlot(slot);
    }

    // NOTE: do NOT call spiMaster.requestStatus(), saveCleanTracksStateToFs() or
    // broadcastSequencerState() here \u2014 those block the AsyncTCP task. The pump's
    // Phase 3/4 will persist + broadcast once Daisy confirms the load.

    StaticJsonDocument<256> response;
    response["success"]      = true;
    response["message"]      = s_cleanTrackStream.active ? "Clean track received, streaming to Daisy" : "Clean track stored";
    response["cleanTrackId"] = slot;
    response["clipName"]     = s_cleanTracks[slot].clipName;
    response["sizeBytes"]    = s_cleanTracks[slot].sizeBytes;
    response["loaded"]       = false;
    response["streaming"]    = s_cleanTrackStream.active;
    String output;
    serializeJson(response, output);
    request->send(200, "application/json", output);
    memset(&s_cleanTrackUpload, 0, sizeof(s_cleanTrackUpload));
  }
}

void WebInterface::broadcastUploadProgress(int pad, int percent) {
  if (!initialized || !ws) return;
  
  StaticJsonDocument<128> doc;
  doc["type"] = "uploadProgress";
  doc["pad"] = pad;
  doc["percent"] = percent;
  
  String output;
  serializeJson(doc, output);
  ws->textAll(output);
}

void WebInterface::broadcastUploadComplete(int pad, bool success, const String& message) {
  if (!initialized || !ws) return;
  
  StaticJsonDocument<256> doc;
  doc["type"] = "uploadComplete";
  doc["pad"] = pad;
  doc["success"] = success;
  doc["message"] = message;
  
  String output;
  serializeJson(doc, output);
  ws->textAll(output);
}

void WebInterface::broadcastRaw(const char* json) {
  if (!initialized || !ws || ws->count() == 0) return;
  if (ESP.getFreeHeap() < 20000) return;   // drop if heap critical
  ws->textAll(json);
}

