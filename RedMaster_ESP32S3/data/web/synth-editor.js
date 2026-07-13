/**
 * synth-editor.js
 * Synth Parameter Editor for RED808 Drum Machine
 * Provides waveform visualization on pads and a full parameter modal
 * for TR-808, TR-909, TR-505, TB-303, WTOSC, SH-101, and FM2Op.
 */

// ============================================================
//  PAD → INSTRUMENT MAPPING (mirrors Daisy main.cpp padTo*)
// ============================================================
const PAD_TO_808 = [0,1,3,4,15,2,13,14,5,6,7,12,11,10,9,8];
const PAD_TO_909 = [0,1,3,4,9,2,10,8,5,6,7,11,12,13,14,15];
const PAD_TO_505 = [0,1,3,4,9,2,10,8,5,6,7,11,12,13,14,15];

function padToInstrument(engine, padIndex) {
    if (engine === 0) return PAD_TO_808[padIndex] ?? padIndex;
    if (engine === 1) return PAD_TO_909[padIndex] ?? padIndex;
    if (engine === 2) return PAD_TO_505[padIndex] ?? padIndex;
    return padIndex; // 303 not instrument-based
}

// ============================================================
//  INSTRUMENT NAMES PER ENGINE
// ============================================================
const INST_NAMES_808 = {
    0:'Kick', 1:'Snare', 2:'Clap', 3:'HiHat Closed', 4:'HiHat Open',
    5:'Low Tom', 6:'Mid Tom', 7:'Hi Tom', 8:'Low Conga', 9:'Mid Conga',
    10:'Hi Conga', 11:'Claves', 12:'Maracas', 13:'Rimshot', 14:'Cowbell', 15:'Cymbal'
};
const INST_NAMES_909 = {
    0:'Kick', 1:'Snare', 2:'Clap', 3:'HiHat Closed', 4:'HiHat Open',
    5:'Low Tom', 6:'Mid Tom', 7:'Hi Tom', 8:'Ride', 9:'Crash', 10:'Rimshot',
    11:'Shaker', 12:'Clave', 13:'Hi Perc', 14:'Mid Perc', 15:'Low Perc'
};
const INST_NAMES_505 = {
    0:'Kick', 1:'Snare', 2:'Clap', 3:'HiHat Closed', 4:'HiHat Open',
    5:'Low Tom', 6:'Mid Tom', 7:'Hi Tom', 8:'Cowbell', 9:'Cymbal', 10:'Rimshot',
    11:'Shaker', 12:'Clave', 13:'Hi Perc', 14:'Mid Perc', 15:'Low Perc'
};

function getInstrumentName(engine, instrument) {
    if (engine === 0) return INST_NAMES_808[instrument] || `Inst ${instrument}`;
    if (engine === 1) return INST_NAMES_909[instrument] || `Inst ${instrument}`;
    if (engine === 2) return INST_NAMES_505[instrument] || `Inst ${instrument}`;
    if (engine === 3) return 'TB-303 Bass';
    if (engine === 4) return 'Wavetable OSC';
    if (engine === 5) return 'SH-101 Lead';   /* I1 */
    if (engine === 6) return 'FM 2-Op';        /* I2 */
    return `Inst ${instrument}`;
}

// ============================================================
//  PARAMETER DEFINITIONS PER ENGINE+INSTRUMENT
//  {id, name, min, max, step, default, unit, paramId}
//  paramId = SYNTH_PARAM_* constant sent to Daisy
// ============================================================
const PARAM_DECAY  = 0;
const PARAM_PITCH  = 1;
const PARAM_TONE   = 2;
const PARAM_VOLUME = 3;
const PARAM_SNAPPY = 4;

// Helper to make param definition
function P(paramId, name, min, max, step, def, unit='') {
    return { paramId, name, min, max, step, default: def, unit };
}

