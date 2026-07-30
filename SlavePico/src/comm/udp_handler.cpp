#include "comm/udp_handler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#include "config.h"
#include "device_map.h"
#include "drivers/input_manager.h"  // input_set_bytebutton_led

namespace {
WiFiUDP g_udp;
IPAddress g_masterIp;
uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastHeartbeatMs = 0;
wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;
bool g_wifiConnectRequested = false;
bool g_wifiAssociated = false;
bool g_wifiHasIp = false;
bool g_hasMasterSync = false;
bool g_delayActive = false;
bool g_reverbActive = false;
bool g_phaserActive = false;
bool g_isPlaying = false;       // local play state (toggled by PLAY button)
int  g_currentPattern = 0;      // 0-indexed, tracks PAT +/- navigation
bool g_muteAllFx = false;       // true when all three FX are muted together
// Module 1 state
bool g_flangerActive = false;
bool g_chorusActive = false;
bool g_compressorActive = false;
bool g_songMode = false;
int  g_currentStepCount = 16;   // 16/32/64

// Track volumes (0-150): synced with master via state_sync replies
static int16_t g_trackVolume[16] = {
  100, 100, 100, 100, 100, 100, 100, 100,
  100, 100, 100, 100, 100, 100, 100, 100
};

void udp_send_json(const JsonDocument& doc) {
  if (!udp_is_ready()) return;
  if (!g_udp.beginPacket(g_masterIp, cfg::kMasterUdpPort)) return;
  serializeJson(doc, g_udp);
  (void)g_udp.endPacket();
}

float map_range(float value, float inMin, float inMax, float outMin, float outMax) {
  if (inMax <= inMin) return outMin;
  float t = (value - inMin) / (inMax - inMin);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return outMin + (outMax - outMin) * t;
}

const char* status_to_str(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_wifiAssociated = false;
    g_wifiHasIp = false;
    g_hasMasterSync = false;
    g_wifiConnectRequested = false;
    if (!cfg::kDebugLog) return;
    Serial.printf(
      "[SlavePico][WiFi] disconnected reason=%d ssid=%s\n",
      static_cast<int>(info.wifi_sta_disconnected.reason),
      cfg::kWifiSsid
    );
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    g_wifiAssociated = true;
    g_wifiConnectRequested = true;
    if (!cfg::kDebugLog) return;
    Serial.printf("[SlavePico][WiFi] STA connected to %s\n", cfg::kWifiSsid);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_wifiAssociated = true;
    g_wifiHasIp = true;
    g_wifiConnectRequested = true;
    g_hasMasterSync = false;
    // Request an authoritative state on the next loop iteration.
    g_lastHeartbeatMs = millis() - cfg::kHeartbeatMs;
    if (!cfg::kDebugLog) return;
    Serial.printf("[SlavePico][WiFi] got ip=%s\n", WiFi.localIP().toString().c_str());
  }
}

void ensure_wifi_connected() {
  if (g_wifiHasIp) return;
  uint32_t now = millis();
  if (g_wifiConnectRequested) {
    if (now - g_lastWifiAttemptMs < cfg::kWifiConnectTimeoutMs) return;
    if (cfg::kDebugLog) Serial.println("[SlavePico][WiFi] connection attempt timed out; retrying");
    WiFi.disconnect();
    g_wifiConnectRequested = false;
    g_wifiAssociated = false;
  }
  if (now - g_lastWifiAttemptMs < cfg::kWifiReconnectMs) return;
  g_lastWifiAttemptMs = now;
  g_wifiConnectRequested = true;
  if (cfg::kDebugLog) {
    Serial.printf("[SlavePico][WiFi] connecting to %s\n", cfg::kWifiSsid);
  }
  WiFi.begin(cfg::kWifiSsid, cfg::kWifiPass);
}

