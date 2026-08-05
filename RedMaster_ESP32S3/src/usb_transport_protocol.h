#pragma once

#include <Arduino.h>

// Framing shared by RedMaster ESP32-S3 (USB device) and BlueSlaveP4
// (USB host). USB CDC is a byte stream, so every application message carries
// an explicit length and CRC. Keep this file byte-for-byte in sync with the P4
// copy; scripts/check_usb_transport_sync.ps1 verifies that invariant.
namespace Red808Usb {

static constexpr uint8_t MAGIC_0 = 'R';
static constexpr uint8_t MAGIC_1 = '8';
static constexpr uint8_t VERSION = 1;
static constexpr size_t HEADER_SIZE = 8;
static constexpr size_t CRC_SIZE = 2;
static constexpr size_t MAX_JSON_PAYLOAD = 8192;
static constexpr size_t MAX_FILE_CHUNK = 1024;

enum FrameType : uint8_t {
    FRAME_JSON       = 0x01,
    FRAME_FILE_BEGIN = 0x02,
    FRAME_FILE_CHUNK = 0x03,
    FRAME_FILE_END   = 0x04,
    FRAME_FILE_ABORT = 0x05,
    FRAME_ACK        = 0x80,
};

enum AckStatus : uint8_t {
    ACK_OK          = 0,
    ACK_BAD_FRAME   = 1,
    ACK_BUSY        = 2,
    ACK_INVALID     = 3,
    ACK_WRITE_ERROR = 4,
};

struct __attribute__((packed)) FrameHeader {
    uint8_t magic0;
    uint8_t magic1;
    uint8_t version;
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
};

struct __attribute__((packed)) AckPayload {
    uint16_t sequence;
    uint8_t status;
};

struct __attribute__((packed)) FileBeginPayload {
    uint8_t pad;
    uint8_t nameLength;
    uint32_t totalSize;
    // UTF-8 filename follows immediately.
};

static inline uint16_t crc16Update(uint16_t crc, uint8_t value) {
    crc ^= (uint16_t)value << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                              : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline uint16_t frameCrc(const FrameHeader& header,
                                const uint8_t* payload) {
    uint16_t crc = 0xFFFFu;
    // Magic is excluded so a parser can use it purely for resynchronization.
    const uint8_t* h = reinterpret_cast<const uint8_t*>(&header);
    for (size_t i = 2; i < sizeof(FrameHeader); ++i) crc = crc16Update(crc, h[i]);
    for (uint16_t i = 0; i < header.length; ++i) crc = crc16Update(crc, payload[i]);
    return crc;
}

}  // namespace Red808Usb