// TR-808 params per instrument index  — v2.0
const PARAMS_808 = {
    0:  [ P(0,'Decay',  0.05,2,   0.01, 0.45,'s'),  P(1,'Pitch',    30,120, 1,    55,  'Hz'),
          P(2,'Drive',  0,1,      0.01, 0.3),         P(4,'Sub Osc',  0,0.5,  0.01, 0.15),
          P(5,'Ptch Amt',1,20,    0.1,  8),            P(6,'Ptch Dec', 0.01,0.5,0.01,0.08,'s'),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    1:  [ P(0,'Decay',  0.05,1,   0.01, 0.18,'s'),   P(1,'Pitch',    100,350,1,    185, 'Hz'),
          P(2,'Tone',   0,1,      0.01, 0.5),          P(4,'Snappy',   0,1,    0.01, 0.6),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    2:  [ P(0,'Decay',  0.05,1,   0.01, 0.28,'s'),   P(2,'Snap',     0,1,    0.01, 0.7),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    3:  [ P(0,'Decay',  0.01,0.3, 0.005,0.042,'s'),  P(3,'Volume',   0,1,    0.01, 0.8) ],
    4:  [ P(0,'Decay',  0.05,2,   0.01, 0.28,'s'),   P(3,'Volume',   0,1,    0.01, 0.8) ],
    5:  [ P(0,'Decay',  0.05,1.5, 0.01, 0.32,'s'),   P(1,'Pitch',    40,200, 1,    78,  'Hz'),
          P(5,'Smack',  0,1,      0.01, 0.18),         P(3,'Volume',   0,1,    0.01, 0.8) ],
    6:  [ P(0,'Decay',  0.05,1.5, 0.01, 0.26,'s'),   P(1,'Pitch',    60,300, 1,    118, 'Hz'),
          P(5,'Smack',  0,1,      0.01, 0.15),         P(3,'Volume',   0,1,    0.01, 0.8) ],
    7:  [ P(0,'Decay',  0.05,1.5, 0.01, 0.20,'s'),   P(1,'Pitch',    80,400, 1,    175, 'Hz'),
          P(5,'Smack',  0,1,      0.01, 0.12),         P(3,'Volume',   0,1,    0.01, 0.8) ],
    8:  [ P(0,'Decay',  0.03,0.6, 0.01, 0.20,'s'),   P(1,'Pitch',    60,300, 1,    168, 'Hz'),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    9:  [ P(0,'Decay',  0.03,0.6, 0.01, 0.16,'s'),   P(1,'Pitch',    80,400, 1,    248, 'Hz'),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    10: [ P(0,'Decay',  0.03,0.6, 0.01, 0.13,'s'),   P(1,'Pitch',    100,500,1,    368, 'Hz'),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    11: [ P(3,'Volume', 0,1,      0.01, 0.8) ],
    12: [ P(3,'Volume', 0,1,      0.01, 0.8) ],
    13: [ P(3,'Volume', 0,1,      0.01, 0.8) ],
    14: [ P(0,'Decay',  0.02,0.8, 0.005,0.08,'s'),   P(1,'Tune',     0.7,1.5,0.01, 1.0),
          P(3,'Volume', 0,1,      0.01, 0.8) ],
    15: [ P(0,'Decay',  0.1,5,    0.01, 0.85,'s'),   P(3,'Volume',   0,1,    0.01, 0.8) ],
};

// TR-909 params per instrument index
const PARAMS_909 = {
    0:  [ P(0,'Decay',0,1,0.01,0.4), P(1,'Pitch',20,200,1,50,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    1:  [ P(0,'Decay',0,1,0.01,0.25), P(2,'Tone',0,1,0.01,0.5), P(4,'Snappy',0,1,0.01,0.5), P(3,'Volume',0,1,0.01,0.8) ],
    2:  [ P(0,'Decay',0,1,0.01,0.3), P(3,'Volume',0,1,0.01,0.8) ],
    3:  [ P(0,'Decay',0,0.5,0.005,0.04), P(3,'Volume',0,1,0.01,0.8) ],
    4:  [ P(0,'Decay',0,1,0.01,0.3), P(3,'Volume',0,1,0.01,0.8) ],
    5:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',40,200,1,80,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    6:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',60,300,1,120,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    7:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',80,400,1,180,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    8:  [ P(0,'Decay',0,2,0.01,0.5), P(3,'Volume',0,1,0.01,0.8) ],
    9:  [ P(0,'Decay',0,2,0.01,0.8), P(3,'Volume',0,1,0.01,0.8) ],
    10: [ P(3,'Volume',0,1,0.01,0.8) ],
    11: [ P(0,'Decay',0.02,0.45,0.005,0.085,'s'), P(2,'Tone',0,1,0.01,0.65), P(3,'Volume',0,1,0.01,0.75) ],
    12: [ P(0,'Decay',0.015,0.25,0.005,0.055,'s'), P(1,'Pitch',900,3200,1,1750,'Hz'), P(3,'Volume',0,1,0.01,0.75) ],
    13: [ P(0,'Decay',0.035,0.55,0.005,0.085,'s'), P(1,'Pitch',160,1600,1,820,'Hz'), P(2,'Metal',0,1,0.01,0.28), P(3,'Volume',0,1,0.01,0.7) ],
    14: [ P(0,'Decay',0.035,0.55,0.005,0.12,'s'), P(1,'Pitch',160,1600,1,520,'Hz'), P(2,'Metal',0,1,0.01,0.2), P(3,'Volume',0,1,0.01,0.72) ],
    15: [ P(0,'Decay',0.035,0.55,0.005,0.17,'s'), P(1,'Pitch',160,1600,1,310,'Hz'), P(2,'Metal',0,1,0.01,0.14), P(3,'Volume',0,1,0.01,0.74) ],
};

// TR-505 params per instrument index
const PARAMS_505 = {
    0:  [ P(0,'Decay',0,1,0.01,0.4), P(1,'Pitch',20,200,1,55,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    1:  [ P(0,'Decay',0,1,0.01,0.25), P(2,'Tone',0,1,0.01,0.5), P(3,'Volume',0,1,0.01,0.8) ],
    2:  [ P(0,'Decay',0,1,0.01,0.3), P(3,'Volume',0,1,0.01,0.8) ],
    3:  [ P(0,'Decay',0,0.5,0.005,0.04), P(3,'Volume',0,1,0.01,0.8) ],
    4:  [ P(0,'Decay',0,1,0.01,0.3), P(3,'Volume',0,1,0.01,0.8) ],
    5:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',40,200,1,80,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    6:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',60,300,1,120,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    7:  [ P(0,'Decay',0,1,0.01,0.3), P(1,'Pitch',80,400,1,180,'Hz'), P(3,'Volume',0,1,0.01,0.8) ],
    8:  [ P(0,'Decay',0,0.5,0.005,0.1), P(3,'Volume',0,1,0.01,0.8) ],
    9:  [ P(0,'Decay',0,2,0.01,0.8), P(3,'Volume',0,1,0.01,0.8) ],
    10: [ P(3,'Volume',0,1,0.01,0.8) ],
    11: [ P(0,'Decay',0.02,0.45,0.005,0.095,'s'), P(2,'Tone',0,1,0.01,0.55), P(5,'LoFi',0,1,0.01,0.35), P(3,'Volume',0,1,0.01,0.76) ],
    12: [ P(0,'Decay',0.015,0.25,0.005,0.05,'s'), P(1,'Pitch',800,2800,1,1650,'Hz'), P(5,'LoFi',0,1,0.01,0.3), P(3,'Volume',0,1,0.01,0.76) ],
    13: [ P(0,'Decay',0.035,0.5,0.005,0.075,'s'), P(1,'Pitch',120,1400,1,760,'Hz'), P(2,'Click',0,1,0.01,0.34), P(5,'LoFi',0,1,0.01,0.34), P(3,'Volume',0,1,0.01,0.7) ],
    14: [ P(0,'Decay',0.035,0.5,0.005,0.115,'s'), P(1,'Pitch',120,1400,1,480,'Hz'), P(2,'Click',0,1,0.01,0.26), P(5,'LoFi',0,1,0.01,0.34), P(3,'Volume',0,1,0.01,0.72) ],
    15: [ P(0,'Decay',0.035,0.5,0.005,0.16,'s'), P(1,'Pitch',120,1400,1,300,'Hz'), P(2,'Click',0,1,0.01,0.18), P(5,'LoFi',0,1,0.01,0.34), P(3,'Volume',0,1,0.01,0.74) ],
};

// TB-303 params (shared across all pads when engine=303)
const PARAMS_303 = [
    { paramId:0,  name:'Cutoff',    min:20,   max:5000,  step:1,    default:800,   unit:'Hz' },
    { paramId:1,  name:'Resonance', min:0,    max:0.97,  step:0.01, default:0.5,   unit:'' },
    { paramId:2,  name:'Env Mod',   min:0,    max:1,     step:0.01, default:0.5,   unit:'' },
    { paramId:3,  name:'Decay',     min:0.02, max:3,     step:0.01, default:0.3,   unit:'s' },
    { paramId:8,  name:'Attack',    min:0.001,max:2,     step:0.001,default:0.001, unit:'s' },
    { paramId:9,  name:'Sustain',   min:0,    max:1,     step:0.01, default:0,     unit:'' },
    { paramId:10, name:'Release',   min:0.005,max:2,     step:0.005,default:0.05,  unit:'s' },
    { paramId:4,  name:'Accent',    min:0,    max:1,     step:0.01, default:0.5,   unit:'' },
    { paramId:5,  name:'Slide',     min:0.01, max:0.5,   step:0.005,default:0.06,  unit:'s' },
    { paramId:11, name:'Overdrive', min:0,    max:1,     step:0.01, default:0,     unit:'' },
    { paramId:12, name:'Sub Osc',   min:0,    max:1,     step:0.01, default:0,     unit:'' },
    { paramId:13, name:'Drift',     min:0,    max:1,     step:0.01, default:0,     unit:'' },
    { paramId:14, name:'Pitch Bend',min:-12,  max:12,    step:0.1,  default:0,     unit:'st' },
    { paramId:6,  name:'Waveform',  min:0,    max:1,     step:1,    default:0,     unit:'', labels:['SAW','SQR'] },
    { paramId:7,  name:'Volume',    min:0,    max:1,     step:0.01, default:0.7,   unit:'' },
];

// Wavetable OSC params (engine 4)
const PARAMS_WTOSC = [
    { paramId:0, name:'Wave Pos',    min:0,    max:7,     step:0.01, default:0,    unit:'' },
    { paramId:1, name:'Attack',      min:0,    max:2000,  step:1,    default:5,    unit:'ms' },
    { paramId:2, name:'Decay',       min:1,    max:4000,  step:1,    default:300,  unit:'ms' },
    { paramId:3, name:'Volume',      min:0,    max:1,     step:0.01, default:0.75, unit:'' },
    { paramId:4, name:'Filter Cut',  min:0,    max:18000, step:10,   default:8000, unit:'Hz' },
    { paramId:5, name:'LFO Rate',    min:0.01, max:20,    step:0.01, default:2,    unit:'Hz' },
    { paramId:6, name:'LFO Depth',   min:0,    max:1,     step:0.01, default:0,    unit:'' },
    { paramId:7, name:'LFO Target',  min:0,    max:2,     step:1,    default:0,    unit:'', labels:['Wave','Pitch','Vol'] },
];

// SH-101 params (engine 5 — monosynth, param IDs match SH101::SetParam)
const PARAMS_SH101 = [
    { paramId:0,  name:'Waveform', min:0, max:2, step:1, default:0, unit:'', labels:['SAW','SQR','PUL'] },
    { paramId:1,  name:'PWM Width', min:0.1, max:0.9, step:0.01, default:0.5, unit:'' },
    { paramId:2,  name:'Sub Level', min:0, max:1, step:0.01, default:0.3, unit:'' },
    { paramId:4,  name:'VCF Cutoff', min:20, max:18000, step:10, default:2000, unit:'Hz' },
    { paramId:5,  name:'VCF Res',    min:0, max:1, step:0.01, default:0.4, unit:'' },
    { paramId:6,  name:'Env → VCF',  min:0, max:1, step:0.01, default:0.5, unit:'' },
    { paramId:7,  name:'VCA Atk',    min:0.001, max:2, step:0.001, default:0.005, unit:'s' },
    { paramId:8,  name:'VCA Dec',    min:0.01, max:3, step:0.01, default:0.3, unit:'s' },
    { paramId:9,  name:'VCA Sus',    min:0, max:1, step:0.01, default:0.6, unit:'' },
    { paramId:10, name:'VCA Rel',    min:0.005, max:3, step:0.005, default:0.15, unit:'s' },
    { paramId:11, name:'VCF Atk',    min:0.001, max:2, step:0.001, default:0.005, unit:'s' },
    { paramId:12, name:'VCF Dec',    min:0.01, max:3, step:0.01, default:0.2, unit:'s' },
    { paramId:13, name:'LFO Rate',   min:0.1, max:20, step:0.1, default:4, unit:'Hz' },
    { paramId:14, name:'LFO Depth',  min:0, max:1, step:0.01, default:0, unit:'' },
    { paramId:15, name:'LFO Target', min:0, max:2, step:1, default:0, unit:'', labels:['Pitch','Cutoff','PWM'] },
    { paramId:16, name:'LFO Wave',   min:0, max:3, step:1, default:0, unit:'', labels:['SIN','TRI','SQR','SAW'] },
    { paramId:17, name:'Portamento', min:0, max:1, step:0.01, default:0, unit:'' },
    { paramId:18, name:'A-Drift',    min:0, max:1, step:0.01, default:0.1, unit:'' },
    { paramId:19, name:'Volume',     min:0, max:1, step:0.01, default:0.75, unit:'' },
];

// FM 2-Op params (engine 6 — Yamaha-style)
const PARAMS_FM2OP = [
    { paramId:0,  name:'C Attack',   min:0.001, max:2, step:0.001, default:0.005, unit:'s' },
    { paramId:1,  name:'C Decay',    min:0.01, max:3, step:0.01, default:0.4, unit:'s' },
    { paramId:2,  name:'C Sustain',  min:0, max:1, step:0.01, default:0.3, unit:'' },
    { paramId:3,  name:'C Release',  min:0.005, max:3, step:0.005, default:0.2, unit:'s' },
    { paramId:4,  name:'M Attack',   min:0.001, max:2, step:0.001, default:0.001, unit:'s' },
    { paramId:5,  name:'M Decay',    min:0.01, max:3, step:0.01, default:0.25, unit:'s' },
    { paramId:6,  name:'M Sustain',  min:0, max:1, step:0.01, default:0, unit:'' },
    { paramId:7,  name:'M Release',  min:0.005, max:3, step:0.005, default:0.1, unit:'s' },
    { paramId:8,  name:'M/C Ratio',  min:0.5, max:8, step:0.5, default:2, unit:'x' },
    { paramId:9,  name:'FM Index',   min:0, max:12, step:0.1, default:3, unit:'' },
    { paramId:10, name:'Feedback',   min:0, max:1, step:0.01, default:0, unit:'' },
    { paramId:11, name:'Algorithm',  min:0, max:2, step:1, default:0, unit:'', labels:['FM','ADD','RING'] },
    { paramId:12, name:'Detune',     min:-1, max:1, step:0.01, default:0, unit:'st' },
    { paramId:13, name:'Vel Sens',   min:0, max:1, step:0.01, default:0.7, unit:'' },
    { paramId:14, name:'Volume',     min:0, max:1, step:0.01, default:0.75, unit:'' },
];

const SYNTH_FACTORY_PRESETS = {
    0: [
        { name:'Classic 808', volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.45,1:55,2:0.30,4:0.15,5:8.0,6:0.08,3:0.80}, 1:{0:0.18,1:185,2:0.50,4:0.60,3:0.80}, 2:{0:0.28,2:0.70,3:0.80}, 3:{0:0.042,3:0.80}, 4:{0:0.28,3:0.80}, 14:{0:0.08,1:1.0,3:0.80}, 15:{0:0.85,3:0.80} } },
        { name:'Hip-Hop',     volumes:[1.1, 0.9, 0.8, 0.55, 0.6, 0.8, 0.7, 0.7, 0.75, 0.7, 0.65, 0.6, 0.5, 0.7, 0.5, 0.4], instrumentValues:{ 0:{0:0.80,1:46,2:0.55,4:0.24,5:10.5,6:0.12,3:0.92}, 1:{0:0.28,1:160,2:0.35,4:0.72,3:0.74}, 2:{0:0.34,2:0.58,3:0.72}, 3:{0:0.030,3:0.55}, 4:{0:0.22,3:0.60}, 5:{0:0.42,1:70,5:0.22,3:0.76}, 6:{0:0.34,1:108,5:0.20,3:0.70}, 7:{0:0.28,1:162,5:0.16,3:0.68} } },
        { name:'Techno',      volumes:[1.2, 1.0, 0.6, 0.8, 0.7, 0.5, 0.5, 0.5, 0.4, 0.4, 0.4, 0.3, 0.6, 0.5, 0.3, 0.5], instrumentValues:{ 0:{0:0.36,1:62,2:0.62,4:0.10,5:13.0,6:0.05,3:0.95}, 1:{0:0.16,1:210,2:0.68,4:0.45,3:0.82}, 2:{0:0.20,2:0.85,3:0.62}, 3:{0:0.050,3:0.82}, 4:{0:0.40,3:0.74}, 14:{0:0.05,1:1.18,3:0.55}, 15:{0:1.20,3:0.62} } },
        { name:'Latin',       volumes:[0.7, 0.6, 0.4, 0.5, 0.5, 0.8, 0.8, 0.9, 1.0, 1.0, 1.1, 1.1, 0.8, 0.8, 0.7, 0.4], instrumentValues:{ 0:{0:0.30,1:58,2:0.18,4:0.10,5:7.0,6:0.06,3:0.68}, 5:{0:0.48,1:92,5:0.14,3:0.86}, 6:{0:0.42,1:144,5:0.14,3:0.84}, 7:{0:0.34,1:215,5:0.10,3:0.92}, 8:{0:0.26,1:150,3:0.92}, 9:{0:0.22,1:228,3:0.96}, 10:{0:0.18,1:320,3:1.00}, 11:{3:0.96}, 12:{3:0.78}, 13:{3:0.82} } },
        { name:'Pure 808',    volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.45,1:52,2:0.05,4:0.0,5:1.0,6:0.05,7:0.4,3:0.92}, 1:{0:0.18,1:180,2:0.50,4:0.50,3:0.85}, 2:{0:0.28,2:0.60,3:0.85}, 3:{0:0.025,3:0.85}, 4:{0:0.18,3:0.80}, 5:{0:0.30,1:75,5:0.0,3:0.80}, 6:{0:0.30,1:120,5:0.0,3:0.80}, 7:{0:0.30,1:175,5:0.0,3:0.80}, 14:{0:0.08,1:1.0,3:0.75}, 15:{0:0.85,3:0.70} } },
    ],
    1: [
        { name:'Classic 909', volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.40,1:50,3:0.80}, 1:{0:0.25,2:0.50,4:0.50,3:0.80}, 2:{0:0.30,3:0.80}, 3:{0:0.04,3:0.80}, 4:{0:0.30,3:0.80}, 5:{0:0.30,1:80,3:0.80}, 6:{0:0.30,1:120,3:0.80}, 7:{0:0.30,1:180,3:0.80}, 8:{0:0.50,3:0.80}, 9:{0:0.80,3:0.80}, 10:{3:0.80} } },
        { name:'Techno',      volumes:[1.2, 1.0, 0.5, 0.9, 0.8, 0.5, 0.5, 0.5, 0.7, 0.3, 0.4], instrumentValues:{ 0:{0:0.55,1:46,3:0.95}, 1:{0:0.20,2:0.68,4:0.72,3:0.84}, 2:{0:0.18,3:0.62}, 3:{0:0.05,3:0.88}, 4:{0:0.42,3:0.80}, 5:{0:0.22,1:76,3:0.60}, 6:{0:0.22,1:116,3:0.60}, 7:{0:0.20,1:170,3:0.60}, 8:{0:0.85,3:0.74}, 9:{0:0.50,3:0.56} } },
        { name:'House Pound', volumes:[1.1, 0.8, 1.0, 0.6, 0.7, 0.6, 0.6, 0.6, 0.8, 0.4, 0.5], instrumentValues:{ 0:{0:0.62,1:42,3:0.92}, 1:{0:0.22,2:0.42,4:0.40,3:0.70}, 2:{0:0.34,3:0.92}, 3:{0:0.032,3:0.66}, 4:{0:0.24,3:0.74}, 5:{0:0.34,1:78,3:0.68}, 6:{0:0.34,1:118,3:0.68}, 7:{0:0.32,1:176,3:0.68}, 8:{0:0.95,3:0.86}, 9:{0:0.58,3:0.62} } },
        { name:'Industrial',  volumes:[1.2, 1.1, 0.8, 1.0, 0.9, 0.7, 0.7, 0.7, 0.5, 0.8, 0.7], instrumentValues:{ 0:{0:0.70,1:58,3:1.00}, 1:{0:0.34,2:0.82,4:0.86,3:0.95}, 2:{0:0.40,3:0.88}, 3:{0:0.06,3:0.96}, 4:{0:0.52,3:0.90}, 5:{0:0.38,1:90,3:0.76}, 6:{0:0.38,1:136,3:0.76}, 7:{0:0.36,1:196,3:0.76}, 8:{0:1.40,3:0.66}, 9:{0:1.80,3:0.82}, 10:{3:0.76} } },
        { name:'Pure 909',    volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.40,1:50,3:0.92}, 1:{0:0.20,2:0.55,4:0.55,3:0.88}, 2:{0:0.28,3:0.82}, 3:{0:0.022,3:0.90}, 4:{0:0.18,3:0.85}, 5:{0:0.30,1:80,3:0.80}, 6:{0:0.30,1:120,3:0.80}, 7:{0:0.30,1:180,3:0.80}, 8:{0:0.55,3:0.78}, 9:{0:0.85,3:0.75}, 10:{3:0.80} } },
    ],
    2: [
        { name:'Classic 505', volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.40,1:55,3:0.80}, 1:{0:0.25,2:0.50,3:0.80}, 2:{0:0.30,3:0.80}, 3:{0:0.04,3:0.80}, 4:{0:0.30,3:0.80}, 5:{0:0.30,1:80,3:0.80}, 6:{0:0.30,1:120,3:0.80}, 7:{0:0.30,1:180,3:0.80}, 8:{0:0.10,3:0.80}, 9:{0:0.80,3:0.80}, 10:{3:0.80} } },
        { name:'New Wave',    volumes:[0.8, 0.9, 0.7, 0.9, 0.8, 0.6, 0.6, 0.7, 1.1, 0.6, 0.7], instrumentValues:{ 0:{0:0.24,1:68,3:0.72}, 1:{0:0.22,2:0.62,3:0.82}, 2:{0:0.22,3:0.66}, 3:{0:0.05,3:0.90}, 4:{0:0.24,3:0.82}, 8:{0:0.14,3:0.98}, 9:{0:0.42,3:0.62} } },
        { name:'Electro',     volumes:[1.2, 1.0, 0.6, 0.8, 0.7, 0.7, 0.7, 0.7, 0.5, 0.5, 0.6], instrumentValues:{ 0:{0:0.30,1:60,3:0.92}, 1:{0:0.18,2:0.70,3:0.84}, 2:{0:0.18,3:0.60}, 3:{0:0.05,3:0.80}, 4:{0:0.22,3:0.72}, 5:{0:0.24,1:92,3:0.72}, 6:{0:0.24,1:136,3:0.72}, 7:{0:0.22,1:196,3:0.72} } },
        { name:'Lo-Fi Hip-Hop', volumes:[1.0, 0.9, 0.7, 0.6, 0.6, 0.8, 0.8, 0.7, 0.4, 0.5, 0.5], instrumentValues:{ 0:{0:0.55,1:48,3:0.88}, 1:{0:0.32,2:0.32,3:0.74}, 2:{0:0.40,3:0.64}, 3:{0:0.03,3:0.58}, 4:{0:0.18,3:0.58}, 5:{0:0.36,1:74,3:0.78}, 6:{0:0.34,1:110,3:0.78}, 7:{0:0.30,1:168,3:0.74}, 8:{0:0.08,3:0.44}, 9:{0:1.10,3:0.50} } },
        { name:'Pure 505',    volumes:[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], instrumentValues:{ 0:{0:0.40,1:55,3:0.90}, 1:{0:0.25,2:0.55,3:0.88}, 2:{0:0.28,3:0.82}, 3:{0:0.025,3:0.92}, 4:{0:0.20,3:0.86}, 5:{0:0.30,1:80,3:0.80}, 6:{0:0.30,1:120,3:0.80}, 7:{0:0.30,1:180,3:0.80}, 8:{0:0.10,3:0.78}, 9:{0:0.80,3:0.74}, 10:{3:0.80} } },
    ],
    3: [
        { name:'Classic Acid', values:{ 0:1200.0, 1:0.72, 2:0.65, 3:0.35, 4:0.60, 5:0.09, 6:0.0, 7:0.80, 8:0.001, 9:0.0, 10:0.15, 11:0.12, 12:0.08, 13:0.04, 14:0.0 } },
        { name:'Resonant Squelch', values:{ 0:900.0, 1:0.92, 2:0.95, 3:0.45, 4:0.85, 5:0.12, 6:0.0, 7:0.85, 8:0.001, 9:0.0, 10:0.18, 11:0.28, 12:0.06, 13:0.08, 14:0.0 } },
        { name:'Sub Bass', values:{ 0:240.0, 1:0.45, 2:0.25, 3:0.60, 4:0.25, 5:0.06, 6:1.0, 7:0.90, 8:0.004, 9:0.45, 10:0.35, 11:0.18, 12:0.45, 13:0.02, 14:0.0 } },
        { name:'Soft Lead', values:{ 0:2200.0, 1:0.58, 2:0.40, 3:0.80, 4:0.35, 5:0.15, 6:1.0, 7:0.75, 8:0.010, 9:0.35, 10:0.40, 11:0.08, 12:0.18, 13:0.12, 14:0.0 } },
    ],
    4: [
        { name:'Classic Pad', values:{ 0:1.2, 1:30, 2:900, 3:0.75, 4:6500, 5:0.20, 6:0.15, 7:2 } },
        { name:'Glass Pluck', values:{ 0:2.7, 1:0, 2:260, 3:0.82, 4:4200, 5:5.20, 6:0.08, 7:1 } },
        { name:'Organ Motion', values:{ 0:6.0, 1:8, 2:1200, 3:0.78, 4:9000, 5:0.90, 6:0.30, 7:2 } },
        { name:'PWM Bass', values:{ 0:4.0, 1:0, 2:320, 3:0.85, 4:2400, 5:3.50, 6:0.12, 7:0 } },
    ],
    5: [
        { name:'Bass Punch', values:{ 0:0.0, 1:0.50, 2:0.72, 4:650.0, 5:0.25, 6:0.55, 7:0.001, 8:0.18, 9:0.0, 10:0.08, 11:0.001, 12:0.14, 13:0.10, 14:0.0, 15:0.0, 16:0.0, 17:0.05, 18:0.04, 19:0.85 } },
        { name:'Acid Lead', values:{ 0:0.0, 1:0.42, 2:0.20, 4:1800.0, 5:0.70, 6:0.75, 7:0.001, 8:0.35, 9:0.25, 10:0.18, 11:0.001, 12:0.25, 13:5.50, 14:0.18, 15:1.0, 16:0.0, 17:0.12, 18:0.07, 19:0.80 } },
        { name:'PWM Keys', values:{ 0:1.0, 1:0.28, 2:0.15, 4:2600.0, 5:0.35, 6:0.45, 7:0.010, 8:0.40, 9:0.55, 10:0.28, 11:0.010, 12:0.45, 13:3.20, 14:0.32, 15:0.0, 16:1.0, 17:0.0, 18:0.03, 19:0.78 } },
        { name:'Drone Pad', values:{ 0:2.0, 1:0.50, 2:0.35, 4:1200.0, 5:0.82, 6:0.60, 7:0.120, 8:1.20, 9:0.75, 10:1.00, 11:0.080, 12:1.60, 13:0.35, 14:0.40, 15:1.0, 16:0.0, 17:0.18, 18:0.15, 19:0.72 } },
    ],
    6: [
        { name:'FM Bass', values:{ 0:0.001, 1:0.30, 2:0.00, 3:0.12, 4:0.001, 5:0.22, 6:0.00, 7:0.15, 8:1.00, 9:5.50, 10:0.08, 11:0.0, 12:0.0, 13:0.40, 14:0.85 } },
        { name:'EPiano', values:{ 0:0.001, 1:1.40, 2:0.15, 3:1.10, 4:0.001, 5:0.90, 6:0.00, 7:0.60, 8:2.00, 9:3.20, 10:0.05, 11:1.0, 12:0.8, 13:0.75, 14:0.80 } },
        { name:'Bell', values:{ 0:0.001, 1:2.60, 2:0.00, 3:1.80, 4:0.001, 5:1.40, 6:0.00, 7:1.00, 8:3.00, 9:8.50, 10:0.12, 11:0.0, 12:1.5, 13:0.85, 14:0.75 } },
        { name:'Growl Lead', values:{ 0:0.005, 1:0.50, 2:0.35, 3:0.25, 4:0.001, 5:0.40, 6:0.20, 7:0.30, 8:1.50, 9:10.50, 10:0.50, 11:2.0, 12:7.0, 13:0.60, 14:0.82 } },
    ],
};

