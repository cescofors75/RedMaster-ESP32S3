#pragma once

// RayDrone <-> P4SlaveDaisy wire protocol.
//
// USB CDC is a byte stream, so packets are explicitly framed and protected
// with CRC-16/CCITT-FALSE. All multi-byte values are little endian. The
// packet stays below one full-speed USB max packet (64 bytes), which keeps
// the 20 Hz telemetry path cheap on both processors.

#include <cstddef>
#include <cstdint>

namespace raydrone_usb
{
constexpr uint8_t  kMagic0          = 'R';
constexpr uint8_t  kMagic1          = 'D';
constexpr uint8_t  kProtocolVersion = 1;
constexpr size_t   kHeaderSize      = 10;
constexpr size_t   kCrcSize         = 2;
constexpr size_t   kStatusPayloadSize = 32;
constexpr size_t   kStatusPacketSize  = kHeaderSize + kStatusPayloadSize + kCrcSize;
constexpr size_t   kMaxPayloadSize    = 64;
constexpr size_t   kMaxPacketSize     = kHeaderSize + kMaxPayloadSize + kCrcSize;

enum class PacketType : uint8_t
{
    Status  = 0x02,
    Command = 0x10,
    Ack     = 0x11,
};

enum StatusFlag : uint16_t
{
    Captured = 1u << 0,
    Recording = 1u << 1,
    Committing = 1u << 2,
    Bypassed = 1u << 3,
    Limiting = 1u << 4,
};

struct Status
{
    uint32_t uptime_ms;
    uint32_t recorded_samples;
    uint32_t capacity_samples;
    uint16_t flags;
    uint16_t character_milli;
    uint16_t intensity_milli;
    uint16_t focus_milli;
    uint16_t limiter_gain_milli;
    uint16_t input_level_milli;
    uint16_t output_level_milli;
    uint16_t active_voices;
    uint8_t  chord_index;
    uint8_t  reserved;
    uint16_t sample_rate_hz;
};

inline void PutU16(uint8_t* dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
}

inline void PutU32(uint8_t* dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t GetU16(const uint8_t* src)
{
    return static_cast<uint16_t>(src[0])
           | static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t GetU32(const uint8_t* src)
{
    return static_cast<uint32_t>(src[0])
           | (static_cast<uint32_t>(src[1]) << 8)
           | (static_cast<uint32_t>(src[2]) << 16)
           | (static_cast<uint32_t>(src[3]) << 24);
}

inline uint16_t Crc16(const uint8_t* data, size_t length)
{
    uint16_t crc = 0xffffu;
    for(size_t i = 0; i < length; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for(uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000u) != 0u
                      ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                      : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

inline size_t EncodeStatus(const Status& status,
                           uint32_t      sequence,
                           uint8_t*      output,
                           size_t        output_capacity)
{
    if(output == nullptr || output_capacity < kStatusPacketSize)
        return 0;

    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kProtocolVersion;
    output[3] = static_cast<uint8_t>(PacketType::Status);
    PutU16(output + 4, static_cast<uint16_t>(kStatusPayloadSize));
    PutU32(output + 6, sequence);

    uint8_t* payload = output + kHeaderSize;
    PutU32(payload + 0, status.uptime_ms);
    PutU32(payload + 4, status.recorded_samples);
    PutU32(payload + 8, status.capacity_samples);
    PutU16(payload + 12, status.flags);
    PutU16(payload + 14, status.character_milli);
    PutU16(payload + 16, status.intensity_milli);
    PutU16(payload + 18, status.focus_milli);
    PutU16(payload + 20, status.limiter_gain_milli);
    PutU16(payload + 22, status.input_level_milli);
    PutU16(payload + 24, status.output_level_milli);
    PutU16(payload + 26, status.active_voices);
    payload[28] = status.chord_index;
    payload[29] = status.reserved;
    PutU16(payload + 30, status.sample_rate_hz);

    PutU16(output + kHeaderSize + kStatusPayloadSize,
           Crc16(output, kHeaderSize + kStatusPayloadSize));
    return kStatusPacketSize;
}

inline bool DecodeStatus(const uint8_t* packet, size_t length, Status& status)
{
    if(packet == nullptr || length != kStatusPacketSize
       || packet[0] != kMagic0 || packet[1] != kMagic1
       || packet[2] != kProtocolVersion
       || packet[3] != static_cast<uint8_t>(PacketType::Status)
       || GetU16(packet + 4) != kStatusPayloadSize
       || GetU16(packet + kHeaderSize + kStatusPayloadSize)
              != Crc16(packet, kHeaderSize + kStatusPayloadSize))
        return false;

    const uint8_t* payload = packet + kHeaderSize;
    status.uptime_ms          = GetU32(payload + 0);
    status.recorded_samples   = GetU32(payload + 4);
    status.capacity_samples   = GetU32(payload + 8);
    status.flags              = GetU16(payload + 12);
    status.character_milli    = GetU16(payload + 14);
    status.intensity_milli    = GetU16(payload + 16);
    status.focus_milli        = GetU16(payload + 18);
    status.limiter_gain_milli = GetU16(payload + 20);
    status.input_level_milli  = GetU16(payload + 22);
    status.output_level_milli = GetU16(payload + 24);
    status.active_voices      = GetU16(payload + 26);
    status.chord_index        = payload[28];
    status.reserved           = payload[29];
    status.sample_rate_hz     = GetU16(payload + 30);
    return true;
}

} // namespace raydrone_usb
