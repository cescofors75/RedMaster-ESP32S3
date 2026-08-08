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

void RayDroneLink::SetFocus(uint16_t focus_milli)
{
    pending_focus_milli_ = focus_milli > 1000u ? 1000u : focus_milli;
    focus_pending_ = true;
}

void RayDroneLink::Process()
{
    usb_cdc_process();

    // Bound each loop pass so malformed or noisy input cannot monopolize the
    // Arduino task. Status v3 (58 bytes) and waveform (64 bytes) each fit in
    // one full-speed USB packet.
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
    model_.waveform_age_ms
        = model_.has_waveform ? now - last_waveform_ms_ : 0xffffffffu;

    if(!usb_cdc_connected())
        model_.link_state = model_.has_status ? RayDroneLinkState::Stale
                                              : RayDroneLinkState::Searching;
    else if(!model_.has_status)
        model_.link_state = RayDroneLinkState::WaitingForTelemetry;
    else if(model_.telemetry_age_ms > kTelemetryStaleMs)
        model_.link_state = RayDroneLinkState::Stale;
    else
        model_.link_state = RayDroneLinkState::Live;

    if(usb_cdc_connected() && focus_pending_
       && static_cast<int32_t>(now - next_focus_send_ms_) >= 0)
    {
        if(SendCommand(raydrone_usb::CommandId::SetFocus,
                       pending_focus_milli_))
            focus_pending_ = false;
        next_focus_send_ms_ = now + 30u;
    }

    if(usb_cdc_connected() && command_pending_)
    {
        if(SendCommand(pending_command_id_, pending_command_value_))
            command_pending_ = false;
    }

    if(usb_cdc_connected()
       && static_cast<int32_t>(now - next_heartbeat_ms_) >= 0)
    {
        SendCommand(raydrone_usb::CommandId::Sync, 0);
        // No inundar la cola cuando Daisy esta ocupada: un heartbeat por
        // segundo basta para pedir una respuesta inmediata.
        next_heartbeat_ms_ = now + 1000u;
    }

    // Un pico DSP puede retrasar telemetria sin que el cable se haya perdido.
    // Damos margen antes de cerrar el handle para no convertir una pausa breve
    // en una carrera de close/open con el driver CDC.
    if(usb_cdc_connected() && model_.has_status
       && model_.telemetry_age_ms > 8000u
       && static_cast<int32_t>(now - next_recovery_ms_) >= 0)
    {
        usb_cdc_request_reconnect();
        next_recovery_ms_ = now + 10000u;
        packet_size_ = 0;
    }
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
    else if(packet_[3]
            == static_cast<uint8_t>(raydrone_usb::PacketType::Waveform))
        AcceptWaveform(packet_, packet_size);
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

void RayDroneLink::AcceptWaveform(const uint8_t* packet, size_t packet_size)
{
    raydrone_usb::WaveformChunk chunk = {};
    if(!raydrone_usb::DecodeWaveformChunk(packet, packet_size, chunk))
    {
        ++model_.invalid_packets;
        return;
    }

    // LIVE cambia mientras llegan los cuatro chunks. Publicamos cada cuarto de
    // onda inmediatamente: el barrido completo tarda 200 ms y una pequeña
    // discontinuidad visual es preferible a esperar una revision estable que
    // nunca existe en un ring continuo.
    const bool live_stream = model_.has_status
        && (model_.status.flags & raydrone_usb::StatusFlag::LiveStream) != 0;
    if(live_stream)
    {
        for(size_t i = 0; i < raydrone_usb::kWaveformBinsPerPacket; ++i)
        {
            const size_t destination = chunk.start_bin + i;
            model_.waveform_min[destination] = chunk.minimum[i];
            model_.waveform_max[destination] = chunk.maximum[i];
        }
        model_.waveform_revision = chunk.revision;
        model_.has_waveform = true;
        last_waveform_ms_ = millis();
        pending_wave_mask_ = 0;
        return;
    }

    if(chunk.revision != pending_wave_revision_)
    {
        pending_wave_revision_ = chunk.revision;
        pending_wave_mask_ = 0;
    }
    if(chunk.start_bin % raydrone_usb::kWaveformBinsPerPacket != 0u)
    {
        ++model_.invalid_packets;
        return;
    }
    const size_t chunk_index
        = chunk.start_bin / raydrone_usb::kWaveformBinsPerPacket;
    constexpr size_t chunk_count = raydrone_usb::kWaveformBinCount
                                   / raydrone_usb::kWaveformBinsPerPacket;
    if(chunk_index >= chunk_count)
    {
        ++model_.invalid_packets;
        return;
    }
    for(size_t i = 0; i < raydrone_usb::kWaveformBinsPerPacket; ++i)
    {
        const size_t destination = chunk.start_bin + i;
        pending_wave_min_[destination] = chunk.minimum[i];
        pending_wave_max_[destination] = chunk.maximum[i];
    }
    pending_wave_mask_ |= static_cast<uint8_t>(1u << chunk_index);

    constexpr uint8_t complete_mask = static_cast<uint8_t>(
        (1u << (raydrone_usb::kWaveformBinCount
                / raydrone_usb::kWaveformBinsPerPacket))
        - 1u);
    if(pending_wave_mask_ == complete_mask)
    {
        memcpy(model_.waveform_min, pending_wave_min_, sizeof(model_.waveform_min));
        memcpy(model_.waveform_max, pending_wave_max_, sizeof(model_.waveform_max));
        model_.waveform_revision = pending_wave_revision_;
        model_.has_waveform = true;
        last_waveform_ms_ = millis();
        pending_wave_mask_ = 0;
    }
}

bool RayDroneLink::SendCommand(raydrone_usb::CommandId id, uint16_t value_milli)
{
    uint8_t packet[raydrone_usb::kCommandPacketSize];
    const raydrone_usb::Command command = {id, value_milli};
    const size_t packet_size = raydrone_usb::EncodeCommand(
        command, command_sequence_ + 1u, packet, sizeof(packet));
    if(packet_size == 0 || usb_cdc_write(packet, packet_size) != packet_size)
        return false;
    ++command_sequence_;
    return true;
}

void RayDroneLink::RequestCommand(raydrone_usb::CommandId id,
                                  uint16_t value_milli)
{
    pending_command_id_ = id;
    pending_command_value_ = value_milli;
    command_pending_ = true;
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