function ensureSynthPresetCss() {
    if (document.getElementById('synth-preset-css')) return;
    const style = document.createElement('style');
    style.id = 'synth-preset-css';
    style.textContent = `
        .synth-preset-section { display:flex; flex-direction:column; gap:8px; margin:0 0 12px; }
        .synth-preset-header { display:flex; justify-content:space-between; align-items:center; gap:10px; }
        .synth-preset-title { font-size:11px; font-weight:700; letter-spacing:1.2px; text-transform:uppercase; color:#9aa4b3; }
        .synth-preset-subtitle { font-size:10px; color:#6c7684; }
        .synth-preset-grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(86px, 1fr)); gap:8px; }
        .synth-preset-chip { border:1px solid rgba(255,255,255,0.12); border-radius:10px; background:linear-gradient(180deg, rgba(255,255,255,0.05), rgba(255,255,255,0.02)); color:#e9eef6; padding:10px 8px; font-size:11px; font-weight:700; cursor:pointer; transition:transform .12s ease, border-color .12s ease, box-shadow .12s ease; }
        .synth-preset-chip:hover { transform:translateY(-1px); border-color:rgba(255,255,255,0.28); }
        .synth-preset-chip.active { box-shadow:0 0 0 1px currentColor inset, 0 0 18px rgba(255,255,255,0.08); }
    `;
    document.head.appendChild(style);
}

function getSynthFactoryPresets(engine) {
    return SYNTH_FACTORY_PRESETS[engine] || [];
}

function getPadsUsingEngine(engine, fallbackPadIndex) {
    const pads = [];
    if (typeof padSynthEngine !== 'undefined') {
        for (let pad = 0; pad < 16; pad++) {
            if (padSynthEngine[pad] === engine) pads.push(pad);
        }
    }
    if (!pads.length && fallbackPadIndex >= 0) pads.push(fallbackPadIndex);
    return pads;
}

function captureSynthEngineSnapshot(engine, fallbackPadIndex) {
    const entries = [];
    getPadsUsingEngine(engine, fallbackPadIndex).forEach((pad) => {
        getParamsForPad(engine, pad).forEach((param) => {
            entries.push({
                pad,
                engine,
                paramId: param.paramId,
                value: getSynthParamValue(pad, engine, param.paramId),
            });
        });
    });
    return entries;
}

function sendSynthParamWs(padIndex, engine, paramId, value) {
    if (typeof ws === 'undefined' || !ws || ws.readyState !== WebSocket.OPEN) return;
    if (engine === 3) {
        ws.send(JSON.stringify({ cmd: 'synth303Param', paramId, value }));
        return;
    }
    const instrument = (engine === 4) ? padIndex : padToInstrument(engine, padIndex);
    ws.send(JSON.stringify({ cmd: 'synthParam', engine, instrument, paramId, value }));
}

function applySynthPresetLocally(engine, presetIndex, sourcePadIndex) {
    const preset = getSynthFactoryPresets(engine)[presetIndex];
    if (!preset) return;

    const pads = getPadsUsingEngine(engine, sourcePadIndex);
    pads.forEach((pad) => {
        if (engine <= 2 && Array.isArray(preset.volumes)) {
            const instrument = padToInstrument(engine, pad);
            const volume = preset.volumes[instrument];
            if (volume !== undefined) {
                setSynthParamValue(pad, engine, 3, volume);
            }
            const instrumentValues = (preset.instrumentValues && preset.instrumentValues[instrument]) || null;
            Object.entries(instrumentValues || {}).forEach(([paramId, value]) => {
                setSynthParamValue(pad, engine, Number(paramId), value);
            });
        }
        Object.entries(preset.values || {}).forEach(([paramId, value]) => {
            setSynthParamValue(pad, engine, Number(paramId), value);
        });
        refreshPadWaveform(pad);
    });
    refreshModalWaveform();
}

function sendSynthPresetCommand(engine, presetIndex) {
    if (typeof ws === 'undefined' || !ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ cmd: 'synthPreset', engine, preset: presetIndex }));
}

