#include "P4UsbLink.h"

#if RED808_P4_USB_ENABLED
#include <USB.h>
#include <USBCDC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if ARDUINO_USB_MODE
#error "RED808_P4_USB_ENABLED requires ARDUINO_USB_MODE=0 (TinyUSB OTG device)"
#endif

#if ARDUINO_USB_CDC_ON_BOOT
#error "The P4 link uses a dedicated USBCDC instance; set ARDUINO_USB_CDC_ON_BOOT=0"
#endif

static USBCDC s_p4Cdc;
static constexpr uint32_t LINK_TIMEOUT_MS = 1600;
static constexpr int TX_QUEUE_DEPTH = 24;

P4UsbLink p4UsbLink;

bool P4UsbLink::begin() {
  if (_txQueue) return true;

  _txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(TxItem));
  _txBuildMutex = xSemaphoreCreateMutex();
  if (!_txQueue || !_txBuildMutex) return false;

  s_p4Cdc.setRxBufferSize(8192);
  s_p4Cdc.setTxTimeoutMs(10);
  s_p4Cdc.enableReboot(false);
  USB.VID(0x303A);
  USB.PID(0x1001);
  USB.manufacturerName("RED808");
  USB.productName("RED808 Master P4 Link");
  USB.serialNumber("RED808-MASTER");
  USB.usbPower(100);
  s_p4Cdc.begin();
  if (!USB.begin()) return false;

  BaseType_t ok = xTaskCreatePinnedToCore(
      txTaskEntry, "p4_usb_tx", 4096, this, 4, &_txTaskHandle, 0);
  return ok == pdPASS;
}

void P4UsbLink::setHandlers(JsonHandler jsonHandler,
                            FileBeginHandler beginHandler,
                            FileChunkHandler chunkHandler,
                            FileEndHandler endHandler,
                            FileAbortHandler abortHandler) {
  _jsonHandler = jsonHandler;
  _fileBeginHandler = beginHandler;
  _fileChunkHandler = chunkHandler;
  _fileEndHandler = endHandler;
  _fileAbortHandler = abortHandler;
}

bool P4UsbLink::physicalConnected() const {
  return (bool)s_p4Cdc;
}

bool P4UsbLink::connected() const {
  return _protocolConnected && physicalConnected() &&
         (uint32_t)(millis() - _lastValidRxMs) <= LINK_TIMEOUT_MS;
}

void P4UsbLink::update() {
  if (!physicalConnected()) {
    _protocolConnected = false;
    resetParser();
    return;
  }
  if (_protocolConnected &&
      (uint32_t)(millis() - _lastValidRxMs) > LINK_TIMEOUT_MS) {
    _protocolConnected = false;
  }

  int budget = 12288;
  while (budget-- > 0 && s_p4Cdc.available()) {
    int value = s_p4Cdc.read();
    if (value < 0) break;
    consumeByte((uint8_t)value);
  }
}

bool P4UsbLink::sendJson(const char* json, size_t len) {
  if (!json || !connected()) return false;
  if (len == 0) len = strnlen(json, Red808Usb::MAX_JSON_PAYLOAD);
  if (len == 0 || len > Red808Usb::MAX_JSON_PAYLOAD || len > UINT16_MAX) {
    return false;
  }
  return queueFrame(Red808Usb::FRAME_JSON,
                    reinterpret_cast<const uint8_t*>(json), (uint16_t)len);
}

