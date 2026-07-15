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
  uint8_t swing;
  uint8_t timing;
  uint8_t velocity;
};

/* Original factory material: useful musical directions rather than song or
 * artist imitations. Every pattern is a two-bar performance with real negative
 * space, an A/B variation and a restrained melodic role. */
constexpr MetaSeed META[BUILTIN_PATTERN_COUNT] = {
  {"Pulse Bloom",     "Broken Club",    "RED 808 KARZ + SH", 124, 10, 1, 5},
  {"Night Transit",   "Deep Techno",    "Samples + 909",     130,  4, 0, 3},
  {"Glass Garage",    "Future Garage",  "Samples + 505",     132, 26, 2, 6},
  {"Warm Pressure",   "Deep House",     "Samples + 303",     122, 12, 1, 4},
  {"Carbon Breaks",   "Broken Beat",    "Samples + 505",     138,  2, 1, 6},
  {"Neon Motorik",    "Night Drive",    "Samples + SH/WT",   116,  4, 1, 4},
  {"Elastic Electro", "Electro",        "Samples + 808",     120, 14, 1, 5},
  {"Low Gravity",     "Half-Time",      "Samples + SH/WT",   100, 18, 2, 7},
  {"Acid Trace",      "Minimal Acid",   "Samples + 909/303", 126,  8, 0, 3},
  {"Sub Signal",      "Bass Club",      "Samples + 808/SH",  128,  6, 1, 5},
  {"Pocket Chrome",   "Swing Beat",     "Samples + FM/SH",   106, 24, 2, 7},
  {"Organ Dust",      "Raw House",      "Samples + 909/WT",  124, 14, 1, 4},
  {"Night Bus",       "Two-Step",       "Samples + SH/FM",   134, 28, 2, 6},
  {"Afterglow",       "Downtempo",      "Samples + FM/WT",    86, 20, 3, 8},
  {"Peak Relay",      "Modern Breaks",  "Samples + 909/505", 140,  2, 1, 5},
  {"Machine Ritual",  "Warehouse",      "Samples + 909/FM",  128,  4, 0, 3},
};

/* Sample-first contract. At least twelve tracks in every factory pattern use
 * the SD kit; generated machines are accent colours, never the whole kit. */
constexpr int8_t ENGINE_PROFILE[BUILTIN_PATTERN_COUNT][MAX_TRACKS] = {
  {SMP,SMP,SMP,SMP,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EWT},
  {SMP,SMP,SMP,E909,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EFM,ESH},
  {SMP,SMP,SMP,E505,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EWT},
  {SMP,SMP,SMP,E909,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EWT,E303},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,E505,SMP,SMP,SMP,ESH,ENOISE},
  {SMP,SMP,SMP,SMP,E505,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EWT},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,E808,SMP,SMP,SMP,SMP,SMP,SMP,EFM,ESH},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EWT},
  {SMP,SMP,SMP,E909,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,E303},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,E808,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EWT},
  {SMP,SMP,SMP,SMP,E505,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EFM,ESH},
  {SMP,SMP,SMP,E909,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EWT},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,ESH,EFM},
  {SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EFM,EWT},
  {SMP,SMP,SMP,E909,SMP,SMP,SMP,SMP,SMP,SMP,E505,SMP,SMP,SMP,ESH,ENOISE},
  {SMP,SMP,SMP,E909,E909,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,SMP,EFM,ESH},
};