function renderSynthPresetSection(engine) {
    const presets = getSynthFactoryPresets(engine);
    if (!presets.length) return '';
    return `
        <div class="synth-preset-section">
            <div class="synth-preset-header">
                <div class="synth-preset-title">Factory Presets</div>
                <div class="synth-preset-subtitle">4 snapshots por motor</div>
            </div>
            <div class="synth-preset-grid">
                ${presets.map((preset, index) => `<button class="synth-preset-chip" data-preset-index="${index}">${preset.name}</button>`).join('')}
            </div>
        </div>
    `;
}

function refreshGenericSynthModalControls(engine, padIndex) {
    const modal = document.getElementById('synthParamModal');
    if (!modal) return;
    getParamsForPad(engine, padIndex).forEach((param) => {
        const value = getSynthParamValue(padIndex, engine, param.paramId);
        if (param.labels) {
            const group = modal.querySelector(`.synth-param-toggle-group[data-param-id="${param.paramId}"]`);
            if (!group) return;
            group.querySelectorAll('.synth-param-toggle-btn').forEach((btn) => {
                btn.classList.toggle('active', parseFloat(btn.dataset.value) === value);
            });
            return;
        }
        const slider = modal.querySelector(`.synth-param-slider[data-param-id="${param.paramId}"]`);
        const span = modal.querySelector(`.synth-param-value[data-param-id="${param.paramId}"]`);
        if (slider) slider.value = value;
        if (span) span.textContent = formatParamValue(value, param.unit);
    });
}

function refreshWtModalControls(padIndex) {
    const wavePos   = getSynthParamValue(padIndex, 4, 0);
    const attackMs  = getSynthParamValue(padIndex, 4, 1);
    const decayMs   = getSynthParamValue(padIndex, 4, 2);
    const volume    = getSynthParamValue(padIndex, 4, 3);
    const filterCut = getSynthParamValue(padIndex, 4, 4);
    const lfoRate   = getSynthParamValue(padIndex, 4, 5);
    const lfoDepth  = getSynthParamValue(padIndex, 4, 6);
    const lfoTarget = getSynthParamValue(padIndex, 4, 7);

    const morphSlider = document.getElementById('wtMorphSlider');
    const morphLabel = document.getElementById('wtMorphLabel');
    const attackLbl = document.getElementById('wtAttackLbl');
    const decayLbl = document.getElementById('wtDecayLbl');
    const filterSlider = document.getElementById('wtFilterSlider');
    const filterLbl = document.getElementById('wtFilterLbl');
    const volFill = document.getElementById('wtVolFill');
    const volumeLbl = document.getElementById('wtVolumeLbl');
    const lfoRateSlider = document.getElementById('wtLfoRate');
    const lfoRateLbl = document.getElementById('wtLfoRateLbl');
    const lfoDepthSlider = document.getElementById('wtLfoDepth');
    const lfoDepthLbl = document.getElementById('wtLfoDepthLbl');

    if (morphSlider) morphSlider.value = wavePos;
    if (morphLabel) morphLabel.textContent = `▸ ${wavePos.toFixed(2)}`;
    if (attackLbl) attackLbl.textContent = Math.round(attackMs);
    if (decayLbl) decayLbl.textContent = Math.round(decayMs);
    if (filterSlider) filterSlider.value = filterCut;
    if (filterLbl) filterLbl.textContent = filterCut >= 1000 ? (filterCut/1000).toFixed(1)+'kHz' : Math.round(filterCut)+'Hz';
    if (volFill) volFill.style.height = `${Math.round(volume * 100)}%`;
    if (volumeLbl) volumeLbl.textContent = `${Math.round(volume * 100)}%`;
    if (lfoRateSlider) lfoRateSlider.value = lfoRate;
    if (lfoRateLbl) lfoRateLbl.textContent = `${lfoRate.toFixed(1)}Hz`;
    if (lfoDepthSlider) lfoDepthSlider.value = lfoDepth;
    if (lfoDepthLbl) lfoDepthLbl.textContent = `${Math.round(lfoDepth * 100)}%`;

    document.querySelectorAll('.wt-wave-cell').forEach((cell) => {
        const selected = Math.round(wavePos) === parseInt(cell.dataset.w, 10);
        cell.classList.toggle('active', selected);
        const canvas = document.getElementById(`wtCell${cell.dataset.w}`);
        if (canvas) _wtDrawCell(canvas, parseInt(cell.dataset.w, 10), selected ? WT_COLOR : '#555');
    });
    document.querySelectorAll('.wt-lfo-target-btn').forEach((btn) => {
        btn.classList.toggle('active', parseInt(btn.dataset.t, 10) === lfoTarget);
    });
    _wtDrawMorph(wavePos);
    _wtDrawAdsr(attackMs, decayMs);
    _wtDrawFilter(filterCut);
    _wtDrawLfo(lfoRate, lfoDepth);
}

function refreshActiveSynthModalControls(engine, padIndex) {
    if (engine === 4) refreshWtModalControls(padIndex);
    else refreshGenericSynthModalControls(engine, padIndex);
    refreshModalWaveform();
}

function attachSynthPresetHandlers(modal, engine, padIndex) {
    const buttons = modal.querySelectorAll('[data-preset-index]');
    if (!buttons.length) return;
    buttons.forEach((button) => {
        button.addEventListener('click', () => {
            const presetIndex = parseInt(button.dataset.presetIndex, 10);
            sendSynthPresetCommand(engine, presetIndex);
            applySynthPresetLocally(engine, presetIndex, padIndex);
            refreshActiveSynthModalControls(engine, padIndex);
            buttons.forEach((b) => b.classList.remove('active'));
            button.classList.add('active');
            scheduleAutoPreview(padIndex, engine);
        });
    });
}

function getParamsForPad(engine, padIndex) {
    if (engine === 4) return PARAMS_WTOSC;
    if (engine === 5) return PARAMS_SH101;  /* I1 */
    if (engine === 6) return PARAMS_FM2OP;  /* I2 */
    if (engine === 3) return PARAMS_303;
    const inst = padToInstrument(engine, padIndex);
    if (engine === 0) return PARAMS_808[inst] || [ P(3,'Volume',0,1,0.01,0.8) ];
    if (engine === 1) return PARAMS_909[inst] || [ P(3,'Volume',0,1,0.01,0.8) ];
    if (engine === 2) return PARAMS_505[inst] || [ P(3,'Volume',0,1,0.01,0.8) ];
    return [];
}

// ============================================================
//  CURRENT PARAM STATE (tracks live values per pad)
// ============================================================
const synthParamValues = new Array(16).fill(null).map(() => ({}));
// synthParamValues[padIdx][paramId] = currentValue

function getSynthParamValue(padIndex, engine, paramId) {
    const key = `${engine}_${paramId}`;
    if (synthParamValues[padIndex] && synthParamValues[padIndex][key] !== undefined) {
        return synthParamValues[padIndex][key];
    }
    // Return default
    const params = getParamsForPad(engine, padIndex);
    const p = params.find(pp => pp.paramId === paramId);
    return p ? p.default : 0;
}

function setSynthParamValue(padIndex, engine, paramId, value) {
    const key = `${engine}_${paramId}`;
    if (!synthParamValues[padIndex]) synthParamValues[padIndex] = {};
    synthParamValues[padIndex][key] = value;
}

// ============================================================
//  SYNTH WAVEFORM DRAWING INSIDE PAD
// ============================================================
const ENGINE_COLORS = {
    0: '#c0392b',  // 808 red
    1: '#0076a8',  // 909 blue
    2: '#1e8449',  // 505 green
    3: '#7d3c98',  // 303 violet
    4: '#e67e22',  // WTOSC orange
    5: '#27ae60',  // SH-101 teal green  (I1)
    6: '#e74c3c',  // FM 2-Op coral red  (I2)
};

function drawSynthWaveformInPad(canvas, engine, padIndex) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const color = ENGINE_COLORS[engine] || '#fff';
    const inst = (engine < 3) ? padToInstrument(engine, padIndex) : -1;

    ctx.clearRect(0, 0, w, h);

    // Dark bg with subtle grid
    ctx.fillStyle = 'rgba(0,0,0,0.6)';
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 0.5;
    for (let x = 0; x < w; x += w/8) {
        ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke();
    }
    // Center line
    ctx.strokeStyle = 'rgba(255,255,255,0.1)';
    ctx.beginPath(); ctx.moveTo(0,h/2); ctx.lineTo(w,h/2); ctx.stroke();

    // Draw waveform
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.shadowColor = color;
    ctx.shadowBlur = 4;

    const mid = h / 2;
    const amp = h * 0.38;

    if (engine === 3) {
        // TB-303: saw or square wave
        const wf = getSynthParamValue(padIndex, 3, 6); // waveform param
        const cutoff = getSynthParamValue(padIndex, 3, 0);
        const cutoffNorm = Math.min(1, cutoff / 5000);
        const cycles = 3 + Math.floor(cutoffNorm * 3);
        if (wf < 0.5) {
            // Sawtooth
            for (let x = 0; x < w; x++) {
                const phase = (x / w) * cycles;
                const frac = phase - Math.floor(phase);
                const y = mid - amp * (2 * frac - 1) * Math.exp(-x / w * 0.5);
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else {
            // Square
            for (let x = 0; x < w; x++) {
                const phase = (x / w) * cycles;
                const frac = phase - Math.floor(phase);
                const y = mid - amp * (frac < 0.5 ? 1 : -1) * Math.exp(-x / w * 0.5);
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        }
    } else if (engine === 5) {
        // SH-101: filtered sawtooth / square / pulse with VCF envelope sweep
        const wf      = getSynthParamValue(padIndex, 5, 0) | 0;  // 0=SAW,1=SQR,2=PUL
        const cutoff  = getSynthParamValue(padIndex, 5, 4) || 2000;
        const lpFrac  = Math.min(1, cutoff / 9000);              // visualize LP cutoff
        const cycles  = 2.5;
        const subLvl  = getSynthParamValue(padIndex, 5, 2) || 0.3;
        const vcaDec  = getSynthParamValue(padIndex, 5, 8) || 0.3;
        const vcfDec  = getSynthParamValue(padIndex, 5, 12) || 0.2;
        for (let x = 0; x < w; x++) {
            const t     = x / w;
            const phase = (t * cycles) % 1;
            const vcaEnv = Math.exp(-t / (vcaDec * 0.5 + 0.05));
            const cutEff = lpFrac * (0.3 + 0.7 * Math.exp(-t / (vcfDec * 0.3 + 0.03)));
            let sig;
            if (wf === 0)      sig = (2 * phase - 1);       // SAW
            else if (wf === 1) sig = (phase < 0.5) ? 1 : -1; // SQR
            else               sig = (phase < 0.35) ? 1 : -1; // PUL (35% duty)
            const sub = Math.sin(t * cycles * Math.PI) * subLvl;
            const filt = sig * cutEff + sub * 0.4;
            const y = mid - amp * filt * vcaEnv;
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else if (engine === 6) {
        // FM 2-Op: sinusoidal spectrum with modulation envelope
        const ratio   = getSynthParamValue(padIndex, 6, 8) || 2;
        const index   = getSynthParamValue(padIndex, 6, 9) || 3;
        const cDec    = getSynthParamValue(padIndex, 6, 1) || 0.4;
        const mDec    = getSynthParamValue(padIndex, 6, 5) || 0.25;
        const cycles  = 3;
        for (let x = 0; x < w; x++) {
            const t    = x / w;
            const cEnv = Math.exp(-t / (cDec * 0.5 + 0.05));
            const mEnv = Math.exp(-t / (mDec * 0.4 + 0.02));
            const phase = t * cycles * Math.PI * 2;
            const mod   = index * mEnv * Math.sin(phase * ratio);
            const sig   = Math.sin(phase + mod);
            const y = mid - amp * sig * cEnv;
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else {
        // Percussion waveform based on instrument type
        const decay = getSynthParamValue(padIndex, engine, 0) || 0.3;
        const isKick = (inst === 0);
        const isSnare = (inst === 1);
        const isHat = (inst === 3 || inst === 4);
        const isTom = (inst >= 5 && inst <= 7);

        if (isKick) {
            // Kick: decaying sine with pitch drop
            const pitch = getSynthParamValue(padIndex, engine, 1) || 55;
            const freq = pitch / 20;
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.5 + 0.05));
                const freqSweep = freq + (freq * 2) * Math.exp(-t * 15);
                const y = mid - amp * Math.sin(t * freqSweep * Math.PI * 2) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isSnare) {
            // Snare: sine + noise
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.03));
                const tone = Math.sin(t * 32 * Math.PI) * 0.6;
                const noise = (Math.random() - 0.5) * 0.8;
                const y = mid - amp * (tone + noise) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isHat) {
            // HiHat: noise burst
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.01));
                const noise = (Math.random() - 0.5) * 1.4;
                const y = mid - amp * noise * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isTom) {
            // Tom: decaying sine
            const pitch = getSynthParamValue(padIndex, engine, 1) || 120;
            const freq = pitch / 30;
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.4 + 0.04));
                const y = mid - amp * Math.sin(t * freq * Math.PI * 2) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else {
            // Generic percussion: short attack + decay
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.02));
                const sig = Math.sin(t * 20 * Math.PI) * 0.5 + (Math.random()-0.5)*0.5;
                const y = mid - amp * sig * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        }
    }
    ctx.stroke();
    ctx.shadowBlur = 0;

    // Glow fill underneath
    ctx.globalAlpha = 0.08;
    ctx.lineTo(w, mid);
    ctx.lineTo(0, mid);
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.fill();
    ctx.globalAlpha = 1.0;
}

