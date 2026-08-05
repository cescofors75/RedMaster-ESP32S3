#pragma once

#include <Arduino.h>
#include "usb_transport_protocol.h"

#ifndef RED808_P4_USB_ENABLED
#define RED808_P4_USB_ENABLED 0
#endif

class P4UsbLink {
public:
  using JsonHandler = void (*)(const char* json, size_t len);
  using FileBeginHandler = uint8_t (*)(uint8_t pad, const char* filename,
                                      uint32_t totalSize);
  using FileChunkHandler = uint8_t (*)(const uint8_t* data, size_t len);
  using FileEndHandler = uint8_t (*)();
  using FileAbortHandler = void (*)();

  bool begin();
  void update();

  void setHandlers(JsonHandler jsonHandler,
                   FileBeginHandler beginHandler,
                   FileChunkHandler chunkHandler,
                   FileEndHandler endHandler,
                   FileAbortHandler abortHandler);

  bool connected() const;
  bool physicalConnected() const;
  bool sendJson(const char* json, size_t len = 0);
  bool sendJson(const String& json) { return sendJson(json.c_str(), json.length()); }

  uint32_t rxFrames() const { return _rxFrames; }
  uint32_t rxErrors() const { return _rxErrors; }
  uint32_t txDrops() const { return _txDrops; }

private:
  struct TxItem {
    uint8_t* bytes;
    uint16_t length;
  };

  static void txTaskEntry(void* arg);
  void txTask();
  bool queueFrame(uint8_t type, uint16_t sequence,
                  const uint8_t* payload, uint16_t length);
  bool queueFrame(uint8_t type, const uint8_t* payload, uint16_t length);
  void sendAck(uint16_t sequence, uint8_t status);
  void consumeByte(uint8_t value);
  void resetParser();
  void dispatchFrame();

  JsonHandler _jsonHandler = nullptr;
  FileBeginHandler _fileBeginHandler = nullptr;
  FileChunkHandler _fileChunkHandler = nullptr;
  FileEndHandler _fileEndHandler = nullptr;
  FileAbortHandler _fileAbortHandler = nullptr;

  QueueHandle_t _txQueue = nullptr;
  TaskHandle_t _txTaskHandle = nullptr;
  SemaphoreHandle_t _txBuildMutex = nullptr;
  uint16_t _nextTxSequence = 1;

  Red808Usb::FrameHeader _rxHeader = {};
  uint8_t _rxPayload[Red808Usb::MAX_FILE_CHUNK + 128] = {};
  uint8_t _rxHeaderPos = 0;
  uint16_t _rxPayloadPos = 0;
  uint8_t _rxCrcBytes[2] = {};
  uint8_t _rxCrcPos = 0;
  enum ParseState : uint8_t { SEEK_MAGIC_0, SEEK_MAGIC_1, READ_HEADER,
                              READ_PAYLOAD, READ_CRC };
  ParseState _parseState = SEEK_MAGIC_0;

  bool _protocolConnected = false;
  uint32_t _lastValidRxMs = 0;
  uint32_t _rxFrames = 0;
  uint32_t _rxErrors = 0;
  uint32_t _txDrops = 0;
};

extern P4UsbLink p4UsbLink;