/* Preset order: 808, 909, 505, 303, WT, SH, FM, PHYS, NOISE. */
constexpr uint8_t PRESET_PROFILE[BUILTIN_PATTERN_COUNT][BUILTIN_ENGINE_COUNT] = {
  {1,1,1,0,3,2,1,0,0}, {1,1,1,0,1,2,2,0,0},
  {1,1,2,0,3,2,1,0,0}, {1,2,1,1,2,1,1,0,0},
  {1,1,2,0,1,2,1,0,1}, {1,1,1,0,3,2,1,0,0},
  {1,1,1,0,1,2,2,0,0}, {1,1,1,0,3,1,1,0,0},
  {1,2,1,1,1,1,1,0,0}, {1,1,1,0,2,2,1,0,0},
  {1,1,2,0,1,2,2,0,0}, {1,2,1,0,2,1,1,0,0},
  {1,1,1,0,1,2,2,0,0}, {1,1,1,0,3,1,1,0,0},
  {1,2,2,0,1,2,1,0,2}, {1,2,1,0,1,2,3,0,0},
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

void hit(Sequencer& seq, uint8_t track, uint8_t step, uint8_t velocity = 108,
         uint8_t probability = 100, uint8_t ratchet = 1, uint8_t noteLen = 1) {
  seq.setStep(track, step, true, velocity);
  seq.setStepProbability(track, step, probability);
  seq.setStepRatchet(track, step, ratchet);
  seq.setStepNoteLen(track, step, noteLen);
}

template <size_t N>
void hits(Sequencer& seq, uint8_t track, const uint8_t (&steps)[N],
          uint8_t velocity = 100, int8_t alternate = -7) {
  for (size_t i = 0; i < N; ++i) {
    int shaped = velocity + ((i & 1u) ? alternate : 0) + ((i % 4u == 0u) ? 5 : 0);
    if (shaped < 1) shaped = 1;
    if (shaped > 127) shaped = 127;
    hit(seq, track, steps[i], (uint8_t)shaped);
  }
}

void melodicHit(Sequencer& seq, uint8_t track, uint8_t step, uint8_t note,
                uint8_t velocity, uint8_t flags = 0, uint8_t probability = 100,
                uint8_t ratchet = 1, uint8_t noteLen = 1) {
  hit(seq, track, step, velocity, probability, ratchet, noteLen);
  seq.setStepNote(track, step, note);
  seq.clearStepNoteVoices(track, step);
  seq.setStepNoteVoice(track, step, 0, note);
  seq.setStepFlags(track, step, flags);
}

void chordHit(Sequencer& seq, uint8_t track, uint8_t step,
              uint8_t root, uint8_t third, uint8_t fifth,
              uint8_t velocity, uint8_t probability = 100) {
  hit(seq, track, step, velocity, probability, 1, 1);
  seq.setStepNote(track, step, root);
  seq.clearStepNoteVoices(track, step);
  seq.setStepNoteVoice(track, step, 0, root);
  seq.setStepNoteVoice(track, step, 1, third);
  seq.setStepNoteVoice(track, step, 2, fifth);
}

void fourFloor(Sequencer& seq, uint8_t velocity = 116) {
  const uint8_t steps[] = {0,4,8,12,16,20,24,28};
  hits(seq, BD, steps, velocity, -4);
}

void backbeat(Sequencer& seq, uint8_t velocity = 108) {
  const uint8_t steps[] = {4,12,20,28};
  hits(seq, SD, steps, velocity, -3);
}

void softOffbeats(Sequencer& seq, uint8_t velocity = 66) {
  const uint8_t steps[] = {2,6,10,14,18,22,26,30};
  hits(seq, CH, steps, velocity, -10);
}

void sampleMotion(Sequencer& seq, uint8_t track, uint16_t low, uint16_t high,
                  uint8_t reverbSend = 12) {
  seq.setStepCutoffLock(track, 0, true, low);
  seq.setStepCutoffLock(track, 16, true, high);
  seq.setStepReverbSendLock(track, 28, true, reverbSend);
}

void buildPattern(Sequencer& seq, int p) {
  seq.selectPattern(p);
  seq.clearPattern(p);
  setMeta(seq, p);

  switch (p) {
    case 0: { // Pulse Bloom — broken sample pocket with a single harmonic breath.
      const uint8_t k[] = {0,6,10,16,23,26};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {2,6,9,14,18,22,25,30};
      hits(seq, BD, k, 113); hits(seq, SD, s, 109); hits(seq, CH, h, 59, -12);
      hit(seq, SD, 11, 38, 48); hit(seq, SD, 27, 43, 58);
      hit(seq, OH, 15, 62, 76); hit(seq, OH, 31, 70, 86);
      melodicHit(seq, MC, 0, 29, 76); melodicHit(seq, MC, 10, 36, 62, 0, 86);
      melodicHit(seq, MC, 16, 27, 78); melodicHit(seq, MC, 26, 32, 67);
      chordHit(seq, LC, 0, 53,56,60, 42); chordHit(seq, LC, 16, 49,53,56, 38);
      sampleMotion(seq, CH, 4200, 7600, 10);
      break;
    }
    case 1: { // Night Transit — minimal pressure; FM answers only twice per bar.
      fourFloor(seq, 120); backbeat(seq, 105); softOffbeats(seq, 61);
      const uint8_t hats[] = {1,5,9,13,17,21,25,29}; hits(seq, OH, hats, 54, -8);
      hit(seq, RS, 11, 47, 62); hit(seq, RS, 27, 54, 72);
      melodicHit(seq, MC, 6, 55, 58, 0, 82); melodicHit(seq, MC, 22, 58, 64, 0, 88);
      melodicHit(seq, LC, 0, 29, 72); melodicHit(seq, LC, 10, 32, 61);
      melodicHit(seq, LC, 16, 29, 74); melodicHit(seq, LC, 27, 27, 64);
      sampleMotion(seq, CH, 6800, 11200, 7);
      break;
    }
    case 2: { // Glass Garage — two-step drums, quiet WT air and no kick wall.
      const uint8_t k[] = {0,7,10,16,22,27,30};
      const uint8_t c[] = {4,12,20,28};
      const uint8_t h[] = {1,3,6,9,11,14,17,19,22,25,27,30};
      hits(seq, BD, k, 114); hits(seq, CP, c, 106); hits(seq, CH, h, 55, -11);
      hit(seq, SD, 11, 40, 52); hit(seq, SD, 19, 44, 58);
      hit(seq, OH, 15, 64, 78); hit(seq, OH, 31, 72, 86);
      melodicHit(seq, MC, 0, 29, 72); melodicHit(seq, MC, 7, 36, 61);
      melodicHit(seq, MC, 16, 27, 74); melodicHit(seq, MC, 22, 39, 62, 0, 82);
      chordHit(seq, LC, 0, 53,56,60, 34); chordHit(seq, LC, 16, 51,55,58, 32);
      break;
    }
    case 3: { // Warm Pressure — sample house body with short acid punctuation.
      fourFloor(seq, 117); backbeat(seq, 107); softOffbeats(seq, 67);
      hit(seq, CP, 12, 72); hit(seq, CP, 28, 78);
      const uint8_t sh[] = {3,7,11,15,19,23,27,31}; hits(seq, MA, sh, 45, -8);
      chordHit(seq, MC, 2, 53,56,60, 48); chordHit(seq, MC, 18, 49,53,56, 45);
      melodicHit(seq, LC, 0, 29, 86, 1); melodicHit(seq, LC, 6, 36, 64);
      melodicHit(seq, LC, 13, 32, 71, 2, 88); melodicHit(seq, LC, 16, 29, 88, 1);
      melodicHit(seq, LC, 27, 39, 68, 2, 82);
      break;
    }
    case 4: { // Carbon Breaks — chopped samples; 505 toms only form the turnaround.
      const uint8_t k[] = {0,6,10,16,19,24,27,30};
      const uint8_t s[] = {4,12,20,25,28};
      const uint8_t h[] = {0,3,6,10,14,17,19,22,26,29,31};
      hits(seq, BD, k, 117); hits(seq, SD, s, 112); hits(seq, CH, h, 61, -13);
      hit(seq, SD, 15, 48, 58, 2); hit(seq, OH, 7, 64, 72);
      hit(seq, LT, 29, 62); hit(seq, MT, 30, 70); hit(seq, HT, 31, 82, 100, 2);
      melodicHit(seq, MC, 0, 29, 78); melodicHit(seq, MC, 10, 36, 67);
      melodicHit(seq, MC, 16, 27, 80); melodicHit(seq, MC, 27, 32, 69);
      melodicHit(seq, LC, 15, 72, 35, 0, 58); melodicHit(seq, LC, 31, 79, 42, 0, 72);
      break;
    }
    case 5: { // Neon Motorik — dry samples, restrained bass and two soft chords.
      const uint8_t k[] = {0,5,8,12,16,21,24,28};
      hits(seq, BD, k, 111); backbeat(seq, 110); softOffbeats(seq, 58);
      hit(seq, CP, 12, 65); hit(seq, CP, 28, 72);
      hit(seq, LT, 29, 54); hit(seq, MT, 30, 62); hit(seq, HT, 31, 72);
      melodicHit(seq, MC, 0, 29, 75); melodicHit(seq, MC, 8, 36, 62);
      melodicHit(seq, MC, 16, 32, 72); melodicHit(seq, MC, 24, 27, 66);
      chordHit(seq, LC, 0, 53,56,60, 40); chordHit(seq, LC, 16, 51,55,58, 37);
      seq.setStepReverbSendLock(SD, 28, true, 18);
      break;
    }
    case 6: { // Elastic Electro — syncopated sample chassis and tiny machine details.
      const uint8_t k[] = {0,3,7,10,16,19,23,27,30};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {2,6,10,14,18,22,26,30};
      hits(seq, BD, k, 117); hits(seq, SD, s, 108); hits(seq, CH, h, 62, -9);
      hit(seq, CB, 11, 46, 76); hit(seq, CB, 27, 50, 82);
      hit(seq, SD, 31, 66, 78, 2);
      melodicHit(seq, MC, 6, 60, 52, 0, 76); melodicHit(seq, MC, 22, 63, 58, 0, 84);
      melodicHit(seq, LC, 0, 29, 80); melodicHit(seq, LC, 7, 36, 65);
      melodicHit(seq, LC, 16, 27, 82); melodicHit(seq, LC, 26, 32, 70);
      break;
    }
    case 7: { // Low Gravity — half-time weight, dusty ghosts and long harmonic space.
      const uint8_t k[] = {0,6,10,16,23,27};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {2,6,9,14,18,22,25,30};
      hits(seq, BD, k, 108); hits(seq, SD, s, 103); hits(seq, CH, h, 48, -10);
      hit(seq, SD, 11, 34, 44); hit(seq, SD, 27, 38, 52);
      hit(seq, RS, 7, 38, 48); hit(seq, OH, 31, 58, 72);
      melodicHit(seq, MC, 0, 29, 72); melodicHit(seq, MC, 10, 36, 58);
      melodicHit(seq, MC, 16, 27, 74); melodicHit(seq, MC, 27, 32, 62);
      chordHit(seq, LC, 0, 53,56,60, 38); chordHit(seq, LC, 16, 49,53,56, 36);
      sampleMotion(seq, CH, 2800, 5200, 16);
      break;
    }
    case 8: { // Acid Trace — most of the space belongs to samples, not the 303.
      fourFloor(seq, 119); backbeat(seq, 106); softOffbeats(seq, 63);
      hit(seq, OH, 14, 65); hit(seq, OH, 30, 72);
      hit(seq, RS, 11, 46, 64); hit(seq, CY, 0, 44, 72);
      const uint8_t st[] = {0,6,10,15,16,22,27,30};
      const uint8_t no[] = {29,36,32,39,29,41,32,27};
      for(size_t i = 0; i < sizeof(st); ++i)
        melodicHit(seq, LC, st[i], no[i], (i % 4u == 0u) ? 91 : 65,
                   (i == 3 || i == 7) ? 2 : 0, (i == 6) ? 82 : 100);
      break;
    }
    case 9: { // Sub Signal — modern bass rhythm with sample transients in front.
      const uint8_t k[] = {0,3,7,10,14,16,22,26,30};
      const uint8_t s[] = {4,12,20,28};
      hits(seq, BD, k, 119); hits(seq, SD, s, 108); softOffbeats(seq, 60);
      hit(seq, OH, 15, 64, 76); hit(seq, OH, 31, 70, 84);
      hit(seq, CB, 11, 42, 72); hit(seq, CB, 27, 47, 80);
      melodicHit(seq, MC, 0, 29, 82); melodicHit(seq, MC, 7, 29, 66);
      melodicHit(seq, MC, 10, 32, 74); melodicHit(seq, MC, 16, 27, 84);
      melodicHit(seq, MC, 26, 36, 70);
      chordHit(seq, LC, 0, 53,56,60, 30); chordHit(seq, LC, 16, 51,55,58, 28);
      break;
    }
    case 10: { // Pocket Chrome — swung conversation, ghosts below the main snare.
      const uint8_t k[] = {0,3,7,10,16,19,23,27,30};
      const uint8_t h[] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30};
      hits(seq, BD, k, 109); backbeat(seq, 111); hits(seq, CH, h, 49, -11);
      hit(seq, SD, 11, 36, 48); hit(seq, SD, 27, 42, 56);
      hit(seq, CP, 28, 62); hit(seq, OH, 31, 66, 80);
      melodicHit(seq, MC, 6, 60, 48, 0, 74); melodicHit(seq, MC, 22, 63, 54, 0, 82);
      melodicHit(seq, LC, 0, 29, 74); melodicHit(seq, LC, 7, 36, 60);
      melodicHit(seq, LC, 16, 27, 76); melodicHit(seq, LC, 26, 32, 64);
      break;
    }
    case 11: { // Organ Dust — raw sample house with quiet WT chord stabs.
      fourFloor(seq, 117); backbeat(seq, 108); softOffbeats(seq, 65);
      hit(seq, CP, 12, 72); hit(seq, CP, 28, 78);
      const uint8_t sh[] = {3,7,11,15,19,23,27,31}; hits(seq, MA, sh, 43, -8);
      chordHit(seq, LC, 2, 53,56,60, 50); chordHit(seq, LC, 10, 51,55,58, 44, 88);
      chordHit(seq, LC, 18, 49,53,56, 48); chordHit(seq, LC, 26, 48,51,55, 46, 90);
      hit(seq, LT, 29, 48, 58); hit(seq, MT, 30, 56, 66); hit(seq, HT, 31, 64, 76);
      break;
    }
    case 12: { // Night Bus — skipping hats, two-step drums and a low SH response.
      const uint8_t k[] = {0,7,10,16,22,26,31};
      const uint8_t c[] = {4,12,20,28};
      const uint8_t h[] = {1,3,6,9,11,14,17,19,22,25,27,30};
      hits(seq, BD, k, 114); hits(seq, CP, c, 106); hits(seq, CH, h, 53, -10);
      hit(seq, SD, 11, 38, 50); hit(seq, SD, 19, 42, 58); hit(seq, OH, 31, 68, 84);
      melodicHit(seq, MC, 0, 29, 76); melodicHit(seq, MC, 7, 36, 62);
      melodicHit(seq, MC, 16, 27, 78); melodicHit(seq, MC, 22, 39, 64, 0, 84);
      melodicHit(seq, LC, 14, 67, 42, 0, 66); melodicHit(seq, LC, 30, 70, 48, 0, 76);
      break;
    }
    case 13: { // Afterglow — slow sample pocket and barely-there harmonic light.
      const uint8_t k[] = {0,6,10,16,23,27};
      const uint8_t s[] = {4,12,20,28};
      const uint8_t h[] = {2,6,10,14,18,22,26,30};
      hits(seq, BD, k, 103); hits(seq, SD, s, 99); hits(seq, CH, h, 44, -9);
      hit(seq, SD, 11, 31, 42); hit(seq, RS, 7, 34, 46); hit(seq, OH, 31, 54, 66);
      melodicHit(seq, MC, 6, 60, 45, 0, 72); melodicHit(seq, MC, 22, 58, 48, 0, 78);
      chordHit(seq, LC, 0, 53,56,60, 39); chordHit(seq, LC, 16, 49,53,56, 36);
      sampleMotion(seq, CH, 2400, 4600, 20);
      break;
    }
    case 14: { // Peak Relay — fast sample break with a single controlled machine fill.
      const uint8_t k[] = {0,6,10,16,19,24,27,30};
      const uint8_t s[] = {4,12,20,25,28};
      const uint8_t h[] = {0,2,3,6,8,10,11,14,16,18,19,22,24,26,27,30};
      hits(seq, BD, k, 120); hits(seq, SD, s, 115); hits(seq, CH, h, 62, -10);
      hit(seq, CY, 0, 52); hit(seq, OH, 15, 67); hit(seq, OH, 31, 76);
      hit(seq, SD, 30, 62, 78, 2);
      hit(seq, LT, 29, 58); hit(seq, MT, 30, 68); hit(seq, HT, 31, 80, 100, 2);
      melodicHit(seq, MC, 0, 29, 80); melodicHit(seq, MC, 10, 36, 66);
      melodicHit(seq, MC, 16, 27, 82); melodicHit(seq, MC, 27, 32, 69);
      melodicHit(seq, LC, 31, 79, 40, 0, 66);
      break;
    }
    case 15: { // Machine Ritual — warehouse pulse, samples lead and FM stays low.
      fourFloor(seq, 122); backbeat(seq, 111); softOffbeats(seq, 68);
      const uint8_t hats[] = {1,5,9,13,17,21,25,29}; hits(seq, OH, hats, 56, -8);
      hit(seq, RS, 3, 48, 68); hit(seq, RS, 11, 54, 76);
      hit(seq, CY, 0, 48); hit(seq, CY, 16, 42, 72);
      melodicHit(seq, MC, 6, 48, 52, 0, 74); melodicHit(seq, MC, 14, 55, 58, 0, 80);
      melodicHit(seq, MC, 22, 51, 54, 0, 76); melodicHit(seq, MC, 30, 58, 62, 0, 84);
      melodicHit(seq, LC, 0, 29, 78); melodicHit(seq, LC, 10, 32, 64);
      melodicHit(seq, LC, 16, 27, 80); melodicHit(seq, LC, 27, 36, 68);
      sampleMotion(seq, CH, 6200, 10800, 8);
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
