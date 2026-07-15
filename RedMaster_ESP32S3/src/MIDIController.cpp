#include "MIDIController.h"

// Static callback wrapper
static MIDIController* s_midiInstance = nullptr;
// volatile: escrito desde el callback USB (contexto USB host) y leido/escrito
// desde readMidiData()/closeMidiDevice() en la task principal.
static volatile bool s_transferSubmitted = false;
static volatile bool s_transferCallbackActive = false;
static volatile bool s_closingDevice = false;
// Protege el ring buffer de historial + contadores, que se escriben desde el
// callback USB (Core0) y se leen desde la task web (AsyncTCP, Core0) — tasks
// distintas que se pueden expropiar entre si.
static portMUX_TYPE s_midiHistMux = portMUX_INITIALIZER_UNLOCKED;

// Transfer callback — se llama cuando USB Host completa una lectura
static void transferCallback(usb_transfer_t* transfer) {
  if (!transfer || !transfer->context) return;
  s_transferCallbackActive = true;
  MIDIController* ctrl = static_cast<MIDIController*>(transfer->context);

  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
    ctrl->handleMIDIData(transfer->data_buffer, transfer->actual_num_bytes);
  }

  // Re-submit para lectura continua
  if (!s_closingDevice &&
      transfer->status != USB_TRANSFER_STATUS_CANCELED &&
      transfer->status != USB_TRANSFER_STATUS_STALL) {
    if (usb_host_transfer_submit(transfer) != ESP_OK) {
      s_transferSubmitted = false;
    }
  } else {
    // STALL/CANCELED: el transfer NO vuelve a estar en vuelo. Sin esto el flag
    // quedaba en true para siempre y readMidiData() abortaba en cada llamada,
    // matando la entrada MIDI hasta reiniciar. Liberamos el flag para que
    // readMidiData() pueda reenviar (recuperacion automatica tras un STALL).
    s_transferSubmitted = false;
  }
  s_transferCallbackActive = false;
}

MIDIController::MIDIController() 
  : clientHandle(nullptr)
  , hostTaskHandle(nullptr)
  , initialized(false)
  , hostInitialized(false)
  , deviceHandle(nullptr)
  , midiTransfer(nullptr)
  , midiEndpointAddress(0)
  , midiMaxPacketSize(0)
  , interfaceNum(-1)
  , historyIndex(0)
  , historyCount(0)
  , totalMessages(0)
  , messagesPerSecond(0)
  , lastSecondTime(0)
  , messagesThisSecond(0)
  , mappingCount(0)
  , mappingsDirty(false)
  , mappingsDirtyAt(0)
{
  deviceInfo.connected = false;
  deviceInfo.deviceName = "No device";
  deviceInfo.vendorId = 0;
  deviceInfo.productId = 0;
  deviceInfo.connectTime = 0;
  
  // Evitar acceso NVS en constructor global: NVS puede no estar listo todavia.
  initializeDefaultMappings();
  
  scanEnabled = false;  // Desactivado por defecto, se activa desde la web
  
  s_midiInstance = this;
}

MIDIController::~MIDIController() {
  closeMidiDevice();
  
  if (hostTaskHandle) {
    vTaskDelete(hostTaskHandle);
  }
  
  if (clientHandle) {
    usb_host_client_unblock(clientHandle);
    usb_host_client_deregister(clientHandle);
  }
  
  if (hostInitialized) {
    usb_host_uninstall();
  }
  
  s_midiInstance = nullptr;
}

bool MIDIController::begin() {

  // Install USB Host driver
  usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1
  };

  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    return false;
  }
  hostInitialized = true;

  // Register client
  usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {
      .client_event_callback = clientEventCallback,
      .callback_arg = this
    }
  };

  err = usb_host_client_register(&clientConfig, &clientHandle);
  if (err != ESP_OK) {
    usb_host_uninstall();
    hostInitialized = false;
    return false;
  }

  // Create USB host task
  xTaskCreatePinnedToCore(
    usbHostTask,
    "usb_host_task",
    4096,
    this,
    5,
    &hostTaskHandle,
    0  // Core 0 (WiFi/System core)
  );

  initialized = true;
  return true;
}

