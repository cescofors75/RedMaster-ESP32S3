/*
 * SampleManager.cpp
 * Implementació del gestor de samples
 */

#include "SampleManager.h"

extern SPIMaster spiMaster;

SampleManager::SampleManager() {
  for (int i = 0; i < MAX_SAMPLES; i++) {
    sampleBuffers[i] = nullptr;
    sampleLengths[i] = 0;
    memset(sampleNames[i], 0, 32);
  }
}

SampleManager::~SampleManager() {
  unloadAll();
}

bool SampleManager::begin() {
  if (!psramFound()) {
    return false;
  }
  
  return true;
}

bool SampleManager::loadSample(const char* filename, int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) {
    return false;
  }
  
  // Unload existing sample if any
  if (sampleBuffers[padIndex] != nullptr) {
    unloadSample(padIndex);
  }
  
  // Open file
  fs::File file = LittleFS.open(filename, "r");
  if (!file) {
    return false;
  }
  
  bool success = false;
  String fname = String(filename);

  // DETECT .RAW OR .WAV
  if (fname.endsWith(".raw") || fname.endsWith(".RAW")) {
    // --- LOAD RAW (No Header, 16-bit signed, Mono) ---
    size_t fileSize = file.size();
    
    uint32_t numSamples = fileSize / 2; // 16-bit = 2 bytes per sample
    
    if (allocateSampleBuffer(padIndex, numSamples)) {
      size_t bytesRead = file.read((uint8_t*)sampleBuffers[padIndex], fileSize);
      if (bytesRead == fileSize) {
        sampleLengths[padIndex] = numSamples;
        success = true;
      } else {
        freeSampleBuffer(padIndex);
      }
    }
  } else {
    // --- LOAD WAV (With Header Parsing) ---
    String parseErr;
    success = parseWavFile(file, padIndex, parseErr);
    if (!success) {
      // Store last error for caller to surface
      strncpy(lastParseError, parseErr.c_str(), sizeof(lastParseError) - 1);
      lastParseError[sizeof(lastParseError) - 1] = '\0';
    }
  }
  
  file.close();
  
  if (!success) {
    return false; 
  }
  
  // Store sample name
  const char* name = strrchr(filename, '/');
  if (name) name++; // Skip '/'
  else name = filename;
  strncpy(sampleNames[padIndex], name, 31);
  sampleNames[padIndex][31] = '\0';
  
  // Register with SPI Master → STM32 audio slave
  spiMaster.setSampleBuffer(padIndex, sampleBuffers[padIndex], sampleLengths[padIndex]);
  
  
  return true;
}

