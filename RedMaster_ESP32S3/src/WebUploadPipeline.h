#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

static constexpr const char* kDaisyUploadTempPath = "/.pad-upload.tmp";
static constexpr uint32_t kUploadReceiveTimeoutMs = 30000;
static constexpr uint32_t kDaisyUploadNoProgressTimeoutMs = 30000;

struct CleanTrackUploadState {
  bool active;
  bool error;
  AsyncWebServerRequest* owner;
  uint32_t lastActivityMs;
  int slot;
  size_t bytesWritten;
  char filename[64];
  char filePath[96];
  char errorMsg[96];
  File file;
};

struct DaisyUploadStreamState {
  int pad = -1;
  bool receiving = false;
  bool active = false;
  bool begun = false;
  bool error = false;
  char errorMsg[96] = "";
  AsyncWebServerRequest* owner = nullptr;
  size_t totalSize = 0;
  size_t bytesWritten = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  uint32_t bytesRemaining = 0;
  uint32_t totalSamples = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t samplesSent = 0;
  uint32_t startedMs = 0;
  uint32_t lastProgressMs = 0;
  char filename[64] = "";
};
