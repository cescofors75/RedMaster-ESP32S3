#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

inline bool webRequestHasTrustedOrigin(AsyncWebServerRequest* request) {
  if (!request || !request->hasHeader("Origin")) return true;
  const String origin = request->header("Origin");
  const String host = request->host();
  return origin == (String("http://") + host) || origin == (String("https://") + host);
}

inline bool webSocketClientReady(AsyncWebSocketClient* client) {
  return client != nullptr && client->status() == WS_CONNECTED;
}

inline void webSendCommandStatus(AsyncWebSocketClient* client, const char* type,
                                 const char* cmd, const char* code = nullptr,
                                 const char* message = nullptr) {
  if (!webSocketClientReady(client)) return;
  StaticJsonDocument<256> reply;
  reply["type"] = type;
  if (cmd && cmd[0]) reply["cmd"] = cmd;
  reply["ok"] = strcmp(type, "ack") == 0;
  if (code && code[0]) reply["code"] = code;
  if (message && message[0]) reply["msg"] = message;
  char output[256];
  const size_t len = serializeJson(reply, output, sizeof(output));
  if (len > 0 && len < sizeof(output)) client->text(output, len);
}