bool SampleManager::parseWavFile(fs::File& file, int padIndex, String& errOut) {
  size_t fileSize = file.size();

  if (fileSize < 12) {
    errOut = "File too small";
    return false;
  }

  file.seek(0);

  // Verificar RIFF + WAVE
  uint8_t riff[12];
  if (file.read(riff, 12) != 12) { errOut = "Read RIFF failed"; return false; }
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    errOut = "Not a WAV file";
    return false;
  }

  // Caminar chunks para encontrar "fmt " y "data"
  uint32_t pos = 12;
  bool fmtFound  = false;
  bool dataFound = false;

  uint16_t audioFormat   = 0;
  uint16_t numChannels   = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataPos       = 0;
  uint32_t dataSize      = 0;

  while (pos + 8 <= (uint32_t)fileSize) {
    file.seek(pos);
    uint8_t hdr[8];
    if (file.read(hdr, 8) != 8) break;

    uint32_t chunkSize = (uint32_t)hdr[4]
                       | ((uint32_t)hdr[5] << 8)
                       | ((uint32_t)hdr[6] << 16)
                       | ((uint32_t)hdr[7] << 24);

    if (memcmp(hdr, "fmt ", 4) == 0) {
      if (chunkSize < 16) { errOut = "fmt chunk too small"; return false; }
      uint8_t fmt[16];
      if (file.read(fmt, 16) != 16) { errOut = "Read fmt failed"; return false; }

      audioFormat   = (uint16_t)fmt[0]  | ((uint16_t)fmt[1]  << 8);
      numChannels   = (uint16_t)fmt[2]  | ((uint16_t)fmt[3]  << 8);
      bitsPerSample = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
      fmtFound = true;

    } else if (memcmp(hdr, "data", 4) == 0) {
      dataPos  = pos + 8;   // posición justo después del header del chunk
      dataSize = chunkSize;
      // Clamp a los bytes realmente disponibles: un WAV corrupto puede declarar
      // un data chunk mayor que el archivo.
      if (dataPos > (uint32_t)fileSize) { errOut = "Bad data chunk offset"; return false; }
      if (dataSize > (uint32_t)fileSize - dataPos) dataSize = (uint32_t)fileSize - dataPos;
      dataFound = true;
      break;  // ya tenemos lo que necesitamos
    }

    // Avance con proteccion de overflow: chunkSize lo controla el archivo
    // (uint32 sin firmar). Sin esto, un valor enorme desbordaba `pos`, hacia
    // wrap el guard `pos + 8 <= fileSize` y podia provocar bucle infinito.
    uint32_t advance = chunkSize + (chunkSize & 1);
    if (chunkSize > (uint32_t)fileSize || advance > (uint32_t)fileSize - pos - 8) break;
    pos += 8 + advance;  // alineado a 2 bytes
  }

  if (!fmtFound)  { errOut = "No fmt chunk found";  return false; }
  if (!dataFound) { errOut = "No data chunk found"; return false; }

  // Aceptar PCM (1) y WAVE_FORMAT_EXTENSIBLE (0xFFFE, tratado como PCM 16-bit)
  if (audioFormat != 1 && audioFormat != 0xFFFE) {
    errOut = "Unsupported format " + String(audioFormat);
    return false;
  }

  // Aceptamos 16-bit nativo y 24-bit convertido a 16-bit
  if (bitsPerSample != 16 && bitsPerSample != 24) {
    errOut = "Need 16/24-bit PCM, got " + String(bitsPerSample) + "-bit";
    return false;
  }

  if (numChannels < 1 || numChannels > 2) {
    errOut = "Bad channel count " + String(numChannels);
    return false;
  }

  // Número de frames (muestras mono)
  uint32_t bytesPerSample = bitsPerSample / 8;
  uint32_t numSamples = dataSize / (bytesPerSample * numChannels);

  if (!allocateSampleBuffer(padIndex, numSamples)) {
    errOut = "No PSRAM for sample (free " + String((uint32_t)(ESP.getFreePsram() / 1024)) +
             "KB, need " + String((uint32_t)(numSamples * 2 / 1024)) + "KB)";
    return false;
  }

  file.seek(dataPos);

  if (bitsPerSample == 16) {
    if (numChannels == 1) {
      size_t bytesRead = file.read((uint8_t*)sampleBuffers[padIndex], numSamples * 2);
      if (bytesRead != numSamples * 2) {
        errOut = "Short read mono16";
        freeSampleBuffer(padIndex);
        return false;
      }
    } else {
      // Stereo 16-bit → mixdown mono
      int16_t stereoBuffer[2];
      for (uint32_t i = 0; i < numSamples; i++) {
        if (file.read((uint8_t*)stereoBuffer, 4) != 4) {
          errOut = "Short read stereo16";
          freeSampleBuffer(padIndex);
          return false;
        }
        sampleBuffers[padIndex][i] = (stereoBuffer[0] / 2) + (stereoBuffer[1] / 2);
        if ((i & 0x0FFF) == 0) {
          yield();
        }
      }
    }
  } else {
    // 24-bit PCM → convertir a 16-bit (descartar 8 bits menos significativos)
    auto s24_to_s16 = [](const uint8_t* b) -> int16_t {
      int32_t v = (int32_t)b[0] | ((int32_t)b[1] << 8) | ((int32_t)b[2] << 16);
      if (v & 0x00800000) {
        v |= ~0x00FFFFFF;  // sign-extend
      }
      return (int16_t)(v >> 8);
    };

    if (numChannels == 1) {
      uint8_t mono24[3];
      for (uint32_t i = 0; i < numSamples; i++) {
        if (file.read(mono24, 3) != 3) {
          errOut = "Short read mono24";
          freeSampleBuffer(padIndex);
          return false;
        }
        sampleBuffers[padIndex][i] = s24_to_s16(mono24);
        if ((i & 0x0FFF) == 0) {
          yield();
        }
      }
    } else {
      // Stereo 24-bit → mixdown mono
      uint8_t stereo24[6];
      for (uint32_t i = 0; i < numSamples; i++) {
        if (file.read(stereo24, 6) != 6) {
          errOut = "Short read stereo24";
          freeSampleBuffer(padIndex);
          return false;
        }
        int16_t l = s24_to_s16(&stereo24[0]);
        int16_t r = s24_to_s16(&stereo24[3]);
        sampleBuffers[padIndex][i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        if ((i & 0x0FFF) == 0) {
          yield();
        }
      }
    }
  }

  sampleLengths[padIndex] = numSamples;
  return true;
}