// ============================================================
//  PAD SYNTH OVERLAY (waveform + param buttons)
// ============================================================
function createSynthOverlay(padIndex) {
    const pad = document.querySelector(`.pad[data-pad="${padIndex}"]`);
    if (!pad) return;

    // Remove existing overlay
    const existing = pad.querySelector('.synth-overlay');
    if (existing) existing.remove();

    const engine = (typeof padSynthEngine !== 'undefined') ? padSynthEngine[padIndex] : -1;
    if (engine < 0) return; // No synth mode

    const overlay = document.createElement('div');
    overlay.className = 'synth-overlay';

    // Waveform canvas — fill the pad as background layer
    const canvas = document.createElement('canvas');
    canvas.className = 'synth-pad-waveform';
    canvas.width = 280;
    canvas.height = 160;
    overlay.appendChild(canvas);
    pad.appendChild(overlay);

    // Params button — fourth action slot, away from the engine selector
    const existingBtn = pad.querySelector('.synth-params-btn');
    if (existingBtn) existingBtn.remove();
    const paramsBtn = document.createElement('button');
    paramsBtn.className = 'synth-params-btn' + (engine === 4 ? ' synth-params-btn--wt' : '');
    paramsBtn.textContent = 'SYN';
    paramsBtn.title = 'Editar parámetros del sintetizador';
    paramsBtn.setAttribute('aria-label', `Editar parámetros de sintetizador del pad ${padIndex + 1}`);
    const stopProp = (e) => { e.stopPropagation(); };
    paramsBtn.addEventListener('touchstart', stopProp);
    paramsBtn.addEventListener('mousedown', stopProp);
    paramsBtn.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopPropagation();
        openSynthModal(padIndex);
    });
    paramsBtn.addEventListener('touchend', (e) => {
        e.preventDefault();
        e.stopPropagation();
        openSynthModal(padIndex);
    });
    (pad.querySelector('.pad-action-bar') || pad).appendChild(paramsBtn);

    // Draw initial waveform
    drawSynthWaveformInPad(canvas, engine, padIndex);
}

function removeSynthOverlay(padIndex) {
    const pad = document.querySelector(`.pad[data-pad="${padIndex}"]`);
    if (!pad) return;
    const ov = pad.querySelector('.synth-overlay');
    if (ov) ov.remove();
    const btn = pad.querySelector('.synth-params-btn');
    if (btn) btn.remove();
}

function refreshSynthOverlay(padIndex) {
    const engine = (typeof padSynthEngine !== 'undefined') ? padSynthEngine[padIndex] : -1;
    if (engine >= 0) {
        createSynthOverlay(padIndex);
    } else {
        removeSynthOverlay(padIndex);
    }
}

// Refresh all pads
function refreshAllSynthOverlays() {
    for (let i = 0; i < 16; i++) refreshSynthOverlay(i);
}

// ============================================================
//  SYNTH PARAMETER MODAL
// ============================================================
let synthModalPad = -1;
let synthModalEngine = -1;
let synthModalSnapshot = [];   // Copia de valores al abrir — para Cancelar
let synthAutoPreviewTimer = null;

// Programa un re-trigger automático ~200ms después de mover un slider
// para que el usuario oiga el cambio sin pulsar TEST manualmente.
function scheduleAutoPreview(padIndex, engine) {
    if (synthAutoPreviewTimer) clearTimeout(synthAutoPreviewTimer);
    synthAutoPreviewTimer = setTimeout(() => {
        synthAutoPreviewTimer = null;
        triggerSynthPreview(padIndex, engine);
    }, 200);
}

// ============================================================
//  WAVETABLE OSC — MODAL VISUAL EDITOR
// ============================================================
const WT_WAVE_NAMES = ['Sine','Triangle','Saw','Square','Pulse25','SoftSin','Organ','SoftSqr'];
const WT_COLOR = '#e67e22';

function _injectWtModalCss() {
    if (document.getElementById('wt-modal-css')) return;
    const s = document.createElement('style');
    s.id = 'wt-modal-css';
    s.textContent = `
 .wt-osc-modal { max-width:1040px; width:min(96vw, 1040px); display:flex; flex-direction:column; gap:14px; }
.wt-section-label { font-size:10px; font-weight:700; letter-spacing:2px; color:#e67e22aa; text-transform:uppercase; margin-bottom:2px; }
.wt-wave-grid { display:grid; grid-template-columns:repeat(8,1fr); gap:5px; }
.wt-wave-cell { display:flex; flex-direction:column; align-items:center; gap:2px; cursor:pointer; border-radius:6px; padding:4px 2px; border:1.5px solid #333; background:#111; transition:all .15s; }
.wt-wave-cell canvas { border-radius:4px; display:block; }
.wt-wave-cell span { font-size:9px; color:#888; white-space:nowrap; }
.wt-wave-cell.active { border-color:${WT_COLOR}; background:#1a1208; }
.wt-wave-cell.active span { color:${WT_COLOR}; }
.wt-wave-cell:hover { border-color:#666; }
.wt-morph-wrap { position:relative; }
.wt-morph-wrap input[type=range] { width:100%; margin-top:5px; accent-color:${WT_COLOR}; }
.wt-morph-label-row { display:flex; justify-content:space-between; font-size:10px; color:#888; margin-top:2px; }
.wt-adsr-wrap { position:relative; }
.wt-adsr-labels { display:flex; gap:20px; font-size:11px; color:#aaa; margin-top:4px; }
.wt-adsr-labels span { display:flex; align-items:center; gap:4px; }
.wt-adsr-labels b { color:${WT_COLOR}; }
.wt-params-grid { display:grid; grid-template-columns:1fr auto 1.8fr; gap:12px; align-items:start; }
.wt-param-block { display:flex; flex-direction:column; gap:4px; }
.wt-param-block label { font-size:10px; letter-spacing:1px; text-transform:uppercase; color:#aaa; }
.wt-param-block span { font-size:11px; color:${WT_COLOR}; text-align:center; }
.wt-param-block input[type=range] { width:100%; accent-color:${WT_COLOR}; }
.wt-volume-block { display:flex; flex-direction:column; align-items:center; gap:4px; }
.wt-volume-block label { font-size:10px; text-transform:uppercase; letter-spacing:1px; color:#aaa; }
.wt-vol-track { position:relative; width:28px; height:90px; background:#1a1a1a; border-radius:6px; overflow:hidden; cursor:pointer; }
.wt-vol-fill { position:absolute; bottom:0; left:0; right:0; background:${WT_COLOR}; border-radius:6px; transition:height .1s; }
.wt-lfo-block { background:#111; border-radius:8px; padding:8px; }
.wt-lfo-row { display:flex; align-items:center; gap:6px; font-size:10px; color:#aaa; }
.wt-lfo-row span:first-child { width:36px; }
.wt-lfo-row span:last-child { width:36px; text-align:right; color:${WT_COLOR}; font-size:11px; }
.wt-lfo-target { display:flex; align-items:center; gap:4px; margin-top:6px; font-size:10px; color:#888; flex-wrap:wrap; }
.wt-lfo-target-btn { padding:2px 8px; border-radius:4px; border:1px solid #444; background:#1a1a1a; color:#888; cursor:pointer; font-size:10px; }
.wt-lfo-target-btn.active { border-color:${WT_COLOR}; color:${WT_COLOR}; background:#1a1008; }
.wt-note-row { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
.wt-note-btn { padding:3px 8px; border-radius:4px; border:1px solid #333; background:#111; color:#888; cursor:pointer; font-size:10px; }
.wt-note-btn.active { border-color:${WT_COLOR}; color:${WT_COLOR}; }
@media (max-width: 900px) { .wt-wave-grid { grid-template-columns:repeat(4,1fr); } .wt-params-grid { grid-template-columns:1fr; } }
`;
    document.head.appendChild(s);
}