void MIDIController::usbHostTask(void* arg) {
  MIDIController* controller = static_cast<MIDIController*>(arg);
  
  
  uint32_t lastDebugTime = 0;
  uint32_t pollCount = 0;
  
  while (true) {
    if (controller->scanEnabled) {
      // Handle USB host events
      usb_host_client_handle_events(controller->clientHandle, pdMS_TO_TICKS(10));
      usb_host_lib_handle_events(pdMS_TO_TICKS(10), nullptr);
      
      pollCount++;
      
      // Debug output every 5 seconds to show the task is alive
      if (millis() - lastDebugTime > 5000) {
        pollCount = 0;
        lastDebugTime = millis();
      }
      
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // Scan disabled - sleep longer to save CPU
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

void MIDIController::clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg) {
  MIDIController* controller = static_cast<MIDIController*>(arg);
  
  
  if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    
    // Open MIDI device and start reading
    if (controller->openMidiDevice(eventMsg->new_dev.address)) {
      controller->deviceInfo.connected = true;
      controller->deviceInfo.deviceName = "USB MIDI Device";
      controller->deviceInfo.connectTime = millis();
      controller->notifyDeviceChange(true);
    } else {
    }
    
  } else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    
    controller->closeMidiDevice();
    controller->deviceInfo.connected = false;
    controller->notifyDeviceChange(false);
  }
}

void MIDIController::update() {
  // Flush diferido de mapeos a NVS (debounce). Se ejecuta aunque el escaneo
  // este desactivado, para no perder ediciones hechas desde la web.
  if (mappingsDirty && (millis() - mappingsDirtyAt) >= kMappingsFlushDelayMs) {
    mappingsDirty = false;
    saveMappings();
  }

  if (!initialized || !scanEnabled) return;

  // Update messages per second counter
  uint32_t now = millis();
  if (now - lastSecondTime >= 1000) {
    portENTER_CRITICAL(&s_midiHistMux);
    messagesPerSecond = messagesThisSecond;
    messagesThisSecond = 0;
    portEXIT_CRITICAL(&s_midiHistMux);
    lastSecondTime = now;
  }

  // Read MIDI data from USB device
  if (deviceHandle && midiTransfer) {
    readMidiData();
  }
}

void MIDIController::handleMIDIData(const uint8_t* data, size_t length) {
  // USB MIDI packets: 4 bytes cada uno [Cable/CIN, Status, Data1, Data2]
  // CIN (Code Index Number) en nibble bajo del primer byte indica el tipo de mensaje
  
  for (size_t i = 0; i + 3 < length; i += 4) {
    uint8_t header = data[i];
    uint8_t status = data[i + 1];
    uint8_t data1 = data[i + 2];
    uint8_t data2 = data[i + 3];
    
    uint8_t cin = header & 0x0F; // Code Index Number
    
    // Filtrar paquetes válidos según CIN
    // 0x08 = Note Off, 0x09 = Note On, 0x0B = Control Change, 0x0E = Pitch Bend, etc.
    if (cin >= 0x08 && cin <= 0x0F) {
      // Procesar solo si el status byte es válido (bit 7 = 1)
      if (status & 0x80) {
        processMIDIMessage(status, data1, data2);
      }
    }
  }
}

void MIDIController::processMIDIMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  MIDIMessage msg;
  msg.type = status & 0xF0;
  msg.channel = status & 0x0F;
  msg.data1 = data1;
  msg.data2 = data2;
  msg.timestamp = millis();
  
  // Add to history + estadisticas bajo seccion critica (lado escritor).
  portENTER_CRITICAL(&s_midiHistMux);
  messageHistory[historyIndex] = msg;
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) {
    historyCount++;
  }
  totalMessages++;
  messagesThisSecond++;
  portEXIT_CRITICAL(&s_midiHistMux);

  // Notify callback (fuera de la seccion critica: ejecuta codigo de usuario)
  if (messageCallback) {
    messageCallback(msg);
  }
  
  // Debug output
  const char* msgType = "Unknown";
  const char* icon = "🎵";
  switch (msg.type) {
    case MIDI_NOTE_ON: msgType = "Note On"; icon = "🎹"; break;
    case MIDI_NOTE_OFF: msgType = "Note Off"; icon = "⬜"; break;
    case MIDI_CONTROL_CHANGE: msgType = "CC"; icon = "🎛️"; break;
    case MIDI_PITCH_BEND: msgType = "Pitch Bend"; icon = "🎚️"; break;
    case MIDI_PROGRAM_CHANGE: msgType = "Program"; icon = "🎼"; break;
  }
  
}

