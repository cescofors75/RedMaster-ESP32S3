#include "PatternBank.h"

#include <cstring>

namespace {
enum Track : uint8_t {
  BD = 0, SD, CH, OH, CY, CP, RS, CB, LT, MT, HT, MA, CL, HC, MC, LC
};

enum Engine : int8_t {
  SMP = -1, E808 = 0, E909, E505, E303, EWT, ESH, EFM, EPHYS, ENOISE
};

struct MetaSeed {
  const char* name;
  const char* genre;
  const char* kit;
  uint16_t bpm;
  uint8_t swing;   // 0 = straight; amount is applied around a constant two-step duration
  uint8_t timing;
  uint8_t velocity;
};

constexpr MetaSeed META[BUILTIN_PATTERN_COUNT] = {
  {"Boom Bap 90s",          "Hip-Hop",          "808/909 + FM",   92, 22, 4, 10},
  {"Detroit Techno",        "Techno",           "909 + FM",      132,  4, 1,  3},
  {"Jungle/Amen 90s",       "Jungle",           "909/505 + WT",  168,  0, 1,  5},
  {"Chicago House",         "House",            "909 + 303",     122, 10, 2,  5},
  {"Synthpop Linn 80s",     "Synthpop",         "505 + WT/FM",   112,  0, 2,  5},
  {"New Wave/Gated Drums",  "New Wave",         "505 + SH-101",  118,  0, 1,  4},
  {"Italo Disco",           "Italo Disco",      "505 + SH/FM",   122,  8, 1,  4},
  {"Electro/Freestyle 80s", "Electro",          "808 + FM",      118, 16, 2,  6},
  {"Acid House 1988",       "Acid House",       "909 + 303",     124, 12, 1,  3},
  {"Miami Bass",            "Miami Bass",       "808 + WT Sub",  126,  4, 1,  5},
  {"New Jack Swing",        "New Jack Swing",   "Hybrid + FM",   104, 28, 3,  8},
  {"Classic House 90s",     "Classic House",    "909 + WT",      124, 12, 2,  5},
  {"UK Garage 90s",         "UK Garage",        "Hybrid + SH",   132, 30, 3,  7},
  {"Trip-Hop",              "Trip-Hop",         "808/505 + FM",   84, 24, 4, 10},
  {"Rave/Breakbeat",        "Rave",             "Hybrid + 303",  142,  0, 1,  5},
  {"Industrial/EBM",        "Industrial",       "909/505 + FM",  128,  0, 1,  3},
};

// The factory bank is self-contained: it does not depend on an SD kit being present.
// One melodic/texture track is reserved per pattern; all other active tracks use
// generated drum engines. Engine changes are applied at pattern boundaries.
constexpr int8_t ENGINE_PROFILE[BUILTIN_PATTERN_COUNT][MAX_TRACKS] = {
  {E808,E909,E505,E505,E909,E909,E808,E808,E808,E808,E808,E505,E505,E505,E505,EFM},
  {E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,EFM},
  {E909,E909,E505,E909,E909,E909,E909,E505,E505,E505,E505,E505,E505,E505,ENOISE,EWT},
  {E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E303},
  {E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,EFM,EWT},
  {E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,ESH},
  {E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,E505,ESH,EFM},
  {E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,EFM},
  {E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E303},
  {E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,E808,EWT},
  {E808,E909,E505,E505,E909,E909,E808,E808,E808,E808,E808,E505,E505,E505,E505,EFM},
  {E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,E909,EWT},
  {E808,E909,E505,E909,E909,E909,E808,E808,E505,E505,E505,E505,E505,E505,E505,ESH},
  {E808,E909,E505,E505,E909,E909,E808,E808,E808,E808,E808,E505,E505,E505,E505,EFM},
  {E808,E909,E505,E909,E909,E909,E808,E808,E808,E808,E808,E505,E505,E505,ENOISE,E303},
  {E909,E909,E505,E909,E909,E909,E909,E909,E505,E505,E505,E505,E505,E505,ENOISE,EFM},
};

// Preset IDs match the Daisy factory presets. Only engines present in a profile
// are sent, so switching patterns remains lightweight.
constexpr uint8_t PRESET_PROFILE[BUILTIN_PATTERN_COUNT][BUILTIN_ENGINE_COUNT] = {
  {1,0,3,0,0,0,0,0,0}, {2,1,0,0,0,0,3,0,0},
  {0,1,2,0,3,0,0,0,0}, {0,2,0,0,0,0,0,0,0},
  {0,0,1,0,0,0,1,0,0}, {0,0,1,0,0,3,0,0,0},
  {0,0,0,0,0,2,1,0,0}, {2,0,0,0,0,0,0,0,0},
  {0,2,0,1,0,0,0,0,0}, {1,0,0,0,3,0,0,0,0},
  {1,0,2,0,0,0,0,0,0}, {0,2,0,0,2,0,0,0,0},
  {0,2,2,0,0,0,0,0,0}, {1,0,3,0,0,0,0,0,0},
  {2,1,2,1,0,0,0,0,0}, {0,3,2,0,0,0,3,0,0},
};

void setMeta(Sequencer& seq, int pattern) {
  PatternMetadata meta{};
  strncpy(meta.name, META[pattern].name, sizeof(meta.name) - 1);
  strncpy(meta.genre, META[pattern].genre, sizeof(meta.genre) - 1);
  strncpy(meta.kit, META[pattern].kit, sizeof(meta.kit) - 1);
  meta.recommendedBpm = META[pattern].bpm;
  meta.swing = META[pattern].swing;
  meta.humanizeTimingMs = META[pattern].timing;
  meta.humanizeVelocity = META[pattern].velocity;
  seq.setPatternMetadata(pattern, meta);
}

void hit(Sequencer& seq, uint8_t track, uint8_t step, uint8_t velocity = 112,
         uint8_t probability = 100, uint8_t ratchet = 1, uint8_t noteLen = 1) {
  seq.setStep(track, step, true, velocity);
  seq.setStepProbability(track, step, probability);
  seq.setStepRatchet(track, step, ratchet);
  seq.setStepNoteLen(track, step, noteLen);
}

void hits(Sequencer& seq, uint8_t track, const uint8_t* steps, size_t count,
          uint8_t velocity = 108) {
  for (size_t i = 0; i < count; ++i) {
    int shaped = velocity + ((i % 4 == 0) ? 9 : ((i & 1) ? -7 : 0));
    if (shaped < 1) shaped = 1;
    if (shaped > 127) shaped = 127;
    hit(seq, track, steps[i], (uint8_t)shaped);
  }
}

template <size_t N>
void hits(Sequencer& seq, uint8_t track, const uint8_t (&steps)[N], uint8_t velocity = 108) {
  hits(seq, track, steps, N, velocity);
}

void melodicHit(Sequencer& seq, uint8_t track, uint8_t step, uint8_t note,
                uint8_t velocity, uint8_t flags = 0, uint8_t probability = 100,
                uint8_t ratchet = 1, uint8_t noteLen = 2) {
  hit(seq, track, step, velocity, probability, ratchet, noteLen);
  seq.setStepNote(track, step, note);
  seq.clearStepNoteVoices(track, step);
  seq.setStepNoteVoice(track, step, 0, note);
  seq.setStepFlags(track, step, flags);
}

void chordHit(Sequencer& seq, uint8_t track, uint8_t step,
              uint8_t root, uint8_t third, uint8_t fifth,
              uint8_t velocity, uint8_t probability = 100, uint8_t noteLen = 2) {
  hit(seq, track, step, velocity, probability, 1, noteLen);
  seq.setStepNote(track, step, root);
  seq.clearStepNoteVoices(track, step);
  seq.setStepNoteVoice(track, step, 0, root);
  seq.setStepNoteVoice(track, step, 1, third);
  seq.setStepNoteVoice(track, step, 2, fifth);
}

void fourFloor(Sequencer& seq, uint8_t velocity = 122) {
  const uint8_t kick[] = {0, 4, 8, 12, 16, 20, 24, 28};
  hits(seq, BD, kick, velocity);
}

void offbeatHats(Sequencer& seq, uint8_t velocity = 96) {
  const uint8_t hats[] = {2, 6, 10, 14, 18, 22, 26, 30};
  hits(seq, OH, hats, velocity);
}

void addHouseClaps(Sequencer& seq, uint8_t velocity = 116) {
  const uint8_t claps[] = {4, 12, 20, 28};
  hits(seq, CP, claps, velocity);
}

void filterMotion(Sequencer& seq, uint8_t track, uint16_t low, uint16_t high,
                  uint8_t send = 18) {
  seq.setStepCutoffLock(track, 0, true, low);
  seq.setStepCutoffLock(track, 16, true, high);
  seq.setStepReverbSendLock(track, 28, true, send);
}

void buildPattern(Sequencer& seq, int p) {
  seq.selectPattern(p);
  seq.clearPattern(p);
  setMeta(seq, p);

  switch (p) {
    case 0: { // Boom Bap: laid-back pocket, ghosts and an answering FM bass phrase
      const uint8_t k[] = {0, 3, 7, 10, 15, 16, 19, 23, 26, 30};
      const uint8_t s[] = {4, 12, 20, 28};
      const uint8_t h[] = {0, 3, 6, 9, 12, 15, 16, 19, 22, 25, 28, 31};
      hits(seq, BD, k, 111); hits(seq, SD, s, 120); hits(seq, CH, h, 72);
      hit(seq, SD, 11, 42, 48); hit(seq, SD, 14, 50, 62);
      hit(seq, SD, 27, 46, 58); hit(seq, OH, 30, 78, 74);
      melodicHit(seq, LC, 0, 36, 104); melodicHit(seq, LC, 7, 36, 86, 0, 82);
      melodicHit(seq, LC, 10, 39, 98); melodicHit(seq, LC, 16, 36, 108);
      melodicHit(seq, LC, 23, 43, 92); melodicHit(seq, LC, 26, 39, 101);
      filterMotion(seq, CH, 4200, 6900, 24);
      break;
    }
    case 1: { // Detroit: relentless foundation, syncopated rim language, growl stabs
      fourFloor(seq, 126); offbeatHats(seq, 101); addHouseClaps(seq, 111);
      const uint8_t ch[] = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31};
      hits(seq, CH, ch, 64);
      hit(seq, RS, 3, 72); hit(seq, RS, 11, 80); hit(seq, RS, 18, 68, 72);
      hit(seq, RS, 27, 88); hit(seq, HT, 30, 92, 100, 2); hit(seq, MT, 31, 112);
      melodicHit(seq, LC, 3, 48, 96, 0, 78, 1, 4);
      melodicHit(seq, LC, 11, 55, 110, 1, 100, 1, 4);
      melodicHit(seq, LC, 19, 51, 94, 0, 74, 1, 4);
      melodicHit(seq, LC, 27, 58, 118, 1, 100, 2, 4);
      filterMotion(seq, CH, 7600, 12800, 14);
      break;
    }
    case 2: { // Jungle: chopped break logic, pitched WT sub and a real end-of-bar rush
      const uint8_t k[] = {0, 6, 10, 16, 19, 24, 27, 30};
      const uint8_t s[] = {4, 12, 20, 25, 28};
      const uint8_t h[] = {0,2,3,6,8,10,11,14,16,18,19,22,24,26,27,29,30,31};
      hits(seq, BD, k, 121); hits(seq, SD, s, 124); hits(seq, CH, h, 78);
      hit(seq, SD, 15, 62, 58, 2); hit(seq, SD, 23, 58, 52, 2);
      hit(seq, SD, 30, 82, 86, 3); hit(seq, OH, 7, 88, 72);
      hit(seq, MC, 14, 68, 48); hit(seq, MC, 31, 96, 70, 4);
      melodicHit(seq, LC, 0, 36, 118, 1, 100, 1, 2);
      melodicHit(seq, LC, 6, 36, 102); melodicHit(seq, LC, 10, 43, 110);
      melodicHit(seq, LC, 16, 39, 116); melodicHit(seq, LC, 19, 46, 96, 0, 82);
      melodicHit(seq, LC, 27, 41, 108); filterMotion(seq, CH, 5800, 11200, 18);
      break;
    }
    case 3: { // Chicago: jack groove with restrained, sliding acid punctuation
      fourFloor(seq, 123); offbeatHats(seq, 102); addHouseClaps(seq, 116);
      const uint8_t sh[] = {3,7,11,15,19,23,27,31}; hits(seq, MA, sh, 62);
      hit(seq, CH, 13, 68, 60); hit(seq, CH, 29, 72, 72, 2);
      hit(seq, CB, 11, 74, 66); hit(seq, LT, 30, 82, 64); hit(seq, MT, 31, 98, 76);
      melodicHit(seq, LC, 0, 36, 112, 1); melodicHit(seq, LC, 3, 43, 88, 2, 86);
      melodicHit(seq, LC, 6, 39, 99); melodicHit(seq, LC, 10, 46, 105, 1);
      melodicHit(seq, LC, 13, 43, 92, 2); melodicHit(seq, LC, 16, 36, 116, 1);
      melodicHit(seq, LC, 22, 48, 108); melodicHit(seq, LC, 27, 41, 96, 2, 88);
      break;
    }
    case 4: { // Synthpop: wide backbeat, gated tom answer and harmonic movement
      const uint8_t k[] = {0, 6, 8, 14, 16, 22, 24, 30};
      const uint8_t s[] = {4, 12, 20, 28};
      const uint8_t h[] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30};
      hits(seq, BD, k, 116); hits(seq, SD, s, 126); hits(seq, CH, h, 76);
      hit(seq, CP, 12, 84); hit(seq, CP, 28, 94);
      hit(seq, LT, 29, 82); hit(seq, MT, 30, 96); hit(seq, HT, 31, 112);
      melodicHit(seq, MC, 2, 72, 86, 0, 72, 1, 4);
      melodicHit(seq, MC, 10, 76, 92, 0, 82, 1, 4);
      melodicHit(seq, MC, 18, 79, 88, 0, 76, 1, 4);
      melodicHit(seq, MC, 26, 76, 98, 0, 90, 1, 4);
      chordHit(seq, LC, 0, 48, 52, 55, 94, 100, 2);
      chordHit(seq, LC, 8, 43, 47, 50, 86, 100, 2);
      chordHit(seq, LC, 16, 45, 48, 52, 92, 100, 2);
      chordHit(seq, LC, 24, 41, 45, 48, 98, 100, 2);
      seq.setStepReverbSendLock(SD, 4, true, 38); seq.setStepReverbSendLock(SD, 28, true, 54);
      break;
    }
    case 5: { // New Wave: dry mechanical drive versus oversized gated snare
      const uint8_t k[] = {0, 5, 8, 11, 16, 21, 24, 27};
      const uint8_t s[] = {4, 12, 20, 28};
      hits(seq, BD, k, 121); hits(seq, SD, s, 127); offbeatHats(seq, 86);
      hit(seq, CH, 7, 64, 58); hit(seq, CH, 15, 72, 68);
      hit(seq, LT, 13, 78); hit(seq, MT, 14, 92); hit(seq, HT, 15, 108);
      hit(seq, LT, 29, 92); hit(seq, MT, 30, 106); hit(seq, HT, 31, 122, 100, 2);
      melodicHit(seq, LC, 0, 36, 112); melodicHit(seq, LC, 3, 36, 84, 0, 78);
      melodicHit(seq, LC, 6, 43, 104); melodicHit(seq, LC, 8, 41, 98);
      melodicHit(seq, LC, 11, 48, 108, 2); melodicHit(seq, LC, 16, 36, 114);
      melodicHit(seq, LC, 22, 46, 101); melodicHit(seq, LC, 27, 43, 106, 2);
      seq.setStepReverbSendLock(SD, 4, true, 62); seq.setStepReverbSendLock(SD, 20, true, 70);
      break;
    }
    case 6: { // Italo: buoyant four-on-floor with a call-and-response arpeggio
      fourFloor(seq, 121); offbeatHats(seq, 94); addHouseClaps(seq, 111);
      const uint8_t ch[] = {0,4,8,12,16,20,24,28}; hits(seq, CH, ch, 66);
      hit(seq, CB, 3, 78); hit(seq, CB, 11, 82); hit(seq, CB, 19, 74, 72); hit(seq, CB, 27, 88);
      hit(seq, CY, 0, 92); hit(seq, CY, 31, 98, 62);
      melodicHit(seq, MC, 0, 48, 96); melodicHit(seq, MC, 2, 55, 82);
      melodicHit(seq, MC, 4, 60, 90); melodicHit(seq, MC, 6, 64, 84);
      melodicHit(seq, MC, 8, 55, 92); melodicHit(seq, MC, 10, 60, 86);
      melodicHit(seq, MC, 12, 67, 98); melodicHit(seq, MC, 14, 64, 88);
      melodicHit(seq, LC, 16, 36, 108); melodicHit(seq, LC, 20, 43, 96);
      melodicHit(seq, LC, 24, 45, 104); melodicHit(seq, LC, 28, 41, 112);
      break;
    }
    case 7: { // Electro: syncopated 808 body, sparse hats and low FM answer
      const uint8_t k[] = {0,3,7,10,14,16,19,23,26,31};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {2,6,10,14,18,22,26,30};
      hits(seq, BD, k, 123); hits(seq, SD, s, 119); hits(seq, CH, h, 76);
      hit(seq, CP, 4, 90); hit(seq, CP, 20, 84); hit(seq, CB, 11, 76, 72);
      hit(seq, SD, 30, 78, 80, 2); hit(seq, SD, 31, 94, 92, 3);
      melodicHit(seq, LC, 0, 36, 116); melodicHit(seq, LC, 3, 43, 92);
      melodicHit(seq, LC, 7, 36, 103); melodicHit(seq, LC, 10, 46, 110);
      melodicHit(seq, LC, 16, 39, 114); melodicHit(seq, LC, 19, 48, 96, 0, 86);
      melodicHit(seq, LC, 23, 43, 108); melodicHit(seq, LC, 27, 41, 101);
      break;
    }
    case 8: { // Acid: tough 909 chassis and a two-bar squelch phrase with slides
      fourFloor(seq, 125); offbeatHats(seq, 99); addHouseClaps(seq, 112);
      const uint8_t ch[] = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31};
      hits(seq, CH, ch, 61); hit(seq, RS, 6, 74, 62); hit(seq, RS, 22, 80, 72);
      hit(seq, OH, 31, 94, 82); hit(seq, CY, 16, 86, 64);
      const uint8_t acidSteps[] = {0,3,6,8,10,13,15,16,19,22,24,27,29,31};
      const uint8_t acidNotes[] = {36,43,39,46,48,43,41,36,39,48,46,43,50,41};
      for (size_t i = 0; i < sizeof(acidSteps); ++i) {
        uint8_t flags = ((i % 4) == 0 ? 1 : 0) | ((i == 5 || i == 11) ? 2 : 0);
        melodicHit(seq, LC, acidSteps[i], acidNotes[i], (i % 4 == 0) ? 121 : 94,
                   flags, (i == 12) ? 72 : 100);
      }
      break;
    }
    case 9: { // Miami Bass: sub-heavy 808 syncopation and deliberate space
      const uint8_t k[] = {0,3,7,10,14,16,19,23,26,30};
      const uint8_t s[] = {4,12,20,28}; const uint8_t h[] = {2,6,10,14,18,22,26,30};
      hits(seq, BD, k, 126); hits(seq, SD, s, 119); hits(seq, CH, h, 78);
      hit(seq, CP, 12, 90); hit(seq, CP, 28, 98); hit(seq, SD, 31, 88, 84, 3);
      hit(seq, OH, 15, 82, 68); hit(seq, OH, 31, 90, 78);
      melodicHit(seq, LC, 0, 36, 122, 1, 100, 1, 2);
      melodicHit(seq, LC, 7, 36, 106); melodicHit(seq, LC, 10, 39, 112);
      melodicHit(seq, LC, 16, 34, 123, 1); melodicHit(seq, LC, 23, 41, 108);
      melodicHit(seq, LC, 26, 39, 114); melodicHit(seq, LC, 30, 36, 98, 0, 82);
      seq.setStepVolumeLock(BD, 16, true, 118);
      break;
    }
    case 10: { // New Jack: pronounced swing, conversational kicks and ghosted snare pickup
      const uint8_t k[] = {0,3,7,10,16,19,22,27,30};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30};
      hits(seq, BD, k, 115); hits(seq, SD, s, 122); hits(seq, CH, h, 69);
      hit(seq, SD, 11, 48, 56); hit(seq, SD, 15, 42, 46);
      hit(seq, SD, 27, 54, 66); hit(seq, CP, 28, 86); hit(seq, OH, 31, 90, 82);
      melodicHit(seq, LC, 0, 36, 110); melodicHit(seq, LC, 3, 43, 82, 0, 78);
      melodicHit(seq, LC, 7, 46, 98); melodicHit(seq, LC, 10, 41, 92);
      melodicHit(seq, LC, 16, 36, 112); melodicHit(seq, LC, 19, 48, 88, 0, 76);
      melodicHit(seq, LC, 22, 43, 104); melodicHit(seq, LC, 27, 39, 96);
      break;
    }
    case 11: { // Classic House: pumping 909, shuffled shaker and organ-like WT stabs
      fourFloor(seq, 123); offbeatHats(seq, 101); addHouseClaps(seq, 117);
      const uint8_t sh[] = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31};
      hits(seq, MA, sh, 58); hit(seq, CH, 15, 70, 64); hit(seq, OH, 31, 96, 84);
      hit(seq, LT, 29, 78, 58); hit(seq, MT, 30, 92, 70); hit(seq, HT, 31, 108, 84);
      chordHit(seq, LC, 2, 48, 51, 55, 92, 100, 4);
      chordHit(seq, LC, 6, 43, 46, 50, 86, 88, 4);
      chordHit(seq, LC, 10, 45, 48, 52, 96, 100, 4);
      chordHit(seq, LC, 14, 41, 44, 48, 89, 92, 4);
      chordHit(seq, LC, 18, 48, 51, 55, 98, 100, 4);
      chordHit(seq, LC, 26, 45, 48, 52, 94, 100, 4);
      break;
    }
    case 12: { // UKG: two-step kick placement, skipping hats and SH bass responses
      const uint8_t k[] = {0,7,10,16,22,26,31}; const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {0,2,5,7,10,13,15,16,18,21,23,26,29,31};
      hits(seq, BD, k, 121); hits(seq, CP, s, 117); hits(seq, CH, h, 73);
      hit(seq, SD, 11, 46, 54); hit(seq, SD, 19, 52, 62); hit(seq, OH, 15, 84, 74);
      hit(seq, CH, 29, 68, 70, 2); hit(seq, CH, 31, 80, 86, 3);
      melodicHit(seq, LC, 0, 36, 112); melodicHit(seq, LC, 5, 43, 88, 0, 80);
      melodicHit(seq, LC, 7, 46, 101); melodicHit(seq, LC, 10, 39, 106);
      melodicHit(seq, LC, 16, 36, 114); melodicHit(seq, LC, 21, 48, 92, 0, 78);
      melodicHit(seq, LC, 23, 43, 104); melodicHit(seq, LC, 27, 41, 99, 2);
      break;
    }
    case 13: { // Trip-Hop: slow broken pocket, dusty hat omissions and noir FM bass
      const uint8_t k[] = {0,6,10,15,16,23,27}; const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {0,3,6,9,12,15,16,19,22,25,28,31};
      hits(seq, BD, k, 108); hits(seq, SD, s, 115); hits(seq, CH, h, 60);
      hit(seq, SD, 11, 38, 44); hit(seq, SD, 27, 44, 54); hit(seq, OH, 14, 70, 58);
      hit(seq, RS, 7, 48, 42); hit(seq, CY, 31, 72, 48);
      melodicHit(seq, LC, 0, 36, 106, 1, 100, 1, 2);
      melodicHit(seq, LC, 6, 39, 82, 0, 78); melodicHit(seq, LC, 10, 43, 94);
      melodicHit(seq, LC, 16, 34, 108, 1); melodicHit(seq, LC, 23, 41, 88);
      melodicHit(seq, LC, 27, 39, 96, 0, 84);
      filterMotion(seq, CH, 2600, 4700, 36);
      break;
    }
    case 14: { // Rave: breakbeat body, noise riser punctuation and an acid hook
      const uint8_t k[] = {0,6,10,16,19,24,27,30}; const uint8_t s[] = {4,12,20,25,28};
      const uint8_t h[] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,29,30,31};
      hits(seq, BD, k, 124); hits(seq, SD, s, 125); hits(seq, CH, h, 79);
      hit(seq, CY, 0, 111); hit(seq, CY, 16, 104); hit(seq, SD, 30, 84, 86, 2);
      hit(seq, SD, 31, 98, 94, 4); hit(seq, MC, 14, 76, 62, 2); hit(seq, MC, 30, 104, 84, 4);
      melodicHit(seq, LC, 0, 36, 118, 1); melodicHit(seq, LC, 3, 43, 96, 2);
      melodicHit(seq, LC, 6, 48, 108); melodicHit(seq, LC, 10, 46, 102);
      melodicHit(seq, LC, 13, 50, 114, 1); melodicHit(seq, LC, 16, 36, 121, 1);
      melodicHit(seq, LC, 22, 51, 110); melodicHit(seq, LC, 27, 43, 103, 2);
      filterMotion(seq, CH, 7000, 13800, 22);
      break;
    }
    case 15: { // Industrial: punishing transient contrast, metallic syncopation and FM growl
      fourFloor(seq, 127);
      const uint8_t s[] = {4,12,20,28}; const uint8_t h[] = {2,6,10,14,18,22,26,30};
      hits(seq, SD, s, 124); hits(seq, CH, h, 88);
      const uint8_t metal[] = {3,7,11,15,19,23,27,31}; hits(seq, RS, metal, 84);
      hit(seq, CY, 0, 98); hit(seq, CY, 16, 116); hit(seq, LT, 13, 88, 72);
      hit(seq, LT, 29, 102); hit(seq, MT, 30, 114); hit(seq, HT, 31, 127, 100, 3);
      hit(seq, MC, 6, 82, 64); hit(seq, MC, 14, 94, 72, 2);
      hit(seq, MC, 22, 88, 68); hit(seq, MC, 30, 112, 86, 4);
      melodicHit(seq, LC, 0, 36, 122, 1, 100, 1, 2);
      melodicHit(seq, LC, 4, 36, 90); melodicHit(seq, LC, 7, 37, 108);
      melodicHit(seq, LC, 11, 43, 102); melodicHit(seq, LC, 16, 34, 124, 1);
      melodicHit(seq, LC, 20, 41, 96); melodicHit(seq, LC, 23, 37, 112);
      melodicHit(seq, LC, 27, 46, 118, 1, 100, 2);
      seq.setStepReverbSendLock(SD, 28, true, 28);
      break;
    }
  }
}
} // namespace

void initializeProfessionalPatternBank(Sequencer& sequencer) {
  sequencer.setPatternLength(32);
  for (int pattern = 0; pattern < BUILTIN_PATTERN_COUNT; ++pattern) {
    buildPattern(sequencer, pattern);
  }
  sequencer.selectPattern(0);
  sequencer.setHumanize(META[0].timing, META[0].velocity);
}

bool getBuiltinPatternSoundProfile(int pattern, BuiltinPatternSoundProfile& out) {
  if (pattern < 0 || pattern >= BUILTIN_PATTERN_COUNT) return false;
  memcpy(out.engines, ENGINE_PROFILE[pattern], sizeof(out.engines));
  memcpy(out.presets, PRESET_PROFILE[pattern], sizeof(out.presets));
  return true;
}
