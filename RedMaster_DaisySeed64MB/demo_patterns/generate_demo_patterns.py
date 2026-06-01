#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generador del banco "THE BELLS — 19 escenas" para el master ESP32-S3.

Recreacion de demo_bells.cpp (DaisySeed) = "The Bells" de Jeff Mills, llevada
al secuenciador del master como 19 ESCENAS para tocar en directo: cambiando de
patron interpretas el build-up, los grooves, breakdowns y solos (el "truco
Mills" de mutear/desmutear bucles).

Material EXTRAIDO 1:1 de demo_bells.cpp:
  · 909  → KICK (4x4), OPEN HAT (offbeats), RIDE (corcheas), CLAP (backbeat)
  · FM2Op→ BELLS: motif en La menor (2 voces, hi siempre / lo mute-unmute)
       kBellHi = A5 . . E5 . G5 . . A5 . . C6 . B5 . .
       kBellLo = A4 . . . . . E4 . . . . C5 . . . .
  · 303  → BASS: A1 en semicorcheas, acento por tiempo, slides ocasionales

Engine POR PATRON (recall en el S3): 909=1, 303=3, FM2Op=6.
"""
import json

STEPS = 16
ENG_909, ENG_303, ENG_FM = 1, 3, 6

# Tracks (909: padTo909 → 0=KICK 3=HIHAT_O 5=CLAP 7=RIDE)
T_KICK, T_OH, T_CLAP, T_RIDE, T_BELLS, T_BASS = 0, 3, 5, 7, 8, 10

# Presets: 909 Industrial-Mills(3) | FM2Op Bell-Mills(2) | 303 TheBells-bass(4)
PRE_909, PRE_FM, PRE_303 = 3, 2, 4

# ── Melodias reales de demo_bells.cpp ──
kBellHi = [81,0,0,76, 0,79,0,0, 81,0,0,84, 0,83,0,0]   # A5 E5 G5 A5 C6 B5
kBellLo = [69,0,0,0,  0,0,64,0, 0,0,0,72,  0,0,0,0]     # A4 E4 C5
kBass   = [33]*STEPS                                    # A1 steady
kBassAcc= [1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0]

# ── Patrones de bateria 909 ──
HITS_KICK = [0,4,8,12]
HITS_OH   = [2,6,10,14]
HITS_RIDE = [0,2,4,6,8,10,12,14]
HITS_CLAP = [4,12]


def drum909(track, hits, vel, accents=None, accent_vel=127):
    steps = [0]*STEPS; vels = [0]*STEPS
    for s in hits:
        steps[s] = 1
        vels[s]  = accent_vel if (accents and s in accents) else vel
    name = {0:"KICK",3:"OH",5:"CLAP",7:"RIDE"}[track]
    return {"track": track, "name": name, "engine": ENG_909, "preset": PRE_909,
            "steps": steps, "velocities": vels}


def bells_track(hi_on, lo_on):
    """FM2Op, 2 voces: voice0=bell alta, voice1=bell baja (segun escena)."""
    steps=[0]*STEPS; vels=[0]*STEPS; flags=[0]*STEPS
    voices=[[] for _ in range(STEPS)]
    for st in range(STEPS):
        v=[]
        if hi_on and kBellHi[st]: v.append(kBellHi[st])
        if lo_on and kBellLo[st]: v.append(kBellLo[st])
        if v:
            voices[st]=v
            steps[st]=1
            accent = (st % 4 == 0)
            vels[st]= 115 if accent else 83          # 0.9 / 0.65 del demo
            flags[st]= 1 if accent else 0
    return {"track": T_BELLS, "name": "BELLS", "engine": ENG_FM, "preset": PRE_FM,
            "steps": steps, "velocities": vels, "noteVoices": voices, "flags": flags}


def bass_track():
    """303: A1 en 16 semicorcheas, acento por tiempo, slide en step%4==3."""
    steps=[1]*STEPS; vels=[0]*STEPS; notes=list(kBass); flags=[0]*STEPS
    for st in range(STEPS):
        acc = kBassAcc[st]!=0
        slide = (st % 4 == 3)
        vels[st]= 120 if acc else 95
        flags[st]= (1 if acc else 0) | (2 if slide else 0)
    return {"track": T_BASS, "name": "303", "engine": ENG_303, "preset": PRE_303,
            "steps": steps, "velocities": vels, "notes": notes, "flags": flags}


def scene(name, kick=False, oh=False, ride=False, clap=False,
          bellHi=False, bellLo=False, bass=False):
    tr=[]
    if kick: tr.append(drum909(T_KICK, HITS_KICK, 118, accents=[0,8]))
    if ride: tr.append(drum909(T_RIDE, HITS_RIDE, 76))
    if oh:   tr.append(drum909(T_OH,   HITS_OH,   92))
    if clap: tr.append(drum909(T_CLAP, HITS_CLAP, 108))
    if bellHi or bellLo: tr.append(bells_track(bellHi, bellLo))
    if bass: tr.append(bass_track())
    return name, tr


# ── Las 19 escenas (build-up → performance → solos → climax) ──
SCENES = [
    scene("KICK",          kick=True),
    scene("+ RIDE",        kick=True, ride=True),
    scene("+ OPEN HAT",    kick=True, ride=True, oh=True),
    scene("+ BELL HI",     kick=True, ride=True, oh=True, bellHi=True),
    scene("+ CLAP+BELL LO",kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True),
    scene("FULL",          kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True, bass=True),
    scene("GROOVE A",      kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True, bass=True),
    scene("GROOVE B",      kick=True, ride=True, oh=True, clap=True, bellHi=True, bass=True),            # bell lo MUTE
    scene("GROOVE C",      kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True, bass=True),
    scene("BREAKDOWN",     kick=True, bellHi=True, bellLo=True, bass=True),                              # drums a kick
    scene("REBUILD",       kick=True, ride=True, oh=True, bellHi=True, bass=True),
    scene("PEAK",          kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True, bass=True),
    scene("BELLS+BASS",    bellHi=True, bellLo=True, bass=True),                                          # sin bateria
    scene("KICK+BASS",     kick=True, bass=True),
    scene("BELLS SOLO",    bellHi=True, bellLo=True),
    scene("DRUMS SOLO",    kick=True, ride=True, oh=True, clap=True),
    scene("BASS SOLO",     bass=True),
    scene("RIDE+BELLS",    ride=True, bellHi=True, bellLo=True),
    scene("CLIMAX",        kick=True, ride=True, oh=True, clap=True, bellHi=True, bellLo=True, bass=True),
]


def build_patterns():
    return [{"slot": i, "name": nm, "tracks": tr}
            for i, (nm, tr) in enumerate(SCENES)]


def build_song_chain():
    # Recorrido tipo The Bells: build-up -> grooves -> breakdown -> climax
    seq = [(0,2),(1,2),(2,2),(3,2),(4,2),(5,4),(6,4),(7,4),(8,4),
           (9,2),(10,2),(11,4),(12,2),(18,4)]
    return [{"pattern": p, "repeats": r} for p, r in seq]


def main():
    bank = {
        "name": "The Bells (Jeff Mills) - 19 escenas performance",
        "tempo": 132, "stepCount": STEPS, "selectPattern": 0,
        # Etapa 2: espacio del demo (reverb larga sobre campanas + delay del clap)
        "fx": {
            "reverb": {"active": True, "feedback": 0.82, "lpFreq": 8500, "mix": 0.35},
            "delay":  {"active": True, "timeMs": 375, "feedback": 0.45, "mix": 0.22},
            # sends por track: campanas (8) a reverb; clap (5) y ride (7) al delay
            "reverbSend": {str(T_BELLS): 70, str(T_BASS): 10},
            "delaySend":  {str(T_CLAP): 60, str(T_RIDE): 25},
        },
        "patterns": build_patterns(),
        "songChain": build_song_chain(),
    }
    out = "19_temas_demo_daisy.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(bank, f, ensure_ascii=False, separators=(",", ":"))
    import os
    print(f"OK -> {out}: {len(bank['patterns'])} escenas, tempo {bank['tempo']}, "
          f"{os.path.getsize(out)} bytes (minificado)")

if __name__ == "__main__":
    main()