void MIDIController::getRecentMessages(MIDIMessage* buffer, size_t& count, size_t maxCount) {
  // Snapshot bajo la misma seccion critica que el escritor para evitar leer un
  // indice/contador inconsistente o una MIDIMessage a medio escribir.
  portENTER_CRITICAL(&s_midiHistMux);
  count = min(historyCount, maxCount);
  size_t startIdx = (historyIndex + MAX_HISTORY - count) % MAX_HISTORY;
  for (size_t i = 0; i < count; i++) {
    size_t idx = (startIdx + i) % MAX_HISTORY;
    buffer[i] = messageHistory[idx];
  }
  portEXIT_CRITICAL(&s_midiHistMux);
}

void MIDIController::notifyDeviceChange(bool connected) {
  if (deviceCallback) {
    deviceCallback(connected, deviceInfo);
  }
  
  if (connected) {
  } else {
  }
}

bool MIDIController::openMidiDevice(uint8_t deviceAddress) {
  // Close previous device if exists
  if (deviceHandle) {
    closeMidiDevice();
    if (deviceHandle) {
      // Cancellation callback is still pending; opening over the old transfer
      // would either leak it or make its callback target the new device.
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for cleanup
  }
  s_transferSubmitted = false;
  
  // Open the device
  esp_err_t err = usb_host_device_open(clientHandle, deviceAddress, &deviceHandle);
  if (err != ESP_OK) {
    return false;
  }
  
  // Get device descriptor
  const usb_device_desc_t* deviceDesc;
  err = usb_host_get_device_descriptor(deviceHandle, &deviceDesc);
  if (err != ESP_OK) {
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
    return false;
  }
  
  deviceInfo.vendorId = deviceDesc->idVendor;
  deviceInfo.productId = deviceDesc->idProduct;
  
  // Get configuration descriptor
  const usb_config_desc_t* configDesc;
  err = usb_host_get_active_config_descriptor(deviceHandle, &configDesc);
  if (err != ESP_OK) {
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
    return false;
  }
  
  // Listar TODAS las interfaces para debug
  const usb_standard_desc_t* desc = (const usb_standard_desc_t*)configDesc;
  uint16_t totalLength = configDesc->wTotalLength;
  uint16_t offset = 0;
  
  while (offset < totalLength) {
    desc = (const usb_standard_desc_t*)((uint8_t*)configDesc + offset);
    
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* intfDesc = (const usb_intf_desc_t*)desc;
    }
    
    offset += desc->bLength;
  }
  
  // ── Buscar interfaz compatible (prioridad: MIDI Streaming > CDC > Vendor) ──
  interfaceNum = -1;
  offset = 0;
  int cdcInterface    = -1;
  int vendorInterface = -1;
  int anyInterface    = -1;

  while (offset < totalLength) {
    desc = (const usb_standard_desc_t*)((uint8_t*)configDesc + offset);

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* intfDesc = (const usb_intf_desc_t*)desc;

      // Clase 0x01 Audio, Subclase 0x03 MIDI Streaming — Arturia, Roland, Korg...
      if (intfDesc->bInterfaceClass == 0x01 && intfDesc->bInterfaceSubClass == 0x03) {
        interfaceNum = intfDesc->bInterfaceNumber;
        break;
      }
      // CDC Data 0x0A
      if (intfDesc->bInterfaceClass == 0x0A && cdcInterface < 0)
        cdcInterface = intfDesc->bInterfaceNumber;
      // Vendor Specific 0xFF
      if (intfDesc->bInterfaceClass == 0xFF && vendorInterface < 0)
        vendorInterface = intfDesc->bInterfaceNumber;
      // Cualquiera con endpoints
      if (intfDesc->bNumEndpoints > 0 && anyInterface < 0)
        anyInterface = intfDesc->bInterfaceNumber;
    }

    offset += desc->bLength;
  }

  // Fallback en orden de preferencia
  if (interfaceNum < 0) interfaceNum = cdcInterface;
  if (interfaceNum < 0) interfaceNum = vendorInterface;
  if (interfaceNum < 0) interfaceNum = anyInterface;
  if (interfaceNum < 0) interfaceNum = 0; // último recurso

  
  // Claim the interface
  err = usb_host_interface_claim(clientHandle, deviceHandle, interfaceNum, 0);
  if (err != ESP_OK) {
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
    return false;
  }
  
  // Find BULK IN endpoint for data (prefer BULK over INTERRUPT)
  offset = 0;
  midiEndpointAddress = 0;
  midiMaxPacketSize = 64;
  uint8_t fallbackEndpoint = 0;
  
  
  while (offset < totalLength) {
    desc = (const usb_standard_desc_t*)((uint8_t*)configDesc + offset);
    
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t* epDesc = (const usb_ep_desc_t*)desc;
      
      uint8_t epType = epDesc->bmAttributes & 0x03;
      bool isIN = (epDesc->bEndpointAddress & 0x80);
      
      if (isIN) {
        const char* typeStr = (epType == 0x02) ? "BULK" : 
                             (epType == 0x03) ? "INTERRUPT" : "OTHER";
        
        // Preferir BULK (0x02) para datos
        if (epType == 0x02) {
          midiEndpointAddress = epDesc->bEndpointAddress;
          midiMaxPacketSize = epDesc->wMaxPacketSize;
        } else if (epType == 0x03 && midiEndpointAddress == 0) {
          // Usar INTERRUPT solo si no hay BULK
          fallbackEndpoint = epDesc->bEndpointAddress;
          midiMaxPacketSize = epDesc->wMaxPacketSize;
        }
      }
    }
    
    offset += desc->bLength;
  }
  
  // Si no encontramos BULK, usar INTERRUPT
  if (midiEndpointAddress == 0 && fallbackEndpoint != 0) {
    midiEndpointAddress = fallbackEndpoint;
  } else if (midiEndpointAddress != 0) {
  }
  
  if (midiEndpointAddress == 0) {
    usb_host_interface_release(clientHandle, deviceHandle, interfaceNum);
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
    return false;
  }
  
  // Allocate transfer for reading data
  err = usb_host_transfer_alloc(midiMaxPacketSize, 0, &midiTransfer);
  if (err != ESP_OK) {
    usb_host_interface_release(clientHandle, deviceHandle, interfaceNum);
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
    return false;
  }
  
  return true;
}

