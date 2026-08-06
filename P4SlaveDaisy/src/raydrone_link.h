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
    bool                 has_status;
};

class RayDroneLink
{
  public:
    void Init();
    void Process();
    const RayDroneModel& Model() const { return model_; }

  private:
    void Feed(uint8_t byte);
    void ConsumePacket(size_t packet_size);
    void Resync();
    void AcceptStatus(const uint8_t* packet, size_t packet_size);

    RayDroneModel model_ = {};
    uint8_t packet_[raydrone_usb::kMaxPacketSize] = {};
    size_t  packet_size_ = 0;
    uint32_t last_status_ms_ = 0;
};