function openWtOscModal(padIndex) {
    _injectWtModalCss();
    ensureSynthPresetCss();
    const engine = 4;
    synthModalPad = padIndex;
    synthModalEngine = engine;

    // Read current values
    const wavePos   = getSynthParamValue(padIndex, engine, 0);
    const attackMs  = getSynthParamValue(padIndex, engine, 1);
    const decayMs   = getSynthParamValue(padIndex, engine, 2);
    const volume    = getSynthParamValue(padIndex, engine, 3);
    const filterCut = getSynthParamValue(padIndex, engine, 4);
    const lfoRate   = getSynthParamValue(padIndex, engine, 5);
    const lfoDepth  = getSynthParamValue(padIndex, engine, 6);
    const lfoTarget = getSynthParamValue(padIndex, engine, 7);

    // Snapshot for cancel
    synthModalSnapshot = captureSynthEngineSnapshot(engine, padIndex);

    const padName  = (typeof padNames !== 'undefined') ? padNames[padIndex] : `Pad ${padIndex+1}`;
    const midiNote = (typeof trackWtNoteJs !== 'undefined') ? trackWtNoteJs[padIndex] : 60;
    const noteNames = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

    let modal = document.getElementById('synthParamModal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'synthParamModal';
        modal.className = 'synth-modal-overlay';
        document.body.appendChild(modal);
    }

    const waveGridHtml = WT_WAVE_NAMES.map((n, i) => `
        <div class="wt-wave-cell ${Math.round(wavePos) === i ? 'active' : ''}" data-w="${i}">
            <canvas id="wtCell${i}" width="62" height="36"></canvas>
            <span>${n}</span>
        </div>`).join('');

    // Note buttons C3..B4
    const noteMin = 48, noteMax = 71;
    const noteBtns = Array.from({length: noteMax - noteMin + 1}, (_, i) => {
        const n = noteMin + i;
        const oct = Math.floor(n / 12) - 1;
        const nm  = noteNames[n % 12];
        const isSharp = nm.includes('#');
        return `<button class="wt-note-btn ${isSharp ? 'sharp' : ''} ${n === midiNote ? 'active' : ''}" data-note="${n}">${nm}${oct}</button>`;
    }).join('');

    modal.innerHTML = `
<div class="synth-modal wt-osc-modal" data-engine="4">
  <div class="synth-modal-header">
    <div class="synth-modal-title">
      <span class="synth-modal-engine-badge" style="background:${WT_COLOR}">WTOSC</span>
      <span class="synth-modal-inst-name">${padName} — Wavetable OSC</span>
    </div>
    <button class="synth-modal-close-btn" id="synthModalClose">✕</button>
  </div>

    ${renderSynthPresetSection(engine)}

  <div class="wt-section-label">Wave Shape</div>
  <div class="wt-wave-grid" id="wtWaveGrid">${waveGridHtml}</div>

  <div class="wt-section-label">Morph</div>
  <div class="wt-morph-wrap">
    <canvas id="wtMorphDisplay" width="600" height="90" style="width:100%;height:90px;display:block;border-radius:6px;"></canvas>
    <input type="range" id="wtMorphSlider" min="0" max="7" step="0.01" value="${wavePos}">
    <div class="wt-morph-label-row">
      <span>${WT_WAVE_NAMES[Math.max(0, Math.floor(wavePos))]} ──</span>
      <span id="wtMorphLabel">▸ ${wavePos.toFixed(2)}</span>
      <span>── ${WT_WAVE_NAMES[Math.min(7, Math.ceil(wavePos))]}</span>
    </div>
  </div>

  <div class="wt-section-label">Envelope</div>
  <div class="wt-adsr-wrap">
    <canvas id="wtAdsrCanvas" width="600" height="95" style="width:100%;height:95px;cursor:crosshair;display:block;border-radius:6px;"></canvas>
    <div class="wt-adsr-labels">
      <span>Attack: <b id="wtAttackLbl">${Math.round(attackMs)}</b> ms</span>
      <span>Decay: <b id="wtDecayLbl">${Math.round(decayMs)}</b> ms</span>
      <span style="margin-left:auto;font-size:10px;color:#555">← drag nodes</span>
    </div>
  </div>

  <div style="display:grid;grid-template-columns:1fr auto 1.8fr;gap:12px;align-items:start;margin-top:2px;">
    <!-- Filter block -->
    <div class="wt-param-block">
      <label class="wt-section-label" style="margin-bottom:4px">Filter</label>
      <canvas id="wtFilterCanvas" width="180" height="56" style="width:100%;height:56px;display:block;border-radius:6px;"></canvas>
      <input type="range" id="wtFilterSlider" min="100" max="18000" step="10" value="${filterCut}">
      <span id="wtFilterLbl">${filterCut >= 1000 ? (filterCut/1000).toFixed(1)+'kHz' : Math.round(filterCut)+'Hz'}</span>
    </div>
    <!-- Volume block -->
    <div class="wt-volume-block">
      <label class="wt-section-label">Vol</label>
      <div class="wt-vol-track" id="wtVolTrack" style="cursor:pointer;">
        <div class="wt-vol-fill" id="wtVolFill" style="height:${Math.round(volume*100)}%"></div>
      </div>
      <span id="wtVolumeLbl">${Math.round(volume*100)}%</span>
    </div>
    <!-- LFO block -->
    <div class="wt-lfo-block wt-param-block">
      <label class="wt-section-label" style="margin-bottom:4px">LFO</label>
      <div class="wt-lfo-row">
        <span>Rate</span>
        <input type="range" id="wtLfoRate" min="0.01" max="20" step="0.01" value="${lfoRate}" style="flex:1">
        <span id="wtLfoRateLbl">${lfoRate.toFixed(1)}Hz</span>
      </div>
      <div class="wt-lfo-row" style="margin-top:4px;">
        <span>Depth</span>
        <input type="range" id="wtLfoDepth" min="0" max="1" step="0.01" value="${lfoDepth}" style="flex:1">
        <span id="wtLfoDepthLbl">${Math.round(lfoDepth*100)}%</span>
      </div>
      <div class="wt-lfo-target">
        Target:
        ${['Wave','Pitch','Vol'].map((t,i)=>`<button class="wt-lfo-target-btn${lfoTarget===i?' active':''}" data-t="${i}">${t}</button>`).join('')}
      </div>
      <canvas id="wtLfoCanvas" width="160" height="36" style="width:100%;height:36px;display:block;border-radius:4px;margin-top:6px;"></canvas>
    </div>
  </div>

  <div class="wt-section-label" style="margin-top:6px;">Note (MIDI)</div>
  <div class="wt-note-row" id="wtNoteRow">${noteBtns}</div>

  <div class="synth-modal-footer">
    <button class="synth-modal-cancel-btn" id="synthModalCancel">✖ Cancelar</button>
    <button class="synth-modal-trigger-btn" id="synthModalTrigger">▶ Test</button>
    <button class="synth-modal-save-btn" id="synthModalSave">✔ Guardar</button>
  </div>
</div>`;

    modal.classList.add('active');
    attachSynthPresetHandlers(modal, engine, padIndex);

    // ── Draw all canvases ──
    for (let i = 0; i < 8; i++) {
        const c = document.getElementById(`wtCell${i}`);
        if (c) _wtDrawCell(c, i, Math.round(wavePos) === i ? WT_COLOR : '#555');
    }
    _wtDrawMorph(wavePos);
    _wtDrawAdsr(attackMs, decayMs);
    _wtDrawFilter(filterCut);
    _wtDrawLfo(lfoRate, lfoDepth);

    // ── State for ADSR drag ──
    let curAttack = attackMs, curDecay = decayMs;

    // ── Helper: send parameter ──
    function sendP(pid, val) {
        setSynthParamValue(padIndex, engine, pid, val);
        if (typeof sendWebSocket === 'function')
            sendWebSocket({ cmd:'synthParam', engine:4, instrument:padIndex, paramId:pid, value:val });
    }

    // ── Wave cell clicks ──
    modal.querySelectorAll('.wt-wave-cell').forEach(cell => {
        cell.addEventListener('click', () => {
            const w = parseInt(cell.dataset.w);
            document.getElementById('wtMorphSlider').value = w;
            _applyMorph(w);
        });
    });

    // ── Morph slider ──
    document.getElementById('wtMorphSlider').addEventListener('input', e => {
        _applyMorph(parseFloat(e.target.value));
    });

    function _applyMorph(val) {
        const lo = WT_WAVE_NAMES[Math.max(0,Math.floor(val))];
        const hi = WT_WAVE_NAMES[Math.min(7,Math.ceil(val))];
        document.getElementById('wtMorphLabel').textContent = `▸ ${val.toFixed(2)}`;
        modal.querySelector('.wt-morph-label-row').children[0].textContent = `${lo} ──`;
        modal.querySelector('.wt-morph-label-row').children[2].textContent = `── ${hi}`;
        modal.querySelectorAll('.wt-wave-cell').forEach(c => {
            const w = parseInt(c.dataset.w);
            const sel = Math.round(val) === w;
            c.classList.toggle('active', sel);
            const cv = document.getElementById(`wtCell${w}`);
            if (cv) _wtDrawCell(cv, w, sel ? WT_COLOR : '#555');
        });
        _wtDrawMorph(val);
        sendP(0, val);
    }

    // ── ADSR drag ──
    let draggingNode = null;
    const aCanvas = document.getElementById('wtAdsrCanvas');

    function _xyToMs(clientX) {
        const rect = aCanvas.getBoundingClientRect();
        const sx = aCanvas.width / rect.width;
        const cx = (clientX - rect.left) * sx;
        const pL = 24, pR = 16;
        const innerW = aCanvas.width - pL - pR;
        const totalMs = _adsrTotalMs(curAttack, curDecay);
        return ((cx - pL) / innerW) * totalMs;
    }

    function _adsrPointerDown(clientX) {
        const rect = aCanvas.getBoundingClientRect();
        const sx = aCanvas.width / rect.width;
        const cx = (clientX - rect.left) * sx;
        const pL = 24, pR = 16;
        const innerW = aCanvas.width - pL - pR;
        const totalMs = _adsrTotalMs(curAttack, curDecay);
        const xA = pL + (curAttack / totalMs) * innerW;
        const xD = pL + ((curAttack + curDecay) / totalMs) * innerW;
        if (Math.abs(cx - xA) < 14) draggingNode = 'A';
        else if (Math.abs(cx - xD) < 14) draggingNode = 'D';
    }

    function _adsrPointerMove(clientX) {
        if (!draggingNode) return;
        const ms = _xyToMs(clientX);
        if (draggingNode === 'A') {
            curAttack = Math.max(1, Math.min(2000, ms));
            document.getElementById('wtAttackLbl').textContent = Math.round(curAttack);
            sendP(1, curAttack);
        } else {
            curDecay = Math.max(1, Math.min(4000, ms - curAttack));
            document.getElementById('wtDecayLbl').textContent = Math.round(curDecay);
            sendP(2, curDecay);
        }
        _wtDrawAdsr(curAttack, curDecay);
    }

    aCanvas.addEventListener('mousedown',  e => _adsrPointerDown(e.clientX));
    window.addEventListener('mousemove',   e => _adsrPointerMove(e.clientX));
    window.addEventListener('mouseup',     () => { draggingNode = null; });
    aCanvas.addEventListener('touchstart', e => { e.preventDefault(); _adsrPointerDown(e.touches[0].clientX); }, {passive:false});
    window.addEventListener('touchmove',   e => { if (draggingNode) { e.preventDefault(); _adsrPointerMove(e.touches[0].clientX); } }, {passive:false});
    window.addEventListener('touchend',    () => { draggingNode = null; });

    // ── Filter slider ──
    document.getElementById('wtFilterSlider').addEventListener('input', e => {
        const v = parseFloat(e.target.value);
        document.getElementById('wtFilterLbl').textContent = v >= 1000 ? (v/1000).toFixed(1)+'kHz' : Math.round(v)+'Hz';
        _wtDrawFilter(v);
        sendP(4, v);
    });

    // ── Volume drag on track ──
    const volTrack = document.getElementById('wtVolTrack');
    let volDragging = false;
    function _setVol(clientY) {
        const rect = volTrack.getBoundingClientRect();
        const pct = Math.max(0, Math.min(1, 1 - (clientY - rect.top) / rect.height));
        document.getElementById('wtVolFill').style.height = `${Math.round(pct*100)}%`;
        document.getElementById('wtVolumeLbl').textContent = `${Math.round(pct*100)}%`;
        sendP(3, pct);
    }
    volTrack.addEventListener('mousedown',  e => { volDragging = true; _setVol(e.clientY); });
    window.addEventListener('mousemove',    e => { if (volDragging) _setVol(e.clientY); });
    window.addEventListener('mouseup',      () => { volDragging = false; });
    volTrack.addEventListener('touchstart', e => { volDragging = true; _setVol(e.touches[0].clientY); }, {passive:false});
    window.addEventListener('touchmove',    e => { if (volDragging) _setVol(e.touches[0].clientY); }, {passive:false});
    window.addEventListener('touchend',     () => { volDragging = false; });

    // ── LFO sliders ──
    document.getElementById('wtLfoRate').addEventListener('input', e => {
        const v = parseFloat(e.target.value);
        document.getElementById('wtLfoRateLbl').textContent = `${v.toFixed(1)}Hz`;
        _wtDrawLfo(v, parseFloat(document.getElementById('wtLfoDepth').value));
        sendP(5, v);
    });
    document.getElementById('wtLfoDepth').addEventListener('input', e => {
        const v = parseFloat(e.target.value);
        document.getElementById('wtLfoDepthLbl').textContent = `${Math.round(v*100)}%`;
        _wtDrawLfo(parseFloat(document.getElementById('wtLfoRate').value), v);
        sendP(6, v);
    });

    // ── LFO target ──
    modal.querySelectorAll('.wt-lfo-target-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            modal.querySelectorAll('.wt-lfo-target-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            sendP(7, parseInt(btn.dataset.t));
        });
    });

    // ── Note buttons ──
    modal.querySelectorAll('.wt-note-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            modal.querySelectorAll('.wt-note-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            const note = parseInt(btn.dataset.note);
            if (typeof trackWtNoteJs !== 'undefined') trackWtNoteJs[padIndex] = note;
            if (typeof sendWebSocket === 'function')
                sendWebSocket({ cmd:'setWtNote', track:padIndex, note });
        });
    });

    // ── Close ──
    document.getElementById('synthModalClose').addEventListener('click', () => modal.classList.remove('active'));
    modal.addEventListener('click', e => { if (e.target === modal) modal.classList.remove('active'); });

    // ── Cancel ──
    document.getElementById('synthModalCancel').addEventListener('click', () => {
        (synthModalSnapshot || []).forEach((entry) => {
            setSynthParamValue(entry.pad, entry.engine, entry.paramId, entry.value);
            sendSynthParamWs(entry.pad, entry.engine, entry.paramId, entry.value);
            refreshPadWaveform(entry.pad);
        });
        refreshModalWaveform();
        modal.classList.remove('active');
    });

    // ── Test ──
    document.getElementById('synthModalTrigger').addEventListener('click', () => triggerSynthPreview(padIndex, engine));

    // ── Save ──
    document.getElementById('synthModalSave').addEventListener('click', () => modal.classList.remove('active'));
}

// ── WT Draw helpers ──────────────────────────────────────────

function _adsrTotalMs(a, d) { return Math.max(a + d * 2.5, 800); }