bool SampleManager::allocateSampleBuffer(int padIndex, uint32_t size) {
  // Comprobar el limite ANTES de multiplicar: size lo deriva el archivo y
  // `size * 2` podia desbordar size_t (32-bit) y pasar el check con un `bytes`
  // minusculo, provocando un overrun al copiar las muestras.
  if (size > MAX_SAMPLE_SIZE / sizeof(int16_t)) {
    return false;
  }
  size_t bytes = size * sizeof(int16_t);

  if (bytes > MAX_SAMPLE_SIZE) {
    return false;
  }
  
  // Verificar PSRAM disponible
  size_t freePsram = ESP.getFreePsram();
  size_t minRequired = bytes + (100 * 1024); // +100KB margen de seguridad
  
  if (freePsram < minRequired) {
    return false;
  }
  
  // Allocate in PSRAM
  sampleBuffers[padIndex] = (int16_t*)ps_malloc(bytes);
  
  if (sampleBuffers[padIndex] == nullptr) {
    return false;
  }
  
  return true;
}

void SampleManager::freeSampleBuffer(int padIndex) {
  if (sampleBuffers[padIndex] != nullptr) {
    free(sampleBuffers[padIndex]);
    sampleBuffers[padIndex] = nullptr;
    sampleLengths[padIndex] = 0;
    memset(sampleNames[padIndex], 0, 32);
  }
}

bool SampleManager::unloadSample(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return false;
  
  freeSampleBuffer(padIndex);
  spiMaster.setSampleBuffer(padIndex, nullptr, 0);
  
  return true;
}

bool SampleManager::trimSample(int padIndex, float startNorm, float endNorm) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES || !sampleBuffers[padIndex]) return false;
  if (startNorm < 0.0f) startNorm = 0.0f;
  if (endNorm > 1.0f) endNorm = 1.0f;
  if (startNorm >= endNorm) return false;
  
  uint32_t origLen = sampleLengths[padIndex];
  uint32_t newStart = (uint32_t)(startNorm * origLen);
  uint32_t newEnd = (uint32_t)(endNorm * origLen);
  uint32_t newLen = newEnd - newStart;
  if (newLen < 64) return false;  // Minimum sample length
  
  // Allocate new buffer
  int16_t* newBuf = (int16_t*)ps_malloc(newLen * sizeof(int16_t));
  if (!newBuf) {
    return false;
  }
  
  // Copy trimmed region
  memcpy(newBuf, sampleBuffers[padIndex] + newStart, newLen * sizeof(int16_t));
  
  // Free old buffer and replace
  free(sampleBuffers[padIndex]);
  sampleBuffers[padIndex] = newBuf;
  sampleLengths[padIndex] = newLen;
  
  // Update SPI Master → STM32
  spiMaster.setSampleBuffer(padIndex, newBuf, newLen);
  
  return true;
}

