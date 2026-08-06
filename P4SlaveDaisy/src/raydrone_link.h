#pragma once

#include "raydrone_usb_protocol.h"

#include <cstdint>

enum class RayDroneLinkState : uint8_t
{
    Searching,
    WaitingForTelemetry,
    Live,
    Stale,
};

struct RayDroneModel
{
    raydrone_usb::Status status;
    RayDroneLinkState    link_state;
    uint32_t             telemetry_age_ms;
    uint32_t             received_packets;
    uint32_t             dropped_packets;
    uint32_t             invalid_packets;
    uint32_t             sequence;
    uint32_t             waveform_age_ms;
    uint16_t             waveform_revision;
    int8_t               waveform_min[raydrone_usb::kWaveformBinCount];
    int8_t               waveform_max[raydrone_usb::kWaveformBinCount];
    bool                 has_status;
    bool                 has_waveform;
};

class RayDroneLink
{
  public:
    void Init();
    void Process();
    void SetFocus(uint16_t focus_milli);
    void RequestCommand(raydrone_usb::CommandId id, uint16_t value_milli);
    const RayDroneModel& Model() const { return model_; }

  private:
    void Feed(uint8_t byte);
    void ConsumePacket(size_t packet_size);
    void Resync();
    void AcceptStatus(const uint8_t* packet, size_t packet_size);
    void AcceptWaveform(const uint8_t* packet, size_t packet_size);
    bool SendCommand(raydrone_usb::CommandId id, uint16_t value_milli);

    RayDroneModel model_ = {};
    uint8_t packet_[raydrone_usb::kMaxPacketSize] = {};
    size_t  packet_size_ = 0;
    uint32_t last_status_ms_ = 0;
    uint32_t last_waveform_ms_ = 0;
    uint32_t next_heartbeat_ms_ = 0;
    uint32_t next_focus_send_ms_ = 0;
    uint32_t next_recovery_ms_ = 0;
    uint32_t command_sequence_ = 0;
    uint16_t pending_focus_milli_ = 500;
    bool     focus_pending_ = false;
    raydrone_usb::CommandId pending_command_id_
        = raydrone_usb::CommandId::Sync;
    uint16_t pending_command_value_ = 0;
    bool command_pending_ = false;
    uint16_t pending_wave_revision_ = 0;
    uint8_t  pending_wave_mask_ = 0;
    int8_t   pending_wave_min_[raydrone_usb::kWaveformBinCount] = {};
    int8_t   pending_wave_max_[raydrone_usb::kWaveformBinCount] = {};
};
