TARGET = Aurora

DIAGNOSTIC ?= 0

ifeq ($(DIAGNOSTIC),1)
CPP_SOURCES = diagnostic.cpp
else
CPP_SOURCES = \
main.cpp \
src/aurora_engine.cpp
endif

OPT = -O3
APP_TYPE ?= BOOT_SRAM

# Reutiliza libDaisy 8.1 ya presente en este workspace. Ambas rutas se pueden
# sustituir desde la linea de comandos si el proyecto se mueve a otro repo.
LIBDAISY_DIR ?= ../RedMaster_DaisySeed64MB/DaisySeed/libdaisy
DAISYSP_DIR ?= ../RedMaster_DaisySeed64MB/DaisySeed/DaisySP
DAISYSP_LGPL_DIR ?= ../RedMaster_DaisySeed64MB/DaisySeed/DaisySP/DaisySP-LGPL

C_INCLUDES += -I.
C_INCLUDES += -I./src
C_INCLUDES += -I$(DAISYSP_DIR)/Source
C_INCLUDES += -I$(DAISYSP_LGPL_DIR)/Source/Effects

LIBS += -ldaisysp-lgpl -ldaisysp
LIBDIR += -L$(DAISYSP_DIR)/build
LIBDIR += -L$(DAISYSP_LGPL_DIR)/build

LDFLAGS += -Wl,--no-warn-rwx-segments

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