// ByteButton module 0 — 8-button command pad
// Button layout:
//  0 = PLAY / PAUSE
//  1 = PATTERN -
//  2 = PATTERN +
//  3 = PATTERN 1 (jump to first pattern)
//  4 = MUTE ALL FX / UNMUTE ALL FX
//  5 = MUTE / UNMUTE PHASER
//  6 = MUTE / UNMUTE DELAY
//  7 = MUTE / UNMUTE REVERB
static void handle_bytebutton_b0(uint8_t button) {
  switch (button) {
    case 0: {  // PLAY / PAUSE
      g_isPlaying = !g_isPlaying;
      JsonDocument d;
      d["src"] = "SlavePico";
      d["cmd"] = g_isPlaying ? "start" : "stop";
      udp_send_json(d);
      // green = playing, dim red = stopped
      input_set_bytebutton_led(0, g_isPlaying ? 0 : 40, g_isPlaying ? 110 : 0, 0);
      break;
    }
    case 1: {  // PATTERN -
      if (g_currentPattern > 0) g_currentPattern--;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "selectPattern"; d["index"] = g_currentPattern;
      udp_send_json(d);
      input_set_bytebutton_led(1, 0, 60, 120);  // bright blue flash
      break;
    }
    case 2: {  // PATTERN +
      if (g_currentPattern < static_cast<int>(cfg::kMasterPatternCount - 1)) g_currentPattern++;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "selectPattern"; d["index"] = g_currentPattern;
      udp_send_json(d);
      input_set_bytebutton_led(2, 0, 60, 120);  // bright blue flash
      break;
    }
    case 3: {  // PATTERN 1 (index 0)
      g_currentPattern = 0;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "selectPattern"; d["index"] = 0;
      udp_send_json(d);
      input_set_bytebutton_led(3, 90, 80, 0);  // yellow
      break;
    }
    case 4: {  // MUTE ALL FX / UNMUTE ALL FX
      g_muteAllFx = !g_muteAllFx;
      bool active = !g_muteAllFx;
      g_delayActive  = active;
      g_reverbActive = active;
      g_phaserActive = active;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setDelayActive";  d["value"]=active; udp_send_json(d); }
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setReverbActive"; d["value"]=active; udp_send_json(d); }
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setPhaserActive"; d["value"]=active; udp_send_json(d); }
      // MUTE_ALL: red=all muted, dim olive=OK
      input_set_bytebutton_led(4, g_muteAllFx ? 120 : 20, g_muteAllFx ? 0 : 15, 0);
      input_set_bytebutton_led(5, active ? 70 : 15, 0, active ? 70 : 15);
      input_set_bytebutton_led(6, 0, active ? 10 : 5, active ? 100 : 20);
      input_set_bytebutton_led(7, 0, active ? 80 : 15, active ? 80 : 15);
      break;
    }
    case 5: {  // MUTE / UNMUTE PHASER
      g_phaserActive = !g_phaserActive;
      g_muteAllFx = false;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setPhaserActive"; d["value"]=g_phaserActive; udp_send_json(d); }
      input_set_bytebutton_led(5, g_phaserActive ? 70 : 15, 0, g_phaserActive ? 70 : 15);
      input_set_bytebutton_led(4, 20, 15, 0);  // mute_all no longer total
      break;
    }
    case 6: {  // MUTE / UNMUTE DELAY
      g_delayActive = !g_delayActive;
      g_muteAllFx = false;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setDelayActive"; d["value"]=g_delayActive; udp_send_json(d); }
      input_set_bytebutton_led(6, 0, g_delayActive ? 10 : 5, g_delayActive ? 100 : 20);
      input_set_bytebutton_led(4, 20, 15, 0);
      break;
    }
    case 7: {  // MUTE / UNMUTE REVERB
      g_reverbActive = !g_reverbActive;
      g_muteAllFx = false;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setReverbActive"; d["value"]=g_reverbActive; udp_send_json(d); }
      input_set_bytebutton_led(7, 0, g_reverbActive ? 80 : 15, g_reverbActive ? 80 : 15);
      input_set_bytebutton_led(4, 20, 15, 0);
      break;
    }
    default: break;
  }
  if (cfg::kDebugLog) {
    Serial.printf("[SlavePico][BTN0] btn=%u play=%d pat=%d muteAll=%d d=%d r=%d p=%d\n",
      button, (int)g_isPlaying, g_currentPattern, (int)g_muteAllFx,
      (int)g_delayActive, (int)g_reverbActive, (int)g_phaserActive);
  }
}