bool P4UsbLink::queueFrame(uint8_t type, const uint8_t* payload,
                           uint16_t length) {
  if (!_txBuildMutex) return false;
  if (xSemaphoreTake(_txBuildMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
    ++_txDrops;
    return false;
  }
  uint16_t sequence = _nextTxSequence++;
  if (_nextTxSequence == 0) _nextTxSequence = 1;
  bool ok = queueFrame(type, sequence, payload, length);
  xSemaphoreGive(_txBuildMutex);
  return ok;
}

bool P4UsbLink::queueFrame(uint8_t type, uint16_t sequence,
                           const uint8_t* payload, uint16_t length) {
  if (!_txQueue || (!payload && length != 0)) return false;
  const size_t total = sizeof(Red808Usb::FrameHeader) + length + 2;
  if (total > UINT16_MAX) return false;
  uint8_t* bytes = static_cast<uint8_t*>(malloc(total));
  if (!bytes) {
    ++_txDrops;
    return false;
  }

  Red808Usb::FrameHeader header = {
      Red808Usb::MAGIC_0, Red808Usb::MAGIC_1, Red808Usb::VERSION,
      type, sequence, length};
  memcpy(bytes, &header, sizeof(header));
  if (length) memcpy(bytes + sizeof(header), payload, length);
  const uint16_t crc = Red808Usb::frameCrc(header, payload);
  bytes[sizeof(header) + length] = (uint8_t)(crc & 0xFFu);
  bytes[sizeof(header) + length + 1] = (uint8_t)(crc >> 8);

  TxItem item = {bytes, (uint16_t)total};
  if (xQueueSend(_txQueue, &item, 0) != pdTRUE) {
    free(bytes);
    ++_txDrops;
    return false;
  }
  return true;
}

void P4UsbLink::sendAck(uint16_t sequence, uint8_t status) {
  Red808Usb::AckPayload ack = {sequence, status};
  queueFrame(Red808Usb::FRAME_ACK, sequence,
             reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
}

void P4UsbLink::txTaskEntry(void* arg) {
  static_cast<P4UsbLink*>(arg)->txTask();
}

void P4UsbLink::txTask() {
  TxItem item = {};
  for (;;) {
    if (xQueueReceive(_txQueue, &item, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    size_t offset = 0;
    const uint32_t deadline = millis() + 250;
    while (offset < item.length && physicalConnected() &&
           (int32_t)(millis() - deadline) < 0) {
      size_t written = s_p4Cdc.write(item.bytes + offset, item.length - offset);
      if (written == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
      } else {
        offset += written;
      }
    }
    if (offset != item.length) ++_txDrops;
    free(item.bytes);
    item = {};
  }
}

void P4UsbLink::resetParser() {
  _parseState = SEEK_MAGIC_0;
  _rxHeaderPos = 0;
  _rxPayloadPos = 0;
  _rxCrcPos = 0;
  memset(&_rxHeader, 0, sizeof(_rxHeader));
}

void P4UsbLink::consumeByte(uint8_t value) {
  uint8_t* headerBytes = reinterpret_cast<uint8_t*>(&_rxHeader);
  switch (_parseState) {
    case SEEK_MAGIC_0:
      if (value == Red808Usb::MAGIC_0) {
        headerBytes[0] = value;
        _parseState = SEEK_MAGIC_1;
      }
      break;
    case SEEK_MAGIC_1:
      if (value == Red808Usb::MAGIC_1) {
        headerBytes[1] = value;
        _rxHeaderPos = 2;
        _parseState = READ_HEADER;
      } else {
        _parseState = value == Red808Usb::MAGIC_0 ? SEEK_MAGIC_1 : SEEK_MAGIC_0;
      }
      break;
    case READ_HEADER:
      headerBytes[_rxHeaderPos++] = value;
      if (_rxHeaderPos == sizeof(_rxHeader)) {
        const bool typeOk = _rxHeader.type == Red808Usb::FRAME_JSON ||
                            _rxHeader.type == Red808Usb::FRAME_FILE_BEGIN ||
                            _rxHeader.type == Red808Usb::FRAME_FILE_CHUNK ||
                            _rxHeader.type == Red808Usb::FRAME_FILE_END ||
                            _rxHeader.type == Red808Usb::FRAME_FILE_ABORT;
        if (_rxHeader.version != Red808Usb::VERSION || !typeOk ||
            _rxHeader.length > sizeof(_rxPayload)) {
          ++_rxErrors;
          resetParser();
        } else {
          _rxPayloadPos = 0;
          _parseState = _rxHeader.length ? READ_PAYLOAD : READ_CRC;
        }
      }
      break;
    case READ_PAYLOAD:
      _rxPayload[_rxPayloadPos++] = value;
      if (_rxPayloadPos == _rxHeader.length) {
        _rxCrcPos = 0;
        _parseState = READ_CRC;
      }
      break;
    case READ_CRC:
      _rxCrcBytes[_rxCrcPos++] = value;
      if (_rxCrcPos == 2) {
        const uint16_t received = (uint16_t)_rxCrcBytes[0] |
                                  ((uint16_t)_rxCrcBytes[1] << 8);
        const uint16_t expected = Red808Usb::frameCrc(_rxHeader, _rxPayload);
        if (received == expected) {
          ++_rxFrames;
          _lastValidRxMs = millis();
          dispatchFrame();
        } else {
          ++_rxErrors;
          sendAck(_rxHeader.sequence, Red808Usb::ACK_BAD_FRAME);
        }
        resetParser();
      }
      break;
  }
}

void P4UsbLink::dispatchFrame() {
  uint8_t status = Red808Usb::ACK_OK;
  switch (_rxHeader.type) {
    case Red808Usb::FRAME_JSON: {
      if (_rxHeader.length == 0 || _rxHeader.length >= sizeof(_rxPayload)) return;
      _rxPayload[_rxHeader.length] = 0;
      const char* json = reinterpret_cast<const char*>(_rxPayload);
      if (strstr(json, "\"cmd\":\"usb_ping\"") != nullptr) {
        _protocolConnected = true;
        char pong[96];
        unsigned long token = 0;
        const char* seqPos = strstr(json, "\"seq\":");
        if (seqPos) token = strtoul(seqPos + 6, nullptr, 10);
        snprintf(pong, sizeof(pong),
                 "{\"type\":\"usb_pong\",\"protocol\":1,\"seq\":%lu}", token);
        sendJson(pong);
        return;
      }
      const bool hello = strstr(json, "\"cmd\":\"hello\"") != nullptr;
      if (hello) {
        _protocolConnected = true;
        sendJson("{\"type\":\"usb_hello\",\"protocol\":1,\"device\":\"RED808_MASTER\"}");
      }
      if (_jsonHandler) _jsonHandler(json, _rxHeader.length);
      break;
    }
    case Red808Usb::FRAME_FILE_BEGIN: {
      if (_rxHeader.length < sizeof(Red808Usb::FileBeginPayload)) {
        status = Red808Usb::ACK_INVALID;
        break;
      }
      Red808Usb::FileBeginPayload begin = {};
      memcpy(&begin, _rxPayload, sizeof(begin));
      const size_t expected = sizeof(begin) + begin.nameLength;
      if (begin.nameLength == 0 || begin.nameLength >= 64 || expected != _rxHeader.length) {
        status = Red808Usb::ACK_INVALID;
        break;
      }
      char filename[64] = {};
      memcpy(filename, _rxPayload + sizeof(begin), begin.nameLength);
      status = _fileBeginHandler
                   ? _fileBeginHandler(begin.pad, filename, begin.totalSize)
                   : Red808Usb::ACK_INVALID;
      break;
    }
    case Red808Usb::FRAME_FILE_CHUNK:
      status = _fileChunkHandler
                   ? _fileChunkHandler(_rxPayload, _rxHeader.length)
                   : Red808Usb::ACK_INVALID;
      break;
    case Red808Usb::FRAME_FILE_END:
      status = _fileEndHandler ? _fileEndHandler() : Red808Usb::ACK_INVALID;
      break;
    case Red808Usb::FRAME_FILE_ABORT:
      if (_fileAbortHandler) _fileAbortHandler();
      break;
    default:
      status = Red808Usb::ACK_INVALID;
      break;
  }
  sendAck(_rxHeader.sequence, status);
}

#else

P4UsbLink p4UsbLink;
bool P4UsbLink::begin() { return false; }
void P4UsbLink::update() {}
void P4UsbLink::setHandlers(JsonHandler, FileBeginHandler, FileChunkHandler,
                            FileEndHandler, FileAbortHandler) {}
bool P4UsbLink::connected() const { return false; }
bool P4UsbLink::physicalConnected() const { return false; }
bool P4UsbLink::sendJson(const char*, size_t) { return false; }

#endif
