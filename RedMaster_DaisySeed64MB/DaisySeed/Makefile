# ══════════════════════════════════════════════════════════════════
#  DrumMachine — Daisy Seed Edition
#  16-Pad Sampler + FX + SPI Slave + Synth Engines (808/909/505/303)
# ══════════════════════════════════════════════════════════════════
#
#  Demo standalone "The Bells" (sin ESP32, sin SD):
#    make DEMO_BELLS=1
#
#  Diagnóstico de audio (440 Hz sine, LED parpadea cada 0.5s):
#    make DEMO_BELLS=1 SINE_TEST=1
# ══════════════════════════════════════════════════════════════════

DEMO_BELLS ?= 0
SINE_TEST  ?= 0

ifeq ($(DEMO_BELLS),1)
TARGET      = DemoBells
CPP_SOURCES = demo_bells.cpp syscalls.cpp
else
TARGET      = DrumMachine
CPP_SOURCES = main.cpp syscalls.cpp
endif

ifeq ($(SINE_TEST),1)
C_DEFS += -DSINE_TEST=1
endif

# Include path for synth headers + DaisySP-LGPL
C_INCLUDES += -I.
C_INCLUDES += -I./DaisySP/DaisySP-LGPL/Source

OPT = -O3

# Optional diagnostics, enabled from command line without replacing libDaisy C_DEFS:
#   make RED808_STARTUP_TONE_TEST=1
#   make RED808_STARTUP_808_SELF_TEST=1
RED808_BOOT_DIAG_MINIMAL ?= 0
RED808_AUDIO_DIAG_MINIMAL ?= 0
RED808_ENABLE_SPI_SLAVE ?= 1
RED808_ENABLE_INIT_FX ?= 1
RED808_STARTUP_TONE_TEST ?= 0
RED808_STARTUP_808_SELF_TEST ?= 0
RED808_STARTUP_TONE_SECONDS ?= 3
RED808_STARTUP_STRESS_REPORT ?= 0
RED808_STARTUP_STRESS_SECONDS ?= 18
RED808_DSP_BLOCK_PROFILE ?= 1
RED808_MEM_AUDIT ?= 0
C_DEFS += -DRED808_BOOT_DIAG_MINIMAL=$(RED808_BOOT_DIAG_MINIMAL)
C_DEFS += -DRED808_AUDIO_DIAG_MINIMAL=$(RED808_AUDIO_DIAG_MINIMAL)
C_DEFS += -DRED808_ENABLE_SPI_SLAVE=$(RED808_ENABLE_SPI_SLAVE)
C_DEFS += -DRED808_ENABLE_INIT_FX=$(RED808_ENABLE_INIT_FX)
C_DEFS += -DRED808_STARTUP_TONE_TEST=$(RED808_STARTUP_TONE_TEST)
C_DEFS += -DRED808_STARTUP_808_SELF_TEST=$(RED808_STARTUP_808_SELF_TEST)
C_DEFS += -DRED808_STARTUP_TONE_SECONDS=$(RED808_STARTUP_TONE_SECONDS)
C_DEFS += -DRED808_STARTUP_STRESS_REPORT=$(RED808_STARTUP_STRESS_REPORT)
C_DEFS += -DRED808_STARTUP_STRESS_SECONDS=$(RED808_STARTUP_STRESS_SECONDS)
C_DEFS += -DRED808_DSP_BLOCK_PROFILE=$(RED808_DSP_BLOCK_PROFILE)
C_DEFS += -DRED808_MEM_AUDIT=$(RED808_MEM_AUDIT)

# ── Boot desde SRAM (requiere Daisy Bootloader) ──
# El STM32H750 solo tiene 128K flash interna, insuficiente.
# BOOT_SRAM: bootloader copia app de QSPI→SRAM (480K disponibles)
APP_TYPE = BOOT_SRAM

# ── Ajusta estas rutas a tu instalación ──
LIBDAISY_DIR = ./libdaisy
DAISYSP_DIR  = ./DaisySP

# ── Linkear DaisySP-LGPL (ReverbSc, Compressor, Fold, etc.) ──
LIBS += -ldaisysp-lgpl
LIBDIR += -L./DaisySP/DaisySP-LGPL/build
LDFLAGS += -Wl,--no-warn-rwx-segments

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
