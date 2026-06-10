/*
 * SampleManager.h
 * Gestió de càrrega de samples des de SPIFFS a PSRAM
 */

#ifndef SAMPLEMANAGER_H
#define SAMPLEMANAGER_H

#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>
#include "SPIMaster.h"

#define MAX_SAMPLES 24  // 16 sequencer + 8 XTRA pads
#define MAX_SAMPLE_SIZE (4 * 1024 * 1024) // 4MB per sample

// WAV file header structure
struct WavHeader {
  char riff[4];           // "RIFF"
  uint32_t fileSize;
  char wave[4];           // "WAVE"
  char fmt[4];            // "fmt "
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4];           // "data"
  uint32_t dataSize;
} __attribute__((packed));

class SampleManager {
public:
  SampleManager();
  ~SampleManager();
  
  // Initialization
  bool begin();
  
  // Sample loading
  bool loadSample(const char* filename, int padIndex);
  bool loadSampleFromBuffer(const uint8_t* data, size_t size, int padIndex);  // Load WAV from PSRAM buffer
  bool decodeSampleFromBuffer(const uint8_t* data, size_t size, int padIndex); // Decode only (no SPI transfer)
  void releaseHostSample(int padIndex);  // Free S3-side decoded buffer (Daisy keeps its copy)
  bool trimSample(int padIndex, float startNorm, float endNorm);
  bool applyFade(int padIndex, float fadeInSec, float fadeOutSec);  // Apply fade in/out to buffer
  bool unloadSample(int padIndex);
  void unloadAll();
  
  // Sample info
  bool isSampleLoaded(int padIndex);
  uint32_t getSampleLength(int padIndex);
  const char* getSampleName(int padIndex);
  int getLoadedSamplesCount();
  const char* getLastParseError() { return lastParseError; }
  
  // Waveform data access (for visualizer)
  int16_t* getSampleBuffer(int padIndex);
  // Escribe 2 valores int8 por punto (pico +/-). outPeaks DEBE tener espacio
  // para maxPoints*2 bytes. Devuelve el numero de puntos (cada uno = 2 bytes).
  int getWaveformPeaks(int padIndex, int8_t* outPeaks, int maxPoints);
  
  // Memory info
  size_t getTotalPSRAMUsed();
  size_t getTotalMemoryUsed(); // Alias para compatibilidad
  size_t getFreePSRAM();
  
private:
  int16_t* sampleBuffers[MAX_SAMPLES];
  uint32_t sampleLengths[MAX_SAMPLES];
  char sampleNames[MAX_SAMPLES][32];
  char lastParseError[64] = {};

  bool parseWavFile(fs::File& file, int padIndex, String& errOut);
  bool parseWavFromBuffer(const uint8_t* data, size_t size, int padIndex, String& errOut);
  bool allocateSampleBuffer(int padIndex, uint32_t size);
  void freeSampleBuffer(int padIndex);
};

#endif // SAMPLEMANAGER_H