bool SampleManager::applyFade(int padIndex, float fadeInSec, float fadeOutSec) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES || !sampleBuffers[padIndex]) return false;
  if (fadeInSec < 0.0f) fadeInSec = 0.0f;
  if (fadeOutSec < 0.0f) fadeOutSec = 0.0f;
  
  uint32_t len = sampleLengths[padIndex];
  if (len < 4) return false;
  
  // Fade in: ramp up gain from 0 to 1 over the first fadeInSamples
  if (fadeInSec > 0.001f) {
    uint32_t fadeInSamples = (uint32_t)(fadeInSec * SAMPLE_RATE);
    if (fadeInSamples > len / 2) fadeInSamples = len / 2;  // Max half the sample
    for (uint32_t i = 0; i < fadeInSamples; i++) {
      float t = (float)i / (float)fadeInSamples;  // 0.0 to 1.0
      sampleBuffers[padIndex][i] = (int16_t)((float)sampleBuffers[padIndex][i] * t);
    }
  }
  
  // Fade out: ramp down gain from 1 to 0 over the last fadeOutSamples
  if (fadeOutSec > 0.001f) {
    uint32_t fadeOutSamples = (uint32_t)(fadeOutSec * SAMPLE_RATE);
    if (fadeOutSamples > len / 2) fadeOutSamples = len / 2;  // Max half the sample
    uint32_t fadeOutStart = len - fadeOutSamples;
    for (uint32_t i = 0; i < fadeOutSamples; i++) {
      float t = 1.0f - ((float)i / (float)fadeOutSamples);  // 1.0 to 0.0
      sampleBuffers[padIndex][fadeOutStart + i] = (int16_t)((float)sampleBuffers[padIndex][fadeOutStart + i] * t);
    }
  }
  
  return true;
}

// ─── loadSampleFromBuffer ────────────────────────────────────────────────────
// Carga un WAV desde un buffer en PSRAM (sin LittleFS) y lo transfiere a Daisy
bool SampleManager::loadSampleFromBuffer(const uint8_t* data, size_t size, int padIndex) {
  if (!data || size < 12 || padIndex < 0 || padIndex >= MAX_SAMPLES) return false;

  if (sampleBuffers[padIndex] != nullptr) {
    unloadSample(padIndex);
  }

  String parseErr;
  bool success = parseWavFromBuffer(data, size, padIndex, parseErr);
  if (!success) {
    strncpy(lastParseError, parseErr.c_str(), sizeof(lastParseError) - 1);
    lastParseError[sizeof(lastParseError) - 1] = '\0';
    return false;
  }
  lastParseError[0] = '\0';

  snprintf(sampleNames[padIndex], 32, "pad%d", padIndex);
  spiMaster.setSampleBuffer(padIndex, sampleBuffers[padIndex], sampleLengths[padIndex]);
  return true;
}

// Decode a WAV from a PSRAM buffer into sampleBuffers[padIndex] WITHOUT sending
// it to the Daisy. The caller streams it non-blocking (a few chunks per tick),
// reading from getSampleBuffer()/getSampleLength(), so a large sample never
// blocks the task or resets the S3.
bool SampleManager::decodeSampleFromBuffer(const uint8_t* data, size_t size, int padIndex) {
  if (!data || size < 12 || padIndex < 0 || padIndex >= MAX_SAMPLES) return false;
  if (sampleBuffers[padIndex] != nullptr) unloadSample(padIndex);
  String parseErr;
  if (!parseWavFromBuffer(data, size, padIndex, parseErr)) {
    strncpy(lastParseError, parseErr.c_str(), sizeof(lastParseError) - 1);
    lastParseError[sizeof(lastParseError) - 1] = '\0';
    return false;
  }
  lastParseError[0] = '\0';
  snprintf(sampleNames[padIndex], 32, "pad%d", padIndex);
  return true;
}

