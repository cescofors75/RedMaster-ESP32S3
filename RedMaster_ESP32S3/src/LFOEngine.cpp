/*
 * LFOEngine.cpp
 * Per-pad LFO system — "Organic Drum Machine"
 *
 * 24 independent LFOs, BPM-synced or free-running.
 * Runs on ESP32-S3 Core 1, sends modulated values via SPI.
 */

#include "LFOEngine.h"
#include "SPIMaster.h"
#include <math.h>

// PRNG local (xorshift32) para S&H: el random() de Arduino comparte estado
// global con codigo de Core0 (no thread-safe entre nucleos). Update() corre
// en Core1.
static inline float lfoRandUnit() {
    static uint32_t s = 0;
    if (s == 0) s = ((uint32_t)micros() ^ 0xBADC0FFE) | 1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    // Map a [-1.0, +1.0]
    return ((float)(s % 2001u) - 1000.0f) / 1000.0f;
}

// ══════════════════════════════════════════════════════
// Constructor
// ══════════════════════════════════════════════════════
LFOEngine::LFOEngine()
    : lastUpdateUs(0)
    , lastSpiSendMs(0)
{
    resetAll();
}

void LFOEngine::resetAll() {
    for (int i = 0; i < LFO_MAX_PADS; i++) {
        lfos[i].active    = false;
        lfos[i].waveform  = LFO_WAVE_SINE;
        lfos[i].division  = LFO_RATE_1_8;
        lfos[i].target    = LFO_TGT_PITCH;
        lfos[i].depth     = 50;
        lfos[i].freeHz    = 2.0f;
        lfos[i].phase     = 0.0f;
        lfos[i].value     = 0.0f;
        lfos[i].shValue   = 0.0f;
        lfos[i].startPhase = 0;
        lfos[i].retrigger = true;
    }
}

// ══════════════════════════════════════════════════════
// Configuration setters/getters
// ══════════════════════════════════════════════════════
void LFOEngine::setActive(uint8_t pad, bool on) {
    if (pad >= LFO_MAX_PADS) return;
    lfos[pad].active = on;
    if (on && lfos[pad].phase == 0.0f) {
        lfos[pad].phase = lfos[pad].startPhase / 255.0f;
    }
}

bool LFOEngine::isActive(uint8_t pad) const {
    return pad < LFO_MAX_PADS && lfos[pad].active;
}

void LFOEngine::setWaveform(uint8_t pad, LfoWaveform wf) {
    if (pad < LFO_MAX_PADS) lfos[pad].waveform = wf;
}

LfoWaveform LFOEngine::getWaveform(uint8_t pad) const {
    return pad < LFO_MAX_PADS ? lfos[pad].waveform : LFO_WAVE_SINE;
}

void LFOEngine::setDivision(uint8_t pad, LfoDivision div) {
    if (pad < LFO_MAX_PADS) lfos[pad].division = div;
}

LfoDivision LFOEngine::getDivision(uint8_t pad) const {
    return pad < LFO_MAX_PADS ? lfos[pad].division : LFO_RATE_1_8;
}

void LFOEngine::setTarget(uint8_t pad, LfoTarget tgt) {
    if (pad < LFO_MAX_PADS) lfos[pad].target = tgt;
}

LfoTarget LFOEngine::getTarget(uint8_t pad) const {
    return pad < LFO_MAX_PADS ? lfos[pad].target : LFO_TGT_PITCH;
}

void LFOEngine::setDepth(uint8_t pad, uint8_t depth) {
    if (pad < LFO_MAX_PADS) lfos[pad].depth = depth > 100 ? 100 : depth;
}

uint8_t LFOEngine::getDepth(uint8_t pad) const {
    return pad < LFO_MAX_PADS ? lfos[pad].depth : 0;
}

void LFOEngine::setFreeHz(uint8_t pad, float hz) {
    if (pad < LFO_MAX_PADS) {
        if (hz < 0.1f) hz = 0.1f;
        if (hz > 20.0f) hz = 20.0f;
        lfos[pad].freeHz = hz;
    }
}

float LFOEngine::getFreeHz(uint8_t pad) const {
    return pad < LFO_MAX_PADS ? lfos[pad].freeHz : 1.0f;
}

void LFOEngine::setPhaseOffset(uint8_t pad, uint8_t phase255) {
    if (pad < LFO_MAX_PADS) lfos[pad].startPhase = phase255;
}

void LFOEngine::setRetrigger(uint8_t pad, bool on) {
    if (pad < LFO_MAX_PADS) lfos[pad].retrigger = on;
}