function _wtDrawCell(canvas, waveIdx, stroke) {
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = stroke === WT_COLOR ? '#1a1208' : '#111';
    ctx.fillRect(0, 0, W, H);
    const N = W;
    const pts = Array.from({length: N}, (_, i) => wtSingleWave(waveIdx, i / (N - 1)));
    ctx.beginPath(); ctx.strokeStyle = stroke; ctx.lineWidth = 1.5;
    pts.forEach((v, i) => {
        const x = (i / (pts.length - 1)) * W;
        const y = H/2 - v * (H/2 - 3);
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
}

function _wtDrawMorph(wavePos) {
    const canvas = document.getElementById('wtMorphDisplay');
    if (!canvas) return;
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    const bg = ctx.createLinearGradient(0, 0, 0, H);
    bg.addColorStop(0, '#0d1117'); bg.addColorStop(1, '#111820');
    ctx.fillStyle = bg; ctx.fillRect(0, 0, W, H);
    // grid
    ctx.strokeStyle = '#ffffff0e'; ctx.lineWidth = 1;
    [0.25, 0.5, 0.75].forEach(f => {
        ctx.beginPath(); ctx.moveTo(0, f*H); ctx.lineTo(W, f*H); ctx.stroke();
    });
    ctx.strokeStyle = '#ffffff20'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, H/2); ctx.lineTo(W, H/2); ctx.stroke();
    // wave glow passes
    const pts = Array.from({length: W}, (_, i) => wtMorphWave(wavePos, i / (W - 1)));
    [[12,'#e67e2220'],[5,'#e67e2250'],[2,WT_COLOR]].forEach(([lw, col]) => {
        ctx.beginPath(); ctx.strokeStyle = col; ctx.lineWidth = lw;
        pts.forEach((v, i) => {
            const x = (i / (pts.length - 1)) * W;
            const y = H/2 - v * (H/2 - 8);
            i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        });
        ctx.stroke();
    });
    // fill
    ctx.beginPath(); ctx.moveTo(0, H/2);
    pts.forEach((v, i) => {
        const x = (i / (pts.length - 1)) * W;
        const y = H/2 - v * (H/2 - 8);
        ctx.lineTo(x, y);
    });
    ctx.lineTo(W, H/2); ctx.closePath();
    ctx.fillStyle = WT_COLOR + '18'; ctx.fill();
}

function _wtDrawAdsr(attackMs, decayMs) {
    const canvas = document.getElementById('wtAdsrCanvas');
    if (!canvas) return;
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    const pL = 24, pR = 16, pT = 12, pB = 14;
    const iW = W - pL - pR, iH = H - pT - pB;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0d1117'; ctx.fillRect(0, 0, W, H);
    // grid lines
    ctx.strokeStyle = '#ffffff0c'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(pL, pT); ctx.lineTo(pL, pT+iH); ctx.stroke();

    const totalMs = _adsrTotalMs(attackMs, decayMs);
    const msX = ms => pL + (ms / totalMs) * iW;
    const aY  = a  => pT + (1 - a) * iH;

    const x0 = pL,              y0 = aY(0);
    const x1 = msX(attackMs),   y1 = aY(1);
    const x2 = msX(attackMs + decayMs), y2 = aY(0);
    const x3 = W - pR,          y3 = aY(0);

    // Fill
    ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3);
    ctx.lineTo(x3, pT+iH); ctx.lineTo(x0, pT+iH); ctx.closePath();
    const fg = ctx.createLinearGradient(0, pT, 0, pT+iH);
    fg.addColorStop(0, WT_COLOR+'70'); fg.addColorStop(1, WT_COLOR+'08');
    ctx.fillStyle = fg; ctx.fill();

    // Stroke
    ctx.beginPath(); ctx.strokeStyle = WT_COLOR; ctx.lineWidth = 2;
    ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3);
    ctx.stroke();

    // Time labels
    ctx.fillStyle = '#ffffff60'; ctx.font = '9px monospace';
    if (x1 > pL + 8) ctx.fillText(`${Math.round(attackMs)}ms`, x1 - 18, y1 - 6);

    // Segment length label midpoint
    const midDX = (x1 + x2) / 2;
    ctx.fillText(`${Math.round(decayMs)}ms`, midDX + 3, (y1 + y2) / 2 - 2);

    // Node handles
    [[x1, y1, 'A'], [x2, y2, 'D']].forEach(([nx, ny, lbl]) => {
        // Glow
        ctx.beginPath(); ctx.arc(nx, ny, 10, 0, Math.PI*2);
        ctx.fillStyle = WT_COLOR + '30'; ctx.fill();
        // Fill
        ctx.beginPath(); ctx.arc(nx, ny, 7, 0, Math.PI*2);
        ctx.fillStyle = WT_COLOR; ctx.fill();
        ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke();
        // Label
        ctx.fillStyle = '#0d1117'; ctx.font = 'bold 8px sans-serif';
        ctx.textAlign = 'center'; ctx.fillText(lbl, nx, ny + 3); ctx.textAlign = 'left';
    });
}

function _wtDrawFilter(cutHz) {
    const canvas = document.getElementById('wtFilterCanvas');
    if (!canvas) return;
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0d1117'; ctx.fillRect(0, 0, W, H);
    // log-scale cutoff position
    const cutX = (Math.log10(cutHz / 100) / Math.log10(18000 / 100)) * W;
    // Fill passband
    ctx.fillStyle = WT_COLOR + '18';
    ctx.fillRect(0, 0, cutX, H);
    // LP curve
    ctx.beginPath(); ctx.strokeStyle = WT_COLOR; ctx.lineWidth = 1.8;
    ctx.moveTo(0, H * 0.15); ctx.lineTo(cutX, H * 0.15);
    for (let x = cutX; x <= W; x++) {
        const r = (x - cutX) / Math.max(1, W - cutX);
        ctx.lineTo(x, Math.min(H * 0.15 + r * r * (H * 0.75), H - 2));
    }
    ctx.stroke();
    // Cutoff marker
    ctx.beginPath(); ctx.strokeStyle = WT_COLOR + '80'; ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]); ctx.moveTo(cutX, 0); ctx.lineTo(cutX, H); ctx.stroke(); ctx.setLineDash([]);
    // Freq label
    ctx.fillStyle = WT_COLOR; ctx.font = '9px monospace';
    ctx.fillText(cutHz >= 1000 ? (cutHz/1000).toFixed(1)+'k' : Math.round(cutHz)+'Hz', 4, H - 4);
}