// ByteButton module 1 — 8-button performance pad
// B0=CLEAR PATTERN, B1=STEPS×16, B2=STEPS×32, B3=STEPS×64
// B4=FLANGER,       B5=CHORUS,    B6=COMPRESSOR,  B7=SONG MODE
static void handle_bytebutton_b1(uint8_t button) {
  // idle colors for this module:
  // B0=red, B1=white-dim, B2=blue, B3=cyan, B4=pink, B5=orange, B6=green, B7=yellow
  auto led_idle1 = [](uint8_t btn) {
    static const uint8_t kR[8] = {40, 20,  0,  0, 50, 45,  0, 35};
    static const uint8_t kG[8] = { 0, 20, 12, 30,  0, 20, 50, 30};
    static const uint8_t kB[8] = { 0, 20, 55, 35, 25,  0,  0,  0};
    input_set_bytebutton1_led(btn, kR[btn], kG[btn], kB[btn]);
  };
  (void)led_idle1;  // may be unused if all cases explicit

  switch (button) {
    case 0: {  // CLEAR CURRENT PATTERN (destructive — bright red flash)
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "clearPattern"; d["pattern"] = g_currentPattern;
      udp_send_json(d);
      // Triple flash: red → off → red → settle to dim red
      input_set_bytebutton1_led(0, 120, 0, 0);
      break;
    }
    case 1: {  // STEPS × 16
      g_currentStepCount = 16;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "setStepCount"; d["count"] = 16;
      udp_send_json(d);
      input_set_bytebutton1_led(1, 60, 60, 60);  // bright white flash
      input_set_bytebutton1_led(2, 0, 12, 55);
      input_set_bytebutton1_led(3, 0, 30, 35);
      break;
    }
    case 2: {  // STEPS × 32
      g_currentStepCount = 32;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "setStepCount"; d["count"] = 32;
      udp_send_json(d);
      input_set_bytebutton1_led(2, 0, 40, 120);  // bright blue flash
      input_set_bytebutton1_led(1, 20, 20, 20);
      input_set_bytebutton1_led(3, 0, 30, 35);
      break;
    }
    case 3: {  // STEPS × 64
      g_currentStepCount = 64;
      JsonDocument d;
      d["src"] = "SlavePico"; d["cmd"] = "setStepCount"; d["count"] = 64;
      udp_send_json(d);
      input_set_bytebutton1_led(3, 0, 80, 80);   // bright cyan flash
      input_set_bytebutton1_led(1, 20, 20, 20);
      input_set_bytebutton1_led(2, 0, 12, 55);
      break;
    }
    case 4: {  // FLANGER toggle
      g_flangerActive = !g_flangerActive;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setFlangerActive"; d["value"]=g_flangerActive; udp_send_json(d); }
      input_set_bytebutton1_led(4, g_flangerActive ? 100 : 50, 0, g_flangerActive ? 50 : 25);
      break;
    }
    case 5: {  // CHORUS toggle
      g_chorusActive = !g_chorusActive;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setChorusActive"; d["value"]=g_chorusActive; udp_send_json(d); }
      input_set_bytebutton1_led(5, g_chorusActive ? 90 : 45, g_chorusActive ? 40 : 20, 0);
      break;
    }
    case 6: {  // COMPRESSOR toggle
      g_compressorActive = !g_compressorActive;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setCompressorActive"; d["value"]=g_compressorActive; udp_send_json(d); }
      input_set_bytebutton1_led(6, 0, g_compressorActive ? 100 : 50, 0);
      break;
    }
    case 7: {  // SONG MODE toggle
      g_songMode = !g_songMode;
      { JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setSongMode"; d["enabled"]=g_songMode; d["length"]=8; udp_send_json(d); }
      input_set_bytebutton1_led(7, g_songMode ? 80 : 35, g_songMode ? 70 : 30, 0);
      break;
    }
    default: break;
  }
  if (cfg::kDebugLog) {
    Serial.printf("[SlavePico][BTN1] btn=%u flng=%d chs=%d cmp=%d song=%d steps=%d\n",
      button, (int)g_flangerActive, (int)g_chorusActive,
      (int)g_compressorActive, (int)g_songMode, g_currentStepCount);
  }
}

static void vol_to_rgb(int v, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (v < 0) v = 0; if (v > 150) v = 150;
  if (v <= 50)       { r = 0; g = (uint8_t)(v * 30 / 50); b = (uint8_t)((50 - v) * 25 / 50 + 5); }
  else if (v <= 100) { r = 0; g = (uint8_t)(30 + (v - 50) * 30 / 50); b = 0; }
  else               { r = (uint8_t)((v - 100) * 50 / 50); g = 60; b = 0; }
}

// Per-track throttle: max 1 setTrackVolume per 40ms (25 Hz) to avoid flooding
static uint32_t g_lastVolumeEventMs[16] = {};

static void handle_encoder_volume(const InputEvent& ev) {
  uint8_t bankOffset = (ev.controlId == devices::CTRL_M5_ENC_BANK_0) ? 0 : 8;
  uint8_t track = bankOffset + ev.elementId;
  if (track >= 16) return;
  uint32_t nowMs = millis();
  if (ev.eventType == 0 && (nowMs - g_lastVolumeEventMs[track] < 40)) return;
  g_lastVolumeEventMs[track] = nowMs;
  if (ev.eventType == 0) {
    int16_t nv = (int16_t)(g_trackVolume[track] + ev.value);
    if (nv < 0) nv = 0; if (nv > 150) nv = 150;
    g_trackVolume[track] = nv;
    JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setTrackVolume"; d["track"]=track; d["volume"]=nv;
    udp_send_json(d);
    uint8_t r, gv, b; vol_to_rgb(nv, r, gv, b);
    if (ev.controlId == devices::CTRL_M5_ENC_BANK_0) input_set_enc0_led(ev.elementId, r, gv, b);
    else                                               input_set_enc1_led(ev.elementId, r, gv, b);
    if (cfg::kDebugLog) Serial.printf("[SlavePico][VOL] track=%u vol=%d\n", track, nv);
  } else if (ev.eventType == 1) {
    g_trackVolume[track] = 100;
    JsonDocument d; d["src"]="SlavePico"; d["cmd"]="setTrackVolume"; d["track"]=track; d["volume"]=100;
    udp_send_json(d);
    uint8_t r, gv, b; vol_to_rgb(100, r, gv, b);
    if (ev.controlId == devices::CTRL_M5_ENC_BANK_0) input_set_enc0_led(ev.elementId, r, gv, b);
    else                                               input_set_enc1_led(ev.elementId, r, gv, b);
    if (cfg::kDebugLog) Serial.printf("[SlavePico][VOL] track=%u reset vol=100\n", track);
  }
}

} // namespace

