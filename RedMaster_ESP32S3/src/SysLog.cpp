#include "SysLog.h"

static bool _logReady = false;
static unsigned long _bootMs = 0;
// Serializa el acceso a LittleFS: syslog() se llama desde varias tareas
// (boot/loopTask, systemTask, AsyncTCP) y LittleFS no es reentrante; ademas
// la rotacion (size+rename) era una carrera entre llamadores concurrentes.
// syslogPanic NO toma el mutex: corre en el shutdown handler y no debe bloquear.
static SemaphoreHandle_t _logMutex = nullptr;

void syslogBegin() {
    _bootMs = millis();
    if (!_logMutex) _logMutex = xSemaphoreCreateMutex();
    _logReady = true;
    // Write boot marker
    syslog("BOOT", "=== RED808 boot at millis=%lu ===", _bootMs);
}

void syslog(const char* tag, const char* fmt, ...) {
    if (!_logReady) return;

#if !SYSLOG_RUNTIME_ENABLED
    // Only allow BOOT tags through when runtime logging is disabled
    if (tag[0] != 'B' || tag[1] != 'O') return;
#endif

    // Rate-limit: buffer line, flush to file
    char line[256];
    int offset = 0;

    // Timestamp: seconds since boot
    unsigned long elapsedMs = millis() - _bootMs;
    unsigned long sec = elapsedMs / 1000;
    unsigned long ms  = elapsedMs % 1000;
    offset = snprintf(line, sizeof(line), "[%lu.%03lu][%s] ", sec, ms, tag);
    // snprintf/vsnprintf devuelven los chars que SE HABRIAN escrito (sin contar
    // el nul): en truncamiento `offset` puede exceder el buffer. Sin acotar,
    // `f.write(line, offset)` leeria fuera del array de 256 bytes (OOB read).
    if (offset < 0) offset = 0;
    if (offset > (int)sizeof(line) - 1) offset = (int)sizeof(line) - 1;

    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(line + offset, sizeof(line) - offset, fmt, args);
    va_end(args);
    if (wrote > 0) offset += wrote;
    if (offset > (int)sizeof(line) - 1) offset = (int)sizeof(line) - 1;

    // Ensure newline
    if (offset < (int)sizeof(line) - 1) {
        line[offset++] = '\n';
        line[offset] = '\0';
    }

    // Also echo to Serial
    Serial.print(line);

    // FS bajo mutex (rotacion + append atomicos respecto a otras tareas).
    // Timeout corto: si esta contendido, perder una linea es preferible a
    // bloquear la tarea llamante.
    if (_logMutex && xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    // Check rotation before writing
    File f = LittleFS.open(SYSLOG_PATH, "r");
    if (f) {
        size_t sz = f.size();
        f.close();
        if (sz > SYSLOG_MAX_SIZE) {
            LittleFS.remove(SYSLOG_OLD_PATH);
            LittleFS.rename(SYSLOG_PATH, SYSLOG_OLD_PATH);
        }
    }

    // Append to log
    f = LittleFS.open(SYSLOG_PATH, "a");
    if (f) {
        f.write((const uint8_t*)line, offset);
        f.close();
    }

    if (_logMutex) xSemaphoreGive(_logMutex);
}

size_t syslogSize() {
    File f = LittleFS.open(SYSLOG_PATH, "r");
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

void syslogPanic(const char* msg) {
    // Minimal write for crash/shutdown handler — no heap, no format
    File f = LittleFS.open(SYSLOG_PATH, "a");
    if (f) {
        unsigned long sec = (millis() - _bootMs) / 1000;
        unsigned long ms  = (millis() - _bootMs) % 1000;
        char hdr[32];
        int hlen = snprintf(hdr, sizeof(hdr), "[%lu.%03lu][PANIC] ", sec, ms);
        f.write((const uint8_t*)hdr, hlen);
        f.write((const uint8_t*)msg, strlen(msg));
        f.write((const uint8_t*)"\n", 1);
        f.close();
    }
}

void syslogClear() {
    LittleFS.remove(SYSLOG_PATH);
    LittleFS.remove(SYSLOG_OLD_PATH);
}