const PadLFO& LFOEngine::getPadLFO(uint8_t pad) const {
    static PadLFO dummy = {};
    return pad < LFO_MAX_PADS ? lfos[pad] : dummy;
}

// ══════════════════════════════════════════════════════
// Waveform generators — input: phase 0..1, output: -1..+1
// ══════════════════════════════════════════════════════
float LFOEngine::waveSine(float phase) {
    return sinf(phase * 2.0f * M_PI);
}

float LFOEngine::waveTriangle(float phase) {
    // 0→0.25: 0→+1, 0.25→0.75: +1→-1, 0.75→1.0: -1→0
    if (phase < 0.25f)      return phase * 4.0f;
    else if (phase < 0.75f) return 1.0f - (phase - 0.25f) * 4.0f;
    else                    return -1.0f + (phase - 0.75f) * 4.0f;
}

float LFOEngine::waveSquare(float phase) {
    return phase < 0.5f ? 1.0f : -1.0f;
}

float LFOEngine::waveSaw(float phase) {
    return 2.0f * phase - 1.0f;  // 0→-1, 0.5→0, 1→+1
}

// ══════════════════════════════════════════════════════
// BPM → Hz conversion for each division
// ══════════════════════════════════════════════════════
float LFOEngine::divisionToHz(float bpm, LfoDivision div) {
    if (bpm <= 0.0f) bpm = 120.0f;
    float beatsPerSec = bpm / 60.0f;
    switch (div) {
        case LFO_RATE_1_4:  return beatsPerSec;           // quarter note
        case LFO_RATE_1_8:  return beatsPerSec * 2.0f;    // eighth note
        case LFO_RATE_1_16: return beatsPerSec * 4.0f;    // sixteenth note
        case LFO_RATE_1_32: return beatsPerSec * 8.0f;    // thirty-second note
        default:            return 1.0f;                    // free — not used here
    }
}

// ══════════════════════════════════════════════════════
// Note trigger → reset phase (retrigger mode)
// ══════════════════════════════════════════════════════
void LFOEngine::onPadTrigger(uint8_t pad) {
    if (pad >= LFO_MAX_PADS) return;
    if (lfos[pad].active && lfos[pad].retrigger) {
        lfos[pad].phase = lfos[pad].startPhase / 255.0f;
    }
}

// ══════════════════════════════════════════════════════
// Main update — call from Core 1 task at ~1kHz
// ══════════════════════════════════════════════════════
void LFOEngine::update(float bpm, SPIMaster& spi) {
    uint32_t nowUs = micros();
    if (lastUpdateUs == 0) {
        lastUpdateUs = nowUs;
        return;
    }

    float dtSec = (nowUs - lastUpdateUs) / 1000000.0f;
    lastUpdateUs = nowUs;

    // Clamp dt to avoid jumps (e.g. after sleep)
    if (dtSec > 0.1f) dtSec = 0.1f;
    if (dtSec <= 0.0f) return;

    bool anyActive = false;

    for (int i = 0; i < LFO_MAX_PADS; i++) {
        PadLFO& lfo = lfos[i];
        if (!lfo.active) continue;
        anyActive = true;

        // Calculate frequency
        float hz;
        if (lfo.division == LFO_RATE_FREE) {
            hz = lfo.freeHz;
        } else {
            hz = divisionToHz(bpm, lfo.division);
        }

        // Advance phase
        lfo.phase += hz * dtSec;
        // Wrap around 0..1
        while (lfo.phase >= 1.0f) lfo.phase -= 1.0f;
        while (lfo.phase < 0.0f) lfo.phase += 1.0f;

        // Generate waveform value (-1..+1)
        float rawValue;
        switch (lfo.waveform) {
            case LFO_WAVE_SINE:     rawValue = waveSine(lfo.phase); break;
            case LFO_WAVE_TRIANGLE: rawValue = waveTriangle(lfo.phase); break;
            case LFO_WAVE_SQUARE:   rawValue = waveSquare(lfo.phase); break;
            case LFO_WAVE_SAW:      rawValue = waveSaw(lfo.phase); break;
            case LFO_WAVE_SH:
                // Sample & Hold: latch new random value on each cycle start
                if (lfo.phase < hz * dtSec) {
                    lfo.shValue = lfoRandUnit();
                }
                rawValue = lfo.shValue;
                break;
            default: rawValue = 0.0f; break;
        }

        // Apply depth: scale -1..+1 by depth percentage
        lfo.value = rawValue * (lfo.depth / 100.0f);
    }

    // Rate-limit SPI sends: every 20ms (50 Hz — smooth enough for modulation)
    uint32_t nowMs = millis();
    if (anyActive && (nowMs - lastSpiSendMs >= 20)) {
        lastSpiSendMs = nowMs;
        for (int i = 0; i < LFO_MAX_PADS; i++) {
            if (lfos[i].active) {
                applyModulation(i, spi);
            }
        }
    }
}