// Free the S3-side decoded buffer after it has been streamed to the Daisy. The
// Daisy keeps its own copy (64MB SDRAM), so holding it on the S3 only wastes the
// 8MB PSRAM and made later uploads fail with "No PSRAM for sample". Does NOT tell
// the Daisy to unload (that is unloadSample()).
void SampleManager::releaseHostSample(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return;
  freeSampleBuffer(padIndex);
}

// ─── parseWavFromBuffer ───────────────────────────────────────────────────────
// Igual que parseWavFile pero opera sobre un bloque de memoria en PSRAM
bool SampleManager::parseWavFromBuffer(const uint8_t* buf, size_t size, int padIndex, String& errOut) {
  if (size < 12) { errOut = "File too small"; return false; }
  if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
    errOut = "Not a WAV file"; return false;
  }

  uint32_t pos = 12;
  bool fmtFound = false, dataFound = false;
  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
  uint32_t dataPos = 0, dataSize = 0;

  while (pos + 8 <= (uint32_t)size) {
    uint32_t chunkSize = (uint32_t)buf[pos+4]
                       | ((uint32_t)buf[pos+5] << 8)
                       | ((uint32_t)buf[pos+6] << 16)
                       | ((uint32_t)buf[pos+7] << 24);

    if (memcmp(buf + pos, "fmt ", 4) == 0) {
      if (chunkSize < 16 || pos + 8 + 16 > (uint32_t)size) { errOut = "fmt chunk too small"; return false; }
      const uint8_t* fmt = buf + pos + 8;
      audioFormat   = (uint16_t)fmt[0]  | ((uint16_t)fmt[1]  << 8);
      numChannels   = (uint16_t)fmt[2]  | ((uint16_t)fmt[3]  << 8);
      bitsPerSample = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
      fmtFound = true;
    } else if (memcmp(buf + pos, "data", 4) == 0) {
      dataPos  = pos + 8;
      dataSize = chunkSize;
      dataFound = true;
      break;
    }
    // Avance con proteccion de overflow (chunkSize controlado por el archivo).
    uint32_t advance = chunkSize + (chunkSize & 1);
    if (chunkSize > (uint32_t)size || advance > (uint32_t)size - pos - 8) break;
    pos += 8 + advance;
  }

  if (!fmtFound)  { errOut = "No fmt chunk found";  return false; }
  if (!dataFound) { errOut = "No data chunk found"; return false; }
  if (audioFormat != 1 && audioFormat != 0xFFFE) {
    errOut = "Unsupported format " + String(audioFormat); return false;
  }
  if (bitsPerSample != 16 && bitsPerSample != 24) {
    errOut = "Need 16/24-bit PCM, got " + String(bitsPerSample) + "-bit"; return false;
  }
  if (numChannels < 1 || numChannels > 2) {
    errOut = "Bad channel count " + String(numChannels); return false;
  }
  if (dataPos > (uint32_t)size) { errOut = "Bad data chunk offset"; return false; }
  if (dataSize > (uint32_t)size - dataPos) {
    dataSize = (uint32_t)size - dataPos;  // truncar si el tamaño del chunk excede el buffer (sin overflow)
  }

  uint32_t bytesPerSample = bitsPerSample / 8;
  uint32_t numSamples = dataSize / (bytesPerSample * numChannels);

  if (!allocateSampleBuffer(padIndex, numSamples)) {
    errOut = "No PSRAM for sample (free " + String((uint32_t)(ESP.getFreePsram() / 1024)) +
             "KB, need " + String((uint32_t)(numSamples * 2 / 1024)) + "KB)"; return false;
  }

  const uint8_t* src = buf + dataPos;

  if (bitsPerSample == 16) {
    if (numChannels == 1) {
      memcpy(sampleBuffers[padIndex], src, numSamples * 2);
    } else {
      // Stereo 16-bit → mixdown mono
      const int16_t* s = (const int16_t*)src;
      for (uint32_t i = 0; i < numSamples; i++) {
        sampleBuffers[padIndex][i] = (int16_t)(((int32_t)s[i*2] + (int32_t)s[i*2+1]) / 2);
        if ((i & 0x0FFF) == 0) yield();
      }
    }
  } else {
    // 24-bit → 16-bit
    auto s24_to_s16 = [](const uint8_t* b) -> int16_t {
      int32_t v = (int32_t)b[0] | ((int32_t)b[1] << 8) | ((int32_t)b[2] << 16);
      if (v & 0x00800000) v |= ~0x00FFFFFF;
      return (int16_t)(v >> 8);
    };
    uint32_t stride = (uint32_t)numChannels * 3;
    for (uint32_t i = 0; i < numSamples; i++) {
      if (numChannels == 1) {
        sampleBuffers[padIndex][i] = s24_to_s16(src + i * 3);
      } else {
        int16_t l = s24_to_s16(src + i * stride);
        int16_t r = s24_to_s16(src + i * stride + 3);
        sampleBuffers[padIndex][i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
      }
      if ((i & 0x0FFF) == 0) yield();
    }
  }

  sampleLengths[padIndex] = numSamples;
  return true;
}

