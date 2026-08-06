#pragma once

// RayDrone <-> P4SlaveDaisy wire protocol.
//
// USB CDC is a byte stream, so packets are explicitly framed and protected
// with CRC-16/CCITT-FALSE. All multi-byte values are little endian. The
// status/command packets stay below one full-speed USB max packet. A waveform
// chunk occupies exactly one 64-byte packet, avoiding transport fragmentation
// while keeping the audio callback completely outside the USB path.

#include <cstddef>
#include <cstdint>

namespace raydrone_usb
{
constexpr uint8_t  kMagic0          = 'R';
constexpr uint8_t  kMagic1          = 'D';
constexpr uint8_t  kProtocolVersion = 2;
constexpr size_t   kHeaderSize      = 10;
constexpr size_t   kCrcSize         = 2;
constexpr size_t   kStatusPayloadSize = 40;
constexpr size_t   kStatusPacketSize  = kHeaderSize + kStatusPayloadSize + kCrcSize;
constexpr size_t   kCommandPayloadSize = 4;
constexpr size_t   kCommandPacketSize  = kHeaderSize + kCommandPayloadSize + kCrcSize;
constexpr size_t   kWaveformBinCount = 96;
constexpr size_t   kWaveformBinsPerPacket = 24;
constexpr size_t   kWaveformPayloadSize = 4 + kWaveformBinsPerPacket * 2;
constexpr size_t   kWaveformPacketSize = kHeaderSize + kWaveformPayloadSize + kCrcSize;
constexpr size_t   kMaxPayloadSize    = 64;
constexpr size_t   kMaxPacketSize     = kHeaderSize + kMaxPayloadSize + kCrcSize;

static_assert(kWaveformPacketSize == 64,
              "Waveform chunks must fit one full-speed USB packet");

enum class PacketType : uint8_t
{
    Status  = 0x02,
    Waveform = 0x03,
    Command = 0x10,
    Ack     = 0x11,
};

enum class CommandId : uint8_t
{
    SetFocus = 0x01,
    Sync     = 0x02,
    SetSource = 0x03,
    SaveSampler = 0x04,
    SetSampleRate = 0x05,
};

enum class SourceCommand : uint16_t
{
    Default = 0,
    Live = 1,
    Freeze = 2,
    Memory = 3,
    SdNext = 4,
};

enum StatusFlag : uint16_t
{
    Captured = 1u << 0,
    Recording = 1u << 1,
    Committing = 1u << 2,
    Bypassed = 1u << 3,
    Limiting = 1u << 4,
    DefaultSample = 1u << 5,
    ShimmerScene = 1u << 6,
    LiveStream = 1u << 7,
    MemorySample = 1u << 8,
    SdSample = 1u << 9,
    StorageBusy = 1u << 10,
    SdMounted = 1u << 11,
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
    uint32_t source_sample_rate_hz;
    uint32_t audio_sample_rate_hz;
    uint16_t cpu_load_milli;
};

struct Command
{
    CommandId id;
    uint16_t  value_milli;
};

struct WaveformChunk
{
    uint16_t revision;
    uint8_t  start_bin;
    int8_t   minimum[kWaveformBinsPerPacket];
    int8_t   maximum[kWaveformBinsPerPacket];
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
    PutU32(payload + 30, status.source_sample_rate_hz);
    PutU32(payload + 34, status.audio_sample_rate_hz);
    PutU16(payload + 38, status.cpu_load_milli);

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
    status.source_sample_rate_hz = GetU32(payload + 30);
    status.audio_sample_rate_hz = GetU32(payload + 34);
    status.cpu_load_milli     = GetU16(payload + 38);
    return true;
}

inline size_t EncodeCommand(const Command& command,
                            uint32_t       sequence,
                            uint8_t*       output,
                            size_t         output_capacity)
{
    if(output == nullptr || output_capacity < kCommandPacketSize)
        return 0;

    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kProtocolVersion;
    output[3] = static_cast<uint8_t>(PacketType::Command);
    PutU16(output + 4, static_cast<uint16_t>(kCommandPayloadSize));
    PutU32(output + 6, sequence);
    output[kHeaderSize + 0] = static_cast<uint8_t>(command.id);
    output[kHeaderSize + 1] = 0;
    PutU16(output + kHeaderSize + 2, command.value_milli);
    PutU16(output + kHeaderSize + kCommandPayloadSize,
           Crc16(output, kHeaderSize + kCommandPayloadSize));
    return kCommandPacketSize;
}

inline bool DecodeCommand(const uint8_t* packet, size_t length, Command& command)
{
    if(packet == nullptr || length != kCommandPacketSize
       || packet[0] != kMagic0 || packet[1] != kMagic1
       || packet[2] != kProtocolVersion
       || packet[3] != static_cast<uint8_t>(PacketType::Command)
       || GetU16(packet + 4) != kCommandPayloadSize
       || GetU16(packet + kHeaderSize + kCommandPayloadSize)
              != Crc16(packet, kHeaderSize + kCommandPayloadSize))
        return false;

    const uint8_t id = packet[kHeaderSize];
    if(id != static_cast<uint8_t>(CommandId::SetFocus)
       && id != static_cast<uint8_t>(CommandId::Sync)
       && id != static_cast<uint8_t>(CommandId::SetSource)
       && id != static_cast<uint8_t>(CommandId::SaveSampler)
       && id != static_cast<uint8_t>(CommandId::SetSampleRate))
        return false;
    command.id = static_cast<CommandId>(id);
    command.value_milli = GetU16(packet + kHeaderSize + 2);
    return true;
}

inline size_t EncodeWaveformChunk(uint16_t      revision,
                                  uint8_t       start_bin,
                                  const int8_t* minimum,
                                  const int8_t* maximum,
                                  uint32_t      sequence,
                                  uint8_t*      output,
                                  size_t        output_capacity)
{
    if(output == nullptr || minimum == nullptr || maximum == nullptr
       || output_capacity < kWaveformPacketSize
       || static_cast<size_t>(start_bin) + kWaveformBinsPerPacket
              > kWaveformBinCount)
        return 0;

    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kProtocolVersion;
    output[3] = static_cast<uint8_t>(PacketType::Waveform);
    PutU16(output + 4, static_cast<uint16_t>(kWaveformPayloadSize));
    PutU32(output + 6, sequence);

    uint8_t* payload = output + kHeaderSize;
    PutU16(payload + 0, revision);
    payload[2] = start_bin;
    payload[3] = static_cast<uint8_t>(kWaveformBinsPerPacket);
    for(size_t i = 0; i < kWaveformBinsPerPacket; ++i)
    {
        payload[4 + i * 2] = static_cast<uint8_t>(minimum[i]);
        payload[5 + i * 2] = static_cast<uint8_t>(maximum[i]);
    }

    PutU16(output + kHeaderSize + kWaveformPayloadSize,
           Crc16(output, kHeaderSize + kWaveformPayloadSize));
    return kWaveformPacketSize;
}

inline bool DecodeWaveformChunk(const uint8_t* packet,
                                size_t         length,
                                WaveformChunk& chunk)
{
    if(packet == nullptr || length != kWaveformPacketSize
       || packet[0] != kMagic0 || packet[1] != kMagic1
       || packet[2] != kProtocolVersion
       || packet[3] != static_cast<uint8_t>(PacketType::Waveform)
       || GetU16(packet + 4) != kWaveformPayloadSize
       || GetU16(packet + kHeaderSize + kWaveformPayloadSize)
              != Crc16(packet, kHeaderSize + kWaveformPayloadSize))
        return false;

    const uint8_t* payload = packet + kHeaderSize;
    chunk.revision  = GetU16(payload + 0);
    chunk.start_bin = payload[2];
    if(payload[3] != kWaveformBinsPerPacket
       || static_cast<size_t>(chunk.start_bin) + kWaveformBinsPerPacket
              > kWaveformBinCount)
        return false;
    for(size_t i = 0; i < kWaveformBinsPerPacket; ++i)
    {
        chunk.minimum[i] = static_cast<int8_t>(payload[4 + i * 2]);
        chunk.maximum[i] = static_cast<int8_t>(payload[5 + i * 2]);
    }
    return true;
}

} // namespace raydrone_usb