// ══════════════════════════════════════════════════════
// Apply modulation — convert LFO value to SPI command
// ══════════════════════════════════════════════════════
void LFOEngine::applyModulation(uint8_t pad, SPIMaster& spi) {
    PadLFO& lfo = lfos[pad];
    float v = lfo.value;  // -1..+1 scaled by depth

    switch (lfo.target) {
        case LFO_TGT_PITCH: {
            // Modulate pitch: ±1200 cents max (1 octave)
            int16_t cents = (int16_t)(v * 1200.0f);
            spi.setTrackPitch(pad, cents);
            break;
        }
        case LFO_TGT_VOLUME: {
            // Modulate per-track volume: base 80, range ±40
            // v=+1 → 100%, v=-1 → 40%, v=0 → 80% (throb effect)
            uint8_t vol = (uint8_t)constrain((int)(80.0f + v * 40.0f), 0, 100);
            spi.setTrackVolume((int)pad, vol);
            break;
        }
        case LFO_TGT_PAN: {
            // Modulate pan: -100..+100
            int8_t pan = (int8_t)(v * 100.0f);
            spi.setTrackPan(pad, pan);
            break;
        }
        case LFO_TGT_FILTER: {
            // Modulate filter cutoff: 200-12000 Hz (logarithmic)
            float logMin = logf(200.0f);
            float logMax = logf(12000.0f);
            float logCenter = (logMin + logMax) / 2.0f;
            float logRange  = (logMax - logMin) / 2.0f;
            float cutoff = expf(logCenter + v * logRange);
            spi.setFilterCutoff(cutoff);
            break;
        }
        case LFO_TGT_DECAY: {
            // Decay target — applied at trigger time via sequencer
            // No SPI send; the value is read by the sequencer callback
            break;
        }
        case LFO_TGT_ECHO_TIME: {
            // Modulate echo time: base 100ms, ±40%
            float ms = 100.0f * (1.0f + v * 0.4f);
            ms = constrain(ms, 10.0f, 200.0f);
            spi.setTrackEcho(pad, true, ms, 40.0f, 50.0f);
            break;
        }
        case LFO_TGT_DIST_DRIVE: {
            // Modulate distortion drive: base 50%, ±80%
            float amount = 50.0f * (1.0f + v * 0.8f);
            amount = constrain(amount, 0.0f, 100.0f);
            spi.setTrackDistortion(pad, amount);
            break;
        }
        case LFO_TGT_CRUSH: {
            // Modulate bitcrush: base 10 bits, ±6 bits
            uint8_t bits = (uint8_t)constrain((int)(10.0f + v * 6.0f), 1, 16);
            spi.setTrackBitCrush(pad, bits);
            break;
        }
        case LFO_TGT_SEND_REV: {
            // Modulate reverb send: base 50, ±50%
            uint8_t send = (uint8_t)constrain((int)(50.0f + v * 25.0f), 0, 100);
            spi.setTrackReverbSend(pad, send);
            break;
        }
        case LFO_TGT_SEND_DEL: {
            // Modulate delay send: base 50, ±50%
            uint8_t send = (uint8_t)constrain((int)(50.0f + v * 25.0f), 0, 100);
            spi.setTrackDelaySend(pad, send);
            break;
        }
        default:
            break;
    }
}

// ══════════════════════════════════════════════════════
// Scope data snapshot (called from web task, Core 0)
// ══════════════════════════════════════════════════════
void LFOEngine::getScopeData(LfoScopeData& out) const {
    out.activeMask[0] = 0;
    out.activeMask[1] = 0;
    out.activeMask[2] = 0;

    for (int i = 0; i < LFO_MAX_PADS; i++) {
        out.values[i] = lfos[i].active ? lfos[i].value : 0.0f;
        if (lfos[i].active) {
            if (i < 8)       out.activeMask[0] |= (1 << i);
            else if (i < 16) out.activeMask[1] |= (1 << (i - 8));
            else             out.activeMask[2] |= (1 << (i - 16));
        }
    }
}