void udp_handler_init() {
  esp_log_level_set("rpc_core", ESP_LOG_NONE);
  esp_log_level_set("esp32-hal-hosted", ESP_LOG_NONE);
  esp_log_level_set("esp32-hal-hosted.c", ESP_LOG_NONE);
  esp_log_level_set("STA", ESP_LOG_WARN);
  WiFi.onEvent(on_wifi_event);
  // Give the P4<->C6 hosted stack a short warm-up window after boot.
  delay(2500);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  g_masterIp.fromString(cfg::kMasterIp);
  g_udp.begin(cfg::kLocalUdpPort);
  ensure_wifi_connected();
}

void udp_handler_process() {
  ensure_wifi_connected();

  wl_status_t st = WiFi.status();
  if (st != g_lastWifiStatus) {
    g_lastWifiStatus = st;
    if (cfg::kDebugLog) {
      if (st == WL_CONNECTED) {
        Serial.printf("[SlavePico][WiFi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else {
        Serial.printf("[SlavePico][WiFi] status=%d (%s)\n", static_cast<int>(st), status_to_str(st));
      }
    }
  }

  uint32_t now = millis();
  if (udp_is_ready() && (now - g_lastHeartbeatMs >= cfg::kHeartbeatMs)) {
    g_lastHeartbeatMs = now;
    udp_send_heartbeat();
  }

  // Accept control state only from the configured master.
  while (int rxLen = g_udp.parsePacket()) {
    if (g_udp.remoteIP() != g_masterIp || rxLen <= 0 || rxLen >= static_cast<int>(cfg::kUdpMaxPacketBytes)) {
      while (g_udp.available()) g_udp.read();
      continue;
    }
    static char rxBuf[cfg::kUdpMaxPacketBytes];
    int n = g_udp.read(rxBuf, sizeof(rxBuf) - 1);
    if (n <= 0) continue;
    rxBuf[n] = 0;
    JsonDocument rxDoc;
    if (deserializeJson(rxDoc, rxBuf) != DeserializationError::Ok) continue;
    const char* rxCmd = rxDoc["cmd"] | "";
    if (strcmp(rxCmd, "state_sync") == 0) {
      g_hasMasterSync = true;
      g_currentPattern = constrain(rxDoc["pattern"] | g_currentPattern, 0, static_cast<int>(cfg::kMasterPatternCount - 1));
      g_isPlaying = rxDoc["playing"] | g_isPlaying;
      int stepCount = rxDoc["stepCount"] | g_currentStepCount;
      if (stepCount == 16 || stepCount == 32 || stepCount == 64) g_currentStepCount = stepCount;
      JsonObject fx = rxDoc["fx"];
      if (fx) {
        if (!fx["delayActive"].isNull()) g_delayActive = fx["delayActive"].as<bool>();
        if (!fx["reverbActive"].isNull()) g_reverbActive = fx["reverbActive"].as<bool>();
        if (!fx["phaserActive"].isNull()) g_phaserActive = fx["phaserActive"].as<bool>();
        if (!fx["flangerActive"].isNull()) g_flangerActive = fx["flangerActive"].as<bool>();
        if (!fx["chorusActive"].isNull()) g_chorusActive = fx["chorusActive"].as<bool>();
        if (!fx["compressorActive"].isNull()) g_compressorActive = fx["compressorActive"].as<bool>();
        g_muteAllFx = !g_delayActive && !g_reverbActive && !g_phaserActive;
      }
      JsonArray vols = rxDoc["trackVolumes"];
      if (vols) {
        int t = 0;
        for (JsonVariant v : vols) {
          if (t >= 16) break;
          int16_t vol = (int16_t)v.as<int>();
          if (vol < 0) vol = 0; if (vol > 150) vol = 150;
          g_trackVolume[t] = vol; t++;
        }
        for (uint8_t enc = 0; enc < 8; enc++) {
          uint8_t r, gv, b; vol_to_rgb(g_trackVolume[enc], r, gv, b);
          input_set_enc0_led(enc, r, gv, b);
        }
        for (uint8_t enc = 0; enc < 8; enc++) {
          uint8_t r, gv, b; vol_to_rgb(g_trackVolume[8 + enc], r, gv, b);
          input_set_enc1_led(enc, r, gv, b);
        }
      }
    }
  }
}

void udp_send_event(const InputEvent& ev) {
  if (!udp_is_ready()) return;

  JsonDocument doc;
  doc["src"] = "SlavePico";

  if (ev.controlId >= devices::CTRL_DF_ROTARY_0 && ev.controlId <= devices::CTRL_DF_ROTARY_3) {
    if (ev.eventType == 1) {
      switch (ev.controlId) {
        case devices::CTRL_DF_ROTARY_0:
          doc["cmd"] = "tempo";
          doc["value"] = 120;
          doc["input"] = "dfRotaryButton";
          doc["param"] = "bpmReset";
          break;
        case devices::CTRL_DF_ROTARY_1:
          g_delayActive = !g_delayActive;
          doc["cmd"] = "setDelayActive";
          doc["value"] = g_delayActive;
          doc["input"] = "dfRotaryButton";
          doc["param"] = "delayActive";
          break;
        case devices::CTRL_DF_ROTARY_2:
          g_reverbActive = !g_reverbActive;
          doc["cmd"] = "setReverbActive";
          doc["value"] = g_reverbActive;
          doc["input"] = "dfRotaryButton";
          doc["param"] = "reverbActive";
          break;
        case devices::CTRL_DF_ROTARY_3:
          g_phaserActive = !g_phaserActive;
          doc["cmd"] = "setPhaserActive";
          doc["value"] = g_phaserActive;
          doc["input"] = "dfRotaryButton";
          doc["param"] = "phaserActive";
          break;
      }
    } else {
      float raw = static_cast<float>(ev.value);
      switch (ev.controlId) {
        case devices::CTRL_DF_ROTARY_0:
          doc["cmd"] = "tempo";
          doc["value"] = map_range(raw, 0.0f, 1023.0f, cfg::kTempoMin, cfg::kTempoMax);
          doc["input"] = "dfRotary";
          doc["param"] = "bpm";
          break;
        case devices::CTRL_DF_ROTARY_1:
        {
          if (!g_delayActive) {
            JsonDocument activeDoc;
            g_delayActive = true;
            activeDoc["src"] = "SlavePico";
            activeDoc["cmd"] = "setDelayActive";
            activeDoc["value"] = true;
            udp_send_json(activeDoc);
          }
          doc["cmd"] = "setDelayMix";
          doc["value"] = map_range(raw, 0.0f, 1023.0f, 0.0f, 100.0f);
          doc["input"] = "dfRotary";
          doc["param"] = "delayMix";
          break;
        }
        case devices::CTRL_DF_ROTARY_2:
        {
          if (!g_reverbActive) {
            JsonDocument activeDoc;
            g_reverbActive = true;
            activeDoc["src"] = "SlavePico";
            activeDoc["cmd"] = "setReverbActive";
            activeDoc["value"] = true;
            udp_send_json(activeDoc);
          }
          doc["cmd"] = "setReverbMix";
          doc["value"] = map_range(raw, 0.0f, 1023.0f, 0.0f, 100.0f);
          doc["input"] = "dfRotary";
          doc["param"] = "reverbMix";
          break;
        }
        case devices::CTRL_DF_ROTARY_3:
        {
          if (!g_phaserActive) {
            JsonDocument activeDoc;
            g_phaserActive = true;
            activeDoc["src"] = "SlavePico";
            activeDoc["cmd"] = "setPhaserActive";
            activeDoc["value"] = true;
            udp_send_json(activeDoc);
          }
          doc["cmd"] = "setPhaserDepth";
          doc["value"] = map_range(raw, 0.0f, 1023.0f, 0.0f, 100.0f);
          doc["input"] = "dfRotary";
          doc["param"] = "phaserDepth";
          break;
        }
      }
    }
    doc["id"] = ev.controlId;
  } else if (ev.controlId == devices::CTRL_BYTEBTN_0 && ev.eventType == 1) {
    handle_bytebutton_b0(ev.elementId);
    return;
  } else if (ev.controlId == devices::CTRL_BYTEBTN_1 && ev.eventType == 1) {
    handle_bytebutton_b1(ev.elementId);
    return;
  } else if (ev.controlId == devices::CTRL_M5_ENC_BANK_0 || ev.controlId == devices::CTRL_M5_ENC_BANK_1) {
    handle_encoder_volume(ev);
    return;
  } else {
    doc["cmd"] = "slaveInput";
    doc["id"] = ev.controlId;
    doc["element"] = ev.elementId;
    doc["type"] = ev.eventType;
    doc["value"] = ev.value;
  }

  if (ev.controlId == devices::CTRL_I2C_HUB) {
    doc["input"] = "i2cHub";
    doc["present"] = (ev.value != 0);
  }

  udp_send_json(doc);

  if (cfg::kDebugLog) {
    const char* cmd = doc["cmd"] | "?";
    Serial.printf("[SlavePico][UDP] event id=%u type=%u cmd=%s value=%d\n", ev.controlId, ev.eventType, cmd, ev.value);
  }
}

void udp_send_heartbeat() {
  if (!udp_is_ready()) return;

  JsonDocument doc;
  doc["cmd"] = "hello";
  doc["src"] = "SlavePico";
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();

  udp_send_json(doc);

  if (cfg::kDebugLog) {
    Serial.printf("[SlavePico][UDP] heartbeat -> %s:%u\n", cfg::kMasterIp, cfg::kMasterUdpPort);
  }
}

bool udp_is_ready() {
  return g_wifiHasIp && WiFi.status() == WL_CONNECTED;
}
