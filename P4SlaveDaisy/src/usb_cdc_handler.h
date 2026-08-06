// =============================================================================
// usb_cdc_handler.h — raw USB Host CDC-ACM byte stream for Daisy
// The framed P4 ↔ Master protocol is implemented by master_usb_transport.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// Initialize USB Host Library + CDC ACM driver on USB-OTG port
void usb_cdc_init(void);

// Process USB Host events — call from main loop
void usb_cdc_process(void);

// Check if the Daisy CDC device is connected
bool usb_cdc_connected(void);

// Bytes available in USB CDC receive buffer
int usb_cdc_available(void);

// Read one byte from USB CDC buffer (-1 if empty)
int usb_cdc_read(void);

// Send a framed command to Daisy via USB CDC (reserved for future controls)
size_t usb_cdc_write(const uint8_t* data, size_t len);

// Get diagnostic string for USB CDC state (for UART-DBG)
// Returns: "init=X conn=X dev=X open=X disc=X last_err=X"
const char* usb_cdc_status_str(void);

// Compact counters for visible UI diagnostics.
void usb_cdc_get_diag(int* detected, int* connects, int* disconnects, int* attempts);
