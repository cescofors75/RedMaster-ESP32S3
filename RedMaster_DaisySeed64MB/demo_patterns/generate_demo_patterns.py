#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generador de banco de patrones a partir del DEMO de la Daisy.

Extrae el material musical del self-test de arranque de la Daisy
(RunStartup808SelfTest, DaisySeed/main.cpp:2367 PH_SYNTH_JAM) y lo convierte
en 20 patrones en el formato JSON que carga el master ESP32-S3
(WebInterface.cpp -> loadPatternBankFromFs).

Material de origen (idéntico al firmware Daisy):
  jamNotes        (TECHNO)  = 36 36 43 .  41 41 48 .  45 45 50 48  43 41 38 .
  jamNotesElectro (ELECTRO) = 36 43 36 .  48 46 43 .  41 43 45 .   50 48 46 43
  jamNotesAmbient (AMBIENT) = 36 .  43 .  48 .  50 .  53 .  48 .   45 .  41 .
  notes303 (escala)         = 36 38 41 43 45 48 50 53

Reglas de accent/slide del demo:
  no-ambient: accent = (st%4==0) o st in {6,14};  slide = (st%8==3) o st==11
  ambient:    accent = (st%8==0);                 slide = (st%8==7)

Reglas de percusión del demo (st = 0..15):
  TECHNO : kick st%4==0 | snare {4,12} | hatC impares | clap {7,15} | ride {10}
  ELECTRO: kick {0,6,8,14} | snare {4,12} | hatO st%4==2 | hatC impares | cowbell {11}
  AMBIENT: kick {0,8} | clap {4,12} | crash {6,14} | hatO st%4==2

