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
  strlcpy(s_daisyUpload.errorMsg, msg ? msg : "Daisy upload failed", sizeof(s_daisyUpload.errorMsg));
}

void WebInterface::handleDaisyUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    // An abandoned request may be reclaimed, but never reset a live request or
    // a file currently being streamed to the Daisy.
    if (s_daisyUpload.receiving &&
        (uint32_t)(millis() - s_daisyUpload.lastProgressMs) > kUploadReceiveTimeoutMs) {
      if (s_daisyUploadFile) s_daisyUploadFile.close();
      LittleFS.remove(kDaisyUploadTempPath);
      s_daisyUpload = DaisyUploadStreamState{};
    }
    if (s_daisyUpload.receiving || s_daisyUpload.active) {
      request->send(409, "application/json",
                    "{\"success\":false,\"reason\":\"upload_busy\",\"message\":\"Another pad upload is active\"}");
      return;
    }

    if (s_daisyUploadFile) s_daisyUploadFile.close();
    LittleFS.remove(kDaisyUploadTempPath);
    s_daisyUpload = DaisyUploadStreamState();
    s_daisyUpload.receiving = true;
    s_daisyUpload.owner = request;
    s_daisyUpload.totalSize = request->contentLength();
    s_daisyUpload.lastProgressMs = millis();
    if (request->hasParam("pad")) {
      s_daisyUpload.pad = request->getParam("pad")->value().toInt();
    } else if (request->hasParam("pad", false)) {
      s_daisyUpload.pad = request->getParam("pad", false)->value().toInt();
    } else {
      daisyUploadError("Missing pad parameter");
    }
    if (s_daisyUpload.pad < 0 || s_daisyUpload.pad >= MAX_SAMPLES) daisyUploadError("Invalid pad number");
    if (!filename.endsWith(".wav") && !filename.endsWith(".WAV")) daisyUploadError("Only WAV files are supported");
    if (s_daisyUpload.totalSize == 0) daisyUploadError("Unknown file size (Content-Length missing)");
    if (s_daisyUpload.totalSize > (8 * 1024 * 1024)) daisyUploadError("File too large (max 8MB)");
    strlcpy(s_daisyUpload.filename, filename.c_str(), sizeof(s_daisyUpload.filename));

    const size_t totalB = LittleFS.totalBytes();
    const size_t usedB = LittleFS.usedBytes();
    const size_t freeB = totalB > usedB ? totalB - usedB : 0;
    if (!s_daisyUpload.error && freeB < s_daisyUpload.totalSize + 8192) {
      daisyUploadError("Insufficient temporary storage");
    }
    if (!s_daisyUpload.error) {
      s_daisyUploadFile = LittleFS.open(kDaisyUploadTempPath, "w");
      if (!s_daisyUploadFile) daisyUploadError("Failed to create temporary upload file");
    }
    if (s_daisyUpload.error) {
      const int failedPad = s_daisyUpload.pad;
      const String msg = s_daisyUpload.errorMsg;
      if (s_daisyUploadFile) s_daisyUploadFile.close();
      LittleFS.remove(kDaisyUploadTempPath);
      s_daisyUpload = DaisyUploadStreamState{};
      broadcastUploadComplete(failedPad, false, msg);
      request->send(400, "application/json", String("{\"success\":false,\"message\":\"") + msg + "\"}");
      return;
    }
  }

  DaisyUploadStreamState& st = s_daisyUpload;
  // Chunks belonging to a rejected/concurrent request are ignored. The 409 was
  // already sent from its index==0 callback.
  if (!st.receiving || st.owner != request) return;

  if (!st.error && len > 0) {
    if (st.bytesWritten > st.totalSize || len > st.totalSize - st.bytesWritten) {
      daisyUploadError("Upload exceeds declared Content-Length");
    } else {
      const size_t written = s_daisyUploadFile.write(data, len);
      if (written != len) daisyUploadError("Failed writing temporary upload file");
      else st.bytesWritten += written;
      st.lastProgressMs = millis();
    }
  }

  if (final) {
    if (s_daisyUploadFile) s_daisyUploadFile.close();
    st.receiving = false;
    st.owner = nullptr;
    // NOTE: st.totalSize is the declared HTTP Content-Length of the whole
    // multipart body (boundaries + headers + file bytes), while st.bytesWritten
    // only counts the decoded file bytes handed to us by the multipart parser
    // (data/len already have the boundary overhead stripped). They are NOT
    // expected to be equal — comparing them here used to reject every single
    // upload with a false "Truncated upload" 400 right at 100%, both from the
    // web UI (/api/upload) and from the P4 SD uploader (/api/uploadDaisy).
    // The only truncation signal we actually have is whether the parser ever
    // called us with final=true and wrote at least one byte.
    if (!st.error && st.bytesWritten == 0) daisyUploadError("Empty upload");
    if (st.error) {
      const int failedPad = st.pad;
      const String msg = st.errorMsg;
      LittleFS.remove(kDaisyUploadTempPath);
      s_daisyUpload = DaisyUploadStreamState{};
      broadcastUploadComplete(failedPad, false, msg);
      request->send(400, "application/json", String("{\"success\":false,\"message\":\"") + msg + "\"}");
      return;
    }
    st.active = true;
    st.startedMs = millis();
    st.lastProgressMs = st.startedMs;
    request->send(202, "application/json", "{\"success\":true,\"message\":\"Upload staged, streaming to Daisy...\"}");
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
      feedTaskWdtIfSubscribed();
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
  DaisyUploadStreamState& st = s_daisyUpload;

  if (st.receiving && (uint32_t)(millis() - st.lastProgressMs) > kUploadReceiveTimeoutMs) {
    if (s_daisyUploadFile) s_daisyUploadFile.close();
    LittleFS.remove(kDaisyUploadTempPath);
    webInterface.broadcastUploadComplete(st.pad, false, "Upload receive timeout");
    st = DaisyUploadStreamState{};
    return;
  }
  if (!st.active) return;

  if (!st.error && !st.begun) {
    s_daisyUploadFile = LittleFS.open(kDaisyUploadTempPath, "r");
    if (!s_daisyUploadFile) {
      daisyUploadError("Failed opening staged upload");
    } else {
      char wavErr[96] = {};
      if (!parseWavFileHeader(s_daisyUploadFile, st.channels, st.bits,
                              st.dataOffset, st.dataSize, wavErr, sizeof(wavErr))) {
        daisyUploadError(wavErr);
      } else {
        const uint32_t frameBytes = (st.bits / 8u) * st.channels;
        st.totalSamples = st.dataSize / frameBytes;
        st.bytesRemaining = st.dataSize;
        st.begun = spiMaster.beginSampleStream(st.pad, st.totalSamples);
        if (!st.begun) daisyUploadError("Daisy stream begin failed");
      }
    }
  }

  if (!st.error && st.begun && st.bytesRemaining > 0) {
    const size_t frameBytes = (st.bits / 8u) * st.channels;
    const size_t frames = min((size_t)256, (size_t)(st.bytesRemaining / frameBytes));
    const size_t bytesToRead = frames * frameBytes;
    uint8_t raw[256 * 6] = {};
    int16_t pcm[256] = {};
    const size_t readPos = s_daisyUploadFile.position();
    if (frames == 0 || bytesToRead > sizeof(raw) ||
        s_daisyUploadFile.read(raw, bytesToRead) != bytesToRead) {
      daisyUploadError("Truncated staged WAV data");
    } else {
      for (size_t i = 0; i < frames; ++i) {
        const uint8_t* frame = raw + i * frameBytes;
        if (st.bits == 16) {
          if (st.channels == 1) pcm[i] = (int16_t)le16(frame);
          else {
            const int16_t l = (int16_t)le16(frame);
            const int16_t r = (int16_t)le16(frame + 2);
            pcm[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
          }
        } else if (st.channels == 1) {
          pcm[i] = wav_s24_to_s16(frame);
        } else {
          const int16_t l = wav_s24_to_s16(frame);
          const int16_t r = wav_s24_to_s16(frame + 3);
          pcm[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        }
      }

      if (spiMaster.writeSampleStreamData(st.pad, pcm, (uint16_t)frames, st.samplesSent)) {
        st.samplesSent += frames;
        st.bytesRemaining -= bytesToRead;
        st.lastProgressMs = millis();
      } else {
        // Preserve the chunk for the next update tick. No waiting occurs in the
        // network callback or in this bounded pump invocation.
        s_daisyUploadFile.seek(readPos, SeekSet);
        if ((uint32_t)(millis() - st.lastProgressMs) > kDaisyUploadNoProgressTimeoutMs) {
          daisyUploadError("Daisy upload stalled");
        }
      }
    }
    feedTaskWdtIfSubscribed();
  }

  if (st.error || (st.begun && st.bytesRemaining == 0)) {
      const bool ok = st.begun && !st.error && st.samplesSent == st.totalSamples;
      if (st.begun) spiMaster.endSampleStream(st.pad, ok, st.samplesSent);
      if (s_daisyUploadFile) s_daisyUploadFile.close();
      LittleFS.remove(kDaisyUploadTempPath);
      if (ok) {
        // Tras subir una stem/sample a un pad del secuenciador, forzar
        // engine=-1 (sampler) en la Daisy: si el track estaba en 808 por el
        // fallback de boot (SD sin kit), el sample subido NO sonaba porque
        // el secuenciador y el trigger live seguian disparando synth808.
        if (st.pad >= 0 && st.pad < 16) {
          setTrackSynthEngine(st.pad, -1);
          spiMaster.dsqSetTrackEngine((uint8_t)st.pad, -1);
          spiMaster.dsqSetMute((uint8_t)st.pad, false);
        }
        webInterface.broadcastUploadComplete(st.pad, true, "Sample streamed to Daisy");
        webInterface.broadcastSequencerState();
      } else {
        const char* msg = st.errorMsg[0] ? st.errorMsg : "Daisy upload failed";
        webInterface.broadcastUploadComplete(st.pad, false, msg);
      }
      st = DaisyUploadStreamState{};
  }
}

void WebInterface::handleCleanTrackUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    if (s_cleanTrackUpload.active && s_cleanTrackUpload.owner != request) {
      if ((uint32_t)(millis() - s_cleanTrackUpload.lastActivityMs) <= kUploadReceiveTimeoutMs) {
        request->send(409, "application/json",
                      "{\"success\":false,\"reason\":\"upload_busy\",\"message\":\"Another clean-track upload is active\"}");
        return;
      }
      if (s_cleanTrackUpload.file) s_cleanTrackUpload.file.close();
      if (s_cleanTrackUpload.filePath[0]) LittleFS.remove(s_cleanTrackUpload.filePath);
    }
    if (s_cleanTrackUpload.file) {
      s_cleanTrackUpload.file.close();
    }
    memset(&s_cleanTrackUpload, 0, sizeof(s_cleanTrackUpload));
    s_cleanTrackLastInitError[0] = 0;
    s_cleanTrackUpload.active = true;
    s_cleanTrackUpload.owner = request;
    s_cleanTrackUpload.lastActivityMs = millis();

    if (!filename.endsWith(".wav") && !filename.endsWith(".WAV")) {
      sendCleanTrackInitError(request, 400, "invalid_wav", "Only WAV files are supported");
      s_cleanTrackUpload.active = false;
      s_cleanTrackUpload.owner = nullptr;
      return;
    }
    if (request->contentLength() == 0 || request->contentLength() > (8 * 1024 * 1024)) {
      sendCleanTrackInitError(request, 400, "invalid_size", "Invalid file size (max 8MB)");
      s_cleanTrackUpload.active = false;
      s_cleanTrackUpload.owner = nullptr;
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
      s_cleanTrackUpload.owner = nullptr;
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
        s_cleanTrackUpload.owner = nullptr;
        return;
      }
    }

    if (!ensureCleanTracksDir()) {
      sendCleanTrackInitError(request, 500, "mkdir_failed", "Failed to prepare clean track storage");
      s_cleanTrackUpload.active = false;
      s_cleanTrackUpload.owner = nullptr;
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
      s_cleanTrackUpload.owner = nullptr;
      return;
    }
  }

  if (s_cleanTrackUpload.owner != request) return;

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
      s_cleanTrackUpload.lastActivityMs = millis();
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