void SampleManager::unloadAll() {
  for (int i = 0; i < MAX_SAMPLES; i++) {
    if (sampleBuffers[i] != nullptr) {
      unloadSample(i);
    }
  }
}

bool SampleManager::isSampleLoaded(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return false;
  return sampleBuffers[padIndex] != nullptr;
}

uint32_t SampleManager::getSampleLength(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return 0;
  return sampleLengths[padIndex];
}

const char* SampleManager::getSampleName(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return "";
  return sampleNames[padIndex];
}

size_t SampleManager::getTotalPSRAMUsed() {
  size_t total = 0;
  for (int i = 0; i < MAX_SAMPLES; i++) {
    if (sampleBuffers[i] != nullptr) {
      total += sampleLengths[i] * sizeof(int16_t);
    }
  }
  return total;
}

size_t SampleManager::getFreePSRAM() {
  return ESP.getFreePsram();
}

int SampleManager::getLoadedSamplesCount() {
  int count = 0;
  for (int i = 0; i < MAX_SAMPLES; i++) {
    if (sampleBuffers[i] != nullptr) {
      count++;
    }
  }
  return count;
}

size_t SampleManager::getTotalMemoryUsed() {
  return getTotalPSRAMUsed();
}

int16_t* SampleManager::getSampleBuffer(int padIndex) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES) return nullptr;
  return sampleBuffers[padIndex];
}

int SampleManager::getWaveformPeaks(int padIndex, int8_t* outPeaks, int maxPoints) {
  if (padIndex < 0 || padIndex >= MAX_SAMPLES || !sampleBuffers[padIndex] || maxPoints <= 0) return 0;
  
  uint32_t len = sampleLengths[padIndex];
  if (len == 0) return 0;
  
  int points = (maxPoints > 200) ? 200 : maxPoints;
  uint32_t samplesPerPoint = len / points;
  if (samplesPerPoint == 0) samplesPerPoint = 1;
  
  for (int i = 0; i < points; i++) {
    uint32_t start = i * samplesPerPoint;
    uint32_t end = start + samplesPerPoint;
    if (end > len) end = len;
    
    int16_t maxVal = 0;
    int16_t minVal = 0;
    for (uint32_t j = start; j < end; j++) {
      int16_t s = sampleBuffers[padIndex][j];
      if (s > maxVal) maxVal = s;
      if (s < minVal) minVal = s;
    }
    // Scale 16-bit to 8-bit signed (-128..127)
    // Use the peak (max of abs(min), abs(max)) for upper, negative for lower
    outPeaks[i * 2]     = (int8_t)(maxVal >> 8);  // positive peak
    outPeaks[i * 2 + 1] = (int8_t)(minVal >> 8);  // negative peak
  }
  return points;
}