NOTA IMPORTANTE (limitacion del firmware): el engine es GLOBAL por track
(gTrackSynthEngine[16] en S3 main.cpp:88), NO por patron. Por eso todo el banco
comparte un unico mapa track->engine, definido en TRACK_ENGINES abajo.
"""
import json

STEPS = 16

# ── Mapa GLOBAL track -> engine (compartido por TODOS los patrones) ──────────
# engine: -1 sampler | 0=808 | 1=909 | 2=505 | 3=303 | 4=WTOSC | 5=SH101 | 6=FM2Op
# El indice de track mapea a instrumento via padTo808/909/505 de la Daisy:
#   0 BD · 1 SD · 2 CH · 3 OH · 4 CY · 5 CP · 6 RS · 7 (303) · 8 LT · 9 MT · 10 HT
TRACK_ENGINES = [
    0,   # 0  BD  kick      (808)
    1,   # 1  SD  snare     (909)
    2,   # 2  CH  closed hat(505)
    1,   # 3  OH  open hat  (909)
    1,   # 4  CY  crash/ride(909)
    0,   # 5  CP  clap      (808)
    2,   # 6  RS  rimshot   (505)
    3,   # 7  303 ACID BASS (melodia)  <-- nota por paso
    0,   # 8  LT  low tom   (808)
    0,   # 9  MT  mid tom   (808)
    0,   # 10 HT  hi tom    (808)
    -1, -1, -1,           # 11-13 XTRA: sampler
    6,   # 14 FM2Op: campana / ataque brillante
    4,   # 15 WTOSC: firma armónica común (pad 16)
]
T_BD, T_SD, T_CH, T_OH, T_CY, T_CP, T_RS, T_303, T_LT, T_MT, T_HT = range(11)
T_FM2OP, T_WT = 14, 15

TRACK_NAMES = ["BD","SD","CH","OH","CY","CP","RS","303","LT","MT","HT",
               "MA","CL","HC","FM2OP","WT"]

# ── Lineas de bajo 303 (idénticas al demo Daisy) ─────────────────────────────
JAM_TECHNO  = [36,36,43,0, 41,41,48,0, 45,45,50,48, 43,41,38,0]
JAM_ELECTRO = [36,43,36,0, 48,46,43,0, 41,43,45,0, 50,48,46,43]
JAM_AMBIENT = [36,0,43,0, 48,0,50,0, 53,0,48,0, 45,0,41,0]
SCALE_303   = [36,38,41,43,45,48,50,53]

# ── Presets por estilo (preset que recuerda cada patron, por engine) ─────────
# 808: 0 Classic 1 HipHop 2 Techno 3 Latin 4 Pure
# 909: 0 Classic 1 Techno 2 HousePound 3 Industrial 4 Pure
# 505: 0 Classic 1 NewWave 2 Electro 3 LoFiHipHop 4 Pure
# 303: 0 Acid 1 Squelch 2 SubBass 3 SoftLead
PRESETS_BY_STYLE = {
    "techno":  {0: 2, 1: 1, 2: 2, 3: 0},   # 808 Techno, 909 Techno, 505 Electro, 303 Acid
    "electro": {0: 4, 1: 2, 2: 2, 3: 1},   # 808 Pure, 909 HousePound, 505 Electro, 303 Squelch
    "ambient": {0: 0, 1: 3, 2: 1, 3: 2},   # 808 Classic, 909 Industrial, 505 NewWave, 303 SubBass
    "acid":    {0: 0, 1: 0, 2: 0, 3: 0},   # 303 Acid + drums Classic
    "fill":    {0: 0, 1: 0, 2: 0, 3: 0, 4: 1, 5: 1, 6: 1},
    "lift":    {0: 2, 1: 1, 2: 2, 3: 1, 4: 1, 5: 2, 6: 2},
}

def preset_for(track, style):
    eng = TRACK_ENGINES[track]
    if eng < 0:
        return 0
    return PRESETS_BY_STYLE.get(style, PRESETS_BY_STYLE["acid"]).get(eng, 0)

def flags_for(st, ambient):
    """Devuelve byte de flags: bit0=accent, bit1=slide (regla del demo)."""
    if ambient:
        accent = (st % 8 == 0)
        slide  = (st % 8 == 7)
    else:
        accent = (st % 4 == 0) or st in (6, 14)
        slide  = (st % 8 == 3) or (st == 11)
    return (1 if accent else 0) | (2 if slide else 0)

def drum_track(track, hits, style, vel=110, accents=None, accent_vel=124):
    steps = [0]*STEPS
    vels  = [0]*STEPS
    for s in hits:
        steps[s] = 1
        vels[s]  = accent_vel if (accents and s in accents) else vel
    return {"track": track, "name": TRACK_NAMES[track],
            "engine": TRACK_ENGINES[track], "preset": preset_for(track, style),
            "steps": steps, "velocities": vels}

def acid_track(notes, ambient, style):
    """Track 303 con nota por paso, accent/slide y velocidades."""
    steps = [0]*STEPS; vels = [0]*STEPS; nts = [0]*STEPS; flgs = [0]*STEPS
    for st in range(STEPS):
        n = notes[st] if st < len(notes) else 0
        nts[st] = n
        if n:
            f = flags_for(st, ambient)
            flgs[st] = f
            steps[st] = 1
            vels[st]  = 122 if (f & 1) else 100
    return {"track": T_303, "name": "303", "engine": 3,
            "preset": preset_for(T_303, style),
            "steps": steps, "velocities": vels, "notes": nts, "flags": flgs}

def synth_track(track, notes, style, vel=88, accents=()):
    """Voz tonal cuantizada al mismo grid de 16 pasos que batería y 303."""
    steps = [1 if note else 0 for note in notes]
    vels = [(vel + 12 if i in accents else vel) if note else 0
            for i, note in enumerate(notes)]
    return {"track": track, "name": TRACK_NAMES[track],
            "engine": TRACK_ENGINES[track], "preset": preset_for(track, style),
            "steps": steps, "velocities": vels, "notes": notes,
            "flags": [0] * STEPS}

# ── Sets de percusion del demo ───────────────────────────────────────────────
def techno_drums():
    s = "techno"
    return [
        drum_track(T_BD, [0,4,8,12], s, vel=120, accents=[0,8]),
        drum_track(T_SD, [4,12],     s, vel=112),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], s, vel=78),
        drum_track(T_CP, [7,15],     s, vel=96),
        drum_track(T_CY, [10],       s, vel=88),   # ride
    ]
def electro_drums():
    s = "electro"
    return [
        drum_track(T_BD, [0,6,8,14], s, vel=120, accents=[0,8]),
        drum_track(T_SD, [4,12],     s, vel=110),
        drum_track(T_OH, [2,6,10,14],s, vel=82),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], s, vel=70),
        drum_track(T_RS, [11],       s, vel=92),   # rimshot 505
    ]
def ambient_drums():
    s = "ambient"
    return [
        drum_track(T_BD, [0,8],      s, vel=96),
        drum_track(T_CP, [4,12],     s, vel=72),
        drum_track(T_CY, [6,14],     s, vel=64),   # crash
        drum_track(T_OH, [2,6,10,14],s, vel=58),
    ]

# ── Definición de los 20 patrones ────────────────────────────────────────────
def build_patterns():
    P = []
    def add(slot, name, tracks):
        P.append({"slot": slot, "name": name, "tracks": tracks})

    # TECHNO (0-4)
    add(0, "TECHNO FULL",  techno_drums() + [acid_track(JAM_TECHNO, False, "techno")])
    add(1, "TECHNO BUILD", techno_drums() + [
        drum_track(T_OH, [2,6,10,14], "techno", vel=74),
        acid_track(JAM_TECHNO, False, "techno")])
    add(2, "TECHNO DRUMS", techno_drums())
    add(3, "TECHNO ACID",  [drum_track(T_BD, [0,4,8,12], "techno", vel=120)] +
                           [acid_track(JAM_TECHNO, False, "techno")])
    add(4, "TECHNO BREAK", [drum_track(T_CH, list(range(STEPS)), "techno", vel=70),
                            drum_track(T_CP, [7,15], "techno", vel=100),
                            acid_track(JAM_TECHNO, False, "techno")])

    # ELECTRO (5-9)
    # P06: C pentatónica abierta. Se elimina el E natural que formaba tritono
    # con el Bb del bajo y se añade una segunda síncopa 505, sin mover kick/bajo.
    electro_vector = [0,60,0,67, 0,62,0,69, 0,67,0,62, 0,60,0,65]
    add(5, "ELECTRO 505 VECTOR", [
        drum_track(T_BD, [0,6,8,14], "electro", vel=120, accents=[0,8]),
        drum_track(T_SD, [4,12], "electro", vel=110),
        drum_track(T_OH, [2,6,10,14], "electro", vel=78),
        drum_track(T_CH, [1,3,5,7,9,10,12,13,15], "electro", vel=68,
                   accents=[7,15], accent_vel=84),
        drum_track(T_RS, [2,7,11,14], "electro", vel=88,
                   accents=[11], accent_vel=104),
        acid_track(JAM_ELECTRO, False, "electro"),
        synth_track(T_WT, electro_vector, "lift", vel=74, accents=(3, 7, 15))])
    add(6, "ELECTRO BUILD", electro_drums() + [
        drum_track(T_CY, [0,8], "electro", vel=70),
        acid_track(JAM_ELECTRO, False, "electro")])
    add(7, "ELECTRO DRUMS", electro_drums())
    add(8, "ELECTRO ACID",  [drum_track(T_BD, [0,6,8,14], "electro", vel=118)] +
                            [acid_track(JAM_ELECTRO, False, "electro")])
    add(9, "ELECTRO BREAK", [drum_track(T_CH, list(range(STEPS)), "electro", vel=66),
                             drum_track(T_RS, [3,7,11,15], "electro", vel=92),
                             acid_track(JAM_ELECTRO, False, "electro")])

    # AMBIENT (10-13)
    # P11: conserva el pulso lento, pero el CH/RS 505 dibuja un groove cruzado.
    ambient_505_wt = [48,0,0,0, 0,0,53,0, 0,0,0,0, 57,0,0,0]
    add(10, "AMBIENT 505 PULSE", ambient_drums() + [
        drum_track(T_CH, [3,7,10,12,15], "ambient", vel=58,
                   accents=[7,15], accent_vel=72),
        drum_track(T_RS, [5,11,14], "ambient", vel=68,
                   accents=[11], accent_vel=82),
        acid_track(JAM_AMBIENT, True, "ambient"),
        synth_track(T_WT, ambient_505_wt, "lift", vel=58, accents=(12,))])
    add(11, "AMBIENT SPARSE", [drum_track(T_BD, [0,8], "ambient", vel=90),
                               acid_track(JAM_AMBIENT, True, "ambient")])
    # P13: se retira el HT afinado que sobresalía del centro tonal. El WT
    # responde sólo dos veces y convierte la percusión en frase, no en bucle.
    dub_anchor_wt = [0,0,0,0, 53,0,0,0, 0,0,0,0, 57,0,0,0]
    add(12, "505 DUB ANCHOR", [
        drum_track(T_BD, [0,6,8,11,14], "lift", vel=112, accents=[0,8]),
        drum_track(T_CH, [1,3,5,7,9,10,13,15], "lift", vel=68),
        drum_track(T_CP, [4,12], "lift", vel=94),
        drum_track(T_RS, [2,10,14], "lift", vel=80),
        drum_track(T_LT, [7,15], "lift", vel=88),
        synth_track(T_WT, dub_anchor_wt, "lift", vel=58, accents=(12,))])
    # P14: pulso 808/505 debajo de la armonía, con espacio entre respuestas.
    ambient_wt = [53,0,0,0, 57,0,0,0, 60,0,0,0, 57,0,0,0]
    add(13, "AMBIENT MACHINE", [
        drum_track(T_BD, [0,5,8,13], "lift", vel=104, accents=[0,8]),
        drum_track(T_CH, [2,6,10,14], "lift", vel=62),
        drum_track(T_CP, [7,15], "lift", vel=82),
        drum_track(T_RS, [3,11], "lift", vel=72),
        acid_track(JAM_AMBIENT, True, "ambient"),
        synth_track(T_WT, ambient_wt, "lift", vel=64, accents=(8,))])

    # ACID STUDIES (14-16) - escala notes303
    up   = [SCALE_303[i % 8] for i in range(STEPS)]
    octv = []
    for i in range(STEPS // 2):
        octv += [SCALE_303[i % 8], SCALE_303[(i + 4) % 8]]
    add(14, "ACID RUN UP", [
        drum_track(T_BD, [0,3,6,8,10,14], "lift", vel=116, accents=[0,8]),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], "lift", vel=68),
        drum_track(T_CP, [7,15], "lift", vel=94),
        drum_track(T_RS, [6,10,14], "lift", vel=84),
        acid_track(up, False, "acid")])
    # P16: una sola armonía C-D-F-G-A. El descenso tiene respiraciones y la
    # respuesta pasa al WT del pad 16 para evitar dos timbres compitiendo.
    acid_descent = [53,50,48,0, 45,43,41,0, 43,41,38,0, 36,0,43,0]
    descent_wt = [0,0,60,0, 0,0,57,0, 0,0,53,0, 0,0,55,0]
    add(15, "ACID DORIAN FALL", [
        drum_track(T_BD, [0,4,6,8,11,14], "lift", vel=116, accents=[0,8]),
        drum_track(T_SD, [4,12], "lift", vel=96),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], "lift", vel=66),
        drum_track(T_CP, [7,15], "lift", vel=88),
        drum_track(T_RS, [2,10,14], "lift", vel=78),
        acid_track(acid_descent, False, "acid"),
        synth_track(T_WT, descent_wt, "lift", vel=64, accents=(6, 14))])
    add(16, "ACID OCTAVE",   [drum_track(T_BD, [0,4,8,12], "acid", vel=116),
                              acid_track(octv, False, "acid")])

    # FILLS / TRANSICIONES (17-19): resolución siempre en el step 0 del compás.
    add(17, "TOM FILL", [
        drum_track(T_LT, [0,1,2,3],   "fill", vel=110),
        drum_track(T_MT, [4,5,6,7],   "fill", vel=114),
        drum_track(T_HT, [8,9,10,11], "fill", vel=118),
        drum_track(T_SD, [12,13,14,15], "fill", vel=122)])
    roll = {"track": T_SD, "name": "SD", "engine": TRACK_ENGINES[T_SD],
            "preset": preset_for(T_SD, "fill"),
            "steps": [1]*STEPS,
            "velocities": [60 + int((127-60) * (i/(STEPS-1))) for i in range(STEPS)]}
    # P19: el roll ya no arranca aislado; kick raíz + WT ascendente fijan el downbeat.
    lift_wt = [0, 0, 60, 0, 0, 62, 0, 0, 64, 0, 0, 67, 0, 0, 69, 72]
    add(18, "SNARE LIFT", [
        drum_track(T_BD, [0,4,8,12], "lift", vel=112, accents=[0,8]),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], "lift", vel=60),
        drum_track(T_CP, [7,15], "lift", vel=88),
        drum_track(T_RS, [6,10,14], "lift", vel=76),
        drum_track(T_LT, [14], "lift", vel=90),
        drum_track(T_MT, [15], "lift", vel=98), roll,
        synth_track(T_WT, lift_wt, "lift", vel=70, accents=(11, 15))])

    # P20: final completo; fundación, groove, bajo, respuesta y brillo.
    final_wt = [53, 0, 0, 57, 0, 0, 60, 0, 53, 0, 0, 57, 0, 0, 62, 0]
    final_fm = [0, 0, 72, 0, 0, 0, 76, 0, 0, 0, 79, 0, 0, 0, 81, 84]
    add(19, "FINAL TRANSCENDENCE", [
        drum_track(T_BD, [0,4,8,12], "lift", vel=120, accents=[0,8]),
        drum_track(T_SD, [4,12], "lift", vel=110),
        drum_track(T_CH, [1,3,5,7,9,11,13,15], "lift", vel=64),
        drum_track(T_OH, [6,14], "lift", vel=76),
        drum_track(T_RS, [11,15], "lift", vel=78),
        drum_track(T_CP, [7,15], "lift", vel=92),
        drum_track(T_LT, [10], "lift", vel=86),
        drum_track(T_MT, [13], "lift", vel=92),
        drum_track(T_HT, [14,15], "lift", vel=96),
        acid_track([36,0,43,0, 41,0,48,0, 45,0,50,0, 43,0,41,0], False, "electro"),
        synth_track(T_WT, final_wt, "lift", vel=72, accents=(6, 14)),
        synth_track(T_FM2OP, final_fm, "lift", vel=62, accents=(14, 15))])
    return P

def build_song_chain():
    # Un recorrido por el banco (pattern, repeats) 1..16
    return [
        {"pattern": 2,  "repeats": 1},  # TECHNO DRUMS (intro)
        {"pattern": 0,  "repeats": 4},  # TECHNO FULL
        {"pattern": 1,  "repeats": 2},  # TECHNO BUILD
        {"pattern": 17, "repeats": 1},  # TOM FILL
        {"pattern": 5,  "repeats": 4},  # ELECTRO FULL
        {"pattern": 6,  "repeats": 2},  # ELECTRO BUILD
        {"pattern": 18, "repeats": 1},  # SNARE LIFT
        {"pattern": 10, "repeats": 4},  # AMBIENT FULL
        {"pattern": 13, "repeats": 2},  # AMBIENT PAD (respiro)
        {"pattern": 19, "repeats": 2},  # FINAL TRANSCENDENCE (outro)
    ]

def main():
    bank = {
        "name": "20 Patrones Factory Daisy (Techno/Electro/Ambient + Acid)",
        "tempo": 124,
        "stepCount": STEPS,
        "selectPattern": 0,
        "trackEngines": TRACK_ENGINES,
        "patterns": build_patterns(),
        "songChain": build_song_chain(),
    }
    out = "20_patrones_factory_daisy.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(bank, f, ensure_ascii=False, indent=1)
    print(f"OK -> {out}: {len(bank['patterns'])} patrones, "
          f"songChain {len(bank['songChain'])} entradas, stepCount {STEPS}")

if __name__ == "__main__":
    main()