function _wtDrawLfo(rateHz, depth) {
    const canvas = document.getElementById('wtLfoCanvas');
    if (!canvas) return;
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0d1117'; ctx.fillRect(0, 0, W, H);
    const d = (typeof depth !== 'undefined') ? depth : 0.5;
    const amp = 0.1 + d * 0.85;
    const cycles = Math.min(rateHz * 0.4, 6);
    ctx.beginPath(); ctx.strokeStyle = '#2ecc71'; ctx.lineWidth = 1.5;
    for (let x = 0; x < W; x++) {
        const t = (x / W) * cycles * Math.PI * 2;
        const y = H/2 - Math.sin(t) * amp * (H/2 - 3);
        x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
}

function openSynthModal(padIndex) {
    const engine = (typeof padSynthEngine !== 'undefined') ? padSynthEngine[padIndex] : -1;
    if (engine < 0) return;
    if (engine === 4) { openWtOscModal(padIndex); return; }
    ensureSynthPresetCss();

    synthModalPad = padIndex;
    synthModalEngine = engine;

    const params = getParamsForPad(engine, padIndex);

    // ── Snapshot: guardar valores actuales para poder cancelar ──────────────
    synthModalSnapshot = captureSynthEngineSnapshot(engine, padIndex);

    const inst = (engine < 3) ? padToInstrument(engine, padIndex) : -1;
    const instName = getInstrumentName(engine, inst);
    const engineLabel = ['TR-808','TR-909','TR-505','TB-303','WTOSC','SH-101','FM2OP'][engine] || '';
    const color = ENGINE_COLORS[engine];
    const padName = (typeof padNames !== 'undefined') ? padNames[padIndex] : `Pad ${padIndex+1}`;

    // Create or reuse modal
    let modal = document.getElementById('synthParamModal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'synthParamModal';
        modal.className = 'synth-modal-overlay';
        document.body.appendChild(modal);
    }

    modal.innerHTML = `
        <div class="synth-modal" data-engine="${engine}">
            <div class="synth-modal-header">
                <div class="synth-modal-title">
                    <span class="synth-modal-engine-badge" style="background:${color}">${engineLabel}</span>
                    <span class="synth-modal-inst-name">${padName} — ${instName}</span>
                </div>
                <div class="synth-modal-actions">
                    <button class="synth-modal-close-btn" id="synthModalClose">✕</button>
                </div>
            </div>
            <div class="synth-modal-waveform-wrap">
                <canvas id="synthModalCanvas" width="600" height="120"></canvas>
            </div>
            ${renderSynthPresetSection(engine)}
            <div class="synth-modal-params" id="synthModalParams"></div>
            <div class="synth-modal-footer">
                <button class="synth-modal-cancel-btn" id="synthModalCancel">✖ Cancelar</button>
                <button class="synth-modal-trigger-btn" id="synthModalTrigger" title="Preview sound">▶ Test</button>
                <button class="synth-modal-save-btn" id="synthModalSave">✔ Guardar</button>
            </div>
        </div>
    `;

    // Build parameter sliders
    const paramsContainer = modal.querySelector('#synthModalParams');
    attachSynthPresetHandlers(modal, engine, padIndex);
    params.forEach(p => {
        const currentVal = getSynthParamValue(padIndex, engine, p.paramId);
        const paramRow = document.createElement('div');
        paramRow.className = 'synth-param-row';

        if (p.labels) {
            // Toggle buttons (for waveform: SAW/SQR)
            paramRow.innerHTML = `
                <label class="synth-param-label">${p.name}</label>
                <div class="synth-param-toggle-group" data-param-id="${p.paramId}">
                    ${p.labels.map((lbl, idx) => `
                        <button class="synth-param-toggle-btn ${currentVal == idx ? 'active' : ''}"
                                data-value="${idx}">${lbl}</button>
                    `).join('')}
                </div>
            `;
            paramRow.querySelectorAll('.synth-param-toggle-btn').forEach(btn => {
                btn.addEventListener('click', (e) => {
                    e.preventDefault();
                    const val = parseFloat(btn.dataset.value);
                    paramRow.querySelectorAll('.synth-param-toggle-btn').forEach(b => b.classList.remove('active'));
                    btn.classList.add('active');
                    onSynthParamChange(padIndex, engine, p.paramId, val);
                });
            });
        } else {
            // Slider
            const pct = ((currentVal - p.min) / (p.max - p.min)) * 100;
            paramRow.innerHTML = `
                <label class="synth-param-label">${p.name}</label>
                <input type="range" class="synth-param-slider" data-param-id="${p.paramId}"
                       min="${p.min}" max="${p.max}" step="${p.step}" value="${currentVal}">
                <span class="synth-param-value" data-param-id="${p.paramId}">
                    ${formatParamValue(currentVal, p.unit)}
                </span>
            `;
            const slider = paramRow.querySelector('.synth-param-slider');
            const valSpan = paramRow.querySelector('.synth-param-value');

            let sendTimeout = null;
            slider.addEventListener('input', () => {
                const val = parseFloat(slider.value);
                valSpan.textContent = formatParamValue(val, p.unit);
                // Throttled send
                if (sendTimeout) clearTimeout(sendTimeout);
                sendTimeout = setTimeout(() => {
                    onSynthParamChange(padIndex, engine, p.paramId, val);
                }, 30);
            });
            slider.addEventListener('change', () => {
                if (sendTimeout) clearTimeout(sendTimeout);
                const val = parseFloat(slider.value);
                onSynthParamChange(padIndex, engine, p.paramId, val);
            });
        }

        paramsContainer.appendChild(paramRow);
    });

    // Draw large waveform
    const bigCanvas = modal.querySelector('#synthModalCanvas');
    if (bigCanvas) {
        drawSynthWaveformModal(bigCanvas, engine, padIndex);
    }

    // Event listeners
    modal.querySelector('#synthModalClose').addEventListener('click', closeSynthModal);

    modal.querySelector('#synthModalTrigger').addEventListener('click', () => {
        triggerSynthPreview(padIndex, engine);
    });

    // Guardar: confirmar cambios y cerrar
    modal.querySelector('#synthModalSave').addEventListener('click', () => {
        closeSynthModal();
    });

    // Cancelar: restaurar valores del snapshot y re-enviar al hardware
    modal.querySelector('#synthModalCancel').addEventListener('click', () => {
        cancelSynthModal(padIndex, engine, params);
    });

    modal.addEventListener('click', (e) => {
        if (e.target === modal) closeSynthModal();
    });

    // Show modal
    requestAnimationFrame(() => {
        modal.classList.add('active');
    });
}

function closeSynthModal() {
    if (synthAutoPreviewTimer) { clearTimeout(synthAutoPreviewTimer); synthAutoPreviewTimer = null; }
    const modal = document.getElementById('synthParamModal');
    if (modal) {
        modal.classList.remove('active');
        setTimeout(() => { modal.innerHTML = ''; }, 300);
    }
    synthModalPad = -1;
    synthModalEngine = -1;
    synthModalSnapshot = [];
}

// Cancelar: restaurar todos los parámetros al estado pre-apertura
function cancelSynthModal(padIndex, engine, params) {
    (synthModalSnapshot || []).forEach((entry) => {
        setSynthParamValue(entry.pad, entry.engine, entry.paramId, entry.value);
        sendSynthParamWs(entry.pad, entry.engine, entry.paramId, entry.value);
        refreshPadWaveform(entry.pad);
    });
    refreshModalWaveform();
    closeSynthModal();
}

function formatParamValue(val, unit) {
    if (unit === 'Hz') return `${Math.round(val)} Hz`;
    if (unit === 's')  return `${val.toFixed(2)}s`;
    if (unit === 'ms') return `${Math.round(val)} ms`;
    if (unit === 'st') return `${val >= 0 ? '+' : ''}${val.toFixed(1)} st`;
    if (val >= 0 && val <= 1 && !unit) return `${Math.round(val * 100)}%`;
    return val.toFixed(2);
}

function drawSynthWaveformModal(canvas, engine, padIndex) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const color = ENGINE_COLORS[engine] || '#fff';
    const inst = (engine < 3) ? padToInstrument(engine, padIndex) : -1;

    ctx.clearRect(0,0,w,h);

    // Panel background
    const bg = ctx.createLinearGradient(0,0,0,h);
    bg.addColorStop(0, 'rgba(18,20,28,0.95)');
    bg.addColorStop(1, 'rgba(6,8,14,0.98)');
    ctx.fillStyle = bg;
    ctx.fillRect(0,0,w,h);

    // Grid
    ctx.strokeStyle = 'rgba(255,255,255,0.05)';
    ctx.lineWidth = 0.5;
    for (let x = 0; x < w; x += 30) {
        ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke();
    }
    for (let y = 0; y < h; y += 20) {
        ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke();
    }
    // Center line
    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.beginPath(); ctx.moveTo(0,h/2); ctx.lineTo(w,h/2); ctx.stroke();

    // Waveform - same logic as pad but bigger
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2.5;
    ctx.shadowColor = color;
    ctx.shadowBlur = 8;

    const mid = h / 2;
    const amp = h * 0.4;

    if (engine === 4) {
        // Wavetable OSC  modal — igual que pad pero a mayor resolución
        const wavePos = getSynthParamValue(padIndex, 4, 0);
        const decayMs = getSynthParamValue(padIndex, 4, 2) || 300;
        const cycles = 5;
        for (let x = 0; x < w; x++) {
            const t = x / w;
            const env = Math.exp(-t / Math.max(decayMs / 1000, 0.01));
            const phase = (t * cycles) % 1.0;
            const s = wtMorphWave(wavePos, phase);
            const y = mid - amp * s * env;
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else if (engine === 3) {
        const wf = getSynthParamValue(padIndex, 3, 6);
        const cutoff = getSynthParamValue(padIndex, 3, 0);
        const reso = getSynthParamValue(padIndex, 3, 1);
        const cutoffNorm = Math.min(1, cutoff / 5000);
        const cycles = 4 + Math.floor(cutoffNorm * 4);
        for (let x = 0; x < w; x++) {
            const t = x / w;
            const phase = t * cycles;
            const frac = phase - Math.floor(phase);
            let sig;
            if (wf < 0.5) {
                sig = 2 * frac - 1; // Sawtooth
            } else {
                sig = frac < 0.5 ? 1 : -1; // Square
            }
            // Simulate filter effect
            const filterEnv = 1.0 - (1.0 - cutoffNorm) * 0.5;
            const resoBoost = 1 + reso * 0.3;
            const y = mid - amp * sig * filterEnv * resoBoost * Math.exp(-t * 0.3);
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else if (engine === 5) {
        // SH-101 modal — filtered saw/sqr with VCF envelope
        const wf      = getSynthParamValue(padIndex, 5, 0) | 0;
        const cutoff  = getSynthParamValue(padIndex, 5, 4) || 2000;
        const reso    = getSynthParamValue(padIndex, 5, 5) || 0.4;
        const lpFrac  = Math.min(1, cutoff / 9000);
        const vcaDec  = getSynthParamValue(padIndex, 5, 8) || 0.3;
        const vcfDec  = getSynthParamValue(padIndex, 5, 12) || 0.2;
        const subLvl  = getSynthParamValue(padIndex, 5, 2) || 0.3;
        const cycles  = 3;
        for (let x = 0; x < w; x++) {
            const t     = x / w;
            const phase = (t * cycles) % 1;
            const vcaEnv = Math.exp(-t / (vcaDec * 0.5 + 0.05));
            const cutEff = lpFrac * (0.3 + 0.7 * Math.exp(-t / (vcfDec * 0.3 + 0.03)));
            const resoBoost = 1 + reso * 0.25;
            let sig;
            if (wf === 0)      sig = 2 * phase - 1;
            else if (wf === 1) sig = phase < 0.5 ? 1 : -1;
            else               sig = phase < 0.35 ? 1 : -1;
            const sub = Math.sin(t * cycles * Math.PI) * subLvl;
            const filt = (sig * cutEff + sub * 0.4) * resoBoost;
            const y = mid - amp * filt * vcaEnv;
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else if (engine === 6) {
        // FM 2-Op modal
        const ratio  = getSynthParamValue(padIndex, 6, 8) || 2;
        const index  = getSynthParamValue(padIndex, 6, 9) || 3;
        const cDec   = getSynthParamValue(padIndex, 6, 1) || 0.4;
        const mDec   = getSynthParamValue(padIndex, 6, 5) || 0.25;
        const fb     = getSynthParamValue(padIndex, 6, 10) || 0;
        const cycles = 4;
        let fbSample = 0;
        for (let x = 0; x < w; x++) {
            const t    = x / w;
            const cEnv = Math.exp(-t / (cDec * 0.5 + 0.05));
            const mEnv = Math.exp(-t / (mDec * 0.4 + 0.02));
            const phase = t * cycles * Math.PI * 2;
            const modIn = phase * ratio + fb * fbSample;
            const mod   = index * mEnv * Math.sin(modIn);
            const sig   = Math.sin(phase + mod);
            fbSample    = sig;
            const y = mid - amp * sig * cEnv;
            x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
        }
    } else {
        const decay = getSynthParamValue(padIndex, engine, 0) || 0.3;
        const isKick = (inst === 0);
        const isSnare = (inst === 1);
        const isHat = (inst === 3 || inst === 4);
        const isTom = (inst >= 5 && inst <= 7);

        if (isKick) {
            const pitch = getSynthParamValue(padIndex, engine, 1) || 55;
            const freq = pitch / 15;
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.5 + 0.05));
                const freqSweep = freq + (freq * 2.5) * Math.exp(-t * 15);
                const y = mid - amp * Math.sin(t * freqSweep * Math.PI * 2) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isSnare) {
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.03));
                const tone = Math.sin(t * 40 * Math.PI) * 0.6;
                const noise = (Math.random() - 0.5) * 0.8;
                const y = mid - amp * (tone + noise) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isHat) {
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.01));
                const noise = (Math.random() - 0.5) * 1.4;
                const y = mid - amp * noise * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else if (isTom) {
            const pitch = getSynthParamValue(padIndex, engine, 1) || 120;
            const freq = pitch / 25;
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.4 + 0.04));
                const y = mid - amp * Math.sin(t * freq * Math.PI * 2) * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        } else {
            for (let x = 0; x < w; x++) {
                const t = x / w;
                const env = Math.exp(-t / (decay * 0.3 + 0.02));
                const sig = Math.sin(t * 24 * Math.PI)*0.5 + (Math.random()-0.5)*0.5;
                const y = mid - amp * sig * env;
                x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
            }
        }
    }
    ctx.stroke();
    ctx.shadowBlur = 0;

    // Glow fill
    ctx.globalAlpha = 0.06;
    ctx.lineTo(w, mid);
    ctx.lineTo(0, mid);
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.fill();
    ctx.globalAlpha = 1.0;
}

// ============================================================
//  WAVEFORM HELPER — genera una muestra de wavetable JS para visualización
//  Espeja la lógica de GenerateTables() en wavetable_osc.h
// ============================================================
function wtSingleWave(waveId, phase) {
    const phi = phase * Math.PI * 2;
    switch (waveId & 7) {
        case 0: return Math.sin(phi);
        case 1: return phase < 0.25 ? phase*4 : phase < 0.75 ? 2 - phase*4 : phase*4 - 4;
        case 2: { let s = 0; for (let h = 1; h <= 8; h++) s += Math.sin(phi*h)/h; return s*(2/Math.PI); }
        case 3: { let s = 0; for (let h = 1; h <= 9; h += 2) s += Math.sin(phi*h)/h; return Math.max(-1, Math.min(1, s*(4/Math.PI))); }
        case 4: return phase < 0.25 ? 0.85 : phase < 0.5 ? -0.5 : phase < 0.75 ? 0.25 : -0.75;
        case 5: return 0.70*Math.sin(phi) + 0.30*Math.sin(phi*2);
        case 6: return 0.58*Math.sin(phi) + 0.25*Math.sin(phi*2) + 0.17*Math.sin(phi*3);
        case 7: { const s = 2*phase-1; const d=3; return Math.tanh(s*d)/Math.tanh(d); }
        default: return Math.sin(phi);
    }
}
function wtMorphWave(wavePos, phase) {
    const wA = Math.floor(wavePos);
    const wB = Math.min(wA + 1, 7);
    const mix = wavePos - wA;
    return wtSingleWave(wA, phase) * (1 - mix) + wtSingleWave(wB, phase) * mix;
}

// ============================================================
//  WEBSOCKET: SEND PARAM CHANGES
// ============================================================
function onSynthParamChange(padIndex, engine, paramId, value) {
    // Store locally
    setSynthParamValue(padIndex, engine, paramId, value);

    // Send to ESP32 via WebSocket (live — sin necesidad de pulsar TEST)
    if (typeof ws !== 'undefined' && ws && ws.readyState === WebSocket.OPEN) {
        if (engine === 3) {
            // TB-303 global param
            ws.send(JSON.stringify({
                cmd: 'synth303Param',
                paramId: paramId,
                value: value
            }));
        } else {
            // TR-808/909/505 per-instrument param, y WTOSC engine=4
            const instrument = (engine === 4) ? padIndex : padToInstrument(engine, padIndex);
            ws.send(JSON.stringify({
                cmd: 'synthParam',
                engine: engine,
                instrument: instrument,
                paramId: paramId,
                value: value
            }));
        }
    }

    // Auto-retrigger para escuchar el cambio en tiempo real
    // - Para 303: el cambio afecta al sonido continuo, no necesita retrigger
    //   pero si no hay nota activa, la lanzamos para que se oiga el filtro
    // - Para percusión (808/909/505): necesita retrigger para escuchar el decay/pitch
    scheduleAutoPreview(padIndex, engine);

    // Redraw waveforms (responde visualmente al parámetro)
    refreshPadWaveform(padIndex);
    refreshModalWaveform();
}

function refreshPadWaveform(padIndex) {
    const pad = document.querySelector(`.pad[data-pad="${padIndex}"]`);
    if (!pad) return;
    const canvas = pad.querySelector('.synth-pad-waveform');
    const engine = (typeof padSynthEngine !== 'undefined') ? padSynthEngine[padIndex] : -1;
    if (canvas && engine >= 0) {
        drawSynthWaveformInPad(canvas, engine, padIndex);
    }
}

function refreshModalWaveform() {
    if (synthModalPad < 0 || synthModalEngine < 0) return;
    const canvas = document.getElementById('synthModalCanvas');
    if (canvas) {
        drawSynthWaveformModal(canvas, synthModalEngine, synthModalPad);
    }
}

function triggerSynthPreview(padIndex, engine) {
    if (typeof triggerSynthPad === 'function') {
        triggerSynthPad(padIndex);
    }
}

// ============================================================
//  HOOK: called by app.js when synth engine changes on a pad
// ============================================================
function onSynthEngineChanged(padIndex, engine) {
    if (engine >= 0) {
        createSynthOverlay(padIndex);
    } else {
        removeSynthOverlay(padIndex);
    }
}

// ============================================================
//  KEYBOARD SHORTCUT: ESC closes modal
// ============================================================
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        const modal = document.getElementById('synthParamModal');
        if (modal && modal.classList.contains('active')) {
            closeSynthModal();
            e.preventDefault();
            e.stopPropagation();
        }
    }
});

// ============================================================
//  INIT: called after pads are created
// ============================================================
function initSynthEditor() {
    // Will be called from app.js after createPads()
    refreshAllSynthOverlays();
    console.log('[SynthEditor] Initialized');
}
