#pragma once

#include "Sequencer.h"

static constexpr uint8_t BUILTIN_PATTERN_COUNT = 16;
static constexpr uint8_t BUILTIN_ENGINE_COUNT = 9;

struct BuiltinPatternSoundProfile {
  int8_t engines[MAX_TRACKS];
  uint8_t presets[BUILTIN_ENGINE_COUNT];
};

void initializeProfessionalPatternBank(Sequencer& sequencer);
bool getBuiltinPatternSoundProfile(int pattern, BuiltinPatternSoundProfile& out);
