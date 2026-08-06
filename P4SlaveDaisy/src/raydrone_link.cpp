#include "raydrone_link.h"

#include "config.h"
#include "usb_cdc_handler.h"

#include <Arduino.h>
#include <cstring>

void RayDroneLink::Init()
{
    model_.link_state = RayDroneLinkState::Searching;
    usb_cdc_init();
}

void RayDroneLink::Process()
{
    usb_cdc_process();

    // Bound each loop pass so malformed or noisy input cannot monopolize the
    // Arduino task. At 20 Hz a valid packet is only 44 bytes.
    uint16_t budget = 512;
    while(budget-- != 0 && usb_cdc_available() > 0)
    {
        const int value = usb_cdc_read();
        if(value >= 0)
            Feed(static_cast<uint8_t>(value));
    }

    const uint32_t now = millis();
    model_.telemetry_age_ms
        = model_.has_status ? now - last_status_ms_ : 0xffffffffu;

    if(!usb_cdc_connected())
        model_.link_state = model_.has_status ? RayDroneLinkState::Stale
                                              : RayDroneLinkState::Searching;
    else if(!model_.has_status)
        model_.link_state = RayDroneLinkState::WaitingForTelemetry;
    else if(model_.telemetry_age_ms > kTelemetryStaleMs)
        model_.link_state = RayDroneLinkState::Stale;
    else
        model_.link_state = RayDroneLinkState::Live;
}

void RayDroneLink::Feed(uint8_t byte)
{
    if(packet_size_ == 0 && byte != raydrone_usb::kMagic0)
        return;
    if(packet_size_ == 1 && byte != raydrone_usb::kMagic1)
    {
        packet_size_ = byte == raydrone_usb::kMagic0 ? 1 : 0;
        packet_[0] = byte;
        return;
    }

    if(packet_size_ >= sizeof(packet_))
    {
        ++model_.invalid_packets;
        Resync();
    }
    packet_[packet_size_++] = byte;

    if(packet_size_ < 6)
        return;

    const uint16_t payload_size = raydrone_usb::GetU16(packet_ + 4);
    if(packet_[2] != raydrone_usb::kProtocolVersion
       || payload_size > raydrone_usb::kMaxPayloadSize)
    {
        ++model_.invalid_packets;
        Resync();
        return;
    }

    const size_t expected = raydrone_usb::kHeaderSize + payload_size
                            + raydrone_usb::kCrcSize;
    if(packet_size_ == expected)
        ConsumePacket(expected);
}

void RayDroneLink::ConsumePacket(size_t packet_size)
{
    const uint16_t expected_crc
        = raydrone_usb::GetU16(packet_ + packet_size - raydrone_usb::kCrcSize);
    const uint16_t actual_crc
        = raydrone_usb::Crc16(packet_, packet_size - raydrone_usb::kCrcSize);
    if(expected_crc != actual_crc)
    {
        ++model_.invalid_packets;
        Resync();
        return;
    }

    if(packet_[3] == static_cast<uint8_t>(raydrone_usb::PacketType::Status))
        AcceptStatus(packet_, packet_size);
    packet_size_ = 0;
}

void RayDroneLink::AcceptStatus(const uint8_t* packet, size_t packet_size)
{
    raydrone_usb::Status status = {};
    if(!raydrone_usb::DecodeStatus(packet, packet_size, status))
    {
        ++model_.invalid_packets;
        return;
    }

    const uint32_t sequence = raydrone_usb::GetU32(packet + 6);
    if(model_.sequence != 0)
    {
        const uint32_t delta = sequence - model_.sequence;
        if(delta > 1 && delta < 0x80000000u)
            model_.dropped_packets += delta - 1u;
    }

    model_.status   = status;
    model_.sequence = sequence;
    ++model_.received_packets;
    model_.has_status = true;
    last_status_ms_   = millis();
}

void RayDroneLink::Resync()
{
    // Drop the current first byte, then keep the earliest complete magic pair
    // or a trailing 'R'. This recovers from inserted/lost bytes without ever
    // trusting the malformed length field.
    size_t start = packet_size_;
    for(size_t i = 1; i + 1 < packet_size_; ++i)
    {
        if(packet_[i] == raydrone_usb::kMagic0
           && packet_[i + 1] == raydrone_usb::kMagic1)
        {
            start = i;
            break;
        }
    }
    if(start < packet_size_)
    {
        packet_size_ -= start;
        memmove(packet_, packet_ + start, packet_size_);
    }
    else if(packet_size_ != 0
            && packet_[packet_size_ - 1] == raydrone_usb::kMagic0)
    {
        packet_[0] = raydrone_usb::kMagic0;
        packet_size_ = 1;
    }
    else
        packet_size_ = 0;
}