void MIDIController::closeMidiDevice() {
  // A submitted transfer owns midiTransfer until its CANCELLED callback has
  // been delivered. Halt+flush cancels queued/in-flight work; then drain client
  // events before freeing the object. Freeing first was a use-after-free when a
  // cable was removed while the callback was still running/resubmitting.
  if (midiTransfer) s_closingDevice = true;
  if (midiTransfer && deviceHandle && midiEndpointAddress != 0 && s_transferSubmitted) {
    usb_host_endpoint_halt(deviceHandle, midiEndpointAddress);
    usb_host_endpoint_flush(deviceHandle, midiEndpointAddress);
    const uint32_t deadline = millis() + 250;
    while ((s_transferSubmitted || s_transferCallbackActive) &&
           (int32_t)(deadline - millis()) > 0) {
      usb_host_client_handle_events(clientHandle, pdMS_TO_TICKS(5));
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  // If the host did not deliver cancellation, do not free memory still owned
  // by USB. Keep it for the next close attempt; this is safer than a UAF.
  if (s_transferSubmitted || s_transferCallbackActive) {
    return;
  }
  s_transferSubmitted = false;

  if (midiTransfer) {
    usb_host_transfer_free(midiTransfer);
    midiTransfer = nullptr;
  }
  
  // Release interface before closing device
  if (deviceHandle && interfaceNum >= 0) {
    esp_err_t err = usb_host_interface_release(clientHandle, deviceHandle, interfaceNum);
    if (err == ESP_OK) {
    }
    interfaceNum = -1;
  }
  
  if (deviceHandle) {
    usb_host_device_close(clientHandle, deviceHandle);
    deviceHandle = nullptr;
  }
  
  midiEndpointAddress = 0;
  midiMaxPacketSize = 0;
  s_closingDevice = false;
  
}

void MIDIController::readMidiData() {
  if (!midiTransfer || !deviceHandle) return;

  // Solo enviar si el transfer NO está ya en vuelo
  // (el callback se encarga de re-submitear automáticamente)
  if (s_transferSubmitted) return;

  midiTransfer->device_handle    = deviceHandle;
  midiTransfer->bEndpointAddress = midiEndpointAddress;
  midiTransfer->callback         = transferCallback;
  midiTransfer->context          = this;
  midiTransfer->num_bytes        = midiMaxPacketSize;
  midiTransfer->timeout_ms       = 0; // 0 = sin timeout, lectura continua

  esp_err_t err = usb_host_transfer_submit(midiTransfer);
  if (err == ESP_OK) {
    s_transferSubmitted = true;
  } else {
  }
}

// ========================================
// MIDI NOTE MAPPING FUNCTIONS
// ========================================

void MIDIController::setPadMapping(int8_t pad, uint8_t newNote) {
  // Buscar en los primeros 16 (mapeos principales) el que tenga este pad
  for (int i = 0; i < mappingCount && i < 16; i++) {
    if (noteMappings[i].pad == pad) {
      noteMappings[i].note    = newNote;
      noteMappings[i].enabled = true;
      markMappingsDirty();
      return;
    }
  }
  // Si no estaba en los primeros 16, añadir nuevo
  setNoteMapping(newNote, pad);
}

void MIDIController::setNoteMapping(uint8_t note, int8_t pad) {
  // Buscar si ya existe mapeo para esta nota
  for (int i = 0; i < mappingCount; i++) {
    if (noteMappings[i].note == note) {
      noteMappings[i].pad = pad;
      noteMappings[i].enabled = (pad >= 0);
      markMappingsDirty();
      return;
    }
  }

  // Agregar nuevo mapeo si hay espacio
  if (mappingCount < MAX_MIDI_MAPPINGS) {
    noteMappings[mappingCount].note = note;
    noteMappings[mappingCount].pad = pad;
    noteMappings[mappingCount].enabled = (pad >= 0);
    mappingCount++;
    markMappingsDirty();
  } else {
  }
}

int8_t MIDIController::getMappedPad(uint8_t note) const {
  for (int i = 0; i < mappingCount; i++) {
    if (noteMappings[i].note == note && noteMappings[i].enabled) {
      return noteMappings[i].pad;
    }
  }
  return -1; // No mapping found
}

void MIDIController::clearMapping(uint8_t note) {
  for (int i = 0; i < mappingCount; i++) {
    if (noteMappings[i].note == note) {
      noteMappings[i].enabled = false;
      markMappingsDirty();  // antes no se persistia: el cambio se perdia al reiniciar
      return;
    }
  }
}

void MIDIController::resetToDefaultMapping() {
  initializeDefaultMappings();
  markMappingsDirty();
}

void MIDIController::markMappingsDirty() {
  mappingsDirty = true;
  mappingsDirtyAt = millis();
}

void MIDIController::initializeDefaultMappings() {
  mappingCount = 0;

  // ================================================================
  // Mapa General MIDI Drum (GM) completo para los 16 pads RED808
  // ================================================================
  // Pad  0: BD  (Bass Drum)      → 36 C1  (Acoustic Bass Drum)
  // Pad  1: SD  (Snare)          → 38 D1  (Acoustic Snare)
  // Pad  2: CH  (Closed Hi-Hat)  → 42 F#1 (Closed Hi-Hat)
  // Pad  3: OH  (Open Hi-Hat)    → 46 A#1 (Open Hi-Hat)
  // Pad  4: CY  (Cymbal)         → 49 C#2 (Crash Cymbal 1)
  // Pad  5: CP  (Clap)           → 39 D#1 (Hand Clap)
  // Pad  6: RS  (Rimshot)        → 37 C#1 (Side Stick / Rimshot)
  // Pad  7: CB  (Cowbell)        → 56 G#2 (Cowbell)
  // Pad  8: LT  (Low Tom)        → 41 F1  (Low Floor Tom)
  // Pad  9: MT  (Mid Tom)        → 47 B1  (Low-Mid Tom)
  // Pad 10: HT  (High Tom)       → 50 D2  (High Tom)
  // Pad 11: MA  (Maracas)        → 70 A#3 (Maracas)
  // Pad 12: CL  (Claves)         → 75 D#4 (Claves)
  // Pad 13: HC  (Hi Conga)       → 62 D3  (Mute Hi Conga)
  // Pad 14: MC  (Mid Conga)      → 63 D#3 (Open Hi Conga)
  // Pad 15: LC  (Low Conga)      → 64 E3  (Low Conga)

  struct { uint8_t note; int8_t pad; } defaultMap[16] = {
    {36, 0},  // BD  - Acoustic Bass Drum
    {38, 1},  // SD  - Acoustic Snare
    {42, 2},  // CH  - Closed Hi-Hat
    {46, 3},  // OH  - Open Hi-Hat
    {49, 4},  // CY  - Crash Cymbal 1
    {39, 5},  // CP  - Hand Clap
    {37, 6},  // RS  - Side Stick
    {56, 7},  // CB  - Cowbell
    {41, 8},  // LT  - Low Floor Tom
    {47, 9},  // MT  - Low-Mid Tom
    {50, 10}, // HT  - High Tom
    {70, 11}, // MA  - Maracas
    {75, 12}, // CL  - Claves
    {62, 13}, // HC  - Mute Hi Conga
    {63, 14}, // MC  - Open Hi Conga
    {64, 15}  // LC  - Low Conga
  };

  for (int i = 0; i < 16; i++) {
    noteMappings[i].note    = defaultMap[i].note;
    noteMappings[i].pad     = defaultMap[i].pad;
    noteMappings[i].enabled = true;
  }
  mappingCount = 16;

  // Notas alternativas GM (alias → mismo pad)
  noteMappings[mappingCount++] = {35, 0, true};  // Acoustic Bass Drum 2 → BD
  noteMappings[mappingCount++] = {40, 1, true};  // Electric Snare       → SD
  noteMappings[mappingCount++] = {44, 2, true};  // Pedal Hi-Hat         → CH
  noteMappings[mappingCount++] = {51, 4, true};  // Ride Cymbal 1        → CY
  noteMappings[mappingCount++] = {57, 4, true};  // Crash Cymbal 2       → CY
  noteMappings[mappingCount++] = {59, 4, true};  // Ride Cymbal 2        → CY
  noteMappings[mappingCount++] = {43, 8, true};  // High Floor Tom       → LT
  noteMappings[mappingCount++] = {45, 9, true};  // Low Tom              → MT
}

void MIDIController::saveMappings() {
  Preferences prefs;
  if (!prefs.begin("midi_map", false)) {
    return;
  }
  prefs.putInt("count", mappingCount);
  for (int i = 0; i < mappingCount; i++) {
    char keyNote[12], keyPad[12], keyEn[12];
    snprintf(keyNote, sizeof(keyNote), "n%d", i);
    snprintf(keyPad,  sizeof(keyPad),  "p%d", i);
    snprintf(keyEn,   sizeof(keyEn),   "e%d", i);
    prefs.putUChar(keyNote, noteMappings[i].note);
    prefs.putChar (keyPad,  noteMappings[i].pad);
    prefs.putBool (keyEn,   noteMappings[i].enabled);
  }
  prefs.end();
}

void MIDIController::loadMappings() {
  Preferences prefs;
  // Open read/write so Preferences creates the namespace on first boot.
  // Opening a namespace that does not exist in read-only mode makes the ESP-IDF
  // print nvs_open NOT_FOUND even though using the defaults is expected.
  if (!prefs.begin("midi_map", false)) {
    return;
  }
  int count = prefs.getInt("count", -1);

  if (count <= 0 || count > MAX_MIDI_MAPPINGS) {
    prefs.end();
    return;  // Mantener el GM por defecto ya cargado
  }

  mappingCount = 0;
  for (int i = 0; i < count; i++) {
    char keyNote[12], keyPad[12], keyEn[12];
    snprintf(keyNote, sizeof(keyNote), "n%d", i);
    snprintf(keyPad,  sizeof(keyPad),  "p%d", i);
    snprintf(keyEn,   sizeof(keyEn),   "e%d", i);
    uint8_t note = prefs.getUChar(keyNote, 0);
    int8_t  pad  = prefs.getChar (keyPad,  -1);
    // Validar por entrada: una NVS corrupta podia devolver pads fuera de
    // 0-15 que luego indexan arrays de pad aguas abajo.
    if (note > 127 || pad < -1 || pad > 15) {
      continue;  // descartar entrada corrupta
    }
    noteMappings[mappingCount].note    = note;
    noteMappings[mappingCount].pad     = pad;
    noteMappings[mappingCount].enabled = prefs.getBool(keyEn, false);
    mappingCount++;
  }
  prefs.end();
}

const MIDINoteMapping* MIDIController::getAllMappings(int& count) const {
  count = mappingCount;
  return noteMappings;
}
